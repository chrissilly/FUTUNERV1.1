# Report: How UDS Went From Working to Broken

> Generated: 2026-04-13
> Last working commit: `7b4e525` (2026-03-29) "feat: CAN working on both boards, full ECU connection, sniffer tool"
> Current broken: `b62fa86` (2026-04-05) "feat: 53-var ECU database, responsive UI, WOT logger, boxcode matrix"
> Time span: 15 days, 8 commits

---

## Timeline (last working → now)

| Commit | Date | Summary | Risk to UDS |
|--------|------|---------|-------------|
| `7b4e525` | 2026-03-29 | **WORKING** baseline | — |
| `aad84d5` | 2026-03-30 | full ECU connection, new UI, **dual WiFi**, CAN sniffer, MDG1 CRC port | **HIGH** — dual WiFi added |
| `1909da6` | 2026-03-30 | value scaling, is_signed, **WiFi STA connect** (Phase 1 polish) | **HIGH** — STA retry loop |
| `ee4590d` | 2026-03-31 | 23 variables, fast-poll 15Hz, multi-rate poll_divisor field | MEDIUM — timing |
| `ad3f544` | 2026-04-01 | 23 vars, fast-poll, sniffer, wifi scan, DTC prep, UI fixes | LOW |
| `417e965` | 2026-04-04 | session checkpoint: bus recovery, reconnect cmd, STA wifi, CAN decode | MEDIUM — bus recovery |
| `b1b57d9` | 2026-04-04 | agents delivered DTC, OTA, flash, UI rebuild, code review | LOW |
| `4c05214` | 2026-04-05 | all agent code compiling — DTC, flash, OTA integrated | LOW |
| `b62fa86` | 2026-04-05 | 53-var ECU database, responsive UI, WOT logger, boxcode matrix | LOW |

---

## The Code Itself Did Not Regress

The discovery state machine is **byte-for-byte identical** to the last working commit:

```c
// connection_manager.c:109 (same in 7b4e525 and HEAD)
static void handle_discovering(void) {
    ESP_LOGI(TAG, "Discovering vehicle...");
    uds_request_t req;
    uds_build_tester_present(&req, UDS_SUBFUNCTION_TESTER_PRESENT_NORMAL, 0);
    send_uds_request(&req);
    change_state(CONN_STATE_WAIT_DISCOVER_RESPONSE);
}
```

- CAN pins: GPIO 17/18 (BOARD_DEVKIT) — confirmed correct via 0 bus errors
- ISO-TP: no changes since working commit
- CAN driver: no changes since working commit
- UDS service IDs: only DTC service added (not in discovery path)

---

## What DID Change (Probable Regression Sources)

### SUSPECT #1: WiFi STA Aggressive Reconnect — HIGHLY LIKELY

**Commit:** `aad84d5` "dual WiFi" + `1909da6` "WiFi STA connect"

Added in `src/wifi/wifi_ap.c`:

```c
if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    sta_connected = false;
    sta_ip_str[0] = '\0';
    /* Retry immediately — no delay in event handler (blocks WiFi task) */
    ESP_LOGW(TAG, "STA disconnected, retrying...");
    esp_wifi_connect();
}
```

**Evidence of damage:** Serial log shows WiFi STA in tight reconnect loop:
```
W (112959) WIFI_AP: STA disconnected, retrying...
I (112969) wifi:state: auth -> assoc (0x0)
I (112969) wifi:state: assoc -> run (0x10)
I (116199) wifi:state: run -> init (0xf00)   <-- 3s later, dies again
W (116209) WIFI_AP: STA disconnected, retrying...
```

The shop WiFi `openc` fails WPA2 4-way handshake (reason=15). STA keeps hammering reconnect attempts every 3 seconds. This:
- Steals CPU cycles from the CAN receive task
- Floods Wi-Fi subsystem with auth/assoc events
- May cause RX buffer overruns on TWAI driver
- Runs `esp_wifi_connect()` from event handler (against ESP-IDF best practices)

**Why this breaks UDS specifically:** The ECU responds within ~50ms of our TesterPresent. If the CAN RX task is CPU-starved during that window, we miss the response, and the next discovery retry sees nothing.

In the working commit (7b4e525), STA mode did not exist — only AP mode. No retry loop.

### SUSPECT #2: Fast-poll Tight Loop

**Commit:** `ee4590d` "fast-poll 15Hz"

Added in `src/main.c`:
```c
-        vTaskDelay(pdMS_TO_TICKS(10));      // old: 10ms between CAN task iterations
+        vTaskDelay(1);                        // new: 1 tick = 10ms at HZ=100
```

At FreeRTOS HZ=100, `vTaskDelay(1)` = 10ms — same as before. But the intent was faster polling. Combined with the new `fast_poll_loop()` in connection_manager (553 lines added), the CAN task is now much hotter.

### SUSPECT #3: Bus Recovery Reinit

**Commit:** `417e965` "bus recovery, reconnect cmd"

Added in `connection_manager.c`:
```c
if (discovery_fail_count >= DISCOVERY_FAILS_BEFORE_REINIT) {  // 5 failures
    can_manager_stop();
    can_driver_deinit();
    vTaskDelay(pdMS_TO_TICKS(1000));
    can_driver_init();
    can_manager_start();
    vTaskDelay(pdMS_TO_TICKS(DISCOVERY_BACKOFF_MS));   // 10 seconds
}
```

This cycles the CAN driver every ~25 seconds when discovery fails. Each reinit takes ~1 second. If WiFi STA churn is causing initial failure, this reinit cycle adds to the problem — driver churn + 10s dead time during which the ECU might move on.

---

## How To Prove The Hypothesis

Checkout `aad84d5` (first commit with dual WiFi). If UDS works there but breaks at `1909da6` or later, it's the WiFi STA retry. If it breaks already at `aad84d5`, it's the APSTA netif creation.

```bash
git checkout aad84d5 -- src/
idf.py build && idf.py flash
# Test — does UDS discover vehicle?

git checkout 1909da6 -- src/
idf.py build && idf.py flash
# Test again
```

---

## Quick Fix (Minimum Viable)

If the WiFi STA retry is the problem, add a delay and a max retry count in `wifi_ap.c`:

```c
static uint8_t sta_retry_count = 0;
#define STA_MAX_RETRIES 3
#define STA_RETRY_DELAY_MS 30000

if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
    sta_connected = false;
    if (sta_retry_count < STA_MAX_RETRIES) {
        sta_retry_count++;
        ESP_LOGW(TAG, "STA disconnected (attempt %d/%d), retrying in %dms",
                 sta_retry_count, STA_MAX_RETRIES, STA_RETRY_DELAY_MS);
        /* Defer reconnect to a task, not event handler */
        esp_timer_handle_t timer;
        esp_timer_create(&(esp_timer_create_args_t){
            .callback = deferred_reconnect_cb,
            .dispatch_method = ESP_TIMER_TASK,
        }, &timer);
        esp_timer_start_once(timer, STA_RETRY_DELAY_MS * 1000);
    } else {
        ESP_LOGW(TAG, "STA retry limit reached, giving up");
    }
}
```

Desktop reports they already implemented "5-retry backoff, stops hammering on persistent failure." But the retry backoff may still be too aggressive, or a second bug is at play.

---

## The Real Diagnostic Path Forward

1. **Dump working firmware** from preserved dongle (in progress — `firmware_dumps/KNOWN_WORKING_UDS.bin`)
2. **Extract app_desc** from the BIN to find exact git hash it was built from
3. **Checkout that hash**, build, flash, verify UDS works
4. **Git bisect** from that hash to HEAD to find the first breaking commit
5. **Review the diff** of the breaking commit for the actual regression

Once we know the commit that broke things, the fix is targeted.

---

## Working Dongle Firmware Dump

In progress at `/Users/rabbit/esp/obd/SEFIv1/firmware_dumps/KNOWN_WORKING_UDS.bin`.

To identify the commit hash:
```bash
# ESP-IDF embeds app_desc at offset 0x20 of the app partition
# Includes: project_name, version, idf_ver, date, time, app_elf_sha256
dd if=firmware_dumps/KNOWN_WORKING_UDS.bin bs=1 skip=0x10020 count=256 2>/dev/null | strings
```

Or via esptool:
```bash
esptool.py image_info firmware_dumps/KNOWN_WORKING_UDS.bin
```
