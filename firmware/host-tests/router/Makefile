# Host-side unit tests for the FW-024 input router dispatch contract.
#
#   make            # build
#   make test       # build + run
#   make clean

ROOT   := $(abspath ../../..)
ROUTER := $(ROOT)/firmware/components/input_router

BUILD := build
BIN   := $(BUILD)/router_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(ROUTER)/include

SRCS := router_test.c $(ROUTER)/src/dispatch.c

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
