#!/bin/sh
set -eu

CHRONO=./build/chrono
FAKE_CRIU="$(pwd)/build/fake_criu"
test_root=$(mktemp -d ./build/dashboard-menu-test-XXXXXX) || {
    echo "test_dashboard_menu: could not create a private test directory" >&2
    exit 1
}
test_root=$(cd "$test_root" && pwd -P)
chmod 700 "$test_root"
dashboard_pid=
cleanup() {
    if [ -n "$dashboard_pid" ]; then
        kill -INT "$dashboard_pid" 2>/dev/null || true
        wait "$dashboard_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT HUP INT TERM

set +e
printf '1\n\n3\n\n4\nr\nq\n5\ny\n\n7\n1\n\n6\ny\n\n8\ny\n8\n10\n\n11\n' |
    TMPDIR="$test_root" CHRONO_CRIU_PATH="$FAKE_CRIU" \
    CHRONO_CRIU_UNPRIVILEGED=1 CHRONO_TEST_ALLOW_FAKE_CRIU=1 \
    CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED=1 \
    CHRONO_FAKE_CRIU_LOG="$test_root/fake-criu.log" \
    "$CHRONO" dashboard >"$test_root/dashboard.log" 2>&1
status=$?
set -e
if [ "$status" -ne 1 ]; then
    cat "$test_root/dashboard.log" >&2
    echo "test_dashboard_menu: fake restore run exited $status, expected unverified status 1" >&2
    exit 1
fi

grep -F 'MAIN MENU' "$test_root/dashboard.log" >/dev/null
grep -F 'PROGRAM IDENTIFICATION' "$test_root/dashboard.log" >/dev/null
grep -F 'SELECTED PROCESS DETAILS' "$test_root/dashboard.log" >/dev/null
grep -F 'PROCESS MONITORING' "$test_root/dashboard.log" >/dev/null
grep -F 'CHECKPOINT MANAGEMENT' "$test_root/dashboard.log" >/dev/null
grep -F 'CHECKPOINT BROWSER' "$test_root/dashboard.log" >/dev/null
grep -F 'PROCESS TERMINATION' "$test_root/dashboard.log" >/dev/null
grep -F 'RESTORE CONFIRMATION' "$test_root/dashboard.log" >/dev/null
grep -F 'RECOVERY REPORT' "$test_root/dashboard.log" >/dev/null
grep -F '[INPUT] Received: 1 - MENU SELECTION' "$test_root/dashboard.log" >/dev/null
grep -F '[INPUT] Received: 10 - MENU SELECTION' "$test_root/dashboard.log" >/dev/null
grep -F 'Custom executables: disabled' "$test_root/dashboard.log" >/dev/null
grep -F 'Launch arguments: counter_workload 1 <launch authorization redacted>' \
    "$test_root/dashboard.log" >/dev/null
grep -E 'PID / parent PID: [0-9]+ / [0-9]+' "$test_root/dashboard.log" >/dev/null
grep -F 'Executable ID:    device=' "$test_root/dashboard.log" >/dev/null
grep -F 'Original state:  RUNNING' "$test_root/dashboard.log" >/dev/null
grep -F 'artifacts=valid structure' "$test_root/dashboard.log" >/dev/null
grep -F '[SUCCESS] Original process identity verified, terminated and reaped.' \
    "$test_root/dashboard.log" >/dev/null
grep -F '[SIMULATION] Fake restore result rejected; no restored process adopted.' \
    "$test_root/dashboard.log" >/dev/null
grep -F 'Restore: FAILED / NOT VERIFIED' "$test_root/dashboard.log" >/dev/null
grep -F '[BLOCKED] A restore has already been attempted in this session' \
    "$test_root/dashboard.log" >/dev/null
grep -F 'FINAL RESULT: RECOVERY NOT VERIFIED' "$test_root/dashboard.log" >/dev/null
if grep -F 'FINAL RESULT: RECOVERY VERIFIED' "$test_root/dashboard.log" >/dev/null; then
    echo "test_dashboard_menu: fake CRIU was reported as verified recovery" >&2
    exit 1
fi
grep '^dump' "$test_root/fake-criu.log" >/dev/null
[ "$(grep -c '^restore' "$test_root/fake-criu.log")" -eq 1 ]

identity=$(sed -n 's/.*Selected bundled program PID=\([0-9][0-9]*\).*START=\([0-9][0-9]*\).*/\1 \2/p' \
    "$test_root/dashboard.log" | head -n 1)
set -- $identity
if [ "$#" -ne 2 ] || "$CHRONO" status "$1" "$2" >"$test_root/status.log" 2>&1; then
    echo "test_dashboard_menu: test-owned original process was not stopped and reaped" >&2
    exit 1
fi
echo "SIMULATION: progressive dashboard and explicit lifecycle tested; fake restore was rejected"
