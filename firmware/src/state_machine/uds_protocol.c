#include "uds_protocol.h"
#include <string.h>

void uds_build_tester_present(uds_request_t *req, uint8_t subfunction, uint8_t parameter) {
    if (!req) return;
    
    req->service = UDS_SERVICE_TESTER_PRESENT;
    req->data[0] = subfunction;
    req->length = 1;
    
    if (parameter != 0) {
        req->data[1] = parameter;
        req->length = 2;
    }
}

void uds_build_read_data_by_id(uds_request_t *req, uint16_t did) {
    if (!req) return;
    
    req->service = UDS_SERVICE_READ_DATA_BY_ID;
    req->data[0] = (did >> 8) & 0xFF;
    req->data[1] = did & 0xFF;
    req->length = 2;
}

bool uds_parse_response(const uint8_t *data, uint16_t length, uds_response_t *resp) {
    if (!data || !resp || length == 0) {
        return false;
    }
    
    memset(resp, 0, sizeof(uds_response_t));
    resp->service = data[0];
    
    if (data[0] == UDS_RESPONSE_NEGATIVE) {
        resp->is_negative = true;
        resp->is_positive = false;
        if (length >= 3) {
            resp->nrc = data[2];
        }
        if (length > 1) {
            memcpy(resp->data, &data[1], length - 1);
            resp->length = length - 1;
        }
    } else {
        resp->is_positive = true;
        resp->is_negative = false;
        if (length > 1) {
            memcpy(resp->data, &data[1], length - 1);
            resp->length = length - 1;
        }
    }
    
    return true;
}

bool uds_is_positive_response(const uds_response_t *resp, uint8_t expected_service) {
    if (!resp) return false;
    return resp->is_positive && (resp->service == (expected_service + UDS_RESPONSE_POSITIVE_OFFSET));
}

bool uds_is_negative_response(const uds_response_t *resp) {
    if (!resp) return false;
    return resp->is_negative;
}

bool uds_extract_data(const uds_response_t *resp, uint16_t expected_did, 
                      uint8_t *out_data, uint16_t *out_length, uint16_t max_length) {
    if (!resp || !out_data || !out_length) {
        return false;
    }
    
    if (!resp->is_positive || resp->length < 2) {
        return false;
    }
    
    uint16_t did = (resp->data[0] << 8) | resp->data[1];
    if (did != expected_did) {
        return false;
    }
    
    uint16_t data_len = resp->length - 2;
    if (data_len > max_length) {
        data_len = max_length;
    }
    
    if (data_len > 0) {
        memcpy(out_data, &resp->data[2], data_len);
    }
    *out_length = data_len;
    
    return true;
}

void uds_build_diagnostic_session(uds_request_t *req, uint8_t session_type) {
    if (!req) return;
    req->service = UDS_SERVICE_DIAGNOSTIC_SESSION_CONTROL;
    req->data[0] = session_type;
    req->length = 1;
}

void uds_build_security_seed_request(uds_request_t *req) {
    if (!req) return;
    req->service = UDS_SERVICE_SECURITY_ACCESS;
    req->data[0] = UDS_SECURITY_REQUEST_SEED;
    req->length = 1;
}

void uds_build_security_key_response(uds_request_t *req, uint32_t key) {
    if (!req) return;
    req->service = UDS_SERVICE_SECURITY_ACCESS;
    req->data[0] = UDS_SECURITY_SEND_KEY;
    req->data[1] = (key >> 24) & 0xFF;
    req->data[2] = (key >> 16) & 0xFF;
    req->data[3] = (key >> 8) & 0xFF;
    req->data[4] = key & 0xFF;
    req->length = 5;
}

const char* uds_get_nrc_name(uint8_t nrc) {
    switch (nrc) {
        case 0x10: return "GeneralReject";
        case 0x11: return "ServiceNotSupported";
        case 0x12: return "SubFunctionNotSupported";
        case 0x13: return "IncorrectMessageLength";
        case 0x22: return "ConditionsNotCorrect";
        case 0x31: return "RequestOutOfRange";
        case 0x33: return "SecurityAccessDenied";
        case 0x78: return "ResponsePending";
        default: return "Unknown";
    }
}

