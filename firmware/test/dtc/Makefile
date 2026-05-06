# Makefile — host-side unit test harness for the DTC read/clear feature.
#
# Built and run by firmware/test/dtc/eval.sh. Produces the binary
# `host_test_runner` in this directory.
#
# Compiles:
#   - feature_manager.c      (real arbiter, system under test interaction)
#   - dtc_uds.c              (with no host-build define needed: pure C)
#   - dtc_feature.c          (with DTC_FEATURE_HOST_BUILD=1; skips
#                             on-target ISO-TP / CAN adapter wiring)
#   - test_dtc.c             (the unit-test cases)
#   - shim/host_shim.c       (FreeRTOS / mutex stubs for host)
#
# dtc_commands.c is NOT compiled host-side because it depends on
# cJSON (only available in the IDF build). The eval's idf.py build
# step verifies that file compiles cleanly on-target.

HERE        := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
SHIM_DIR    := $(HERE)shim
FW_SRC      := $(HERE)../../src
FM_SRC      := $(FW_SRC)/feature_manager
DTC_SRC     := $(FW_SRC)/dtc
CFG_SRC     := $(FW_SRC)/config
TEST_SRC    := $(HERE)..

CC          ?= cc
CFLAGS      ?= -std=c11 -Wall -Wextra -Wpedantic \
               -Wno-unused-parameter \
               -Wno-variadic-macro-arguments-omitted \
               -Wno-gnu-zero-variadic-macro-arguments \
               -DDTC_FEATURE_HOST_BUILD=1 \
               -O0 -g
INCLUDES    := -I$(SHIM_DIR) -I$(FM_SRC) -I$(DTC_SRC) -I$(CFG_SRC)

SOURCES     := \
    $(FM_SRC)/feature_manager.c \
    $(DTC_SRC)/dtc_uds.c \
    $(DTC_SRC)/dtc_feature.c \
    $(TEST_SRC)/test_dtc.c \
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
