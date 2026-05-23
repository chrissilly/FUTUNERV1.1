# Phase 1 Completion Plan — FUTUNER v1.1

> Living document. Captures every work item between today's state
> ("HIL Phase 1 PARTIAL PASS") and Phase 1 PERFECT — the point at
> which Phase 2 (real CAL flash on a customer ECU) is unblocked from
> a Phase-1-side correctness standpoint.
>
> Owner: Sean / SRM Engineering. Created: 2026-05-21.
>
> **Companion docs:**
> - `MISSION_SPEC.md` (Phase 1 deliverables 1.1 through 1.7)
> - `PHASE_2_PREREQUISITES.md` (Phase 2-side prerequisites — separate list)
> - `SCALE_ARCHITECTURE_PROPOSAL.md` (scaling architecture)
> - `status-2026-05-19.md` (multi-day HIL Chip Report, lines 728-875)

---

## Gating rule

`FUTUNER_PHASE2_ENABLED` stays `0` in `firmware/src/config/futuner_config.h`
until every item in this doc is GREEN. No carve-outs. No "ship it
broken, fix on field telemetry." No "wait for the customer to find
it." Phase 2 is the destructive surface — every PARTIAL on Phase 1
becomes a potential brick during a real CAL flash.

---

## Current state snapshot — 2026-05-21 (post scope reduction)

Two-column honesty: "host gate" = `firmware/test/<feature>/eval.sh` green;
"customer-experience" = customer plugs into RS7, opens browser, exercises
the feature, observes correct result. Phase 1 close gates on the
customer-experience column for every in-scope row.

| MISSION_SPEC § | Feature | Host gate | Customer experience | Notes |
|----------------|---------|-----------|---------------------|-------|
| 4.1 | VIN pairing + licensing | 🟢 PASS | 🟡 PARTIAL | Cloud confirmed `paid=1` on RS7; UI shows `paired:false` post-reboot (P-58 — persistence suspected broken) |
| **4.2** | **SBF / FBF live cal switching** | n/a | **🚚 MOVED TO PHASE 3 §6.1** | See `docs/PHASE_3_PREREQUISITES.md` P3-02. |
| **4.2** | **SBF / FBF builder tool** | n/a | **🚚 MOVED TO PHASE 3 §6.1** | See `docs/PHASE_3_PREREQUISITES.md` P3-01. |
| 4.3 | Live gauges (WebSocket streaming) | 🟢 PASS | 🟢 PASS | P-55 parser bugs fixed (commit `f916b04`); KOEO HIL re-verify shows nmot_w=0 rpm, tmot=25.5 °C ambient. P-57 routing fix applied earlier (commit `a54d690`). |
| 4.3 | Data logging (gzip + cloud sync) | 🟢 PASS | 🔴 BLOCKED | P-28 — `wot_logger_init` runs before profile load, `FEATURE_WOT_LOGGING` never registers |
| 4.4a | Ethanol BLE bridge (LOGGING-ONLY, Phase 1) | N/A | 🔴 NOT BUILT | `flex_*` commands return "Not yet implemented". Scope: ethanol % logged in WOT rows. |
| 4.4b | Ethanol BLE live-tune feed | n/a | **🚚 MOVED TO PHASE 3 §4.4b → §6** | See `docs/PHASE_3_PREREQUISITES.md` P3-06. |
| **4.5** | **Ethanol constraints + rev limiter** | n/a | **🚚 MOVED TO PHASE 3 §6.2** | See `docs/PHASE_3_PREREQUISITES.md` P3-03 / P3-04. |
| 4.6 | OBD fault code read | 🟢 PASS | 🟡 PARTIAL | Read returns 7 DTCs on RS7 wire-witnessed; UI routing prevents display (P-57 downstream) |
| 4.6 | OBD fault code clear | 🟢 PASS | 🔴 BROKEN | Demux fixed (P-53 RESOLVED); ECU returns NRC 0x11 — needs session-state fix (P-54) |
| 4.7 | Transport — CAN | 🟢 PASS | 🟢 PASS | Production driver in use |
| 4.7 | Transport — Ethernet skeleton | 🟡 PARTIAL | DEFERRED-PENDING-HARDWARE | Hardware "arriving soon" per spec |

**Reading the table:** the "Customer experience" column is the Phase 1
exit gate. Rows marked 🛑 DEFERRED do not block Phase 1 close. Rows
marked 🔴 BROKEN or 🟡 PARTIAL must reach 🟢 PASS before the
`FUTUNER_PHASE2_ENABLED=0` gate flips.

Three new firmware commits landed during HIL (`4253304`, `2e28b5f`,
`a54d690`) — all LOCAL, push pending (auth fix per Sean).

---

## Track A — Open P-item bug fixes (close before any new feature work)

Each item below has a one-pass diagnostic and a fix sketch. Most are
small, all are localized. Sequenced by dependency. None require ECU
write access; all are safe-to-test on RS7.

### A1 · P-54 — ClearDTC NRC 0x11 root cause

**Hypothesis:** Session-state issue, not service-support issue. Bosch
MG1 honors `0x14 ClearDTC` only in Extended Diagnostic Session, not in
Default Session. `0x19 ReadDTC` IS in the Default Session service set —
that's why Phase 4-read worked while Phase 4-clear didn't.

**Discriminator (3 added frames before clear request):**
1. `10 03` (DiagnosticSessionControl → Extended) → expect `50 03 …`
2. `14 FF FF FF` (ClearDTC) → look at the response:
   - Positive `54` → **fixed** (session was the issue)
   - NRC `33` → SecurityAccess required (chain `27 0x` after extended)
   - NRC `22` → conditionsNotCorrect (engine state requirement)
   - NRC `11` again → DID/service mapping deeper than session; escalate

**Fix scope (if hypothesis holds):** wrap `dtc_uds_clear_diagnostic_information`
with a session-entry preamble + session-exit. Add config flag
`DTC_CLEAR_REQUIRES_EXTENDED_SESSION` to `dtc_config.h` so other ECU
families that DON'T need it can opt out.

**Exit criteria:** `dtc_clear` over WS returns `{"ok":true}` AND DTC
read after clear returns 0 codes on dev RS7 AND wire witness shows
both the `10 03` and `14 FF FF FF` frames.

### A2 · P-55 — Logger DID resolution / value scaling

**Updated diagnosis (per 2026-05-21 KOEO clarification):** KOEO RPM
must be 0. Observed `-5369` cannot be derived from any byte
permutation of a valid 0. This is **not** a scale-formula bug. It's
either wrong DID mapping, response demux conflation (same class as
P-53), or wrong UDS service for the variant.

**Discriminator (one HIL session, three single-shot reads):**
1. Single-shot read of RPM DID alone (not the bundled 6-var poll)
2. Single-shot read with raw response bytes captured pre-parse
3. Cross-check active DID list in `firmware/src/logger/` against the
   A2L for `4K0907557G__0003` specifically (not whatever MG1 variant
   the table was originally sourced from)

**Pre-work:** Add `get_logger_data_raw` WS command surface that
returns the hex bytes pre-parse. This lets us validate against the
A2L offline without re-running HIL for every scale-formula guess.

**Fix scope:** Depending on which discriminator branch wins —
- Wrong DID table → regenerate DID map from `4K0907557G__0003` A2L
- Demux conflation → port the P-53 transport-layer NRC drain logic
- Wrong service → switch `$22 ReadDataByIdentifier` →
  `$23 ReadMemoryByAddress` for affected signals

**Exit criteria:** All 6 logger variables (`nmot_w`,
`InjSys_ratEthPrtnBascFu`, `Com_stCrCtlPan`, `rl_w`, `tmot`, `wdkba`)
return plausible KOEO values on dev RS7. Plausibility table in
`firmware/test/logger/koeo_baseline.json` for future regression.

### A3 · P-28 — WOT logger recorder init returns rc=258

**Root cause already confirmed** (per status-2026-05-19, lines 766-779):
`wot_logger_init()` runs in `main.c` before any logger profile is
loaded; `snapshot_logger_profile()` returns 0 vars; `wot_recorder_init`
rejects with `ESP_ERR_INVALID_ARG`; `FEATURE_WOT_LOGGING` never
registers.

**Fix path (recommended):** Defer
`wot_logger_register_with_feature_manager()` to a callback fired after
profile load. ~50 lines + new hook in `logger_profile.c`.

**Exit criteria:** Boot log shows `WOT_LOG: recorder init OK`.
`wot_log_start` over WS returns `{"ok":true,"feature":"wot_logger"}`.
With `wifi_mode ap` mid-active, `wot_log_start` returns
`{"ok":false,"error":"feature_active"}` (interlock test for the
network-feature-blocks-wifi-mode-swap safety net).

### A4 · P-29..P-32 — UI fixes lost in PC reset

Reserved entries during the 2026-05-19 audit close-out (per
`PHASE_2_PREREQUISITES.md:959-984`). Content was never recorded; need
to reconstruct from the PC handoff doc or accept the loss and refile
with current observations.

**Action:** Read `handoffs/archive/PC_PHASE1_HANDOFF.md` for the
original UI-fix list, refile content under P-29..P-32 with current
audit findings.

**Exit criteria:** Each of P-29..P-32 has either real content or is
explicitly marked OBSOLETE in `PHASE_2_PREREQUISITES.md`.

### A5 · P-33 — wifi_manager/eval.sh bash 3.2 portability

`declare -A` requires bash 4+. macOS ships bash 3.2.

**Fix paths (pick one):**
- Rewrite using parallel indexed arrays (portable across 3.2 / 4+)
- Migrate eval logic to a Python wrapper (consistent with `tools/`)

**Exit criteria:** `bash firmware/test/wifi_manager/eval.sh` runs to
completion exit 0 on stock macOS bash 3.2 without `brew install bash`.

### A6 · P-42 — Shadow-test primary halt gate regression

Tracked but no diagnostic captured this cycle. Blocks Phase 2
shadow-validation if not resolved. Need a fresh repro on `main` HEAD
post the three pending pushes.

**Exit criteria:** `firmware/test/mdg1_flash_orchestrator/eval.sh`
primary halt gate fires as designed.

### A7 · P-43 — Cloud `GET /admin/devices/{mac}` endpoint

**Diagnostic:** Endpoint doesn't exist on the cloud server; current
workaround is to GET the device list and filter client-side.

**Fix scope:** Add the endpoint to `cloud/src/main.py` as a single-row
selector against `devices` table by MAC. Mirror the existing
`POST /admin/devices/{mac}/license` access pattern (same auth, same
404 shape).

**Exit criteria:** `GET /admin/devices/{mac}` returns the row by MAC
or `404` if absent; admin auth required.

### A8 · P-47, P-48 — Cloud source + docs still reference `api.*`

Cleanup after `4253304` URL fix. Cloud source comments, `cloud/Caddyfile`,
`cloud/scripts/centos-server-setup.sh`, and 9 doc files still cite
`api.sillyrabbitmotorsport.com`. Dead reference but misleading.

**Exit criteria:** `grep -rE 'api\.sillyrabbitmotorsport\.com'
~/esp/obd/FUTV1.1/` returns nothing except in deprecation-noted
archive files.

### A9 · P-49 — Refactor 3 cloud HTTPS clients to single factory

Currently `vin_pairing.c`, `wot_logger.c`, `sbf_orchestrator.c` each
duplicate the `esp_http_client_config_t` initializer with
`.crt_bundle_attach`. Future TLS knobs (cert pinning, IP allowlist,
timeout policy) would require touching 3 sites.

**Fix scope:** Extract to `firmware/src/cloud/cloud_client.{c,h}` —
`cloud_client_https_init(esp_http_client_handle_t *out, const char
*path)` constructs the bundle-attached, host-correct config.

**Exit criteria:** All three sites call `cloud_client_https_init` and
no `esp_http_client_config_t` literal contains a `.crt_bundle_attach`
field outside `cloud_client.c`.

### A10 · P-50 — HTTPS smoke test in `firmware/test/`

Reproduces the TLS-handshake regression that P-46 surfaced on HIL.
Host-side test against a known-good CA bundle + a known-bad cert
(no SAN) using `esp_http_client` mocks. Catches future regressions
in the bundle attachment.

**Exit criteria:** `firmware/test/cloud_client/eval.sh` exists,
passes, and would have caught P-46 if it had existed.

### A11 · P-51 — Battery voltage read surface

Phase 1.6 should expose battery voltage for the pre-flash gate
(MISSION_SPEC §2.1 and Phase 2 P-04 will both need it). Standard OBD
PID `0x4221` or UDS DID. Add WS command `battery_voltage` returning
millivolts.

**Exit criteria:** `battery_voltage` WS command returns ECU-reported
battery voltage on dev RS7 within ±0.5 V of an OBD scan-tool reference.

### A12 · P-52 — Candlelight macOS gs_usb sustained-use wedge

Driver-level macOS issue. Both `can_sniff.py` and Sean's known-working
`can_tail.py` wedge after sustained use. Confirmed not a code issue.

**Options (Sean decides — listed for visibility, not for Claude to pick):**
1. Linux box for wire witness (kernel-mode `gs_usb` is stable)
2. Replace Candlelight with a USB-CAN adapter with proper macOS driver
   (PEAK PCAN-USB FD, Kvaser, or similar)
3. Userspace driver alternative (libusb-mac fork, gs_usb-mac project)

**Exit criteria:** Wire witness sustains a 60+ minute HIL session
without wedging. Required for re-establishing the three-stream
contract (WS + serial + wire) for the next HIL pass.

### A13 · P-56 — HIL doc still references `can_sniff.py`

`handoffs/PHASE1_HIL_VALIDATION.md` references `can_sniff.py`; Sean's
known-working sniffer is `~/sniffer/can_tail.py`. Per Sean's directive
2026-05-19 ("tell code to always use /Users/rabbit/sniffer/can_tail.py"),
HIL doc needs the swap.

**Exit criteria:** All capture-step invocations in
`handoffs/PHASE1_HIL_VALIDATION.md` use `can_tail.py`. `can_sniff.py`
remains in `tools/` but is no longer referenced by the canonical HIL
procedure.

### A14 · Formal P-item filing in PHASE_2_PREREQUISITES.md

P-44 through P-56 are referenced informally in the 2026-05-19 Chip
Report but have no formal entries in `docs/PHASE_2_PREREQUISITES.md`
(highest filed entry is P-40).

**Action:** Add formal entries for P-44 through P-56 mirroring the
existing P-item template. Mark P-44, P-45, P-46, P-53 as 🟢 RESOLVED
with commit references (`2e28b5f`, `4253304` ×2, `a54d690`).

**Exit criteria:** `docs/PHASE_2_PREREQUISITES.md` contains formal
entries for every P-NN referenced anywhere in `status-2026-05-19.md`
Chip Report.

---

## Track B — Reduced scope (post 2026-05-21 + 2026-05-22 owner directives)

> **Phase 3 silo 2026-05-22:** Tracks B1 (builder tool), B2 (live-tune
> E2E), B4 (constraints + rev limiter) have been **MOVED TO PHASE 3**.
> They are no longer Phase 1 work and do not gate Phase 1 close.
> See `docs/PHASE_3_PREREQUISITES.md` for P3-01..P3-11 tracking.
>
> Track B reduces to **B3** (ethanol BLE — LOGGING-ONLY scope, Phase 1)
> and **B5** (Ethernet skeleton, hardware-pending). Old in-tree content
> for the live-tune tracks remains in git history at `4253304^..HEAD`
> and is exercised in Phase 3, not removed.

### B3 · Ethanol Sensor BLE Bridge Firmware — LOGGING-ONLY (MISSION_SPEC §4.4a)

> **Logging path only.** The live-tune feed for the ethanol value
> (§4.4b) is Phase 3 work — see `docs/PHASE_3_PREREQUISITES.md` P3-06.
> Phase 1 ships the read path and the logged column only.

**Scope (reduced):** Receive ethanol percentage from external BLE
ethanol sensor; surface the value as a **logged variable only**.
The sensor reading appears in WOT log rows alongside RPM / boost /
AFR. Cloud log shows ethanol % per data point. If sensor disconnects,
manual override via UI keeps the logged value populated. (Constraint
engine + actuator paths intentionally absent — those land in Phase 3.)

**Where it lives:** `firmware/src/ethanol_ble/` — new module.

**Surface:**
- `flex_status` WS command → `{ethanol_pct, sensor_paired,
  last_update_ms, fallback_active}`
- `flex_set_override` WS command → manual override; takes precedence
- BLE pairing flow: discovery + bonding once during setup; auto-reconnect
- Fallback: BLE drop → `sensor_paired=false, fallback_active=true`;
  manual entry path stays live

**Hardware decisions (Sean input needed — D2, D3 from decision table):**
- Which ethanol sensor product is the target?
- Is sensor present on dev RS7 for HIL verification, or order one in?

**Modular scope:**
- `ethanol_ble_scanner.c` — passive scan for known service UUID
- `ethanol_ble_parser.c` — payload decode (sensor-specific)
- `ethanol_ble_state.c` — bonded peer + last-known-value state
- `ethanol_source.h` — abstraction (in the reduced scope, the only
  consumer is the WOT log row builder)
- `wot_recorder` integration: ethanol_pct column added to log schema

**Exit criteria:** With a real BLE ethanol sensor in scan range on
RS7, `flex_status` returns the sensor's current reading. Disconnect
sensor; `flex_status` reports `sensor_paired=false, fallback_active=true`
within configured timeout. Manual override via `flex_set_override`
takes precedence. During a WOT log pull, log row contains an
`ethanol_pct` field populated from whichever source is active.

### B5 · Phase 4.7 Transport Abstraction — Ethernet Driver Skeleton

**What:** MISSION_SPEC §1.7 calls for a transport-agnostic UDS/ISO-TP
stack so CAN and Ethernet can both back the same upper layers.
Currently CAN driver is finalized; Ethernet driver skeleton has not
been verified to compile against the abstract interface.

**Hardware status (Sean input needed):** Spec says "Ethernet hardware
arriving soon" — has it arrived? If not, this becomes a
design-and-compile-only deliverable (no on-car verification possible).

**Modular scope:**
- `firmware/src/transport/transport_iface.h` — abstract send / receive
  / status interface (likely already exists in some form)
- `firmware/src/transport/transport_can.c` — current CAN driver
  refactored to implement the interface
- `firmware/src/transport/transport_eth.c` — Ethernet driver
  implementing the same interface (W5500 or equivalent per spec)
- ISO-TP layer calls abstract methods only — verify no CAN-specific
  literals leak into upper layers

**Exit criteria (if hardware available):** Both transports pass the
same host-side regression suite. Switching transport at boot is a
config-only change. Failover logic (primary unavailable → try
secondary) verified.

**Exit criteria (if hardware NOT available):** Code review of
upper-layer ISO-TP confirms no CAN-specific assumptions. Ethernet
skeleton compiles. Defer on-car verification until hardware arrives;
mark item DEFERRED-PENDING-HARDWARE rather than blocking Phase 1 close.

**Audit findings (2026-05-22, overnight Phase α):**

- `firmware/src/transport/` did not exist. Scaffolded: README.md +
  `transport_iface.h` (proposed interface, mirrors the Phase 2
  `mdg1_uds_transport_t` shape) + `transport_eth.c` (TODO stubs).
  None registered in CMakeLists.txt — design-doc-in-code-form
  rather than active firmware.
- Phase 2 flash orchestrator already implements its own per-call
  transport vtable (`mdg1_uds_transport_t`). Phase 1 main UDS path
  does NOT — calls `can_manager_*` functions directly. Upper
  layers (`isotp_coordinator`, `logger_manager`, `dtc_uds`,
  `vin_pairing`) only touch the CAN-named facade, not TWAI
  primitives directly. Migration to the transport interface should
  be a thin adapter swap on the CAN side; the big work is the
  Ethernet leg.
- `main_sniff.c` uses `twai_general_config_t` directly, but that's
  a separate sniffer build target gated by `SNIFF_MODE=1` — not in
  the production runtime path.
- Status STAYS DEFERRED-PENDING-HARDWARE. README.md captures the
  migration sequence; reviewable in
  `firmware/src/transport/README.md`.

---

## Track C — Verification gaps

### C1 · Post-fix HIL Phase 1 full re-run

After Tracks A and B close, the entire Phase 1 HIL procedure (per
`handoffs/PHASE1_HIL_VALIDATION.md`) needs to re-run end-to-end as
a single dispatch. Every phase must PASS, not PARTIAL. Three-stream
contract must hold (depends on P-52 resolution).

**Exit criteria:** Chip Report shows PASS on every phase. No PARTIAL.
No N/A (or only the explicit hardware-pending ones).

### C2 · Phase 1 Regression Test Pinning

After all features pass HIL, capture each feature's golden output
(WS responses, wire frames, log shapes) as test fixtures under
`firmware/test/<feature>/golden/`. Per-feature `eval.sh` extended to
diff against golden on every run. Prevents the kind of silent
regressions that bit us during the PC/Mac merge cycle.

**Exit criteria:** Each of the 5 in-scope Phase 1 feature modules
(vin_pairing, dtc, wot_logger, ui, ethanol_ble) has a golden-output
fixture in its test directory and the eval gate diffs against it.
(The Phase 3 features — calibration switcher, constraint engine — get
their own golden fixtures in Phase 3.)

---

## Sequencing

Dependencies and parallelization opportunities.

**Phase α — Cleanup and unblock (1 session, no HIL needed)**

Parallelizable:
- A14 (formal P-item filing) — pure doc work
- A4 (P-29..P-32 reconstruction) — pure doc work
- A5 (P-33 bash 3.2) — host tool work
- A7 (P-43 cloud endpoint) — cloud server work
- A8 (P-47/P-48 api.* cleanup) — search-and-replace

Push the 3 pending commits as part of this phase.

**Phase β — Bug fix HIL (1 HIL session)**

- A1 (P-54 ClearDTC) — 3-frame discriminator
- A2 (P-55 logger) — add raw-byte surface, then off-vehicle A2L cross-check
- A3 (P-28 WOT logger init) — firmware refactor + boot test
- A6 (P-42 shadow-test) — host work + a single HIL probe

Requires P-52 resolution OR explicit two-stream-contract acceptance.

**Phase γ — Cloud client refactor + smoke (1 session, host-side)**

- A9 (P-49 single factory)
- A10 (P-50 smoke test)
- A11 (P-51 battery voltage)
- A13 (P-56 HIL doc → can_tail.py)

Phases α, β, γ should finish before Track B starts. Reason: Track B
features build on top of the cleaned-up infrastructure. Doing them
out of order means rewriting B work when α/β/γ land.

**Phase δ — Reduced unbuilt features (1-2 sessions)**

- B3 (Ethanol BLE bridge — **LOGGING-ONLY** per 2026-05-21 scope cut)
  — firmware + HIL with sensor; logged variable in WOT rows. The
  live-tune feed for this sensor is Phase 3 work, not Phase δ.
- B5 (Ethernet skeleton) — design-and-compile if hardware not
  arrived; full validation deferred-pending-hardware

The former B1 / B2 / B4 entries are Phase 3 work (see
`docs/PHASE_3_PREREQUISITES.md`). Phase δ in this plan is now
small and well-bounded.

**Phase ε — Final HIL + regression pinning (1 HIL session + 1 host session)**

- C1 (full Phase 1 HIL re-run)
- C2 (golden fixture pinning)

After ε closes, Phase 1 is PERFECT. Phase 2 unblocked from
Phase-1-side standpoint (Phase 2 still has its own prereqs in
`PHASE_2_PREREQUISITES.md`).

---

## Decision points needing owner input

These gate planning, not implementation. Surfaced for explicit
Sean sign-off before each track starts.

| ID | Question | Affects |
|----|----------|---------|
| D1 | P-52 wire witness — Linux box, new hardware, or accept two-stream contract permanently? | Phase β onward |
| D2 | Ethanol sensor product choice — which BLE sensor is the v1 target? | B3 |
| D3 | Ethanol sensor on dev RS7 today, or order one in? | B3 HIL timing |
| D4 | Ethernet hardware status — arrived yet? | B5 (becomes design-only if no) |
| ~~D5~~ | ~~Rev limiter RAM address on `4K0907557G__0003`~~ 🚚 | MOVED TO PHASE 3 — P3-04 (`docs/PHASE_3_PREREQUISITES.md`) |
| ~~D6~~ | ~~File storage location for cal switching — `/storage/...`~~ 🚚 | MOVED TO PHASE 3 — P3-05 |
| D7 | Admin password rotation (P-19/P-20 security pass) — bundle into Phase 1 or defer? | Out of plan |

---

## Phase 1 EXIT criteria — binary checklist

When every line below reads `🟢`, Phase 1 is PERFECT and Phase 2 is
unblocked from a Phase-1-side correctness standpoint.

- [ ] All 3 pending commits pushed to `origin/main`
- [ ] `PHASE_2_PREREQUISITES.md` contains formal entries for every
      P-NN referenced in the project (no orphan numbers)
- [ ] Every P-item in this plan that is not RESOLVED-AS-DEFERRED
      shows 🟢 in `PHASE_2_PREREQUISITES.md`
- [ ] MISSION_SPEC §4.1 — VIN pair + license: HIL PASS, persistence
      across power cycle verified
- [ ] ~~MISSION_SPEC §4.2~~ 🚚 MOVED TO PHASE 3 — see
      `docs/PHASE_3_PREREQUISITES.md` (P3-01, P3-02). Not a Phase 1
      exit gate.
- [ ] MISSION_SPEC §4.3 — Live gauges: all 6 vars return plausible
      KOEO values (P-55 closed); UI routing fixed (P-57 closed);
      gauges populate in browser on dashboard mount
- [ ] MISSION_SPEC §4.3 — Data logging: P-28 closed; WOT log
      captures, gzip, cloud-uploads, and local-deletes within
      configured window
- [ ] MISSION_SPEC §4.4a — Ethanol BLE bridge (LOGGING-ONLY, Phase 1):
      sensor reading appears in WOT log rows; manual fallback
      functional on RS7. (§4.4b live-tune feed is Phase 3 work — see
      P3-06, not a Phase 1 exit gate.)
- [ ] ~~MISSION_SPEC §4.5~~ 🚚 MOVED TO PHASE 3 — see
      `docs/PHASE_3_PREREQUISITES.md` (P3-03, P3-04). Not a Phase 1
      exit gate.
- [ ] MISSION_SPEC §4.6 — Fault code read AND clear both PASS on RS7
      (P-54 closed for clear)
- [ ] MISSION_SPEC §4.7 — Transport abstraction: CAN PASS in
      production; Ethernet design-verified (deferred-pending-hardware
      acceptable here)
- [ ] Three-stream HIL contract restored (WS + serial + wire), OR
      two-stream contract explicitly accepted by owner
- [ ] Full Phase 1 HIL dispatch (C1) shows PASS on every phase, no
      PARTIAL
- [ ] Per-feature golden fixtures landed (C2)
- [ ] `FUTUNER_PHASE2_ENABLED` still 0 — flipping it is a separate
      owner-signed action
- [ ] `FUTUNER_PHASE3_ENABLED` still 0 — flipping it is a separate
      owner-signed action (Phase 3 silo, 2026-05-22)

---

## Out of scope for Phase 1 PERFECT

These exist and matter but do NOT block Phase 1 close. Listed here
for visibility, owned elsewhere.

- Phase 2 prerequisites (`PHASE_2_PREREQUISITES.md` P-01 through P-23+)
- Phase 3 prerequisites (`PHASE_3_PREREQUISITES.md` P3-01 through P3-11) —
  live-tune ecosystem, owned and tracked separately as of 2026-05-22
- Multi-variant cal support beyond `4K0907557G__0003` — moved to
  Phase 3 with the rest of the cal-switching surface (P3-10)
- Security pass (P-19 default AP password, P-20 admin password, P-25
  WS auth posture review)
- Cloud build pipeline integration (P-13)
- Recovery binary per ECU family (P-03) — Phase 2 surface
- Per-variant manifest schema beyond dev car (Phase 3, P3-10)
- AES key custody decision (P-06) — Phase 2 surface

---

## Update protocol

When an item closes, mark its track entry with the commit SHA and
flip its status in `PHASE_2_PREREQUISITES.md`. Add a one-line note in
today's `status-YYYY-MM-DD.md`. When you discover a new item that
fits Phase 1, add it under the right Track with a new identifier
(A15, B6, C3, etc.) — don't reuse retired numbers.

---

## Owner directive log

### 2026-05-21 — Phase 1 scope reduction (live tune deferred)

> **Sean / SRM Engineering.** Phase 2 fully gated until reduced-scope
> Phase 1 is 100% on-car validated. No exceptions.

Live-tune surface (MISSION_SPEC §4.2 SBF live calibration switching,
§4.5 constraint logic + rev limiter, §6 Phase 1 Deliverable 3 SBF/FBF
builder tool, Track B1/B2/B4 in this doc) moves to a later phase after
the non-tuning Phase 1 surface is shipped and customer-validated
end-to-end.

Rationale: Phase 1 was never validated end-to-end. Host gates pass
but on-car customer-experience testing never happened. The 2026-05-21
Claude-in-Chrome UI vet (`firmware/test/ui/claude_chrome_vet_report_2026-05-21.md`)
surfaced P-57 through P-61 — BLOCKER-class UI issues that gate the
customer experience independently of any live-tune work. The directive
sequences cleanly: close P-57..P-61 + P-28 + P-54 + P-55 first, then
re-validate the in-scope §4.x deliverables end-to-end on RS7, then
reconsider live tune as a follow-on phase.

**What stays in Phase 1:**
- §4.1 VIN pair + license (with persistence fix per P-58)
- §4.3 Live gauges + WOT log + cloud sync
- §4.4 Ethanol BLE bridge — **scope reduced to LOGGING-ONLY**
- §4.6 DTC read + clear
- §4.7 Transport (CAN; Ethernet deferred-pending-hardware)

**What moves to a later phase:**
- §4.2 SBF live calibration switching
- §4.5 Ethanol constraint engine + rev limiter
- §6 Phase 1 Deliverable 3 — SBF/FBF builder tool
- Track B1, B2, B4 (no Phase 1 sessions, no HIL exercise)

**Phase 2 (full binary flash) stays gated.** `FUTUNER_PHASE2_ENABLED`
stays `0` until reduced Phase 1 closes per the EXIT criteria checklist
above.

### 2026-05-22 — Phase 3 silo (live tune given its own phase)

> **Sean / SRM Engineering.** The live-tune ecosystem moves out of
> "later phase" limbo and into a discrete **Phase 3**. Phase 1
> remains gated by the EXIT checklist above; Phase 2 (destructive
> flash) and Phase 3 (live RAM-write tuning) are independent gates
> beyond it.

What changed today:

1. New doc `docs/PHASE_3_PREREQUISITES.md` seeded with P3-01..P3-11,
   mirroring the structure of `PHASE_2_PREREQUISITES.md`. It owns
   the calibration-file builder tool, the live cal-switching apply
   path on RS7, the ethanol constraint engine, the rev-limiter RAM
   address, on-device storage layout, the BLE live-tune feed, the
   pre-apply safety gate, the 9 map-switch slots UI, the
   `FEATURE_LIVE_TUNE` arbitration registration, the manifest schema,
   and the `FUTUNER_PHASE3_ENABLED` build flag.

2. `MISSION_SPEC.md` restructured: a new §6 ("Phase 3 — Live Tuning
   Ecosystem") sits between Phase 2 and the development roadmap.
   §4.2 and §4.5 are now pointer stubs into §6. §4.4 split into
   §4.4a (Phase 1 logging-only) and §4.4b (Phase 3 live-tune feed
   pointer).

3. This plan (`PHASE_1_COMPLETION_PLAN.md`) scrubbed: B1/B2/B4 are
   no longer Phase 1 work, the exit checklist no longer references
   §4.2 / §4.5 as Phase 1 gates, decision points D5/D6 redirect to
   Phase 3 P-items, and the Track B intro callout points readers
   at `PHASE_3_PREREQUISITES.md` rather than calling the work
   "deferred."

4. `firmware/src/config/futuner_config.h` gets a `FUTUNER_PHASE3_ENABLED`
   compile flag defaulting to `0`, mirroring the existing
   `FUTUNER_PHASE2_ENABLED` pattern. Flipping it is a separate
   owner-signed action just like Phase 2.

Rationale for siloing rather than continuing to call it "deferred":
deferred items rot in plans because there is no owner, no doc, no
P-numbers, no exit criteria. Naming a Phase 3 forces those to exist
up front. It also makes the customer-facing roadmap honest:
non-destructive logging + DTC (Phase 1) → destructive flash (Phase 2)
→ live RAM-write tuning (Phase 3), each gated behind its own
compile flag and its own owner sign-off.

What is **NOT** changed:

- Phase 1 EXIT criteria above. Phase 1 still closes when its own
  checklist is green. Phase 3 work does not gate Phase 1 close.
- Phase 2 prereqs in `PHASE_2_PREREQUISITES.md`. Those items
  remain Phase 2's responsibility. Where a former Phase 2 prereq
  is actually Phase 3 territory, it is annotated in-place with a
  "MOVED TO PHASE 3 — see PHASE_3_PREREQUISITES.md P-NN" note;
  numbering is preserved.
- The B3 logging-only ethanol surface stays in Phase 1 (it is the
  read path; the live-tune actuator path is what moves).
- Phase 2 stays gated on Phase 1 closing, and now also stays
  independent of Phase 3 (you can ship Phase 2 with Phase 3
  disabled, and vice versa).
