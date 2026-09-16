---
name: worktree-needs-submodules-init
description: A fresh git worktree of pika-firmware cannot build until the submodules are populated and the branch is rebased onto local main.
metadata: 
  node_type: memory
  type: project
  originSessionId: db7be002-14a7-4374-96c6-14bb447e4183
  modified: 2026-09-13T13:24:31.799Z
---

`submodules/ChibiOS` is a registered git submodule as of 2026-09-16 (before
that it was an untracked plain directory that had to be symlinked by hand). A
new worktree still starts with it *empty*, and the build fails at configure
time until it is populated:

```
git submodule update --init --reference /home/ton/Pika/pika-firmware/submodules/ChibiOS submodules/ChibiOS
```

ChibiOS is a big, slow clone, so pass `--reference` at the shared checkout to
take the objects off local disk rather than GitHub. Symlinking the shared
directory in still works and is the fastest option when there is no network,
but then the gitlink is not honoured — you get whatever commit the shared
checkout happens to be on, not the one this branch records.

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
