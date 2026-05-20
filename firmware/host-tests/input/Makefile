# Host-side unit tests for the FW-007 quadrature decoder.
#
#   make            # build
#   make test       # build + run
#   make clean

ROOT  := $(abspath ../../..)
INPUT := $(ROOT)/firmware/components/input

BUILD := build
BIN   := $(BUILD)/quad_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(INPUT)/src

SRCS := quad_test.c $(INPUT)/src/quad.c

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
