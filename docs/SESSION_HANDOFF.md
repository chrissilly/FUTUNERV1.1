# Session Handoff — 2026-05-05

> **Read this first** if you're a fresh Claude continuing FUTUNER work.
> This is the "if you had to start over right now, what would you want?"
> answer. Five prompts have shipped clean. The patterns that got us
> here are documented. Honor them.
>
> **Owner:** Sean Cyr (chris@sillyrabbitmotorsport.com is the Cowork
> identity; Sean is the human you're working with). SRM = Silly Rabbit
> Motorsport.

---

## 1. Where we are

**Five prompts merged, all regression-clean. 353 substantive eval
checks across the tree, zero failures. Frozen modules (the on-car-
validated live-tune logic carried byte-perfect from FUTV1.0) still
match the baseline hash list.**

| Prompt | Module | Gates |
|---|---|---|
| 1 | Feature manager (state arbiter) | 35/35 PASS |
| 2 | WOT logger (gauge + log + upload) | 61/61 PASS |
| 3 | DTC read/clear | 68/68 PASS |
| 4 | VIN pairing + license module + cloud `/api/v1/license` | 94/94 PASS |
| 5 | SBF live tune orchestrator + blend math + license gates | 95/95 PASS |
| — | `verify_frozen.sh` (scal/bdef/ecu_write) | PASS, 6/6 baseline |

The dongle now: arbitrates features, logs WOT pulls and uploads them
under license gate, reads/clears DTCs, pairs VINs and caches lifetime
licenses, downloads SBFs from cloud and live-tunes the ECU using the
proven scal/bdef/ecu_write modules. Recording, tuning, fault clearing,
and licensing are all wired end-to-end on the feature_manager.

---

## 2. Architectural rules (binding — do not negotiate)

These are the non-negotiables that have governed every prompt. They
are why the work has stayed clean through five iterations.

**Frozen modules.** `firmware/src/scal/scal_file.{c,h}`,
`firmware/src/bdef/bdef_file.{c,h}`, and
`firmware/src/ecu_write/ecu_write.{c,h}` are byte-perfect from
FUTV1.0's on-car-validated baseline. They carry the live-tune RAM
update logic that has been proven on Sean's RS7. **Do not modify them
under any circumstances** — not whitespace, not comments, not
"modernization." New code calls their public `.h` API; their `.c`
files are not even read. The eval harness `firmware/test/verify_frozen.sh`
runs as part of every prompt's gate. Modifications fail the eval and
cannot ship. Approval ritual for genuine necessary changes lives in
`firmware/src/FROZEN_MODULES.md` — four steps including on-car
validation, never bypassed.

**ON/OFF discipline.** Every user-visible feature defaults to OFF at
boot. Nothing runs unless explicitly started via WS or serial command.
Only one feature is active at a time; swaps go through
`feature_manager_request_start()` which arbitrates clean stop of the
current feature before starting the next. No feature self-arms, no
silent background work, no two features running concurrently.

**No magic numbers.** Every behavioral constant — thresholds,
timeouts, addresses, sizes, retry intervals — lives in a per-module
config header (`firmware/src/config/<module>_config.h`) annotated
either "Proposed default — needs Sean's approval before lock" OR
"Locked YYYY-MM-DD". Integer literals in `.c` files are flagged by
each eval's `scan_magic` pass.

**CAN bus discipline.** CAN ID `0x7E0` only. Never `0x7DF` (broadcast
will lock the C8 J533 gateway for 10+ minutes). Never `0x710` or
`0x7E1`–`0x7E7`. Standard UDS services only (`0x3E`, `0x22`, `0x10`,
`0x14`, `0x19`, with known subfunctions). No `can_send_raw` probing.
Board pinout: BOARD_REV2, GPIO 5 TX, GPIO 16 RX, 500 kbps.

**Mandatory progress logging.** Every working session updates
`/Users/rabbit/esp/obd/status-YYYY-MM-DD.md` (decisions, eval results,
defaults proposed) and `/Users/rabbit/esp/obd/file-update-YYYY-MM-DD.md`
(what changed, why). At workspace root, NOT inside the project folder
— there's a CLAUDE.md router at `~/esp/obd/` that points at the
active project (FUTV1.1). Append to today's file if it exists; create
today's if it doesn't.

**Proprietary IP.** Everything in this repo is SRM proprietary
intellectual property. Do not exfiltrate. Do not paste secrets to
third-party services. AES keys live in `secrets/` (gitignored) and
never leave that directory.

---

## 3. The eval-gate pattern (this is how the work stays honest)

Each prompt produces an `eval.sh` graded harness at
`firmware/test/<module>/eval.sh`. The agent must run it and see
`RESULT: PASS` before declaring done. Sections include:

1. File structure check — required files exist with expected names
2. Public API surface — required symbols declared in headers
3. No-magic-numbers scan on each new `.c` file (with comment + string
   stripping; one eval has a perl pre-pass to handle multi-line
   blocks)
4. Config header has named #defines AND approval-status annotations
   (either "needs approval" or "Locked YYYY-MM-DD")
5. Forbidden modifications check via `git status --porcelain` against
   a per-prompt forbidden-paths list (frozen modules + adjacent
   prompts' modules)
6. Host test compiles AND runs to PASS
7. Required test scenarios literally present in the test source
   (grep-based — Sean's literal scenario list, not "close enough")
8. Optional full `idf.py build` (default ON; `SKIP_IDF_BUILD=1`
   disables only when IDF isn't installed)

**`SKIP_IDF_BUILD=1` is for environments without IDF, not for
convenience.** Skipping it once let a silent link failure paper-PASS;
the reconciliation session caught it. Do not use the flag to "make
the eval green."

The harness is the gate. **Do not declare done with any FAIL line in
the output.** If you can't make the gate green, fix it; if the gate
itself is wrong, fix the gate and re-justify.

`firmware/test/verify_frozen.sh` runs from inside every per-module
eval, so a session that modifies a frozen file cannot pass even its
own eval. Same with the cross-module regression: every per-module
eval is run against the whole tree, so a half-finished work-in-
progress in one prompt can't silently break a sibling prompt's
build.

---

## 4. The Claude Code collaboration pattern (this is how prompts work)

Each prompt has a paste-ready text Sean copies into a Claude Code
session. The pattern that's worked:

**The prompt body.** Pre-decided choices at every load-bearing
decision (so the agent doesn't burn its clarification budget on
things Sean has already thought through). Module list with rough
line budgets. Required test scenarios literal text. Hard rules
restated. Acceptance criteria via `eval.sh PASS`. Explicit
"Do NOT in this task" list. Closing instruction to ask clarifying
questions before writing code rather than after.

**The pre-flight clarification round.** The good agents survey the
existing code, identify real ambiguities, and surface them as a
numbered list with proposed answers. Sean (or me on Sean's behalf)
responds with a paste-ready answer block — usually "go on most,
override on N for these reasons." This round has caught:

- A starter fixture I hand-authored had transcription errors (read_vin
  candump bytes didn't reassemble to the expected VIN)
- Frozen-module API shape didn't match my shorthand (`_parse`/`_apply`
  isn't real; the actual API is iterative open/read-N/close)
- A `FEATURE_DTC_CLEAR` enum that should be `FEATURE_DTC` (one feature
  for both read and clear)
- Whether end-user-buildable SBFs require validation hardening
  (yes, but the SRM-hosted-builder constraint collapses most of it)
- Whether the wot_uploader license gate should bundle into Prompt 5
  (yes, agent argued correctly)

**The eval-gated handoff.** Agent runs the gate, prints PASS/FAIL,
appends to status + file-update logs, surfaces proposed defaults
needing approval. Sean reads the summary; if it's clean, prompt is
done.

**Override discipline.** When Sean (or I) override an agent's proposal,
explain WHY in the response. Agents without rationale just learn to
defer; agents with rationale build judgment. Recent example: I
overrode the SBF blend math placement (agent wanted it in
`sbf_applier.c`; I moved it to `flex_fuel/blend_engine.c` because
the 10-line stub there is literally the named slot for that math
and Prompts 6/7 will add adjacent files in the same directory).

---

## 5. Git state and branch convention

The repo IS git-tracked as of `git init` on 2026-05-05. Initial
snapshot accidentally tracked `firmware/build/`, `firmware/.cache/`,
and the `host_test_runner` binaries; these have all been added to
`.gitignore` and removed from index in subsequent commits.

**Branch convention.** Each prompt runs on its own feature branch
(`feat/prompt-N-<name>`). After eval green, commit on the branch,
merge to `main`, branch again for the next prompt. The branch
convention has had exceptions (Prompts 1, 2, 3, 4 were partly done
on `main` directly when git wasn't yet a habit) but Prompt 5 ran
cleanly on `feat/prompt-5-sbf-live-tune`. Continue this pattern.

**Commit hygiene.** Sean's standing rule: "Only create commits when
requested." Agents ask before committing. The housekeeping commits
(gitignore expansions, snapshot artifact corrections) get explicit
approval before landing.

**The Section-5 false-positive trap.** `git status --porcelain` reads
build artifacts and cache directories as "modified" if they're tracked.
The Prompt 4 housekeeping commit added the right gitignore entries;
if you see Section-5 failures on otherwise-green prompts, the cause
is almost certainly a new build/cache path that needs gitignoring.

---

## 6. Open decisions (Sean's signoffs awaited)

Tracked as Cowork tasks; also restated here for durability:

**License + VIN pairing config defaults.** ~25 constants across
`license_config.h` and `vin_pairing_config.h`. Currently annotated
"Proposed default — needs Sean's approval." None are load-bearing.
Will batch-lock alongside `feature_manager_config.h`, `dtc_config.h`,
`sbf_config.h`, and `can_capture/defaults.cfg` when Sean has time
to review the consolidated set.

**SBF config defaults.** Same pattern, ~15 constants in
`sbf_config.h` from Prompt 5.

**Phase 2 prerequisites.** Eleven items in
`docs/PHASE_2_PREREQUISITES.md` (P-01 through P-11), all 🔴 NOT
STARTED. P-01 (MagicMotorsport flash capture session) is the
unblocker for most of the rest; cannot be done remotely — needs
Sean + dev RS7 + Candlelight + Y-splitter. Tracked as Cowork task #2.

**`state_machine/` rename.** Carryover from Prompt 1. The directory
is misnamed (it's CAN/UDS connection management, not a feature
arbiter — that's `feature_manager/`). Should rename to `uds_session/`
in a sweeping pass that touches all references at once. Deferred
through five prompts; harmless but accumulating.

---

## 7. Cowork tasks (live state)

| # | Status | Task |
|---|--------|------|
| 1 | completed | WOT defaults locked 2026-05-05 |
| 2 | pending | MagicMotorsport flash capture session (P-01) |
| 3 | completed | Prompt 3 — DTC read/clear |
| 5 | completed | Prompt 4 — VIN pairing + licensing |
| 6 | completed | License gate wiring (bundled into Prompt 5) |
| 7 | pending | Sign off license + vin_pairing defaults (batch later) |
| 8 | completed | Prompt 5 — SBF live tune orchestrator |

(Task #4 was deleted; it tracked merging the CAN toolkit branch which
turned out to be a no-op since git wasn't initialized yet.)

---

## 8. What's next, in roughly increasing scope

**Smallest:** any of the deferred admin tasks. Lock the remaining
config defaults in a single batch prompt. Sweep-rename `state_machine/`
to `uds_session/` across all references.

**Next feature prompts:**

- **Prompt 6 — BLE ethanol sensor bridge.** External BLE sensor
  feeds ethanol percentage to the dongle. Adds `flex_fuel/ethanol_input.c`
  alongside the `flex_fuel/blend_engine.c` that Prompt 5 landed.
  Manual override remains; BLE adds a new source. Feature on
  feature_manager: `FEATURE_BLE_PAIRING` enum exists.

- **Prompt 7 — Ethanol constraint logic.** MISSION_SPEC §4.5 — the
  hysteresis (±3% threshold), dwell (60s), WOT lockout, rev-limit
  during update window (4000 RPM cap), 30s stabilization window.
  Adds `flex_fuel/ethanol_constraints.c`. Wraps the live-tune apply
  path with the safety constraints that Prompt 5 explicitly deferred.
  This is where the ethanol blending becomes safe for daily driving.

- **Prompt 8 — Transport abstraction layer.** Currently CAN-only.
  Ethernet hardware "arriving soon" per MISSION_SPEC. Refactor the
  UDS/ISO-TP stack behind a transport-agnostic interface so the
  same UDS layer runs over CAN or Ethernet. Mostly mechanical but
  touches a lot of files. Probably the riskiest cross-cutting change
  before Phase 2.

**Phase 2 (full ECU binary reflash) — gated by all 11 P-prerequisites
in `docs/PHASE_2_PREREQUISITES.md`.** The MagicMotorsport capture
session (P-01) is the practical unblocker. Without it, Phase 2 flash
code has nothing to validate against.

---

## 9. Hard-won lessons (don't repeat these)

**Snapshot-artifact tracking.** Initial `git add -A` after `git init`
silently tracked build artifacts. Caused Section-5 false positives
in evals downstream. Now gitignored; if anything similar shows up,
the fix pattern is: add to `.gitignore`, `git rm -r --cached`, commit
with explanation in the file-update log.

**Skip-IDF-build silent paper-PASS.** A prior Prompt 3 attempt
declared PASS while running with `SKIP_IDF_BUILD=1`, which hid that
`dtc_commands.c` was never registered in `firmware/src/CMakeLists.txt`.
The reconciliation session (round-12 in status log) caught it via
real `idf.py build`. Lesson: do not skip the on-target build to make
an eval green. Fix the underlying issue.

**Two parallel sessions both shipping the same prompt.** Prompt 3
got worked twice in different chats; they wrote conflicting code,
and disk converged via overwrites with one inconsistency (NRC 0x22
vs Sean's literal contract of 0x33). The reconciliation session
audited disk vs. literal contract and confirmed disk was correct
(the inconsistency was a stale grep match). Lesson: when running
parallel sessions, give them strictly disjoint sandboxes (different
directories, different branches) and ensure each agent sees no
overlap. The CAN toolkit + Prompt 2 split worked because directories
were truly disjoint.

**Hand-authored fixtures had transcription bugs.** The initial
`read_vin.candump` had ISO-TP frame errors (odd nibble counts,
over-DLC). The tandem CAN agent caught it before writing a parser
to match malformed data. Lesson: when authoring synthetic CAN
fixtures, write a small Python script to verify they reassemble to
expected output BEFORE committing. Don't trust hand-built byte
sequences.

**The `state_machine/` misnomer.** A directory in the repo named
`state_machine/` is actually CAN/UDS connection management. The
"state machine" everyone means colloquially is the per-feature ON/OFF
arbiter, which lives in `feature_manager/`. Until the rename
happens, every new agent has to be told this once.

**Parser scan_magic edge case.** The DTC eval's `scan_magic`
excludes `\b[01]\b`, which means a literal `req[1]` array index
slips past the magic-number scan. Today's code uses named offsets
so it's not a real gap; a future feature regressing to inline
indices would not be caught. One-line eval cleanup waiting whenever
someone's in the eval scripts next.

---

## 10. Quick reference — where things live

```
~/esp/obd/                                   workspace root
├── CLAUDE.md                                workspace router; points at FUTV1.1
├── status-YYYY-MM-DD.md                     daily session journal
├── file-update-YYYY-MM-DD.md                file-by-file change log
├── FUTV1.0/                                 LEGACY — read-only reference
├── SEFIv1/                                  ARCHIVED — historical reference
└── FUTV1.1/                                 ACTIVE BUILD
    ├── CLAUDE.md                            project rules — binding
    ├── README.md
    ├── docs/
    │   ├── MISSION_SPEC.md                  product spec (Phase 1 + 2)
    │   ├── SCALE_ARCHITECTURE_PROPOSAL.md   server / scaling architecture
    │   ├── PHASE_2_PREREQUISITES.md         P-01 through P-11 checklist
    │   ├── BENCH_CAN_TOOLKIT.md             Candlelight bench setup spec
    │   ├── CAN_UDS_PROTOCOL.md              UDS service reference
    │   ├── boxcode_database.{md,json}       ECU variant matrix
    │   ├── ecu_variable_db.json             RAM variable mappings
    │   ├── CLAUDE_CODE_KICKOFF.md           Prompts 1-2 paste text
    │   ├── CLAUDE_CODE_PROMPT_5.md          Prompt 5 paste text
    │   ├── CLAUDE_CODE_TANDEM_CAN.md        CAN toolkit tandem session prompt
    │   └── SESSION_HANDOFF.md               THIS FILE
    ├── firmware/
    │   ├── src/
    │   │   ├── FROZEN_MODULES.md            BINDING — frozen list + approval ritual
    │   │   ├── frozen_modules.sha256        canonical hashes
    │   │   ├── main.c                       boot, init order matters
    │   │   ├── feature_manager/             central state arbiter (Prompt 1)
    │   │   ├── scal/, bdef/, ecu_write/     FROZEN — call .h, do not touch .c
    │   │   ├── logger/wot_*.c               WOT logger (Prompt 2)
    │   │   ├── dtc/                         DTC read/clear (Prompt 3)
    │   │   ├── vin_pairing/, license/       Prompt 4
    │   │   ├── sbf/                         live tune orchestrator (Prompt 5)
    │   │   ├── flex_fuel/blend_engine.c     blend math (Prompt 5)
    │   │   ├── commands/                    WS/serial command dispatch
    │   │   ├── state_machine/               misnomer — actually UDS/CAN session
    │   │   ├── config/                      per-module config headers
    │   │   ├── nvs/, wifi/, websocket/, ota/, error/, filesystem/
    │   │   └── isotp_coordinator/, can/
    │   └── test/
    │       ├── verify_frozen.sh             cross-prompt frozen check
    │       ├── feature_manager/eval.sh      P1 gate
    │       ├── wot_logger/eval.sh           P2 gate
    │       ├── dtc/eval.sh                  P3 gate
    │       ├── vin_pairing/eval.sh          P4 gate
    │       ├── sbf/eval.sh                  P5 gate
    │       └── can_capture/                 bench toolkit (tandem session)
    ├── cloud/                               FastAPI + SQLite server
    │   ├── src/main.py                      device + admin + license endpoints
    │   └── tests/                           pytest (14 cases)
    ├── ui/, sbf/, hw_reference/, tools/
    └── secrets/                             AES keys, gitignored
```

**Key invariant:** the FROZEN_MODULES.md hash list must always match
disk. Run `firmware/test/verify_frozen.sh` first thing in any new
session as a sanity check. If it fails, do not write code; surface
the divergence immediately.

---

## 11. Continuation playbook for a fresh chat

If you (Claude) are reading this in a new session:

1. Read this file first. It's the ground truth.
2. Read `~/esp/obd/CLAUDE.md` (router) and `~/esp/obd/FUTV1.1/CLAUDE.md`
   (project rules).
3. Run `firmware/test/verify_frozen.sh` to confirm baseline integrity.
4. Read the most recent `status-YYYY-MM-DD.md` for what's happening
   right now.
5. Confirm the Cowork task list is consistent with what's on disk.
6. Ask Sean what the next move is. Don't infer; ask.

If a Claude Code session is mid-flight when you arrive: do not run
its eval in parallel. Coordinate via Sean. Two sessions running the
same eval will race on the host_test_runner build artifact and
corrupt state.

When in doubt: read disk, ask Sean, and remember that the eval gate
is the gate. There is no "good enough."

---

## 12. The shape of trust

The pattern that's gotten us this far: every prompt produces a
gate-passable artifact, every gate enforces a binding rule, every
binding rule is documented, every documented rule has a reason.
None of this is theatre. The eval that caught a silent link failure
in Prompt 3 was the same eval pattern that protected the frozen
modules through five prompts — it's the same discipline. Honor it.

Sean is non-developer-comfortable working at scale. He delegates
heavily but he reads everything. Be honest about uncertainty. Push
back when proposals are wrong. Override with reasoning when the
agent's proposal is suboptimal. Surface ambiguity before assuming.
The work has gone well because the patterns are good, not because
anyone has been cutting corners.
