CC ?= cc
CPPFLAGS ?= -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDFLAGS ?=

BUILD_DIR := build
CHRONO := $(BUILD_DIR)/chrono
COUNTER := $(BUILD_DIR)/counter_workload

.PHONY: all clean help test test-criu-integration

all: $(CHRONO) $(COUNTER)

$(BUILD_DIR):
	mkdir -p $@

$(CHRONO): src/main.c src/demo.c src/demo_manual.c src/demo_dashboard.c src/demo_dashboard.h src/demo_format.c src/supervisor.c src/supervisor_service.c src/process.c src/checkpoint.c src/checkpoint_criu.c include/chrono/process.h include/chrono/process_control.h include/chrono/supervisor.h include/chrono/supervisor_service.h include/chrono/checkpoint.h include/chrono/checkpoint_criu.h include/chrono/demo.h src/demo_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/demo.c src/demo_manual.c src/demo_dashboard.c src/demo_format.c src/supervisor.c src/supervisor_service.c src/process.c src/checkpoint.c src/checkpoint_criu.c

$(COUNTER): tests/counter_workload.c include/chrono/process_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/test_process: tests/test_process.c src/process.c src/process_internal.h include/chrono/process.h include/chrono/process_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_process.c src/process.c

$(BUILD_DIR)/registry_fixture: tests/registry_fixture.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/fake_criu: tests/fake_criu.c src/process.c include/chrono/process.h include/chrono/process_control.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/fake_criu.c src/process.c

$(BUILD_DIR)/test_criu: tests/test_criu.c src/process.c src/checkpoint.c src/checkpoint_criu.c $(COUNTER) include/chrono/process.h include/chrono/process_control.h include/chrono/checkpoint.h include/chrono/checkpoint_criu.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_criu.c src/process.c src/checkpoint.c src/checkpoint_criu.c

$(BUILD_DIR)/test_demo_format: tests/test_demo_format.c src/demo_format.c src/demo_dashboard.c src/demo_internal.h src/demo_dashboard.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_demo_format.c src/demo_format.c src/demo_dashboard.c

$(BUILD_DIR)/test_dashboard_process: tests/test_dashboard_process.c src/demo_dashboard.c src/demo_format.c src/demo_dashboard.h src/demo_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_dashboard_process.c src/demo_dashboard.c src/demo_format.c

$(BUILD_DIR)/test_dashboard_terminal: tests/test_dashboard_terminal.c src/demo_dashboard.c src/demo_format.c src/demo_dashboard.h src/demo_internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_dashboard_terminal.c src/demo_dashboard.c src/demo_format.c

$(BUILD_DIR)/test_supervisor: tests/test_supervisor.c src/supervisor.c src/process.c include/chrono/supervisor.h include/chrono/process.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_supervisor.c src/supervisor.c src/process.c

test: all $(BUILD_DIR)/test_process $(BUILD_DIR)/registry_fixture $(BUILD_DIR)/test_checkpoint $(BUILD_DIR)/fake_criu $(BUILD_DIR)/test_criu $(BUILD_DIR)/verified_stop $(BUILD_DIR)/verified_stop_no_pidfd $(BUILD_DIR)/test_demo_format $(BUILD_DIR)/test_dashboard_process $(BUILD_DIR)/test_dashboard_terminal $(BUILD_DIR)/test_supervisor
	sh tests/test_phase1.sh
	sh tests/test_cli_checkpoint_path.sh
	sh tests/test_launch_identity.sh
	sh tests/test_counter_workload.sh
	sh tests/test_verified_stop.sh
	$(BUILD_DIR)/test_checkpoint
	$(BUILD_DIR)/test_criu $(BUILD_DIR)/fake_criu
	$(BUILD_DIR)/test_demo_format
	$(BUILD_DIR)/test_dashboard_process
	$(BUILD_DIR)/test_dashboard_terminal
	$(BUILD_DIR)/test_supervisor
	sh tests/test_demo.sh
	sh tests/test_dashboard_menu.sh
	sh tests/test_supervisor_cli.sh
	sh tests/test_supervisor_service.sh
	sh tests/test_scheduler.sh

test-criu-integration: all $(BUILD_DIR)/verified_stop
	sh tests/test_criu_integration.sh

$(BUILD_DIR)/test_checkpoint: tests/test_checkpoint.c src/process.c src/checkpoint.c src/checkpoint_criu.c include/chrono/process.h include/chrono/checkpoint.h include/chrono/checkpoint_criu.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_checkpoint.c src/process.c src/checkpoint.c src/checkpoint_criu.c

$(BUILD_DIR)/verified_stop: tests/verified_stop.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(BUILD_DIR)/verified_stop_no_pidfd: tests/verified_stop.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DCHRONO_DISABLE_PIDFD $(LDFLAGS) -o $@ $<

clean:
	rm -f $(CHRONO) $(COUNTER) $(BUILD_DIR)/test_process $(BUILD_DIR)/registry_fixture $(BUILD_DIR)/test_checkpoint $(BUILD_DIR)/fake_criu $(BUILD_DIR)/test_criu $(BUILD_DIR)/verified_stop $(BUILD_DIR)/verified_stop_no_pidfd $(BUILD_DIR)/test_demo_format $(BUILD_DIR)/test_dashboard_process $(BUILD_DIR)/test_dashboard_terminal $(BUILD_DIR)/test_supervisor

help:
	@printf '%s\n' 'Targets:' '  all                   Build chrono and counter_workload' '  test                  Build and run process, metadata, fake CRIU, supervisor, and demo tests' '  test-criu-integration Run opt-in real CRIU preflight and disposable workload test' '  clean                 Remove generated build binaries' '  help                  Show this help' 'Commands:' '  ./build/chrono supervisor run|status|shutdown  Run or control the foreground supervisor' '  ./build/chrono protect -- PROGRAM [ARG ...]  Launch and register a workload' '  ./build/chrono list   List registered workloads' '  ./build/chrono status CHRONO_PROCESS_ID  Inspect a registered workload' '  ./build/chrono counter PID CURRENT_STARTTIME [CHECKPOINT_STARTTIME]  Query the in-memory counter over the verified control channel' '  ./build/chrono checkpoint-latest CHRONO_PROCESS_ID  Resolve latest valid checkpoint' '  ./build/chrono stop CHRONO_PROCESS_ID  Safely stop a registered workload' '  ./build/chrono dashboard  Run the progressive managed-counter workflow' '  ./build/chrono demo   Run the bounded managed-process demonstration' '  ./build/chrono demo-interactive  Alias for the same real demonstration' '  ./build/chrono demo-manual  Run the separate key-driven counter workflow'
