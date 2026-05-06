#ifndef VIN_PAIRING_CONFIG_H
#define VIN_PAIRING_CONFIG_H

/*
 * vin_pairing_config.h — central tunables for the VIN pairing
 * feature. Per FUTV1.1/CLAUDE.md "no magic numbers" rule.
 *
 * All defaults below are PROPOSED and need approval from Sean before lock.
 */

/*
 * Feature descriptor name shown in feature_manager logs and any
 * "active feature" UI surface.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define VIN_PAIRING_FEATURE_NAME                "vin_pairing"

/*
 * Maximum length of the device MAC string used in /register payloads.
 * Sized for "AA:BB:CC:DD:EE:FF" + NUL with headroom.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define VIN_PAIRING_MAC_STRING_MAX              24

/*
 * Maximum length of the boxcode string used in /register payloads.
 * Real-world boxcodes are <16 chars (e.g. "4K0907557G__0003"); 32 is
 * comfortable headroom.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define VIN_PAIRING_BOXCODE_STRING_MAX          32

/*
 * Maximum length of an err_out buffer used by vin_pairing internal
 * helpers when surfacing failures to the WS UI.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define VIN_PAIRING_ERR_BUF_MAX                 192

#endif /* VIN_PAIRING_CONFIG_H */
