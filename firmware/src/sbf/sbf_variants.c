#include "sbf_variants.h"
#include "sbf_config.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"

// sbf_variants — see sbf_variants.h. Static seeded table; v1 only
// covers the dev car. Future boxcodes are added by appending rows.
//
// TODO: replaced by per-variant manifest in Phase A. See P-11 in
// docs/PHASE_2_PREREQUISITES.md and SCALE_ARCHITECTURE_PROPOSAL §2.2.

static const char *TAG = "SBF_VAR";

static const sbf_variant_entry_t k_default_table[] = {
    // Audi RS7 C8 4.0L V8 TFSI dev car (Sean's). Mid byte and
    // address offset taken from logger_variables's defaults for
    // this boxcode; if the two ever drift, fail fast.
    { .boxcode        = "4K0907557G__0003",
      .mid_byte       = (uint8_t)0x80,
      .address_offset = (uint32_t)0x80000000 },
};

#define SBF_DEFAULT_TABLE_LEN  (sizeof(k_default_table) / sizeof(k_default_table[0]))

// Active table pointer + length. Defaults to the seeded table; the
// test harness replaces it via sbf_variants_test_override().
static const sbf_variant_entry_t *s_active     = k_default_table;
static size_t                     s_active_len = SBF_DEFAULT_TABLE_LEN;

bool sbf_variants_lookup(const char *boxcode, sbf_variant_entry_t *out) {
    if (boxcode == NULL || out == NULL) {
        return false;
    }
    for (size_t i = (size_t)0; i < s_active_len; i++) {
        if (s_active[i].boxcode != NULL &&
            strcmp(s_active[i].boxcode, boxcode) == (int)0) {
            *out = s_active[i];
            ESP_LOGI(TAG, "lookup hit: boxcode=%s mid=0x%02X off=0x%08X",
                     boxcode, (unsigned)out->mid_byte, (unsigned)out->address_offset);
            return true;
        }
    }
    ESP_LOGW(TAG, "lookup miss: boxcode=%s", boxcode);
    return false;
}

size_t sbf_variants_count(void) {
    return s_active_len;
}

void sbf_variants_test_override(const sbf_variant_entry_t *entries, size_t count) {
    if (entries == NULL || count == (size_t)0) {
        s_active     = k_default_table;
        s_active_len = SBF_DEFAULT_TABLE_LEN;
        return;
    }
    if (count > (size_t)SBF_VARIANTS_TABLE_MAX) {
        count = (size_t)SBF_VARIANTS_TABLE_MAX;
    }
    s_active     = entries;
    s_active_len = count;
}
