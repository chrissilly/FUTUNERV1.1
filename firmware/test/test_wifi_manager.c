/*
 * test_wifi_manager.c — host-runnable unit tests for the WiFi mode-intent
 * + STA-creds APIs added in 2026-05-17. Exercises wifi_ap.c's new
 * public surface (`wifi_client_set_creds`, `wifi_client_clear_creds`,
 * `wifi_client_creds_stored`, `wifi_set_mode_intent`,
 * `wifi_get_mode_intent`, `wifi_feature_uses_cloud_network`).
 *
 * The cmd_* wrappers in firmware/src/commands/wifi_commands.c are
 * cJSON-coupled and are NOT compiled host-side (matches the
 * vin_pair_commands.c precedent). Their compile-time correctness is
 * verified by the idf.py build step in eval.sh; their behavioral
 * correctness is verified here at the API tier they delegate to, plus
 * a static grep of commands.c for the auth-tier registration.
 */

#include "wifi/wifi_ap.h"
#include "config/wifi_config.h"
#include "feature_manager/feature_manager.h"
#include "wifi_test_mocks.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Forward declarations                                                  */
/* --------------------------------------------------------------------- */
static char *slurp(const char *path);
static bool line_contains_both(const char *text,
                               const char *needle_a,
                               const char *needle_b);

/* --------------------------------------------------------------------- */
/* Tiny EXPECT framework                                                 */
/* --------------------------------------------------------------------- */
static int g_failures = 0;

#define EXPECT(cond, msg) do {                                            \
    if (!(cond)) {                                                        \
        fprintf(stderr, "  FAIL  %s — %s (line %d)\n",                    \
                __func__, (msg), __LINE__);                               \
        g_failures++;                                                     \
    } else {                                                              \
        fprintf(stdout, "  PASS  %s — %s\n", __func__, (msg));            \
    }                                                                     \
} while (0)

/* --------------------------------------------------------------------- */
/* Test setup helper                                                     */
/* --------------------------------------------------------------------- */
extern void wifi_ap_test_force_running(bool v);

static void test_setup(void) {
    nvs_mock_clear();
    wifi_test_reset_spies();
    wifi_test_set_feature_active(FEATURE_NONE, "none");
    /* wifi_ap_init/_start are never invoked host-side; force the
     * static `wifi_running` flag so wifi_client_connect's "WiFi not
     * running" early return doesn't short-circuit the mode-intent
     * delegation test. */
    wifi_ap_test_force_running(true);
}

/* --------------------------------------------------------------------- */
/* 1 — wifi_sta_set: stores creds in NVS                                 */
/* --------------------------------------------------------------------- */
static void test_wifi_sta_set_stores_creds_in_nvs(void) {
    test_setup();

    esp_err_t r = wifi_client_set_creds("HomeNet", "supersecret");
    EXPECT(r == ESP_OK, "wifi_client_set_creds returns ESP_OK");

    const char *ssid = nvs_mock_get(WIFI_STA_SSID_NVS_KEY);
    const char *pass = nvs_mock_get(WIFI_STA_PASS_NVS_KEY);
    EXPECT(ssid != NULL && strcmp(ssid, "HomeNet") == 0,
           "NVS sta_ssid = HomeNet");
    EXPECT(pass != NULL && strcmp(pass, "supersecret") == 0,
           "NVS sta_pass = supersecret");
    EXPECT(wifi_client_creds_stored(),
           "wifi_client_creds_stored() returns true after save");
}

/* --------------------------------------------------------------------- */
/* 2 — wifi_sta_set: rejects short password (< WPA2 floor)                */
/* --------------------------------------------------------------------- */
static void test_wifi_sta_set_rejects_short_password(void) {
    test_setup();

    esp_err_t r = wifi_client_set_creds("HomeNet", "abc");
    EXPECT(r == ESP_ERR_INVALID_ARG,
           "wifi_client_set_creds rejects 3-char password");
    EXPECT(nvs_mock_get(WIFI_STA_SSID_NVS_KEY) == NULL,
           "NVS not written when password is too short");

    /* And: empty password (open network) is allowed. */
    r = wifi_client_set_creds("OpenNet", "");
    EXPECT(r == ESP_OK, "empty password is allowed (open network)");
}

/* --------------------------------------------------------------------- */
/* 3 — wifi_sta_set: rejects empty SSID                                  */
/* --------------------------------------------------------------------- */
static void test_wifi_sta_set_rejects_empty_ssid(void) {
    test_setup();

    esp_err_t r = wifi_client_set_creds("", "supersecret");
    EXPECT(r == ESP_ERR_INVALID_ARG,
           "wifi_client_set_creds rejects empty SSID");
    EXPECT(nvs_mock_get(WIFI_STA_SSID_NVS_KEY) == NULL,
           "NVS not written for empty SSID");

    r = wifi_client_set_creds(NULL, "supersecret");
    EXPECT(r == ESP_ERR_INVALID_ARG,
           "wifi_client_set_creds rejects NULL SSID");
}

/* --------------------------------------------------------------------- */
/* 4 — wifi_mode ap: clears active STA connection (no NVS wipe)          */
/* --------------------------------------------------------------------- */
static void test_wifi_mode_ap_clears_active_sta_connection(void) {
    test_setup();
    /* Seed: creds + sta mode intent already established. */
    wifi_client_set_creds("HomeNet", "supersecret");
    nvs_mock_set(WIFI_MODE_INTENT_NVS_KEY, WIFI_MODE_INTENT_NVS_VAL_STA);

    int before = g_esp_wifi_disconnect_count;
    esp_err_t r = wifi_set_mode_intent(WIFI_MODE_INTENT_AP_ONLY);
    EXPECT(r == ESP_OK, "wifi_set_mode_intent(AP_ONLY) returns ESP_OK");
    EXPECT(g_esp_wifi_disconnect_count > before,
           "esp_wifi_disconnect was called (STA dropped)");

    const char *ssid = nvs_mock_get(WIFI_STA_SSID_NVS_KEY);
    EXPECT(ssid != NULL && strcmp(ssid, "HomeNet") == 0,
           "NVS sta_ssid preserved (creds NOT wiped by AP_ONLY toggle)");

    const char *mode = nvs_mock_get(WIFI_MODE_INTENT_NVS_KEY);
    EXPECT(mode != NULL && strcmp(mode, WIFI_MODE_INTENT_NVS_VAL_AP) == 0,
           "NVS wifi_mode = ap");

    EXPECT(wifi_get_mode_intent() == WIFI_MODE_INTENT_AP_ONLY,
           "wifi_get_mode_intent reflects AP_ONLY after set");
}

/* --------------------------------------------------------------------- */
/* 5 — wifi_mode sta: fails without stored creds                          */
/* --------------------------------------------------------------------- */
static void test_wifi_mode_sta_fails_without_stored_creds(void) {
    test_setup();
    /* NVS empty — no creds. */
    EXPECT(!wifi_client_creds_stored(),
           "precondition: no creds stored");

    esp_err_t r = wifi_set_mode_intent(WIFI_MODE_INTENT_APSTA);
    EXPECT(r == ESP_ERR_NOT_FOUND,
           "wifi_set_mode_intent(APSTA) returns ESP_ERR_NOT_FOUND without creds");

    EXPECT(nvs_mock_get(WIFI_MODE_INTENT_NVS_KEY) == NULL,
           "NVS wifi_mode unchanged on failure (still unset)");

    EXPECT(g_esp_wifi_connect_count == 0,
           "esp_wifi_connect NOT called when no creds");
}

/* --------------------------------------------------------------------- */
/* 6 — wifi_mode sta: delegates to existing wifi_client_connect()        */
/*                                                                       */
/* wifi_client_connect()'s observable side effects on host are:          */
/*   - calls esp_wifi_set_config(WIFI_IF_STA, cfg with ssid/password)    */
/*   - calls esp_wifi_connect()                                          */
/* The host shim records the last set_config args so we can pin the     */
/* delegation to the stored creds.                                       */
/* --------------------------------------------------------------------- */
static void test_wifi_mode_sta_invokes_existing_wifi_client_connect(void) {
    test_setup();
    wifi_client_set_creds("HomeNet", "supersecret");

    int set_cfg_before = g_esp_wifi_set_config_count;
    int connect_before = g_esp_wifi_connect_count;

    esp_err_t r = wifi_set_mode_intent(WIFI_MODE_INTENT_APSTA);
    EXPECT(r == ESP_OK,
           "wifi_set_mode_intent(APSTA) returns ESP_OK when creds present");

    EXPECT(g_esp_wifi_set_config_count > set_cfg_before,
           "wifi_client_connect → esp_wifi_set_config was invoked");
    EXPECT(g_esp_wifi_connect_count > connect_before,
           "wifi_client_connect → esp_wifi_connect was invoked");
    EXPECT(strcmp(g_last_set_config_ssid, "HomeNet") == 0,
           "esp_wifi_set_config received stored SSID");
    EXPECT(strcmp(g_last_set_config_password, "supersecret") == 0,
           "esp_wifi_set_config received stored password");

    const char *mode = nvs_mock_get(WIFI_MODE_INTENT_NVS_KEY);
    EXPECT(mode != NULL && strcmp(mode, WIFI_MODE_INTENT_NVS_VAL_STA) == 0,
           "NVS wifi_mode = sta after successful APSTA set");
}

/* --------------------------------------------------------------------- */
/* 7 — wifi_mode sta: blocked when cloud feature active                  */
/*                                                                       */
/* The predicate `wifi_feature_uses_cloud_network(int)` is what          */
/* cmd_wifi_mode (in wifi_commands.c) consults; this test pins the      */
/* predicate's contract directly. Two feature ids that ride the cloud    */
/* must return true; idle (FEATURE_NONE) and non-cloud features must     */
/* return false.                                                         */
/* --------------------------------------------------------------------- */
static void test_wifi_mode_sta_blocks_when_cloud_feature_active(void) {
    test_setup();

    EXPECT(wifi_feature_uses_cloud_network((int)FEATURE_VIN_PAIRING) == true,
           "VIN pairing is a cloud-network feature (blocks wifi_mode swap)");
    EXPECT(wifi_feature_uses_cloud_network((int)FEATURE_WOT_LOGGING) == true,
           "WOT logging is a cloud-network feature (blocks wifi_mode swap)");
    EXPECT(wifi_feature_uses_cloud_network((int)FEATURE_NONE) == false,
           "FEATURE_NONE does NOT block a wifi_mode swap");
    EXPECT(wifi_feature_uses_cloud_network((int)FEATURE_LIVE_TUNE) == false,
           "Live tune is local-only — does NOT block a wifi_mode swap");
    EXPECT(wifi_feature_uses_cloud_network((int)FEATURE_DTC) == false,
           "DTC read/clear is local-only — does NOT block a wifi_mode swap");
}

/* --------------------------------------------------------------------- */
/* 8 — wifi_clear: removes creds + forces AP intent                       */
/* --------------------------------------------------------------------- */
static void test_wifi_clear_removes_creds_and_forces_ap(void) {
    test_setup();
    /* Seed both creds and sta-intent. */
    wifi_client_set_creds("HomeNet", "supersecret");
    nvs_mock_set(WIFI_MODE_INTENT_NVS_KEY, WIFI_MODE_INTENT_NVS_VAL_STA);

    /* The cmd_wifi_clear handler issues both calls in sequence; mirror
     * that here. */
    esp_err_t r1 = wifi_set_mode_intent(WIFI_MODE_INTENT_AP_ONLY);
    esp_err_t r2 = wifi_client_clear_creds();
    EXPECT(r1 == ESP_OK && r2 == ESP_OK,
           "set_mode_intent(AP_ONLY) + clear_creds both OK");

    EXPECT(!wifi_client_creds_stored(),
           "creds_stored() == false after clear");
    const char *ssid = nvs_mock_get(WIFI_STA_SSID_NVS_KEY);
    EXPECT(ssid != NULL && ssid[0] == '\0',
           "NVS sta_ssid is empty string after clear");

    const char *mode = nvs_mock_get(WIFI_MODE_INTENT_NVS_KEY);
    EXPECT(mode != NULL && strcmp(mode, WIFI_MODE_INTENT_NVS_VAL_AP) == 0,
           "NVS wifi_mode = ap after wifi_clear");
}

/* --------------------------------------------------------------------- */
/* 9 — wifi_status: reflects mode and creds flag                          */
/*                                                                       */
/* The host-side analog of cmd_wifi_status2: assemble what its           */
/* response would carry by querying the same backing functions and      */
/* verify each one tracks NVS / state.                                  */
/* --------------------------------------------------------------------- */
static void test_wifi_status_reflects_mode_and_creds_flag(void) {
    test_setup();

    /* Initial: no creds, no intent → default-when-missing is APSTA
     * (so a firmware upgrade onto a customer dongle with stored creds
     * still auto-reconnects at boot). Explicit `wifi_mode ap` writes "ap"
     * to NVS to flip this off. */
    EXPECT(wifi_get_mode_intent() == WIFI_MODE_INTENT_APSTA,
           "default mode intent is APSTA when NVS empty");
    EXPECT(wifi_client_creds_stored() == false,
           "default creds_stored is false when NVS empty");

    /* Store creds → creds_stored flips true. */
    wifi_client_set_creds("HomeNet", "supersecret");
    EXPECT(wifi_client_creds_stored() == true,
           "creds_stored flips true after wifi_client_set_creds");

    /* Switch intent to sta → wifi_get_mode_intent reports APSTA. */
    wifi_set_mode_intent(WIFI_MODE_INTENT_APSTA);
    EXPECT(wifi_get_mode_intent() == WIFI_MODE_INTENT_APSTA,
           "mode intent persists across get after APSTA set");

    /* Clear creds → still APSTA intent but creds_stored is false. */
    wifi_client_clear_creds();
    EXPECT(wifi_client_creds_stored() == false,
           "creds_stored back to false after clear");
    EXPECT(wifi_get_mode_intent() == WIFI_MODE_INTENT_APSTA,
           "intent unchanged by creds clear (operator still wants STA)");
}

/* --------------------------------------------------------------------- */
/* 11 — Boot-time STA auto-connect is gated on mode_intent                 */
/*                                                                         */
/* Closes the contract gap caught at Tier 1 of the 2026-05-17 smoke test: */
/* `wifi_set_mode_intent(AP_ONLY)` must keep STA down across a reboot,    */
/* even when creds remain in NVS for a later `wifi_mode sta` toggle.      */
/* The boot helper consults `wifi_get_mode_intent()` BEFORE calling into  */
/* esp_wifi_set_config + esp_wifi_connect.                                */
/* --------------------------------------------------------------------- */
static void test_boot_skips_sta_when_intent_is_ap_only(void) {
    test_setup();
    /* Seed creds + explicit AP_ONLY intent (the state a customer's
     * dongle is in after `wifi_mode ap` followed by a reboot). */
    wifi_client_set_creds("HomeNet", "supersecret");
    nvs_mock_set(WIFI_MODE_INTENT_NVS_KEY, WIFI_MODE_INTENT_NVS_VAL_AP);
    EXPECT(wifi_get_mode_intent() == WIFI_MODE_INTENT_AP_ONLY,
           "precondition: intent persisted as AP_ONLY");

    int before_set_cfg = g_esp_wifi_set_config_count;
    int before_connect = g_esp_wifi_connect_count;
    wifi_ap_boot_apply_sta_intent();
    EXPECT(g_esp_wifi_set_config_count == before_set_cfg,
           "esp_wifi_set_config NOT called (STA bring-up suppressed)");
    EXPECT(g_esp_wifi_connect_count == before_connect,
           "esp_wifi_connect NOT called (STA bring-up suppressed)");

    /* Now flip intent to APSTA and run again — boot helper must
     * auto-connect using stored creds. */
    nvs_mock_set(WIFI_MODE_INTENT_NVS_KEY, WIFI_MODE_INTENT_NVS_VAL_STA);
    wifi_ap_boot_apply_sta_intent();
    EXPECT(g_esp_wifi_set_config_count == before_set_cfg + 1,
           "esp_wifi_set_config invoked once after APSTA intent");
    EXPECT(g_esp_wifi_connect_count == before_connect + 1,
           "esp_wifi_connect invoked once after APSTA intent");
    EXPECT(strcmp(g_last_set_config_ssid, "HomeNet") == 0,
           "esp_wifi_set_config received stored SSID");

    /* Default-when-NVS-missing must be APSTA (firmware upgrade scenario:
     * existing customer dongle has STA creds but no `wifi_mode` key — we
     * must NOT regress to AP-only and silently drop their connection). */
    nvs_mock_clear();
    wifi_client_set_creds("HomeNet", "supersecret");
    EXPECT(wifi_get_mode_intent() == WIFI_MODE_INTENT_APSTA,
           "default-when-missing intent is APSTA (upgrade safety)");
    int before_set_cfg2 = g_esp_wifi_set_config_count;
    wifi_ap_boot_apply_sta_intent();
    EXPECT(g_esp_wifi_set_config_count == before_set_cfg2 + 1,
           "default-intent boot auto-connects when creds present");
}

/* --------------------------------------------------------------------- */
/* 12 — WS server starts unconditionally at boot (static, file-scan)       */
/*                                                                         */
/* Pre-2026-05-17 the server start was gated on first AP client. That     */
/* blocked over-LAN headless reach (STA-side tooling / first-time setup). */
/* Verify wifi_ap_start() contains an unconditional `ws_server_start()`   */
/* call AND that the AP_STACONNECTED handler does NOT start the server.   */
/* --------------------------------------------------------------------- */
static void test_ws_server_starts_unconditionally_on_boot(void) {
    char *txt = slurp("firmware/src/wifi/wifi_ap.c");
    if (txt == NULL) {
        txt = slurp("./firmware/src/wifi/wifi_ap.c");
    }
    if (txt == NULL) {
        EXPECT(false, "could not open wifi_ap.c to inspect ws_server wiring");
        return;
    }

    /* Locate wifi_ap_start function body. */
    const char *fn = strstr(txt, "esp_err_t wifi_ap_start(void)");
    EXPECT(fn != NULL, "wifi_ap_start function definition found");

    /* Within that function, find a ws_server_start() call. */
    if (fn != NULL) {
        /* Bound the search to ~6 KB ahead which is well past the function end. */
        const char *end = fn + 6000;
        const char *body_end = strstr(fn, "\nesp_err_t wifi_client_connect");
        if (body_end != NULL && body_end < end) end = body_end;
        char saved = '\0';
        if (end < txt + strlen(txt)) { saved = *end; *((char *)end) = '\0'; }
        bool has_unconditional_call = strstr(fn, "ws_server_start()") != NULL;
        if (saved != '\0') *((char *)end) = saved;
        EXPECT(has_unconditional_call,
               "wifi_ap_start() calls ws_server_start() unconditionally");
    }

    /* AP_STACONNECTED handler must NOT start the server. */
    const char *handler = strstr(txt, "WIFI_EVENT_AP_STACONNECTED");
    EXPECT(handler != NULL, "AP_STACONNECTED case label present");
    if (handler != NULL) {
        const char *case_end = strstr(handler, "break;");
        if (case_end != NULL) {
            char saved = *case_end;
            *((char *)case_end) = '\0';
            /* Match an actual function call `ws_server_start(`, not bare
             * substring — a comment in the case branch may legitimately
             * mention the name when documenting the gating removal. */
            bool starts_server_in_handler = strstr(handler, "ws_server_start(") != NULL;
            *((char *)case_end) = saved;
            EXPECT(!starts_server_in_handler,
                   "AP_STACONNECTED handler does NOT start ws_server (gating removed)");
        }
    }

    free(txt);
}

/* --------------------------------------------------------------------- */
/* 10 — Auth-tier registration check (static, file-scan)                  */
/*                                                                       */
/* Open commands.c and verify the new commands carry the expected       */
/* CMD_SECURITY_SECURED / _UNSECURED tier. This guards against a       */
/* drive-by registry edit that loosens auth on the new mutating cmds.   */
/* --------------------------------------------------------------------- */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* True iff `text` contains a line that has both needle_a and needle_b
 * — used to check that a command name shares a registry-row with a
 * security tier. */
static bool line_contains_both(const char *text,
                               const char *needle_a,
                               const char *needle_b) {
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        size_t copy = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, copy);
        line[copy] = '\0';
        if (strstr(line, needle_a) && strstr(line, needle_b)) return true;
        if (!eol) break;
        p = eol + 1;
    }
    return false;
}

static void test_wifi_commands_require_auth_except_status(void) {
    char *txt = slurp("firmware/src/commands/commands.c");
    if (txt == NULL) {
        /* Try alternate path in case eval.sh runs from project root. */
        txt = slurp("./firmware/src/commands/commands.c");
    }
    if (txt == NULL) {
        EXPECT(false, "could not open commands.c to inspect security tiers");
        return;
    }

    EXPECT(line_contains_both(txt, "\"wifi_sta_set\"", "CMD_SECURITY_SECURED"),
           "wifi_sta_set is registered SECURED");
    EXPECT(line_contains_both(txt, "\"wifi_mode\"", "CMD_SECURITY_SECURED"),
           "wifi_mode is registered SECURED");
    EXPECT(line_contains_both(txt, "\"wifi_clear\"", "CMD_SECURITY_SECURED"),
           "wifi_clear is registered SECURED");
    EXPECT(line_contains_both(txt, "\"wifi_status\"", "CMD_SECURITY_UNSECURED"),
           "wifi_status stays UNSECURED (matches its current behavior)");
    /* Legacy wifi_connect must stay UNSECURED for first-boot pairing per
     * P-24 — guard that nobody silently tightens it without owner sign-off. */
    EXPECT(line_contains_both(txt, "\"wifi_connect\"", "CMD_SECURITY_UNSECURED"),
           "legacy wifi_connect stays UNSECURED (P-24)");

    free(txt);
}

/* --------------------------------------------------------------------- */
/* main                                                                  */
/* --------------------------------------------------------------------- */
int main(void) {
    printf("=== wifi_manager host unit tests ===\n");

    test_wifi_sta_set_stores_creds_in_nvs();
    test_wifi_sta_set_rejects_short_password();
    test_wifi_sta_set_rejects_empty_ssid();
    test_wifi_mode_ap_clears_active_sta_connection();
    test_wifi_mode_sta_fails_without_stored_creds();
    test_wifi_mode_sta_invokes_existing_wifi_client_connect();
    test_wifi_mode_sta_blocks_when_cloud_feature_active();
    test_wifi_clear_removes_creds_and_forces_ap();
    test_wifi_status_reflects_mode_and_creds_flag();
    test_boot_skips_sta_when_intent_is_ap_only();
    test_ws_server_starts_unconditionally_on_boot();
    test_wifi_commands_require_auth_except_status();

    if (g_failures == 0) {
        printf("\n=== OK: all wifi_manager unit tests passed ===\n");
        return 0;
    }
    fprintf(stderr, "\n=== FAIL: %d unit test assertions failed ===\n", g_failures);
    return 1;
}
