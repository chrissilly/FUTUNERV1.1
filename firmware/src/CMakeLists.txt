# FUTUNER v2 Source Files

set(app_sources
    "main.c"
    "can/can_driver.c"
    "can/isotp_shims.c"
    "can/can_manager.c"
    "nvs/nvs_manager.c"
    "nvs/ecu_info.c"
    "state_machine/uds_protocol.c"
    "state_machine/connection_manager.c"
    "logger/logger_config.c"
    "logger/logger_manager.c"
    "logger/logger_variables.c"
    "logger/logger_profile.c"
    "logger/wot_logger.c"
    "logger/wot_recorder.c"
    "logger/wot_uploader.c"
    "isotp_coordinator/isotp_coordinator.c"
    "wifi/wifi_ap.c"
    "websocket/ws_server.c"
    "commands/command_handler.c"
    "commands/commands.c"
    "commands/ecu_commands.c"
    "commands/logger_commands.c"
    "commands/logger_data_commands.c"
    "commands/ecu_write_commands.c"
    "commands/system_commands.c"
    "commands/file_commands.c"
    "commands/profile_commands.c"
    "error/error_tracker.c"
    "filesystem/fs_manager.c"
    "bdef/bdef_file.c"
    "scal/scal_file.c"
    "ecu_write/ecu_write.c"
    "commands/flex_commands.c"
    "commands/can_sniffer.c"
    "commands/serial_console.c"
    "commands/wot_log_commands.c"
    "commands/dtc_commands.c"
    "commands/vin_pair_commands.c"
    "commands/sbf_commands.c"
    "commands/phase2_hil_preflight_commands.c"
    "flex_fuel/blend_engine.c"
    "flex_fuel/flex_fuel.c"
    "feature_manager/feature_manager.c"
    "dtc/dtc_feature.c"
    "dtc/dtc_uds.c"
    "license/license.c"
    "vin_pairing/vin_pairing.c"
    "sbf/sbf_loader.c"
    "sbf/sbf_applier.c"
    "sbf/sbf_downloader.c"
    "sbf/sbf_orchestrator.c"
    "sbf/sbf_variants.c"

    # Phase 2 flash orchestrator + crypto/transport supporting modules.
    # Always compiled into the firmware build for type/symbol coverage;
    # gated at the FEATURE-DISPATCH boundary by FUTUNER_PHASE2_ENABLED
    # in futuner_config.h (default 0). When the gate is 0, main.c does
    # NOT call mdg1_flash_orchestrator_init() and feature_manager never
    # routes work here, so the code paths are dead in customer firmware.
    # When the gate is 1, main.c wires the orchestrator + mbedtls AES
    # iface + (eventually) production CAN transport.
    "flash/lzrb.c"
    "flash/mdg1_payload.c"
    "flash/mdg1_variant_manifest.c"
    "flash/mdg1_transport_shadow.c"
    "flash/mdg1_transport_can.c"
    "flash/mdg1_aes_mbedtls.c"
    "flash/mdg1_flash_orchestrator.c"
    "flash/mdg1_flash.c"
    "flash/phase2_hil_autostart.c"
)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "can" "nvs" "config" "state_machine" "logger" "isotp_coordinator" "wifi" "websocket" "commands" "error" "filesystem" "bdef" "scal" "ecu_write" "flex_fuel" "feature_manager" "dtc" "license" "vin_pairing" "sbf" "flash"
    REQUIRES esp_http_server esp_http_client esp_rom json littlefs mbedtls isotp-c driver nvs_flash esp_wifi esp_event
    EMBED_FILES "${CMAKE_SOURCE_DIR}/futuner_control_panel.html"
)

# Bench-only helpers. Uncomment AT MOST ONE to auto-arm the HIL autostart
# at boot without needing a working command-input channel. NEVER ship
# either in customer firmware.
#   _THIS_BUILD       → mode=shadow (Layer 2)
#   _PROD_THIS_BUILD  → mode=prod   (Step B-2 / Layer 4 dry runs)
# target_compile_definitions(${COMPONENT_LIB} PRIVATE PHASE2_HIL_AUTOSTART_FORCE_ARM_THIS_BUILD=1)
# target_compile_definitions(${COMPONENT_LIB} PRIVATE PHASE2_HIL_AUTOSTART_FORCE_ARM_PROD_THIS_BUILD=1)