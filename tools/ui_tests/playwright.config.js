// @ts-check
/**
 * Playwright config for the FUTUNER dongle UI tests.
 *
 * Drives the real browser against the dongle's served HTML at
 * DONGLE_URL (default http://10.188.195.232). The dongle must be on
 * STA + paired to a paid VIN for most ACs to pass.
 *
 * Single worker on purpose — there's exactly one dongle and these
 * tests mutate its WS state (logger_start, wot_log_start, etc.).
 *
 * Env knobs:
 *   DONGLE_URL=http://x.x.x.x      override dongle base URL
 *   DEBUG=1                        run headed instead of headless
 *   NIGHTLY=1                      enable AC10 (60-minute soak)
 */

const { defineConfig } = require('@playwright/test');

const BASE_URL = process.env.DONGLE_URL || 'http://10.188.195.232';
const HEADED   = process.env.DEBUG === '1';

module.exports = defineConfig({
  testDir: '.',
  testMatch: /.*\.test\.js$/,
  timeout: 30_000,
  retries: 1,
  workers: 1,                       /* one dongle, no parallelism */
  fullyParallel: false,
  reporter: [['list']],
  use: {
    baseURL: BASE_URL,
    headless: !HEADED,
    actionTimeout: 8_000,
    navigationTimeout: 15_000,
    trace: 'retain-on-failure',
  },
  projects: [
    {
      name: 'chromium',
      use: { browserName: 'chromium' },
    },
  ],
});
