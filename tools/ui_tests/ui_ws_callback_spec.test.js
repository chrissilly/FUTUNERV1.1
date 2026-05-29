// @ts-check
/**
 * WS callback infrastructure smoke spec.
 * Verifies the per-command callback dispatch in wsSend's pendingCb
 * queue resolves cb correctly when the firmware echoes back. This
 * is the core mechanism every per-tab feature depends on; if it
 * regresses, every feature breaks silently. Regression catch is
 * a hand-rolled callback against a read-only command
 * (license_status) and assertion that the cb fires with the
 * post-envelope-unwrap data.
 */
const { test, expect } = require('@playwright/test');

test('wsSend cb fires with unwrapped data on license_status', async ({ page }) => {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  /* Inject a probe: send license_status with a cb, capture the
   * resolved msg shape. */
  const probed = await page.evaluate(() => new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject('timeout'), 6000);
    wsSend({command:'license_status'}, msg => {
      clearTimeout(timer);
      resolve({
        hasCommand: typeof msg.command === 'string',
        success: msg.success === true,
        /* Post-unwrap: paid/vin appear at top level. */
        hasPaid: 'paid' in msg,
        hasVin: 'vin' in msg,
        /* Original data envelope should ALSO still be present. */
        hasData: typeof msg.data === 'object',
      });
    });
  }));
  expect(probed.hasCommand).toBe(true);
  expect(probed.success).toBe(true);
  expect(probed.hasPaid).toBe(true);
  expect(probed.hasData).toBe(true);
});
