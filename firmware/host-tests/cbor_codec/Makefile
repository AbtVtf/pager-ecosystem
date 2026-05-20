# Host-side conformance build for the PagerOS CBOR codec.
#
# Compiles the same `firmware/components/cbor_codec/` and vendored
# `firmware/components/tinycbor/` translation units with the system gcc so
# the codec can be exercised against the PROTO-003 test vectors without
# requiring ESP-IDF or hardware.
#
#   make            # build the cbor_roundtrip binary
#   make test       # build + walk every vector in protocol/test-vectors/ui/
#   make clean
#
# The vector walker is a thin Python wrapper that uses index.json as the
# source of truth for which vectors are encode / decode_only / negative.

ROOT     := $(abspath ../../..)
CODEC    := $(ROOT)/firmware/components/cbor_codec
TINYCBOR := $(ROOT)/firmware/components/tinycbor
VECTORS  := $(ROOT)/protocol/test-vectors/ui

BUILD := build
BIN   := $(BUILD)/cbor_roundtrip

CC      := gcc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare
OPT     := -O2
CFLAGS  := $(CSTD) $(WARN) $(OPT) \
           -I$(CODEC)/include \
           -I$(TINYCBOR)/include \
           -I$(TINYCBOR)/src \
           -DCBOR_PARSER_MAX_RECURSIONS=16

SRCS := cbor_roundtrip.c \
        $(CODEC)/src/pageros_cbor.c \
        $(TINYCBOR)/src/cborencoder.c \
        $(TINYCBOR)/src/cborencoder_close_container_checked.c \
        $(TINYCBOR)/src/cborencoder_float.c \
        $(TINYCBOR)/src/cborerrorstrings.c \
        $(TINYCBOR)/src/cborparser.c \
        $(TINYCBOR)/src/cborparser_dup_string.c \
        $(TINYCBOR)/src/cborparser_float.c

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

$(BUILD):
	mkdir -p $(BUILD)

test: $(BIN)
	@python3 run_vectors.py --binary $(BIN) --vectors-dir $(VECTORS)

clean:
	rm -rf $(BUILD)
