#!/bin/sh
set -eu

test_log=$(mktemp -d ./build/verified-stop-test-XXXXXX) || {
    echo "test_verified_stop: could not create private test directory" >&2
    exit 1
}
cleanup() { rm -rf -- "$test_log"; }
on_signal() { signal=$1; trap - EXIT HUP INT TERM; cleanup; exit "$signal"; }
trap cleanup EXIT
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

start=$(awk '{ sub(/^.*[)] /, ""); if (NF >= 20 && $20 ~ /^[0-9]+$/) print $20; else exit 1; }' "/proc/$$/stat")
wrong_start=$((start + 1))
exe=$(readlink -f "/proc/$$/exe")
if ./build/verified_stop "$$" "$wrong_start" "$exe" >"$test_log/pidfd" 2>&1; then
    echo "test_verified_stop: mismatched process start time was accepted" >&2
    exit 1
fi
if ! grep -E 'start-time mismatch; no signal sent|pidfd_open:|pidfd operations are unavailable in this build' "$test_log/pidfd" >/dev/null; then
    cat "$test_log/pidfd" >&2
    echo "test_verified_stop: expected fail-closed diagnostic was not produced" >&2
    exit 1
fi
if ./build/verified_stop_no_pidfd "$$" "$start" "$exe" >"$test_log/no-pidfd" 2>&1; then
    echo "test_verified_stop: build without pidfd support reported success" >&2
    exit 1
fi
if ! grep -F "pidfd operations are unavailable in this build" "$test_log/no-pidfd" >/dev/null; then
    cat "$test_log/no-pidfd" >&2
    echo "test_verified_stop: pidfd-disabled path did not fail closed" >&2
    exit 1
fi
if ! kill -0 "$$" 2>/dev/null; then
    echo "test_verified_stop: test shell unexpectedly stopped" >&2
    exit 1
fi
echo "verified pidfd signaling tests passed"
