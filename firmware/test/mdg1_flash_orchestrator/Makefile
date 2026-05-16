# Host-side tests for mdg1_flash_orchestrator + mdg1_variant_manifest
# + mdg1_transport_shadow + integration with mdg1_payload + lzrb.
#
# Pulls tiny_aes from firmware/test/mdg1_payload/ (already-vendored
# host AES backend).
#
# All sources are pure C, no ESP-IDF dependency. Host shims via:
#   -DMDG1_PAYLOAD_HOST_BUILD=1
#   -DMDG1_VARIANT_MANIFEST_HOST_BUILD=1
#   -DMDG1_FLASH_ORCHESTRATOR_HOST_BUILD=1

HERE         := $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
FW_SRC       := $(HERE)../../src
FLASH_SRC    := $(FW_SRC)/flash
CFG_SRC      := $(FW_SRC)/config
TEST_SRC     := $(HERE)..
PAYLOAD_TEST := $(HERE)../mdg1_payload

CC           ?= cc
CFLAGS       ?= -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-function -O0 -g \
                -DMDG1_PAYLOAD_HOST_BUILD=1 \
                -DMDG1_VARIANT_MANIFEST_HOST_BUILD=1 \
                -DMDG1_FLASH_ORCHESTRATOR_HOST_BUILD=1
INCLUDES     := -I$(FLASH_SRC) -I$(CFG_SRC) -I$(PAYLOAD_TEST) -I$(HERE)

# NOTE: mdg1_transport_can.c and mdg1_aes_mbedtls.c are firmware-only —
# they self-exclude on HOST_BUILD via #ifndef guard. Listing them here
# would add empty TUs to the build; we just omit them.
SOURCES      := \
    $(FLASH_SRC)/lzrb.c \
    $(FLASH_SRC)/mdg1_payload.c \
    $(FLASH_SRC)/mdg1_variant_manifest.c \
    $(FLASH_SRC)/mdg1_transport_shadow.c \
    $(FLASH_SRC)/mdg1_flash_orchestrator.c \
    $(PAYLOAD_TEST)/tiny_aes.c \
    $(HERE)test_orchestrator.c

BIN := $(HERE)host_test_runner

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) $(SOURCES) -o $(BIN)

run: $(BIN)
	$(BIN)

clean:
	rm -f $(BIN)
