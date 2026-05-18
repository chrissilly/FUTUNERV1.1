#ifndef ISOTP_SHIMS_H
#define ISOTP_SHIMS_H

#include <stdint.h>

int isotp_user_send_can(const uint32_t arbitration_id, const uint8_t* data, const uint8_t size);

uint32_t isotp_user_get_ms(void);

void isotp_user_debug(const char* message, ...);

#endif // ISOTP_SHIMS_H

