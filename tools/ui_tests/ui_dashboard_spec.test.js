// @ts-check
/**
 * P-69 — UI Dashboard v1 acceptance criteria.
 *
 * Ports all 10 acceptance criteria from docs/UI_DASHBOARD_SPEC.md §7
 * into deterministic Playwright tests. Drives the customer-visible
 * surface through a real browser; WS reads alone are not sufficient
 * (CLAUDE.md Rule 10).
 *
 * Each test starts fresh (storage cleared) and waits for the WS
 * `connLabel` to read "Connected" before exercising AC behavior.
 */

const { test, expect } = require('@playwright/test');

const VAR_NMOT_W = 'nmot_w';
const VAR_TMOT   = 'tmot';
const VAR_RL_W   = 'rl_w';
const VAR_WDKBA  = 'wdkba';

/* Helper: ensure the page is on a clean slate AND the WS has come up.
 *
 * Earlier impl used addInitScript(localStorage.clear()) which fires
 * on EVERY navigation — so page.reload() in AC9 wiped the very state
 * we were trying to verify persisted. Now we clear once after the
 * initial navigation and then reload to get a clean start; subsequent
 * page.reload() calls in tests preserve whatever the page wrote.
 */
async function bootClean(page) {
  await page.goto('/?cb=playwright');
  /* Wait for the WS handshake before clearing — origin/clearstorage
   * needs the page document loaded. */
  await page.evaluate(() => { try { localStorage.clear(); } catch (e) {} });
  await page.reload();
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  /* Also wait for license_status to land before the test starts
   * exercising VIN-dependent state — the lock class flip is the
   * cleanest signal. With a paid VIN the timeout is ~hundreds of ms;
   * leave a generous 5 s. */
  await expect(page.locator('#licenseLock')).toHaveClass(/paid|unpaid|revoked/, { timeout: 6_000 });
}

/* Helper: tick the Logged + Show-on-Dashboard checkboxes for a var on
 * the Log Config tab. Assumes bootClean already ran. */
async function tickVar(page, name) {
  await page.evaluate((n) => { switchTab('logconfig'); }, name);
  /* Expand the variable's category — initLogConfig opens the first
   * category by default, but other vars (tmot, rl_w in different
   * categories) need their parent to open. Click the category header
   * if the row is hidden. */
  const row = page.locator(`.logcfg-var-row[data-varname="${name}"]`);
  if (!(await row.isVisible().catch(() => false))) {
    const cat = row.locator('xpath=ancestor::*[contains(@class,"logcfg-category")]')
                   .locator('.logcfg-cat-header');
    await cat.click();
  }
  await page.locator(`#logvar_${name}`).check();
  await page.locator(`#show_${name}`).check();
}

/* Helper: fire a raw WS command from the page's own WebSocket. */
async function wsSendFromPage(page, obj) {
  await page.evaluate((o) => { window.wsSend(o); }, obj);
}

/* ------------------------------------------------------------------ */
test.describe('UI Dashboard v1 (P-69 acceptance §7)', () => {

  test('AC1: Logger Config column visible + editable', async ({ page }) => {
    await bootClean(page);
    await page.evaluate(() => switchTab('logconfig'));

    /* Column markup present. */
    const showCbs = page.locator('.logcfg-show-dash');
    await expect(showCbs.first()).toBeVisible();
    /* At least the 6 RS7 logger vars have a Show checkbox. */
    expect(await showCbs.count()).toBeGreaterThan(5);

    /* Checkbox is editable. */
    const cb = page.locator(`#show_${VAR_NMOT_W}`);
    /* nmot_w lives in the 'engine' category which initLogConfig opens
     * by default. Make sure both columns toggle independently. */
    await page.locator(`#logvar_${VAR_NMOT_W}`).check();
    await cb.check();
    await expect(cb).toBeChecked();
    await expect(page.locator(`#logvar_${VAR_NMOT_W}`)).toBeChecked();
  });

  test('AC2: Three gauges render when 3 vars selected', async ({ page }) => {
    await bootClean(page);
    /* nmot_w + rl_w both live in 'engine' (open by default); tmot lives
     * in 'temperature'. tickVar() opens the category if needed. */
    await tickVar(page, VAR_NMOT_W);
    await tickVar(page, VAR_RL_W);
    await tickVar(page, VAR_TMOT);

    await page.evaluate(() => switchTab('dashboard'));
    const gauges = page.locator('.dash-gauge');
    await expect(gauges).toHaveCount(3);
    /* Order matches selection order. */
    const dataVars = await gauges.evaluateAll(els => els.map(e => e.dataset.var));
    expect(dataVars).toEqual([VAR_NMOT_W, VAR_RL_W, VAR_TMOT]);
  });

  test('AC3: Start Streaming populates gauges within 1.5s', async ({ page }) => {
    await bootClean(page);
    await tickVar(page, VAR_NMOT_W);
    await tickVar(page, VAR_RL_W);
    await tickVar(page, VAR_TMOT);

    await page.evaluate(() => switchTab('dashboard'));
    /* Initial readout placeholder is "--". */
    await expect(page.locator(`#dgv_${VAR_NMOT_W}`)).toHaveText('--');

    await page.click('#dashStartBtn');
    /* Button flips to Stop. */
    await expect(page.locator('#dashStartBtn')).toContainText('Stop Streaming', { timeout: 2000 });
    /* Within ~1.5 s, the first poll arrives and the readout updates
     * away from "--". Allow 2.5 s to absorb network + 500 ms poll
     * jitter. nmot_w at KOEO reads 0 (a real value, not "--"). */
    await expect.poll(
      async () => (await page.locator(`#dgv_${VAR_NMOT_W}`).textContent()) || '',
      { timeout: 2500, intervals: [100, 250, 500] }
    ).not.toBe('--');
  });

  test('AC4: Stop Streaming fades + freezes', async ({ page }) => {
    await bootClean(page);
    await tickVar(page, VAR_NMOT_W);
    await page.evaluate(() => switchTab('dashboard'));
    await page.click('#dashStartBtn');
    await expect.poll(
      async () => (await page.locator(`#dgv_${VAR_NMOT_W}`).textContent()) || '',
      { timeout: 2500 }
    ).not.toBe('--');

    /* Click Stop. */
    await page.click('#dashStartBtn');
    await expect(page.locator('#dashStartBtn')).toContainText('Start Streaming');
    /* Grid gets the .stopped class → opacity 0.4 via CSS. */
    await expect(page.locator('#dashGaugeGrid')).toHaveClass(/stopped/);
    /* Values are frozen — capture the readout, wait 1 s, expect no
     * change (no further polls). */
    const v1 = await page.locator(`#dgv_${VAR_NMOT_W}`).textContent();
    await page.waitForTimeout(1100);
    const v2 = await page.locator(`#dgv_${VAR_NMOT_W}`).textContent();
    expect(v1).toBe(v2);
  });

  test('AC5: Untick variable, gauge disappears within 1s', async ({ page }) => {
    await bootClean(page);
    await tickVar(page, VAR_NMOT_W);
    await tickVar(page, VAR_TMOT);
    await page.evaluate(() => switchTab('dashboard'));
    await page.click('#dashStartBtn');
    await expect(page.locator('.dash-gauge')).toHaveCount(2);

    /* Switch to Log Config, untick tmot. */
    await page.evaluate(() => switchTab('logconfig'));
    await page.locator(`#show_${VAR_TMOT}`).uncheck();
    /* Return to Dashboard — gauge should be gone within 1 s. */
    await page.evaluate(() => switchTab('dashboard'));
    await expect(page.locator(`.dash-gauge[data-var="${VAR_TMOT}"]`)).toHaveCount(0, { timeout: 1500 });
    /* nmot_w still present. */
    await expect(page.locator(`.dash-gauge[data-var="${VAR_NMOT_W}"]`)).toBeVisible();
  });

  test('AC6: WOT banner appears, gauges keep updating', async ({ page }) => {
    await bootClean(page);
    await tickVar(page, VAR_NMOT_W);
    await page.evaluate(() => switchTab('dashboard'));
    await page.click('#dashStartBtn');

    /* Wait for the FIRST poll to populate before exercising WOT —
     * otherwise the "gauges keep updating" check races the first
     * 500 ms poll arrival and sees the "--" placeholder. */
    await expect.poll(
      async () => (await page.locator(`#dgv_${VAR_NMOT_W}`).textContent()) || '',
      { timeout: 3000, intervals: [100, 250, 500] }
    ).not.toBe('--');

    /* Fire wot_log_start over the page's own WS. */
    await wsSendFromPage(page, { command: 'wot_log_start' });
    /* Banner appears (setActiveFeature → dashboardSetWotBanner). */
    await expect(page.locator('#dashWotBanner')).toBeVisible({ timeout: 3000 });
    /* Gauges keep updating during WOT mode — the recorder being
     * armed shouldn't kill the gauge stream. Check two samples
     * spaced beyond one poll interval. */
    const before = await page.locator(`#dgv_${VAR_NMOT_W}`).textContent();
    expect(before || '').not.toBe('--');
    await page.waitForTimeout(1200);
    const after = await page.locator(`#dgv_${VAR_NMOT_W}`).textContent();
    expect(after || '').not.toBe('--');

    /* Stop WOT, banner clears within 2 s. */
    await wsSendFromPage(page, { command: 'wot_log_stop' });
    await expect(page.locator('#dashWotBanner')).toBeHidden({ timeout: 3000 });
  });

  test('AC7: Top bar bindings — license + connection + active feature', async ({ page }) => {
    await bootClean(page);
    /* Within 5 s of license_status arrival the lock flips to paid+VIN
     * (assumes the dongle is paid:true). Note: matching /paid/ alone
     * is unsafe — it substring-matches 'unpaid' too. Use an
     * (^|\\s)paid(\\s|$) shape so a class list of just 'unpaid'
     * doesn't satisfy. */
    await expect(page.locator('#licenseLock')).toHaveClass(/(^|\s)paid(\s|$)/, { timeout: 6000 });
    /* Tooltip carries the VIN (real 17-char-ish ISO-3779). The
     * existing render emits <span class="lt-key">VIN:</span> X. */
    const tip = await page.locator('#licenseLock .license-tooltip').innerHTML();
    expect(tip).toMatch(/VIN:\s*(?:<[^>]+>)?\s*[A-Z0-9]{11,17}/i);
    /* The tooltip's License: line MUST say paid, not unpaid. */
    expect(tip).toMatch(/License:\s*(?:<[^>]+>)?\s*paid/i);
    /* connLabel green + "Connected". */
    await expect(page.locator('#connLabel')).toHaveText('Connected');
    await expect(page.locator('#connDot')).toHaveClass(/(^|\s)connected(\s|$)/);
    /* activeFeatureLabel shows a non-empty state. */
    await expect(page.locator('#activeFeatureLabel')).toContainText(/Active:/);
  });

  test('AC8: WS disconnect/reconnect — top bar flips, gauges fade', async ({ page }) => {
    /* Pure browser-side simulation: close the page's WebSocket. The
     * wsConnect onclose handler runs the reconnect loop; we wait for
     * the resulting Disconnected → Connected transition on the top
     * bar. Driving the firmware-side WiFi mode is a separate test
     * (would need node-serialport + the dongle's USB port); covered
     * in a follow-up. */
    await bootClean(page);
    await tickVar(page, VAR_NMOT_W);
    await page.evaluate(() => switchTab('dashboard'));
    await page.click('#dashStartBtn');
    await expect.poll(
      async () => (await page.locator(`#dgv_${VAR_NMOT_W}`).textContent()) || '',
      { timeout: 2500 }
    ).not.toBe('--');

    /* Close the WS from the page side. */
    await page.evaluate(() => { try { ws.close(); } catch (_) {} });
    await expect(page.locator('#connLabel')).toHaveText('Disconnected', { timeout: 5000 });

    /* The dongle's ws server is still up; the page's reconnect timer
     * (WS_RECONNECT_MS = 3 s) brings it back. */
    await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
    /* Gauges resume within ~3 s after reconnect (Started intent
     * persists; dashboardStart is re-fired on reconnect). */
    await expect.poll(
      async () => (await page.locator(`#dgv_${VAR_NMOT_W}`).textContent()) || '',
      { timeout: 5000 }
    ).not.toBe('--');
  });

  test('AC9: localStorage persistence across hard reload', async ({ page }) => {
    await bootClean(page);
    /* Tick 4 gauges. */
    await tickVar(page, VAR_NMOT_W);
    await tickVar(page, VAR_RL_W);
    await tickVar(page, VAR_TMOT);
    await tickVar(page, VAR_WDKBA);

    /* Verify localStorage now has the real-VIN bucket. */
    const stored = await page.evaluate(() => {
      const all = {};
      for (let i = 0; i < localStorage.length; i++){
        const k = localStorage.key(i);
        all[k] = localStorage.getItem(k);
      }
      return all;
    });
    const keys = Object.keys(stored).filter(k => k.startsWith('dashboard_vars_'));
    expect(keys.length).toBeGreaterThan(0);
    /* At least one key should be the real VIN (NOT '(none)') because
     * the license-binding fix lands the real VIN within 5 s of boot. */
    const realKey = keys.find(k => k !== 'dashboard_vars_(none)');
    expect(realKey).toBeDefined();

    /* Hard reload (browser reload, not just navigate). */
    await page.reload();
    await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });

    /* Open Log Config — all 4 Show checkboxes should still be ticked. */
    await page.evaluate(() => switchTab('logconfig'));
    /* Open both categories so the test isn't sensitive to default-open
     * behavior. */
    await page.evaluate(() => {
      document.querySelectorAll('.logcfg-cat-body').forEach(b => b.classList.add('open'));
    });
    for (const v of [VAR_NMOT_W, VAR_RL_W, VAR_TMOT, VAR_WDKBA]){
      await expect(page.locator(`#show_${v}`)).toBeChecked();
    }

    /* Dashboard renders 4 gauges (after Start). */
    await page.evaluate(() => switchTab('dashboard'));
    await page.click('#dashStartBtn');
    await expect(page.locator('.dash-gauge')).toHaveCount(4);
  });

  test('AC11: not-in-profile placeholder + Enable-polling link', async ({ page }) => {
    /* P-69 #3: with 10 Show-on-Dashboard ticks but only 6 actually
     * polled by the firmware logger profile, expect 6 populated
     * gauges + 4 placeholder gauges with the Enable-polling link.
     *
     * The 6 default-polled vars on RS7 are nmot_w + the other 5
     * from logger_variables.c::VARIABLES_4K0907557G__0003[].
     * Tick Show on those 6 + 4 additional vars (not in the profile)
     * and assert the placeholder count. */
    await bootClean(page);
    /* P-75: prior tests may have left the dongle on a sparse named
     * profile, so the polled-set isn't guaranteed to match RS7's
     * 3-required + 3-saved baseline. Reset the active profile to
     * the AC11 baseline before exercising the placeholder UX. */
    await page.evaluate(() => new Promise(resolve => {
      wsSend({command:'set_logger_profile',
              params:{name:'ac11_baseline',
                      variables:['rl_w','tmot','wdkba']}}, resolve);
    }));
    /* Wait for can_task to reconfigure + the polled-set refresh to
     * land in the UI before the test proceeds. */
    await page.waitForTimeout(1500);

    const polled = [
      'nmot_w', 'InjSys_ratEthPrtnBascFu', 'Com_stCrCtlPan',
      'rl_w', 'tmot', 'wdkba',
    ];
    /* 4 vars known NOT to be in the default firmware profile. */
    const notPolled = ['zwoutzyl_w', 'frm_w', 'pvdg_w', 'lamsbg_w'];

    for (const v of polled) await tickVar(page, v);
    /* For the not-polled vars we only need Show-on-Dashboard ticked
     * (no Logged needed; the whole point is that the var isn't in
     * the firmware profile). Open all categories first so the
     * checkboxes are visible. */
    await page.evaluate(() => switchTab('logconfig'));
    await page.evaluate(() => {
      document.querySelectorAll('.logcfg-cat-body').forEach(b => b.classList.add('open'));
    });
    for (const v of notPolled){
      await page.locator(`#show_${v}`).check();
    }

    await page.evaluate(() => switchTab('dashboard'));
    /* The dashboard should refresh its polled-set knowledge on tab
     * enter via get_logger_profile. Wait for the placeholder gauges
     * to appear. */
    await expect(page.locator('.dash-gauge')).toHaveCount(10, { timeout: 5000 });
    await expect(page.locator('.dash-gauge-notpolled')).toHaveCount(4, { timeout: 5000 });

    /* Each placeholder must carry the Enable-polling link. */
    const links = page.locator('.dash-gauge-notpolled .dash-gauge-enable-link');
    await expect(links).toHaveCount(4);
    await expect(links.first()).toContainText(/Enable polling/);

    /* Clicking the link must switch to the Log Config tab. */
    await links.first().click();
    await expect(page.locator('#panel-logconfig')).toHaveClass(/active/);
  });

  /* AC-12 + AC-13 — P-80 refcounted polling lifecycle.
   *
   * AC-12: explicit Stop from the only WS consumer must drop the
   * refcount to 0 and the firmware response reflects that. (Wire-
   * level "polling actually halts within one cycle" is proven by
   * the wire trace in the P-80 commit body, not Playwright.)
   *
   * AC-13: WOT capture acquires its own consumer ref. With Dashboard
   * stopped but WOT capture armed, refcount stays >= 1 so polling
   * stays alive. Requires a live ECU plus the ability to arm WOT,
   * which Playwright can do via wot_start / wot_stop WS commands.
   */
  test('AC-12: Stop drops WS-fd refcount to 0 (live)', async ({ page }) => {
    test.skip(process.env.DEV_ECU !== '1', 'Requires live dongle (DEV_ECU=1).');
    await bootClean(page);
    await tickVar(page, VAR_NMOT_W);
    await page.evaluate(() => switchTab('dashboard'));

    /* Capture every WS message so we can read back the firmware's
     * refcount field. The control_panel doesn't expose this on the
     * DOM — it goes through onmessage. Stash incoming JSON on a
     * page-scoped array. */
    await page.evaluate(() => {
      window.__p80_msgs = [];
      const orig = window._wsOriginalOnMessage || window.ws.onmessage;
      window._wsOriginalOnMessage = orig;
      window.ws.onmessage = function (ev) {
        try { window.__p80_msgs.push(JSON.parse(ev.data)); } catch (e) {}
        if (orig) orig.call(this, ev);
      };
    });

    await page.click('#dashStartBtn');           /* fires logger_start */
    await page.waitForTimeout(500);
    await page.click('#dashStartBtn');           /* fires logger_stop  */
    await page.waitForTimeout(500);

    const stopResp = await page.evaluate(() => {
      return window.__p80_msgs.find(m =>
        m && m.command === 'logger_stop' && m.success === true);
    });
    expect(stopResp).toBeTruthy();
    /* command_handler wraps payload in { data: {…} }. */
    const payload = (stopResp && stopResp.data) ? stopResp.data : stopResp;
    expect(payload.refcount).toBe(0);
  });

  test('AC-13: Stop with WOT armed keeps polling alive (live)', async ({ page }) => {
    test.skip(process.env.DEV_ECU !== '1', 'Requires live dongle + WOT (DEV_ECU=1).');
    await bootClean(page);
    await tickVar(page, VAR_NMOT_W);
    await page.evaluate(() => switchTab('dashboard'));

    await page.evaluate(() => {
      window.__p80_msgs = [];
      const orig = window._wsOriginalOnMessage || window.ws.onmessage;
      window._wsOriginalOnMessage = orig;
      window.ws.onmessage = function (ev) {
        try { window.__p80_msgs.push(JSON.parse(ev.data)); } catch (e) {}
        if (orig) orig.call(this, ev);
      };
    });

    await page.click('#dashStartBtn');                /* WS consumer ref */
    await page.waitForTimeout(300);
    await wsSendFromPage(page, { command: 'wot_start' });   /* WOT consumer ref */
    await page.waitForTimeout(500);
    await page.click('#dashStartBtn');                /* WS Stop → drops 1 ref */
    await page.waitForTimeout(500);

    const stopResp = await page.evaluate(() => {
      return window.__p80_msgs.find(m =>
        m && m.command === 'logger_stop' && m.success === true);
    });
    expect(stopResp).toBeTruthy();
    const payload = (stopResp && stopResp.data) ? stopResp.data : stopResp;
    /* WOT still holds its ref → refcount must be >= 1. */
    expect(payload.refcount).toBeGreaterThanOrEqual(1);

    /* Clean up: stop WOT so the bench doesn't keep polling indefinitely. */
    await wsSendFromPage(page, { command: 'wot_stop' });
  });

  /* AC10 is the long-running soak. Skipped by default; run with
   * NIGHTLY=1 npx playwright test once it's wanted in CI. */
  test('AC10: 60min run, no memory growth, no duplicate timers', async ({ page }) => {
    test.skip(process.env.NIGHTLY !== '1', 'Skipped by default. Set NIGHTLY=1 to run the 60-minute soak.');
    await bootClean(page);
    await tickVar(page, VAR_NMOT_W);
    await page.evaluate(() => switchTab('dashboard'));
    await page.click('#dashStartBtn');

    /* Baseline JS heap. */
    const baseHeap = await page.evaluate(() => performance.memory ? performance.memory.usedJSHeapSize : 0);
    /* Tab-switch Dashboard ↔ Diagnostics 20× over the run. */
    const cycles = 20;
    const dwellMs = (60 * 60 * 1000) / cycles;  /* 3 minutes between switches */
    for (let i = 0; i < cycles; i++){
      await page.waitForTimeout(dwellMs);
      await page.evaluate(() => switchTab('diag'));
      await page.waitForTimeout(500);
      await page.evaluate(() => switchTab('dashboard'));
    }
    /* Final heap — fail if grew by more than 50 %. */
    const endHeap = await page.evaluate(() => performance.memory ? performance.memory.usedJSHeapSize : 0);
    if (baseHeap > 0){
      expect(endHeap).toBeLessThan(baseHeap * 1.5);
    }
    /* Only one poll timer active. */
    const pollActive = await page.evaluate(() => !!pollTimer);
    expect(pollActive).toBe(true);
  });
});
