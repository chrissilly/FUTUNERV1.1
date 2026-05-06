# Claude Code — Prompt 9 (UI catchup sweep) — DRAFT SKELETON

> Status: skeleton, not yet a paste-ready prompt. Drafted at the end
> of the 2026-05-05 session before context ran out. The next chat
> should flesh this out into the same format as
> `docs/CLAUDE_CODE_PROMPT_5.md` before sending it to a Claude Code
> session.
>
> Read `docs/SESSION_HANDOFF.md` first. This skeleton assumes that
> context.

---

## Why this prompt exists

Prompts 1–5 shipped five features and zero UI updates. The dongle's
WebSocket command surface now includes ~20 commands across feature
manager, WOT logger, DTC, VIN pairing, license, and SBF live tune —
none of which are exposed in `firmware/futuner_control_panel.html`
or `ui/control_panel.html` as user-friendly controls. Sean uses raw
WS clients today; this is hostile dogfooding and impossible for
customers.

**Catchup sweep first, then UI-per-feature going forward.** Prompts
6, 7, 8 should each include their own UI surface alongside firmware
+ cloud changes (the established pattern proved out by Prompts 1–5
shipping in clean isolation).

---

## What's missing — surface area to land

For each Prompt 1–5 feature, the UI needs:

**Feature manager (Prompt 1) — minimal status bar.**
- Current active feature name (or "idle")
- Swap warnings when starting a new feature preempts the active one

**WOT logger (Prompt 2).**
- Start/stop WOT logging buttons (gated by license — show "Unpaid"
  state if license refuses)
- Queued-logs count + total bytes
- Most recent upload status (success/failed/pending)
- Manual "force upload now" button
- Live indicator when WOT trigger crosses threshold

**DTC read/clear (Prompt 3).**
- "Read DTCs" button (UDS 0x19)
- Table of DTCs with code, status flags (active/pending/confirmed),
  description from the seed table
- Per-row Clear button + bulk "Clear All" (UDS 0x14)
- Last-cleared timestamp display

**VIN pairing (Prompt 4).**
- Wi-Fi credentials entry (AP-mode captive form is bare-bones today)
- VIN display (current ECU vs cached license)
- License status panel: paid/unpaid, revoked + reason if any
- "Re-pair now" button for re-running the register + license fetch

**SBF live tune (Prompt 5) — the headline UI.**
- Stage selector (1, 2, 3 — gated to current cached SBF per Q5
  override; show "Stage N loaded" status)
- Ethanol % slider/input (0–100)
- "Apply" button → emits live_tune_set
- Progress bar fed by apply_started/apply_progress/apply_completed
  events; show last_apply_elapsed_ms after completion
- Current state: idle / loading / applying / applied
- Stop button → live_tune_stop
- Refusal banner if license_can_run_feature returns false

**Cross-cutting:**
- License/paid status indicator persistently visible (lock icon when
  unpaid; tooltip with revoke reason)
- Error toast for any WS command that returns success=false
- Feature swap confirmation modal ("Stop WOT logging to start live
  tune?")

---

## What to read before drafting the full prompt

The next chat should consume:
- `~/esp/obd/FUTV1.1/docs/SESSION_HANDOFF.md`
- `~/esp/obd/FUTV1.1/docs/MISSION_SPEC.md` §1.3 (UI architecture —
  WebSocket streaming, browser-only, no native app)
- `~/esp/obd/FUTV1.1/firmware/futuner_control_panel.html` (embedded
  β plan UI)
- `~/esp/obd/FUTV1.1/ui/control_panel.html` (active SPA, red/black
  theme)
- `~/esp/obd/FUTV1.1/firmware/src/commands/commands.c` (canonical WS
  command registry — the surface to wire UI buttons against)
- `~/esp/obd/FUTV1.1/firmware/src/commands/wot_log_commands.c`,
  `dtc_commands.c`, `vin_pair_commands.c`, `sbf_commands.c` —
  per-feature command details, response shapes
- The existing `_reference/` ScorpionEFI compiled bundle in `ui/` for
  visual styling reference

---

## Pre-decided choices the next chat should resolve before paste

These are open questions the agent will probably ask. Sean answers
them; the answers go into the full prompt:

1. **Single SPA or split between embedded + browser-served?** The
   embedded HTML lives in firmware partition; the SPA in `ui/` is
   served by something (cloud? local?). Need to confirm where the
   active UI gets served from in production.
2. **WebSocket reconnection / event-stream library.** Vanilla JS,
   or pull in a small lib? Today's HTML is vanilla; staying vanilla
   keeps the embedded-firmware-bundle small.
3. **State management.** With 5 features each having their own state,
   a tiny store pattern (~50 lines of JS) beats prop-drilling. Or
   keep it dumb and let each feature panel manage its own state
   independently.
4. **Eval shape for UI work.** Visual regression tests need a
   headless browser; that's a heavier dep than the existing host
   tests. Alternatives: WS-round-trip tests against a mock dongle,
   HTML lint, smoke tests for "does the page load and connect."
   Pick one.
5. **Mobile vs desktop.** Phone in the car is the realistic use case;
   desktop is for tuning sessions. Mobile-first responsive layout,
   or two layouts?
6. **Theme.** Red/black is established. Stick with it. Style tokens
   in CSS variables so future re-skin is easy.

---

## Acceptance criteria (placeholder — flesh out in full prompt)

- All 20+ WS commands exposed via clearly-labeled UI controls
- License status visible on every page
- Feature swap warnings fire correctly when user clicks a button for
  feature B while feature A is active
- New eval harness `firmware/test/ui/eval.sh` exits 0 (shape TBD per
  decision 4 above)
- All five prior eval gates still PASS (regression-clean)
- The embedded HTML still fits in the firmware partition (don't blow
  the binary size)

---

## Forbidden / out of scope

- No firmware feature changes. UI sweep ONLY consumes existing WS
  command surface; if a feature needs a new command for UI to work
  cleanly, that's a small follow-up to that feature's prompt, not
  this one.
- No frozen-module modifications. (Reflexive at this point but stating
  for safety.)
- No cloud server changes.
- No Phase 2 UI yet (Phase 2 itself isn't shipped).
- BLE ethanol pairing UI (Prompt 6 territory).
- Ethanol constraint visualizations (Prompt 7 territory).

---

## Closing instruction template

Once the full prompt is written:
- Same paste-ready format as `docs/CLAUDE_CODE_PROMPT_5.md`
- Branch convention: `feat/prompt-9-ui-catchup`
- After eval green, merge to main, then Prompts 6/7/8 each include
  their own UI surface as part of feature scope (no more standalone
  UI sweeps).
