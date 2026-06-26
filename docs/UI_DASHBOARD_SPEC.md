# UI Dashboard Specification — v1

> Spec for the dashboard gauge screen and the Logger Config integration.
> The dashboard prototype that exists today predates spec; this doc is
> the spec.
>
> Owner: Sean / SRM Engineering. Created: 2026-05-28.
>
> **Companion docs:**
> - `MISSION_SPEC.md` §4.3 (Phase 1 live gauges + logging spec)
> - `PHASE_1_COMPLETION_PLAN.md` (Phase 1 customer-experience close gates)
>
> **Scope:** v1 is UI-side only. No firmware changes, no new WS commands,
> no logger_manager refactor. Implementation lives in
> `ui/control_panel.html` inline JS + a new `ui/dashboard_config.js`.

---

## 1. Why this spec exists

The dashboard tab that exists today was an early prototype with no spec
and no auto-start logic. As a result:

- Gauges show `--` because nothing fires `logger_start` on tab mount
- Top bar shows `License: unpaid` despite cloud `paid=1` because
  `license_status` isn't bound to the top bar
- The gauge layout is hardcoded — no user control over which variables
  appear

This spec replaces the prototype with a vetted, configurable dashboard
that integrates cleanly with existing logger + WOT capture without
stepping on either.

---

## 2. Logging architecture today (synchronicity context)

Before adding the dashboard as a logger consumer, understand the
existing producer/consumer model:

- `logger_manager` is the single CAN-side polling engine. Polls at
  ~1 Hz from `can_task`. Issues one UDS read-by-identifier per cycle.
- `logger_profile_apply()` sets the variable list that gets polled.
  Shared resource — changing it affects every consumer.
- `data_callback` is a single subscriber slot (`static
  logger_data_callback_t data_callback`). Only ONE consumer can
  register at a time.
- `wot_logger` claims the `data_callback` slot during WOT capture
  (`logger_manager_set_data_callback(on_logger_data)` on
  `wot_log_start`).
- `get_logger_data` is a passive WS command. Returns last parsed
  sample. Does NOT touch the callback slot or profile.
- `logger_start` is idempotent at the WS layer — repeated calls return
  ok without restarting.
- `logger_stop` brings polling down; consumers must coordinate.
- `LOGGER_PROFILE_MAX_ON_APPLY_CBS = 4` — supports 4 late-binding
  init-time hooks, NOT real-time data fan-out.

**Synchronicity hazards if dashboard does it wrong:**

1. Grabbing `data_callback` slot would conflict with `wot_logger`.
2. Calling `set_logger_profile` would step on the WOT capture variable
   set (this caused P-63 yesterday).
3. Doubling the poll rate would starve other UDS users (DTC read,
   VIN pair refresh).
4. Calling `logger_stop` would kill polling for everyone else.

---

## 3. Architecture decisions (v1)

| Decision | Choice | Why |
|----------|--------|-----|
| Data source | Poll `get_logger_data` on a UI timer | Passive read, no callback conflict |
| Poll cadence | 500 ms | Human-eye-good; doesn't double bus load (logger still polls ECU at ~1 Hz) |
| Logger lifecycle | Dashboard fires `logger_start` on Start button (idempotent); NEVER fires `logger_stop` | Refcount in v1.5; leave logger alive until user stops it explicitly |
| Variable selection | User-configurable on Logger Config tab | Data-driven UI, per-VIN |
| Variable persistence | `localStorage` keyed by VIN in v1; cloud DB in v2 | Ship fast, refine later |
| WOT capture coexistence | Dashboard shows "WOT capture in progress" banner; gauges keep updating | WOT mode adds variables to the profile; doesn't remove the base set |
| Top bar binding | Separate polls for `license_status` (5 s) and `get_status` (2 s) | One source of truth per element; no conflation |
| Variable widgets | Type-inferred from per-variant manifest | Numeric → dial+readout; enum → text; multi-dim → existing widgets |

**v1.5 future work (NOT in this spec):**

- Refcount on `logger_manager` so consumers `acquire()` / `release()` and
  the logger auto-stops when count → 0.
- Real-time push channel (WebSocket events) so dashboard subscribes
  instead of polls.
- Per-VIN cloud DB persistence for dashboard preferences.

---

## 4. Logger Config tab — additions

The existing Logger Config tab already lets the user pick which
variables the logger polls. Add a second checkbox column.

### 4.1 Variable table layout

```
[Variable Name]            [Logged]  [Show on Dashboard]  [Type]    [Range / Unit]
nmot_w                     [✓]       [✓]                  numeric   0–9000 rpm
tmot                       [✓]       [✓]                  numeric   -40 to 150 °C
InjSys_ratEthPrtnBascFu    [✓]       [ ]                  numeric   0–100 %
rl_w                       [✓]       [✓]                  numeric   0–200 %
wdkba                      [✓]       [ ]                  numeric   0–100 %
Com_stCrCtlPan             [✓]       [ ]                  enum      idle | active | fault
...
[ Save Profile ]   [ Reset to defaults ]
```

### 4.2 Validation rules

- A variable that is `Show on Dashboard: ✓` but `Logged: ☐` shows an
  inline warning: "Enable Logged first" with a one-click link that
  ticks both.
- "Save Profile" commits both columns at once.
- Save is gated on auth (existing `set_logger_profile` security tier
  unchanged).
- Reset to defaults restores the boxcode-specific default profile from
  the per-variant manifest AND resets dashboard selection to the
  manifest-recommended set.

### 4.3 Storage

- Logged column → existing `set_logger_profile` WS command (no change)
- Show-on-Dashboard column → `localStorage.setItem('dashboard_vars_' +
  VIN, JSON.stringify([...]))`

### 4.4 Read-on-mount

When Logger Config tab mounts:

1. Fire `get_logger_profile` to fetch the current Logged set
2. Read `localStorage` for the Show-on-Dashboard set keyed by current VIN
3. Render the joined table

---

## 5. Dashboard tab — full spec

### 5.1 Top bar (separate from gauges)

Four elements, each bound to a different data source:

| Element | Source | Update cadence | Display |
|---------|--------|----------------|---------|
| `connLabel` | WS `readyState` + `onopen`/`onclose` hooks | Event-driven | "Connected" / "Disconnected" with green/red dot |
| `licenseLock` | `license_status` WS poll | 5000 ms | `🔓 PAID · VIN <xxx>` if paid && !revoked; `🔒 UNPAID` otherwise |
| `authStatus` | Local in-page state | Event-driven on `unlock` success | "Locked" / "Unlocked" |
| `activeFeatureLabel` | `get_status` data.state | 2000 ms | Human-readable from state enum |

Top bar is ALWAYS active regardless of Start/Stop button. It runs as
long as the page is loaded.

### 5.2 Start / Stop button

Single two-state button at top of Dashboard tab.

**State: Stopped (initial)**
- Label: `▶ Start Streaming`
- Background: muted grey
- Action on click:
  - Fire `get_status` to check `logger` running
  - If logger not running, fire `logger_start` (idempotent)
  - Start the local 500 ms poll timer
  - Flip to Started state

**State: Started**
- Label: `⏸ Stop Streaming`
- Background: green indicator
- Action on click:
  - Stop local poll timer
  - Do NOT fire `logger_stop` (lifecycle owned by user via Logger
    Config or by other features)
  - Gauges fade to grey "paused" state at `DASHBOARD_PAUSE_OPACITY`
  - Last known values frozen
  - Flip to Stopped state

**Tab-switch behavior:**

- Page mount: Stopped state by default (no auto-start).
- Tab switch away from Dashboard while Started: stop the timer but
  DON'T fire logger_stop. Remember the Started intent.
- Tab switch back to Dashboard with Started intent: restart timer.
- Hard page reload: Stopped state (no persistence across page reloads;
  user opts in fresh each session).

### 5.3 Gauge layout (dynamic)

CSS grid that adapts to N selected variables (cap = `DASHBOARD_MAX_GAUGES`,
default 16).

For each variable in the user's `dashboard_vars_<VIN>` set AND present
in the latest `get_logger_data` response:

- Determine widget type from per-variant manifest metadata:
  - **Numeric scalar** (RPM, temp, %, °, hPa): numeric readout + radial
    dial. Min/max from manifest. Dial color gradient configurable.
  - **Enum** (Com_stCrCtlPan): text label with active-state highlight.
  - **Multi-dim** (per-cylinder knock 8-array): existing 8-dot row
    widget.
- Layout: CSS `grid-template-columns: repeat(auto-fit, minmax(180px,
  1fr))` so gauges flow naturally for any N.

### 5.4 Per-gauge update behavior

On every 500 ms poll response:

- For each gauge:
  - If variable name present in response → update value
  - If variable name absent (e.g., WOT mode swapped the profile and
    removed it) → render last value with grey overlay; do NOT show `--`
- Stale indicator timing:
  - 2000 ms (`DASHBOARD_STALE_FADE_MS`) without fresh update: fade
    gauge to 40% opacity
  - 5000 ms (`DASHBOARD_STALE_DASH_MS`) without fresh update: switch
    to `--` placeholder

### 5.5 WOT capture coexistence

When `get_status.data.active_feature == "wot_logger"`:

- Show banner above gauges: `⚠ WOT capture in progress`
- Banner color: orange/amber, persistent until WOT stops
- Gauges keep updating from `get_logger_data` (WOT mode usually adds
  variables, doesn't remove the base set)
- Banner disappears within one poll cycle of `active_feature` reverting
  to `none`

Dashboard MUST NOT:
- Call `wot_log_start` or `wot_log_stop` (those belong to WOT Logger tab)
- Call `set_logger_profile` while WOT is active
- Call `logger_stop` while WOT is active
- Render any "Stop WOT" affordance (separation of concerns)

### 5.6 Race conditions

- Rapid tab switching: idempotent `logger_start` + start/stop timer
  guards handle this.
- Stale wsSend callbacks after tab unmount: wrap render calls in `if
  (panel === 'dashboard' && started)` to discard orphan responses.
- WS disconnect mid-stream: top bar's `connLabel` flips to Disconnected.
  Gauges go stale → fade → dash per timing above. On reconnect, top bar
  flips back and if Started state was set, restart poll timer.
- VIN change (user re-pairs to different ECU): VIN-keyed localStorage
  switches, dashboard reloads its Show-on-Dashboard set.

---

## 6. Configuration constants

In a new file `ui/dashboard_config.js` (or inline `<script>` constants
block at top of `control_panel.html` script section):

```javascript
const DASHBOARD_POLL_INTERVAL_MS_DEFAULT = 500;
const DASHBOARD_STATUS_POLL_INTERVAL_MS_DEFAULT = 2000;
const DASHBOARD_LICENSE_POLL_INTERVAL_MS_DEFAULT = 5000;
const DASHBOARD_STALE_FADE_MS_DEFAULT = 2000;
const DASHBOARD_STALE_DASH_MS_DEFAULT = 5000;
const DASHBOARD_PAUSE_OPACITY_DEFAULT = 0.4;
const DASHBOARD_MAX_GAUGES_DEFAULT = 16;
const DASHBOARD_LOCALSTORAGE_KEY_PREFIX = "dashboard_vars_";
```

Per workspace no-magic-numbers rule: every numeric constant is named
and tunable from one location.

---

## 7. Acceptance criteria

Run these on a flashed dongle paired to a VIN. Expected results listed.

1. **Logger Config additions visible**
   - Navigate to Logger Config tab
   - See variable table with new "Show on Dashboard" column
   - All checkboxes editable and persisted on Save

2. **User selects 3 gauges**
   - Tick `nmot_w`, `tmot`, `rl_w` for Show on Dashboard
   - Save profile
   - Navigate to Dashboard tab
   - See only 3 gauges rendered

3. **Start streaming**
   - On Dashboard, click `▶ Start Streaming`
   - Within 1.5 s, all 3 gauges populate with live values
   - Button flips to `⏸ Stop Streaming`

4. **Stop streaming**
   - Click `⏸ Stop Streaming`
   - Gauges fade to 40% opacity, values freeze
   - No further WS traffic generated by Dashboard
   - Button flips to `▶ Start Streaming`

5. **Live variable removal**
   - With streaming Started, navigate to Logger Config
   - Untick `tmot` for Show on Dashboard, save
   - Return to Dashboard tab
   - Within 1 s, `tmot` gauge disappears
   - `nmot_w` and `rl_w` continue updating

6. **WOT capture coexistence**
   - On Dashboard with streaming Started, navigate to WOT Logger tab
   - Fire `wot_log_start`
   - Return to Dashboard
   - Banner shows `⚠ WOT capture in progress`
   - Gauges keep updating
   - Fire `wot_log_stop`
   - Banner clears within 2 s, gauges continue

7. **Top bar binding**
   - Fresh page load
   - Top bar shows `🔒 UNPAID` briefly during boot
   - Within 5 s, flips to `🔓 PAID · VIN WUAPCBF28NN902533`
   - `connLabel` shows "Connected" with green dot
   - `activeFeatureLabel` shows "CONNECTED" (or current state)

8. **WS disconnect / reconnect**
   - On Dashboard with streaming Started
   - Disconnect WiFi (or stop dongle)
   - Top bar flips to "Disconnected"; gauges fade then dash within 5 s
   - Reconnect
   - Top bar flips back; gauges resume within 3 s (assuming Started
     intent was preserved)

9. **localStorage persistence**
   - Set up 4 gauges, Save
   - Hard reload page (Cmd+Shift+R)
   - Logger Config still shows 4 checkboxes ticked
   - Dashboard renders 4 gauges (after Start)

10. **No memory leak / no orphan timers**
    - Run Dashboard for 60 minutes continuously
    - Open DevTools Performance tab, check no growing memory
    - Tab switch Dashboard ↔ Diagnostics 20 times
    - Only one active poll timer at any time

---

## 8. Out of scope (v1.5+)

- Logger refcount + auto-stop when consumer count → 0
- WebSocket push channel for real-time data fan-out (replace polling)
- Cloud-side persistence of dashboard preferences per VIN
- Animated dial sweeps and gauge color theming
- Multiple dashboard layouts (compact / detailed / data-logger style)
- Per-gauge custom min/max overrides (use manifest values for now)
- Touch-optimized layout for phone viewports (responsive but not
  hand-tuned)

---

## 9. Implementation notes

- Single file diff in `ui/control_panel.html` (inline JS) plus the new
  `ui/dashboard_config.js` if extracted.
- Estimate: ~200 lines of new JS for Dashboard + ~50 lines for the
  Logger Config column addition.
- No firmware build needed. Data partition flash for the HTML/JS
  changes only.
- HIL verification can run KOEO — all 10 acceptance criteria above are
  exercisable at key-on-engine-off.

---

## 10. Open questions for owner

- Per-gauge custom colors? (defer to v1.5; use type-based defaults for
  v1)
- Default "Show on Dashboard" set if user has never saved before?
  Recommended: tick the top 4 by manifest priority (typically RPM,
  boost, AFR, coolant).
- Phone-portrait layout: separate stacked-card view or just rely on
  responsive grid? (defer; responsive grid for v1)

---

## Change log

- 2026-05-28: Initial spec, post-Phase-1-close.
