# Handoffs

> **As of 2026-05-19, all work is Mac-only.** PC handoff docs are preserved in [`archive/`](archive/) for history. Cross-machine coordination sections in this README are obsolete.

This directory holds paste-ready handoff documents — self-contained prompts and setup docs designed to bootstrap a fresh Claude Code session for a specific task without that session needing prior context.

Because they're in the git-tracked project (not workspace-root), they travel with `git push` / `git pull`.

---

## Active

| File | Purpose | Use when |
|---|---|---|
| `PHASE1_HIL_VALIDATION.md` | Paste-ready prompt for executing Phase 1 hardware-in-loop validation against a real vehicle. Self-contained — includes the full validation procedure with three-stream monitoring (WS + Candlelight + UI). | When running Phase 1 HIL validation against the dev RS7 or another paired vehicle, on whatever machine is wired to the car |

---

## How to use

### Running HIL validation against a paired vehicle

```bash
# On the machine wired to the dongle + car
cat handoffs/PHASE1_HIL_VALIDATION.md   # read first
# Then paste contents into Claude Code as the working prompt
```

### Adding a new handoff doc

When a new task needs its own paste-ready prompt (e.g., a future Phase 2 HIL flash), add it here. Naming convention:

- Migration / one-time setup docs: `HANDOFF_<TOPIC>.md`
- Task-specific paste-ready prompts: `<TASK>_<MODIFIER>.md` (e.g., `PHASE1_HIL_VALIDATION.md`)

Update the Active table when a new doc is added.

---

## Discipline

- **Status logs do NOT live here.** Per `~/esp/obd/CLAUDE.md`, daily `status-YYYY-MM-DD.md` and `file-update-YYYY-MM-DD.md` files live at workspace root (`~/esp/obd/`), not inside `FUTV1.1/`. Those files are append-only.
- **No secrets here.** Handoff docs reference VINs, MAC addresses, and dev car identifiers — those are operationally necessary and acceptable in the private repo. AES keys, admin passwords, raw bin bytes, and customer data stay in `secrets/` (gitignored) or never enter the repo at all.

---

## Archive (historical)

| File | Purpose | Status |
|---|---|---|
| [`archive/HANDOFF_TO_PC.md`](archive/HANDOFF_TO_PC.md) | Comprehensive Mac → PC migration doc from May 12-17. Covered project state, hard rules, workspace layout, hardware, toolchain install, key constants. | Obsolete 2026-05-19 — PC out of the picture |
| [`archive/PC_PHASE1_HANDOFF.md`](archive/PC_PHASE1_HANDOFF.md) | Standing authorization for the PC machine. Defined Phase 1 ownership scope, in/out lists, coordination discipline with Mac. | Obsolete 2026-05-19 — PC out of the picture |

(Originally these and `PHASE1_HIL_VALIDATION.md` lived at workspace root as `~/esp/obd/HANDOFF_TO_PC.md`, `~/esp/obd/PC_PHASE1_HANDOFF.md`, `~/esp/obd/rabbit.md`. Moved into the repo on 2026-05-18; the two PC docs archived on 2026-05-19.)

---

## See also

- `~/esp/obd/CLAUDE.md` — workspace router
- `~/esp/obd/FUTV1.1/CLAUDE.md` — project hard rules
- `~/esp/obd/FUTV1.1/docs/MISSION_SPEC.md` — Phase 1 + Phase 2 product spec
- `~/esp/obd/FUTV1.1/docs/PHASE_2_PREREQUISITES.md` — open P-items
- `~/esp/obd/FUTV1.1/hw_reference/` — reverse-engineering notes, key recovery, MM analysis
