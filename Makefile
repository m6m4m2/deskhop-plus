# SPDX-License-Identifier: GPL-2.0-only
#
# Host build: the protocol core and its tests. No hardware, no toolchain
# beyond a C compiler -- everything in core/ is written so it can run on a
# development machine with a simulated clock, which is what makes the
# election and failover behaviour testable at all.
#
# For firmware see firmware/rp2040/, which needs the Pico SDK.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wno-sign-conversion -O2 -g
INCS     = -Icore/include -Itests

CORE_SRC = $(wildcard core/src/*.c)
TEST_SRC = $(wildcard tests/*.c)

BUILD    = build
TEST_BIN = $(BUILD)/dhp-tests

.PHONY: all test sanitize clean fmt-check

all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(TEST_BIN): $(CORE_SRC) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INCS) $^ -o $@

test: $(TEST_BIN)
	@$(TEST_BIN)

# The protocol stack is full of buffer arithmetic and fixed tables, so the
# sanitised run is the one that counts.
sanitize: $(CORE_SRC) $(TEST_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined \
	      -fno-omit-frame-pointer $(INCS) $^ -o $(BUILD)/dhp-tests-san
	@$(BUILD)/dhp-tests-san

# core/ must stay freestanding: it is compiled for a Cortex-M0+ with no libc
# beyond string.h, so anything that creeps in gets caught here.
freestanding: | $(BUILD)
	@for f in $(CORE_SRC); do \
	  $(CC) $(CFLAGS) $(INCS) -ffreestanding -c $$f -o /dev/null || exit 1; \
	done
	@echo "core/ builds freestanding"

clean:
	rm -rf $(BUILD)
