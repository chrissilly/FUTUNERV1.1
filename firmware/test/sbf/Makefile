# Makefile — host-side unit test harness for the SBF live-tune
# orchestrator. Compiles a host_test_runner that exercises the
# orchestrator + applier + blend_engine without linking ANY of the
# frozen scal_file_*/bdef_file_*/ecu_write_* implementations.
#
# Per FROZEN_MODULES.md: the frozen .c files are NEVER compiled into
# this harness. The test installs its own sbf_loader_iface_t mock and
# its own ecu_write fn pointer.
#
# Compiles:
#   - feature_manager.c           (real arbiter)
#   - license.c                   (real license module — gate path)
#   - blend_engine.c              (real pure math)
#   - sbf_loader.c                (gated to host build — target_iface stub)
#   - sbf_applier.c               (real apply orchestration)
#   - sbf_downloader.c            (compiled but mocked from test)
#   - sbf_orchestrator.c          (gated to host build — no FreeRTOS task)
#   - sbf_variants.c              (real lookup; test_override seeds custom)
#   - test_sbf_orchestrator.c     (the unit test cases)
#   - shim/host_shim.c            (FreeRTOS / mutex stubs)
#
# sbf_commands.c is NOT compiled host-side because it depends on cJSON.
# The eval's idf.py build verifies it compiles on-target.

HERE        := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
SHIM_DIR    := $(HERE)shim
FW_SRC      := $(HERE)../../src
FM_SRC      := $(FW_SRC)/feature_manager
LIC_SRC     := $(FW_SRC)/license
SBF_SRC     := $(FW_SRC)/sbf
FLEX_SRC    := $(FW_SRC)/flex_fuel
CFG_SRC     := $(FW_SRC)/config
TEST_SRC    := $(HERE)..

CC          ?= cc
CFLAGS      ?= -std=c11 -Wall -Wextra -Wpedantic \
               -Wno-unused-parameter \
               -Wno-variadic-macro-arguments-omitted \
               -Wno-gnu-zero-variadic-macro-arguments \
               -DSBF_HOST_BUILD=1 \
               -DVIN_PAIRING_HOST_BUILD=1 \
               -O0 -g
INCLUDES    := -I$(SHIM_DIR) -I$(FM_SRC) -I$(LIC_SRC) -I$(SBF_SRC) \
               -I$(FLEX_SRC) -I$(CFG_SRC)

SOURCES     := \
    $(FM_SRC)/feature_manager.c \
    $(LIC_SRC)/license.c \
    $(FLEX_SRC)/blend_engine.c \
    $(SBF_SRC)/sbf_loader.c \
    $(SBF_SRC)/sbf_applier.c \
    $(SBF_SRC)/sbf_downloader.c \
    $(SBF_SRC)/sbf_orchestrator.c \
    $(SBF_SRC)/sbf_variants.c \
    $(TEST_SRC)/test_sbf_orchestrator.c \
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
