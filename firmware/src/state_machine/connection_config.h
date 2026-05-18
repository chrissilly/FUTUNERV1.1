#ifndef CONNECTION_CONFIG_H
#define CONNECTION_CONFIG_H

#define UDS_VIN_RESOURCE_ID 0xF190
#define UDS_SERIAL_NUMBER_DID 0xF187
#define UDS_SOFTWARE_VERSION_DID 0xF189
#define UDS_BUILD_ID_DID 0xF1F4

#define UDS_SERVICE_TESTER_PRESENT 0x3E
#define UDS_SERVICE_READ_DATA_BY_ID 0x22

#define UDS_SUBFUNCTION_TESTER_PRESENT_NORMAL 0x00
#define UDS_SUBFUNCTION_LIVE_MODE_ENABLE 0x39

/* Patch info request: 0x3E with subfunction 0x30, parameter 0x80
 * Found by reverse-engineering old firmware binary (byte pattern at offset 0x00864c).
 * Old code had 0x0180 which sent [0x3E, 0x01, 0x80] — WRONG (SubFunctionNotSupported).
 * Correct is [0x3E, 0x30, 0x80] as the old firmware sends. */
#define UDS_SUBFUNCTION_PATCH_INFO 0x30
#define UDS_PATCH_INFO_PARAM 0x80
#define UDS_PATCH_VERSION_V2 0x01
#define UDS_PATCH_BUFFER_SIZE 1023

#define UDS_RESPONSE_POSITIVE_OFFSET 0x40
#define UDS_RESPONSE_NEGATIVE 0x7F

#define CONNECTION_REQUEST_TIMEOUT_MS 1000
#define CONNECTION_DISCOVERY_RETRY_DELAY_MS 3000

#define KEEPALIVE_INTERVAL_MS 1000

#endif // CONNECTION_CONFIG_H

