# SRM Cloud — `sillyrabbitmotorsport.com`

Server-side replacement for `api.dynoscorpion.com`. Implements the same
three endpoints the v1.0 SEFI dongle expects, plus an admin surface for
managing devices, firmware builds, and calibration files.

**Status:** spec-complete reference implementation. Single-file FastAPI
app + SQLite + Caddy for TLS. ~400 lines, no DB to set up, no message
broker, no microservices. Deploy on any VPS for $5/mo.

---

## What it does

### Dongle-facing protocol (Bearer-auth gated)

| Method | Path | Body / query | Returns |
|---|---|---|---|
| `POST` | `/api/v1/device/register` | `{mac, vin, boxcode, build_hash, …}` | `{ok, server_time}` |
| `GET`  | `/api/v1/device/update_available` | `?current_hash=…` | `{available: 0\|1, build_hash, url, sha256, …}` |
| `GET`  | `/api/v1/device/download_update/{filename}` | — | firmware binary stream |
| `GET`  | `/api/v1/device/calibration` | — | the device's assigned SBF, **patched server-side with the per-VIN ethanol_random** |

### Admin surface (gated by `X-Admin-Key: <ADMIN_API_KEY>`)

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/admin/devices` | enroll a dongle; returns the Bearer token to flash into NVS |
| `GET`  | `/admin/devices` | list all devices |
| `PUT`  | `/admin/firmware/{build_hash}` | upload a firmware binary |
| `POST` | `/admin/devices/{mac}/assign_firmware?build_hash=…` | push a build to a device |
| `POST` | `/admin/calibrations/{filename}` | upload a base SBF |
| `POST` | `/admin/devices/{mac}/assign_calibration?filename=…` | assign tune to device |
| `GET`  | `/admin/log/{mac}` | per-device event log |

---

## Why this design

Sean's `api.dynoscorpion.com` did exactly these things — register,
update, download, and serve per-VIN-locked SBFs. Replicating it
end-to-end means existing v1.0 dongles can talk to your server with
either a firmware string-patch (replacing the URL) or a DNS hijack
(pointing the original URL at your IP), with no protocol changes.

The **per-VIN SBF patcher** (`patch_sbf_for_device`) is the central
trick: the base SBF on disk has a placeholder `ethanol_random` value;
when a device requests it, the server stamps that device's specific
`ethanol_random` (stored in the `devices` table) into the file before
streaming it out. The dongle's ethanol-validation step then succeeds
only on the ECU we provisioned for. This is the IP-protection layer
Sean had — recreated in clear, auditable code.

---

## Deploy

### 1. Pick a host

A $5 VPS works (DigitalOcean, Hetzner, Linode, etc.). You need:

- Public IPv4
- Ports 80 + 443 open
- Some way to ssh in and install Caddy + Docker

### 2. DNS

Add an A record:

```
sillyrabbitmotorsport.com   →   <your VPS public IP>
```

### 3. Run with Docker (recommended)

```bash
# clone or scp this folder to the VPS
cd ~/srm-cloud

# Set the admin key (used for ALL /admin/* endpoints)
echo "ADMIN_API_KEY=$(openssl rand -hex 32)" > .env

# Build + start
docker compose up -d --build

# Server is now on http://127.0.0.1:8000 (localhost-only by default)
curl http://127.0.0.1:8000/health
```

### 4. Front it with Caddy for HTTPS

```bash
sudo apt install caddy
sudo cp Caddyfile /etc/caddy/Caddyfile
sudo systemctl reload caddy
```

Caddy auto-provisions a Let's Encrypt cert. Within ~30 seconds:

```bash
curl https://sillyrabbitmotorsport.com/fut/health
# {"ok": true, "ts": 1746384000}
```

### 5. Run without Docker (dev mode)

```bash
pip install -r requirements.txt
export ADMIN_API_KEY=$(openssl rand -hex 32)
uvicorn src.main:app --host 0.0.0.0 --port 8000 --reload
```

---

## Bootstrap a device end-to-end

```bash
ADMIN_KEY="<your ADMIN_API_KEY>"
BASE=https://sillyrabbitmotorsport.com/fut

# 1. Enroll the dongle. Save the auth_token from the response.
curl -X POST "$BASE/admin/devices" \
  -H "X-Admin-Key: $ADMIN_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "mac": "30:ed:a0:b6:35:40",
    "vin": "WUAPCBF28NN902533",
    "boxcode": "4K0907557G__0003",
    "owner_name": "Chris",
    "owner_email": "chris@sillyrabbitmotorsport.com",
    "ethanol_random": 687
  }'
# → {"ok":true,"mac":"30:ed:a0:b6:35:40","auth_token":"a1b2c3..."}

# 2. Upload a firmware build. Use the git short-hash.
curl -X PUT "$BASE/admin/firmware/30b57c4?build_number=1&is_release=1" \
  -H "X-Admin-Key: $ADMIN_KEY" \
  -F "file=@/path/to/firmware_full_16MB.bin"

# 3. Assign that build to the device.
curl -X POST "$BASE/admin/devices/30:ed:a0:b6:35:40/assign_firmware?build_hash=30b57c4" \
  -H "X-Admin-Key: $ADMIN_KEY"

# 4. Upload a base SBF (pre-patcher).
curl -X POST "$BASE/admin/calibrations/stage1.sbf?boxcode=4K0907557G__0003" \
  -H "X-Admin-Key: $ADMIN_KEY" \
  -F "file=@/path/to/stage1.sbf"

# 5. Assign it.
curl -X POST "$BASE/admin/devices/30:ed:a0:b6:35:40/assign_calibration?filename=stage1.sbf" \
  -H "X-Admin-Key: $ADMIN_KEY"

# 6. Now the device pulls the patched SBF on demand:
curl "$BASE/api/v1/device/calibration" \
  -H "Authorization: Bearer a1b2c3..." \
  --output stage1_for_my_ecu.sbf
```

---

## Test the implementation

```bash
pip install pytest
PYTHONPATH=. pytest tests/ -v
```

Includes auth tests, firmware upload/assign/download flow, and a SBF
patcher round-trip that asserts the per-device `ethanol_random` ends up
in the right header offset.

---

## How the dongle reaches this server

Three options for getting the v1.0 dongle to call your server instead
of Sean's:

### A. DNS hijack (zero firmware change)

On the network the dongle joins (STA WiFi), make
`api.dynoscorpion.com` resolve to your server's IP.

- **Router-level**: most routers let you add a DNS override.
- **Pi-hole / AdGuard Home**: add `api.dynoscorpion.com → your-IP` as
  a custom DNS rule.
- The dongle's TLS validation will fail unless your cert covers
  `api.dynoscorpion.com` too. You can't get a Let's Encrypt cert
  for a domain you don't own — so this only works if the dongle
  doesn't validate the server cert (Sean may or may not have).

### B. Firmware string-patch (single-file change, no DNS needed)

Replace the URL bytes in the v1.0 firmware binary. Constraint: the new
hostname must fit in 20 chars (the length of `api.dynoscorpion.com`).

Pick a short SRM domain, e.g. `api.srmotorsport.com` (exactly 20 chars).
Then run the patch script (we'll write it next):

```bash
./patch_firmware_url.py firmware/firmware_full_16MB.bin --new api.srmotorsport.com
```

This rewrites the URL bytes in-place across all 6 occurrences (3 in
app0, 3 in app1) without changing any file lengths or pointers.

### C. New firmware build (FUTUNER v2)

The forthcoming FUTUNER replacement firmware will hard-code SRM URLs
from the start. Until then, A or B.

**Recommended path for now: B.** Register `srmotorsport.com` (~$10/yr),
point `api.srmotorsport.com` at your server, patch the firmware,
flash, done.

---

## What's NOT in this server (yet)

- A web admin UI (everything is curl/script for now)
- Email notifications
- Multi-tenant dealers (the schema has the column but no enforcement)
- A patch builder pipeline (you bring the SBFs already compiled)
- TLS pinning support (would require the dongle's TLS to support pinning)

These are intentional cuts to keep the first version ~400 LOC and easy
to audit. Add them when you actually need them.

---

## Files

```
cloud_server/
├── README.md              ← this file
├── requirements.txt       ← Python deps (fastapi, uvicorn, multipart)
├── .env.example           ← env vars template
├── Dockerfile             ← container build
├── docker-compose.yml     ← single-container compose
├── Caddyfile              ← reverse proxy + Let's Encrypt
├── src/
│   └── main.py            ← the entire server, ~400 lines
├── tests/
│   └── test_smoke.py      ← pytest, exercises auth + firmware + cal flows
├── data/                  ← SQLite DB lives here (gitignore in real deploy)
├── firmware/              ← uploaded firmware binaries
└── calibrations/          ← uploaded base SBFs
```
