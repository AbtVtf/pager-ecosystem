# Host-side unit tests for FW-004 logger format + rotation arithmetic.
#
#   make            # build
#   make test       # build + run

ROOT   := $(abspath ../../..)
LOGGER := $(ROOT)/firmware/components/logger

BUILD := build
BIN   := $(BUILD)/logger_format_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(LOGGER)/src

SRCS := logger_format_test.c $(LOGGER)/src/logger_format.c

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
