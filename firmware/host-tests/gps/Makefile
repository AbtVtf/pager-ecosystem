# Host-side unit tests for the PagerOS NMEA parser.
#
#   make            # build
#   make test       # build + run
#   make clean

ROOT  := $(abspath ../../..)
GPS   := $(ROOT)/firmware/components/gps

BUILD := build
BIN   := $(BUILD)/nmea_test

CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 \
          -I$(GPS)/src

SRCS := nmea_test.c $(GPS)/src/nmea.c

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

$(BUILD):
	mkdir -p $(BUILD)

test: $(BIN)
	@./$(BIN)

clean:
	rm -rf $(BUILD)
