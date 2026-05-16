/*
 * test_sa2.c — host-runnable unit tests for the SA2 mini-instruction-set VM.
 *
 * Built into firmware/test/sa2/host_test_runner via the Makefile in that
 * directory. Verifies sa2_run() against:
 *   1. The five (seed, key) pairs from the SA2-060331-V10 spec example
 *      (Tabelle 4 / page 9).
 *   2. Error-path coverage (invalid opcode, truncated operand, OOB jump,
 *      FOR with zero count, NEXT without FOR, missing FINISH).
 *   3. The in-tree MG1 generic bytecode runs without a VM error.
 */

#include "sa2_vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define EXPECT(cond, msg) do {                                              \
    if (!(cond)) {                                                          \
        fprintf(stderr, "  FAIL  %s — %s (line %d)\n",                      \
                __func__, (msg), __LINE__);                                 \
        g_failures++;                                                       \
    } else {                                                                \
        fprintf(stdout, "  PASS  %s — %s\n", __func__, (msg));              \
    }                                                                       \
} while (0)

/* ------------------------------------------------------------------ */
/* Spec example script: VW80126 SA2-060331-V10 Tabelle 4              */
/* ------------------------------------------------------------------ */

static const uint8_t SPEC_EXAMPLE[] = {
    0x68, 0x05,                          /* FOR I=5 TO 1 */
    0x81,                                /* RSL */
    0x4A, 0x05,                          /* BCC +5  (skip the EOR if carry==0) */
    0x87, 0x0A, 0x22, 0x12, 0x89,        /* EOR 0x0A221289 */
    0x49,                                /* NEXT */
    0x4C                                 /* FINISH */
};

static void test_spec_example_pairs(void) {
    static const struct { uint32_t seed; uint32_t key; } pairs[] = {
        { 0x107778EDu, 0x1AAB38B0u },
        { 0xAC13491Bu, 0x02E25348u },
        { 0x198F23CEu, 0x2F824E58u },
        { 0xFA9E0138u, 0x961FE678u },
        { 0x27B3EA04u, 0xDEF50AA0u },
    };

    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        uint32_t key = 0;
        sa2_status_t s = sa2_run(pairs[i].seed, SPEC_EXAMPLE, sizeof(SPEC_EXAMPLE), &key);
        char msg[96];
        snprintf(msg, sizeof(msg), "seed=0x%08X expect=0x%08X got=0x%08X (%s)",
                 pairs[i].seed, pairs[i].key, key, sa2_status_str(s));
        EXPECT(s == SA2_OK && key == pairs[i].key, msg);
    }
}

/* ------------------------------------------------------------------ */
/* Error-path coverage                                                 */
/* ------------------------------------------------------------------ */

static void test_invalid_opcode(void) {
    const uint8_t script[] = { 0xFF, 0x4C };
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0xCAFEBABEu, script, sizeof(script), &key);
    EXPECT(s == SA2_ERR_INVALID_OPCODE, "0xFF rejected");
}

static void test_truncated_add_operand(void) {
    const uint8_t script[] = { 0x93, 0x01, 0x02 }; /* ADD missing 2 bytes */
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0u, script, sizeof(script), &key);
    EXPECT(s == SA2_ERR_TRUNCATED, "ADD truncated rejected");
}

static void test_oob_jump(void) {
    const uint8_t script[] = { 0x6B, 0xFF, 0x4C }; /* BRA +255 — past end */
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0u, script, sizeof(script), &key);
    EXPECT(s == SA2_ERR_INVALID_JUMP, "BRA past end rejected");
}

static void test_for_zero(void) {
    const uint8_t script[] = { 0x68, 0x00, 0x49, 0x4C };
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0u, script, sizeof(script), &key);
    EXPECT(s == SA2_ERR_LOOP_ZERO, "FOR 0 rejected");
}

static void test_next_without_for(void) {
    const uint8_t script[] = { 0x49, 0x4C };
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0u, script, sizeof(script), &key);
    EXPECT(s == SA2_ERR_NEXT_WITHOUT_FOR, "NEXT without FOR rejected");
}

static void test_no_finish(void) {
    const uint8_t script[] = { 0x81 }; /* RSL only, no FINISH */
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0xABCD1234u, script, sizeof(script), &key);
    EXPECT(s == SA2_ERR_NO_FINISH, "missing FINISH rejected");
}

/* ------------------------------------------------------------------ */
/* Single-opcode behavioural checks                                    */
/* ------------------------------------------------------------------ */

static void test_rsl_sets_carry(void) {
    /* Operand starts at 0x80000000 — RSL must wrap bit31 to bit0 and set carry. */
    const uint8_t script[] = { 0x81, 0x4C };
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0x80000000u, script, sizeof(script), &key);
    EXPECT(s == SA2_OK && key == 0x00000001u, "RSL of 0x80000000 -> 0x1, carry observed via wrap");
}

static void test_rsr_sets_carry(void) {
    /* Bit 0 was 1 -> carry, and rotates to bit 31. */
    const uint8_t script[] = { 0x82, 0x4C };
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0x00000001u, script, sizeof(script), &key);
    EXPECT(s == SA2_OK && key == 0x80000000u, "RSR of 0x1 -> 0x80000000");
}

static void test_add_carry(void) {
    /* ADD then BCC — verify carry semantics are visible to a subsequent BCC. */
    const uint8_t script[] = {
        0x93, 0x00, 0x00, 0x00, 0x01,    /* ADD 1 — no overflow, carry := 0 */
        0x4A, 0x05,                       /* BCC +5 — carry==0, take, skip 5 EOR bytes */
        0x87, 0xDE, 0xAD, 0xBE, 0xEF,    /* EOR (skipped) */
        0x4C                              /* FINISH */
    };
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0x00000010u, script, sizeof(script), &key);
    /* operand: 0x10 + 1 = 0x11, no EOR, FINISH -> 0x11 */
    EXPECT(s == SA2_OK && key == 0x00000011u, "ADD with no overflow: carry=0, BCC taken");
}

static void test_add_overflow_no_branch(void) {
    const uint8_t script[] = {
        0x93, 0x00, 0x00, 0x00, 0x01,    /* ADD 1 — overflow, carry := 1 */
        0x4A, 0x05,                       /* BCC +5 — carry==1, NOT taken */
        0x87, 0x00, 0x00, 0x00, 0x07,    /* EOR 0x07 */
        0x4C                              /* FINISH */
    };
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0xFFFFFFFFu, script, sizeof(script), &key);
    /* operand: 0xFFFFFFFF + 1 = 0 (with carry). EOR 0x07 -> 0x07. */
    EXPECT(s == SA2_OK && key == 0x00000007u, "ADD overflow: carry=1, BCC not taken, EOR runs");
}

/* ------------------------------------------------------------------ */
/* In-tree MG1 generic bytecode: must execute without VM error.        */
/* No authoritative test pair available without a live ECU — only      */
/* exercise the interpreter for invalid-opcode / OOB-jump regressions. */
/* ------------------------------------------------------------------ */

static const uint8_t MG1_GENERIC_SCRIPT[] = {
    0x68, 0x07, 0x87, 0x04, 0x01, 0x20, 0x15, 0x93, 0x05, 0x02, 0x20, 0x16,
    0x4A, 0x03, 0x82, 0x6B, 0x06, 0x81, 0x93, 0x06, 0x03, 0x20, 0x17, 0x84,
    0x07, 0x04, 0x20, 0x18, 0x49, 0x4C
};

static void test_mg1_generic_executes(void) {
    uint32_t key = 0;
    sa2_status_t s = sa2_run(0xDEADBEEFu, MG1_GENERIC_SCRIPT, sizeof(MG1_GENERIC_SCRIPT), &key);
    char msg[96];
    snprintf(msg, sizeof(msg), "mg1 generic returns %s, key=0x%08X", sa2_status_str(s), key);
    EXPECT(s == SA2_OK, msg);
}

/* ------------------------------------------------------------------ */

int main(void) {
    fprintf(stdout, "== SA2 VM host tests ==\n");
    test_spec_example_pairs();
    test_invalid_opcode();
    test_truncated_add_operand();
    test_oob_jump();
    test_for_zero();
    test_next_without_for();
    test_no_finish();
    test_rsl_sets_carry();
    test_rsr_sets_carry();
    test_add_carry();
    test_add_overflow_no_branch();
    test_mg1_generic_executes();

    if (g_failures > 0) {
        fprintf(stderr, "\n== %d FAILURES ==\n", g_failures);
        return 1;
    }
    fprintf(stdout, "\n== all SA2 tests passed ==\n");
    return 0;
}
