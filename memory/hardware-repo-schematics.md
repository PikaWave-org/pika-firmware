---
name: hardware-repo-schematics
description: Schematics for pika_go_0 live in a sibling repo; how to resolve a net label to an MCU pin without kicad-cli.
metadata: 
  node_type: memory
  type: reference
  originSessionId: 992d855b-fcb6-460b-ae10-3f41c6073967
  modified: 2026-09-16T06:28:28.733Z
---

The board schematics are **not** in pika-firmware. They are at
`~/Pika/pika-hardware` (github.com/PikaWave-org/pika-hardware), sheets in
`pika_go_0/*.kicad_sch` (`mcu`, `power`, `audio`, `gnss`, `radio`, `usb`) plus
`pika_go_0.kicad_pcb`. Use it to check anything in `boards/pika_go_0/board.h`
before assuming a pin is a placeholder — see [[pika-go-0-board-placeholders]].

`kicad-cli` is **not installed** here, so there is no netlist export. Resolving
a net by hand works fine: parse the `.kicad_sch` s-expressions, build the pin
coordinate table from the embedded `lib_symbols` (transform each pin by the
placed symbol's `at`/`mirror`, remembering library Y is inverted vs the sheet),
flood-fill along `wire` `pts`, and report which placed pins a `label` reaches.
About 100 lines of Python.

For the `.kicad_pcb`, counting `(net "/MCU/<NAME>")` occurrences grouped by
enclosing block (`footprint` / `segment` / `via`) is a cheap routed-or-not
check. Do **not** try to brace-match every block in that file with a Python
scan - it is O(n^2) on a multi-MB file and does not finish.

Verified 2026-09-16: every `LINE_BTN_*` in board.h matches the schematic,
including `BTN_UP` = SW4.1 = U4 pin 57 = PD10.
