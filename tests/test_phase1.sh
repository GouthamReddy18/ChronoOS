#!/bin/sh
set -eu

TEST_WORK_DIR=$(mktemp -d ./build/phase1-test-XXXXXX) || {
    echo "could not create a private Phase 1 test directory" >&2
    exit 1
}
CHRONO=./build/chrono
COUNTER=./build/counter_workload
REGISTRY_FIXTURE=./build/registry_fixture
repo_dir=$(pwd -P)
LOG=$TEST_WORK_DIR/test-counter.log
fixture_created=0
fixture_id=
fixture_process_pid=
fixture_start_time=
fixture_retry_count=0
FIXTURE_RETRY_LIMIT=64
COUNTER_EXECUTABLE=$repo_dir/build/counter_workload
permission_pid=
permission_start=
registry_mode_changed=0
cleanup_done=0
cleanup_failed=0
registry_dir="/tmp/chronoos-$(id -u)"

expect_status() {
    expected=$1
    shift
    set +e
    "$@" >"$TEST_WORK_DIR/test-output.log" 2>&1
    actual=$?
    set -e
    if [ "$actual" -ne "$expected" ]; then
        cat "$TEST_WORK_DIR/test-output.log"
        echo "expected exit $expected, got $actual: $*" >&2
        exit 1
    fi
}

stop_managed() {
    pid=$1
    start=$2
    "$CHRONO" stop "$pid" "$start" >/dev/null
    i=0
    while "$CHRONO" status "$pid" "$start" >/dev/null 2>&1 && [ "$i" -lt 100 ]; do
        sleep 0.02; i=$((i + 1))
    done
    expect_status 1 "$CHRONO" status "$pid" "$start"
}

cleanup_stop_managed() {
    pid=$1
    start=$2
    label=$3
    [ -n "$pid" ] && [ -n "$start" ] || return 0
    if ! "$CHRONO" stop "$pid" "$start" >/dev/null 2>&1; then
        echo "test cleanup failure: could not safely stop $label (PID $pid); it may still be running" >&2
        cleanup_failed=1
        return 0
    fi
    i=0
    while "$CHRONO" status "$pid" "$start" >/dev/null 2>&1 && [ "$i" -lt 100 ]; do
        sleep 0.02
        i=$((i + 1))
    done
    if "$CHRONO" status "$pid" "$start" >/dev/null 2>&1; then
        echo "test cleanup failure: $label (PID $pid) still reports running after stop" >&2
        cleanup_failed=1
    fi
    return 0
}

proc_start_time() {
    proc_start_pid=$1
    awk '{ sub(/^.*[)] /, ""); if (NF >= 20 && $20 ~ /^[0-9]+$/) print $20; else exit 1; }' \
        "/proc/$proc_start_pid/stat"
}

stop_fixture_process() {
    [ -n "$fixture_process_pid" ] || return 0
    if [ -z "$fixture_start_time" ]; then
        if [ -e "/proc/$fixture_process_pid/stat" ]; then
            echo "test cleanup failure: fixture start time was never established; possible leftover PID $fixture_process_pid was not signaled" >&2
            cleanup_failed=1
            return 1
        fi
        wait "$fixture_process_pid" 2>/dev/null || true
        fixture_process_pid=
        return 0
    fi
    observed_start=$(proc_start_time "$fixture_process_pid" 2>/dev/null || true)
    if [ -z "$observed_start" ]; then
        if [ -e "/proc/$fixture_process_pid/stat" ]; then
            echo "test cleanup failure: fixture identity is unreadable; possible leftover PID $fixture_process_pid was not signaled" >&2
            cleanup_failed=1
            return 1
        fi
        wait "$fixture_process_pid" 2>/dev/null || true
        fixture_process_pid=
        fixture_start_time=
        return 0
    fi
    if [ "$observed_start" != "$fixture_start_time" ]; then
        wait "$fixture_process_pid" 2>/dev/null || true
        fixture_process_pid=
        fixture_start_time=
        return 0
    fi
    observed_state=$(awk '{ sub(/^.*[)] /, ""); print $1; }' "/proc/$fixture_process_pid/stat" 2>/dev/null || true)
    if [ "$observed_state" = Z ]; then
        wait "$fixture_process_pid" 2>/dev/null || true
        fixture_process_pid=
        fixture_start_time=
        return 0
    fi
    if [ -z "$fixture_start_time" ] ||
       ! ./build/verified_stop "$fixture_process_pid" "$fixture_start_time" "$COUNTER_EXECUTABLE" >/dev/null 2>&1; then
        echo "test cleanup failure: fixture process identity could not be verified; possible leftover PID $fixture_process_pid was not signaled" >&2
        cleanup_failed=1
        return 1
    fi
    i=0
    while [ "$i" -lt 100 ]; do
        current_start=$(proc_start_time "$fixture_process_pid" 2>/dev/null || true)
        [ "$current_start" = "$fixture_start_time" ] || break
        current_state=$(awk '{ sub(/^.*[)] /, ""); print $1; }' "/proc/$fixture_process_pid/stat" 2>/dev/null || true)
        [ "$current_state" = Z ] && break
        sleep 0.02
        i=$((i + 1))
    done
    current_start=$(proc_start_time "$fixture_process_pid" 2>/dev/null || true)
    current_state=$(awk '{ sub(/^.*[)] /, ""); print $1; }' "/proc/$fixture_process_pid/stat" 2>/dev/null || true)
    if [ "$current_start" = "$fixture_start_time" ] && [ "$current_state" != Z ]; then
        echo "test cleanup failure: fixture process PID $fixture_process_pid remains alive after verified SIGTERM" >&2
        cleanup_failed=1
        return 1
    fi
    wait "$fixture_process_pid" 2>/dev/null || true
    fixture_process_pid=
    fixture_start_time=
    return 0
}

cleanup_managed() {
    [ "$cleanup_done" -eq 0 ] || return 0
    cleanup_done=1
    needs_registry_cleanup=0
    if [ "$fixture_created" -eq 1 ] || [ "$registry_mode_changed" -eq 1 ] ||
       [ -n "${permission_pid:-}" ] || [ -n "${managed_pid:-}" ] ||
       [ -n "${path_pid:-}" ] || [ -n "${link_pid:-}" ] ||
       [ -n "${repeated_pid:-}" ]; then
        needs_registry_cleanup=1
    fi
    registry_ready=1
    if [ "$needs_registry_cleanup" -eq 0 ]; then
        :
    elif [ "$registry_mode_changed" -eq 1 ]; then
        restored=0
        attempt=0
        while [ "$attempt" -lt 3 ]; do
            if [ -d "$registry_dir" ] && [ ! -L "$registry_dir" ] &&
               [ "$(stat -c %u "$registry_dir" 2>/dev/null || true)" = "$(id -u)" ] &&
               chmod 700 "$registry_dir" 2>/dev/null &&
               [ -d "$registry_dir" ] && [ ! -L "$registry_dir" ] &&
               [ "$(stat -c %u "$registry_dir" 2>/dev/null || true)" = "$(id -u)" ] &&
               [ "$(stat -c %a "$registry_dir" 2>/dev/null || true)" = 700 ]; then
                registry_mode_changed=0
                restored=1
                break
            fi
            attempt=$((attempt + 1))
            sleep 0.05
        done
        if [ "$restored" -ne 1 ]; then
            registry_ready=0
        fi
    elif [ "$needs_registry_cleanup" -eq 1 ] && { [ ! -d "$registry_dir" ] || [ -L "$registry_dir" ] ||
         [ "$(stat -c %u "$registry_dir" 2>/dev/null || true)" != "$(id -u)" ] ||
         [ "$(stat -c %a "$registry_dir" 2>/dev/null || true)" != 700 ]; }; then
        registry_ready=0
    fi
    if [ "$registry_ready" -eq 1 ]; then
        if [ "$fixture_created" -eq 1 ] && [ -n "$fixture_process_pid" ]; then
            if ! "$REGISTRY_FIXTURE" remove "$fixture_process_pid" "$fixture_id" >/dev/null 2>&1; then
                echo "test cleanup failure: could not remove the fixture created for PID $fixture_process_pid" >&2
                cleanup_failed=1
            fi
        fi
        cleanup_stop_managed "${permission_pid:-}" "${permission_start:-}" "permission-test counter"
        cleanup_stop_managed "${managed_pid:-}" "${managed_start:-}" "managed-test counter"
        cleanup_stop_managed "${path_pid:-}" "${path_start:-}" "PATH-test counter"
        cleanup_stop_managed "${link_pid:-}" "${link_start:-}" "symlink-test counter"
        cleanup_stop_managed "${repeated_pid:-}" "${repeated_start:-}" "repeated-launch counter"
    else
        echo "test cleanup failure: registry directory could not be restored and verified as owned mode 0700; registry cleanup was skipped" >&2
        cleanup_failed=1
        for pid in "${permission_pid:-}" "${managed_pid:-}" "${path_pid:-}" "${link_pid:-}" "${repeated_pid:-}"; do
            if [ -n "$pid" ]; then
                echo "test cleanup failure: managed PID $pid may still be running because registry safety was not verified" >&2
                break
            fi
        done
    fi
    if [ -n "${counter_pid:-}" ]; then
        kill -TERM "$counter_pid" 2>/dev/null || true
        wait "$counter_pid" 2>/dev/null || true
    fi
    if [ -n "$fixture_process_pid" ]; then
        stop_fixture_process || true
    fi
}

finish_exit() {
    exit_code=$1
    trap '' HUP INT TERM
    trap - EXIT
    cleanup_managed
    if [ "$cleanup_failed" -ne 0 ] && [ "$exit_code" -eq 0 ]; then
        exit_code=1
    fi
    exit "$exit_code"
}

finish_signal() {
    exit_code=$1
    trap '' HUP INT TERM
    trap - EXIT
    cleanup_managed
    exit "$exit_code"
}

trap 'finish_exit $?' EXIT
trap 'finish_signal 129' HUP
trap 'finish_signal 130' INT
trap 'finish_signal 143' TERM

expect_status 2 "$CHRONO" inspect
expect_status 2 "$CHRONO" inspect 12x
expect_status 2 "$CHRONO" inspect +12
expect_status 2 "$CHRONO" inspect 0
expect_status 2 "$CHRONO" inspect 999999999999999999999999
expect_status 1 "$CHRONO" inspect 1
expect_status 1 "$CHRONO" inspect 2147483647
expect_status 2 "$CHRONO" inspect 12 extra
fake_criu_log=$TEST_WORK_DIR/test-fake-criu.log
: >"$fake_criu_log"
expect_status 1 env CHRONO_CRIU_PATH=./build/fake_criu CHRONO_FAKE_CRIU_LOG="$fake_criu_log" "$CHRONO" checkpoint 1 1 review
[ ! -s "$fake_criu_log" ]
grep -q '^chrono: managed identity validation failed:' "$TEST_WORK_DIR/test-output.log"
expect_status 1 "$CHRONO" checkpoint 1 1 review
grep -q '^chrono: managed identity validation failed:' "$TEST_WORK_DIR/test-output.log"
expect_status 1 "$CHRONO" restore review
grep -Fxq 'chrono: cannot open checkpoint metadata' "$TEST_WORK_DIR/test-output.log"
expect_status 2 "$CHRONO" checkpoint 1 1
expect_status 2 "$CHRONO" restore
expect_status 2 "$CHRONO" checkpoint nope 1 review
expect_status 2 "$CHRONO" checkpoint 1 0 review
expect_status 2 "$CHRONO" checkpoint 1 1 ../review
expect_status 2 "$CHRONO" restore ../review

: >"$LOG"
"$COUNTER" 60 >"$LOG" 2>&1 &
counter_pid=$!
i=0
while [ ! -s "$LOG" ] && [ "$i" -lt 50 ]; do sleep 0.02; i=$((i + 1)); done
"$CHRONO" inspect "$counter_pid" >"$TEST_WORK_DIR/test-output.log"
grep -q "^PID: $counter_pid$" "$TEST_WORK_DIR/test-output.log"
kill -INT "$counter_pid"
wait "$counter_pid"
counter_pid=
grep -q 'stopping counter=' "$LOG"

: >"$LOG"
"$COUNTER" 60 >"$LOG" 2>&1 &
counter_pid=$!
i=0
while [ ! -s "$LOG" ] && [ "$i" -lt 50 ]; do sleep 0.02; i=$((i + 1)); done
kill -TERM "$counter_pid"
wait "$counter_pid"
counter_pid=
grep -q 'stopping counter=' "$LOG"
./build/test_process

# Launch must return while its managed child continues running.
managed_pid=
managed_start=
launch_output=$("$CHRONO" launch counter 1)
managed_pid=$(printf '%s\n' "$launch_output" | sed -n 's/^PID: //p')
managed_start=$(printf '%s\n' "$launch_output" | sed -n 's/^STARTTIME: //p')
[ -n "$managed_pid" ] && [ -n "$managed_start" ]
"$CHRONO" status "$managed_pid" "$managed_start" | grep -q '^running$'
expect_status 2 "$CHRONO" launch /bin/sleep
expect_status 1 "$CHRONO" status "$managed_pid" 1
"$CHRONO" stop "$managed_pid" "$managed_start" | grep -q '^termination requested$'
i=0
while [ -e "/proc/$managed_pid" ] && [ "$i" -lt 1000 ]; do
    sleep 0.02; i=$((i + 1))
done
[ ! -e "/proc/$managed_pid" ]
expect_status 1 "$CHRONO" status "$managed_pid" "$managed_start"
grep -Fxq 'chrono: managed process is no longer running' "$TEST_WORK_DIR/test-output.log"
managed_pid=
managed_start=

# PATH and symlink invocation must resolve the executable's real bundle directory.
path_pid=
path_start=
path_launch=$(cd /tmp && PATH="$repo_dir/build:$PATH" chrono launch counter 1)
path_pid=$(printf '%s\n' "$path_launch" | sed -n 's/^PID: //p')
path_start=$(printf '%s\n' "$path_launch" | sed -n 's/^STARTTIME: //p')
[ -n "$path_pid" ] && [ -n "$path_start" ]
stop_managed "$path_pid" "$path_start"
path_pid=
path_start=

ln -s "$repo_dir/build/chrono" "$TEST_WORK_DIR/chrono-link"
link_pid=
link_start=
link_launch=$(cd /tmp && "$repo_dir/$TEST_WORK_DIR/chrono-link" launch counter 1)
link_pid=$(printf '%s\n' "$link_launch" | sed -n 's/^PID: //p')
link_start=$(printf '%s\n' "$link_launch" | sed -n 's/^STARTTIME: //p')
[ -n "$link_pid" ] && [ -n "$link_start" ]
stop_managed "$link_pid" "$link_start"
link_pid=
link_start=

mkdir -p "$TEST_WORK_DIR/no-workload"
cp ./build/chrono "$TEST_WORK_DIR/no-workload/chrono"
expect_status 1 "$TEST_WORK_DIR/no-workload/chrono" launch counter

# Registry parser and stale cleanup checks use a live process started here.
# Older test runs may have left a record for a reused PID. Never overwrite or
# remove such a record: reap only our dedicated child and choose another PID.
start_fixture_process() {
    while [ "$fixture_retry_count" -lt "$FIXTURE_RETRY_LIMIT" ]; do
        fixture_retry_count=$((fixture_retry_count + 1))
        "$COUNTER" 60 >/dev/null 2>&1 &
        fixture_process_pid=$!
        sleep 0.05
        fixture_start_time=$(proc_start_time "$fixture_process_pid" 2>/dev/null || true)
        if [ -z "$fixture_start_time" ]; then
            echo "could not establish start time for dedicated fixture PID $fixture_process_pid; it was not signaled" >&2
            return 1
        fi
        fixture_record="$registry_dir/$fixture_process_pid"
        if [ ! -e "$fixture_record" ] && [ ! -L "$fixture_record" ]; then
            return 0
        fi
        if ! stop_fixture_process; then
            echo "could not safely reap dedicated fixture process after registry PID collision" >&2
            return 1
        fi
    done
    echo "could not find an unused registry PID within $FIXTURE_RETRY_LIMIT dedicated fixture launches" >&2
    return 1
}

start_fixture_process
fixture_token=00000000000000000000000000000000
fixture_uid=$(id -u)

create_fixture() {
    fixture_contents=$1
    attempt=0
    while [ "$attempt" -lt "$FIXTURE_RETRY_LIMIT" ]; do
        attempt=$((attempt + 1))
        if fixture_id=$(printf '%b' "$fixture_contents" | "$REGISTRY_FIXTURE" create "$fixture_process_pid"); then
            fixture_created=1
            return 0
        fi
        fixture_record="$registry_dir/$fixture_process_pid"
        if [ ! -e "$fixture_record" ] && [ ! -L "$fixture_record" ]; then
            echo "registry fixture creation failed without a PID collision" >&2
            return 1
        fi
        if ! stop_fixture_process || ! start_fixture_process; then
            return 1
        fi
    done
    echo "could not create the registry fixture within $FIXTURE_RETRY_LIMIT attempts" >&2
    return 1
}

check_fixture() {
    "$REGISTRY_FIXTURE" check "$fixture_process_pid" "$fixture_id"
}

remove_fixture() {
    "$REGISTRY_FIXTURE" remove "$fixture_process_pid" "$fixture_id"
    fixture_created=0
    fixture_id=
}

assert_fixture_process_alive() {
    kill -0 "$fixture_process_pid"
    fixture_state=$(awk '/^State:/ { print $2; exit }' "/proc/$fixture_process_pid/status")
    [ -n "$fixture_state" ] && [ "$fixture_state" != Z ] && [ "$fixture_state" != X ]
}

assert_malformed_record() {
    create_fixture "$1"
    expect_status 1 "$CHRONO" status "$fixture_process_pid" 1
    check_fixture
    expect_status 1 "$CHRONO" stop "$fixture_process_pid" 1
    check_fixture
    assert_fixture_process_alive
    remove_fixture
}

assert_malformed_record "malformed\n"
assert_malformed_record "+1 0 0 $fixture_uid $fixture_token\n"
assert_malformed_record "-1 0 0 $fixture_uid $fixture_token\n"
assert_malformed_record "01 0 0 $fixture_uid $fixture_token\n"
assert_malformed_record "18446744073709551616 0 0 $fixture_uid $fixture_token\n"
assert_malformed_record "1 0 0 $fixture_uid 0000000000000000\n"
assert_malformed_record "1 0 0 $fixture_uid $fixture_token"
create_fixture "1 0 0 $fixture_uid $fixture_token\\000\\n"
expect_status 1 "$CHRONO" status "$fixture_process_pid" 1
check_fixture
remove_fixture
create_fixture "1 0 0 $fixture_uid $fixture_token\ntrailing\n"
expect_status 1 "$CHRONO" status "$fixture_process_pid" 1
check_fixture
remove_fixture
create_fixture "1 0 0 $fixture_uid $fixture_token"
expect_status 1 "$CHRONO" status "$fixture_process_pid" 1
check_fixture
remove_fixture
create_fixture "1 0 0 $fixture_uid $fixture_token\n"
expect_status 1 "$CHRONO" status "$fixture_process_pid" 1
if "$REGISTRY_FIXTURE" check "$fixture_process_pid" "$fixture_id"; then
    echo "valid stale record was not removed" >&2
    exit 1
fi
fixture_created=0
fixture_id=
assert_fixture_process_alive

# Use a real managed process and successful controls around the sole mode
# change, so a missing record or invalid identity cannot pass spuriously.
permission_pid=
permission_start=
permission_launch=$("$CHRONO" launch counter 1)
permission_pid=$(printf '%s\n' "$permission_launch" | sed -n 's/^PID: //p')
permission_start=$(printf '%s\n' "$permission_launch" | sed -n 's/^STARTTIME: //p')
[ -n "$permission_pid" ] && [ -n "$permission_start" ]
if [ ! -d "$registry_dir" ] || [ -L "$registry_dir" ] ||
   [ "$(stat -c %u "$registry_dir")" != "$(id -u)" ] ||
   [ "$(stat -c %a "$registry_dir")" != 700 ]; then
    echo "registry directory is not the expected owned 0700 directory" >&2
    exit 1
fi
"$CHRONO" status "$permission_pid" "$permission_start" | grep -q '^running$'
registry_mode_changed=1
chmod 750 "$registry_dir"
[ "$(stat -c %a "$registry_dir")" = 750 ]
expect_status 1 "$CHRONO" status "$permission_pid" "$permission_start"
grep -Fxq 'chrono: registry directory ownership or permissions are invalid (expected owned mode 0700)' "$TEST_WORK_DIR/test-output.log"
chmod 700 "$registry_dir"
if [ "$(stat -c %a "$registry_dir")" != 700 ]; then
    echo "failed to restore registry directory permissions" >&2
    exit 1
fi
registry_mode_changed=0
"$CHRONO" status "$permission_pid" "$permission_start" | grep -q '^running$'
stop_managed "$permission_pid" "$permission_start"
permission_pid=
permission_start=

stop_fixture_process

# Repeated launch/stop cycles exercise record creation and removal safety.
for iteration in 1 2 3; do
    repeated_pid=
    repeated_start=
    launch_output=$("$CHRONO" launch counter 1)
    repeated_pid=$(printf '%s\n' "$launch_output" | sed -n 's/^PID: //p')
    repeated_start=$(printf '%s\n' "$launch_output" | sed -n 's/^STARTTIME: //p')
    [ -n "$repeated_pid" ] && [ -n "$repeated_start" ]
    "$CHRONO" status "$repeated_pid" "$repeated_start" | grep -q '^running$'
    stop_managed "$repeated_pid" "$repeated_start"
    repeated_pid=
    repeated_start=
done

# The suite has no remaining managed identities here, so cleanup must not
# require a registry directory that this run never created.
cleanup_managed
trap - EXIT HUP INT TERM
echo "Phase 1 and Phase 2 regression tests passed (logs and fixtures retained in $TEST_WORK_DIR)"
