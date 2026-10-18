#!/bin/sh
set -eu

CHRONO=./build/chrono
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/chronoos-cli-path-test-XXXXXX") || {
    echo "test_cli_checkpoint_path: temporary directory setup failed" >&2
    exit 1
}
cleanup() { rm -rf -- "$test_dir"; }
on_signal() { signal=$1; trap - EXIT HUP INT TERM; cleanup; exit "$signal"; }
trap cleanup EXIT
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

expect_path_error() {
    expected=$1
    shift
    if "$@" >"$test_dir/output" 2>&1; then
        cat "$test_dir/output" >&2
        echo "test_cli_checkpoint_path: command unexpectedly succeeded: $*" >&2
        exit 1
    fi
    if ! grep -F "$expected" "$test_dir/output" >/dev/null; then
        cat "$test_dir/output" >&2
        echo "test_cli_checkpoint_path: expected diagnostic not found: $expected" >&2
        exit 1
    fi
}

expect_path_error "invalid or too-long checkpoint directory path" \
    env CHRONO_CHECKPOINT_DIR=relative "$CHRONO" checkpoints
expect_path_error "invalid or too-long checkpoint directory path" \
    env CHRONO_CHECKPOINT_DIR=relative "$CHRONO" restore sample
expect_path_error "invalid or too-long checkpoint directory path" \
    env CHRONO_CHECKPOINT_DIR="/$(awk 'BEGIN { for (i=0; i<140; i++) printf "x" }')" "$CHRONO" checkpoints
expect_path_error "invalid or too-long checkpoint directory path" \
    env CHRONO_CHECKPOINT_DIR="/$(awk 'BEGIN { for (i=0; i<140; i++) printf "x" }')" "$CHRONO" restore sample

mkdir "$test_dir/real"
ln -s "$test_dir/real" "$test_dir/link"
expect_path_error "cannot open checkpoint directory" \
    env CHRONO_CHECKPOINT_DIR="$test_dir/link" "$CHRONO" checkpoints

mkdir "$test_dir/insecure"
chmod 0755 "$test_dir/insecure"
expect_path_error "cannot open checkpoint directory" \
    env CHRONO_CHECKPOINT_DIR="$test_dir/insecure" "$CHRONO" checkpoints

echo "checkpoint directory path tests passed"
