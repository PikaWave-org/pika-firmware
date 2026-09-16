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
git submodule update --init submodules/ChibiOS
```

Plain, with no `--reference`. It clones from GitHub and checks out the commit
the branch records, and on 2026-09-17 took well under a minute.

**Do not add `--reference` pointing at the shared checkout.** Its ChibiOS is
itself a shallow clone and git refuses to borrow from one:

```
fatal: reference repository '/home/ton/Pika/pika-firmware/submodules/ChibiOS' is shallow
fatal: clone of 'https://github.com/ChibiOS/ChibiOS.git' into submodule path ... failed
```

That failure leaves an empty `submodules/ChibiOS` behind, which has to be
`rmdir`ed before anything else will work — and it is easy to misread as "the
submodule cannot be populated here" and reach for a symlink instead.

**Do not symlink the shared checkout in, now that ChibiOS is a submodule.**
It builds, but it leaves the worktree in a state that is a trap for everyone
sharing it: the index holds the gitlink while the working tree holds a
symlink, so `git status` reports `T submodules/ChibiOS`, `git diff --summary`
reports `mode change 160000 => 120000`, and **any `git commit -a` or
`git add -A` silently records the symlink over the submodule pointer**,
breaking the build for anyone who checks that branch out. The symlink also
bypasses the gitlink, so you build whatever commit the shared checkout is on
rather than the one recorded. Only reach for it with no network, and then add
paths explicitly on every commit.

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
