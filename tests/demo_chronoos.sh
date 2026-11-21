#!/bin/sh
set -eu

CHRONO=./build/chrono
CRIU_COMMAND=$(command -v criu 2>/dev/null || true)
if [ -z "$CRIU_COMMAND" ] || ! command -v timeout >/dev/null 2>&1; then
    echo "DEMO BLOCKED: CRIU and timeout are required for the real checkpoint demo." >&2
    exit 77
fi
CRIU_PATH=$(readlink -f "$CRIU_COMMAND") || exit 77
if [ ! -x "$CRIU_PATH" ]; then
    echo "DEMO BLOCKED: CRIU path is not executable: $CRIU_PATH" >&2
    exit 77
fi

demo_dir=$(mktemp -d "${TMPDIR:-/tmp}/chronoos-presentation-XXXXXX")
chmod 700 "$demo_dir"
export XDG_STATE_HOME="$demo_dir/state"
export CHRONO_CHECKPOINT_DIR="$demo_dir/checkpoints"
export CHRONO_CRIU_PATH="$CRIU_PATH"
export CHRONO_CRIU_UNPRIVILEGED=1
service_pid=
workload_id=
workload_pid=
workload_start=

cleanup() {
    if [ -n "$workload_id" ]; then
        "$CHRONO" stop "$workload_id" >/dev/null 2>&1 || true
    fi
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        "$CHRONO" supervisor shutdown >/dev/null 2>&1 || true
        wait "$service_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
    echo "DEMO FAILED: $*" >&2
    echo "Supervisor log: $demo_dir/supervisor.log" >&2
    echo "Artifacts and state retained at: $demo_dir" >&2
    exit 1
}

echo "[1] Building ChronoOS"
make -B all build/verified_stop >"$demo_dir/build.log" 2>&1 || fail "build failed"
echo "[2] Checking unprivileged CRIU support"
if ! timeout --signal=TERM --kill-after=2s 20s "$CRIU_PATH" check --unprivileged \
    >"$demo_dir/criu-check.log" 2>&1; then
    cat "$demo_dir/criu-check.log" >&2
    echo "DEMO BLOCKED: CRIU preflight failed; no workload was started." >&2
    echo "Diagnostics retained at: $demo_dir" >&2
    exit 77
fi
echo "[3] Registering the bundled counter at a 2-second checkpoint interval"
"$CHRONO" protect --counter --checkpoint-interval 2 >"$demo_dir/protect.log" 2>&1 || fail "workload registration failed"
workload_id=$(awk '/^ChronoOS Process ID:/ {print $4}' "$demo_dir/protect.log")
workload_pid=$(awk '/^Linux PID:/ {print $3}' "$demo_dir/protect.log")
workload_start=$(awk '/^Start Time:/ {print $3}' "$demo_dir/protect.log")
[ -n "$workload_id" ] && [ -n "$workload_pid" ] && [ -n "$workload_start" ] ||
    fail "registration did not report a complete process identity"
echo "[4] Starting persistent foreground supervision"
"$CHRONO" supervisor run >"$demo_dir/supervisor.log" 2>&1 &
service_pid=$!
attempt=0
while [ ! -S "$XDG_STATE_HOME/chronoos/.supervisor.sock" ] && [ "$attempt" -lt 100 ]; do
    kill -0 "$service_pid" 2>/dev/null || fail "supervisor exited during startup"
    sleep 0.1
    attempt=$((attempt + 1))
done
[ -S "$XDG_STATE_HOME/chronoos/.supervisor.sock" ] || fail "supervisor socket did not become ready"
echo "[5] Waiting for two real CRIU checkpoints"
attempt=0
checkpoint_count=0
while [ "$attempt" -lt 120 ]; do
    listing=$("$CHRONO" checkpoints 2>>"$demo_dir/cli.log") || fail "checkpoint listing failed"
    checkpoint_count=$(printf '%s\n' "$listing" | grep -F "WORKLOAD=$workload_id" | grep -c 'ARTIFACT=valid' || true)
    [ "$checkpoint_count" -ge 2 ] && break
    kill -0 "$service_pid" 2>/dev/null || fail "supervisor exited before two checkpoints"
    sleep 0.25
    attempt=$((attempt + 1))
done
[ "$checkpoint_count" -ge 2 ] || fail "two valid checkpoints were not produced within 30 seconds"
echo "[6] Resolving latest valid checkpoint"
"$CHRONO" checkpoint-latest "$workload_id" | tee "$demo_dir/latest.log"
echo "[7] Verifying the protected workload is still running"
"$CHRONO" status "$workload_id" >"$demo_dir/status-before-failure.log" || fail "workload status failed"
grep -Fx 'State: RUNNING' "$demo_dir/status-before-failure.log" >/dev/null || fail "workload is not RUNNING"
echo "[8] Injecting an unexpected exit into the exact verified workload"
./build/verified_stop "$workload_pid" "$workload_start" "$(readlink -f ./build/counter_workload)" \
    >"$demo_dir/failure-injection.log" 2>&1 || fail "verified failure injection failed"
attempt=0
while [ "$attempt" -lt 60 ]; do
    if "$CHRONO" status "$workload_id" >"$demo_dir/status-after-failure.log" 2>/dev/null &&
       grep -Fx 'State: FAILED' "$demo_dir/status-after-failure.log" >/dev/null; then break; fi
    sleep 0.1
    attempt=$((attempt + 1))
done
[ "$attempt" -lt 60 ] || fail "supervisor did not record confirmed workload disappearance"
echo "[9] Confirmed failure detected; automatic recovery is not implemented yet"
"$CHRONO" supervisor shutdown >/dev/null || fail "supervisor shutdown request failed"
wait "$service_pid" || fail "supervisor did not shut down cleanly"
service_pid=
echo "[10] Demo completed: periodic checkpointing and failure detection verified"
echo "Workload: $workload_id (PID $workload_pid, start time $workload_start)"
echo "Valid checkpoints observed: $checkpoint_count"
echo "Artifacts and logs retained at: $demo_dir"
