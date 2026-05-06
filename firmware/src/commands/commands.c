#include "commands.h"
#include "ecu_commands.h"
#include "logger_commands.h"
#include "logger_data_commands.h"
#include "ecu_write_commands.h"
#include "system_commands.h"
#include "file_commands.h"
#include "profile_commands.h"
#include "flex_commands.h"
#include "can_sniffer.h"
#include "wot_log_commands.h"
#include "dtc_commands.h"
#include "vin_pair_commands.h"
#include "sbf_commands.h"

const command_def_t COMMAND_REGISTRY[] = {
    {"pair_ecu", "Pair with current ECU", CMD_SECURITY_UNSECURED, cmd_pair_ecu},
    {"configure_logger", "Configure logger variables", CMD_SECURITY_UNSECURED, cmd_configure_logger},
    {"get_logger_data", "Get all logger variable values", CMD_SECURITY_UNSECURED, cmd_get_logger_data},
    {"get_single_variable", "Get single variable value by name", CMD_SECURITY_UNSECURED, cmd_get_single_variable},
    {"write_ecu", "Write data to ECU RAM", CMD_SECURITY_SECURED, cmd_write_ecu},
    {"remove_pairing", "Remove paired vehicle", CMD_SECURITY_SECURED, cmd_remove_pairing},
    {"get_status", "Get system status", CMD_SECURITY_UNSECURED, cmd_get_status},
    {"list_commands", "List all available commands", CMD_SECURITY_UNSECURED, cmd_list_commands},
    {"get_errors", "Get error log history", CMD_SECURITY_UNSECURED, cmd_get_errors},
    {"clear_errors", "Clear error log", CMD_SECURITY_SECURED, cmd_clear_errors},
    {"wifi_connect", "Join external WiFi (saves to NVS)", CMD_SECURITY_UNSECURED, cmd_wifi_connect},
    {"wifi_disconnect", "Drop STA, clear saved creds", CMD_SECURITY_SECURED, cmd_wifi_disconnect},
    {"wifi_status", "STA connection state + IP", CMD_SECURITY_UNSECURED, cmd_wifi_status},
    {"logger_start", "Begin logger polling", CMD_SECURITY_UNSECURED, cmd_logger_start},
    {"logger_stop", "Pause logger polling", CMD_SECURITY_UNSECURED, cmd_logger_stop},
    {"fs_info", "Get filesystem information", CMD_SECURITY_SECURED, cmd_fs_info},
    {"fs_list", "List directory contents", CMD_SECURITY_SECURED, cmd_fs_list},
    {"fs_read", "Read file contents (base64)", CMD_SECURITY_SECURED, cmd_fs_read},
    {"fs_write", "Write file (base64 data)", CMD_SECURITY_SECURED, cmd_fs_write},
    {"fs_delete", "Delete file or directory", CMD_SECURITY_SECURED, cmd_fs_delete},
    {"fs_mkdir", "Create directory", CMD_SECURITY_SECURED, cmd_fs_mkdir},
    /* Logger profile management */
    {"list_available_vars", "List all loggable variables for current boxcode", CMD_SECURITY_UNSECURED, cmd_list_available_vars},
    {"get_logger_profile", "Get current logger variable selection", CMD_SECURITY_UNSECURED, cmd_get_logger_profile},
    {"set_logger_profile", "Save logger variable selection", CMD_SECURITY_UNSECURED, cmd_set_logger_profile},
    {"delete_logger_profile", "Delete saved profile, revert to defaults", CMD_SECURITY_UNSECURED, cmd_delete_logger_profile},
    /* Flex fuel blending */
    {"flex_load_scal", "Load SCAL calibration file for flex fuel", CMD_SECURITY_SECURED, cmd_flex_load_scal},
    {"flex_unload_scal", "Unload SCAL and free PSRAM", CMD_SECURITY_SECURED, cmd_flex_unload_scal},
    {"flex_status", "Get flex fuel blending status", CMD_SECURITY_UNSECURED, cmd_flex_status},
    {"flex_enable", "Enable live flex fuel blending", CMD_SECURITY_SECURED, cmd_flex_enable},
    {"flex_disable", "Disable live flex fuel blending", CMD_SECURITY_SECURED, cmd_flex_disable},
    {"flex_set_override", "Set manual ethanol % override", CMD_SECURITY_SECURED, cmd_flex_set_override},
    /* CAN sniffer / dev tools */
    {"can_sniff_start", "Start CAN bus sniffer (streams frames via WebSocket)", CMD_SECURITY_SECURED, cmd_can_sniff_start},
    {"can_sniff_stop", "Stop CAN bus sniffer", CMD_SECURITY_SECURED, cmd_can_sniff_stop},
    {"can_sniff_status", "Get CAN sniffer capture stats", CMD_SECURITY_UNSECURED, cmd_can_sniff_status},
    {"can_send_raw", "Send raw CAN frame (id, data[], len)", CMD_SECURITY_SECURED, cmd_can_send_raw},
    /* WOT logger — first feature plumbed through feature_manager */
    {"wot_log_start", "Start WOT logging (via feature_manager)", CMD_SECURITY_UNSECURED, cmd_wot_log_start},
    {"wot_log_stop",  "Stop WOT logging (via feature_manager)",  CMD_SECURITY_UNSECURED, cmd_wot_log_stop},
    /* DTC read/clear — second feature plumbed through feature_manager */
    {"dtc_read",  "Read active DTCs from ECU (UDS 0x19 0x02)", CMD_SECURITY_UNSECURED, cmd_dtc_read},
    {"dtc_clear", "Clear all DTCs in ECU (UDS 0x14 FF FF FF)", CMD_SECURITY_SECURED,   cmd_dtc_clear},
    /* VIN pairing + license cache — first cloud round-trip feature */
    {"vin_pair_now",   "Run VIN pair-or-refresh (cloud /register + /license)", CMD_SECURITY_UNSECURED, cmd_vin_pair_now},
    {"set_auth_token", "Install device Bearer token in NVS (admin pre-enrollment)", CMD_SECURITY_SECURED, cmd_set_auth_token},
    {"license_status", "Snapshot of cached license state (read-only)", CMD_SECURITY_UNSECURED, cmd_license_status},
    /* SBF live tune — Prompt 5; gated by license_can_run_feature(FEATURE_LIVE_TUNE) */
    {"live_tune_start",  "Begin live-tune (params: stage, ethanol_pct)", CMD_SECURITY_UNSECURED, cmd_live_tune_start},
    {"live_tune_set",    "Update active live-tune (params: stage, ethanol_pct)", CMD_SECURITY_UNSECURED, cmd_live_tune_set},
    {"live_tune_stop",   "Drain queue and unload active SBF",            CMD_SECURITY_UNSECURED, cmd_live_tune_stop},
    {"live_tune_status", "Snapshot of orchestrator state (read-only)",   CMD_SECURITY_UNSECURED, cmd_live_tune_status},
};

const uint8_t COMMAND_COUNT = sizeof(COMMAND_REGISTRY) / sizeof(command_def_t);
