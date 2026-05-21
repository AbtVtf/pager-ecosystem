# Host-side validation for FW-014 identity / FW-015 Ed25519 building block.
#
#   make            # build
#   make test       # build + run

ROOT   := $(abspath ../../..)
CRYPTO := $(ROOT)/firmware/components/crypto

BUILD := build
BIN   := $(BUILD)/identity_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -O2 \
          -I$(CRYPTO)/src

SRCS := identity_test.c \
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
