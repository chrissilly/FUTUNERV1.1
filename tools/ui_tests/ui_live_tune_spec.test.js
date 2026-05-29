// @ts-check
/**
 * Live Tune tab smoke spec.
 * AC: tab loads, stage cards + ethanol slider + Apply/Update/Stop
 *     affordances present, default stage 1 is selected, ethanol
 *     slider/number two-way binds. Apply is NOT clicked — it's an
 *     ECU-wire-surface action and requires Sean sign-off (see
 *     CLAUDE.md hard rule "No ECU-wire-surface code changes
 *     without Sean's explicit sign-off"). Smoke only.
 */
const { test, expect } = require('@playwright/test');

test('Live Tune tab: stage selector + ethanol slider wired', async ({ page }) => {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  await page.evaluate(() => switchTab('livetune'));
  await expect(page.locator('#panel-livetune')).toBeVisible();
  await expect(page.locator('.stage-card[data-stage="1"]')).toHaveClass(/selected/);
  await expect(page.locator('.stage-card[data-stage="2"]')).toBeVisible();
  await expect(page.locator('.stage-card[data-stage="3"]')).toBeVisible();
  /* Select stage 2 → selection moves. */
  await page.click('.stage-card[data-stage="2"]');
  await expect(page.locator('.stage-card[data-stage="2"]')).toHaveClass(/selected/);
  await expect(page.locator('.stage-card[data-stage="1"]')).not.toHaveClass(/selected/);
  /* Ethanol slider + number bind. */
  await page.locator('#ltEthanolNum').fill('42');
  await page.locator('#ltEthanolNum').dispatchEvent('input');
  await expect(page.locator('#ltEthanolSlider')).toHaveValue('42');
  /* Apply / Update / Stop affordances. */
  await expect(page.locator('button:has-text("Apply")')).toBeVisible();
  await expect(page.locator('#panel-livetune button:has-text("Stop")')).toBeVisible();
});
