# Handoffs

This directory holds **cross-machine handoff documents** for the FUTUNER project. These are paste-ready prompts and setup docs designed to bootstrap a fresh Claude Code session on a different machine — without that machine needing prior session context.

Because they're in the git-tracked project (not workspace-root), they travel with `git push` / `git pull` between machines. That's the canonical transfer mechanism now that the Mac and PC are split across Phase 1 / Phase 2.

---

## What lives here

| File | Purpose | Use when |
|---|---|---|
| `HANDOFF_TO_PC.md` | Comprehensive Mac → PC migration doc. Covers entire project state (Phase 1 + Phase 2), all hard rules, workspace layout, hardware, toolchain install, key constants. **Read this first on any new machine.** | First-time bootstrap on a new dev machine |
| `PC_PHASE1_HANDOFF.md` | Standing authorization for the PC machine. Defines Phase 1 ownership scope, what's in/out, coordination discipline with Mac, work prioritization, what-not-to-do. | Every PC working session |
| `PHASE1_HIL_VALIDATION.md` | Paste-ready prompt for executing Phase 1 hardware-in-loop validation against a real vehicle. Self-contained — includes the full validation procedure with three-stream monitoring (WS + Candlelight + UI). | When running Phase 1 HIL validation against the dev RS7 or another paired vehicle |

(Previously these lived at workspace root as `~/esp/obd/HANDOFF_TO_PC.md`, `~/esp/obd/PC_PHASE1_HANDOFF.md`, `~/esp/obd/rabbit.md`. Moved here on 2026-05-18 so they ship with the git repo.)

---

## How to use

### Receiving a new machine (e.g., bootstrapping rabbit@ on the PC)

```bash
# On the new machine
cd ~/esp/obd/FUTV1.1
git pull origin main
cat handoffs/HANDOFF_TO_PC.md   # read end-to-end before anything else
cat handoffs/PC_PHASE1_HANDOFF.md  # standing scope authorization
```

Then paste `PC_PHASE1_HANDOFF.md` contents into a fresh Claude Code session as the session-opener prompt.

### Running HIL validation against a paired vehicle

```bash
# On the machine wired to the dongle + car
cat handoffs/PHASE1_HIL_VALIDATION.md   # read first
# Then paste contents into Claude Code as the working prompt
```

### Adding a new handoff doc

When a new cross-machine task needs its own handoff (e.g., a future Phase 2 → Phase 3 migration), add it here. Naming convention:

- Migration / one-time setup docs: `HANDOFF_<TOPIC>.md`
- Standing scope authorizations: `<MACHINE>_<SCOPE>_HANDOFF.md` (e.g., `PC_PHASE1_HANDOFF.md`)
- Task-specific paste-ready prompts: `<TASK>_<MODIFIER>.md` (e.g., `PHASE1_HIL_VALIDATION.md`)

Update this README's table when a new doc is added.

---

## Discipline

- These docs are **the contract** between Mac and PC. If the scope split changes (Mac picks up a Phase 1 surface, or PC takes a Phase 2 deliverable), update `PC_PHASE1_HANDOFF.md`'s in/out lists first, push, then start the new work.
- **Status logs do NOT live here.** Per `~/esp/obd/CLAUDE.md`, daily `status-YYYY-MM-DD.md` and `file-update-YYYY-MM-DD.md` files live at workspace root (`~/esp/obd/`), not inside `FUTV1.1/`. Those files are append-only and tagged `[MAC]` or `[PC]` per machine.
- **No secrets here.** Handoff docs reference VINs, MAC addresses, and dev car identifiers — those are operationally necessary and acceptable in the private repo. AES keys, admin passwords, raw bin bytes, and customer data stay in `secrets/` (gitignored) or never enter the repo at all.
- **Cross-machine coordination via tags.** When a PC working session surfaces something Mac needs to address (e.g., a Phase 2 bug noticed during Phase 1 work), the PC writes it to its `status-YYYY-MM-DD.md` entry as `## [PC->MAC] <topic>` and Mac picks it up next session.

---

## See also

- `~/esp/obd/CLAUDE.md` — workspace router (Mac/PC layout, ownership split intro)
- `~/esp/obd/FUTV1.1/CLAUDE.md` — project hard rules
- `~/esp/obd/FUTV1.1/docs/MISSION_SPEC.md` — Phase 1 + Phase 2 product spec
- `~/esp/obd/FUTV1.1/docs/PHASE_2_PREREQUISITES.md` — open P-items
- `~/esp/obd/FUTV1.1/hw_reference/` — reverse-engineering notes, key recovery, MM analysis
