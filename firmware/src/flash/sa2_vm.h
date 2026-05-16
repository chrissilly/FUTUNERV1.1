#ifndef SA2_VM_H
#define SA2_VM_H

/*
 * SA2 mini-instruction-set interpreter.
 *
 * Implements the VW/Audi "Seed&Key" challenge-response algorithm specified by
 * VW80126 Anhang "SA2-060331-V10". The bootloader on a Bosch MG1/MDG1 ECU
 * sends a 4-byte seed in response to UDS 0x27 01; the programmer must run a
 * per-variant SA2 bytecode script over that seed and reply 0x27 02 with the
 * resulting 4-byte key.
 *
 * Pure C, no ESP-IDF or FreeRTOS dependencies — host-testable.
 */

#include <stdint.h>
#include <stddef.h>

typedef enum {
    SA2_OK = 0,
    SA2_ERR_INVALID_OPCODE,
    SA2_ERR_TRUNCATED,        /* operand bytes missing past end of script */
    SA2_ERR_INVALID_JUMP,     /* BCC/BRA target outside script */
    SA2_ERR_LOOP_ZERO,        /* FOR with iteration count 0 (per spec, illegal) */
    SA2_ERR_LOOP_OVERFLOW,    /* nesting deeper than the loop stack supports */
    SA2_ERR_NEXT_WITHOUT_FOR, /* NEXT with no matching FOR */
    SA2_ERR_NO_FINISH,        /* ran off the end without hitting FINISH */
    SA2_ERR_BUDGET_EXCEEDED,  /* instruction budget exhausted (likely infinite loop) */
} sa2_status_t;

/*
 * Run an SA2 script. Per the spec:
 *   - operand starts at seed, carry starts at 0
 *   - on FINISH, the operand is the key
 *
 * Returns SA2_OK and writes *key_out on success. On error, *key_out is left
 * untouched and the error code is returned.
 */
sa2_status_t sa2_run(uint32_t seed,
                     const uint8_t *script,
                     size_t script_len,
                     uint32_t *key_out);

const char *sa2_status_str(sa2_status_t s);

#endif /* SA2_VM_H */
