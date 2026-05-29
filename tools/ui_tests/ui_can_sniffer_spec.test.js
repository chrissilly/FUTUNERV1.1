// @ts-check
/**
 * CAN Sniffer tab acceptance tests.
 *
 * P-75 removed the dongle-command auth gate. AC-SN1 (the
 * "Authentication required" silent-fail regression test) is
 * deleted — the behavior it gates is gone. AC-SN2 stays as the
 * nightly with-live-frame test and no longer needs an auth setup.
 */

const { test, expect } = require('@playwright/test');

const NIGHTLY = process.env.NIGHTLY === '1';

async function boot(page) {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
}

async function openSniffer(page) {
  await page.evaluate(() => switchTab('sniffer'));
  await expect(page.locator('#panel-sniffer')).toBeVisible();
}

test.describe('CAN Sniffer', () => {
  test('AC-SN2: a can_frame event lands in the body @nightly', async ({ page }) => {
    test.skip(!NIGHTLY, 'NIGHTLY=1 required (can_send_raw needs a quiet bench)');
    await boot(page);
    await openSniffer(page);
    await page.locator('#panel-sniffer button:has-text("Start")').click();
    /* Inject a frame so we don't depend on a live bus. */
    await page.evaluate(() => wsSend({command:'can_send_raw',
                                       params:{id:0x7E0, data:[0x02,0x10,0x01,0,0,0,0,0]}}));
    const row = page.locator('#sniffBody tr').first();
    await expect(row).toBeVisible({ timeout: 10_000 });
  });
});
