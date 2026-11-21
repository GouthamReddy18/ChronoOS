#!/bin/sh
set -eu

COUNTER=./build/counter_workload
workload_pid=
log_path=
cleanup() {
    if [ -n "$workload_pid" ]; then
        kill -TERM "$workload_pid" 2>/dev/null || true
        wait "$workload_pid" 2>/dev/null || true
        workload_pid=
    fi
    if [ -n "$log_path" ]; then
        rm -f "$log_path"
        log_path=
    fi
}
trap cleanup EXIT HUP INT TERM

expect_invalid_token() {
    label=$1
    shift
    set +e
    "$COUNTER" "$@" >"$log_path" 2>&1
    status=$?
    set -e
    if [ "$status" -ne 2 ]; then
        cat "$log_path" >&2
        echo "counter workload accepted $label token invocation (exit $status)" >&2
        return 1
    fi
}

run_signal_case() {
    signal_name=$1
    log_path=$(mktemp "${TMPDIR:-/tmp}/chronoos-counter-test.XXXXXX")
    unset CHRONO_TEST_STATE_FILE
    "$COUNTER" 1 >"$log_path" 2>&1 &
    workload_pid=$!
    attempt=0
    while [ "$attempt" -lt 50 ] && [ "$(grep -c '^pid=' "$log_path" || true)" -lt 2 ]; do
        kill -0 "$workload_pid" 2>/dev/null || break
        sleep 0.1
        attempt=$((attempt + 1))
    done
    [ "$(grep -c '^pid=' "$log_path" || true)" -ge 2 ] || {
        echo "counter workload did not emit two heartbeats" >&2
        return 1
    }
    grep -Fx "pid=$workload_pid counter=0" "$log_path" >/dev/null
    grep -Fx "pid=$workload_pid counter=1" "$log_path" >/dev/null
    tested_pid=$workload_pid
    comm=$(cat "/proc/$workload_pid/comm")
    case "$comm" in
        c:*) comm_counter=${comm#c:} ;;
        *) echo "counter was not visible in /proc comm: $comm" >&2; return 1 ;;
    esac
    case "$comm_counter" in ''|*[!0-9]*) echo "invalid counter in /proc comm: $comm" >&2; return 1 ;; esac
    [ "$comm_counter" -ge 1 ] || { echo "counter did not advance in /proc comm: $comm" >&2; return 1; }
    kill -s "$signal_name" "$workload_pid"
    set +e
    wait "$workload_pid"
    status=$?
    set -e
    workload_pid=
    [ "$status" -eq 0 ] || {
        echo "counter workload did not exit cleanly after SIG$signal_name" >&2
        return 1
    }
    grep -F "pid=$tested_pid stopping counter=" "$log_path" >/dev/null
    rm -f "$log_path"
    log_path=
}

log_path=$(mktemp "${TMPDIR:-/tmp}/chronoos-counter-token-test.XXXXXX")
expect_invalid_token "missing" 1 0123456789abcdef0123456789abcdef 1
expect_invalid_token "empty" 1 "" 1 1
expect_invalid_token "short" 1 0123456789abcdef0123456789abcde 1 1
expect_invalid_token "malformed" 1 0123456789abcdef0123456789abcdeg 1 1
expect_invalid_token "oversized" 1 0123456789abcdef0123456789abcdef0 1 1
rm -f "$log_path"
log_path=

run_signal_case TERM
run_signal_case INT
printf '%s\n' 'counter workload token validation, heartbeat, /proc comm, SIGTERM, and SIGINT tests passed'
