# Makefile — host-side round-trip tests for the LZRB codec.
#
# lzrb.c is pure C with no IDF deps, so the host build is just the
# codec source plus the test harness.

HERE        := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
FW_SRC      := $(HERE)../../src
LZRB_SRC    := $(FW_SRC)/flash
TEST_SRC    := $(HERE)..

CC          ?= cc
CFLAGS      ?= -std=c11 -Wall -Wextra -Wpedantic -O0 -g
INCLUDES    := -I$(LZRB_SRC)

SOURCES     := \
    $(LZRB_SRC)/lzrb.c \
    $(TEST_SRC)/test_lzrb.c

BIN := $(HERE)host_test_runner

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) $(SOURCES) -o $(BIN)

run: $(BIN)
	$(BIN)

clean:
	rm -f $(BIN)
