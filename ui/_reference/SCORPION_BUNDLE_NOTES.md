# ScorpionEFI Embedded — Recovered Web UI

> **Origin:** Extracted from a working dongle's flash dump
> (`/Users/rabbit/esp/obd/SEFIv1/HW/working_dongle_full_dump.bin`, dated
> 2026-04-13). The UI source itself was never delivered with the project — this
> is a recovery of the *production bundle* from the dongle's flash filesystem.

## What this is

A single-page React application built with Vite, originally served by an
ESP32-S3 dongle's web server. App title: **"ScorpionEFI Embedded"** — the UI
the ex-employee shipped under the Scorpion / Dyno Scorpion brand. Built using
[Lovable.dev](https://lovable.dev) (visible from the `@lovable_dev` Twitter
meta tag in `index.html`).

This folder is the **canonical secured copy**. Don't edit files here directly
— treat it as a frozen artifact. To customize, copy out into a new project.

---

## Folder layout

```
UI_RECOVERED/
├── README.md              ← this file
├── MANIFEST.txt           ← sha256 + sizes of every artifact (integrity check)
│
├── dist/                  ← decompressed production files (browser-ready)
│   ├── index.html         ← entry point, points at /assets/*
│   ├── logo.png
│   ├── favicon.ico
│   ├── placeholder.svg
│   ├── robots.txt
│   └── assets/
│       ├── index.B-h9rcZL.css   (70 KB compiled Tailwind)
│       └── index.D_CrOv9L.js    (561 KB minified React bundle)
│
├── dist_gz/               ← gzipped originals as flashed to data0 partition
│   └── ...                  (use these for embedded LittleFS/SPIFFS)
│
├── source_dump/
│   └── data0_partition.bin  ← the raw 352KB LittleFS partition (the source
│                              of truth — extract again if dist/dist_gz lost)
│
├── analysis/
│   ├── routes.txt           ← React Router routes (pages in the SPA)
│   ├── api_endpoints.txt    ← every /api/* the bundle calls
│   ├── metadata.txt         ← title, author, framework, build hashes
│   └── deobfuscation_notes.md ← how to recover readable React source
│
├── serve/
│   ├── serve_static.py      ← static-only HTTP server (port 8766)
│   ├── serve_proxy.py       ← static + /api proxy to a live dongle (port 8766)
│   └── README.md            ← when to use which
│
└── flash/
    ├── make_littlefs_image.py  ← rebuild the data0 partition from dist_gz/
    ├── flash_to_dongle.sh      ← write rebuilt image to the dongle's flash
    └── README.md               ← step-by-step flashing guide
```

---

## How to use this — three scenarios

### 1. Just want to see what the UI looks like
```bash
cd serve && python3 serve_static.py
open http://localhost:8766/
```
Opens the UI but no API calls work (no backend). Good for browsing layout.

### 2. Want to drive a live dongle from this UI
```bash
# Mac joins WiFi 'SEFI_e0d886' / 'scorpion' first
cd serve && python3 serve_proxy.py
open http://localhost:8766/
```
Static UI served locally, all `/api/*` requests are proxied to
`http://192.168.10.1` (your dongle's AP IP).

### 3. Want to flash this UI back onto a dongle's data0 partition
```bash
cd flash && python3 make_littlefs_image.py    # builds data0.bin
./flash_to_dongle.sh                          # writes it to /dev/cu.usbmodem*
```
After flash, the dongle serves the UI directly at `http://192.168.10.1/`.

---

## Source code recovery options

This folder contains the **compiled bundle**, not the React source. The
sourcemap (`.map` file) was deliberately stripped before delivery. To get
back to readable React component source:

| Tool | Command | Output quality |
|------|---------|----------------|
| `webcrack` | `npx webcrack dist/assets/index.D_CrOv9L.js -o source/` | Decent — restores module boundaries, names are mangled |
| `humanify` | `npx humanify dist/assets/index.D_CrOv9L.js` | Better — uses LLM to rename variables; takes minutes |
| Manual reading | open in editor | Lots of effort; routes/endpoints already extracted in `analysis/` |
| Re-create in Lovable | upload screenshots, re-prompt | ~1-2 hours; you own the source |

See `analysis/deobfuscation_notes.md` for step-by-step.

---

## Provenance & integrity

- **Original source:** ESP32-S3 dongle MAC `30:ed:a0:b6:35:40`,
  `data0` partition (offset 0xF50000, size 0x58000) of
  `working_dongle_full_dump.bin`
- **Filesystem:** LittleFS, block size 4096, 88 blocks
- **Recovered:** 2026-05-03
- **Integrity:** see `MANIFEST.txt` for sha256 of every file. Re-run
  `python3 verify_manifest.py` (in this dir) to detect tampering.

If you suspect any file in `dist/` or `dist_gz/` has been altered, you can
always rebuild from `source_dump/data0_partition.bin`:
```bash
python3 -c "
from littlefs import LittleFS
d = open('source_dump/data0_partition.bin','rb').read()
fs = LittleFS(block_size=4096, block_count=len(d)//4096, mount=False)
fs.context.buffer = bytearray(d); fs.mount()
# walk + extract — see flash/make_littlefs_image.py for the inverse
"
```

---

## Notes for the FUTUNER project

The user's intent is to base the **FUTUNER v2 firmware's** web UI on this
recovered ScorpionEFI bundle. Two reasonable strategies:

1. **Drop-in reuse** — flash `dist_gz/` into FUTUNER's `data` partition.
   Works for any endpoint FUTUNER firmware also implements. Endpoints that
   FUTUNER lacks will 404 silently in the UI (button does nothing).

2. **Decompile + port** — run `humanify` on the JS bundle, port the
   recovered React source into a fresh Vite project under FUTUNER's
   repo. More work, but fully owned source.

The endpoint list this UI calls is in `analysis/api_endpoints.txt` — use it
as a checklist for FUTUNER's REST surface.
