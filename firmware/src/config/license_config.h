#ifndef LICENSE_CONFIG_H
#define LICENSE_CONFIG_H

/*
 * license_config.h — central tunables for the license cache + sync
 * feature. Per FUTV1.1/CLAUDE.md "no magic numbers" rule, every
 * numeric and string constant the license module uses lives here.
 *
 * All defaults below are PROPOSED and need approval from Sean before lock.
 */

/* ------------------------------------------------------------------ */
/* Cloud endpoints                                                     */
/* ------------------------------------------------------------------ */

/*
 * Cloud host base URL (no trailing slash). Overridable at runtime
 * via NVS key LICENSE_CLOUD_HOST_NVS_KEY so the same firmware build
 * can target staging vs. production.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_DEFAULT_HOST                    "https://sillyrabbitmotorsport.com/fut"

/*
 * Path components appended to the configured host. Per
 * docs/SCALE_ARCHITECTURE_PROPOSAL.md §3.4.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_REGISTER_PATH                   "/api/v1/device/register"
#define LICENSE_LICENSE_PATH                    "/api/v1/license"

/* ------------------------------------------------------------------ */
/* HTTP timing                                                         */
/* ------------------------------------------------------------------ */

/*
 * HTTP request timeout (ms). Long enough to cover a slow cellular
 * round-trip; short enough that a stuck server doesn't stall the
 * dongle. Mirrors WOT_UPLOAD_HTTP_TIMEOUT_MS for consistency.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_HTTP_TIMEOUT_MS                 15000

/*
 * HTTP status code lower/upper bounds that count as a successful
 * response.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_HTTP_OK_MIN                     200
#define LICENSE_HTTP_OK_MAX                     299

/*
 * HTTP status codes the license module reasons about by name.
 * Each is the canonical RFC 9110 value; not a tunable, but named
 * to keep .c files free of bare digits.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_HTTP_STATUS_UNAUTHORIZED        401
#define LICENSE_HTTP_STATUS_CONFLICT            409

/* ------------------------------------------------------------------ */
/* NVS keys                                                            */
/* ------------------------------------------------------------------ */

/*
 * NVS keys for the persisted license cache and the device's auth
 * token. The cache is forever-cached per the VIN-lifetime model
 * (SCALE_ARCHITECTURE §6.3): no expiry, refresh opportunistically
 * when Wi-Fi is available, override only via explicit revoke.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_NVS_PAID_KEY                    "lic_paid"
#define LICENSE_NVS_VIN_KEY                     "lic_vin"
#define LICENSE_NVS_REVOKED_KEY                 "lic_revoked"
#define LICENSE_NVS_REVOKED_REASON_KEY          "lic_revrsn"
#define LICENSE_NVS_LAST_SYNC_KEY               "lic_last_ms"
#define LICENSE_NVS_PRESENT_KEY                 "lic_present"
#define LICENSE_NVS_AUTH_TOKEN_KEY              "auth_token"
#define LICENSE_NVS_CLOUD_HOST_KEY              "cloud_host"

/* ------------------------------------------------------------------ */
/* Buffer sizing                                                       */
/* ------------------------------------------------------------------ */

/*
 * VIN length. ISO 3779 mandates 17 characters. +1 for NUL.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_VIN_LEN                         17
#define LICENSE_VIN_BUF_LEN                     (LICENSE_VIN_LEN + 1)

/*
 * Maximum length of a cloud-supplied revocation reason string.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_REVOKE_REASON_MAX               128

/*
 * Maximum length of an auth token. Cloud emits 32 hex chars
 * (cloud/src/main.py: secrets.token_hex(16)). +1 for NUL, +
 * headroom for any future format change.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_AUTH_TOKEN_MAX                  64

/*
 * Maximum length of a built URL (host + path).
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_URL_MAX                         256

/*
 * Maximum response body size accepted from /api/v1/license. The
 * canonical body is well under 256 bytes; we size at 1 KiB to leave
 * room for future fields without blowing past a sensible cap.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_RESPONSE_BUF_MAX                1024

/*
 * Maximum length of a register-request body the dongle emits.
 * Today it is JSON: {"mac":..., "vin":..., "boxcode":...} with all
 * fields short.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_REGISTER_BODY_MAX               512

/* ------------------------------------------------------------------ */
/* Auth header                                                         */
/* ------------------------------------------------------------------ */

/*
 * The Bearer prefix the dongle prepends to its auth token when
 * building the Authorization header. Cloud expects the literal RFC
 * 6750 form.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_BEARER_PREFIX                   "Bearer "
#define LICENSE_BEARER_PREFIX_LEN               7

/*
 * Authorization header buffer size: prefix + token + NUL.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_AUTH_HEADER_MAX                 (LICENSE_BEARER_PREFIX_LEN + \
                                                 LICENSE_AUTH_TOKEN_MAX + 1)

/* ------------------------------------------------------------------ */
/* Internal sizing (named so the .c file carries no bare integers)     */
/* ------------------------------------------------------------------ */

/*
 * Scratch buffer used to format the JSON-key needle ("\"<key>\"")
 * inside license.c's tiny JSON parser. 64 chars covers every key the
 * license response declares with comfortable headroom.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_JSON_NEEDLE_BUF_MAX             64

/*
 * Bytes consumed when the JSON parser sees a backslash-escape: the
 * backslash plus the following character. Named so license.c's
 * `v += LICENSE_JSON_ESCAPE_CHAR_BYTES` reads as intent, not magic.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define LICENSE_JSON_ESCAPE_CHAR_BYTES          2

#endif /* LICENSE_CONFIG_H */
