# transport/ — design review (B5, 2026-05-22)

> MISSION_SPEC §4.7 calls for a transport-agnostic UDS/ISO-TP stack
> so CAN and Ethernet can both back the same upper layers. This dir
> holds the proposed interface + future Ethernet skeleton. **Audit
> status as of 2026-05-22:** the Phase 1 main UDS path is NOT yet
> transport-abstracted; the Phase 2 flash orchestrator already
> implements its own per-call transport vtable.

---

## As-is state (audit findings)

### What's transport-abstracted today

`firmware/src/flash/` ships a per-call transport interface:

- `mdg1_uds_transport_t` struct (in `mdg1_flash_orchestrator.h`)
- `mdg1_transport_can.{c,h}` — production CAN implementation
- `mdg1_transport_shadow.{c,h}` — host-test shadow implementation

The orchestrator calls `transport->exchange(req, rx, ...)` and never
touches TWAI or CAN-driver primitives directly. This is the shape
MISSION_SPEC §4.7 prescribes — just scoped to Phase 2 (flash
orchestrator) instead of Phase 1.

### What's NOT transport-abstracted today

Phase 1's main UDS path (logger polling, DTC read/clear,
vin_pairing, ISO-TP coordinator) calls `can_manager_*` functions
directly. The API is CAN-named (`can_manager_send_isotp`,
`can_manager_receive_isotp`), uses TWAI peripherals
(`twai_general_config_t` in `can_driver.c`), and hardcodes
`ECU_PHYSICAL_TX_ID` / `ECU_PHYSICAL_RX_ID` from `can_config.h`.

Grep audit `firmware/src/ \ --include='*.c' --include='*.h' \
| grep -E 'twai_|CAN_'`:

| Module | Direct TWAI use | Direct CAN constant use |
|---|---|---|
| `firmware/src/can/can_driver.c` | yes | yes |
| `firmware/src/can/can_manager.c` | indirect (via can_driver) | yes |
| `firmware/src/main_sniff.c` | yes | yes |
| Phase 1 upper layers (`isotp_coordinator`, `logger_manager`, `dtc`, `vin_pairing`) | no | indirect (only via `can_manager_*`) |

Conclusion: the upper layers already call through a CAN-named
facade. Refactoring to a transport-named facade is a relatively
small change. The big work is implementing the Ethernet leg.

### Why it isn't blocking Phase 1

Per the 2026-05-21 owner directive (PHASE_1_COMPLETION_PLAN.md
"Owner directive log"), Ethernet hardware is "arriving soon" per
MISSION_SPEC §2. Phase 1 customer-experience validation runs on
CAN. B5 is explicitly DEFERRED-PENDING-HARDWARE in
PHASE_1_COMPLETION_PLAN.md.

---

## Proposed interface

See `transport_iface.h` in this dir for the v2 (Phase-1-and-beyond)
shape. Mirrors `mdg1_uds_transport_t`'s "open / exchange / close"
ownership model but lifts it out of the Phase 2 namespace.

Key contract bits:

```
typedef struct futuner_transport futuner_transport_t;

esp_err_t  futuner_transport_open  (futuner_transport_t **out, const futuner_transport_config_t *cfg);
esp_err_t  futuner_transport_send  (futuner_transport_t *t, const uint8_t *payload, size_t len);
esp_err_t  futuner_transport_recv  (futuner_transport_t *t, uint8_t *payload, size_t cap, size_t *out_len, uint32_t timeout_ms);
const char *futuner_transport_name (const futuner_transport_t *t);
esp_err_t  futuner_transport_close (futuner_transport_t *t);
```

Implementations:
- `transport_can.c` (planned) — adapter over `can_manager_*` so
  the existing Phase 1 wire path keeps working unchanged at the
  upper-layer call site. Phase 1 close doesn't need this; Phase β
  could land it as a refactor.
- `transport_eth.c` (skeleton, see below) — empty for now;
  populates once the W5500 hardware arrives.

---

## Migration path (when scope permits)

This is post-Phase-1-PERFECT work. Sequence:

1. Land `transport_iface.h` + `transport_can.c` as a thin adapter
   over `can_manager_*`. No call-site changes yet.
2. Refactor Phase 1 upper layers (`isotp_coordinator`,
   `logger_manager`, `dtc_uds`, `vin_pairing`) to call the
   transport interface. Should compile to byte-identical output
   for the CAN path (compiler should fold the indirection).
3. Implement `transport_eth.c` against the W5500 SDK (or LwIP +
   raw TCP if Ethernet is treated as a TCP socket rather than a
   raw PHY).
4. Wire boot-time selection (NVS key `transport_intent` or build
   flag) so the active transport is config-driven.

---

## Why scaffolding is here even though it's not built

Per the overnight dispatch B5 acceptance:

> If a Ethernet driver skeleton exists, verify it implements the
> same interface. If not, scaffold an empty
> firmware/src/transport/transport_eth.c with TODO stubs.

`transport_iface.h` lands the proposed interface so future work
has a target. `transport_eth.c` is a stub with TODOs. Neither is
registered in `firmware/src/CMakeLists.txt` yet — they're design
docs in code form, not active firmware. Wiring them in is the
first step of the refactor sequence above.

---

## Audit scope NOT done

- ISO-TP layer's transport-coupling depth: the `isotp_shims.c`
  helpers may bypass the transport interface even after a refactor.
  Needs a code-level diff during the actual refactor.
- W5500 hardware verification: deferred-pending-hardware per
  PHASE_1_COMPLETION_PLAN.md Track B5.
- Failover semantics: spec says "if primary transport unavailable,
  fall back to secondary." Out of scope for this audit; design
  decision Sean owns.
