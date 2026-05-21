# Host-side validation for FW-015 crypto primitive wrappers.
#
#   make            # build
#   make test       # build + run

ROOT   := $(abspath ../../..)
CRYPTO := $(ROOT)/firmware/components/crypto

BUILD := build
BIN   := $(BUILD)/crypto_test

CC     := gcc
CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(CRYPTO)/include -I$(CRYPTO)/src

SRCS := crypto_test.c \
        $(CRYPTO)/src/pageros_crypto.c \
        $(CRYPTO)/src/monocypher.c \
        $(CRYPTO)/src/monocypher-ed25519.c

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

$(BUILD):
	mkdir -p $(BUILD)

test: $(BIN)
	@./$(BIN)

clean:
	rm -rf $(BUILD)
