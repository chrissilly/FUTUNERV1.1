#ifndef CAN_SNIFFER_H
#define CAN_SNIFFER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

/*
 * CAN Sniffer — Developer Tool
 * ============================
 * Captures raw CAN frames and streams them over WebSocket as JSON.
 * Useful for reverse engineering, debugging CAN bus issues, and
 * verifying protocol sequences.
 *
 * WebSocket commands:
 *   {"command":"can_sniff_start"}              — Start capturing (all IDs)
 *   {"command":"can_sniff_start","filter":"7E"} — Start with ID prefix filter
 *   {"command":"can_sniff_stop"}               — Stop capturing
 *   {"command":"can_sniff_status"}             — Get capture stats
 *   {"command":"can_send_raw","id":1824,"data":[62,0],"len":2} — Send raw CAN frame
 *
 * Captured frames are pushed to all connected WebSocket clients as:
 *   {"event":"can_frame","id":2024,"hex_id":"0x7E8","data":[126,0],"hex":"7E00","len":2,"ts":12345}
 */

esp_err_t cmd_can_sniff_start(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_can_sniff_stop(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_can_sniff_status(int fd, const char *params, char *response, size_t response_size);
esp_err_t cmd_can_send_raw(int fd, const char *params, char *response, size_t response_size);

/* Called from CAN task to check if sniffer wants to capture a received frame */
void can_sniffer_on_frame(uint32_t id, const uint8_t *data, uint8_t len);

/* Is sniffer active? */
bool can_sniffer_is_active(void);

#endif // CAN_SNIFFER_H
