# Host-side unit tests for FW-008 LoRa parameter helpers.
#
#   make            # build
#   make test       # build + run

ROOT := $(abspath ../../..)
LORA := $(ROOT)/firmware/components/lora

BUILD := build
BIN   := $(BUILD)/lora_params_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(LORA)/include -I$(LORA)/src \
          -I$(ROOT)/firmware/components/lora/include

SRCS := lora_params_test.c $(LORA)/src/lora_params.c

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
