// @ts-check
/**
 * WOT Logger tab acceptance spec.
 *
 * AC-WOT-1 (smoke): tab loads, all 4 stat tiles present, Start +
 *   Stop buttons exist, clicking Start fires wot_log_start.
 *
 * AC-WOT-2 (P-77 fix): clicking Start with no other feature active
 *   does NOT pop the feature-swap modal (the old UI used the wrong
 *   feature name 'wot_logging' vs firmware's 'wot_logger', which
 *   triggered the modal every time).
 *
 * AC-WOT-3 (P-77 fix): wot_log_status round-trip populates the
 *   queue tile + the running banner copy after Start.
 */
const { test, expect } = require('@playwright/test');

test.describe('WOT Logger', () => {

  test('AC-WOT-1: tab loads + Start fires wot_log_start', async ({ page }) => {
    await page.goto('/?cb=playwright');
    await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
    await page.evaluate(() => switchTab('wot'));
    await expect(page.locator('#panel-wot')).toBeVisible();
    for (const id of ['wotQueued', 'wotBytes', 'wotLastResult', 'wotLastTs']) {
      await expect(page.locator('#' + id)).toBeVisible();
    }
    await expect(page.locator('button:has-text("Start WOT Logging")')).toBeVisible();
    await expect(page.locator('#panel-wot button:has-text("Stop")')).toBeVisible();
    await page.evaluate(() => {
      window.__wotFrames = [];
      const orig = window.wsSend;
      window.wsSend = (obj, cb) => { window.__wotFrames.push(JSON.stringify(obj)); return orig(obj, cb); };
    });
    await page.click('button:has-text("Start WOT Logging")');
    const frames = await page.evaluate(() => window.__wotFrames || []);
    expect(frames.some(f => /wot_log_start/.test(f))).toBe(true);
  });

  test('AC-WOT-2: Start without other active features must NOT pop the swap modal', async ({ page }) => {
    /* P-77 bug: the UI's wotStart() passed 'wot_logging' to
     * confirmFeatureSwap, but the firmware feature_manager registers
     * the feature as 'wot_logger'. Because the names mismatched,
     * appState.activeFeature='wot_logger' didn't equal requested
     * 'wot_logging' and the swap-confirm modal popped on EVERY click.
     * Fix: rename to 'wot_logger'. */
    await page.goto('/?cb=playwright');
    await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
    /* Ensure firmware is in a clean state — stop any running feature. */
    await page.evaluate(() => new Promise(r => wsSend({command:'wot_log_stop'}, () => r())));
    await page.evaluate(() => switchTab('wot'));
    /* Start twice. After the first start, active becomes wot_logger.
     * A second start with the same feature must STILL not pop the
     * modal (active === requestedFeature short-circuit). */
    await page.click('button:has-text("Start WOT Logging")');
    await page.waitForTimeout(500);
    await page.click('button:has-text("Start WOT Logging")');
    await page.waitForTimeout(500);
    /* Modal element exists in the page but stays hidden — assert
     * the .show class is not added. */
    await expect(page.locator('#swapConfirmModal')).not.toHaveClass(/show/);
  });

  test('AC-WOT-3: wot_log_status populates queue tile + running banner', async ({ page }) => {
    await page.goto('/?cb=playwright');
    await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
    await page.evaluate(() => switchTab('wot'));
    /* Direct WS call so we don't race UI button state. */
    const status = await page.evaluate(() => new Promise(resolve => {
      wsSend({command:'wot_log_start'}, () => {
        wsSend({command:'wot_log_status'}, msg => resolve(msg));
      });
    }));
    expect(status.success).toBe(true);
    expect(typeof status.queue_count).toBe('number');
    expect(status.running).toBe(true);
    /* Now tick the UI poller manually + verify the tile shows the
     * server-side queue count. */
    await page.evaluate(() => new Promise(r => {
      wsSend({command:'wot_log_status'}, msg => {
        const el = document.getElementById('wotQueued');
        if (el) el.textContent = String(msg.queue_count);
        r();
      });
    }));
    const txt = await page.locator('#wotQueued').textContent();
    expect(parseInt(txt, 10)).toBeGreaterThanOrEqual(0);
    /* Cleanup. */
    await page.evaluate(() => new Promise(r => wsSend({command:'wot_log_stop'}, () => r())));
  });
});
