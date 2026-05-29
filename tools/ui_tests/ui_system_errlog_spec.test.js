// @ts-check
/**
 * System tab smoke spec — sys grid + (Pair ECU / Remove Pairing /
 * Reboot) affordances + STEP 3B post-rename verification (buttons
 * say "Pair ECU" / "Remove Pairing", matching the WS commands they
 * fire — Rule 9 doc-vs-code-surface discipline at the button label
 * layer).
 *
 * Reboot is NOT clicked — it would knock the dongle off the wire
 * mid-suite.
 */
const { test, expect } = require('@playwright/test');

test('System tab: status + button labels match WS commands', async ({ page }) => {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  await page.evaluate(() => switchTab('system'));
  await expect(page.locator('#panel-system')).toBeVisible();
  /* Refresh populates #sysGrid. */
  await page.click('#panel-system button:has-text("Refresh")');
  await expect(page.locator('#sysGrid > *').first()).toBeVisible({ timeout: 6000 });
  /* STEP 3B verification — button text matches WS command path. */
  await expect(page.locator('button:has-text("Pair ECU")')).toBeVisible();
  await expect(page.locator('button:has-text("Remove Pairing")')).toBeVisible();
  /* Old labels MUST NOT appear. */
  await expect(page.locator('button:has-text("Pair Vehicle")')).toHaveCount(0);
  await expect(page.locator('button:has-text("Unpair Vehicle")')).toHaveCount(0);
  /* Reboot button present but not clicked. */
  await expect(page.locator('button:has-text("Reboot")')).toBeVisible();
});
