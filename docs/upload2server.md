# upload2server.md — pre-flash server readiness checklist

> **What this is.** Everything you need to push to your cloud server
> (`api.sillyrabbitmotorsport.com`) before flashing a dongle, in the
> order it has to happen. Paste-able commands. Assumes you have shell
> access to the box running Caddy + Docker and the `ADMIN_API_KEY` set
> in your shell.
>
> **What this is not.** A first-time deployment guide for the cloud
> box itself — for that, see `cloud/README.md` (Caddy install, A
> record, Docker install, initial certificate). This doc assumes the
> cloud is already up and reachable at
> `https://api.sillyrabbitmotorsport.com/health`.

---

## Pre-flight assumptions

- DNS: `api.sillyrabbitmotorsport.com` resolves to your box.
- Caddy is reading `cloud/Caddyfile` and reverse-proxying to
  `127.0.0.1:8000`.
- Docker + docker-compose installed on the box.
- You have the `ADMIN_API_KEY` you set during deployment exported in
  your local shell:

  ```bash
  export ADMIN_API_KEY='paste-your-long-random-string-here'
  export CLOUD='https://api.sillyrabbitmotorsport.com'
  ```

- The dongle's MAC is known (read off the chip or from the boot serial
  log). Save it — every per-device admin call below takes the MAC.

  ```bash
  export DONGLE_MAC='AA:BB:CC:DD:EE:FF'   # replace with real MAC
  ```

- The dev car's VIN is known. The dongle reads it from the ECU on
  first pair, so you don't need to set it manually — but you'll see
  it in admin listings.

---

## 1 — Push the cloud code

The server source lives at `FUTV1.1/cloud/`. Volumes
(`./data`, `./firmware`, `./calibrations`) hold persistent state and
must be preserved across deploys. The container itself is rebuilt.

On your laptop:

```bash
cd ~/esp/obd/FUTV1.1/cloud
rsync -av --exclude='__pycache__' --exclude='.pytest_cache' \
  ./ user@your-server:/opt/srm-cloud/
```

On the server:

```bash
cd /opt/srm-cloud
docker-compose down
docker-compose build --no-cache
docker-compose up -d
docker-compose logs -f srm-cloud   # watch boot, ctrl-C when ready
```

Verify:

```bash
curl -fsS $CLOUD/health
# expect: {"ok":true,"version":"...","time":...}
```

**What's in the bundle:** `src/main.py` (the FastAPI app, ~640 lines,
all endpoints), `requirements.txt` (FastAPI, uvicorn, pydantic,
python-multipart), `Dockerfile`, `docker-compose.yml`, `Caddyfile`
(reference only — the live one is at `/etc/caddy/Caddyfile`).

**What's NOT in the bundle (intentionally):** `data/` (SQLite DB),
`firmware/` (uploaded firmware binaries), `calibrations/` (uploaded
SBFs). These are server-resident state — never overwrite from your
laptop. Docker mounts them as volumes; if you redeploy, they
survive.

---

## 2 — Confirm endpoints are live

Quick smoke test — hits both the public device-side and admin-side:

```bash
# Device-side endpoints (these are the ones the dongle calls)
curl -fsS $CLOUD/                                 # banner JSON
curl -fsS $CLOUD/health                           # health probe

# Admin endpoints — list enrolled devices (will be [] on first deploy)
curl -fsS -H "x-admin-key: $ADMIN_API_KEY" $CLOUD/admin/devices
```

Expected device-facing endpoints currently live:

| Method | Path | Used by |
|--------|------|---------|
| POST   | `/api/v1/device/register`            | Dongle phones home on every boot (Prompt 4) |
| GET    | `/api/v1/license`                    | License cache fetch (Prompt 4) |
| GET    | `/api/v1/device/update_available`    | OTA check (existing) |
| GET    | `/api/v1/device/download_update/...` | OTA download (existing) |
| GET    | `/api/v1/device/calibration`         | SBF download (Prompt 5) |

**Pending — NOT yet implemented in `cloud/src/main.py`:**

| Method | Path | Used by |
|--------|------|---------|
| POST   | `/api/v1/telemetry/log` | WOT log upload (Prompt 2) — endpoint not present; uploads will retain on the dongle until this lands |

This is the documented carryover from the 2026-05-05 session. WOT
recording, gzipping, and queueing all work on the dongle; only the
upload completes (the dongle sees a connection error or 404, retains
the file, retries on next interval). Sean to land this endpoint as a
small follow-up before WOT-on-car validation gets full coverage.

---

## 3 — Enroll the dongle (one-time per dongle)

Creates the `devices` row and returns the Bearer token. Save the
token — you'll feed it back into the dongle over WS as the next step.

```bash
curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
     -H "Content-Type: application/json" \
     -d "{\"mac\":\"$DONGLE_MAC\"}" \
     $CLOUD/admin/devices
```

Response shape:

```json
{ "ok": true, "mac": "AA:BB:CC:DD:EE:FF", "auth_token": "32-char-hex-string" }
```

Save the `auth_token`:

```bash
export DEVICE_TOKEN='32-char-hex-string-from-response'
```

Optional fields (POST body) you can include now or set later:
`vin`, `boxcode`, `owner_name`, `owner_email`, `dealer_id`,
`ethanol_random`. None are required for first boot — the dongle
populates `vin` and `boxcode` on first `register` call. `paid` and
`revoked` are set via the license endpoint (step 5), not enrollment.

**If you re-enroll the same MAC,** the server returns 409. Either
delete the old row (no admin endpoint exists today — you'd have to
sqlite3 the data file directly) or pick a different MAC.

---

## 4 — Install the auth token on the dongle

This is the only step that's NOT a curl. After flashing the dongle:

1. Connect to the dongle's AP-mode Wi-Fi and load the control panel.
2. Auth as admin in the header (existing `Unlock` flow).
3. Fire the install-token command via WS:

   ```json
   { "command": "set_auth_token", "params": { "token": "PASTE_DEVICE_TOKEN_HERE" } }
   ```

   Expected: `{"ok": true}`. Token persists in NVS — survives reboots.

4. Get the dongle onto STA Wi-Fi (your home or hotspot SSID + password)
   via the existing `wifi_connect` flow.
5. Trigger first pair-and-license:

   ```json
   { "command": "vin_pair_now" }
   ```

   This calls `POST /api/v1/device/register` (writes VIN + boxcode
   into the device row) AND `GET /api/v1/license` (caches license
   state in dongle NVS). Expected: `{"ok": true, "message": "VIN
   paired and license refreshed"}`.

After this, the device row has `vin`, `boxcode`, and `last_seen`
populated. Confirm:

```bash
curl -fsS -H "x-admin-key: $ADMIN_API_KEY" $CLOUD/admin/devices | \
  python3 -c "import sys, json; [print(d['mac'], d['vin'], d['boxcode'], d['last_seen']) for d in json.load(sys.stdin)]"
```

---

## 5 — Mark the device paid

VIN-lifetime license, single `paid` flag. Required for `wot_log_*`
upload and `live_tune_*` to pass the license gate.

```bash
curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
     -H "Content-Type: application/json" \
     -d '{"paid":1}' \
     $CLOUD/admin/devices/$DONGLE_MAC/license
```

Expected: `{"ok": true, "mac": "...", "paid": true, "revoked": false, ...}`.

Then refresh the dongle's cache so the gate flips on without waiting
for the next periodic license poll:

```json
{ "command": "vin_pair_now" }
```

To revoke later (chargeback, fraud, dev-only flag):

```bash
curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
     -H "Content-Type: application/json" \
     -d '{"revoked":1,"revoked_reason":"test"}' \
     $CLOUD/admin/devices/$DONGLE_MAC/license
```

---

## 6 — Upload an SBF (live tune file)

Required only if you're validating `live_tune_*`. Skip this section
if you're flashing for DTC-only or WOT-only validation.

The dev car's stage1 SBF lives on disk at:

```
~/esp/obd/FUTV1.1/sbf/stage1_patched.sbf
```

Upload it (the cloud validates the SCPN magic bytes on receipt):

```bash
curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
     -F "file=@$HOME/esp/obd/FUTV1.1/sbf/stage1_patched.sbf" \
     "$CLOUD/admin/calibrations/stage1_patched.sbf?boxcode=4K0907557G__0003"
```

Replace `4K0907557G__0003` with the actual boxcode for your dev car
if different — that's the dev RS7's value per `boxcode_database.md`.
Expected: `{"ok": true, "filename": "stage1_patched.sbf", "size_bytes": ..., "sha256": "..."}`.

Then assign it to the device:

```bash
curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
     "$CLOUD/admin/devices/$DONGLE_MAC/assign_calibration?filename=stage1_patched.sbf"
```

Expected: `{"ok": true, "mac": "...", "active_cal": "stage1_patched.sbf"}`.

The dongle's `live_tune_start` will now find a calibration to
download via `GET /api/v1/device/calibration`.

---

## 7 — Optional: upload a firmware build to the server

Only if you want OTA delivery of the next firmware build. Not
required for the initial flash (you flash via USB-serial). The OTA
path is for downstream updates without re-cabling the dongle.

Skip this whole section unless you specifically want OTA configured
today. The dongle won't poll `update_available` if the existing
boot logic isn't pointed at it; check `firmware/src/ota/` for the
poll-trigger before relying on this path.

```bash
# from the firmware build directory
GIT_SHORT=$(git -C ~/esp/obd/FUTV1.1 rev-parse --short HEAD)
curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
     -X PUT \
     -F "file=@build/futuner.bin" \
     "$CLOUD/admin/firmware/$GIT_SHORT?build_number=1&is_release=1"

curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
     "$CLOUD/admin/devices/$DONGLE_MAC/assign_firmware?build_hash=$GIT_SHORT"
```

---

## 8 — Verify the device is fully provisioned

```bash
curl -fsS -H "x-admin-key: $ADMIN_API_KEY" \
     $CLOUD/admin/devices | \
  python3 -m json.tool
```

For your dongle's row, you want to see (after steps 3–6):

| Field | Expected after provisioning |
|-------|----------------------------|
| `mac` | your dongle MAC |
| `auth_token` | non-null (set in step 3) |
| `vin` | populated by dongle (step 4) |
| `boxcode` | populated by dongle (step 4) |
| `paid` | `1` (step 5) |
| `revoked` | `0` |
| `active_cal` | `stage1_patched.sbf` (step 6) |
| `last_seen` | recent timestamp |
| `active_firmware` | optional (step 7) |

If any of those are wrong, the dongle won't pass the corresponding
license/calibration gate at runtime.

---

## 9 — On-car validation order

You're now provisioned. Once flashed, validate in this order
(cheapest first):

1. **DTC read/clear** — no cloud, no license, low risk.
   `dtc_read` → table populates from UDS 0x19. `dtc_clear` → UDS 0x14.
2. **VIN pair refresh** — confirms cloud round-trip works end-to-end.
   `vin_pair_now` → `register` + `license` 200s, dongle NVS reflects
   `paid:true`.
3. **WOT logger record** — confirms recorder + queue work.
   `wot_log_start` → drive a pull → `wot_log_stop`. File queues in
   storage; upload retains until `/api/v1/telemetry/log` lands.
4. **SBF live tune** — the headline path.
   `live_tune_start({stage:1, ethanol_pct:0})` → cloud fetch →
   apply. Expect `apply_completed` event with `elapsed_ms < 2000`.

Per the Prompt 5 Q3 decision, ethanol live updates have **no safety
rails yet** (Prompt 7 deferred — no hysteresis, no dwell, no WOT
lockout, no rev-limit window). Drive accordingly. Dev-car operator
discipline is the only safeguard until Prompt 7 ships.

---

## Summary — what to push, in order

1. Push `cloud/` source via rsync, rebuild Docker container.
2. Verify `/health` returns 200.
3. Enroll dongle MAC → save `auth_token`.
4. Install token on dongle via WS, trigger `vin_pair_now`.
5. Mark device `paid:1`, refresh dongle license cache.
6. (If validating live tune) Upload SBF, assign to device.
7. (Optional) Upload firmware build, assign to device.
8. Verify `/admin/devices` shows the device fully provisioned.
9. Flash + validate on car in the order above.

Pending follow-ups before this list is fully covered:

- `/api/v1/telemetry/log` endpoint on the cloud (WOT upload
  destination). Until it lands, WOT logs queue locally instead of
  completing the upload-and-delete cycle.
- Phase 2 prerequisites P-01..P-11 (full binary flash) — every item
  still 🔴, MagicMotorsport capture session is the unblocker. Phase 2
  is not in scope for this checklist.
