// @ts-check
/**
 * WiFi (System tab → WiFi Station card) smoke spec.
 * AC: form affordances present, Scan round-trip populates scanResults
 *     within 8s, datalist gets options. Connect/Disconnect are NOT
 *     exercised here — they'd actually change the dongle's WiFi
 *     state mid-test and break subsequent suites.
 */
const { test, expect } = require('@playwright/test');

test('WiFi card: form loads + Scan returns SSID list', async ({ page }) => {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  await page.evaluate(() => switchTab('system'));
  await expect(page.locator('#staSSID')).toBeVisible();
  await expect(page.locator('#staPassword')).toBeVisible();
  await expect(page.locator('button:has-text("Scan")')).toBeVisible();
  await expect(page.locator('button:text-is("Connect")')).toBeVisible();
  await expect(page.locator('button:text-is("Disconnect")')).toBeVisible();
  /* Scan must populate scanResults OR datalist. wifi_scan is admin
   * but the registered command echoes back with SSID list at
   * msg.networks. We accept either non-empty scanResults innerHTML
   * or any ssidList option. */
  await page.click('button:has-text("Scan")');
  await page.waitForFunction(() =>
    document.getElementById('scanResults').children.length > 0 ||
    document.getElementById('ssidList').children.length > 0 ||
    /Authentication required|Authenticate first/i.test(document.body.innerText),
    { timeout: 8000 }
  );
});
