# A11 battery_voltage WS surface — skipped (ECU-wire-surface)

## Block (2026-05-22)

A11 in PHASE_1_COMPLETION_PLAN.md asks for a `battery_voltage` WS
command using "standard OBD PID 0x4221 or vendor DID — use whichever
the existing logger/UDS layer already accesses (search the source)."

Search result: the dongle's logger layer reads ECU RAM via the
v1.0-style patched-ECU buffer, not standard OBD PIDs. The DTC layer
uses UDS 0x19/0x14 (read/clear). VIN pairing uses 0x09 0x02 (Mode 09
Info 02, standard OBD VIN). None of the existing UDS request shapes
read battery voltage.

To land A11 I'd have to:
- Add a new UDS request emitter (e.g., `0x22 0x02 0x8A` for the
  common VAG battery DID, or Mode 01 PID 0x42 / 0x4221 if the dispatch
  meant OBD-II), OR
- Add a new ADC-tap pathway on the ESP32-S3 (hardware change)

Both are new on-wire / on-rev-pin surfaces.

## Why I'm not landing this autonomously

Per the overnight dispatch's rules:

> 3. No ECU-wire-surface code changes (P-54 stays held for Sean's
>    explicit sign-off).
> [...]
> HALT criteria — surface block + move on, don't spin:
> - ECU-wire-surface change required → STOP item, document, skip

A11's only viable implementation paths add ECU-wire-surface code
(new UDS request emitter or new ADC sampling path). Same gating as
P-54 — Sean's explicit sign-off needed.

## Suggested resolution for morning review

Three options for Sean to choose:

1. **OBD-II Mode 01 PID 42 (Control module voltage)**
   Request bytes: `02 01 42`. Response decodes as `(A*256 + B) / 1000`
   in volts.
   This addresses ECU on `0x7E0` per existing convention.
   Pro: standardized across all OBD-II ECUs.
   Con: depends on the customer ECU exposing this PID — Bosch MG1
   typically does, but MED17 variants may not.

2. **VAG DID via UDS 0x22**
   Request bytes: `03 22 02 8A` (or similar VAG-specific DID; needs
   A2L lookup for `4K0907557G__0003`).
   Pro: works in the existing extended-session context the dongle
   already maintains.
   Con: per-variant DID — needs the A2L cross-check that's in the
   P-55 audit pile.

3. **ESP32-S3 ADC tap on OBD power pin (OBD-II pin 16)**
   Pro: independent of ECU support; works key-on/engine-off; reads
   the OBD-port voltage directly.
   Con: hardware change — needs a voltage divider on a board rev. The
   current BOARD_REV2 doesn't have an ADC tap on pin 16 mapped (per
   my read of the schematic notes).

P-51 in PHASE_2_PREREQUISITES.md already lists the same three
options. Pinging up to Sean for the choice.

## State at block time

No code touched. No firmware change. P-51 stays 🟡 in
PHASE_2_PREREQUISITES.md until Sean picks an implementation path.
