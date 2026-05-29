// @ts-check
/**
 * P-75 regression — command auth gate is gone.
 *
 * Pre-fix: a curated set of commands (clear_errors, reboot,
 * wifi_sta_set, fs_list, can_sniff_start, etc.) returned
 * {success:false, message:"Authentication required"} until the
 * client sent {command:'unlock', password:...}.
 *
 * Post-fix: every command in COMMAND_REGISTRY is UNSECURED. A
 * fresh WS session must be able to call any of them and receive
 * either success or a content-level rejection (NRC from the ECU,
 * "Feature blocked by feature_manager", etc.) — but never
 * "Authentication required".
 *
 * Test strategy: fire one previously-SECURED command from each
 * subsystem cluster and assert the response message does NOT
 * match /Authentication required/. Pick commands whose execution
 * is reversible / harmless (clear_errors is no-op on empty error
 * log; fs_list /data/profiles just reads; can_sniff_status is
 * pure read).
 */
const { test, expect } = require('@playwright/test');

const PROBES = [
  /* Each: [WS command name, params object]. Picked from the
   * pre-P-75 SECURED list — see firmware/src/commands/commands.c
   * git history at HEAD~1. */
  ['clear_errors',     {}],
  ['fs_list',          {path: '/data'}],
  ['wifi_sta_set',     {ssid: 'p75_probe', password: 'p75_probe'}],
  ['can_sniff_start',  {}],
  ['can_sniff_stop',   {}],
];

test('AC-NOAUTH: previously-SECURED commands return no "Authentication required"', async ({ page }) => {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });

  for (const [cmd, params] of PROBES) {
    const resp = await page.evaluate(({c, p}) => new Promise(resolve => {
      const payload = Object.keys(p).length ? {command: c, params: p} : {command: c};
      wsSend(payload, msg => resolve({command: c, message: msg.message || '', success: msg.success}));
    }), {c: cmd, p: params});
    /* Post-P-75, this message must NEVER appear regardless of
     * whether the command itself succeeds or fails. */
    expect(resp.message).not.toMatch(/Authentication required/i);
  }
});

test('AC-NOAUTH-UI: lock indicator + Unlock button are gone', async ({ page }) => {
  await page.goto('/?cb=playwright');
  await expect(page.locator('#connLabel')).toHaveText('Connected', { timeout: 12_000 });
  /* The lock UI lived at the top-right of the header. Confirm
   * the IDs the old UI used are absent from the DOM. */
  await expect(page.locator('#lockIcon')).toHaveCount(0);
  await expect(page.locator('#authPass')).toHaveCount(0);
  await expect(page.locator('#authBtn')).toHaveCount(0);
  await expect(page.locator('#authStatus')).toHaveCount(0);
  /* And the "Unlock" button is not present anywhere. */
  await expect(page.locator('button:has-text("Unlock")')).toHaveCount(0);
});
