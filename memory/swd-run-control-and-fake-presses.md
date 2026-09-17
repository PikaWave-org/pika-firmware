---
name: swd-run-control-and-fake-presses
description: JLinkExe command files have no run control; use the GDB server with the ELF. Flipping PUPDR over SWD fakes a button press.
metadata: 
  node_type: memory
  type: reference
  originSessionId: dd0f43e1-540b-4675-a799-c0b2bf3f7da7
  modified: 2026-09-15T23:02:27.204Z
---

**JLinkExe `-CommandFile` cannot resume the core.** `mem32`, `w4` and `Sleep`
work, but `g`, `go`, `q`, `qc` and `exit` all answer "Unknown command", and
connecting halts the target - so a read-modify sequence leaves the board
halted and the debug log simply stops. Without `-autoconnect 1` the script is
processed *before* the connection, so even the commands that do exist fail.
Fine for reading static config registers, useless for anything timed.

For run control use the GDB server instead:

```
JLinkGDBServerCLExe -device STM32H733VG -if SWD -speed 4000 -port 2331 -nogui
gdb-multiarch -batch -nx build/targets/firmware/firmware.elf \
  -ex "target extended-remote localhost:2331" \
  -ex "monitor halt" -ex "set {unsigned int}0xADDR = 0xVAL" \
  -ex "monitor go" -ex "monitor sleep 1200" -ex "detach"
```

`monitor halt` / `go` / `sleep <ms>` do exactly what they say. **Pass the ELF**
- without it gdb-multiarch guesses the architecture wrong and every `monitor`
returns "not supported by this target" and every memory access fails, which
reads like a broken probe rather than a missing argument.

**But there is no ARM gdb on this machine.** As of 2026-09-17 `gdb` is the
x86-only build, and neither `gdb-multiarch` nor `arm-none-eabi-gdb` is
installed - so the recipe above cannot be run here at all, and installing one
is a question for whoever owns the machine. Use **`tools/swd.py`** instead: the
GDB server speaks RSP on the port, and halt, go, sleep, read and write are a
handful of packets, which that file implements along with starting the server
itself. `tools/hw run -- tools/swd.py mic_blocks rec_state` prints symbols;
import `Swd` for a scripted press sequence.

Two things it knows that cost time to find out:

- **A file-scope `static` is mangled.** `rec_state` is `_ZL9rec_state` in the
  symbol table, so a plain name lookup misses exactly the variables a single
  .cpp keeps to itself.
- **A halt stops the CPU but not the converters.** Polling a state machine
  every 250ms visibly stretched every phase being measured - a 1s pause read
  as 2.1s and a 4s replay as 5.5s. Sample sparsely, at known wall times, and
  do not halt at all during playback.

**Faking a button press without fingers:** the buttons are active low with
internal pull-ups, so switching a line's PUPDR from pull-up (01) to pull-down
(10) drives it low exactly as a press would - a real edge for the EXTI lines,
a real low level for the polled one. Halt, write PUPDR, resume, wait past the
debounce, then write it back. GPIOD PUPDR is 0x58020C0C, GPIOE 0x5802100C.
EXTI is 0x58000000 and SYSCFG_EXTICR1 is 0x58000408.

All three navigation buttons live in GPIOD PUPDR, which rests at **0x10501000**,
so a press is one whole-register write and needs no read-modify-write:

| press | write to 0x58020C0C | pad | PUPDR bits |
|-------|---------------------|-----|------------|
| UP    | 0x10601000          | PD10 | 21:20 |
| DOWN  | 0x10901000          | PD11 | 23:22 |
| RIGHT | 0x20501000          | PD14 | 29:28 |

RIGHT is both "open the menu" and "activate", so `right, down, right` reaches
the second menu entry from the home screen. Confirmed twice on 2026-09-16, from
two sessions, by reading `ui.screen_` and `ui.cursor_` back by name.

**Do the whole sequence in one `gdb-multiarch -batch` invocation.** A fresh
connect per press or per sample costs about a second, which is enough to step
straight over what you are trying to observe - it silently missed a 1s sound
clip in its entirety, and the run looked like a speaker that never played.

**Never let a J-Link session overlap a VCOM capture.** Attaching JLinkExe or
the GDB server while `cat /dev/ttyACM0` is running truncates the stream
SWD-side, and the result is indistinguishable from a firmware hang: the log
stops mid-line and stays silent to the end. This is *not* the two-readers
symptom - the port has one reader and the clash is on the debug link, so the
usual `fuser` check sees nothing wrong. It has already scored a board as
"10/10 hung" while it was really running past heartbeat 77, and it is why the
`btn: X pressed` lines were missing from every press test driven over SWD here
while a clean `tools/hw check` boot showed them fine. `tools/hw log`/`check`
now warn when they detect it, but the only real fix is not to overlap them:
either capture with nothing attached, or read state over SWD and ignore the
log.

Everything here still goes through `tools/hw run` / `tools/hw exec`, for the
reasons CLAUDE.md gives. Use `exec`, not `run`, for anything that reads
board state: the lock stops concurrent *access*, but the board keeps whatever
image was flashed last, and reading someone else's image gives confident
nonsense. That happened here - a first register dump showed no EXTI configured
at all because a peer had flashed their own firmware in between.
