# Makefile — host-side unit tests for the SA2 mini-instruction-set VM.
#
# sa2_vm.c is pure C with no IDF or FreeRTOS dependencies, so the host
# build is just the VM source plus the test harness.

HERE        := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
FW_SRC      := $(HERE)../../src
SA2_SRC     := $(FW_SRC)/flash
TEST_SRC    := $(HERE)..

CC          ?= cc
CFLAGS      ?= -std=c11 -Wall -Wextra -Wpedantic -O0 -g
INCLUDES    := -I$(SA2_SRC)

SOURCES     := \
    $(SA2_SRC)/sa2_vm.c \
    $(TEST_SRC)/test_sa2.c

BIN := $(HERE)host_test_runner

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) $(SOURCES) -o $(BIN)

run: $(BIN)
	$(BIN)

clean:
	rm -f $(BIN)
