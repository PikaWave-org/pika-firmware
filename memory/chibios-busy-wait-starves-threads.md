---
name: chibios-busy-wait-starves-threads
description: A busy-wait at NORMALPRIO in this ChibiOS config silently stops every other thread at that priority.
metadata: 
  node_type: memory
  type: project
  originSessionId: 9bc822b4-dec4-47fb-8f73-83e9f61391cb
  modified: 2026-09-13T14:26:11.491Z
---

`CH_CFG_TIME_QUANTUM` is 0 in this project, so ChibiOS does no round-robin.
A thread that spins without blocking never yields, and every other thread at
the same priority - which here is all of them - stops running. The board looks
completely dead: no log, no LCD updates, no hint about where it stopped.

Hit on 2026-09-13 by an unbounded `while ((usart->ISR & USART_ISR_TC) == 0U)`
in `main()`. It cost a long debugging detour because the symptom looks exactly
like a hard fault.

The same fact has a second consequence worth knowing: because nothing preempts
a thread at equal priority, code can accidentally depend on that for its
correctness. Two threads at `NORMALPRIO` sharing state without a lock work
today and break the day one of them is given a higher priority.

**How to apply:** never write an unbounded spin in thread context here. Bound
every hardware-flag wait with a counter and return a step code, so a failure is
reportable rather than silent.
