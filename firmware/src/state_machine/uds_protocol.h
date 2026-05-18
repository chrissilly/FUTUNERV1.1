#ifndef UDS_PROTOCOL_H
#define UDS_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#define UDS_SERVICE_DIAGNOSTIC_SESSION_CONTROL 0x10
#define UDS_SERVICE_ECU_RESET 0x11
#define UDS_SERVICE_SECURITY_ACCESS 0x27
#define UDS_SERVICE_READ_DATA_BY_ID 0x22
#define UDS_SERVICE_TESTER_PRESENT 0x3E

#define UDS_SESSION_DEFAULT 0x01
#define UDS_SESSION_EXTENDED 0x03
#define UDS_SESSION_PROGRAMMING 0x02

#define UDS_SECURITY_REQUEST_SEED 0x01
#define UDS_SECURITY_SEND_KEY 0x02

#define UDS_RESPONSE_POSITIVE_OFFSET 0x40
#define UDS_RESPONSE_NEGATIVE 0x7F

typedef struct {
    uint8_t service;
    uint8_t data[255];
    uint16_t length;
} uds_request_t;

typedef struct {
    uint8_t service;
    uint8_t data[255];
    uint16_t length;
    bool is_positive;
    bool is_negative;
    uint8_t nrc;
} uds_response_t;

void uds_build_tester_present(uds_request_t *req, uint8_t subfunction, uint8_t parameter);
void uds_build_read_data_by_id(uds_request_t *req, uint16_t did);
void uds_build_diagnostic_session(uds_request_t *req, uint8_t session_type);
void uds_build_security_seed_request(uds_request_t *req);
void uds_build_security_key_response(uds_request_t *req, uint32_t key);

bool uds_parse_response(const uint8_t *data, uint16_t length, uds_response_t *resp);
bool uds_is_positive_response(const uds_response_t *resp, uint8_t expected_service);
bool uds_is_negative_response(const uds_response_t *resp);

bool uds_extract_data(const uds_response_t *resp, uint16_t expected_did, 
                      uint8_t *out_data, uint16_t *out_length, uint16_t max_length);

const char* uds_get_nrc_name(uint8_t nrc);

#endif // UDS_PROTOCOL_H

