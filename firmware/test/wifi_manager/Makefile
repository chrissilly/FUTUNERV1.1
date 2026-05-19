# Makefile — host-side unit test harness for the WiFi mode-intent
# + STA-creds APIs added in wifi_ap.c (2026-05-17).
#
# Built and run by firmware/test/wifi_manager/eval.sh. Produces
# host_test_runner in this directory.
#
# Compiles:
#   - wifi_ap.c              (system under test — the new APIs only;
#                             init/start paths are linked but not run)
#   - test_wifi_manager.c    (unit-test cases)
#   - shim/host_shim.c       (in-RAM NVS mock + esp_wifi spies +
#                             feature_manager mock + freertos stubs)
#
# wifi_commands.c is NOT compiled host-side because it depends on cJSON
# (only available in the IDF build). The eval's idf.py build step
# verifies that file compiles cleanly on-target. The wifi_commands
# behavioral surface is exercised at the API tier its handlers delegate
# to. (Matches the vin_pair_commands.c precedent.)

HERE        := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
SHIM_DIR    := $(HERE)shim
FW_SRC      := $(HERE)../../src
WIFI_SRC    := $(FW_SRC)/wifi
CFG_SRC     := $(FW_SRC)/config
TEST_SRC    := $(HERE)..

CC          ?= cc
CFLAGS      ?= -std=c11 -Wall -Wextra -Wpedantic \
               -Wno-unused-parameter \
               -Wno-variadic-macro-arguments-omitted \
               -Wno-gnu-zero-variadic-macro-arguments \
               -DWIFI_AP_HOST_TEST=1 \
               -O0 -g
# Header search order:
#   shim/    → esp_err.h, esp_log.h, esp_wifi.h, freertos/*.h, nvs/*.h, ...
#   src/     → for "wifi/wifi_ap.h" + "config/wifi_config.h" includes
#   src/wifi → for the bare "wifi_ap.h" include inside wifi_ap.c
INCLUDES    := -I$(SHIM_DIR) -I$(FW_SRC) -I$(WIFI_SRC) -I$(CFG_SRC)

SOURCES     := \
    $(WIFI_SRC)/wifi_ap.c \
    $(TEST_SRC)/test_wifi_manager.c \
    $(SHIM_DIR)/host_shim.c

BIN := $(HERE)host_test_runner

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) $(SOURCES) -o $(BIN)

run: $(BIN)
	$(BIN)

clean:
	rm -f $(BIN)
