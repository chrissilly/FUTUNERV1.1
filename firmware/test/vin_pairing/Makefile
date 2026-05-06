# Makefile — host-side unit test harness for the VIN pairing + license
# modules.
#
# Built and run by firmware/test/vin_pairing/eval.sh. Produces
# host_test_runner in this directory.
#
# Compiles:
#   - feature_manager.c    (real arbiter, system under test interaction)
#   - license.c            (system under test)
#   - vin_pairing.c        (with VIN_PAIRING_HOST_BUILD=1; skips
#                           on-target HTTP / NVS / WiFi adapters)
#   - test_vin_pairing.c   (the unit-test cases)
#   - shim/host_shim.c     (FreeRTOS / mutex stubs for host)
#
# vin_pair_commands.c is NOT compiled host-side because it depends on
# cJSON (only available in the IDF build). The eval's idf.py build
# step verifies that file compiles cleanly on-target.

HERE        := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
SHIM_DIR    := $(HERE)shim
FW_SRC      := $(HERE)../../src
FM_SRC      := $(FW_SRC)/feature_manager
LIC_SRC     := $(FW_SRC)/license
VP_SRC      := $(FW_SRC)/vin_pairing
CFG_SRC     := $(FW_SRC)/config
TEST_SRC    := $(HERE)..

CC          ?= cc
CFLAGS      ?= -std=c11 -Wall -Wextra -Wpedantic \
               -Wno-unused-parameter \
               -Wno-variadic-macro-arguments-omitted \
               -Wno-gnu-zero-variadic-macro-arguments \
               -DVIN_PAIRING_HOST_BUILD=1 \
               -O0 -g
INCLUDES    := -I$(SHIM_DIR) -I$(FM_SRC) -I$(LIC_SRC) -I$(VP_SRC) -I$(CFG_SRC)

SOURCES     := \
    $(FM_SRC)/feature_manager.c \
    $(LIC_SRC)/license.c \
    $(VP_SRC)/vin_pairing.c \
    $(TEST_SRC)/test_vin_pairing.c \
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
