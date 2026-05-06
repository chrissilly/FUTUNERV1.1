# Makefile — host-side unit test harness for the feature manager.
#
# Built and run by firmware/test/feature_manager/eval.sh. Produces the
# binary `host_test_runner` in this directory; the eval script invokes
# that binary and treats a non-zero exit as failure.
#
# Compiles three translation units:
#   1. feature_manager.c     (system under test)
#   2. test_feature_manager.c (the test cases)
#   3. shim/host_shim.c      (FreeRTOS / mutex stubs for host)
#
# Header search order:
#   shim/                — esp_err.h, esp_log.h, freertos/*.h
#   ../../src/feature_manager  — feature_manager.h
#   ../../src/config            — feature_manager_config.h

HERE       := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
SHIM_DIR   := $(HERE)shim
FW_SRC     := $(HERE)../../src
FM_SRC     := $(FW_SRC)/feature_manager
CFG_SRC    := $(FW_SRC)/config
TEST_SRC   := $(HERE)..

CC         ?= cc
CFLAGS     ?= -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-parameter \
              -Wno-variadic-macro-arguments-omitted -Wno-gnu-zero-variadic-macro-arguments \
              -O0 -g
INCLUDES   := -I$(SHIM_DIR) -I$(FM_SRC) -I$(CFG_SRC)
SOURCES    := \
    $(FM_SRC)/feature_manager.c \
    $(TEST_SRC)/test_feature_manager.c \
    $(SHIM_DIR)/host_shim.c

BIN := $(HERE)host_test_runner

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SOURCES) $(SHIM_DIR)/esp_err.h $(SHIM_DIR)/esp_log.h \
        $(SHIM_DIR)/freertos/FreeRTOS.h $(SHIM_DIR)/freertos/semphr.h \
        $(SHIM_DIR)/freertos/task.h \
        $(FM_SRC)/feature_manager.h $(CFG_SRC)/feature_manager_config.h
	$(CC) $(CFLAGS) $(INCLUDES) $(SOURCES) -o $(BIN)

run: $(BIN)
	$(BIN)

clean:
	rm -f $(BIN)
