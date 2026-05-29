# VCDS Clear-All-Modules Capture — 2026-05-29

> **Process note (Rule 12 retrospective):** this markdown was the diagnostic Sean's dispatch (PHASE 5) required *before* any firmware patch. It was written, but the firmware patch (commit `a0319e0`) shipped before he read it. The fix turned out to be correct (`$14`→`$04`, NRC 0x22 + empty-table mapping, ISO-TP acquire bump), but the workflow violated the diagnostic-then-signoff-then-patch gate. Rule 12 was added (commit `a6aa5f2`) to make that gate explicit and binding for future wire-surface changes. The fix stays in place per owner sign-off retrospectively; this note is the audit trail.


Source: `firmware/test/can_capture/dev_session/obd_clear.log`
Captured: 2026-05-29 ~11:46 PT, can_tail running as root
Dongle state: POWERED DOWN — zero `0x7E0` outbound frames in the
capture window, so all traffic is pure VCDS↔car. The dongle was
NOT a participant.

## Headline

The patched SRM ECU on Sean's dev RS7 **does not implement UDS
`$14 ClearDiagnosticInformation`**. It DOES implement the
**OBD-II legacy `$04` (Mode 04 ClearDiagnosticInformation)**.
VCDS uses both — only `$04` gets a positive acknowledgement from
the engine ECU.

Our dongle has been sending `$14 FF FF FF` exclusively and
receiving NRC `0x11` (serviceNotSupported). Fix is to switch
the clear path to `$04`.

## VCDS workflow (byte-by-byte)

VCDS addresses the bus through the J533 gateway by broadcasting
on **CAN ID `0x700`** (not our dongle's direct `0x7E0`
physical-addressing). ECUs respond on their individual response
IDs. Engine ECU response ID is **`0x7E8`** (same as the dongle
uses).

### Step 1 — broadcast extended session

```
   ts 336.653388   →  700#02 10 03 55 55 55 55 55        $10 03 broadcast
   ts 336.668878   ←  7E8#06 50 03 00 1E 01 E0 00        engine ECU positive
                                                          (P2=0x001E=30ms, P2*=0x01E0=480ms)
   …25+ other ECUs respond positively from individual response IDs
```

### Step 2 — TesterPresent keepalive

```
   ts 337.657977   →  700#02 3E 80 55 55 55 55 55        $3E with suppress-positive-response
```

### Step 3 — broadcast UDS $14 ClearDTC

```
   ts 338.669559   →  700#04 14 FF FF FF 55 55 55        $14 FFFFFF broadcast
   ts 338.672–.690 ←  many ECUs reply positive $54 from individual IDs
                       (7AB, 7B6, 77E, 791, 7B4, 7B8, 775, 786, 7BD, 7B5,
                        7D1, 776, 778, 785, 77C, 780, 7CA, 7B9, 7A8, 7A9,
                        17FE0080, 17FE0084, 17FE008A, 17FE008B, 17FE0096,
                        17FE0097, 17FE009D, 17FE009E)
   ts 338.681172   ←  7DD#03 7F 14 78 …                  NRC 0x78 pending (some ECUs)
                       7E9 not yet — different ECU
   --- ENGINE ECU (7E8): NO RESPONSE AT ALL ---
```

The engine ECU is completely silent on `$14`. This matches our
dongle's experience — the ECU rejects `$14` with NRC `0x11`
when physical-addressed (`7E0`), and ignores it entirely when
broadcast (`700`). The SRM patch has stripped `$14` from the
service table.

### Step 4 — broadcast OBD-II legacy $04 ClearDiagnosticInformation

```
   ts 340.688033   →  700#01 04 55 55 55 55 55 55        $04 broadcast (no params)
   ts 340.722300   ←  7E8#03 7F 04 78 00 00 00 00        engine ECU NRC 0x78 PENDING
   ts 341.152097   ←  7E8#01 44 00 00 00 00 00 00        engine ECU POSITIVE $44
                                                          ($04 + $40 = $44)
```

**Engine ECU clears successfully via `$04`.** ~430ms total from
request to positive response (one NRC-0x78 pending in between).

A second clear cycle at ts 392.64 → 396.99 shows the same
sequence reproducing the result.

## Comparison vs current dongle behavior

| Step | VCDS | Current dongle | Result |
|---|---|---|---|
| Addressing | broadcast `700` | physical `7E0` | both reach engine ECU |
| Session change | `$10 03` to `700` | `$10 03` to `7E0` | both get positive `$50 03` |
| Clear request | `$04` to `700` | `$14 FF FF FF` to `7E0` | **`$04` succeeds, `$14` is NRC 0x11** |
| Engine ECU ack | `7E8#01 44 00` | none for `$14` | — |
| Return to default | unobserved (VCDS may rely on session timeout) | `$10 01` to `7E0` | both get positive `$50 01` |

The session-control preamble (P-54 first attempt) was correct —
`$10 0x03` is in the ECU's service table and works fine. The
remaining bug is the choice of clear service: `$14` is not in
this patched ECU's service table; `$04` is.

## NRC interpretation

- NRC `0x11` = serviceNotSupported — service literally not in
  the ECU's UDS service table. Returned on every `$14` attempt
  regardless of session.
- NRC `0x78` = requestCorrectlyReceivedResponsePending — ECU is
  working on it; final response follows. Observed on `$04` during
  the ~430ms work window.
- Response SID `$44` = `$04 + $40` = positive Mode 04 clear ACK.
  No NRC, no payload — just an acknowledgement byte.

## Proposed firmware change

Single-line patch to `dtc_uds.c::dtc_uds_clear_diagnostic_information`:

```c
/* OBD-II Mode 04 ClearDiagnosticInformation. SRM-patched engine
 * ECU does not implement UDS $14 (returns NRC 0x11 / no
 * response). $04 is preserved by the patch (OBD-II legacy
 * compliance) and the ECU acknowledges with $44. */
req[0] = (uint8_t)0x04;
static const size_t k_req_len = (size_t)1;
```

Plus rename + constant cleanup in `dtc_config.h`:

```c
#define DTC_UDS_SID_CLEAR_LEGACY        0x04
#define DTC_UDS_CLEAR_LEGACY_REQ_BYTES  1
#define DTC_UDS_CLEAR_LEGACY_POSITIVE_SID 0x44
```

Remove the `$14 FF FF FF` group bytes — `$04` takes no
parameters.

Session preamble can be **removed** — the engine ECU appears to
accept `$04` from the default session (VCDS broadcast hits it
without per-ECU session change after the broadcast `$10 03`,
but our dongle physical-addressed `7E0` likely doesn't need
session at all; this is verifiable in one flash cycle).

`P2*` pending handling on `$04` works the same way as `$14`
would have — wait for non-`0x78` final response. The existing
pending-skip loop in `dtc_uds.c` covers this without change.

## Side findings

1. **Gateway routing**: VCDS uses `0x700` (gateway broadcast) to
   reach all modules; the gateway routes to each ECU and
   ECU-individual response IDs come back. Our dongle's `0x7E0`
   direct physical addressing also works for the engine ECU
   specifically — we don't need to mimic the broadcast path
   unless the goal is "clear all modules" rather than "clear
   engine ECU DTCs."

2. **Extended-frame addressing**: ECUs in this car also respond
   on 29-bit extended IDs like `17FE0080`, `17FE0084` etc. for
   certain replies. The engine ECU does NOT use these — `7E8`
   stays the canonical engine ECU response ID. No firmware
   change needed for this.

3. **OBD-II Mode 04 has no group selector**: Unlike UDS `$14
   FFFFFF` (clear all groups), Mode 04 always clears ALL emissions-
   related DTCs in the ECU's table. There's no fine-grained
   selection. For our use case (user clicks "Clear All DTCs"
   in the UI) this is exactly what we want.

## Proposed next dispatch

P-54 phase-2: switch firmware clear path from `$14` to `$04`,
re-flash, verify on live ECU, update UI banner copy if needed.
Single firmware file change (`dtc_uds.c`) plus the config
constants. Session preamble removal is optional — test both
paths and keep whichever is cleaner.

Wire-witness for the verify cycle: continue `obd_clear.log` or
start a new pinned-to-SHA capture. No changes to dongle WiFi /
power state needed.
