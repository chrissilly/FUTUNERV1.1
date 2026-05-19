#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

/*
 * wifi_config.h — central tunables for the WiFi AP / STA module.
 *
 * Per FUTV1.1/CLAUDE.md "no magic numbers" rule, every numeric/literal
 * constant the wifi module uses lives in this header. wifi_ap.c MUST NOT
 * contain inline literals for behavioral values.
 *
 * All defaults below are PROPOSED and need approval from Sean before
 * lock. The annotation tag is "approval before lock — DEFER LOCK UNTIL
 * OWNER REVIEW" so it greps cleanly across the codebase.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum auto-reconnect attempts after an STA disconnect event before
 * the driver gives up. Originally inlined in wifi_ap.c as
 * `#define STA_MAX_RETRIES 5`.
 *
 * approval before lock — DEFER LOCK UNTIL OWNER REVIEW
 */
#define STA_MAX_RETRIES                 5

/*
 * 2.4 GHz channel the SoftAP advertises on. Originally inlined in
 * wifi_ap.h as `#define WIFI_AP_CHANNEL 1`.
 *
 * approval before lock — DEFER LOCK UNTIL OWNER REVIEW
 */
#define WIFI_AP_CHANNEL                 1

/*
 * SoftAP maximum simultaneous client connections. Originally inlined in
 * wifi_ap.h as `#define WIFI_AP_MAX_CONNECTIONS 4`.
 *
 * approval before lock — DEFER LOCK UNTIL OWNER REVIEW
 */
#define WIFI_AP_MAX_CONNECTIONS         4

/*
 * Default SoftAP password used when no per-device password has been
 * saved in NVS. Originally inlined in wifi_ap.h as
 * `#define WIFI_AP_PASSWORD_DEFAULT "password"`. Known-weak; tracked as
 * P-19 in docs/PHASE_2_PREREQUISITES.md. Relocated unchanged this
 * prompt — owner has explicitly held the value pending the security pass
 * that also touches first-boot UX.
 *
 * approval before lock — DEFER LOCK UNTIL OWNER REVIEW
 */
#define WIFI_AP_PASSWORD_DEFAULT        "password"

/* -------------------------------------------------------------------- */
/* WiFi mode intent (NEW in this prompt)                                */
/* -------------------------------------------------------------------- */

/*
 * NVS key recording the operator's WiFi mode intent. The radio is
 * ALWAYS APSTA (AP up + optional STA on top); this flag captures the
 * intent flag the operator picked so the firmware can decide whether
 * to bring STA up at boot and on `wifi_set_mode_intent` calls.
 *
 * Values: WIFI_MODE_INTENT_NVS_VAL_AP / WIFI_MODE_INTENT_NVS_VAL_STA.
 *
 * approval before lock — DEFER LOCK UNTIL OWNER REVIEW
 */
#define WIFI_MODE_INTENT_NVS_KEY        "wifi_mode"

#define WIFI_MODE_INTENT_NVS_VAL_AP     "ap"
#define WIFI_MODE_INTENT_NVS_VAL_STA    "sta"

/*
 * Time `wifi_mode sta` waits for the STA association to succeed (or for
 * `IP_EVENT_STA_GOT_IP` to fire) before returning
 * `{"ok":false,"error":"sta_connect_timeout",...}`. The STA driver keeps
 * retrying in the background after the command returns — this is just
 * the command-response deadline.
 *
 * approval before lock — DEFER LOCK UNTIL OWNER REVIEW
 */
#define WIFI_STA_CONNECT_TIMEOUT_MS     8000

/*
 * Poll cadence inside the STA-connect wait loop.
 *
 * approval before lock — DEFER LOCK UNTIL OWNER REVIEW
 */
#define WIFI_STA_CONNECT_POLL_MS        100

/*
 * WPA2 minimum password length. Technical floor (spec-defined), not a
 * tunable — kept here for locality with the other WiFi knobs.
 */
#define WIFI_STA_PASSWORD_MIN_LEN       8

/* -------------------------------------------------------------------- */
/* WiFi mode-intent enum                                                */
/* -------------------------------------------------------------------- */

/*
 * Operator-selected WiFi mode intent. Maps to NVS string values above.
 * The hardware radio mode is independently always APSTA; this enum
 * captures customer-visible intent only.
 */
typedef enum {
    WIFI_MODE_INTENT_AP_ONLY = 0,   /* STA disabled (AP-only experience)         */
    WIFI_MODE_INTENT_APSTA   = 1,   /* STA active alongside AP (joined external) */
} wifi_mode_intent_t;

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONFIG_H */
