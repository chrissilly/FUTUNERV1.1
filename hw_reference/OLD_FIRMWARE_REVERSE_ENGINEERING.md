# Old Firmware Reverse Engineering Report

**Binary**: `old_firmware_backup.bin` (2,621,440 bytes / 2.5 MB)
**Platform**: ESP32-S3, PlatformIO/Arduino framework
**Build Environment**: `C:/Users/sean/.platformio/packages/framework-arduinoespressif32/`
**PlatformIO Project**: `firmware_sefi_v1_dev`
**Libraries**: ArduinoJson V7.42 (PB22), NimBLE (BLE stack), WebServer, LittleFS
**ESP-IDF**: Embedded (Arduino-ESP32 wrapper around IDF, TWAI v2 API used)
**Developer**: Sean (Windows build machine)

---

## 1. CAN / TWAI Initialization

### Driver API
The firmware uses the **ESP-IDF TWAI v2 API** (not the older v1), evidenced by:
- `twai_driver_install_v2`
- `twai_driver_uninstall_v2`
- `twai_transmit_v2`
- `twai_configure_gpio`
- `twai_release_gpio`
- `twai_ll_set_clock_source`
- `twai_handle_tx_buffer_frame`
- `twai_free_driver_obj`
- `clk_src == TWAI_CLK_SRC_APB` (APB clock source assertion)

### Key Log Messages
```
[INFO][%s] Initializing CAN bus...
[INFO][%s] CAN bus initialized
[ERROR][%s] Failed to install TWAI driver
[ERROR][%s] Failed to start TWAI driver
[Scorpion] CAN timeout
```

### GPIO Pins
**No explicit GPIO pin numbers were found as strings.** The pin assignments are hardcoded as integer constants in the binary, not as string literals. The assertion `GPIO_IS_VALID_OUTPUT_GPIO(p_obj->tx_io)` confirms GPIO validation occurs during init.

Based on the hardware (NCV7344 CAN transceiver on the PCB), the pins are likely **GPIO 17 (TX) and GPIO 18 (RX)** as documented in the MEMORY.md for the same PCB design.

### CAN Bus Speed
**No explicit baud rate string found** (e.g., "500kbps"). The speed is set as a numeric constant in the timing configuration struct. For MDG1 ECU communication, this is almost certainly **500 kbit/s** (standard OBD-II / UDS diagnostic speed for VAG).

### Mode
The firmware uses **normal mode** (bidirectional TX/RX) -- it transmits UDS requests and receives responses. No "listen only" or "no ack" mode strings found.

### Clock Source
`clk_src == TWAI_CLK_SRC_APB` -- uses APB clock (80 MHz) as the TWAI peripheral clock source.

### CRITICAL FINDING FOR ESP-IDF REWRITE
The old firmware uses `twai_driver_install_v2` and `twai_transmit_v2` -- these are the **newer v2 TWAI APIs**. If the ESP-IDF rewrite is using the older `twai_driver_install()` / `twai_transmit()`, that could be part of the problem, though functionally they should be similar. The v2 API supports multiple TWAI controllers.

---

## 2. GPIO Configuration

### Port Injector Board Pins
```
[Scorpion] Configuring Port Injector board pins from settings
[Scorpion] Warning: Using default pins for Port Injector board
[Scorpion] Warning: Using default pin for Relay board
```
Pins are configurable via settings with fallback defaults.

### Relay Output
```
[Scorpion] Initialized relay output, pin: <number>
```

### Timer/PWM Outputs
The firmware uses hardware timers for PWM generation:
```
[Scorpion] Initializing timers...
[Scorpion] Timer initialization complete.
[Scorpion] Injector duty cycle resolution: <value>
[Scorpion] Injector timer period: <value>
[Scorpion] NOX/METH duty cycle resolution: <value>
[Scorpion] NOX/METH timer period: <value>
```
PWM is used for: port injectors, methanol pump, methanol solenoid, NOx solenoid.

### GPIO Functions Used
- `gpio_config_as_analog`
- `gpio_input_enable` / `gpio_input_disable`
- `gpio_output_enable` / `gpio_output_disable`
- `gpio_pullup_en` / `gpio_pullup_dis`
- `gpio_pulldown_en` / `gpio_pulldown_dis`
- `gpio_set_level`
- `gpio_set_intr_type`
- `gpio_isr_handler_add`
- `gpio_install_isr_service`
- `gpio_od_enable` / `gpio_od_disable` (open drain)

---

## 3. UDS / ISO-TP Protocol Implementation

### ISO-TP Transport Layer
```
[DEBUG][%s]   - Read chunk (max %d bytes, ISO-TP limit %d)
[ERROR][%s] Flow control timeout
[ERROR][%s] Flow control: OVERFLOW
[ERROR][%s] Invalid extended first frame
```
Custom ISO-TP implementation with flow control handling (not a third-party library).

### CAN IDs
No explicit CAN ID strings found. The IDs are hardcoded as numeric constants. For VAG MDG1, standard UDS addressing would be:
- **Request**: 0x7E0 (or extended addressing)
- **Response**: 0x7E8
- **Functional**: 0x7DF

### UDS Connection State Machine
The firmware implements a full UDS state machine with these steps (in order):

1. **TESTER_PRESENT** -- Send 0x3E to discover/keep-alive ECU
2. **EXTENDED_DIAGNOSTIC** -- Enter extended diagnostic session (0x10 03)
3. **PROGRAMMING_SESSION** -- Enter programming session (0x10 02)
4. **REQUEST_SEED** -- Security access seed request (0x27 01)
5. **SEND_KEY** -- Security access key response (0x27 02)
6. **PATCH_INFO** -- Read patch information from ECU
7. **START_TRANSFER** -- Request download (0x34)
8. **TRANSFER_DATA** -- Transfer data (0x36)
9. **END_TRANSFER** -- Request transfer exit (0x37)
10. **RESET_ECU** -- ECU reset (0x11)

### UDS Services Used
- **0x10** - Diagnostic Session Control (Extended + Programming)
- **0x11** - ECU Reset
- **0x19** - Read DTC Information (count, list, snapshot, supported, clear)
- **0x22** - Read Data By Identifier (VIN, Build ID, Serial Number, Software Version, Hardware Version)
- **0x27** - Security Access (seed/key with SA2 algorithm)
- **0x2E** - Write Data By Identifier (ethanol value, cruise display)
- **0x34** - Request Download
- **0x36** - Transfer Data
- **0x37** - Request Transfer Exit
- **0x3E** - Tester Present

### Security Access (0x27) - SA2 Algorithm
```
[DEBUG][%s] Starting SA2 execution with seed: 0x%08X, bytecode length: %d
[DEBUG][%s] Seed received: 0x%08X
[DEBUG][%s] Key calculated: 0x%08X
[DEBUG][%s] Key 0x%08X sent
[DEBUG][%s] Key accepted - security access granted
[DEBUG][%s] Executing instruction 0x%02X at position %d, register: 0x%08X
[INFO][%s] SA2 execution completed, key: 0x%08X
```

The SA2 implementation is a **bytecode virtual machine** that executes instructions to compute the key from the seed. Instructions found:
- **ADD** - Addition
- **SUB** - Subtraction
- **EOR** - Exclusive OR
- **BCC** - Branch on carry clear
- **BRA** - Branch always
- **FOR/NEXT** - Loop construct

The SA2 bytecode is loaded dynamically from an ODX security database:
```
[DEBUG][%s]   - Search ODX security database
[DEBUG][%s]   - Validate security data integrity
[DEBUG][%s] Dynamically loaded security: %s for %s
[DEBUG][%s]   - Load SA2 bytecode or CRC checksum
```

There is also a CRC-based security fallback:
```
CRC security not found for block
SA2 security not found
```

### Diagnostic Session States
```
diagnostic_session_active
Extended diagnostic session active
Extended diagnostic session failed
Programming session active
Programming session failed
Security Access Denied
Security access granted
```

### DTC (Diagnostic Trouble Code) Handling
Full DTC management:
- Count DTCs by status mask
- Read DTC list
- Read DTC snapshots (freeze frame)
- Read supported DTCs
- Clear individual DTCs
- Clear all DTCs

API endpoints: `/api/diagnostics/dtcs`, `/api/diagnostics/count`, `/api/diagnostics/clear`, `/api/diagnostics/clear_dtc`, `/api/diagnostics/snapshot`, `/api/diagnostics/status`, `/api/diagnostics/supported`

---

## 4. WiFi AP Configuration

### Hotspot Manager
```
[INFO][%s] Initializing hotspot manager
[INFO][%s] Access point started with SSID: %s
[ERROR][%s] Failed to configure access point
[ERROR][%s] Failed to start access point
[ERROR][%s] Failed to start WiFi hotspot
```

### SSID Pattern
```
SEFI_%06x
```
The SSID is generated dynamically using `SEFI_` prefix + 6-character hex suffix (likely derived from MAC address). This produces SSIDs like `SEFI_1a2b3c`. The current car dongle broadcasts as `SRM-C8-SEFI` which is from a different/renamed flash.

### NVS Configuration Keys
- `ap_ssid` -- Access point SSID
- `ap_enabled` -- Whether AP is enabled
- `ap_ip` -- AP IP address
- `wifi_ssid` -- STA mode saved SSID
- `wifi_pass` -- STA mode saved password

### STA Mode (Client)
The firmware supports dual-mode WiFi:
- **AP mode**: Always-on hotspot for direct device control
- **STA mode**: Connect to external WiFi for cloud services (updates, registration)

```
[INFO][%s] Connecting to WiFi: %s
[INFO][%s] STA connected to WiFi network
[INFO][%s] WiFi credentials saved to configuration
[DEBUG][%s] Starting periodic WiFi scan to look for known networks
[INFO][%s] Found known network '%s' with RSSI %d, attempting connection
```

### Channel Scanning
Sophisticated per-channel scanning with periodic background scans to find known networks:
```
[DEBUG][%s] Started %s %s scan on channel %d%s
[DEBUG][%s] Waiting %dms before scanning channel %d
[INFO][%s] Starting periodic channel scan for SSID: %s
```

### Web Server
- HTTP server on **port 80**
- WebSocket server on **port 8080**
- mDNS: `http://<hostname>.local`
- Serves gzipped static files from LittleFS

---

## 5. BLE Configuration

### BLE Stack
**NimBLE** (not Bluedroid) -- lighter weight BLE-only stack:
```
nimble
NimBLE
nimble_host
nimble_bond
nimble_timer
NIMBLE_NVS
```

### SEFI BLE Service UUIDs
Base UUID pattern: `b00b8915-2786-43de-XXXX-1ba2441cb00b`

| UUID | Purpose |
|------|---------|
| `b00b8915-2786-43de-AAAA-1ba2441cb00b` | **SEFI Service** (main service) |
| `b00b8915-2786-43de-AAA1-1ba2441cb00b` | SEFI Characteristic 1 (likely TX/notify) |
| `b00b8915-2786-43de-AAA2-1ba2441cb00b` | SEFI Characteristic 2 (likely RX/write) |
| `b00b8915-2786-43de-0000-1ba2441cb00b` | **Ethanol Service** |
| `b00b8915-2786-43de-0001-1ba2441cb00b` | Ethanol Characteristic 1 |
| `b00b8915-2786-43de-0002-1ba2441cb00b` | Ethanol Characteristic 2 |
| `b00b8915-2786-43de-FFFF-1ba2441cb00b` | Device Info / Control characteristic |

### BLE Device Discovery
```
[INFO][%s] SEFI Device: %s (%s) | Type: %s | Version: %u | RSSI: %d | %s | %s
[DEBUG][%s] %d SEFI devices in discovery list
[INFO][%s] Started async BLE scanning (30 seconds with callback)
```

### BLE API Endpoints
- `/api/ble/connect`
- `/api/ble/discovered`
- `/api/ble/forget`
- `/api/ble/rename`
- `/api/ble/saved`
- `/api/ble/status`

### BLE Device Name
`SEFI_H` appears as a BLE device identifier/prefix.

---

## 6. ECU Connection/Pairing Sequence

### Full Connection Flow
1. **BLE Scan** -- Discover SEFI devices via BLE advertising
2. **BLE Connect** -- Connect to target device, discover services
3. **Service Discovery** -- Find SEFI service and ethanol service by UUID
4. **Subscribe Notifications** -- Subscribe to characteristic notifications
5. **Tester Present** -- Send UDS 0x3E to discover ECU on CAN bus
6. **ECU Identification** -- Read VIN, Build ID, Serial Number, Software/Hardware Version
7. **Boxcode Lookup** -- Match ECU build identification to boxcode configuration
8. **Pairing** -- Store VIN and ECU versions for future validation
9. **Extended Diagnostic Session** -- Enter 0x10 03
10. **Security Access** -- SA2 seed/key exchange
11. **Live Mode** -- Enable ECU live mode for logging
12. **Patch Info** -- Read patch version and log buffer address
13. **Logger Configuration** -- Configure dynamic logger variables
14. **Polling Loop** -- Continuous logger data polling at ~1Hz

### Pairing Validation
```
paired_vin
paired_ecu_build_identification
paired_ecu_hardware_version
paired_ecu_software_version
```
VIN-locked pairing prevents connecting to wrong ECU.

### Connection States
```
[INFO][%s] State changing from %s to %s
[INFO][%s] Connection Step: %s
[INFO][%s] ECU discovered in %lu ms
[INFO][%s] Connection to unpatched ECU established in %lu ms
[INFO][%s] Connection to unsupported ECU established in %lu ms
[INFO][%s] Live mode enabled in %lu ms
```

### Unpatched/Unsupported ECU Handling
The firmware gracefully handles ECUs that are not patched:
```
[INFO][%s] Live mode enable failed - treating as unpatched ECU
[ERROR][%s] Write to ECU blocked - unpatched ECU
[INFO][%s] Note: Logging and ECU writing will be disabled
```

---

## 7. Security Access (0x27) Deep Dive

### SA2 Bytecode VM
The security access uses a virtual machine that executes bytecode loaded from ODX (Open Diagnostic Exchange) files stored on the filesystem. This is the standard VAG/Continental SA2 algorithm.

### VM Instructions
| Opcode | Mnemonic | Description |
|--------|----------|-------------|
| varies | ADD | Add to register |
| varies | SUB | Subtract from register |
| varies | EOR | XOR register |
| varies | BCC | Branch if carry clear |
| varies | BRA | Branch always |
| varies | FOR | Loop start |
| varies | NEXT | Loop end |

### Safety
```
[ERROR][%s] Execution exceeded safety limit - possible infinite loop
```
The VM has a maximum instruction count to prevent infinite loops.

### Security Database
```
[DEBUG][%s]   - Search ODX security database
[DEBUG][%s]   - Validate security data integrity
[DEBUG][%s] Dynamically loaded security: %s for %s
```
Security parameters are loaded per-boxcode, meaning different ECU software versions may use different SA2 bytecodes.

---

## 8. Scorpion Engine Management Integration

### Architecture
The Scorpion module receives ECU data via BLE notifications and CAN bus, processes it, and drives physical outputs (injectors, methanol, relays).

### Data Flow
ECU variables are received in three forms:
- **Internal values**: Raw ECU data
- **TX values**: Transmitted/processed values
- **Received values**: Final computed values

### ECU Variables Monitored
| Variable | Description |
|----------|-------------|
| Engine Speed (RPM) | Crankshaft speed |
| Fuel Mass | Fuel injection quantity |
| Injector Load | Injector duty cycle/load |
| Gear | Current gear position |
| Lambda1 / Lambda2 | Wideband O2 sensor (bank 1/2) |
| Ethanol | Ethanol content percentage |
| Bank1/Bank2 Trim | Fuel trim values |

### Scorpion Output Subsystems
1. **Port Injection Manager** -- Supplemental port injectors for E85/methanol
2. **Methanol Pump Manager** -- Controls methanol/water injection pump duty cycle
3. **Methanol Solenoid Manager** -- Controls methanol solenoid valve
4. **Relay Output** -- General purpose relay (fuel pump, etc.)

### Board Types
```
[Scorpion] Initializing outputs for board type: <type>
[Scorpion] Unsupported board type!
```
Multiple hardware board variants are supported.

### Computation
```
[Scorpion] Calculated tables:
[Scorpion] Flex Fuel Trim: <value>
[Scorpion] Master DIFuel Trim: <value>
[Scorpion] Gear Trim: <value>
[Scorpion] New Fuel Mass: <value>
```
The Scorpion module computes fuel trims based on ethanol content and gear, then calculates new fuel mass and injector duty cycles.

### Versioning
```
[Scorpion] Initialized with remote version: <ver>
[Scorpion] Supported versions: <list>
[Scorpion] Unsupported remote version: <ver>
```
Protocol versioning between SEFI dongle and Scorpion ECU piggyback.

---

## 9. API Endpoints (scorpionefi.com)

### Cloud API Base
`https://api.scorpionefi.com/api/v1/`

### Endpoints
| URL | Purpose |
|-----|---------|
| `https://api.scorpionefi.com/api/v1/devices/register` | Device registration |
| `https://api.scorpionefi.com/api/v1/updates/check` | Check for firmware/calibration updates |
| `https://api.scorpionefi.com/api/v1/updates/download/` | Download update packages |

### Authentication
```
auth_token
[INFO][%s] Auth token: %s
[INFO][%s] Added authorization header to update check request
[INFO][%s] Added authorization header to update download request
Authorization: <token>
```
Bearer token authentication for cloud API calls.

### Device Registration
Registers device with user info:
```
[INFO][%s] Registering device for user: %s
[INFO][%s] Company: %s
[INFO][%s] Email: %s
[INFO][%s] Phone: %s
```

---

## 10. Local REST API Endpoints

### Complete API Map

**ECU Control**
- `POST /api/ecu/pair` -- Pair with ECU (VIN lock)
- `GET /api/ecu/status` -- ECU connection status

**BLE Management**
- `POST /api/ble/connect` -- Connect to BLE device
- `GET /api/ble/discovered` -- List discovered SEFI devices
- `POST /api/ble/forget` -- Forget saved device
- `POST /api/ble/rename` -- Rename saved device
- `GET /api/ble/saved` -- List saved devices
- `GET /api/ble/status` -- BLE connection status

**WiFi**
- `POST /api/wifi/connect` -- Connect to WiFi network
- `POST /api/wifi/disconnect` -- Disconnect from WiFi
- `GET /api/wifi/scan` -- Scan for networks
- `GET /api/wifi/status` -- WiFi status

**Calibration**
- `GET /api/calibration/active` -- Active calibration
- `DELETE /api/calibration/delete` -- Delete calibration
- `GET /api/calibration/list` -- List calibrations
- `GET /api/calibration/metadata` -- Calibration metadata
- `GET /api/calibration/table` -- Read calibration table
- `POST /api/calibration/table` -- Write calibration table
- `GET /api/calibration/tables` -- List tables in calibration
- `POST /api/calibration/upload` -- Upload calibration file

**Diagnostics**
- `POST /api/diagnostics/clear` -- Clear all DTCs
- `POST /api/diagnostics/clear_dtc` -- Clear specific DTC
- `GET /api/diagnostics/count` -- DTC count
- `GET /api/diagnostics/dtcs` -- Read DTCs
- `GET /api/diagnostics/snapshot` -- DTC freeze frame
- `GET /api/diagnostics/status` -- Diagnostics status
- `GET /api/diagnostics/supported` -- Supported DTCs

**Ethanol**
- `POST /api/ethanol/clear` -- Clear ethanol value
- `POST /api/ethanol/set` -- Set manual ethanol percentage
- `GET /api/ethanol/source` -- Ethanol source (sensor/manual)
- `GET /api/ethanol/status` -- Ethanol status

**Logger**
- `GET /api/logger/presets` -- Logger presets
- `POST /api/logger/variables` -- Configure logger variables

**System**
- `GET /api/status` -- System status
- `GET /api/version` -- Firmware version
- `POST /api/reboot` -- Reboot device
- `POST /api/reset_config` -- Factory reset
- `GET /api/board_role` -- Get board role
- `POST /api/board_role` -- Set board role
- `GET /api/partition` -- Partition info
- `POST /api/switch_app` -- Switch OTA partition
- `POST /api/update/check` -- Check for updates
- `POST /api/update/download` -- Download update
- `POST /api/registration/register` -- Register device

**Development**
- `POST /api/mock` -- Set mock value
- `POST /api/mock/clear` -- Clear mocks
- `GET /api/mock/list` -- List active mocks

---

## 11. Ethanol / Flex Fuel System

### Architecture
The firmware manages ethanol content from two sources:
1. **BLE Ethanol Sensor** -- External flex fuel sensor connected via BLE
2. **Manual Input** -- User-set ethanol percentage via API

### Ethanol BLE Service
UUID: `b00b8915-2786-43de-0000-1ba2441cb00b`

NVS keys:
- `ethanol_sensor` / `ethanol_sensor_raw` / `ethanol_sensor_valid`
- `ethanol_signal_present`
- `ethanol_source` (sensor vs manual)
- `ethanol_ble_connected` / `ethanol_ble_data_fresh`
- `ethanol_disconnect_time`

### Ethanol Map Blending
The firmware blends gasoline and ethanol calibration maps based on ethanol percentage:
```
[DEBUG][%s] Flex map #%d: original=0x%08X, gasoline=0x%08X, ethanol=0x%08X, blend_map=0x%08X, dims=%ux%u
[INFO][%s] Blending ethanol maps with sensor value: %.2f%%
[INFO][%s] Blend map at 0x%08X evaluated: ethanol=%.2f%% -> blend=%.4f
```

### Ethanol Validation (SBF Compatibility)
Before syncing ethanol to ECU, the firmware validates compatibility:
```
[INFO][%s] Ethanol validation: ECU=0x%04X, mask=0x%04X, extracted=0x%04X, expected=0x%04X
[INFO][%s] Ethanol validation PASSED - SBF is compatible with ECU
[ERROR][%s] Ethanol validation FAILED - SBF is NOT compatible with ECU
```

### ECU Ethanol Write
```
[INFO][%s] Writing ethanol value with validation bits preserved: scaled=%u, final=%u to address 0x%X
[INFO][%s] Writing scaled ethanol value %u to address 0x%X (no validation)
```

---

## 12. Calibration File Formats

### BDEF (Binary Definition File)
```
[INFO][%s] BDEF file initialized: version=%u, size=%u, segments=%u, inverse_segments=%u
[ERROR][%s] Invalid signature: %c%c%c%c (expected BDEF)
```
Signature: `BDEF` (4 bytes)
Contains: segments, inverse segments, pre-calibration data, post-calibration data.

### SCAL (Scorpion Calibration File)
```
[INFO][%s] SCAL file initialized: version=%u, size=%u, flex_maps=%u
[ERROR][%s] Invalid signature: %c%c%c%c (expected SCAL)
```
Signature: `SCAL` (4 bytes)
Contains: flex fuel maps (gasoline + ethanol variants).

### Filesystem Layout
- `/data` -- Read-only data partition (BDEF files, boxcode configs)
- `/cal` -- Read-write calibration partition (user calibrations, SCAL files)
- Both use **LittleFS** filesystem

---

## 13. ECU Variables (Logger)

### MDG1 ECU Parameters (Audi RS7 C8 4.0 TFSI)
The firmware logs these ECU parameters (from OBD/UDS diagnostic descriptions):

| Parameter | Description |
|-----------|-------------|
| Engine speed | Crankshaft RPM |
| Engine temperature | Coolant temperature |
| Vehicle speed | Vehicle speed |
| Wheel speed (FL/FR/RL/RR) | Individual wheel speeds |
| Lambda Sensor B1/B2 | Wideband O2 (bank 1 & 2) |
| Short Term Fuel Trim B1/B2 | STFT bank 1 & 2 |
| Long term fuel trim bank1/2 | LTFT bank 1 & 2 |
| Boost pressure | Actual/set turbo boost |
| Intake air temperature | IAT sensor |
| Oil temperature | Engine oil temp |
| Oil level | Oil level sensor |
| Fuel level | Fuel tank level |
| Throttle position | Absolute throttle % |
| Ignition angle output array | Per-cylinder timing |
| Injection Start Angle S0 | Injection timing |
| Output injection time | Injector pulse width |
| Exhaust gas temperature | EGT bank 1 & 2 |
| Misfire Counter | Per-cylinder misfire |
| Knock retard | Per-cylinder knock retard |
| Indicated engine torque | Actual torque |
| Maximum attainable torque | Torque limit |
| Duty cycle boost pressure | Wastegate duty cycle |
| Longitudinal acceleration | G-force from CAN |
| Ethanol content | Fuel ethanol % |
| Relative air charge | Predicted air charge |
| Fuel mass (relative) | Relative fuel limitation |
| Gear information | Transmission gear |
| Brake pressure | Master cylinder pressure |

### Logger Configuration
```
[INFO][%s] Logger configuration built successfully: %d variables
[INFO][%s] Variables: %d required + %d optional = %d total
[INFO][%s] Group %d (0x%04X): %d variables
[INFO][%s] Built config payload: %d bytes, %d groups, %d vars
```
Variables are grouped by ECU address groups, with required variables (always logged) and optional variables (user-configurable).

---

## 14. OTA Update System

### Dual Partition Scheme
- Two app partitions for A/B OTA updates
- Two calibration LittleFS partitions with atomic swap
- Rollback support

### Update Flow
1. Check for updates via cloud API
2. Download compressed update (gzip/zlib)
3. SHA256 checksum verification
4. Write to next OTA partition
5. Atomic swap of calibration files
6. Set next boot partition
7. Reboot

---

## 15. Flash/Programming ECU Sequence

The firmware can flash/reprogram the ECU:

1. Enter Extended Diagnostic Session (0x10 03)
2. Enter Programming Session (0x10 02)
3. Request Seed (0x27 01)
4. Send Key (0x27 02) -- SA2 bytecode computation
5. Check Preconditions
6. Verify Dependencies
7. Erase Block
8. Start Transfer (0x34 - Request Download)
9. Transfer Data (0x36) -- chunked, ISO-TP segmented
10. End Transfer (0x37 - Request Transfer Exit)
11. Verify Checksum
12. Reset ECU (0x11)

```
[INFO][%s] Suspending normal UDS operations for flashing
[INFO][%s] Flash step: %s -> %s
[INFO][%s] ECU reset initiated - flash completed
[INFO][%s] Resuming normal UDS operations
```

---

## 16. Key Findings for ESP-IDF CAN Debugging

### What the old firmware does RIGHT:
1. Uses `twai_driver_install_v2` (v2 API)
2. Uses `TWAI_CLK_SRC_APB` as clock source
3. Validates GPIO with `GPIO_IS_VALID_OUTPUT_GPIO`
4. CAN init is a simple two-step: install driver, then start driver
5. Uses `twai_transmit_v2` for sending frames
6. Has flow control timeout handling for ISO-TP
7. Runs on Arduino framework which may configure GPIO matrix differently

### Things to verify in the ESP-IDF rewrite:
1. **GPIO pin assignment** -- Confirm TX=17, RX=18 (or whatever the PCB uses)
2. **Clock source** -- Must be `TWAI_CLK_SRC_APB` (80 MHz)
3. **Baud rate timing** -- 500 kbit/s with APB clock: BRP=8, TSEG1=15, TSEG2=4, SJW=3 (typical)
4. **CAN transceiver** -- NCV7344 needs no special enable pin (always active in normal mode)
5. **GPIO matrix vs direct I/O** -- Arduino may use `gpio_matrix_out()` / `gpio_matrix_in()` differently
6. **Pull-ups/pull-downs** -- Check if the old firmware enables internal pull-ups on CAN RX
7. **TWAI operating mode** -- Must be `TWAI_MODE_NORMAL` (not listen-only)
8. **Interrupt handling** -- `esp_intr_enable(p_twai_obj->isr_handle)` and `esp_intr_disable(p_twai_obj->isr_handle)` are used

### Recommended debugging approach:
1. Start with Tester Present (0x3E 00) -- simplest UDS message
2. Verify with a CAN bus analyzer that frames are actually being transmitted
3. Check that the NCV7344 transceiver has proper power (3.3V VCC, 5V VBAT for CAN bus levels)
4. Verify termination resistor (120 ohm) if standalone testing
