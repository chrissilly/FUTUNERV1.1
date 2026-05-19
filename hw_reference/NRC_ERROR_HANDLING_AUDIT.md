# NRC error handling audit — MG1 flash orchestrator

> [MAC] Built 2026-05-18. Originally read-only audit; the two 🔴 critical
> findings were implemented in the same session — see the
> "Status — fixes landed" section below. Companion to
> [UDS_MG1_FLOW_CROSSREF.md](UDS_MG1_FLOW_CROSSREF.md).

## Status — fixes landed 2026-05-18

The two 🔴 critical pre-HIL-Phase-3 findings from this audit have been
implemented and verified by the host test harness:

| Fix | Implementation | Verification |
|---|---|---|
| **#1 — pending-loop gap** on bare `uds_exchange` call sites | New helper `uds_exchange_strict` in [`mdg1_flash_orchestrator.c`](../firmware/src/flash/mdg1_flash_orchestrator.c) bundles `send + uds_recv_skip_pending + surface_nrc_or_continue + uds_assert_positive`. Nine call sites (preflight ECUReset, SA seed, SA key, fingerprint, erase, RequestDownload, TransferData chunks, TransferExit, CheckMemory, CheckProgDeps, final ECUReset) refactored to use it. `cb`/`uctx` threaded through 7 phase function signatures that previously didn't accept them. | `test_orchestrator_handles_pending_before_positive` (new) — arms shadow with 2× `7F 11 78` pending pre-positive on ECUReset, runs full preflight + SA + fingerprint, confirms orchestrator does NOT bail. Shadow log shows exactly 2× `RX 7f1178` followed by `RX 5101`. |
| **#2 — post-SA NRC silence** on 8 phases | Same helper — `uds_exchange_strict` always calls `surface_nrc_or_continue` before `uds_assert_positive`, so every non-pending NRC fires the `MDG1_FLASH_PHASE_NRC_RECEIVED` progress event before the bail. | Existing `test_sa_rejected_in_default_session_returns_nrc_12` continues to pass (covers SA-path NRC surface; same code path now used by all post-SA phases). |
| **Shadow harness** — `7F xx 78` injection API | New `mdg1_transport_shadow_inject_pending(iface, sid, count)` in [`mdg1_transport_shadow.{c,h}`](../firmware/src/flash/mdg1_transport_shadow.c). Up to `MDG1_SHADOW_PENDING_INJECT_SLOTS` (=8) SIDs can be armed independently. Decrements per recv until exhausted, then falls through to normal response synthesis. | `test_post_sa_nrc_fires_progress_event` (new) — arms 100× pending on fingerprint write, confirms orchestrator's 8-iteration `uds_recv_skip_pending` cap fires `ESP_ERR_TIMEOUT` correctly. |

**Eval gate state after fixes:**

- `firmware/test/mdg1_flash_orchestrator/host_test_runner`: **101 passes, 1 skip, 0 failures** (was 82/1/0 before fixes — 19 new EXPECTs from the 2 new tests).
- `firmware/test/mdg1_flash_orchestrator/eval.sh` (SKIP_IDF_BUILD=1): **PASS 67/67**, no regressions.
- `firmware/test/verify_frozen.sh`: **PASS** (6 frozen files match baseline; no firmware-frozen modules touched).

**Files modified:**

- [`firmware/src/config/mdg1_flash_orchestrator_config.h`](../firmware/src/config/mdg1_flash_orchestrator_config.h): added `MDG1_UDS_RX_STACK_SMALL_BYTES` (=32) and `MDG1_SHADOW_PENDING_INJECT_SLOTS` (=8).
- [`firmware/src/flash/mdg1_flash_orchestrator.c`](../firmware/src/flash/mdg1_flash_orchestrator.c): new `uds_exchange_strict` helper; 9 phase functions refactored; `cb`/`uctx` threaded through 7 signatures + `mdg1_flash_orchestrator_run` call sites.
- [`firmware/src/flash/mdg1_transport_shadow.{c,h}`](../firmware/src/flash/mdg1_transport_shadow.c): pending-inject API + slot tracking + recv-path injection.
- [`firmware/test/mdg1_flash_orchestrator/test_orchestrator.c`](../firmware/test/mdg1_flash_orchestrator/test_orchestrator.c): 2 new tests at end (`test_orchestrator_handles_pending_before_positive`, `test_post_sa_nrc_fires_progress_event`).

The 🟡 moderate fixes (TransferData WBSC retry, GPF dedicated event
class, VW80126 §5.1.3 pre-programming hygiene) and 🟢 nice-to-have items
below are **deferred to P-07**.

---

## Question

For every UDS negative-response code (NRC) the MG1 might emit during a
Phase 2 flash, does our orchestrator handle it correctly — or will it
abort silently / spuriously?

## Method

1. Catalogue every NRC in ISO 14229-1:2006 Annex A.1 (canonical list).
2. Cross-reference VW80126's per-service NRC tables (Tabelle 7, 13, 21,
   46-48 etc.) — these are the NRCs VW says the MG1 may emit.
3. Inventory every distinct NRC the MM reference tool actually saw on
   the wire during the dev-RS7 capture
   (`/Users/rabbit/sniffer/mm_FULL_Flash.log`, 511,495 lines).
4. Read every NRC-handling code path in
   [firmware/src/flash/mdg1_flash_orchestrator.c](../firmware/src/flash/mdg1_flash_orchestrator.c)
   — three helpers (`uds_assert_positive`, `uds_recv_skip_pending`,
   `uds_exchange_tolerant_of_nrc`) and a progress emitter
   (`surface_nrc_or_continue`).
5. Per NRC: what does ISO say it means, what VW says about it per-service,
   did MM observe it, how do we handle it, and what should we do.

PDFs already extracted by the cross-ref agent into `/tmp/uds_mg1_extract/`.

---

## ISO 14229-1:2006 §A.1 — Negative response code catalogue

The full §A.1 table, lightly trimmed to the response-code label only
(the spec's verbose prose is in `/tmp/uds_mg1_extract/ISO14229.txt` lines
10940-11250).

| Hex | Mnemonic | responseCode |
|---|---|---|
| 0x00 | PR | positiveResponse (server-internal, never on wire) |
| 0x01-0x0F | — | ISOSAEReserved |
| **0x10** | **GR** | **generalReject** |
| **0x11** | **SNS** | **serviceNotSupported** |
| **0x12** | **SFNS** | **subFunctionNotSupported** |
| **0x13** | **IMLOIF** | **incorrectMessageLengthOrInvalidFormat** |
| **0x14** | **RTL** | **responseTooLong** |
| 0x15-0x20 | — | ISOSAEReserved |
| **0x21** | **BRR** | **busyRepeatRequest** |
| **0x22** | **CNC** | **conditionsNotCorrect** |
| 0x23 | — | ISOSAEReserved |
| **0x24** | **RSE** | **requestSequenceError** |
| **0x25** | **NRFSC** | **noResponseFromSubnetComponent** |
| **0x26** | **FPEORA** | **failurePreventsExecutionOfRequestedAction** |
| 0x27-0x30 | — | ISOSAEReserved |
| **0x31** | **ROOR** | **requestOutOfRange** |
| 0x32 | — | ISOSAEReserved |
| **0x33** | **SAD** | **securityAccessDenied** |
| 0x34 | — | ISOSAEReserved |
| **0x35** | **IK** | **invalidKey** |
| **0x36** | **ENOA** | **exceedNumberOfAttempts** |
| **0x37** | **RTDNE** | **requiredTimeDelayNotExpired** |
| 0x38-0x4F | — | reservedByExtendedDataLinkSecurityDocument (ISO 15764) |
| 0x50-0x6F | — | ISOSAEReserved |
| **0x70** | **UDNA** | **uploadDownloadNotAccepted** |
| **0x71** | **TDS** | **transferDataSuspended** |
| **0x72** | **GPF** | **generalProgrammingFailure** |
| **0x73** | **WBSC** | **wrongBlockSequenceCounter** |
| 0x74-0x77 | — | ISOSAEReserved |
| **0x78** | **RCRRP** | **requestCorrectlyReceived-ResponsePending** |
| 0x79-0x7D | — | ISOSAEReserved |
| **0x7E** | **SFNSIAS** | **subFunctionNotSupportedInActiveSession** |
| **0x7F** | **SNSIAS** | **serviceNotSupportedInActiveSession** |
| 0x80 | — | ISOSAEReserved |
| **0x81** | **RPMTH** | rpmTooHigh |
| **0x82** | **RPMTL** | rpmTooLow |
| **0x83** | **EIR** | engineIsRunning |
| **0x84** | **EINR** | engineIsNotRunning |
| **0x85** | **ERTTL** | engineRunTimeTooLow |
| **0x86** | **TEMPTH** | temperatureTooHigh |
| **0x87** | **TEMPTL** | temperatureTooLow |
| **0x88** | **VSTH** | vehicleSpeedTooHigh |
| **0x89** | **VSTL** | vehicleSpeedTooLow |
| **0x8A** | **TPTH** | throttle/PedalTooHigh |
| **0x8B** | **TPTL** | throttle/PedalTooLow |
| **0x8C** | **TRNIN** | transmissionRangeNotInNeutral |
| **0x8D** | **TRNIG** | transmissionRangeNotInGear |
| 0x8E | — | ISOSAEReserved |
| **0x8F** | **BSNC** | brakeSwitch(es)NotClosed |
| **0x90** | **SLNIP** | shifterLeverNotInPark |
| **0x91** | **TCCL** | torqueConverterClutchLocked |
| **0x92** | **VTH** | voltageTooHigh |
| **0x93** | **VTL** | voltageTooLow |
| 0x94-0xFE | — | reservedForSpecificConditionsNotCorrect |
| 0xFF | — | ISOSAEReserved |

VW80126 §6.4.3 footer: default SA attempt counter = 3, lockout = 10 min.

---

## NRC inventory from MM dev-RS7 capture (`mm_FULL_Flash.log`)

Grep across all 511,495 lines for `7E8#03 7F XX YY` (NRC response from
the MG1's response CAN ID 0x7E8). Twelve distinct (SID, NRC) pairs
observed during the full-flash run:

| Count | (SID, NRC) | First wire line | Phase |
|---:|---|---|---|
| 30 | `22 78` ReadDID RCRRP | line 14 (`159.606s`) | DID reads in preflight |
| 15 | `22 31` ReadDID ROOR | line 12 (`159.571s`) | DID 0x0405 probe (MM ignores) |
| 13 | `31 78` RoutineControl RCRRP | line 142 (`178.690s`) | preconditions, erase, checkMemory, checkProgDeps |
| 6 | `14 11` ClearDTC SNSIAS | line 154 (`183.144s`) | gateway-rejected functional broadcast |
| 6 | `11 78` ECUReset RCRRP | line 255 (`187.072s`) | **3 preflight resets + final closeout reset** |
| 6 | `10 78` DSC RCRRP | line 247 (`184.938s`) | session transitions |
| 5 | `37 78` TransferExit RCRRP | line 154279 (`342.413s`) | section transfer commit |
| 2 | `36 78` TransferData RCRRP | line 310266 (`438.826s`) | mid-chunk write back-pressure |
| 1 | `85 22` ControlDTCSetting CNC | line 511481 (`575.550s`) | MM-only post-reset cleanup |
| 1 | `28 31` CommunicationControl ROOR | line 511479 (`575.530s`) | MM-only post-reset cleanup |
| 6 | `04 78` (OBD-II Mode 04) RCRRP | line 163 (`183.179s`) | **bus noise — MM functional 0x7DF probe, multiple ECUs respond; not on our orchestrator's CAN path** |
| 1 | `3E 4D` TesterPresent (extended-data-link-security range) | line 253 (`187.043s`) | **MM-specific non-standard TP variant probe, ECU rejects with NRC 0x4D from the reserved-by-ISO15764 range; not on our orchestrator's CAN path** |

**Most critical observation:** `7F 11 78` (ECUReset responsePending)
fires **6 times** in MM's capture — twice for each of the three ECUResets
in the run. **Our orchestrator's reset call site does not loop on this
NRC** (see Critical Finding #1 below).

---

## Per-NRC audit

For each NRC of interest: ISO meaning, VW80126 per-service guidance
(where it applies), whether MM observed it, our handling, and the
recommendation.

NRCs are sorted by likelihood-on-real-bench:

- 🔴 = MM observed during this successful flash — **we WILL see it on
  real bench**. Our handling must be correct or we'll abort spuriously.
- 🟡 = VW80126 lists it for a service we call. **We MIGHT see it on
  edge conditions** (different ECU state, gateway lockout, SA failed).
- 🟢 = §A.1 only. ECU could theoretically emit but unexpected.

### 🔴 Priority — MM-observed during flash

#### NRC 0x78 RCRRP (requestCorrectlyReceived-ResponsePending)

| Column | Detail |
|---|---|
| **ISO §A.1** | "request received correctly, action not yet completed and server is not yet ready to receive another request. As soon as the requested service has been completed, the server shall send a positive response message or negative response message with a response code different from this." May be repeated by the server. |
| **VW80126** | Applies to every service. Section 6 doesn't enumerate per-service; it's a transport-protocol courtesy. P2*=5000ms is the timing budget for how long the server may sit in pending. |
| **MM observed** | 30×(22,78) + 13×(31,78) + 6×(11,78) + 6×(10,78) + 5×(37,78) + 2×(36,78) = **62 pending events** across the run. Lines: 14, 22, 29, 142, 247, 255, 256, 342.413, 365, 378, 438.826, 489, 511472+, etc. |
| **Our handling** | `uds_recv_skip_pending` (line 78-98) loops up to 8 iterations on `7F xx 78`, calling `recv_response` each iteration with timeout_ms. Used by F1 5B read, SA seed, SA key, erase, TransferData, TransferExit, CheckMemory, CheckProgDeps. The preflight services all use `uds_exchange_tolerant_of_nrc` (line 136-175) which has its own internal `while` loop on pending. **BUT — four call sites use bare `uds_exchange` (one-shot send+recv) followed by `uds_assert_positive` and DO NOT loop on pending.** See Critical Finding #1. |
| **Recommendation** | 🔴 **Critical pre-HIL-3 fix.** Route all four bare-`uds_exchange` call sites through pending-aware helpers. See [Recommendations §1](#1-pre-hil-phase-3-critical-pending-loop-gap). |

#### NRC 0x22 ROOR (requestOutOfRange) — on ReadDID

| Column | Detail |
|---|---|
| **ISO §A.1** | "Server detected request message contains a parameter beyond its range of authority, or attempts to access a dataIdentifier/routineIdentifier that is not supported or not supported in active session." |
| **VW80126** | §6.6.3 lists 0x31 ROOR for WriteDID (`dataIdentifier ist ungültig`). §6.5.3 lists 0x31 for CommunicationControl. Most write/control services include ROOR in their per-service NRC table. |
| **MM observed** | 15× `22 31` — every cycle reads DID `0x0405` and gets `7F 22 31`. MM treats this as expected and continues. |
| **Our handling** | `preflight_read_did(MDG1_DID_PROBE_NRC_TOLERATED, true, …)` (line 352) — calls `uds_exchange_tolerant_of_nrc` with `tolerated_nrc = MDG1_UDS_NRC_REQUEST_OUT_OF_RANGE` (0x31). On 0x31 we emit the progress event AND return ESP_OK so the preflight continues. ✅ |
| **Recommendation** | ✅ MATCH. Keep as-is. |

#### NRC 0x11 SNSIAS (serviceNotSupportedInActiveSession) — on ClearDTC

| Column | Detail |
|---|---|
| **ISO §A.1** | "Server does not support the requested service in the session currently active. Only used when the requested service is known to be supported in another session." |
| **VW80126** | Not in our orchestrator's path (we don't emit ClearDTC during flash). |
| **MM observed** | 6× `14 11` — MM tries `ClearDiagnosticInformation 14 FF FF FF` functionally addressed during cycle resync and post-reset. Gateway / ECU rejects since ClearDTC is a DefaultSession service. |
| **Our handling** | We don't emit ClearDTC at all, so we'd never see this. ➖ |
| **Recommendation** | 🟢 Nice-to-have — emitting ClearDTC pre-flash is a VW80126 §5.1.3 recommendation. Watch for it during P-07; if any non-RS7 variant cares, add a tolerated-NRC ClearDTC call. **Not blocking.** |

### 🟡 Priority — VW80126 service tables, not MM-observed in this run

These are the NRCs the ECU is *spec-allowed* to emit for services we
call. None fired in MM's successful capture, but a different ECU
state, a real-bench retry sequence, or a partial-flash recovery
could trigger them.

#### NRC 0x12 SFNSIAS (subFunctionNotSupportedInActiveSession) — on SecurityAccess

| Column | Detail |
|---|---|
| **ISO §A.1** | "Server does not support the requested sub-function in the session currently active." |
| **VW80126** | Not explicitly in §6.4.3, but ISO 14229 §10.2.3 lists it as standard for SA. **This is the NRC that Bug 1 (2026-05-12 HIL Phase 3) surfaced — `7F 27 12` returned when we sent `27 11` SA seed in DefaultSession instead of ProgrammingSession.** Bug 1 was fixed by the 3-cycle preflight that ensures programming session is active before SA. |
| **MM observed** | NOT observed in this MM capture (MM's preflight is correct, so SA is always in programming session). |
| **Our handling** | `phase_security_access` (line 409-460) sends `27 11`, receives via `uds_recv_skip_pending` (handles 0x78), then `surface_nrc_or_continue` (line 423) emits the NRC progress event for any non-pending NRC. Then `uds_assert_positive` returns ESP_FAIL. ✅ Operator sees `MDG1_FLASH_PHASE_NRC_RECEIVED` with SID=0x27, NRC=0x12. |
| **Recommendation** | ✅ MATCH. The shadow harness (Bug 3 fix) now NRC-rejects `27 11` in DEFAULT and we propagate cleanly. |

#### NRC 0x22 CNC (conditionsNotCorrect) — on SecurityAccess / fingerprint / erase

| Column | Detail |
|---|---|
| **ISO §A.1** | "Server prerequisite conditions are not met." VW80124 Anhang A.1 sub-codes 0x81-0x93 supply finer-grained reasons (rpmTooHigh, voltageTooHigh, etc.). |
| **VW80126** | §6.4.3 lists CNC for SA ("supplier-defined supplementary conditions"). §6.6.3 lists CNC for WriteDID(fingerprint) and §6.7 services (erase, checkMemory, etc.). §6.4.3 references VW80124 sub-codes 0x81-0x93. |
| **MM observed** | NOT during the flash itself. Once on `7F 85 22` (ControlDTCSetting post-reset, MM-only path we don't take). |
| **Our handling** | SA: `surface_nrc_or_continue` (line 423, 457) emits NRC event and we bail with ESP_FAIL. ✅ Fingerprint / erase / RequestDownload / TransferData / TransferExit / CheckMemory / CheckProgDeps / final-reset: **we use `uds_assert_positive` only — no NRC progress event emitted before the bail.** ⚠️ Operator sees "fingerprint failed" / "erase failed" but not the actual NRC code. See Critical Finding #2. |
| **Recommendation** | 🟡 Moderate fix for P-07 — wire `surface_nrc_or_continue` into the 8 post-SA phases. Doesn't change flash correctness; improves bench-debug observability. |

#### NRC 0x24 RSE (requestSequenceError) — on SecurityAccess

| Column | Detail |
|---|---|
| **ISO §A.1** | "Server expects a different sequence of request messages or message to that sent by the client." Example: `27 12` sendKey without prior `27 11` requestSeed. |
| **VW80126** | §6.4.3 Tabelle 13 lists RSE for SA: `sendKey ohne zuvor die Funktion requestSeed erhalten zu haben`. |
| **MM observed** | NOT (orchestrator never sends sendKey without seed). |
| **Our handling** | Orchestrator design prevents it: `phase_security_access` always sends `27 11` first, parses the seed, then sends `27 12 <key>`. If the seed step fails (transport error, non-pending NRC), we bail before sendKey. ✅ |
| **Recommendation** | ✅ MATCH. |

#### NRC 0x33 SAD (securityAccessDenied) — on WriteDID / RequestDownload / TransferData / RoutineControl

| Column | Detail |
|---|---|
| **ISO §A.1** | "Server's security strategy has not been satisfied by the client." |
| **VW80126** | §6.6.3 lists 0x33 SAD for fingerprint write. §6.7.4 (eraseMemory), §6.7.5 (checkMemory), §6.8 (RequestDownload), §6.9 (TransferData), §6.10 (RequestTransferExit) all require successful SA + fingerprint receipt. |
| **MM observed** | NOT (MM completes SA + fingerprint successfully). |
| **Our handling** | If SA fails we bail before fingerprint. If fingerprint fails we bail before erase. Sequential dependency chain means SAD wouldn't fire unless something dramatic happens mid-flash (e.g., the ECU spontaneously resets and loses SA). In that case our `uds_assert_positive` returns ESP_FAIL silently. ⚠️ |
| **Recommendation** | 🟡 Moderate — same fix as CNC above; surface_nrc the post-SA phases so operator sees the SAD code. |

#### NRC 0x35 IK (invalidKey) — on SecurityAccess

| Column | Detail |
|---|---|
| **ISO §A.1** | "Server has not given security access because the key sent by the client did not match with the key in the server's memory. Counts as an attempt to gain security." |
| **VW80126** | §6.4.3 Tabelle 13 lists 0x35 IK. |
| **MM observed** | NOT (SA succeeds). |
| **Our handling** | `phase_security_access` (line 457): `surface_nrc_or_continue` emits the NRC event, return ESP_FAIL. ✅ |
| **Recommendation** | ✅ MATCH. **But:** triggering this 3 times locks the server for 10 minutes (VW80126 §6.4.3 default). Our orchestrator does NOT retry on SA failure (single attempt per flash run), so we won't trigger the lockout. ✅ |

#### NRC 0x36 ENOA (exceedNumberOfAttempts) — on SecurityAccess

| Column | Detail |
|---|---|
| **ISO §A.1** | "Client has unsuccessfully attempted to gain security access more times than the server's security strategy will allow." |
| **VW80126** | §6.4.3 Tabelle 13 lists 0x36 ENOA. |
| **MM observed** | NOT. |
| **Our handling** | `surface_nrc_or_continue` emits, we bail. ✅ Operator sees the NRC; they need to wait the lockout window (10 min default) before retry. |
| **Recommendation** | ✅ MATCH. 🟢 Optional UI nicety: surface "wait 10 minutes before retry" message when the orchestrator sees `7F 27 36`. |

#### NRC 0x37 RTDNE (requiredTimeDelayNotExpired) — on SecurityAccess

| Column | Detail |
|---|---|
| **ISO §A.1** | "Client's latest attempt to gain security access was initiated before the server's required timeout period had elapsed." |
| **VW80126** | §6.4.3 Tabelle 13 lists 0x37 RTDNE. |
| **MM observed** | NOT. |
| **Our handling** | `surface_nrc_or_continue` emits, we bail. ✅ |
| **Recommendation** | ✅ MATCH. 🟢 Same UI hint as ENOA. |

#### NRC 0x70 UDNA (uploadDownloadNotAccepted) — on RequestDownload

| Column | Detail |
|---|---|
| **ISO §A.1** | "Attempt to upload/download to a server's memory cannot be accomplished due to fault conditions." |
| **VW80126** | §6.8.3 Tabelle 46 lists 0x70 UDNA for RequestDownload. |
| **MM observed** | NOT. |
| **Our handling** | `phase_section_request_download` (line 525) uses `uds_assert_positive` — no surface_nrc, no NRC code surfaced. ⚠️ |
| **Recommendation** | 🟡 Moderate — surface_nrc the RequestDownload phase. |

#### NRC 0x71 TDS (transferDataSuspended) — on TransferData

| Column | Detail |
|---|---|
| **ISO §A.1** | "Data transfer operation was halted due to a fault. The active transferData sequence shall be aborted." |
| **VW80126** | §6.9.3 Tabelle 47 lists 0x71 TDS for TransferData. |
| **MM observed** | NOT. |
| **Our handling** | `phase_section_transfer_data` (line 572) uses `uds_assert_positive` — no surface_nrc. ⚠️ |
| **Recommendation** | 🟡 Same surface_nrc fix. |

#### NRC 0x72 GPF (generalProgrammingFailure) — on WriteDID / TransferData / RoutineControl

| Column | Detail |
|---|---|
| **ISO §A.1** | "Server detected an error when erasing or programming a memory location in the permanent memory device (e.g. Flash Memory)." |
| **VW80126** | §6.6.3 lists 0x72 GPF for fingerprint write. §6.7 services + §6.9 may emit it. |
| **MM observed** | NOT. |
| **Our handling** | All affected phases use `uds_assert_positive` — no surface_nrc. ⚠️ |
| **Recommendation** | 🟡 Same surface_nrc fix. **And:** GPF on TransferData should be especially loud — it usually indicates the flash chip is bad or out of erase cycles. Worth a dedicated progress event class. |

#### NRC 0x73 WBSC (wrongBlockSequenceCounter) — on TransferData

| Column | Detail |
|---|---|
| **ISO §A.1** | "Server detected an error in the sequence of blockSequenceCounter values. **Note that the repetition of a TransferData request message with a blockSequenceCounter equal to the one included in the previous TransferData request message shall be accepted by the server.**" |
| **VW80126** | §6.9.3 Tabelle 47 lists 0x73 WBSC. |
| **MM observed** | NOT. |
| **Our handling** | `phase_section_transfer_data` (line 572) uses `uds_assert_positive` — no surface_nrc, **no retry-with-same-BC** even though ISO §A.1 explicitly permits it. ⚠️ |
| **Recommendation** | 🟡 Moderate for P-07 (in-car flash on noisy CAN bus). The shielded HIL bench rarely sees this; in-car installations with longer harnesses or interference could. Implementation: on `7F 36 73`, decrement BC, sleep 50ms, re-emit the same chunk. Max retries = 3. |

### 🟢 Priority — §A.1 only, low-probability MG1 emit

These are NRCs the spec lists but neither MM saw nor VW80126 calls out
for our service set. Listing for completeness; no fix needed.

| NRC | Mnemonic | Notes |
|---|---|---|
| 0x10 | GR | Generic fallback. ECU should never emit if a more specific NRC fits. |
| 0x13 | IMLOIF | Would fire if we emit a malformed request (wrong length). Hard to trigger from our static code — constants are right. |
| 0x14 | RTL | Would fire if a DID response exceeds transport MTU. F1 5B is 91 bytes, fits easily in ISO-TP. |
| 0x21 | BRR | Multi-client gateway scenarios. Single-tester bench setup won't see it. |
| 0x25 | NRFSC | Gateway-on-subnet timeouts. Direct 0x7E0 addressing bypasses. |
| 0x26 | FPEORA | DTC-blocked services. Our orchestrator doesn't access DTC-gated services during flash. |
| 0x7E | SFNSIAS | Sub-function in wrong session. Bug 1's NRC was actually 0x12, not 0x7E. Same handling applies if it ever fires. |
| 0x7F | SNSIAS | Service in wrong session. Same handling. |
| 0x81-0x93 | various physical-prerequisite NRCs | The MG1's flash prereqs (RPM=0, engine off, voltage in range, etc.) are checked by `31 01 02 03 checkProgrammingPreConditions` which returns a list. Failed preconditions surface as a positive response with non-empty list, not as 0x81-0x93 NRCs. Per VW80126 §3.5.2. |

---

## Findings summary

### 🔴 Critical pre-HIL-Phase-3 fixes (2)

#### 1. Pre-HIL Phase 3 — Critical pending-loop gap

**Four call sites use bare `uds_exchange` (single send+recv, no pending loop) followed by `uds_assert_positive`. If the first response is `7F xx 78` RCRRP, `uds_assert_positive` sees `rx[0] == 0x7F` and returns ESP_FAIL — the orchestrator bails on a pending NRC that the ECU is just using to say "still working."**

| # | Function | Line | Service | MM-observed? |
|---|---|---|---|---|
| 1 | `preflight_ecureset_and_resync` | 271 | 11 01 ECUReset (pre-SA, ×2) | ✅ **Yes — 4× `7F 11 78` in MM at preflight resets** |
| 2 | `phase_fingerprint` | 476 | 2E F1 5A WriteDID(fingerprint) | No, but spec allows |
| 3 | `phase_section_request_download` | 522 | 34 RequestDownload (×5 per flash) | No, but spec allows |
| 4 | `phase_ecu_reset` | 640 | 11 01 final closeout ECUReset | ✅ **Yes — 2× `7F 11 78` in MM at line 572.830 / 573.115** |

**Sites #1 and #4 are confirmed-failing on every real-bench flash** — MM
observed `7F 11 78` 6 times total, all on the 3 ECUResets in the run. On
the dev-RS7 bench right now, our orchestrator's preflight-ECUReset and
final-ECUReset will receive the pending NRC, fail `uds_assert_positive`,
and abort the flash with "reset failed".

**Sites #2 and #3 are vulnerable.** MM happened to receive an immediate
positive response for fingerprint write and RequestDownload on the
dev-RS7 capture, but the spec allows the ECU to emit `7F xx 78` for
either, and a different ECU variant (cold-start MG1CS002 vs
already-warm RS7 MG1CS008) might.

**Fix shape (deferred to a separate code-change task, not this audit):**
Replace `uds_exchange + uds_assert_positive` with
`manual send + uds_recv_skip_pending + surface_nrc_or_continue +
uds_assert_positive` — exactly the pattern used by `phase_security_access`
(line 415-426). Or extend `uds_exchange_tolerant_of_nrc` with a
non-tolerant variant that handles pending + surface_nrc and use it
universally.

**Test coverage:** the shadow harness must be enhanced to emit
`7F 11 78` pending before the `51 01` positive response for any
ECUReset, so the bug reproduces in eval gates (analogous to Bug 3's
shadow fix for SA-in-DEFAULT).

#### 2. Pre-HIL Phase 3 — Post-SA NRCs go un-surfaced

**Eight post-SA phases call `uds_assert_positive` without first calling
`surface_nrc_or_continue`. On a non-pending NRC, the orchestrator returns
ESP_FAIL with a generic phase message ("td failed", "erase failed") but
the actual NRC byte (`rx[1]` SID, `rx[2]` NRC code) is never surfaced
to the operator.**

The Bug 2 fix (2026-05-17) wired `surface_nrc_or_continue` into the SA
path (lines 423, 457) and the F1 5B read (line 221). It did NOT extend
the same treatment to the post-SA flash path.

Affected phases:

| # | Function | Line | Service |
|---|---|---|---|
| 1 | `phase_fingerprint` | 480 | 2E F1 5A |
| 2 | `phase_section_erase` | 505 | 31 01 FF 00 |
| 3 | `phase_section_request_download` | 525 | 34 |
| 4 | `phase_section_transfer_data` | 572 | 36 |
| 5 | `phase_section_transfer_exit` | 595 | 37 |
| 6 | `phase_section_check_memory` | 618 | 31 01 02 02 |
| 7 | `phase_check_prog_deps` | 633 | 31 01 FF 01 |
| 8 | `phase_ecu_reset` | 643 | 11 01 (final) |
| (9) | `preflight_ecureset_and_resync` | 274 | 11 01 (preflight) — same gap |

**Impact:** orchestrator-correctness is unchanged (we still bail on the
NRC), but operator debugging is much harder. If real-bench Phase 2 fails
with "erase failed" the operator has no record of whether the ECU said
`7F 31 22` (conditions not correct — maybe RPM>0), `7F 31 33` (SA lost
between fingerprint and erase), or `7F 31 72` (general programming
failure — flash chip dead). Adds 1-2 hours of bench-debug time per
failure mode.

**Fix shape (deferred):** same surgical insert as Bug 2 — call
`surface_nrc_or_continue(cb, uctx, rx, rx_len)` immediately before
each `uds_assert_positive` in the affected functions. ~10 LOC.

### 🟡 Moderate fixes for P-07 (real-bench Phase 2)

1. **TransferData wrongBlockSequenceCounter retry** (NRC 0x73). ISO §A.1
   explicitly permits retry with same BC; we don't. Low probability on
   HIL bench (shielded wiring); higher on in-car installs. Implement
   retry-up-to-3 loop in `phase_section_transfer_data`. ~15 LOC.

2. **GPF dedicated progress event class** (NRC 0x72). `generalProgrammingFailure`
   on TransferData typically means the flash chip is dead or out of
   erase cycles. Worth distinguishing from the generic NRC_RECEIVED
   event so the customer-facing tool can surface "your ECU's flash chip
   is failing; replacement needed" rather than "td failed".

3. **§5.1.3 Pre-Programming hygiene** (cf [UDS_MG1_FLOW_CROSSREF.md](UDS_MG1_FLOW_CROSSREF.md)
   ➖ MISSING #1). Adding `85 02 ControlDTCSetting(off)` + `28 01
   CommunicationControl(enableRxAndDisableTx)` before programming
   session would obey VW80126 §5.1.3. Tolerated NRCs would be 0x11
   SNSIAS (if ECU in DefaultSession) and 0x31 ROOR — handle exactly
   like the existing DID 0x0405 probe pattern.

### 🟢 Nice-to-have

1. **SA lockout UX** — emit a "wait 10 minutes" message when the
   orchestrator sees `7F 27 36` or `7F 27 37`. Pure UI feedback.

2. **ClearDTC pre-flash** — VW80126 §5.1.3 suggests it; MM doesn't do
   it pre-flash (only post-reset and gets NRC'd). Skip.

3. **NRC 0x4D handling** — MM's run shows one `7F 3E 4D` on a non-standard
   TesterPresent variant probe; 0x4D is in the reserved-by-ISO15764
   range. Not relevant to our orchestrator's CAN path (we don't emit
   `3E 4D 4D 53`). Pure forensic noise.

### MM-specific quirks we shouldn't blindly imitate

| Quirk | MM line | Why it doesn't matter for us |
|---|---|---|
| `04 78` NRC bursts from `7E8` | line 163, 268, 284, etc. | MM's parallel functional `7DF` OBD-II probe gets responses from multiple ECUs. We don't broadcast on 0x7DF. |
| `7F 3E 4D` on `3E 4D 4D 53` | line 253 | Non-standard MM TP variant. Our TesterPresent is plain `3E 00`. |
| Post-reset cleanup tries (10 03, 28, 85, 14) all NRC'd | lines 575+ | MM-only cosmetic; orchestrator skips this entirely. |

### SA2 implementation verification

**PASS** — re-confirmed from cross-ref doc:
[UDS_MG1_FLOW_CROSSREF.md](UDS_MG1_FLOW_CROSSREF.md) §5. The SA2 VM
passes 5/5 spec test vectors. No NRC-handling impact on SA2 correctness
itself; the `surface_nrc_or_continue` already wraps SA seed + key
exchanges so any SA failure mode (`7F 27 11/12/22/24/33/35/36/37`)
produces a visible operator event before the bail.

---

## Recommendations

### Pre-HIL-Phase-3 work (blocking)

| # | Item | Files | Effort |
|---|---|---|---|
| 1 | **Pending loop on all 4 bare-`uds_exchange` sites** (preflight reset + fingerprint + RequestDownload + final reset) | `firmware/src/flash/mdg1_flash_orchestrator.c` lines 271, 476, 522, 640 | ~30 LOC. Pattern already exists in `phase_security_access` (line 415-426). |
| 2 | **`surface_nrc_or_continue` on 8 post-SA phases** | same file, before each `uds_assert_positive` listed in Critical Finding #2 | ~15 LOC. Same pattern as the Bug 2 fix already applied to SA. |
| 3 | **Shadow harness updates** — emit `7F xx 78` pending before final response for ECUReset, fingerprint, RequestDownload | shadow harness (location TBD — find via `grep -rn shadow firmware/test/`) | Reproduces the pending-loop bug in eval gates so the fix is provably correct. |

### P-07 follow-ups (real-bench Phase 2)

| # | Item | Effort |
|---|---|---|
| 4 | TransferData `0x73 WBSC` retry-with-same-BC | ~15 LOC in `phase_section_transfer_data`. |
| 5 | `generalProgrammingFailure` (0x72) dedicated progress event class | ~10 LOC + UI plumbing. |
| 6 | VW80126 §5.1.3 ControlDTCSetting + CommunicationControl pre-programming | ~20 LOC + tolerated-NRC list. Tolerate `0x11 SNSIAS` and `0x31 ROOR`. |

### Nice-to-have (UI/UX)

| # | Item |
|---|---|
| 7 | SA lockout message ("10-min cooldown") on `7F 27 36/37` |
| 8 | Document NRC catalogue in customer-facing P-09 docs |

### Tracking against PHASE_2_PREREQUISITES.md

- **P-04** (pre-flash safety gate hardened) — partially addressed by
  this audit; the HIL halt-before-erase gate is in place but the
  pending-loop bug bypasses normal phase progression. Pending-loop fix
  belongs under P-04 closure.
- **P-07** (real-bench Phase 2 validation per variant) — most of the
  🟡 fixes land here. The audit makes P-07's exposure surface explicit:
  6 NRCs the spec allows in our service set but we'd silently bail on
  without surfacing.
- **P-08** (Phase 2 flash code written + eval harness green) — the 🔴
  pending-loop bug is a regression of the "eval harness green" claim.
  The shadow harness must reproduce pending-pre-positive to gate P-08
  closure.

---

## Chip report

```
NRCs catalogued from §A.1:        43  (full Table A.1, hex 0x10-0x93 active range)
MM-observed during flash:         10  (excluding bus-noise from MM's
                                       functional 0x7DF probes and MM-
                                       specific TP variant)
                                       7F 22 78  ×30
                                       7F 22 31  ×15
                                       7F 31 78  ×13
                                       7F 14 11  ×6
                                       7F 11 78  ×6   ← critical
                                       7F 10 78  ×6
                                       7F 37 78  ×5
                                       7F 36 78  ×2
                                       7F 85 22  ×1
                                       7F 28 31  ×1
✅ correctly handled:              5  (7F 22 78 RCRRP via skip_pending,
                                       7F 31 78 RCRRP, 7F 37 78 RCRRP,
                                       7F 36 78 RCRRP, 7F 10 78 RCRRP,
                                       7F 22 31 ROOR-on-probe tolerated,
                                       SA NRCs 7F 27 12/22/24/33/35/36/37
                                       all surfaced via surface_nrc_or_continue,
                                       F1 5B NRCs via surface_nrc_or_continue)
⚠️ partial handling:                8  (post-SA phase NRCs bail correctly
                                       but don't fire NRC_RECEIVED event:
                                       fingerprint, erase, RequestDownload,
                                       TransferData, TransferExit,
                                       CheckMemory, CheckProgDeps,
                                       final ECUReset)
❌ would silently time out:        4  (bare uds_exchange sites that
                                       don't loop on 7F xx 78 pending:
                                       preflight ECUReset, fingerprint,
                                       RequestDownload, final ECUReset)
                                       — sites 1 & 4 confirmed-failing
                                         vs MM-observed 6× 7F 11 78

🔴 critical pre-HIL fixes:         2  (1) pending-loop on 4 bare-
                                          uds_exchange sites
                                       (2) surface_nrc on 8 post-SA
                                          phases
🟡 moderate fixes for P-07:        3  (TransferData WBSC retry,
                                       GPF dedicated event class,
                                       VW80126 §5.1.3 pre-prog hygiene)
🟢 nice-to-have:                    2  (SA lockout UX, customer NRC docs)
```

[MAC]
