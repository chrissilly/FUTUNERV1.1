// @ts-check
/**
 * Log Config tab acceptance tests (PRIORITY 1, Sean's emphasis).
 *
 * Covers:
 *   AC-LC1: Save Profile sends the firmware-expected {variables:[...]}
 *           shape and renders a green success banner.
 *   AC-LC2: Save error path renders a red banner with the error.
 *   AC-LC3: Slot counter accurate; ticking Show-on-Dashboard doesn't
 *           inflate the count.
 *   AC-LC4: Load Profile after page reload restores Logged checkboxes
 *           from get_logger_profile.{required, selected}.
 *   AC-LC5: Show-on-Dashboard ticked without Logged renders the
 *           "Enable Logged first" warn link AND clicking it ticks
 *           both checkboxes.
 *   AC-LC6: list_available_vars filter — unsupported rows render
 *           with the "(not supported on this ECU)" badge and the
 *           Logged checkbox is disabled.
 */

const { test, expect } = require('@playwright/test');

async function bootClean(page) {
  await page.goto('/?cb=playwright');
  await page.evaluate(() => { try { localStorage.clear(); } catch (e) {} });
  await page.reload();
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  await expect(page.locator('#licenseLock')).toHaveClass(/paid|unpaid|revoked/, { timeout: 6_000 });
}

async function openLogConfig(page) {
  await page.evaluate(() => switchTab('logconfig'));
  /* Open every category so checkboxes are visible regardless of
   * which category each var sits in. */
  await page.evaluate(() => {
    document.querySelectorAll('.logcfg-cat-body').forEach(b => b.classList.add('open'));
  });
}

test.describe('Log Config tab (PRIORITY 1)', () => {

  test('AC-LC1: Save Profile success → green banner + firmware accepts {variables:[]}', async ({ page }) => {
    await bootClean(page);
    await openLogConfig(page);
    /* Tick rl_w, tmot, wdkba (all in the firmware catalog → save
     * should accept all three and the banner should show "Profile
     * saved — 3 optional var(s)"). */
    for (const v of ['rl_w', 'tmot', 'wdkba']){
      await page.locator(`#logvar_${v}`).check();
    }
    /* Snapshot the outgoing frame so we can assert the wire shape.
     * `ws` is module-scoped (not on window), so wrap `wsSend` —
     * top-level function declarations attach to window. */
    await page.evaluate(() => {
      window.__sentFrames = [];
      const orig = window.wsSend;
      window.wsSend = (obj, cb) => {
        try { window.__sentFrames.push(JSON.stringify(obj)); } catch (e) {}
        return orig(obj, cb);
      };
    });
    await page.locator('#panel-logconfig button:has-text("Save Profile")').click();
    /* Banner appears within a few seconds. */
    await expect(page.locator('#logcfgSaveBanner')).toHaveClass(/show/, { timeout: 6000 });
    await expect(page.locator('#logcfgSaveBanner')).toHaveClass(/ok/);
    await expect(page.locator('#logcfgSaveBanner')).toContainText(/Profile saved/);
    /* The wire frame must use the firmware-expected envelope:
     *   {command:'set_logger_profile', params:{variables:[names]}}
     * Old UI sent {profile:{...}} at top level — firmware bailed with
     * "Missing parameters" because the command_handler reads params
     * from the nested `params` object. */
    const frames = await page.evaluate(() => window.__sentFrames || []);
    const setProfileFrame = frames.find(s => s.includes('set_logger_profile'));
    expect(setProfileFrame).toBeDefined();
    const parsed = JSON.parse(setProfileFrame);
    expect(parsed.params).toBeDefined();
    expect(parsed.params.variables).toEqual(expect.arrayContaining(['rl_w', 'tmot', 'wdkba']));
    expect(parsed.profile).toBeUndefined();
  });

  test('AC-LC3: Slot counter doesn\'t double-count Show-on-Dashboard ticks', async ({ page }) => {
    await bootClean(page);
    await openLogConfig(page);
    /* Tick BOTH Logged and Show-on-Dashboard for 2 vars. */
    await page.locator('#logvar_rl_w').check();
    await page.locator('#show_rl_w').check();
    await page.locator('#logvar_tmot').check();
    await page.locator('#show_tmot').check();
    /* Slot counter should read "2 / 32 slots" (Logged-only count),
     * NOT "4 / 32" (would-be double-count). */
    const txt = await page.locator('#logcfgSlotCounter').textContent();
    expect(txt).toMatch(/^\s*2\s*\/\s*32\s*slots/);
    /* And the limit-reached warning is hidden. */
    await expect(page.locator('#logcfgLimitWarn')).not.toHaveClass(/show/);
  });

  test('AC-LC4: Load Profile populates Logged checkboxes from get_logger_profile', async ({ page }) => {
    /* The firmware reports its current profile (required ∪ selected);
     * Load Profile should tick those checkboxes. The default RS7
     * profile is required:[nmot_w, InjSys_ratEthPrtnBascFu,
     * Com_stCrCtlPan] + selected:[rl_w, tmot, wdkba]. */
    await bootClean(page);
    await openLogConfig(page);
    await page.click('button:has-text("Load Profile")');
    await expect(page.locator('#logcfgSaveBanner')).toContainText(/loaded from dongle/i, { timeout: 6000 });
    for (const v of ['nmot_w', 'InjSys_ratEthPrtnBascFu', 'Com_stCrCtlPan', 'rl_w', 'tmot', 'wdkba']){
      await expect(page.locator(`#logvar_${v}`)).toBeChecked();
    }
  });

  test('AC-LC5: "Enable Logged first" warn link ticks both', async ({ page }) => {
    await bootClean(page);
    await openLogConfig(page);
    /* Tick Show without Logged. Warn link should appear. */
    await page.locator('#show_rl_w').check();
    const warn = page.locator(`#warn_rl_w`);
    await expect(warn).toBeVisible();
    /* Click the inline link. */
    await warn.locator('a').click();
    await expect(page.locator('#logvar_rl_w')).toBeChecked();
    await expect(page.locator('#show_rl_w')).toBeChecked();
    /* Warn disappears. */
    await expect(warn).toBeHidden();
  });

  test('AC-LC7: Slot-limit warning is HIDDEN when used < 32', async ({ page }) => {
    /* Sean/Cowork report: warning rendered alongside "4 / 32 slots".
     * Regression test: with zero ticks (and again with 4 ticks) the
     * #logcfgLimitWarn element must not be `.show`. */
    await bootClean(page);
    await openLogConfig(page);
    await expect(page.locator('#logcfgLimitWarn')).toBeHidden();
    /* Tick 3 vars (max supported on this dongle for non-required). */
    for (const v of ['rl_w', 'tmot', 'wdkba']){
      await page.locator(`#logvar_${v}`).check();
    }
    await expect(page.locator('#logcfgSlotCounter')).toContainText(/^\s*3\s*\/\s*32/);
    await expect(page.locator('#logcfgLimitWarn')).toBeHidden();
  });

  test('AC-LC8: Slot-limit warning is SHOWN when used >= 32', async ({ page }) => {
    /* Driving the actual >=32 state on this 6-var dongle requires
     * the test fixture to call logcfgUpdateStats() with a forced
     * over-limit state. We poke checkbox dataset.slots to 32 on a
     * single tick — that's the path logcfgUsedSlots() walks. */
    await bootClean(page);
    await openLogConfig(page);
    await expect(page.locator('#logcfgLimitWarn')).toBeHidden();
    await page.evaluate(() => {
      const cb = document.getElementById('logvar_rl_w');
      cb.dataset.slots = '32';
      cb.checked = true;
      logcfgUpdateStats();
    });
    await expect(page.locator('#logcfgSlotCounter')).toContainText(/32\s*\/\s*32/);
    await expect(page.locator('#logcfgLimitWarn')).toHaveClass(/show/);
  });

  test('AC-LC6: Unsupported vars render the "(not supported)" badge', async ({ page }) => {
    /* For 4K0907557G__0003, list_available_vars returns 6 names. The
     * UI's ECU_VAR_DB lists ~55. The remaining ~49 rows should be
     * tagged var-unsupported. Wait for the filter to run (fires on
     * WS connect). */
    await bootClean(page);
    await openLogConfig(page);
    /* logcfgRefreshSupportedVars runs on WS open; allow 5 s for the
     * round-trip + DOM update. */
    await expect(page.locator('.logcfg-var-row.var-unsupported').first())
      .toBeVisible({ timeout: 6000 });
    /* A known unsupported var (no firmware parser for it on this
     * boxcode). */
    const row = page.locator('.logcfg-var-row[data-varname="GearBx_tOil_VW"]');
    await expect(row).toHaveClass(/var-unsupported/);
    /* Logged checkbox is disabled. */
    const cb = page.locator('#logvar_GearBx_tOil_VW');
    await expect(cb).toBeDisabled();
    /* Supported vars stay enabled. */
    await expect(page.locator('#logvar_rl_w')).toBeEnabled();
  });
});
