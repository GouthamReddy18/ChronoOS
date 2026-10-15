#!/bin/sh
set -eu

CHRONO=./build/chrono
COUNTER=./build/counter_workload
LOG=./build/test-counter.log

expect_status() {
    expected=$1
    shift
    set +e
    "$@" >./build/test-output.log 2>&1
    actual=$?
    set -e
    if [ "$actual" -ne "$expected" ]; then
        cat ./build/test-output.log
        echo "expected exit $expected, got $actual: $*" >&2
        exit 1
    fi
}

expect_status 2 "$CHRONO" inspect
expect_status 2 "$CHRONO" inspect 12x
expect_status 2 "$CHRONO" inspect +12
expect_status 2 "$CHRONO" inspect 0
expect_status 2 "$CHRONO" inspect 999999999999999999999999
expect_status 1 "$CHRONO" inspect 1
expect_status 1 "$CHRONO" inspect 2147483647
expect_status 2 "$CHRONO" inspect 12 extra

: >"$LOG"
"$COUNTER" 60 >"$LOG" 2>&1 &
counter_pid=$!
trap 'kill -TERM "$counter_pid" 2>/dev/null || true; wait "$counter_pid" 2>/dev/null || true' EXIT HUP INT TERM
i=0
while [ ! -s "$LOG" ] && [ "$i" -lt 50 ]; do sleep 0.02; i=$((i + 1)); done
"$CHRONO" inspect "$counter_pid" >./build/test-output.log
grep -q "^PID: $counter_pid$" ./build/test-output.log
kill -INT "$counter_pid"
wait "$counter_pid"
grep -q 'stopping counter=' "$LOG"

: >"$LOG"
"$COUNTER" 60 >"$LOG" 2>&1 &
counter_pid=$!
i=0
while [ ! -s "$LOG" ] && [ "$i" -lt 50 ]; do sleep 0.02; i=$((i + 1)); done
kill -TERM "$counter_pid"
wait "$counter_pid"
grep -q 'stopping counter=' "$LOG"
trap - EXIT HUP INT TERM

./build/test_process
echo "Phase 1 regression tests passed"
