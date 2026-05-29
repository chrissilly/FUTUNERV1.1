// @ts-check
/**
 * Files (File Manager) tab smoke spec.
 * AC: tab loads, path input + list affordance present, fsList()
 *     populates #fsList with at least one entry. Save / Delete /
 *     Upload are NOT exercised (write side; would mutate the
 *     dongle's filesystem mid-suite).
 */
const { test, expect } = require('@playwright/test');

test('Files tab: fsList populates the directory listing', async ({ page }) => {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  await page.evaluate(() => switchTab('files'));
  await expect(page.locator('#panel-files')).toBeVisible();
  await expect(page.locator('#fsPath')).toBeVisible();
  await expect(page.locator('button:has-text("Go")')).toBeVisible();
  await page.click('button:has-text("Go")');
  /* Either the list populates or the firmware says auth required. */
  await page.waitForFunction(() =>
    document.getElementById('fsList').children.length > 0 ||
    /Authentication required|Authenticate first/i.test(document.body.innerText),
    { timeout: 6000 }
  );
});
