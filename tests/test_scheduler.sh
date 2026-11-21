#!/bin/sh
set -eu

temporary=$(mktemp -d "${TMPDIR:-/tmp}/chronoos-scheduler-XXXXXX")
chmod 700 "$temporary"
export XDG_STATE_HOME="$temporary/state"
export CHRONO_CHECKPOINT_DIR="$temporary/checkpoints"
export CHRONO_CRIU_PATH="$(pwd)/build/fake_criu"
export CHRONO_CRIU_UNPRIVILEGED=1
export CHRONO_TEST_ALLOW_FAKE_CRIU=1
export CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED=1
service_pid=
workload_id=
second_id=
cleanup() {
    if [ -n "$workload_id" ]; then ./build/chrono stop "$workload_id" >/dev/null 2>&1 || true; fi
    if [ -n "$second_id" ]; then ./build/chrono stop "$second_id" >/dev/null 2>&1 || true; fi
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        ./build/chrono supervisor shutdown >/dev/null 2>&1 || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM

./build/chrono protect --counter --checkpoint-interval 1 >"$temporary/protect.log"
workload_id=$(awk '/^ChronoOS Process ID:/ {print $4}' "$temporary/protect.log")
[ -n "$workload_id" ] || { echo "counter workload registration failed" >&2; exit 1; }
./build/chrono protect --counter --checkpoint-interval 2 >"$temporary/protect-second.log"
second_id=$(awk '/^ChronoOS Process ID:/ {print $4}' "$temporary/protect-second.log")
[ -n "$second_id" ] && [ "$second_id" != "$workload_id" ] || {
    echo "second counter workload registration failed" >&2
    exit 1
}

./build/chrono supervisor run >"$temporary/supervisor.log" 2>&1 &
service_pid=$!
socket="$XDG_STATE_HOME/chronoos/.supervisor.sock"
attempt=0
while [ ! -S "$socket" ] && [ "$attempt" -lt 50 ]; do
    if ! kill -0 "$service_pid" 2>/dev/null; then
        cat "$temporary/supervisor.log" >&2
        echo "supervisor exited during scheduler test startup" >&2
        exit 1
    fi
    sleep 0.1
    attempt=$((attempt + 1))
done
[ -S "$socket" ] || { echo "supervisor socket did not become ready" >&2; exit 1; }

attempt=0
checkpoint_count=0
second_count=0
while [ "$attempt" -lt 80 ]; do
    checkpoint_count=0
    second_count=0
    for metadata in "$CHRONO_CHECKPOINT_DIR"/*.meta; do
        [ -f "$metadata" ] || continue
        if grep -q '^COMPLETE$' "$metadata" 2>/dev/null; then
            if grep -q "^$workload_id$" "$metadata" 2>/dev/null; then
                checkpoint_count=$((checkpoint_count + 1))
            fi
            if grep -q "^$second_id$" "$metadata" 2>/dev/null; then
                second_count=$((second_count + 1))
            fi
        fi
    done
    [ "$checkpoint_count" -ge 2 ] && [ "$second_count" -ge 2 ] && break
    if ! kill -0 "$service_pid" 2>/dev/null; then
        cat "$temporary/supervisor.log" >&2
        echo "supervisor exited before two periodic checkpoints" >&2
        exit 1
    fi
    sleep 0.25
    attempt=$((attempt + 1))
done
[ "$checkpoint_count" -ge 2 ] && [ "$second_count" -ge 2 ] || {
    cat "$temporary/supervisor.log" >&2
    echo "scheduler did not publish two valid checkpoints" >&2
    exit 1
}
latest_output=$(./build/chrono checkpoint-latest "$workload_id")
latest_id=$(printf '%s\n' "$latest_output" | awk 'NR == 1 {print $1}')
checkpoint_listing=$(./build/chrono checkpoints)
printf '%s\n' "$checkpoint_listing" | grep -F "$latest_id METADATA_STATE=COMPLETE ARTIFACT=valid " >/dev/null || {
    echo "latest-checkpoint CLI did not resolve a complete valid artifact" >&2
    exit 1
}
./build/chrono status "$workload_id" | grep -F 'State: RUNNING' >/dev/null || {
    echo "leave-running checkpointing did not preserve workload" >&2
    exit 1
}
./build/chrono status "$second_id" | grep -F 'State: RUNNING' >/dev/null || {
    echo "second workload was not independently checkpointed" >&2
    exit 1
}

# A supervisor restart should retain records and resume both schedules.
./build/chrono supervisor shutdown >/dev/null
wait "$service_pid"
service_pid=
./build/chrono supervisor run >>"$temporary/supervisor.log" 2>&1 &
service_pid=$!
attempt=0
while [ ! -S "$socket" ] && [ "$attempt" -lt 50 ]; do
    if ! kill -0 "$service_pid" 2>/dev/null; then
        cat "$temporary/supervisor.log" >&2
        echo "supervisor failed to restart during scheduler test" >&2
        exit 1
    fi
    sleep 0.1
    attempt=$((attempt + 1))
done
[ -S "$socket" ] || { echo "supervisor socket did not return after scheduler restart" >&2; exit 1; }
attempt=0
while [ "$attempt" -lt 100 ]; do
    checkpoint_count=0
    second_count=0
    for metadata in "$CHRONO_CHECKPOINT_DIR"/*.meta; do
        [ -f "$metadata" ] || continue
        if grep -q '^COMPLETE$' "$metadata" 2>/dev/null; then
            if grep -q "^$workload_id$" "$metadata" 2>/dev/null; then checkpoint_count=$((checkpoint_count + 1)); fi
            if grep -q "^$second_id$" "$metadata" 2>/dev/null; then second_count=$((second_count + 1)); fi
        fi
    done
    [ "$checkpoint_count" -ge 3 ] && [ "$second_count" -ge 3 ] && break
    sleep 0.2
    attempt=$((attempt + 1))
done
[ "$checkpoint_count" -ge 3 ] && [ "$second_count" -ge 3 ] || {
    cat "$temporary/supervisor.log" >&2
    echo "checkpoint schedules did not resume after supervisor restart" >&2
    exit 1
}
completed_workload_id=$workload_id
./build/chrono supervisor shutdown >/dev/null
wait "$service_pid"
service_pid=
./build/chrono stop "$workload_id" >/dev/null
workload_id=
./build/chrono stop "$second_id" >/dev/null
second_id=
echo "periodic scheduler test passed: workload $completed_workload_id had $checkpoint_count validated checkpoints; second workload had $second_count"
