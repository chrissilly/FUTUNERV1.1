#ifndef SBF_VARIANTS_H
#define SBF_VARIANTS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sbf_variants — per-boxcode lookup of the write parameters that
 * ecu_write_data() needs (mid_byte, address_offset).
 *
 * v1 ships a tiny static table seeded with the dev car's
 * 4K0907557G__0003 row. The applier asks for write parameters by
 * boxcode string; if not found, the apply path refuses cleanly
 * with a clear "unknown variant" error.
 *
 * TODO: Phase A migrates this lookup into the per-variant manifest
 * defined in docs/SCALE_ARCHITECTURE_PROPOSAL.md §2.2 (specifically
 * memory_map.write_mid_byte and memory_map.write_offset). Tracked
 * as P-11 in docs/PHASE_2_PREREQUISITES.md.
 *
 * Note on coexistence with logger_variables's
 * get_write_mid_byte/_address_offset accessors: the logger module
 * carries the same parameters loaded at runtime from the
 * configure_logger command. Until both migrate to the variant
 * manifest in Phase A, the two tables MUST agree for any boxcode
 * that appears in both. Discrepancies are a real footgun.
 */

typedef struct {
    /* The boxcode key, e.g. "4K0907557G__0003". Compared with
     * strcmp; case-sensitive. */
    const char *boxcode;

    /* Mid byte that ecu_write_data sends as the write-protocol
     * subfunction selector. Per-boxcode value; differs across
     * Bosch families. */
    uint8_t     mid_byte;

    /* Address offset subtracted from the SBF's "original_address"
     * before forming the on-wire 32-bit ECU address. */
    uint32_t    address_offset;
} sbf_variant_entry_t;

/*
 * Look up write parameters for a boxcode. Returns true and fills
 * `out` if found; returns false otherwise (out left untouched).
 */
bool sbf_variants_lookup(const char *boxcode, sbf_variant_entry_t *out);

/*
 * Diagnostics: total entries in the table. Useful for tests.
 */
size_t sbf_variants_count(void);

/*
 * Test-only: replace the active table with a caller-provided array
 * for the duration of the test run. Pass NULL/0 to restore the
 * default seeded table. Used by host unit tests to inject known
 * boxcodes.
 */
void sbf_variants_test_override(const sbf_variant_entry_t *entries, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SBF_VARIANTS_H */
