# Hermes audit — BLOCKED (endpoint down)

## Block (2026-05-22)

Hermes endpoint at `http://192.168.1.180:3000` is unreachable from
this Mac. Both `/api/tags` and `/v1/models` probes time out after
10 seconds. Two attempts during the overnight session, ~3 hours
apart, same result.

## Why I'm not workaround-ing

The overnight dispatch was explicit about which endpoint to use
(192.168.1.180:3000, model `nemo180:latest`). Pinging a different
Hermes instance would change the audit corpus and the model
version — defeats the point of an audit baseline.

Also: even if I had a reachable Hermes endpoint, the audit
would commit a `docs/HERMES_AUDIT_2026-05-21_OVERNIGHT.md` based
on its findings. Without a verified endpoint, I'd be running
findings that don't represent the canonical Hermes corpus, which
risks committing a misleading audit doc to origin/main.

## What was supposed to happen

Per dispatch:

> Dispatch Hermes (Nemotron-120B at 192.168.1.180:3000, model
> nemo180:latest) to audit the 10 commits Sean just pushed [...]
> Focus areas:
>   - Bug fix completeness
>   - Regression risk
>   - Magic-number rule compliance
>   - Frozen-module rule compliance
>   - Rule 9 (WS command name match) compliance for the new cmd_reboot
>   - cloud_client factory + battery_voltage if they landed during
>     this overnight run

Plus output at `docs/HERMES_AUDIT_2026-05-21_OVERNIGHT.md`.

## Suggested resolution

Sean restarts the Hermes service (or ssh tunnels through it) and
re-fires the audit prompt manually. The full prompt would target
the range `26beff2..HEAD` (currently 6dd6e01), covering:

- `4253304` Cloud HTTPS URL + TLS bundle
- `2e28b5f` P-44 ws_server STA rebind
- `a54d690` P-53 dtc_clear demux
- `39e3b1e` Phase 1 scope reduction
- `3c0aef7` P-57 UI wsSend
- `a9c0b5f` P-58 vin_pair persistence
- `4788876` P-28 WOT init order
- `fed30f1` P-55 diag surface
- `92be4f1` P-59 UI rename + cmd_reboot
- `4243982` A14 P-43..P-61
- `4794174` B1 BLOCKED
- `2959ec2` A4 P-29..P-32 OBSOLETE
- `5f62cea` P-33 bash 3.2
- `91d0f26` P-42 verified clean
- `ec7d044` P-43 cloud endpoint (PENDING-DEPLOY)
- `c60126a` P-48 api.* sweep
- `aaa8ea8` P-49 cloud_client factory
- `1a18fa3` P-50 cloud_client regression gate
- `6000fd1` A11 SKIPPED
- `79b8f77` P-56 HIL doc → can_tail.py
- `2653644` A2 A2L audit
- `6dd6e01` B5 Ethernet skeleton scaffold

If Hermes is still hard-down at morning review, an alternate audit
path is the human-eyes pass + the existing host eval gates
(orchestrator/cloud_client/etc.).
