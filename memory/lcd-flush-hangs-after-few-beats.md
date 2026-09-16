---
name: lcd-flush-hangs-after-few-beats
description: On main the heartbeat thread can block forever inside lcd.flush(); usually within a few beats but sometimes not for 100+, so a long run proves nothing.
metadata: 
  node_type: memory
  type: project
  originSessionId: dd0f43e1-540b-4675-a799-c0b2bf3f7da7
  modified: 2026-09-15T23:00:47.989Z
---

As of 2026-09-16 the firmware stops logging a few seconds after boot - always
right after the `gnss:` line, always before any `lcd: flush` line. It looks
like a crash or a dead UART. It is neither.

`heartbeat` is the only thread that logs periodically (the `gnss:` lines come
from its `log_gnss()` call, not from the GNSS thread), and the statement after
`log_gnss()` is `lcd.flush()`. When output stops, the thread is blocked in
there. Halting over SWD shows `ch_system.state == ch_sys_running` and the core
in `__idle_thread` - no fault, everything simply blocked.

I2C1 (0x40005400) during the hang:

    CR1 0x0000C091   PE, NACKIE, ERRIE, TXDMAEN, RXDMAEN - TCIE and STOPIE clear
    CR2 0x0029007E   SADD 0x7E (= 0x3F<<1), NBYTES 41, write
    ISR 0x00000021   TXE | STOPF, BUSY clear, no NACKF, no error bits

**Read this signature carefully - it is not a stalled peripheral.** STOPF set
with STOPIE clear is *normal* for I2Cv3 in master mode: the driver completes on
TC and only ever sets STOPIE in its two slave paths (`hal_i2c_lld.c` 1646,
1709), while master transmit with DMA sets TCIE alone (1529-1532). Nothing was
waiting on that STOP.

TCIE clear plus BUSY clear says the opposite of a stall: the only place TCIE is
cleared is line 551, between `dp->CR2 |= I2C_CR2_STOP` (548) and
`_i2c_wakeup_isr()` (554). So the TC path ran to the end and *did* issue the
wakeup. This is a genuinely lost wakeup, not a transfer that never finished.

**Size is NOT the trigger - this was proposed, then refuted by experiment.**
It looked compelling: a single I2Cv3 operation caps at 255 and sets RELOAD
(`i2c_lld_setup_[tr]x_transfer`, ~267-274), each TCR re-arms DMA for the next
chunk (496-503), and a full frame is 1 control byte + 160x13 pages = 2081 =
8*255 + 41 - so the observed NBYTES 41 is the final reload chunk.

**That inference was circular.** On main *every* flush is 2081 bytes, so the
last chunk is always 41 whatever the cause. NBYTES 41 tells you which transfer
was in flight, not which property breaks it. (What it does legitimately say is
that this particular hang landed on the last chunk rather than mid-frame -
a mid-frame hang would read NBYTES 255.)

cleanup settled it on 2026-09-16 with 10 boots per arm: baseline a2abf8b
10/10 hung at beats 1-4, mostly beat 1; partial flush `0372531` (narrow
per-page spans <=161 bytes, no RELOAD at all) still hung 8-9/10, at beats 3-9.
Decisively, those hung boots complete 3-4 *narrow* flushes first - `lcd: flush
took 30ms` lines - and then hang on a later narrow flush. **A transfer that
never reloads hangs just fine. Do not use ">255 bytes" as a repro handle.**

**This is not the button work.** Verified by building bare `HEAD` (a2abf8b, no
`buttons.cpp` at all) in a scratch worktree and flashing it - it hangs the same
way, at heartbeat 2. The number of beats before it hangs varies, which is what
you would expect of a lost-wakeup race.

The spread is wider than it first looked. Three consecutive `tools/hw check`
runs on the button branch hung at beats 1, 3 and 1 - but one boot on that same
image ran past **heartbeat 100** (106s) with `lcd: flush took` lines
throughout. So a healthy-looking long run does not mean a fix, and a boot that
survives is the window in which log-based verification actually works: the
`btn: X pressed` lines for all seven buttons were captured during that one
surviving boot, between t=35s and t=89s.

**The failure looks bimodal**, which is what makes a repeat-count protocol
affordable. Observed hangs run from beat 1 to beat 9 - beats 1-4 on full-frame
builds (bare HEAD, the button branch, cleanup's forced `invalidate()`), beats
3-9 on partial flush, where each beat costs several small transfers instead of
one big one. Every survivor ran long: 81 beats/87s and >100 beats/106s. Nothing
has been seen to die in between, i.e. anywhere in beats 10-80.

Be careful what supports that. **A boot that died at beat 2 produced no beats
3-19, so it is not evidence against a late death** - only a boot still alive at
beat 30 can tell you whether one dies there. A long capture window does not
help if the run was already dead inside it. So the late-death question rests on
exactly two boots, the 81-beat and >100-beat survivors, which did pass through
beats 5-80 alive. Bimodality is consistent with all ~7 boots but directly
supported by 2.

What the short runs *do* establish, separately and solidly, is that the hang is
**permanent rather than a long stall**: 16-18s of continued silence after the
death rules out recovery.

It reads like a race decided once during startup rather than per-flush
flakiness, which would let a boot be scored pass/fail within ~10 beats. Do not
assume that yet: run the first several boots of any repeat protocol long (60s+)
and only drop to short scoring once late deaths have actually been excluded. On
n=2, short scoring would silently convert a beat-30 death into a pass.

**Do not bisect this on single boots.** One surviving run is not evidence that
a commit is clean: a2abf8b-based images hang at beat 1-4 *and* sometimes run
past 100. A claim that "0d7322c reached heartbeat 81, so a2abf8b introduced it"
rests on exactly one boot and does not hold. Any bisect here needs repeated
boots per commit. Record **beat-of-death**, not just pass/fail: different
commits carry different LCD code and hang at different beats, so "does it
hang" alone compares unlike with unlike.

Partial flush does not fix it, it only *delays* it - beat 3-9 instead of
beat 1-2, and maybe a slightly lower rate. That is what fewer and smaller
transactions per beat buys you: fewer chances per beat, not a cure. Chunking
I2C writes to <=255 bytes is therefore unlikely to help either, and would
anyway need the DDRAM address counter to survive STOP/START between chunks -
plausible for this controller family, unverified on this panel.

## Measuring it without fooling yourself

**Never let a JLinkExe session overlap the capture.** cleanup's first 10-boot
run scored 10/10 hangs on a board that was in fact running past heartbeat 77:
resetting with JLinkExe while `cat` was running cuts the stream off part way,
and a truncated log is byte-identical to a hang - output stops mid-beat, then
silence. Flash or reset FIRST, let JLinkExe exit, and only then capture.
`tools/hw check` already sequences it correctly (`locked_flash` returns only
after the flash target completes, then `sleep 1`, then `locked_log`), so use
it rather than hand-rolling reset-plus-capture.

Scoring rules that survive contact:

- **Hang** = silence longer than two heartbeat intervals (>2.1s) before the
  capture ends.
- Take the **last** heartbeat number, never the maximum: a capture can open
  with buffered bytes from the previous boot, whose numbers are higher.
- `tools/hw check` starts capturing ~1s after the reset, so the first beat or
  two are never recorded. An entirely silent capture therefore means death at
  beat 0-1, not "the board never booted". Read beat-of-death from the last
  logged beat *number*, which is absolute, rather than by counting lines.

**Flush cadence is not the trigger.** 2026-09-16: the heartbeat was changed to
tick at 10Hz (flushing only on a beat or a UI change, so the same ~1 flush/s
when idle). It died at heartbeat 1 at 1.893s - and a control build with the
cadence put back to 1Hz died at *exactly* the same point. Do not blame the
loop rate.

Probably related to [[i2c-dma-silently-broken]]; see [[lcd-st75160-facts]] for
the panel itself. Reproduce with `tools/hw check 20` and count heartbeats.
