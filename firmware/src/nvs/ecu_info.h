#ifndef ECU_INFO_H
#define ECU_INFO_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define VIN_LENGTH 17
#define SOFTWARE_VERSION_LENGTH 32
#define HARDWARE_VERSION_LENGTH 32
#define BUILD_ID_LENGTH 64
#define BOXCODE_LENGTH 128

typedef struct {
    char vin[VIN_LENGTH + 1];
    char software_version[SOFTWARE_VERSION_LENGTH + 1];
    char hardware_version[HARDWARE_VERSION_LENGTH + 1];
    char build_id[BUILD_ID_LENGTH + 1];
    char boxcode[BOXCODE_LENGTH + 1];
    bool is_valid;
} ecu_info_t;

void ecu_info_init(ecu_info_t *info);

bool ecu_info_is_valid(const ecu_info_t *info);

void ecu_info_set_vin(ecu_info_t *info, const char *vin);
void ecu_info_set_software_version(ecu_info_t *info, const char *sw_version);
void ecu_info_set_hardware_version(ecu_info_t *info, const char *hw_version);
void ecu_info_set_build_id(ecu_info_t *info, const char *build_id);

const char* ecu_info_get_vin(const ecu_info_t *info);
const char* ecu_info_get_software_version(const ecu_info_t *info);
const char* ecu_info_get_hardware_version(const ecu_info_t *info);
const char* ecu_info_get_build_id(const ecu_info_t *info);

void ecu_info_print(const ecu_info_t *info);
const char* ecu_info_get_boxcode(const ecu_info_t *info);

#endif // ECU_INFO_H

