// @ts-check
/**
 * DTC tab acceptance tests (P-77 fallback wording + P-78 FTB
 * preservation).
 *
 * AC-DT1 (live): Read DTCs round-trips and populates #dtcList
 *                within 5 s. DEV_ECU=1 gated — depends on the
 *                live RS7 returning at least one record.
 *
 * AC-DT2 (live): For any returned record with a non-zero FTB,
 *                the UI displays code + FTB suffix ("P0077.84").
 *                Two records with the same code but different
 *                FTB MUST render as visually distinct rows.
 *
 * AC-DT3 (fallback wording, synthetic): Inject a fake response
 *                with a SAE-standard P0xxx code NOT in the
 *                description table; assert the rendered text
 *                does NOT contain "manufacturer-specific".
 *
 * AC-DT4 (status chip vocab, synthetic): Inject a fake response
 *                with status = 0xED; assert chips for failed,
 *                pending, confirmed, since-clear, op-incomplete,
 *                mil are all rendered, AND the tooltip exposes
 *                the raw hex.
 */
const { test, expect } = require('@playwright/test');

const DEV_ECU = process.env.DEV_ECU === '1';

async function boot(page) {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
}

async function openDiag(page) {
  await page.evaluate(() => switchTab('diag'));
  await expect(page.locator('#panel-diag')).toBeVisible();
}

/* Inject a synthetic dtc_read response by directly calling the
 * handler. Bypasses the wire round-trip so we can test UI logic
 * deterministically. */
async function injectDtcResponse(page, codes) {
  await page.evaluate((codes) => {
    onDtcReadResp({
      command: 'dtc_read',
      success: true,
      ok: true,
      codes,
    });
  }, codes);
}

test.describe('DTC tab', () => {

  test('AC-DT1 + AC-DT2: live read renders code + FTB suffix @dev-ecu', async ({ page }) => {
    test.skip(!DEV_ECU, 'DEV_ECU=1 required (live RS7 dependency)');
    await boot(page);
    await openDiag(page);
    await page.click('button:has-text("Read DTCs")');
    /* List populates within 5 s. */
    const firstItem = page.locator('#dtcList .dtc-item').first();
    await expect(firstItem).toBeVisible({ timeout: 5000 });
    /* RS7 known to return P0077 with FTBs 0x84 + 0x89 — both
     * must render as distinct, neither as bare "P0077". */
    const codes = await page.locator('#dtcList .dtc-code').allTextContents();
    /* Codes with non-zero FTB carry a ".XX" suffix. */
    const ftbBearing = codes.filter(c => /\.[0-9A-F]{2}$/.test(c));
    expect(ftbBearing.length).toBeGreaterThan(0);
    /* No two visible rows share the same code+FTB display string. */
    expect(new Set(codes).size).toBe(codes.length);
  });

  test('AC-DT3: SAE-standard miss does NOT show "manufacturer-specific"', async ({ page }) => {
    await boot(page);
    await openDiag(page);
    /* P0X99 is a SAE-prefix code guaranteed not to be in the
     * description table (it's not even valid SAE, but the
     * fallback router still classifies by P0xxx prefix as
     * SAE-defined). */
    await injectDtcResponse(page, [
      { code: 'P0X99', ftb: 0, status: 0x08, description: '(no description in database — SAE J2012 code)' },
    ]);
    const text = await page.locator('#dtcList').textContent();
    expect(text).not.toMatch(/manufacturer-specific/i);
    expect(text).toMatch(/no description in database/i);
  });

  test('AC-DT4: status 0xED renders the full chip vocab + tooltip exposes hex', async ({ page }) => {
    await boot(page);
    await openDiag(page);
    await injectDtcResponse(page, [
      { code: 'P0077', ftb: 0x84, status: 0xED, description: 'O2 Sensor Heater Control Circuit' },
    ]);
    /* Display string includes ".84" FTB suffix. */
    await expect(page.locator('#dtcList .dtc-code')).toContainText('P0077.84');
    /* Each set bit in 0xED produces its own chip. 0xED bits:
     * 0x01 testFailed, 0x04 pending, 0x08 confirmed,
     * 0x20 failedSinceClear, 0x40 notCompletedThisOpCycle,
     * 0x80 warningIndicator. */
    for (const cls of ['failed', 'pending', 'confirmed', 'since-clear', 'op-incomplete', 'mil']) {
      await expect(page.locator(`.dtc-status-chip.${cls}`)).toHaveCount(1);
    }
    /* Code element title tooltip carries the raw status hex. */
    const titleAttr = await page.locator('#dtcList .dtc-code').getAttribute('title');
    expect(titleAttr).toContain('0xED');
  });
});
