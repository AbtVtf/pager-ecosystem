# Host-side unit tests for FW-003 path walk.
#
#   make            # build
#   make test       # build + run

ROOT    := $(abspath ../../..)
STORAGE := $(ROOT)/firmware/components/storage

BUILD := build
BIN   := $(BUILD)/storage_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(STORAGE)/src

SRCS := storage_test.c $(STORAGE)/src/path.c

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
