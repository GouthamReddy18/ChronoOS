# ChronoOS Limitations

ChronoOS currently provides persistent foreground supervision, per-workload
checkpoint intervals, bounded checkpoint workers, CRIU periodic snapshots,
latest-valid checkpoint references, and configurable checkpoint retention.
It does **not** implement automatic restoration after process failure.

## Workload and CRIU constraints

- The verified end-to-end workload is the bundled counter. Generic registration
  does not prove CRIU compatibility or establish every application's scoped
  authorization requirements.
- The live terminal dashboard reads the authoritative counter from the
  bundled workload through its peer-credential- and executable-verified
  control channel. `/proc/<pid>/comm` is display-only. The restored process is
  adopted only in the dashboard session and is not re-registered in the
  persistent supervisor registry.
- CRIU `--leave-running` was verified for the bundled counter. It can leave
  stale external file, socket, or other resource state for workloads that
  continue mutating those resources. It is not safe to generalize this behavior
  to arbitrary programs.
- Structural validation checks private ownership and the expected flat image
  layout. It does not prove image integrity or that a later restore will work.
- `CHRONO_CRIU_PATH` is an executable trust boundary, not a cryptographically
  authenticated backend selection. A deliberately deceptive configured binary
  could imitate CRIU's check, artifact, or PID-file behavior. Restore identity
  checks reject a nonexistent or mismatched PID, but cannot prove which binary
  created a live matching process. Use only a trusted CRIU executable; fake
  adapter tests are not evidence of real checkpoint or restore.
- Checkpoints depend on kernel features, CRIU version, privileges, namespaces,
  process resources, and workload behavior.

## Recovery and observability

- Unexpected exits are recorded as failed managed-process state, but are not
  automatically recovered. The restore adapter verifies a live non-zombie
  process, current-user ownership, start time, and checkpoint executable
  device/inode before reporting restore success. ChronoOS does not yet prove a
  fresh scoped authorization channel for the restored identity, automatically
  verify resumed computation, or atomically re-register the new process.
- Recovery retry policy, persistent recovery history, and operator recovery
  controls are not implemented.
- Checkpoint metadata records workload/checkpoint IDs, original PID and start
  time, creation time, executable, backend, and state. Duration, image size,
  CRIU exit status, and persistent detailed failure reasons are not yet stored.
- The supervisor supports at most 32 registry entries and four concurrent
  checkpoint workers. Scheduling deadlines restart from supervisor start; the
  persisted interval and prior checkpoints remain available.
- Retention defaults to three valid checkpoints per workload and can be set
  through `CHRONO_CHECKPOINT_RETENTION` to a value from 1 to 100. Invalid or
  uncertain artifacts are preserved, so disk use can exceed the configured
  limit when cleanup safety cannot be established.
