# Phase 1 close — wire-witness capture archive

Wire witness (`~/sniffer/can_tail.py`, gs_usb adapter) was attempted
at the start of the 2026-05-26 Phase 1 close-out HIL dispatch
(STAGE 0 of the autonomous walk-away dispatch). The adapter
enumerated and printed `# listening @ 500000 bps`, but **captured
0 frames** across 11 s — including 6 s with the ECU logger
actively polling via serial `logger_start`.

The adapter is not electrically on this car's dongle↔ECU bus —
P-52-class (wire-witness wedge / wrong tap). Per dispatch rule
"don't halt on wire-witness alone", noted and not halted.

The host is macOS; `rmmod gs_usb` from the dispatch's retry path
is Linux-only, so the documented retry-then-archive sequence runs
empty here. The capture log file is archived as-is for the audit
trail.

File: `phase1_close_<stamp>.log` — 0 lines (no frames captured).
