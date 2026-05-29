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

/* P-75: clean up any leftover named profiles from prior runs so
 * overwrite-confirm dialogs + rename-already-exists errors don't
 * leak across runs. Safe — only touches names this suite creates. */
const LC_TEST_PROFILES = [
  'ac_lc1_test', 'ac_lc4_alpha', 'ac_lc9_test',
  'ac_lc10_demo', 'ac_lc11_a', 'ac_lc11_b',
  'ac_lc12_doomed',
  'ac_lc13_orig', 'ac_lc13_renamed',
];
async function cleanLogConfigProfiles(page) {
  await page.evaluate((names) => Promise.all(names.map(n => new Promise(resolve => {
    wsSend({command:'delete_logger_profile', params:{name:n}}, () => resolve());
  }))), LC_TEST_PROFILES);
}

test.describe('Log Config tab (PRIORITY 1)', () => {
  /* Auto-accept any confirm()/prompt() dialogs so tests that
   * trigger overwrite or rename UI flows don't deadlock.
   * Bootstrap a clean WS context THEN nuke any leftover profiles
   * this suite uses, so re-runs don't trip overwrite / rename
   * collisions. */
  test.beforeEach(async ({ page }) => {
    page.on('dialog', d => d.accept(''));
    await page.goto('/?cb=playwright-prelude');
    await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
    await cleanLogConfigProfiles(page);
  });

  test('AC-LC1: Save to Dongle with name → green banner + firmware accepts {name, variables[]}', async ({ page }) => {
    /* P-75 redesign: save requires a name. UI gates locally;
     * firmware re-validates. Wire envelope:
     *   {command:'set_logger_profile', params:{name, variables:[...]}}
     */
    await bootClean(page);
    await openLogConfig(page);
    for (const v of ['rl_w', 'tmot', 'wdkba']){
      await page.locator(`#logvar_${v}`).check();
    }
    await page.locator('#logcfgProfileName').fill('ac_lc1_test');
    await page.evaluate(() => {
      window.__sentFrames = [];
      const orig = window.wsSend;
      window.wsSend = (obj, cb) => {
        try { window.__sentFrames.push(JSON.stringify(obj)); } catch (e) {}
        return orig(obj, cb);
      };
    });
    await page.locator('#panel-logconfig button:has-text("Save to Dongle")').click();
    await expect(page.locator('#logcfgSaveBanner')).toHaveClass(/show ok/, { timeout: 6000 });
    await expect(page.locator('#logcfgSaveBanner')).toContainText(/Saved "ac_lc1_test"/);
    const frames = await page.evaluate(() => window.__sentFrames || []);
    const setFrame = frames.find(s => s.includes('set_logger_profile'));
    expect(setFrame).toBeDefined();
    const parsed = JSON.parse(setFrame);
    expect(parsed.params).toBeDefined();
    expect(parsed.params.name).toBe('ac_lc1_test');
    expect(parsed.params.variables).toEqual(expect.arrayContaining(['rl_w', 'tmot', 'wdkba']));
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

  test('AC-LC4: Load from dropdown populates Logged checkboxes', async ({ page }) => {
    /* P-75: Load Profile button replaced by dropdown + Load button.
     * Save a known profile first, then pick from dropdown + click
     * Load, then assert checkboxes match what was saved. */
    await bootClean(page);
    await openLogConfig(page);
    /* Save "ac_lc4_alpha" with {rl_w, wdkba}. */
    await page.locator('#logvar_rl_w').check();
    await page.locator('#logvar_wdkba').check();
    await page.locator('#logcfgProfileName').fill('ac_lc4_alpha');
    await page.locator('#panel-logconfig button:has-text("Save to Dongle")').click();
    await expect(page.locator('#logcfgSaveBanner')).toHaveClass(/show ok/, { timeout: 6000 });
    /* Deselect everything, then load via dropdown. */
    await page.locator('button:has-text("Deselect All")').click();
    await expect(page.locator('#logvar_rl_w')).not.toBeChecked();
    /* Wait for dropdown to populate (refresh fires after save). */
    await expect(page.locator('#logcfgProfileSelect option[value="ac_lc4_alpha"]')).toHaveCount(1, { timeout: 6000 });
    await page.locator('#logcfgProfileSelect').selectOption('ac_lc4_alpha');
    await page.locator('#panel-logconfig button:has-text("Load")').first().click();
    /* Wait for the Load round-trip to complete — banner shows
     * "Loaded ..." once load_logger_profile + get_logger_profile
     * both come back. */
    await expect(page.locator('#logcfgSaveBanner'))
      .toContainText(/Loaded "ac_lc4_alpha"/, { timeout: 8000 });
    /* Required + saved both come back checked. */
    for (const v of ['nmot_w', 'InjSys_ratEthPrtnBascFu', 'Com_stCrCtlPan', 'rl_w', 'wdkba']){
      await expect(page.locator(`#logvar_${v}`)).toBeChecked();
    }
    /* tmot was not in this profile — should NOT be checked. */
    await expect(page.locator('#logvar_tmot')).not.toBeChecked();
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

  test('AC-LC9: Save non-default profile persists, no duplicate inserts', async ({ page }) => {
    /* P-72: regression. The previous race between cmd_set_logger_profile's
     * apply (WS task) and conn_manager_update's apply (can_task) corrupted
     * logger_manager with duplicate inserts that took a full CONN_MGR
     * cascade to settle. Fix defers the apply to can_task via
     * logger_manager_force_reconfigure() so it runs single-threaded.
     *
     * Discriminators (each fails on the racy build, passes on the fix):
     *  1. The save response no longer includes total_active (dropped).
     *  2. saved_count equals the var count sent (1 for [wdkba]).
     *  3. No get_logger_profile call in the seconds after save EVER
     *     returns a selected array with duplicates. (Pre-fix race
     *     produces ["wdkba","wdkba"] during the cascade window; post-
     *     fix the apply runs once, so no duplicates can appear.) */
    await bootClean(page);
    await openLogConfig(page);
    for (const v of ['rl_w', 'tmot']){
      await page.locator(`#logvar_${v}`).uncheck();
    }
    await page.locator('#logvar_wdkba').check();
    /* Snoop the save response shape. */
    const saveResp = await page.evaluate(() => new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject('timeout'), 6000);
      wsSend({command:'set_logger_profile', params:{name:'ac_lc9_test', variables:['wdkba']}}, msg => {
        clearTimeout(timer);
        resolve({
          success: msg.success === true,
          saved_count: msg.saved_count,
          /* msg.data is the inner envelope from command_handler. */
          has_total_active_inner: msg.data && 'total_active' in msg.data,
          has_total_active_top: 'total_active' in msg,
        });
      });
    }));
    expect(saveResp.success).toBe(true);
    expect(saveResp.saved_count).toBe(1);
    /* Discriminator 1: post-fix response drops total_active. */
    expect(saveResp.has_total_active_inner).toBe(false);
    expect(saveResp.has_total_active_top).toBe(false);
    /* Discriminator 3: poll get_logger_profile rapidly for ~3 s; if
     * ANY response has duplicates in `selected`, the race is alive. */
    const duplicates = await page.evaluate(() => new Promise(resolve => {
      const seen = [];
      const start = Date.now();
      const probe = () => {
        wsSend({command:'get_logger_profile'}, msg => {
          const sel = msg.selected || (msg.data && msg.data.selected) || [];
          const set = new Set(sel);
          if (sel.length !== set.size){
            seen.push(sel.slice());
          }
          if (Date.now() - start < 3000){
            setTimeout(probe, 100);
          } else {
            resolve(seen);
          }
        });
      };
      probe();
    }));
    expect(duplicates).toEqual([]);
  });

  test('AC-LC10: Save appends to dropdown + list_logger_profiles returns it', async ({ page }) => {
    /* P-75: saved name shows up in the dropdown options + on the
     * wire via list_logger_profiles within the refresh window. */
    await bootClean(page);
    await openLogConfig(page);
    await page.locator('#logvar_rl_w').check();
    await page.locator('#logcfgProfileName').fill('ac_lc10_demo');
    await page.locator('#panel-logconfig button:has-text("Save to Dongle")').click();
    await expect(page.locator('#logcfgSaveBanner')).toHaveClass(/show ok/, { timeout: 6000 });
    /* Dropdown picks up the new name. */
    await expect(page.locator('#logcfgProfileSelect option[value="ac_lc10_demo"]')).toHaveCount(1, { timeout: 6000 });
    /* Active tag updates. */
    await expect(page.locator('#logcfgActiveTag')).toContainText(/ac_lc10_demo/);
    /* Direct probe: list_logger_profiles also returns it. */
    const listResp = await page.evaluate(() => new Promise(resolve => {
      wsSend({command:'list_logger_profiles'}, msg => resolve(msg));
    }));
    expect(listResp.success).toBe(true);
    const names = (listResp.profiles || []).map(p => p.name);
    expect(names).toContain('ac_lc10_demo');
    expect(listResp.active).toBe('ac_lc10_demo');
  });

  test('AC-LC11: Load swaps active to the picked profile', async ({ page }) => {
    /* Save two named profiles, then Load the first one and verify
     * the firmware-side active flips. */
    await bootClean(page);
    await openLogConfig(page);
    /* Profile A: rl_w only */
    await page.locator('#logvar_rl_w').check();
    await page.locator('#logcfgProfileName').fill('ac_lc11_a');
    await page.locator('#panel-logconfig button:has-text("Save to Dongle")').click();
    await expect(page.locator('#logcfgSaveBanner')).toHaveClass(/show ok/, { timeout: 6000 });
    /* Profile B: tmot only */
    await page.locator('button:has-text("Deselect All")').click();
    await page.locator('#logvar_tmot').check();
    await page.locator('#logcfgProfileName').fill('ac_lc11_b');
    await page.locator('#panel-logconfig button:has-text("Save to Dongle")').click();
    await expect(page.locator('#logcfgSaveBanner')).toHaveClass(/show ok/, { timeout: 6000 });
    /* B is the most-recently-saved → active. Now Load A. */
    await expect(page.locator('#logcfgActiveTag')).toContainText(/ac_lc11_b/);
    await page.locator('#logcfgProfileSelect').selectOption('ac_lc11_a');
    await page.locator('#panel-logconfig button:has-text("Load")').first().click();
    await expect(page.locator('#logcfgSaveBanner')).toContainText(/Loaded "ac_lc11_a"/, { timeout: 6000 });
    /* Probe — active marker on the dongle now points at A. */
    const status = await page.evaluate(() => new Promise(resolve => {
      wsSend({command:'get_logger_profile'}, msg => resolve(msg));
    }));
    expect(status.active_name).toBe('ac_lc11_a');
  });

  test('AC-LC12: Delete removes profile from dropdown + dongle', async ({ page }) => {
    await bootClean(page);
    await openLogConfig(page);
    await page.locator('#logvar_rl_w').check();
    await page.locator('#logcfgProfileName').fill('ac_lc12_doomed');
    await page.locator('#panel-logconfig button:has-text("Save to Dongle")').click();
    await expect(page.locator('#logcfgProfileSelect option[value="ac_lc12_doomed"]')).toHaveCount(1, { timeout: 6000 });
    /* Direct WS delete (avoids the confirm() dialog the button uses). */
    const delResp = await page.evaluate(() => new Promise(resolve => {
      wsSend({command:'delete_logger_profile', params:{name:'ac_lc12_doomed'}}, msg => resolve(msg));
    }));
    expect(delResp.success).toBe(true);
    /* Re-list to refresh dropdown. */
    await page.evaluate(() => logcfgRefreshProfileList());
    await expect(page.locator('#logcfgProfileSelect option[value="ac_lc12_doomed"]')).toHaveCount(0, { timeout: 6000 });
  });

  test('AC-LC13: Rename moves the profile in the dropdown + tracks active', async ({ page }) => {
    await bootClean(page);
    await openLogConfig(page);
    await page.locator('#logvar_rl_w').check();
    await page.locator('#logcfgProfileName').fill('ac_lc13_orig');
    await page.locator('#panel-logconfig button:has-text("Save to Dongle")').click();
    await expect(page.locator('#logcfgProfileSelect option[value="ac_lc13_orig"]')).toHaveCount(1, { timeout: 6000 });
    /* Direct WS rename (avoids prompt() dialog). */
    const renResp = await page.evaluate(() => new Promise(resolve => {
      wsSend({command:'rename_logger_profile',
              params:{old_name:'ac_lc13_orig', new_name:'ac_lc13_renamed'}},
             msg => resolve(msg));
    }));
    expect(renResp.success).toBe(true);
    await page.evaluate(() => logcfgRefreshProfileList());
    await expect(page.locator('#logcfgProfileSelect option[value="ac_lc13_renamed"]')).toHaveCount(1, { timeout: 6000 });
    await expect(page.locator('#logcfgProfileSelect option[value="ac_lc13_orig"]')).toHaveCount(0);
    /* Active marker tracked the rename. */
    const status = await page.evaluate(() => new Promise(resolve => {
      wsSend({command:'get_logger_profile'}, msg => resolve(msg));
    }));
    expect(status.active_name).toBe('ac_lc13_renamed');
  });

  test('AC-LC14: Invalid profile names are rejected client-side + server-side', async ({ page }) => {
    await bootClean(page);
    await openLogConfig(page);
    await page.locator('#logvar_rl_w').check();
    /* Empty name → banner error, no WS frame. */
    await page.locator('#logcfgProfileName').fill('');
    await page.evaluate(() => {
      window.__sentSet = [];
      const orig = window.wsSend;
      window.wsSend = (obj, cb) => {
        if (obj.command === 'set_logger_profile') window.__sentSet.push(JSON.stringify(obj));
        return orig(obj, cb);
      };
    });
    await page.locator('#panel-logconfig button:has-text("Save to Dongle")').click();
    await expect(page.locator('#logcfgSaveBanner')).toHaveClass(/show err/);
    expect(await page.evaluate(() => window.__sentSet)).toEqual([]);
    /* Server-side: a direct WS send with an invalid name (slash) is
     * rejected. */
    const r = await page.evaluate(() => new Promise(resolve => {
      wsSend({command:'set_logger_profile', params:{name:'has/slash', variables:[]}},
             msg => resolve(msg));
    }));
    expect(r.success).toBe(false);
    expect(r.message).toMatch(/invalid 'name'/i);
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
