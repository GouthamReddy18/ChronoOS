CC ?= cc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDFLAGS ?=

BUILD_DIR := build
CHRONO := $(BUILD_DIR)/chrono
COUNTER := $(BUILD_DIR)/counter_workload

.PHONY: all clean help test

all: $(CHRONO) $(COUNTER)

$(BUILD_DIR):
	mkdir -p $@

$(CHRONO): src/main.c src/process.c include/chrono/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/process.c

$(COUNTER): tests/counter_workload.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/test_process: tests/test_process.c src/process.c src/process_internal.h include/chrono/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_process.c src/process.c

$(BUILD_DIR)/registry_fixture: tests/registry_fixture.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

test: all $(BUILD_DIR)/test_process $(BUILD_DIR)/registry_fixture
	sh tests/test_phase1.sh

clean:
	rm -f $(CHRONO) $(COUNTER) $(BUILD_DIR)/test_process $(BUILD_DIR)/registry_fixture

help:
	@printf '%s\n' 'Targets:' '  all    Build chrono and counter_workload' '  test   Build and run Phase 1 and Phase 2 lifecycle tests' '  clean  Remove generated build binaries' '  help   Show this help'
