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

$(CHRONO): src/main.c src/process.c src/checkpoint.c src/checkpoint_criu.c include/chrono/process.h include/chrono/checkpoint.h include/chrono/checkpoint_criu.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/process.c src/checkpoint.c src/checkpoint_criu.c

$(COUNTER): tests/counter_workload.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/test_process: tests/test_process.c src/process.c src/process_internal.h include/chrono/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_process.c src/process.c

$(BUILD_DIR)/registry_fixture: tests/registry_fixture.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/fake_criu: tests/fake_criu.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/test_criu: tests/test_criu.c src/checkpoint.c src/checkpoint_criu.c include/chrono/checkpoint.h include/chrono/checkpoint_criu.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_criu.c src/checkpoint.c src/checkpoint_criu.c

test: all $(BUILD_DIR)/test_process $(BUILD_DIR)/registry_fixture $(BUILD_DIR)/test_checkpoint $(BUILD_DIR)/fake_criu $(BUILD_DIR)/test_criu
	sh tests/test_phase1.sh
	$(BUILD_DIR)/test_checkpoint
	$(BUILD_DIR)/test_criu $(BUILD_DIR)/fake_criu

$(BUILD_DIR)/test_checkpoint: tests/test_checkpoint.c src/checkpoint.c src/checkpoint_criu.c include/chrono/checkpoint.h include/chrono/checkpoint_criu.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_checkpoint.c src/checkpoint.c src/checkpoint_criu.c

clean:
	rm -f $(CHRONO) $(COUNTER) $(BUILD_DIR)/test_process $(BUILD_DIR)/registry_fixture $(BUILD_DIR)/test_checkpoint $(BUILD_DIR)/fake_criu $(BUILD_DIR)/test_criu

help:
	@printf '%s\n' 'Targets:' '  all    Build chrono and counter_workload' '  test   Build and run regression, metadata, and fake CRIU adapter tests' '  clean  Remove generated build binaries' '  help   Show this help'
