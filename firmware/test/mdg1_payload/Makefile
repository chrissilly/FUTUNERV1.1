# Makefile — host-side tests for mdg1_payload.
#
# Builds: tiny_aes (host AES) + lzrb (codec) + mdg1_payload + test driver
# into one host_test_runner binary that exercises all 9 scenarios.
#
# All sources are pure C with no ESP-IDF dependency. The host-build
# shim for esp_err_t lives inside mdg1_payload.h, gated by
# MDG1_PAYLOAD_HOST_BUILD which this Makefile sets.

HERE        := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
FW_SRC      := $(HERE)../../src
FLASH_SRC   := $(FW_SRC)/flash
CFG_SRC     := $(FW_SRC)/config
TEST_SRC    := $(HERE)..

CC          ?= cc
CFLAGS      ?= -std=c11 -Wall -Wextra -Wpedantic -O0 -g \
               -DMDG1_PAYLOAD_HOST_BUILD=1
INCLUDES    := -I$(FLASH_SRC) -I$(CFG_SRC) -I$(HERE)

SOURCES     := \
    $(FLASH_SRC)/lzrb.c \
    $(FLASH_SRC)/mdg1_payload.c \
    $(HERE)tiny_aes.c \
    $(TEST_SRC)/test_mdg1_payload.c

BIN := $(HERE)host_test_runner

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) $(SOURCES) -o $(BIN)

run: $(BIN)
	$(BIN)

clean:
	rm -f $(BIN)
