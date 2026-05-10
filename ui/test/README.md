# UI eval — host-side harness

Three-layer eval for the FUTUNER v2 single-page UI. Runs without the
real ESP32-S3 in the loop; the dongle is replaced by a fixture-driven
WebSocket + admin-HTTP shim (`mock_dongle.py`).

## What's here

| File | Role |
|---|---|
| `mock_dongle.py` | Asyncio WS server (port 47821) + admin HTTP server (port 47822). Reads `mock_dongle_responses.json` and serves canned responses; admin POST `/admin/fire` replays scripted event sequences. |
| `mock_dongle_responses.json` | Fixture: per-command canned responses + named event sequences. The eval verifies every COMMAND_REGISTRY entry has a fixture key (modulo `_eval_command_exemptions`) and every required sequence is present. |
| `test_round_trip.py` | Client. Runs all 9 required test scenarios, prints `RESULT: PASS` on full pass. |
| `README.md` | This file. |

## Dependencies

- Python 3.10+ (asyncio + type hints).
- One pip install: `websockets` (≥ 11). On Sean's macOS dev box this is
  already installed system-wide.

```sh
python3 -m pip install --user websockets
```

stdlib used: `asyncio`, `json`, `urllib.request`, `re`, `argparse`,
`pathlib`, `logging`. No other third-party deps.

## Running the eval

The full eval runs from one entry point — `firmware/test/ui/eval.sh`:

```sh
cd ~/esp/obd/FUTV1.1
firmware/test/ui/eval.sh
```

That harness:
1. **Layer A** — static checks (panel IDs, CSS tokens, command/event coverage).
2. **Layer B** — `node --check` on the JS, `python3 -m py_compile` on
   the Python sources.
3. **Layer C** — spawns `mock_dongle.py` on ports 47821/47822, runs
   `test_round_trip.py` against it, greps the output for
   `RESULT: PASS`, and tears the mock down.

You can run pieces by hand for debugging:

```sh
# Start the mock in one terminal.
python3 ui/test/mock_dongle.py --log-level DEBUG

# In another:
python3 ui/test/test_round_trip.py --ws ws://127.0.0.1:47821/ \
    --admin http://127.0.0.1:47822/

# Fire a sequence manually:
curl -X POST http://127.0.0.1:47822/admin/fire \
    -H 'Content-Type: application/json' \
    -d '{"sequence":"apply_progress_3_then_complete"}'

# Set a per-command override (returns to default by passing null):
curl -X POST http://127.0.0.1:47822/admin/override \
    -H 'Content-Type: application/json' \
    -d '{"command":"live_tune_start","override_key":"live_tune_start_unpaid"}'
```

## What the mock is NOT

- Not a firmware substitute. It serves canned responses, not real ECU
  behaviour.
- Not a load tester. The admin port has a 64 KB body cap and no
  rate limiting beyond what asyncio gives you.
- Not a security tool. Bound to 127.0.0.1 by default; do not expose
  to a network.

Real on-car testing remains manual, per-feature.
