# tools/ — host-side scripts

Four small Python utilities that support the FUTUNER firmware + UI
build / push / debug loop. None of them touch firmware C source; they
sit alongside the firmware tree and consume artifacts.

| Script | Role |
|---|---|
| `bundle_ui.py` | Concatenate `ui/control_panel.{html,css,js}` into the single-file `firmware/futuner_control_panel.html` that the dongle's flash partition serves. Deterministic — same input bytes → same output bytes. Wired into `firmware/build.sh` so every build refreshes the bundle automatically. |
| `sbf_to_json.py` | Decode an SCPN-format SBF calibration file into a JSON snapshot of header + segments + maps. Useful for diffing two SBFs or sanity-checking a build. |
| `can_sniff.py` | macOS-friendly CAN sniffer wrapping a CandleLight-style USB-CAN adapter via gs_usb. Standalone — for bench debugging. |
| `bench_push.py` | Phase-gated bench-day push pipeline. Build → flash → push cloud assets → provision the dongle, all in one CLI. See below. |

## bench_push.py — quick start

End-to-end bench loop in one invocation:

```sh
ADMIN_API_KEY='...' tools/bench_push.py --all \
    --port /dev/cu.usbmodem1101 \
    --sbf-file sbf/stage1_patched.sbf \
    --paid 1
```

Partial runs are equally one-line:

```sh
# Just push a new SBF (no reflash) — pass --mac so the cloud phase
# knows which device to assign to.
tools/bench_push.py --only=cloud \
    --mac AA:BB:CC:DD:EE:FF \
    --sbf-file sbf/stage2_patched.sbf

# Just reflash and rescrape MAC (no cloud, no provision).
tools/bench_push.py --no-cloud --no-provision --port /dev/cu.usbmodem1101

# See exactly what would happen, no side effects.
ADMIN_API_KEY=stub tools/bench_push.py --dry-run --all \
    --port /dev/cu.usbmodem1101 \
    --mac AA:BB:CC:DD:EE:FF
```

Phase semantics (each opt-out via `--no-X`, or pick one with `--only=X`):

- **build** — runs `firmware/build.sh` (which itself bundles the UI).
- **flash** — runs `firmware/flash.sh`, then tails the serial port to
  scrape the dongle's MAC from the boot log. Auto-detects the port
  if a single `/dev/cu.usbmodem*` (macOS) or `/dev/ttyUSB*` (Linux)
  is connected.
- **cloud** — pushes assets to `$CLOUD_URL`:
  - `POST /admin/devices` to enroll (409 → fall through to `GET` + reuse token).
  - `POST /admin/calibrations/<filename>` if `--sbf-file` given.
  - `PUT /admin/firmware/<git-short-hash>` if `firmware/build/futuner_v2.bin` exists.
  - `POST /admin/devices/<mac>/license` to set `paid` flag.
- **provision** — WS commands to the dongle at `--dongle-host`:
  - `set_auth_token` (with operator confirmation unless `--yes`).
  - Optional `wifi_connect` if `--ssid` given.
  - `vin_pair_now`.

### Safety knobs

- `--allow-dirty` — by default, refuses to flash on a dirty git tree.
  Bypassing this loses the binding between "what's on the dongle" and
  "what's in git", which is painful to recover six months later.
- `--i-know-what-im-doing` — by default, refuses to provision a dongle
  that wasn't just flashed (its state is unknown). Override at your own
  risk (different VIN cached / different token in NVS / different fw).
- `--yes` — skip operator confirmations before irreversible WS steps
  (`set_auth_token`, `wifi_connect`).
- `--dry-run` — print the entire plan with redacted secrets; zero
  side effects. Use to sanity-check the invocation before running for real.

### Required env

| Var | Purpose | Default |
|---|---|---|
| `ADMIN_API_KEY` | x-admin-key header for cloud admin endpoints. | (no default — required for `--cloud`/`--provision`) |
| `CLOUD_URL` | Cloud base URL. | `https://sillyrabbitmotorsport.com/fut` |

The script never logs the raw key — `--dry-run` shows it as `****-key`,
the appended progress-log block redacts it the same way.

## Dependencies

- Python 3.9+ (uses `argparse.BooleanOptionalAction`).
- `pyserial` (only for the `--flash` boot-log MAC scrape).
- `websockets` (only for the `--provision` phase).
- `pytest` and `pyflakes` for the test layer (dev-time only).

```sh
python3 -m pip install --user pyserial websockets pytest pyflakes
```

`bundle_ui.py` and `sbf_to_json.py` are pure stdlib. `can_sniff.py`
needs `pyusb` + a CandleLight-class adapter — see its docstring.

## Tests

```sh
cd ~/esp/obd/FUTV1.1
python3 -m pytest -x tools/test_bench_push.py
```

The test layer is light by design — pytest with mocks for
`subprocess.run`, the urllib opener, the WS runner, and the serial
reader. Real subprocess invocation requires an actual dongle and an
actual cloud, which is bench-day work.
