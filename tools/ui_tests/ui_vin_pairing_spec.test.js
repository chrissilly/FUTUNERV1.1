// @ts-check
/**
 * VIN Pairing tab smoke spec.
 * AC: tab loads, Pair-VIN affordance present, Refresh License round-trip
 *     completes and updates the grid (license_status comes back with
 *     paid:true on this dongle's known-good token).
 */
const { test, expect } = require('@playwright/test');

test('VIN Pairing tab: load + license refresh round-trip', async ({ page }) => {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  await page.evaluate(() => switchTab('vinpair'));
  await expect(page.locator('#panel-vinpair')).toBeVisible();
  await expect(page.locator('button:has-text("Pair VIN now")')).toBeVisible();
  await expect(page.locator('button:has-text("Refresh License")')).toBeVisible();
  /* license_status round-trip — the grid is populated on response. */
  await page.click('button:has-text("Refresh License")');
  /* vinpairGrid gets innerHTML on the response; just wait for it to
   * have at least one child. */
  await expect(page.locator('#vinpairGrid > *').first()).toBeVisible({ timeout: 6000 });
});
