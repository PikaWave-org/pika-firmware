---
name: worktree-needs-chibios-symlink
description: A fresh git worktree of pika-firmware cannot build until ChibiOS is linked in and the branch is rebased onto local main.
metadata: 
  node_type: memory
  type: project
  originSessionId: db7be002-14a7-4374-96c6-14bb447e4183
  modified: 2026-09-13T13:24:31.799Z
---

`submodules/ChibiOS` is an untracked plain directory, not a registered git
submodule, so a new worktree starts without it and the build fails at configure
time. Link it in before building:

```
ln -s /home/ton/Pika/pika-firmware/submodules/ChibiOS submodules/ChibiOS
```

Also check the branch point, twice over. `origin/main` lags local `main`, and
worktrees are created from `origin/main` by default, so a fresh worktree can
silently miss the most recent commits - `git reset --hard <local main sha>`
before starting work. But local `main` can lag too: on 2026-09-13 the whole
enum-based u-blox driver and the LCD status block existed only as *uncommitted*
changes in the shared checkout, so even a worktree at local `main` had a much
older `src/pika/ublox.cpp`. Diff the worktree's files against
`/home/ton/Pika/pika-firmware/<path>` with plain `diff` (git refuses to reach
into the shared checkout from an isolated worktree) and copy the modified ones
in as a clearly-labelled WIP base commit before building on them.

There are no push credentials in background sessions — `git push` fails with
"could not read Username for 'https://github.com'". Commits are still safe:
refs live in the shared `.git`, so a branch survives deletion of the worktree
directory and the user can push it from the main checkout.
