# Host-side unit tests for the FW-006 TCA8418 decoder.
#
#   make            # build
#   make test       # build + run
#   make clean

ROOT := $(abspath ../../..)
KBD  := $(ROOT)/firmware/components/keyboard

BUILD := build
BIN   := $(BUILD)/tca8418_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(KBD)/src

SRCS := tca8418_test.c $(KBD)/src/tca8418.c

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
