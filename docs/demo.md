# Presentation Demo

The primary professor-facing live checkpoint/termination/restore workflow is
the interactive dashboard; follow the
[professor demonstration guide](PROFESSOR_DEMO_GUIDE.md) for the exact sequence.

## Live Terminal Dashboard

Build and run the real managed-counter dashboard on the institute Linux host:

```sh
make
./build/chrono dashboard
```

The progressive menu keeps the complete demonstration in one session. Select
1 to launch the bundled counter, 3 to inspect its identity, and 4 to monitor
it. Select 5 to create a checkpoint, 7 to browse and explicitly select a
checkpoint, 6 to request original-process termination (then confirm with `y`),
and 8 to restore the selected checkpoint (also confirmed with `y`). Options 9
and 10 reverify the restored process and display the recovery report; 11 exits
and cleans up only identities launched or restored by this session. Option 2
is intentionally unavailable because arbitrary executables are not authorized.

The managed counter's in-memory value comes from its
peer-credential- and executable-verified control channel. `/proc/<pid>/comm`
is display-only; restored memory state is not inferred from a sidecar file.
`./build/chrono demo-manual` retains the separate single-key walkthrough,
including its paginated, read-only `/proc` browser (`p`, `n`/`b`, `r`, `h`).
That browser never signals or modifies selected processes. Neither workflow
authorizes checkpointing arbitrary processes.

The dashboard presents one workflow screen at a time and visibly acknowledges
menu selections and confirmation keys. Its event history is bounded; the full
diagnostic log is retained separately in the private session directory.

The numbered menu routes operations through the existing CRIU adapters. The
genuine CRIU 4.3 `--leave-running`/restore flow was previously verified through
the single-key `demo-manual` implementation; the numbered menu has not been
rerun with real CRIU after its UI change. Fake-backend menu tests are
simulation-only. A custom `CHRONO_CRIU_PATH` is labelled unverified and is not
proof of real CRIU. Restore status becomes verified only after process identity
and counter progression checks pass. Recovery events are session/demo history,
not persistent supervisor history. The verified restored identity is managed
only for the duration of the session; persistent supervisor re-registration
and automatic recovery are not implemented.

The run directory, checkpoint metadata, and exact artifact paths are printed
by the dashboard and retained for review. A terminal capture can be recorded
with `script -q -c 'CHRONO_CRIU_UNPRIVILEGED=1 ./build/chrono dashboard'
/tmp/chronoos-dashboard.typescript`.

For non-root users the adapter automatically invokes CRIU with `--unprivileged`
unless `CHRONO_CRIU_UNPRIVILEGED=0` explicitly requests the default mode. The
real demo requires CRIU, `timeout`, and a host whose CRIU preflight succeeds. It
uses a private temporary registry and checkpoint
directory, starts only the bundled counter, and keeps logs and CRIU images for
review.

```sh
sh tests/demo_chronoos.sh
```

The script builds ChronoOS, runs `criu check --unprivileged`, registers the
counter, starts the supervisor, waits for two structurally valid CRIU periodic
checkpoints, resolves the latest-valid checkpoint, and verifies that the
counter remains running after those snapshots. It then uses the verified
PID/start-time/executable helper to inject an unexpected termination and waits
for the supervisor to mark the process `FAILED`.

The demo deliberately does not restore the workload: automatic recovery and
fresh authorization for a restored workload are not implemented. If CRIU
preflight fails, the script exits 77 before launching a workload. On failure,
artifacts and logs are retained and their path is printed. On success, the
temporary demo directory is also retained for presentation evidence.

Expected command-line observables include:

```sh
./build/chrono supervisor status
./build/chrono list
./build/chrono checkpoint-latest CHRONO_PROCESS_ID
./build/chrono checkpoints
./build/chrono status CHRONO_PROCESS_ID
```
