#include "sa2_vm.h"

/*
 * Opcode encoding from VW80126 Anhang "SA2-060331-V10" §2.4.
 *   0x81 RSL          rotate operand left  1 bit, carry := old bit 31
 *   0x82 RSR          rotate operand right 1 bit, carry := old bit 0
 *   0x93 ADD wwwwwwww operand += value,  carry := overflow past 32 bits
 *   0x84 SUB wwwwwwww operand -= value,  carry := (value > old operand)
 *   0x87 EOR wwwwwwww operand ^= value,  carry := 0
 *   0x68 FOR ww       begin loop, ww iterations  (ww = 0 is illegal)
 *   0x49 NEXT         end loop body, decrement, branch back if non-zero
 *   0x4A BCC ww       if carry == 0, skip ww bytes (PC already past operand)
 *   0x6B BRA ww       always skip ww bytes (PC already past operand)
 *   0x4C FINISH       stop, operand is key
 *
 * BCC / BRA target = (PC after consuming the operand byte) + ww. Forward only,
 * per the spec example (Tabelle 4) and per the in-tree MG1 generic script.
 */

#define OP_RSL    0x81
#define OP_RSR    0x82
#define OP_ADD    0x93
#define OP_SUB    0x84
#define OP_EOR    0x87
#define OP_FOR    0x68
#define OP_NEXT   0x49
#define OP_BCC    0x4A
#define OP_BRA    0x6B
#define OP_FIN    0x4C

#define LOOP_STACK_DEPTH   8       /* spec only requires 1, allow some nesting */
#define INSTRUCTION_BUDGET 1000000 /* generous ceiling against infinite loops */

typedef struct {
    size_t   for_pc;     /* PC of the byte after the FOR's count byte (loop body start) */
    uint32_t remaining;  /* iterations left */
} loop_frame_t;

const char *sa2_status_str(sa2_status_t s) {
    switch (s) {
    case SA2_OK:                    return "ok";
    case SA2_ERR_INVALID_OPCODE:    return "invalid opcode";
    case SA2_ERR_TRUNCATED:         return "truncated script";
    case SA2_ERR_INVALID_JUMP:      return "invalid jump target";
    case SA2_ERR_LOOP_ZERO:         return "FOR with zero count";
    case SA2_ERR_LOOP_OVERFLOW:     return "loop nesting too deep";
    case SA2_ERR_NEXT_WITHOUT_FOR:  return "NEXT without FOR";
    case SA2_ERR_NO_FINISH:         return "no FINISH reached";
    case SA2_ERR_BUDGET_EXCEEDED:   return "instruction budget exceeded";
    default:                        return "unknown";
    }
}

static int read_u32_be(const uint8_t *script, size_t script_len, size_t at, uint32_t *out) {
    if (at + 4 > script_len) return -1;
    *out = ((uint32_t)script[at]     << 24) |
           ((uint32_t)script[at + 1] << 16) |
           ((uint32_t)script[at + 2] <<  8) |
           ((uint32_t)script[at + 3]);
    return 0;
}

sa2_status_t sa2_run(uint32_t seed,
                     const uint8_t *script,
                     size_t script_len,
                     uint32_t *key_out)
{
    if (!script || script_len == 0 || !key_out) return SA2_ERR_TRUNCATED;

    uint32_t operand = seed;
    uint32_t carry   = 0;
    size_t   pc      = 0;
    loop_frame_t loop_stack[LOOP_STACK_DEPTH];
    int loop_top = -1;

    for (uint32_t budget = 0; budget < INSTRUCTION_BUDGET; budget++) {
        if (pc >= script_len) return SA2_ERR_NO_FINISH;
        uint8_t op = script[pc++];

        switch (op) {
        case OP_RSL: {
            carry = (operand >> 31) & 1u;
            operand = (operand << 1) | carry;
            break;
        }
        case OP_RSR: {
            carry = operand & 1u;
            operand = (operand >> 1) | (carry << 31);
            break;
        }
        case OP_ADD: {
            uint32_t v;
            if (read_u32_be(script, script_len, pc, &v) != 0) return SA2_ERR_TRUNCATED;
            pc += 4;
            uint64_t sum = (uint64_t)operand + (uint64_t)v;
            carry = (sum >> 32) & 1u;
            operand = (uint32_t)sum;
            break;
        }
        case OP_SUB: {
            uint32_t v;
            if (read_u32_be(script, script_len, pc, &v) != 0) return SA2_ERR_TRUNCATED;
            pc += 4;
            carry = (v > operand) ? 1u : 0u;
            operand = operand - v;
            break;
        }
        case OP_EOR: {
            uint32_t v;
            if (read_u32_be(script, script_len, pc, &v) != 0) return SA2_ERR_TRUNCATED;
            pc += 4;
            operand ^= v;
            carry = 0;
            break;
        }
        case OP_FOR: {
            if (pc >= script_len) return SA2_ERR_TRUNCATED;
            uint8_t count = script[pc++];
            if (count == 0) return SA2_ERR_LOOP_ZERO;
            if (loop_top + 1 >= LOOP_STACK_DEPTH) return SA2_ERR_LOOP_OVERFLOW;
            loop_top++;
            loop_stack[loop_top].for_pc    = pc;
            loop_stack[loop_top].remaining = count;
            break;
        }
        case OP_NEXT: {
            if (loop_top < 0) return SA2_ERR_NEXT_WITHOUT_FOR;
            loop_stack[loop_top].remaining--;
            if (loop_stack[loop_top].remaining > 0) {
                pc = loop_stack[loop_top].for_pc;
            } else {
                loop_top--;
            }
            break;
        }
        case OP_BCC: {
            if (pc >= script_len) return SA2_ERR_TRUNCATED;
            uint8_t off = script[pc++];
            if (carry == 0) {
                size_t target = pc + off;
                if (target > script_len) return SA2_ERR_INVALID_JUMP;
                pc = target;
            }
            break;
        }
        case OP_BRA: {
            if (pc >= script_len) return SA2_ERR_TRUNCATED;
            uint8_t off = script[pc++];
            size_t target = pc + off;
            if (target > script_len) return SA2_ERR_INVALID_JUMP;
            pc = target;
            break;
        }
        case OP_FIN:
            *key_out = operand;
            return SA2_OK;
        default:
            return SA2_ERR_INVALID_OPCODE;
        }
    }

    return SA2_ERR_BUDGET_EXCEEDED;
}
