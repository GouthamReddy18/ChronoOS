#!/bin/sh
set -eu
. tests/decimal_identity.sh

CHRONO=./build/chrono
COUNTER=$(readlink -f ./build/counter_workload)
if ! command -v timeout >/dev/null 2>&1; then
    echo "CRIU integration test skipped: timeout utility is unavailable" >&2
    exit 77
fi
if ! command -v criu >/dev/null 2>&1; then
    echo "CRIU integration preflight blocked: criu executable is unavailable" >&2
    exit 77
fi
CRIU_COMMAND=$(command -v criu)
CHRONO_CRIU_PATH=$(readlink -f "$CRIU_COMMAND") || {
    echo "CRIU integration preflight blocked: could not resolve the CRIU executable" >&2
    exit 77
}
if [ ! -f "$CHRONO_CRIU_PATH" ] || [ ! -x "$CHRONO_CRIU_PATH" ]; then
    echo "CRIU integration preflight blocked: resolved CRIU executable is not executable" >&2
    exit 77
fi
export CHRONO_CRIU_PATH
export CHRONO_CRIU_UNPRIVILEGED=1

check_log=$(mktemp "${TMPDIR:-/tmp}/chronoos-criu-check-XXXXXX") || {
    echo "CRIU integration test failed: could not create preflight log" >&2
    exit 1
}
set +e
timeout --signal=TERM --kill-after=5s 20s "$CHRONO_CRIU_PATH" check --unprivileged >"$check_log" 2>&1
check_status=$?
set -e
if [ "$check_status" -ne 0 ]; then
    echo "CRIU integration preflight blocked: criu check --unprivileged exited $check_status" >&2
    cat "$check_log" >&2
    echo "preflight log preserved at $check_log" >&2
    exit 77
fi
echo "Unprivileged CRIU preflight passed; diagnostic log preserved at $check_log"

test_dir=$(mktemp -d "${TMPDIR:-/tmp}/chronoos-criu-integration-XXXXXX") || {
    echo "CRIU integration test failed: could not create isolated test directory" >&2
    exit 1
}
chmod 700 "$test_dir"
if [ "$(stat -c %u "$test_dir")" != "$(id -u)" ] || [ "$(stat -c %a "$test_dir")" != 700 ]; then
    echo "CRIU integration test failed: isolated directory ownership or mode is unsafe" >&2
    exit 1
fi
log="$test_dir/integration.log"
state_file="$test_dir/counter.state"
checkpoint_id="integration-$$"
pid=
start=
restored_pid=
restored_start=
launch_attempted=0
original_stopped=0
cleanup_started=0
scheduler_id=
scheduler_service_pid=
proc_start_time() {
    process_pid=$1
    timeout --signal=TERM --kill-after=1s 2s awk '{ sub(/^.*[)] /, ""); if (NF >= 20 && $20 ~ /^[0-9]+$/) print $20; else exit 1; }' "/proc/$process_pid/stat"
}
proc_counter() {
    process_pid=$1
    process_comm=$(cat "/proc/$process_pid/comm" 2>/dev/null) || return 1
    case "$process_comm" in c:*) process_counter=${process_comm#c:} ;; *) return 1 ;; esac
    case "$process_counter" in ''|*[!0-9]*) return 1 ;; esac
    printf '%s\n' "$process_counter"
}
wait_for_counter_advance() {
    state_before=$1
    attempt=0
    while [ "$attempt" -lt 30 ]; do
        state_now=$(cat "$state_file" 2>/dev/null || echo 0)
        case "$state_now" in ''|*[!0-9]*) return 1 ;; esac
        if [ "$state_now" -gt "$state_before" ]; then
            live_state=$state_now
            return 0
        fi
        sleep 0.2
        attempt=$((attempt + 1))
    done
    return 1
}
cleanup_restored() {
    [ -n "$restored_pid" ] || return 0
    case "$restored_pid" in ''|*[!0-9]*)
        echo "possible leftover restored process: malformed PID '$restored_pid'; not signaling" >&2
        return 1
        ;;
    esac
    if [ "$restored_pid" -le 1 ] || [ "$restored_pid" = "$$" ]; then
        echo "possible leftover restored process: unsafe PID value $restored_pid; not signaling" >&2
        return 0
    fi
    proc="/proc/$restored_pid"
    [ -r "$proc/stat" ] || return 0
    current_start=$(proc_start_time "$restored_pid" 2>/dev/null || true)
    [ -n "$current_start" ] || {
        echo "possible leftover restored process PID $restored_pid: start time unreadable; not signaling" >&2
        return 1
    }
    [ -n "$restored_start" ] || {
        echo "possible leftover restored process PID $restored_pid: identity was not established; not signaling" >&2
        return 1
    }
    [ "$current_start" = "$restored_start" ] || return 0
    current_uid=$(stat -c %u "$proc" 2>/dev/null || true)
    current_exe=$(readlink "$proc/exe" 2>/dev/null || true)
    if [ "$current_uid" != "$(id -u)" ] || [ "$current_exe" != "$COUNTER" ]; then
        echo "possible leftover restored process PID $restored_pid: UID or executable mismatch; not signaling" >&2
        return 1
    fi
    if ! ./build/verified_stop "$restored_pid" "$restored_start" "$COUNTER" >>"$log" 2>&1; then
        echo "possible leftover restored process PID $restored_pid: pidfd identity validation or SIGTERM failed; not retrying by PID" >&2
        return 1
    fi
    attempt=0
    while [ "$attempt" -lt 5 ]; do
        current_start=$(proc_start_time "$restored_pid" 2>/dev/null || true)
        [ "$current_start" = "$restored_start" ] || return 0
        sleep 1
        attempt=$((attempt + 1))
    done
    current_start=$(proc_start_time "$restored_pid" 2>/dev/null || true)
    if [ "$current_start" = "$restored_start" ]; then
        echo "restored process PID $restored_pid remains alive after verified SIGTERM" >&2
        return 1
    fi
    return 0
}
cleanup_original() {
    [ "$launch_attempted" -eq 1 ] || return 0
    [ "$original_stopped" -eq 0 ] || return 0
    if ! decimal_positive_in_range "$pid" 2 2147483647 ||
       ! decimal_positive_in_range "$start" 1 18446744073709551615; then
        echo "possible leftover original test process: launch identity was incomplete or invalid; not signaling" >&2
        return 1
    fi
    timeout --signal=TERM --kill-after=2s 10s "$CHRONO" stop "$pid" "$start" >/dev/null 2>&1 || true
    attempt=0
    while [ "$attempt" -lt 10 ]; do
        current_start=$(proc_start_time "$pid" 2>/dev/null || true)
        [ "$current_start" = "$start" ] || return 0
        sleep 1
        attempt=$((attempt + 1))
    done
    current_start=$(proc_start_time "$pid" 2>/dev/null || true)
    if [ "$current_start" = "$start" ]; then
        echo "possible leftover original test process PID $pid: ChronoOS stop did not terminate it" >&2
        return 1
    fi
    if [ -r "/proc/$pid/stat" ] && [ -z "$current_start" ]; then
        echo "possible leftover original test process PID $pid: could not verify its start time after stop" >&2
        return 1
    fi
    return 0
}
cleanup_all() {
    [ "$cleanup_started" -eq 0 ] || return 0
    cleanup_started=1
    if [ -n "$scheduler_id" ]; then
        timeout --signal=TERM --kill-after=2s 10s "$CHRONO" stop "$scheduler_id" >/dev/null 2>&1 || true
    fi
    if [ -n "$scheduler_service_pid" ] && kill -0 "$scheduler_service_pid" 2>/dev/null; then
        timeout --signal=TERM --kill-after=2s 10s "$CHRONO" supervisor shutdown >/dev/null 2>&1 || true
        wait "$scheduler_service_pid" 2>/dev/null || true
        scheduler_service_pid=
    fi
    cleanup_original || true
    cleanup_restored || true
}
fail() {
    echo "CRIU integration test failed: $*" >&2
    echo "diagnostics preserved at $log" >&2
    exit 1
}
on_signal() {
    signal_name=$1
    exit_status=$2
    trap - EXIT HUP INT TERM
    echo "CRIU integration test interrupted by $signal_name" >&2
    if [ "$cleanup_started" -eq 0 ]; then
        cleanup_all
    elif [ -n "$restored_pid" ]; then
        echo "possible leftover restored process PID $restored_pid: signal arrived during cleanup; not repeating cleanup" >&2
    fi
    echo "diagnostics preserved at $log" >&2
    exit "$exit_status"
}
on_exit() {
    exit_status=$?
    trap - EXIT HUP INT TERM
    if [ "$cleanup_started" -eq 0 ]; then
        if [ "$exit_status" -ne 0 ]; then
            echo "CRIU integration test exited unexpectedly with status $exit_status" >&2
            echo "diagnostics preserved at $log" >&2
        fi
        cleanup_all
    fi
    exit "$exit_status"
}
trap on_exit EXIT
trap 'on_signal HUP 129' HUP
trap 'on_signal INT 130' INT
trap 'on_signal TERM 143' TERM

export CHRONO_CHECKPOINT_DIR="$test_dir/checkpoints"
export CHRONO_TEST_STATE_FILE="$state_file"
launch_attempted=1
timeout --signal=TERM --kill-after=5s 20s "$CHRONO" launch counter 1 >"$log" 2>&1 || fail "could not launch dedicated counter"
pid=$(awk '/^PID:/ {print $2}' "$log")
start=$(awk '/^STARTTIME:/ {print $2}' "$log")
decimal_positive_in_range "$pid" 2 2147483647 || fail "launch returned an invalid PID"
_decimal_normalize "$pid"
pid=$_decimal_normalized
decimal_positive_in_range "$start" 1 18446744073709551615 || fail "launch returned an invalid STARTTIME"
_decimal_normalize "$start"
start=$_decimal_normalized
sleep 2
[ -s "$state_file" ] || fail "counter did not write its observable state"
before=$(cat "$state_file")
before_comm=$(proc_counter "$pid") || fail "counter value was not published in the managed process comm field"
initial_memory_output=$(timeout --signal=TERM --kill-after=2s 10s "$CHRONO" counter "$pid" "$start" "$start" 2>>"$log") ||
    fail "could not query the original counter from workload memory"
initial_memory=${initial_memory_output#COUNTER: }
case "$initial_memory" in ''|*[!0-9]*) fail "original in-memory counter query returned malformed data" ;; esac
live_checkpoint_one="live-one-$$"
live_checkpoint_two="live-two-$$"
timeout --signal=TERM --kill-after=5s 120s "$CHRONO" checkpoint "$pid" "$start" "$live_checkpoint_one" --leave-running >>"$log" 2>&1 || fail "first leave-running checkpoint failed or timed out"
timeout --signal=TERM --kill-after=2s 10s "$CHRONO" status "$pid" "$start" >/dev/null 2>&1 ||
    fail "workload identity was not live after first leave-running dump"
wait_for_counter_advance "$before" || fail "workload did not continue after first leave-running dump"
before=$live_state
timeout --signal=TERM --kill-after=5s 120s "$CHRONO" checkpoint "$pid" "$start" "$live_checkpoint_two" --leave-running >>"$log" 2>&1 || fail "second leave-running checkpoint failed or timed out"
timeout --signal=TERM --kill-after=2s 10s "$CHRONO" status "$pid" "$start" >/dev/null 2>&1 ||
    fail "workload identity was not live after second leave-running dump"
wait_for_counter_advance "$before" || fail "workload did not continue after second leave-running dump"
before=$live_state
before_memory_output=$(timeout --signal=TERM --kill-after=2s 10s "$CHRONO" counter "$pid" "$start" "$start" 2>>"$log") ||
    fail "could not query the checkpoint-time in-memory counter"
before_memory=${before_memory_output#COUNTER: }
case "$before_memory" in ''|*[!0-9]*) fail "checkpoint-time in-memory counter query returned malformed data" ;; esac
[ "$before_memory" -gt 0 ] || fail "checkpoint-time in-memory counter baseline must be nonzero"
timeout --signal=TERM --kill-after=5s 120s "$CHRONO" checkpoint "$pid" "$start" "$checkpoint_id" >>"$log" 2>&1 || fail "ChronoOS checkpoint failed or timed out"
if timeout --signal=TERM --kill-after=2s 10s "$CHRONO" status "$pid" "$start" >/dev/null 2>&1; then
    fail "original test process is still reported running after dump"
fi
original_current_start=$(proc_start_time "$pid" 2>/dev/null || true)
if [ "$original_current_start" = "$start" ]; then
    fail "original process identity remains live after dump"
fi
if [ -r "/proc/$pid/stat" ] && [ -z "$original_current_start" ]; then
    fail "cannot verify that original process stopped; refusing state comparison"
fi
original_stopped=1
listing=$(timeout --signal=TERM --kill-after=5s 20s "$CHRONO" checkpoints 2>>"$log") || fail "could not read checkpoint metadata"
printf '%s\n' "$listing" | grep -F "$checkpoint_id METADATA_STATE=COMPLETE ARTIFACT=valid " >/dev/null ||
    fail "checkpoint metadata or artifacts were not reported complete"
if ! timeout --signal=TERM --kill-after=5s 120s "$CHRONO" restore "$checkpoint_id" >>"$log" 2>&1; then
    echo "possible leftover restored process: restore failed or timed out and no trusted restored PID is available; no process will be signaled" >&2
    fail "ChronoOS restore failed or timed out"
fi
restored_pid=$(awk '/^restored PID:/ {print $3}' "$log")
if ! decimal_positive_in_range "$restored_pid" 2 2147483647; then
    echo "possible leftover restored process: restore returned no valid PID; no process will be signaled" >&2
    fail "restore returned an invalid PID"
fi
_decimal_normalize "$restored_pid"
restored_pid=$_decimal_normalized
[ "$restored_pid" != "$$" ] || fail "restore returned an unsafe PID"
proc="/proc/$restored_pid"
attempt=0
while [ "$attempt" -lt 15 ] && [ ! -r "$proc/stat" ]; do
    sleep 1
    attempt=$((attempt + 1))
done
[ -r "$proc/stat" ] || fail "restored process is not live; PID may have exited before identity was established"
restored_start=$(proc_start_time "$restored_pid" 2>/dev/null || true)
[ -n "$restored_start" ] || fail "could not establish restored process start time; possible leftover PID $restored_pid was not signaled"
restored_uid=$(stat -c %u "$proc" 2>/dev/null || true)
restored_exe=$(readlink "$proc/exe" 2>/dev/null || true)
[ "$restored_uid" = "$(id -u)" ] || fail "restored process has unexpected owner; possible leftover PID $restored_pid"
[ "$restored_exe" = "$COUNTER" ] || fail "restored process executable does not match the test workload; possible leftover PID $restored_pid"
attempt=0
while [ "$attempt" -lt 15 ]; do
    after=$(cat "$state_file" 2>/dev/null || echo 0)
    case "$after" in ''|*[!0-9]*) fail "observable counter state is malformed" ;; esac
    if [ "$after" -gt "$before" ]; then break; fi
    sleep 1
    attempt=$((attempt + 1))
done
[ "$after" -gt "$before" ] || fail "restored counter state did not advance from $before (last value $after)"
restored_counter=
attempt=0
while [ "$attempt" -lt 12 ]; do
    memory_output=$(timeout --signal=TERM --kill-after=2s 10s "$CHRONO" counter \
        "$restored_pid" "$restored_start" "$start" 2>>"$log" || true)
    case "$memory_output" in
        'COUNTER: '*) restored_counter=${memory_output#COUNTER: } ;;
        *) restored_counter= ;;
    esac
    case "$restored_counter" in ''|*[!0-9]*) ;;
        *) [ "$restored_counter" -gt "$before_memory" ] && break ;;
    esac
    sleep 0.25
    attempt=$((attempt + 1))
done
case "$restored_counter" in ''|*[!0-9]*) fail "restored in-memory counter query returned malformed data" ;; esac
[ "$restored_counter" -gt "$before_memory" ] ||
    fail "restored process memory counter did not continue from $before_memory (last $restored_counter)"
first_restored_memory=$restored_counter
restored_counter_next=
attempt=0
while [ "$attempt" -lt 12 ]; do
    memory_output=$(timeout --signal=TERM --kill-after=2s 10s "$CHRONO" counter \
        "$restored_pid" "$restored_start" "$start" 2>>"$log" || true)
    case "$memory_output" in
        'COUNTER: '*) restored_counter_next=${memory_output#COUNTER: } ;;
        *) restored_counter_next= ;;
    esac
    case "$restored_counter_next" in ''|*[!0-9]*) ;;
        *) [ "$restored_counter_next" -gt "$first_restored_memory" ] && break ;;
    esac
    sleep 0.25
    attempt=$((attempt + 1))
done
[ -n "$restored_counter_next" ] && [ "$restored_counter_next" -gt "$first_restored_memory" ] ||
    fail "restored in-memory counter did not advance a second time from $first_restored_memory"
listing_after_restore=$(timeout --signal=TERM --kill-after=5s 20s "$CHRONO" checkpoints 2>>"$log") ||
    fail "could not recheck checkpoint metadata after restore"
printf '%s\n' "$listing_after_restore" |
    grep -F "$checkpoint_id METADATA_STATE=COMPLETE ARTIFACT=valid " >/dev/null ||
    fail "restored CRIU sidecar files made the checkpoint artifacts appear invalid"
verified_restored_pid=$restored_pid
cleanup_started=1
cleanup_restored || fail "could not confirm safe cleanup of restored process PID $verified_restored_pid"
restored_pid=
cleanup_started=0
unset CHRONO_TEST_STATE_FILE
export XDG_STATE_HOME="$test_dir/state"
"$CHRONO" protect --counter --checkpoint-interval 1 >"$test_dir/scheduler-protect.log" 2>&1 ||
    fail "could not register the periodic-checkpoint workload"
scheduler_id=$(awk '/^ChronoOS Process ID:/ {print $4}' "$test_dir/scheduler-protect.log")
[ -n "$scheduler_id" ] || fail "periodic workload registration returned no ChronoOS ID"
"$CHRONO" supervisor run >>"$log" 2>&1 &
scheduler_service_pid=$!
attempt=0
while [ ! -S "$XDG_STATE_HOME/chronoos/.supervisor.sock" ] && [ "$attempt" -lt 50 ]; do
    kill -0 "$scheduler_service_pid" 2>/dev/null || fail "periodic supervisor exited during startup"
    sleep 0.1
    attempt=$((attempt + 1))
done
[ -S "$XDG_STATE_HOME/chronoos/.supervisor.sock" ] || fail "periodic supervisor socket did not become ready"
attempt=0
scheduler_count=0
while [ "$attempt" -lt 100 ]; do
    scheduler_count=0
    for metadata in "$CHRONO_CHECKPOINT_DIR"/*.meta; do
        [ -f "$metadata" ] || continue
        if grep -q '^COMPLETE$' "$metadata" 2>/dev/null &&
           grep -q "^$scheduler_id$" "$metadata" 2>/dev/null; then
            scheduler_count=$((scheduler_count + 1))
        fi
    done
    [ "$scheduler_count" -ge 2 ] && break
    kill -0 "$scheduler_service_pid" 2>/dev/null || fail "periodic supervisor exited before two checkpoints"
    sleep 0.2
    attempt=$((attempt + 1))
done
[ "$scheduler_count" -ge 2 ] || fail "real supervisor did not create two periodic CRIU checkpoints"
timeout --signal=TERM --kill-after=2s 10s "$CHRONO" status "$scheduler_id" >"$test_dir/scheduler-status.log" 2>&1 ||
    fail "periodically checkpointed workload is not verifiably running"
grep -Fx 'State: RUNNING' "$test_dir/scheduler-status.log" >/dev/null ||
    fail "periodic workload did not remain RUNNING after CRIU dumps"
timeout --signal=TERM --kill-after=2s 10s "$CHRONO" supervisor shutdown >/dev/null 2>&1 ||
    fail "periodic supervisor did not accept shutdown"
wait "$scheduler_service_pid" || fail "periodic supervisor shutdown failed"
scheduler_service_pid=
timeout --signal=TERM --kill-after=2s 10s "$CHRONO" stop "$scheduler_id" >/dev/null 2>&1 ||
    fail "could not safely stop periodic test workload"
scheduler_id=
echo "CRIU integration test passed: original PID $pid STARTTIME $start was absent before restore; restored PID $verified_restored_pid STARTTIME $restored_start UID $restored_uid executable $restored_exe; in-memory counter advanced from $before_memory through $first_restored_memory to $restored_counter_next (comm display $before_comm; observable state $before to $after); periodic supervisor published $scheduler_count real CRIU checkpoints while keeping its workload RUNNING" |
    tee -a "$log"
echo "integration artifacts: $test_dir"
echo "preflight log: $check_log"
echo "Remove this directory only after reviewing its contents: $test_dir"
exit 0
