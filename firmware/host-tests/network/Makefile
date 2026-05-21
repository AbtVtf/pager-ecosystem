# Host-side unit tests for the FW-009 URL guard.
#
#   make            # build
#   make test       # build + run

ROOT    := $(abspath ../../..)
NETWORK := $(ROOT)/firmware/components/network

BUILD := build
BIN   := $(BUILD)/network_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(NETWORK)/src

SRCS := network_test.c $(NETWORK)/src/url.c

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
