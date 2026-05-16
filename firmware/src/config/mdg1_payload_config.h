#ifndef MDG1_PAYLOAD_CONFIG_H
#define MDG1_PAYLOAD_CONFIG_H

/*
 * mdg1_payload_config.h — central tunables for the MDG1 wire-payload
 * pack/unpack module (LZRB + AES-128-CBC + PKCS#7).
 *
 * Per FUTV1.1/CLAUDE.md "no magic numbers" rule, every numeric or
 * vector constant the pack/unpack path consumes lives here. The IV
 * comes from the Bosch reference implementation (RL_MDG1.cpp line 116)
 * and has been validated end-to-end against the .enc reference plus
 * the live MagicMotorsport CAL capture (see
 * hw_reference/FINDINGS_2026-05-12_phase2_key_recovery.md).
 *
 * All defaults below are PROPOSED and need approval from Sean before lock.
 */

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Cipher / KDF dimensions                                            */
/* ------------------------------------------------------------------ */

/*
 * AES-128 key length in bytes. AES-128-CBC is the only mode this module
 * supports; AES-192/256 would require a separate iface and a different
 * key layout.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_PAYLOAD_AES_KEY_BYTES          16

/*
 * AES block size in bytes (== AES IV size). Standard AES; do not change.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_PAYLOAD_AES_BLOCK_BYTES        16

/* ------------------------------------------------------------------ */
/* PKCS#7 padding bounds                                              */
/* ------------------------------------------------------------------ */

/*
 * PKCS#7 padding always adds 1..MDG1_PAYLOAD_AES_BLOCK_BYTES bytes
 * (never zero — a full block of padding is appended when the input is
 * already block-aligned). Used as a sanity bound when validating pad.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_PAYLOAD_PKCS7_MIN_PAD          1
#define MDG1_PAYLOAD_PKCS7_MAX_PAD          MDG1_PAYLOAD_AES_BLOCK_BYTES

/* ------------------------------------------------------------------ */
/* Fixed Bosch IV (MG1 / MDG1 family default)                         */
/* ------------------------------------------------------------------ */

/*
 * Fixed Bosch IV used by every MG1 / MD1 variant in AES_KEYS_MASTER.md
 * except MG1CS011. Documented in the Aftab Hussain reference port
 * (RL_MDG1.cpp line 116) and re-validated against both the .enc
 * reference file AND the live RS7 capture during the 2026-05-12 key
 * recovery work.
 *
 * NOTE: when MG1CS011 support lands, a per-variant IV override field
 * is needed (extend aes_keys_per_boxcode.json with an `aes_iv` block
 * mirroring the existing key block). Not yet implemented — the global
 * default below is the only IV the codepath supports today.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_BOSCH_FIXED_IV_INIT { \
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, \
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F  \
}

/* ------------------------------------------------------------------ */
/* Input-size guards                                                  */
/* ------------------------------------------------------------------ */

/*
 * Pack() needs to size an output buffer for the LZRB-compressed
 * plaintext before knowing the compressed size. The codec can inflate
 * incompressible input by ~13%, so the upper-bound estimate is:
 *   cap = input + input / LZRB_HEADROOM_DIVISOR + LZRB_HEADROOM_FIXED_BYTES
 * (matches the heuristic used in firmware/test/test_lzrb.c).
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_PAYLOAD_LZRB_HEADROOM_DIVISOR     8u
#define MDG1_PAYLOAD_LZRB_HEADROOM_FIXED_BYTES 64u

/*
 * Hard ceiling on plaintext input to pack(). The largest MDG1 flash
 * section is ASW1/ASW2 at 2 MiB (0x200000) — this constant leaves
 * headroom for the unknown future case. Pack() returns
 * ESP_ERR_INVALID_SIZE for any plaintext beyond this.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_PAYLOAD_MAX_PLAINTEXT_BYTES    (4u * 1024u * 1024u)

/*
 * Hard ceiling on ciphertext input to unpack(). Same reasoning as
 * MAX_PLAINTEXT; ciphertext is similar order of magnitude.
 *
 * Proposed default — needs approval from Sean before lock.
 */
#define MDG1_PAYLOAD_MAX_CIPHERTEXT_BYTES   (4u * 1024u * 1024u)

#endif /* MDG1_PAYLOAD_CONFIG_H */
