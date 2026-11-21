#!/bin/sh
set -eu

temporary=$(mktemp -d "${TMPDIR:-/tmp}/chronoos-supervisor-service-XXXXXX")
chmod 700 "$temporary"
export XDG_STATE_HOME="$temporary/state"
service_pid=
first_id=
second_id=
cleanup() {
    if [ -n "$first_id" ]; then ./build/chrono stop "$first_id" >/dev/null 2>&1 || true; fi
    if [ -n "$second_id" ]; then ./build/chrono stop "$second_id" >/dev/null 2>&1 || true; fi
    if [ -n "$service_pid" ] && kill -0 "$service_pid" 2>/dev/null; then
        ./build/chrono supervisor shutdown >/dev/null 2>&1 || true
        wait "$service_pid" 2>/dev/null || true
    fi
    rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM

./build/chrono supervisor run >"$temporary/supervisor.log" 2>&1 &
service_pid=$!
registry="$XDG_STATE_HOME/chronoos"
socket="$registry/.supervisor.sock"
attempt=0
while [ ! -S "$socket" ] && [ "$attempt" -lt 50 ]; do
    if ! kill -0 "$service_pid" 2>/dev/null; then
        cat "$temporary/supervisor.log" >&2
        echo "supervisor exited during startup" >&2
        exit 1
    fi
    sleep 0.1
    attempt=$((attempt + 1))
done
[ -S "$socket" ] || { echo "supervisor socket did not become ready" >&2; exit 1; }
[ "$(stat -c %a "$socket")" = 600 ] || { echo "supervisor socket mode is not 0600" >&2; exit 1; }

status=$(./build/chrono supervisor status)
case "$status" in "RUNNING pid=$service_pid protocol=1") ;; *) echo "unexpected status: $status" >&2; exit 1 ;; esac
if ./build/chrono supervisor run >"$temporary/duplicate.log" 2>&1; then
    echo "duplicate supervisor was accepted" >&2
    exit 1
fi

./build/chrono protect -- /bin/sleep 60 >"$temporary/first-protect.log"
./build/chrono protect --checkpoint-interval 2 -- /bin/sleep 60 >"$temporary/second-protect.log"
first_output=$(cat "$temporary/first-protect.log")
second_output=$(cat "$temporary/second-protect.log")
first_id=$(printf '%s\n' "$first_output" | awk '/^ChronoOS Process ID:/ {print $4}')
second_id=$(printf '%s\n' "$second_output" | awk '/^ChronoOS Process ID:/ {print $4}')
[ -n "$first_id" ] && [ -n "$second_id" ] && [ "$first_id" != "$second_id" ] || {
    echo "could not register two independent workloads" >&2
    exit 1
}
grep -F 'Checkpoint interval: 2 seconds' "$temporary/second-protect.log" >/dev/null || {
    echo "configured checkpoint interval was not shown by the CLI" >&2
    exit 1
}
./build/chrono status "$first_id" >/dev/null
./build/chrono status "$second_id" >/dev/null
sleep 1
kill -0 "$service_pid" 2>/dev/null || { echo "supervisor did not persist after client exit" >&2; exit 1; }

first_pid=$(printf '%s\n' "$first_output" | awk '/^Linux PID:/ {print $3}')
first_start=$(printf '%s\n' "$first_output" | awk '/^Start Time:/ {print $3}')
./build/verified_stop "$first_pid" "$first_start" /usr/bin/sleep
attempt=0
while [ "$attempt" -lt 30 ]; do
    if ./build/chrono status "$first_id" | grep -F 'State: FAILED' >/dev/null; then break; fi
    sleep 0.1
    attempt=$((attempt + 1))
done
[ "$attempt" -lt 30 ] || { echo "service did not record unexpected workload disappearance" >&2; exit 1; }
./build/chrono status "$second_id" | grep -F 'State: RUNNING' >/dev/null || {
    echo "failure of one workload affected the other workload" >&2
    exit 1
}

response=$(./build/chrono supervisor shutdown)
[ "$response" = STOPPING ] || { echo "unexpected shutdown response: $response" >&2; exit 1; }
wait "$service_pid"
service_pid=
if [ -e "$socket" ]; then
    echo "supervisor socket remains after shutdown" >&2
    exit 1
fi
./build/chrono supervisor run >"$temporary/restarted-supervisor.log" 2>&1 &
service_pid=$!
attempt=0
while [ ! -S "$socket" ] && [ "$attempt" -lt 50 ]; do
    if ! kill -0 "$service_pid" 2>/dev/null; then
        cat "$temporary/restarted-supervisor.log" >&2
        echo "supervisor failed to restart" >&2
        exit 1
    fi
    sleep 0.1
    attempt=$((attempt + 1))
done
if [ ! -S "$socket" ]; then
    echo "supervisor socket did not return after restart" >&2
    exit 1
fi
./build/chrono status "$second_id" | grep -F 'State: RUNNING' >/dev/null || {
    echo "registered workload did not survive supervisor restart" >&2
    exit 1
}
./build/chrono supervisor shutdown >/dev/null
wait "$service_pid"
service_pid=
if [ -e "$socket" ]; then
    echo "restarted supervisor socket remains after shutdown" >&2
    exit 1
fi
printf 'invalid registry record\n' >"$registry/cp-999999.rec"
if ./build/chrono supervisor run >"$temporary/invalid-registry.log" 2>&1; then
    echo "supervisor accepted a malformed registry record" >&2
    exit 1
fi
if [ -S "$socket" ]; then
    echo "failed startup left a supervisor socket" >&2
    exit 1
fi
echo "persistent supervisor, private socket, duplicate prevention, multiple registrations, status, and shutdown passed"
