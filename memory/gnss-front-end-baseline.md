---
name: gnss-front-end-baseline
description: What healthy UBX-MON-HW numbers look like on this board, and why two of its fields mean nothing yet.
metadata:
  node_type: memory
  type: project
  originSessionId: 9bc822b4-dec4-47fb-8f73-83e9f61391cb
  modified: 2026-09-16T00:00:00.000Z
---

Measured on pika_go_0 over several runs on 2026-09-13, indoors with no fix,
antenna connected and the front end healthy:

```
gnss: hw noise=97 agc=4056 jam=14 jamstate=0 ant=ok apwr=1 flags=0x00
```

Across runs `noisePerMS` sat between **97 and 105** and `agcCnt` between
**~4050 and ~4520** - mid-scale of the 0..8191 range. Quote the range, not one
sample: a single reading looks more precise than the measurement is.

Reading them:

- **AGC pinned at 8191 with `ant=open`** is the disconnected-antenna signature.
- **Noise climbing while AGC falls** is something transmitting nearby.
- `ant=ok apwr=1` is the antenna supervisor happy with the LNA powered.

**`jamInd` and `jammingState` carry no information.** The driver never sends
CFG-ITFM, so the interference monitor is disabled and those two read ~14 and 0
("unknown") whatever the conditions - a non-zero `jamInd` here is *not*
jamming. Enabling CFG-ITFM, with `antSetting` matching the real antenna, is
what would make them mean something; that is unwritten.

Quick liveness check: `frames()` climbing by **2 per second** is the tell that
MON-HW is really arriving, since it is enabled at one per navigation epoch
alongside NAV-PVT. Frozen counters mean the link is down, not that the numbers
are steady - see [[gnss-slow-clock-emi-dead-end]] for how that misleads.
