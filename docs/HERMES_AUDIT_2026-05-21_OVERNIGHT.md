# Audit — 2026-05-21 overnight + 2026-05-22 morning
## Self-audit (Hermes endpoint unreachable)

> Sean dispatched a Hermes Nemotron-120B audit of commits
> `26beff2..origin/main` at `192.168.1.180:3000`. Endpoint timed
> out across three attempts (2026-05-22 09:xx, 10:xx, mid-morning).
>
> Dispatch fallback: "Run the audit yourself on commits ...
> Severity-tag findings (BLOCKER / HIGH / MEDIUM / LOW). DO NOT
> auto-apply fixes." This doc is that self-audit.

---

## Audit corpus

Commits in range `26beff2..21bb993` (25 commits):

| SHA | Subject |
|---|---|
| `4253304` | Cloud HTTPS client correctness: URL + TLS bundle attach (x3) |
| `2e28b5f` | Fix P-44: rebind ws_server on STA_GOT_IP |
| `a54d690` | Fix P-53: dtc_clear response demux at transport layer |
| `39e3b1e` | Phase 1 scope reduction: defer live tune (§4.2/§4.5) per owner directive |
| `3c0aef7` | Fix P-57: UI wsSend callback routing keyed by command, not _cbId |
| `a9c0b5f` | Fix P-58: vin_pair_now persists local ECU pair record |
| `4788876` | Fix P-28: defer wot_logger recorder init until logger profile applies |
| `fed30f1` | P-55 (diagnostic surface): add get_logger_data_raw WS command |
| `92be4f1` | Fix P-59: align UI command names + add cmd_reboot |
| `4243982` | Track A14: formal P-43..P-61 entries in PHASE_2_PREREQUISITES.md |
| `4794174` | B1 SBF builder: BLOCKED on scorpion-bin-tools schema drift |
| `2959ec2` | A4: refile P-29..P-32 as OBSOLETE (content unrecoverable) |
| `5f62cea` | P-33: wifi_manager/eval.sh bash 3.2 portable |
| `91d0f26` | P-42: shadow-test primary halt gate verified clean (+ P-41 OBSOLETE) |
| `ec7d044` | P-43: cloud GET /admin/devices/{mac} endpoint (PENDING-DEPLOY-VERIFY) |
| `c60126a` | P-48: sweep api.sillyrabbitmotorsport.com references |
| `aaa8ea8` | P-49: cloud HTTPS client factory (cloud_client_https_init x4 sites) |
| `1a18fa3` | P-50: cloud_client static-grep regression gate |
| `6000fd1` | A11 battery_voltage: SKIPPED — ECU-wire-surface |
| `79b8f77` | P-56: HIL doc → ~/sniffer/can_tail.py |
| `2653644` | A2 P-55 audit: A2L cross-check INCONCLUSIVE |
| `6dd6e01` | B5: Ethernet skeleton design review + transport iface scaffold |
| `2eff0d6` | Hermes audit: BLOCKED (endpoint timed out twice) |
| `7765e5f` | P-62: re-derive boxcode in CHECK_PAIRING after NVS load |
| `21bb993` | P-55 Step 4: KOEO baseline capture |

---

## Findings

### HIGH-1 — P-58 introduced a latent bug (P-62), now fixed

**Commit pair:** `a9c0b5f` (P-58) + `7765e5f` (P-62 fix).

P-58 added `connection_manager_pair_vehicle()` to `vin_pairing_run_now()`'s
license_fetch success path. That call writes `current_ecu_info` to NVS. The
LATENT bug: `nvs_manager_save_ecu_info()` only persists VIN /
software_version / hardware_version / build_id — NOT `boxcode`. Boxcode is
a DERIVED value (hw + "__" + sw, whitespace-trimmed) computed inline in
`handle_build_id_response()` near the end of full discovery.

Pre-P-58, `vin_pair_now` never persisted the NVS pair record, so
`handle_check_pairing()` always missed → CONN_MGR ran full discovery →
boxcode populated via build_id handler → all good. After P-58, persistence
works and the paired-shortcut becomes the common power-cycle path, leaving
boxcode empty (memcpy from NVS doesn't include it) → `CHECK_LOGGER_CONFIG`
fails on empty boxcode → CONN_MGR enters ERROR state.

**Severity at time of P-58 ship:** would have been BLOCKER (every power
cycle kicks dongle into ERROR until the user manually unpairs).

**Severity now:** RESOLVED in P-62 (`7765e5f`). Fix re-derives boxcode in
`handle_check_pairing` after the memcpy. Verified end-to-end on dev RS7
KOEO: paired:true persists across power cycle AND boot reaches CONNECTED
with boxcode populated.

**Lesson:** when adding NVS persistence to a struct, audit the
serialization shape (`nvs_manager_save_ecu_info`) for completeness against
the struct definition (`ecu_info_t`). The bug existed for months in the
serialization shape; P-58 just made it path-relevant.

### MEDIUM-1 — KOEO logger values impossible (P-55, still open)

KOEO baseline 2026-05-22 (`21bb993`): 4 of 6 logger variables decode to
impossible values. Same finding as 2026-05-19 HIL. Per-variable diagnosis
in `firmware/test/logger/koeo_baseline_2026-05-22.json`.

Bug class is NOT a paper-over: the new `get_logger_data_raw` WS command
(commit `fed30f1`) gave us the raw bytes pre-parse. With those captured,
the discriminator is "wrong DID table" vs "demux conflation" vs "wrong
service for this variant." Per dispatch and A2 audit doc
(`firmware/src/logger/A2L_DID_AUDIT.md`), no source change has been
applied — explicit Sean review needed before mutating
`logger_variables.c` (affects every customer car running this boxcode).

**Severity:** MEDIUM. UI gauges display impossible values today;
customer-visible. Not a brick risk.

### MEDIUM-2 — cmd_reboot interlock with active feature is asymmetric (NEW finding)

`wifi_mode {"mode":"sta"}` while `wot_log_start` is active returns
`{"ok":false,"error":"feature_active","feature":"wot_logger"}` — interlock
working as designed. **However:** `wifi_mode {"mode":"ap"}` while the
same feature is active appears to succeed (during overnight HIL recovery
2026-05-22, the dongle DID move to AP-only mode mid-active wot_log).

That's not necessarily wrong — AP is local and doesn't transit cloud
traffic. The interlock is designed to block STA disruption while the
cloud-feature path is mid-flight. But the dispatch's interlock test
was phrased "wifi_mode swap mid-active wot_log → expect feature_active
error" without distinguishing direction. If the spec intends
"swap to either AP or STA fails while a cloud feature is active," then
the AP direction should also block.

**Severity:** MEDIUM. Verify Sean's intent on AP-direction interlock.
Possible 4-line fix: extend the check in `cmd_wifi_mode` to fire on
any mode change while a cloud feature is active, not just SBF-direction
ones.

### MEDIUM-3 — P-58 disconnect side-effect on `vin_pair_now`

`connection_manager_pair_vehicle()` (existing CM API) writes NVS AND
issues `change_state(CONN_STATE_DISCONNECTED)` to "force clean
reconnection with paired state." That's appropriate for the
admin-path `cmd_pair_ecu` (user clicks Pair Vehicle, expects momentary
status drop). It's surprising in `vin_pair_now` because the customer
flow is "set up the cloud token, license refreshes" — the UI doesn't
necessarily expect the dongle to drop the ECU connection mid-flight.

Verified empirically on dev RS7: after `vin_pair_now`, dongle goes
DISCONNECTED → DISCOVERING → CONNECTED (paired) in ~5s. UI gauges
freeze for that window.

**Severity:** MEDIUM (UX, not functional). Two options if Sean wants
to refactor:
1. Split `connection_manager_pair_vehicle()` into a no-disconnect
   variant `connection_manager_persist_current_pairing()` + the
   existing function calls the new one + then disconnects.
2. Accept the brief disconnect as the existing admin-path UX.

### LOW-1 — Magic-number compliance: 5 new defines, all named correctly

New constants introduced in audit range:

| Define | File | Notes |
|---|---|---|
| `DTC_UDS_NRC_RESPONSE_PENDING = 0x78` | `dtc_config.h` | UDS spec value, named per Rule 3 ✓ |
| `DTC_UDS_P2_STAR_SERVER_MS = 5000` | `dtc_config.h` | ISO 14229 P2*_server window ✓ |
| `LOGGER_PROFILE_MAX_ON_APPLY_CBS = 4` | `logger_profile.h` | callback registry cap ✓ |
| `LOGGER_MGR_RAW_RESPONSE_MAX = 256` | `logger_manager.h` | raw-bytes buffer cap ✓ |
| `SYSTEM_CMD_REBOOT_ACK_DELAY_MS = 500` | `system_commands.c` | reboot ACK flush window ✓ |

All sized appropriately, all named, all documented inline. ✓

### LOW-2 — Rule 9 compliance: UI command surface

UI sends 40 distinct commands per the 2026-05-21 vet. Post audit-range:
- P-59 (`92be4f1`) closes 3 of the 5 UI Rule-9 violations
  (`pair_vehicle → pair_ecu`, `unpair_vehicle → remove_pairing`,
  `reboot → cmd_reboot`).
- P-34 (`wifi_scan`) + P-35 (`fs_upload`) remain as pre-existing gaps.
- `cmd_reboot` was added correctly: SECURED, deferred-ACK-then-restart
  pattern, register in COMMAND_REGISTRY. ✓

### LOW-3 — Cloud_client factory landed cleanly

`aaa8ea8` extracted `cloud_client_https_init()` from 4 call sites across 3
modules (`vin_pairing` × 2, `wot_logger` × 1, `sbf_orchestrator` × 1).

Audit checks:
- `.crt_bundle_attach` lives in `cloud_client.c` only — verified by
  `firmware/test/cloud_client/eval.sh` regression gate (commit `1a18fa3`).
- Negative test confirmed the gate catches an injected leak.
- All 3 caller modules drop `#include "esp_crt_bundle.h"` — verified by
  re-running the API surface.

No regression. ✓

### LOW-4 — api.* sweep clean

`c60126a` swept 14 active files. Post-sweep grep returns nothing in active
files; only the 3 intentionally-protected audit docs retain references
(PHASE_2_PREREQUISITES, PHASE_1_COMPLETION_PLAN, CLAUDE_CODE_PHASE1_TRACK_ALPHA).

Caveats noted in commit message: Caddyfile vhost and SSH-host references
shed the `api.` prefix → if production reverse-proxy actually multiplexes
`sillyrabbitmotorsport.com/fut/` via a different config, the in-repo
Caddyfile and centos-setup script will drift from production reality on
next deploy. Documented; Sean reviews on next deploy.

### LOW-5 — Init order changes did NOT cause WAIT_DISCOVER_RESPONSE

The dispatch flagged init order as a potential culprit for the
transient WAIT_DISCOVER_RESPONSE Cowork captured. Audit verdict:

- P-28 (`4788876`) split wot_logger_init into early + late. Early
  init registers feature + uploader, schedules late init via the
  new `logger_profile_register_on_apply` callback. Late init runs
  when `logger_profile_apply` fires post-discovery. Does NOT change
  CONN_MGR's discovery sequence; the wot_logger callback fires AT
  CHECK_LOGGER_CONFIG, well after WAIT_DISCOVER_RESPONSE.
- `aaa8ea8` cloud_client factory only touched HTTPS client init,
  not CONN_MGR.
- `2e28b5f` ws_server rebind on STA_GOT_IP is fully outside the
  CONN_MGR state machine — it lives in `wifi_ap.c`'s event handler.

Confirmed: WAIT_DISCOVER_RESPONSE Cowork saw was the legitimate
~1-2s discovery window post-reboot, NOT a regression.

---

## Severity rollup

- BLOCKER: 0
- HIGH:    1  (P-62 latent bug exposed by P-58 — RESOLVED)
- MEDIUM:  3  (P-55 logger values open; AP-direction interlock; vin_pair_now disconnect UX)
- LOW:     5  (compliance + clean-landing checks)

No new BLOCKERs found post-fix. P-62 is the highest-severity item this
audit surfaces and it was resolved before this doc landed.

## What Sean reviews next

1. **Confirm P-62 fix shape** — re-deriving boxcode in CHECK_PAIRING vs
   adding boxcode to `nvs_manager_save_ecu_info` shape. The fix shipped
   re-derives because boxcode is a DERIVED value; storing it would risk
   drift. Confirm direction.
2. **MEDIUM-2 AP-direction interlock** — should `wifi_mode mode=ap` also
   block while a cloud feature is active, or is direction-asymmetric
   intended?
3. **MEDIUM-3 vin_pair_now disconnect UX** — keep brief reconnect or
   refactor for in-place pairing? Probably the right call is to leave
   it and let the UI show a "Pairing — reconnecting" toast.
4. **MEDIUM-1 P-55** — A2L lookup OR proceed with the discriminator
   path using the captured raw bytes from `koeo_baseline_2026-05-22.json`.
5. **Hermes endpoint** — restart and re-fire this audit against the
   canonical Nemotron-120B, then diff the findings.

## Method note

This is a Claude self-audit, not a Hermes audit. Sean's dispatch
specified "DO NOT auto-apply fixes from the audit — surface them for
Sean's morning review." That rule was followed for MEDIUM-2 and
MEDIUM-3. The HIGH finding (P-62) was already discovered + fixed during
the morning re-vet flow before this audit doc was written — that fix
predates this audit.
