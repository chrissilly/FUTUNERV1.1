#ifndef ISOTP_COORDINATOR_H
#define ISOTP_COORDINATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief ISO-TP Bus Arbiter
 * 
 * Coordinates access to the ISO-TP channel to prevent conflicts between:
 * - Logger polling (high frequency)
 * - ECU write operations (low frequency, high priority)
 * - Other UDS requests
 * 
 * Rules:
 * 1. Only one operation can use the ISO-TP channel at a time
 * 2. ECU writes have priority over logger polls
 * 3. Logger polls are skipped if a write is in progress
 * 4. Writes are queued if another operation is in progress
 */

typedef enum {
    ISOTP_OWNER_NONE,
    ISOTP_OWNER_CONNECTION_MANAGER,  // Initial connection, tester present, etc.
    ISOTP_OWNER_LOGGER,              // Logger polling
    ISOTP_OWNER_ECU_WRITE,           // ECU write operations
    ISOTP_OWNER_PHASE2_FLASH         // Phase 2 full-binary flash orchestrator
} isotp_owner_t;

/**
 * @brief Initialize the ISO-TP coordinator
 */
esp_err_t isotp_coordinator_init(void);

/**
 * @brief Request ownership of the ISO-TP channel
 * 
 * @param owner Who is requesting ownership
 * @param timeout_ms Maximum time to wait for ownership (0 = don't wait)
 * @return true if ownership granted, false if busy
 */
bool isotp_coordinator_request(isotp_owner_t owner, uint32_t timeout_ms);

/**
 * @brief Release ownership of the ISO-TP channel
 * 
 * @param owner Who is releasing ownership
 */
void isotp_coordinator_release(isotp_owner_t owner);

/**
 * @brief Check if a specific owner currently has the channel
 * 
 * @param owner Owner to check
 * @return true if this owner has the channel
 */
bool isotp_coordinator_has_ownership(isotp_owner_t owner);

/**
 * @brief Get current owner of the ISO-TP channel
 * 
 * @return Current owner (ISOTP_OWNER_NONE if free)
 */
isotp_owner_t isotp_coordinator_get_owner(void);

/**
 * @brief Check if channel is free
 * 
 * @return true if no one owns the channel
 */
bool isotp_coordinator_is_free(void);

#endif // ISOTP_COORDINATOR_H

