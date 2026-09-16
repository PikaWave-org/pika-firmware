# Working on pika-firmware

## The board is shared — take the lock

One Pika board is attached to this machine, and several agents work on this
repo at once (including from worktrees under `.claude/worktrees/`). The SWD
link and the J-Link VCOM debug log on `/dev/ttyACM0` cannot be shared: two
readers on the VCOM each get a random subset of the bytes, which looks exactly
like line noise, and a flash landing inside someone else's capture looks like a
firmware bug. Both have already cost long false debugging detours.

So **never touch the board directly**. No bare `JLinkExe`, no
`cmake --build … --target flash-firmware`, no `cat /dev/ttyACM0`. Go through
`tools/hw`, which holds a machine-wide `flock` shared by every worktree:

```
tools/hw status                    # who has the board right now
tools/hw check 15 -r "why"         # build, flash, then capture 15s — one critical section
tools/hw flash -r "why"            # build and program only
tools/hw log 15 -r "why"           # capture the debug log only
tools/hw run -r "why" -- CMD...    # anything else (gdb, JLinkExe, RAM reads)
```

Prefer `tools/hw check`: it does not release the board between programming and
reading, so the log you get is guaranteed to come from the image you just
built. Always pass `-r` with a short reason — it is what another agent sees
while it waits on you.

Hold the board for as short a time as you can, and identify yourself by
exporting `PIKA_HW_AGENT` to something recognisable at the start of a session.

The lock cannot cover a `cu` or `screen` session the user started by hand.
`tools/hw status` lists any such reader; never kill it — ask the user to
detach, or read the value you need out of RAM over SWD instead
(`arm-none-eabi-nm -S` on the ELF for the address).
