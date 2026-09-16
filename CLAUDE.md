# Working on pika-firmware

## Read `memory/` first

Hard-won facts about this board live in `memory/`, one file per fact, indexed
in `memory/MEMORY.md`. Read that index at the start of a session and open the
notes it points at when they touch what you are doing — they record hardware
quirks, dead ends already explored and the measurements behind them, and most
cost a session each to learn. Add to it when you learn something that the code
cannot say for itself, and delete a note when it stops being true.

## Keep it small

This is firmware for one board with one MCU. Abstraction that is not paying for
itself now is cost, not insurance: prefer the plainest mechanism that meets the
requirement, a table and a switch over a class hierarchy until there is a second
real case, and let a comment carry the design rather than the type system. The
build system is deliberately hardcoded to this target — don't generalize it.

When a simplification trades something away, say what the trade is rather than
hiding it. To support a new peripheral or MCU, extend the hardcoded lists in
`cmake/chibios.cmake` rather than adding per-MCU abstraction or auto-discovery.

## Code style

Headers under `src/pika/` are included with angle brackets and their full path
from `src/` — `<pika/log.h>`, `<pika/audio/DACSpeaker.h>` — in their own sorted
block. Quotes are only for a file's own header, a sibling in the same directory,
and third-party headers (`"ch.h"`, `"hal.h"`, `"chprintf.h"`). Generated headers
follow the same rule; they land under `build/generated_src/pika/...`.

**`.clang-format` does not round-trip.** The committed files were formatted by
CLion's bundled clang-format, so running plain `clang-format` over a whole file
proposes changes to lines nobody touched and buries the real diff. Format only
what you added: `clang-format -i --lines=<start>:<end> <file>`. The tree is
deliberately mixed 2-space and 4-space — match the file you are editing, not the
config. Run it on the file in place: clang-format searches upward from the
file's own directory, so formatting a copy in `/tmp` silently falls back to
LLVM defaults and the diff is meaningless.

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
