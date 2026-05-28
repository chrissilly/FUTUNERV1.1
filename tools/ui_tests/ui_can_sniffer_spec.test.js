// @ts-check
/**
 * CAN Sniffer tab acceptance tests (PRIORITY 5).
 *
 * Root cause of the cowork-reported "count stays at 0" bug:
 * `can_sniff_start` is admin-gated server-side; the UI fired it
 * without checking auth and the firmware returned "Authentication
 * required". The UI never surfaced that — the button looked dead.
 *
 * AC-SN1: Start without auth → red error toast, sniffing flag stays
 *         false, no frames captured.
 * AC-SN2: Start with auth (unlocked session) → at least one
 *         can_frame event lands in the body within 10 s. We inject
 *         a frame via can_send_raw so the test doesn't depend on a
 *         live bus.
 *
 * AC-SN2 is NIGHTLY-gated when ENGINE_OFF=1 — can_send_raw is a
 * write-side admin command and the cowork prod dongle may not have
 * the auth session unlocked under unattended Playwright. The auth-
 * guard test (AC-SN1) covers the regression path we actually
 * shipped.
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

test.describe('CAN Sniffer (PRIORITY 5)', () => {

  test('AC-SN1: Start without auth surfaces an error, does not silently fail', async ({ page }) => {
    /* Capture toast pop-ups by snooping the toast() function. */
    await boot(page);
    await page.evaluate(() => {
      window.__toasts = [];
      const orig = window.toast;
      window.toast = (msg, kind) => { window.__toasts.push({msg, kind}); return orig ? orig(msg, kind) : null; };
    });
    await openSniffer(page);
    /* `authenticated` is module-scoped, so we can't poke it from
     * outside. It starts false on page load — that's exactly the
     * regression case. Hit Start directly. */
    await page.locator('#panel-sniffer button:has-text("Start")').click();
    /* Either an "Authenticate first" toast (UI guarded) or the
     * firmware echoed "Authentication required" and the UI
     * surfaced it (response-handler guarded). Both are acceptable
     * — what's NOT acceptable is a silent no-op. */
    await page.waitForFunction(() => (window.__toasts || []).some(t =>
      /Authenticate|Authentication required/i.test(t.msg) && t.kind === 'error'
    ), { timeout: 3000 });
    /* The `sniffing` flag is module-scoped (not on window) so we
     * can't poke at it directly. The toast presence is the
     * observable customer signal; that's what we assert. */
  });

  test('AC-SN2: With auth, a can_frame event lands in the body @nightly', async ({ page }) => {
    test.skip(!NIGHTLY, 'NIGHTLY=1 required (admin auth + can_send_raw)');
    /* This one needs the unlocked session — Cowork sets the auth
     * password before invoking the nightly run. */
    await boot(page);
    await openSniffer(page);
    await page.click('#sniffStart');
    /* Inject a frame so we don't depend on a live bus. */
    await page.evaluate(() => wsSend({command:'can_send_raw', id:0x7E0, data:[0x02,0x10,0x01,0,0,0,0,0]}));
    /* Body rows should populate within 10 s. */
    const row = page.locator('#sniffBody tr').first();
    await expect(row).toBeVisible({ timeout: 10_000 });
  });
});
