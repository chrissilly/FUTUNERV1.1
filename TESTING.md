# TESTING.md — Flash & exercise a FUTUNER dongle (no dev setup)

> For a **trusted tester** who wants to flash a dongle and exercise
> Phase 1 — on the bench or in a car — *without* installing the full
> ESP-IDF build toolchain. If you intend to **build** firmware from
> source instead, use the `firmware/build.sh` path in `README.md`.
>
> **Phase 2 (full binary ECU flash) and Phase 3 are compile-gated OFF**
> in this build — `firmware/src/config/futuner_config.h` has
> `FUTUNER_PHASE2_ENABLED 0` and `FUTUNER_PHASE3_ENABLED 0`. Nothing in
> this guide reflashes the ECU; Phase 1 is RAM-only / standard-OBD and
> non-destructive.

## 0. What you need

- A FUTUNER dongle (ESP32-S3, BOARD_REV2) + USB-C cable.
- A **macOS or Linux** host. (Windows isn't supported by the tooling.)
- `esptool` to flash:  `pip install esptool`  (already present if you have ESP-IDF).
- `pyserial` to watch the boot log (optional):  `pip install pyserial`.
- A phone/laptop with Wi-Fi + a browser, to reach the dongle UI.

## 1. Flash the prebuilt firmware (no build)

The repo ships known-good binaries in `firmware/prebuilt/`
(`futuner_v2.bin`, `bootloader.bin`, `partition-table.bin`,
`ota_data_initial.bin`). From the repo root:

```bash
cd firmware
./flash.sh --prebuilt --full      # first time on a dongle: bootloader + part-table + otadata + app
./flash.sh --prebuilt             # later: re-flash just the app (fast)
```

`flash.sh` auto-detects the first `/dev/cu.usbmodem*` port. Pass an
explicit port if you have more than one device connected:

```bash
./flash.sh --prebuilt --full /dev/cu.usbmodemXXXX
```

Watch it boot (optional):

```bash
./monitor.sh                      # Ctrl+C to exit  (./monitor.sh -t 30 for a 30s capture)
```

> `flash.sh` runs `esptool` through the ESP-IDF venv python if present,
> otherwise plain `python3 -m esptool` — so `pip install esptool` is all
> the prebuilt path needs.

## 2. Connect to the dongle + open the UI

After boot the dongle hosts its own Wi-Fi access point:

| | |
|---|---|
| **SSID** | `FUTUNER_xxxxxx` (last 6 hex derived from the device serial) |
| **Password** | `password` (compile-time default in `firmware/src/config/wifi_config.h`; per-device override in NVS) |
| **UI** | open `http://192.168.10.1/` once joined |

## 3. Exercise Phase 1 (read-only, non-destructive)

From the dongle UI:

- **Live gauges** — confirm the dashboard streams engine values (RPM,
  coolant, etc.).
- **DTC read / clear** — read stored diagnostic codes, then clear them.
- **WOT log** — start/stop a wide-open-throttle capture and confirm a log
  is produced.

These are all RAM-only / standard-OBD reads and writes — they never
reflash the ECU.

### On-car / hardware-in-the-loop validation

For the full procedure against a real keyed-on vehicle — triangulating
the dongle's WebSocket responses, the rendered UI, and an independent
Candlelight wire capture — follow **`handoffs/PHASE1_HIL_VALIDATION.md`**.

## Safety rails (enforced in firmware)

- **CAN ID `0x7E0` only** — never `0x7DF`, `0x710`, or `0x7E1`–`0x7E7`.
  Address scanning locks out the C8 J533 gateway for ~10 minutes.
- **Standard UDS services only**; no raw CAN probing.
- **Board:** BOARD_REV2 — GPIO 5 TX, GPIO 16 RX, 500 kbps.

See `README.md` → *Critical Operational Rules* for the full list.
