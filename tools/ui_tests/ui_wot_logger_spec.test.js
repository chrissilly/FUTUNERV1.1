// @ts-check
/**
 * WOT Logger tab smoke spec.
 * AC: tab loads, Start/Stop affordances present, the 4 stat boxes
 *     render. Start round-trip is feature-manager-gated; we just
 *     assert the button is wired and the click produces SOME toast/
 *     banner (success or "another feature active").
 */
const { test, expect } = require('@playwright/test');

test('WOT Logger tab: load + Start affordance fires a response', async ({ page }) => {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  await page.evaluate(() => switchTab('wot'));
  await expect(page.locator('#panel-wot')).toBeVisible();
  /* 4 stat tiles. */
  await expect(page.locator('#wotQueued')).toBeVisible();
  await expect(page.locator('#wotBytes')).toBeVisible();
  await expect(page.locator('#wotLastResult')).toBeVisible();
  await expect(page.locator('#wotLastTs')).toBeVisible();
  /* Start + Stop. */
  await expect(page.locator('button:has-text("Start WOT Logging")')).toBeVisible();
  await expect(page.locator('#panel-wot button:has-text("Stop")')).toBeVisible();
  /* Snoop toast / wsSend to confirm the click actually attempts a
   * round-trip (whether the firmware accepts or rejects is feature-
   * manager-state-dependent and out of scope for the smoke test). */
  await page.evaluate(() => {
    window.__wotFrames = [];
    const orig = window.wsSend;
    window.wsSend = (obj, cb) => { window.__wotFrames.push(JSON.stringify(obj)); return orig(obj, cb); };
  });
  await page.click('button:has-text("Start WOT Logging")');
  const frames = await page.evaluate(() => window.__wotFrames || []);
  /* wotStart() invokes doWotStart() which sends {command:'wot_log_start'}
   * when no other feature is active. confirmFeatureSwap returns true
   * for idle → fall-through. */
  expect(frames.some(f => /wot_log_start/.test(f))).toBe(true);
});
