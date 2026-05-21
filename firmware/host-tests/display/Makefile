# Host-side unit tests for FW-005 display gradient + clip math.
#
#   make            # build
#   make test       # build + run

ROOT    := $(abspath ../../..)
DISPLAY := $(ROOT)/firmware/components/display

BUILD := build
BIN   := $(BUILD)/display_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(DISPLAY)/src

SRCS := display_test.c $(DISPLAY)/src/gradient.c

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
