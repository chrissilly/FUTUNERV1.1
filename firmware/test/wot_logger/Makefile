# Makefile — host-side unit test harness for the WOT logger pipeline.
#
# Built and run by firmware/test/wot_logger/eval.sh. Produces the
# binary `host_test_runner` in this directory.
#
# Compiles:
#   - feature_manager.c      (system under test, real arbiter)
#   - wot_recorder.c         (with WOT_RECORDER_HOST_BUILD = stored DEFLATE body)
#   - wot_uploader.c
#   - wot_logger.c           (with WOT_LOGGER_HOST_BUILD = skip target adapters)
#   - test_wot_logger.c
#   - shim/host_shim.c
#
# wot_log_commands.c is NOT compiled host-side because it depends on
# cJSON (only available in the IDF build). The eval's idf.py build
# step verifies that file compiles cleanly on-target.

HERE        := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
SHIM_DIR    := $(HERE)shim
FW_SRC      := $(HERE)../../src
FM_SRC      := $(FW_SRC)/feature_manager
LOG_SRC     := $(FW_SRC)/logger
CMD_SRC     := $(FW_SRC)/commands
CFG_SRC     := $(FW_SRC)/config
TEST_SRC    := $(HERE)..

CC          ?= cc
CFLAGS      ?= -std=c11 -Wall -Wextra -Wpedantic \
               -Wno-unused-parameter \
               -Wno-variadic-macro-arguments-omitted \
               -Wno-gnu-zero-variadic-macro-arguments \
               -DWOT_RECORDER_HOST_BUILD=1 \
               -DWOT_LOGGER_HOST_BUILD=1 \
               -DWOT_LOG_COMMANDS_HOST_BUILD=1 \
               -O0 -g
INCLUDES    := -I$(SHIM_DIR) -I$(FM_SRC) -I$(LOG_SRC) -I$(CMD_SRC) -I$(CFG_SRC)

SOURCES     := \
    $(FM_SRC)/feature_manager.c \
    $(LOG_SRC)/wot_recorder.c \
    $(LOG_SRC)/wot_uploader.c \
    $(LOG_SRC)/wot_logger.c \
    $(TEST_SRC)/test_wot_logger.c \
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
