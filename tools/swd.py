#!/usr/bin/env python3
"""Run control and memory access over SWD, without a gdb.

There is no ARM-capable gdb on this machine - `gdb` is the x86 one and
gdb-multiarch is not installed - and JLinkExe command files cannot resume the
core, so neither half of the usual recipe works here. What does work is talking
to the J-Link GDB server in its own protocol: it speaks RSP on a TCP port, and
halt, go, sleep, read and write are a handful of packets.

As a library:

    from swd import Swd
    with Swd() as s:                 # starts the server, connects, cleans up
        s.halt()
        print(s.u32(s.sym("mic_blocks")))
        s.write_u32(0x58020C0C, 0x20501000)   # fake a BTN_RIGHT press
        s.go()

As a command, for a quick look at a symbol or an address:

    tools/hw run -r "read state" -- tools/swd.py mic_blocks rec_state 0x24000000

Every use still goes through tools/hw, and never alongside a VCOM capture -
attaching truncates the log SWD-side and it looks exactly like a hang.

Note what a halt costs: it stops the CPU but not the converters, so polling a
state machine every 250ms visibly stretches every phase it is trying to
measure. Sample sparsely, at known wall times.
"""

import socket
import struct
import subprocess
import sys
import time

ELF = "cmake-build-debug/targets/firmware/firmware.elf"
DEVICE = "STM32H733VG"


class Swd:
    def __init__(self, elf=ELF, port=2331, start_server=True):
        self.elf = elf
        self.port = port
        self.server = None
        self.buf = b""

        if start_server:
            self.server = subprocess.Popen(
                ["JLinkGDBServerCLExe", "-device", DEVICE, "-if", "SWD", "-speed", "4000",
                 "-port", str(port), "-nogui"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        deadline = time.time() + 15.0
        while True:
            try:
                self.s = socket.create_connection(("localhost", port), timeout=30)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        self.s.settimeout(30)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        try:
            self.s.close()
        finally:
            if self.server is not None:
                self.server.terminate()

    # -- the protocol ------------------------------------------------------

    def _recv_byte(self):
        if not self.buf:
            self.buf = self.s.recv(4096)
            if not self.buf:
                raise IOError("the GDB server closed the connection")
        b, self.buf = self.buf[:1], self.buf[1:]
        return b

    def _recv_packet(self):
        while self._recv_byte() != b"$":
            pass
        data = b""
        while True:
            b = self._recv_byte()
            if b == b"#":
                break
            data += b
        self._recv_byte()
        self._recv_byte()
        self.s.send(b"+")
        return data

    def cmd(self, data):
        """One request. Skips acks and the server's O<hex> console output."""
        self.s.send(b"$" + data + b"#%02x" % (sum(data) & 0xFF))
        while True:
            b = self._recv_byte()
            if b == b"$":
                self.buf = b"$" + self.buf
                break
        while True:
            reply = self._recv_packet()
            if not reply.startswith(b"O") or reply == b"OK":
                return reply

    def monitor(self, text):
        return self.cmd(b"qRcmd," + text.encode().hex().encode())

    def halt(self):
        self.monitor("halt")

    def go(self):
        self.monitor("go")

    # -- memory ------------------------------------------------------------

    def read(self, addr, length):
        out = b""
        while length:
            n = min(length, 512)
            reply = self.cmd(b"m%x,%x" % (addr, n))
            if reply.startswith(b"E"):
                raise IOError("read of %#x failed: %s" % (addr, reply))
            out += bytes.fromhex(reply.decode())
            addr += n
            length -= n
        return out

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def u8(self, addr):
        return self.read(addr, 1)[0]

    def i16(self, addr):
        return struct.unpack("<h", self.read(addr, 2))[0]

    def write_u32(self, addr, value):
        reply = self.cmd(b"M%x,4:%s" % (addr, struct.pack("<I", value).hex().encode()))
        if reply != b"OK":
            raise IOError("write of %#x failed: %s" % (addr, reply))

    # -- symbols -----------------------------------------------------------

    def sym(self, name):
        """Address of a symbol. A file-scope static is mangled: rec_state is
        _ZL9rec_state, which is why a plain name lookup misses it."""
        out = subprocess.check_output(["arm-none-eabi-nm", self.elf], text=True)
        wanted = (name, "_ZL%d%s" % (len(name), name))
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] in wanted:
                return int(parts[0], 16)
        raise KeyError(name)


def main(argv):
    if not argv:
        print(__doc__)
        return 1

    with Swd() as s:
        s.halt()
        for name in argv:
            addr = int(name, 0) if name.startswith("0x") else s.sym(name)
            print("%-16s %#010x = %u" % (name, addr, s.u32(addr)))
        s.go()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
