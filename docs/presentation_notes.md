# ChronoOS Presentation Notes

For the primary live checkpoint/restore story, use
[`PROFESSOR_DEMO_GUIDE.md`](PROFESSOR_DEMO_GUIDE.md). The
`tests/demo_chronoos.sh` walkthrough below is a separate supervisor scheduling
and failure-detection demonstration; it does not restore the workload.

## Two-minute introduction

ChronoOS is a Linux process supervision and checkpoint scheduling framework
built around CRIU. A user explicitly registers a workload, and ChronoOS stores
its PID, start time, owner, executable identity, command configuration, and
checkpoint interval in a private persistent registry. A foreground supervisor
monitors registered processes and runs bounded checkpoint workers on monotonic
deadlines. Each worker asks CRIU to create a leave-running checkpoint, checks
the expected image layout, publishes metadata atomically, updates the
workload's latest-valid reference, and applies retention. A private Unix socket
supports status and clean shutdown; registry locks prevent duplicate
supervisors.

The verified workload is a bundled counter. On the tested CRIU 4.3 host,
ChronoOS created real periodic checkpoints and kept the counter running during
those dumps. A verified failure-injection helper can terminate only the exact
registered PID/start-time/executable. The supervisor then records failure.
Automatic restore is not implemented yet, so this project does not claim a
complete automatic-recovery lifecycle.

## Architecture explanation

The CLI registers workloads in the persistent registry. The supervisor refreshes
that registry and inspects process identity. The scheduler uses `CLOCK_MONOTONIC`
for intervals and skips missed ticks rather than creating a burst. It allows at
most four concurrent workers and prevents concurrent checkpoints for one
workload. CRIU performs the process dump; ChronoOS validates artifacts, manages
checkpoint metadata and latest references, and enforces retention. Process
identity checks use more than PID, and the bundled workload's CRIU authorization
uses a scoped peer-verified control channel.

CRIU supplies process image creation and restoration primitives. ChronoOS adds
registration, persistent supervision, per-workload scheduling, process
identity checks, checkpoint publication/retention, and failure detection.

## Demo steps

Run:

```sh
sh tests/demo_chronoos.sh
```

The script checks CRIU preflight before starting a workload, launches the
bundled counter in private state, produces at least two periodic checkpoints,
shows the latest valid checkpoint, verifies the counter remains live, injects
an unexpected exit through the identity-checked helper, and shows the FAILED
state. It retains its private artifacts and logs. It does not claim or perform
automatic restoration.

## Test evidence

- `make -B test` exercises process identity, registry/lifecycle behavior,
  checkpoint metadata and retention, fake-CRIU adapter behavior, supervisor
  operation, and scheduler restart.
- `make test-criu-integration` is separate and uses real CRIU. The verified run
  produced two periodic checkpoints for the managed counter while it remained
  RUNNING, and separately exercised a real dump/restore continuity path.
- Fake-CRIU tests are deterministic error-path tests, not evidence of real CRIU
  compatibility.

## Known limitations and future scope

`--leave-running` can produce inconsistent snapshots for applications that
continue mutating external files or sockets. Verification currently applies to
the bundled counter only. The standalone restore command and guided counter
restore demo are implemented; automatic recovery, fresh authorization after restore,
restored-process identity registration, recovery retry policy, recovery
history, richer checkpoint metrics, and experiments across workloads remain
future work. Automatic recovery must not be enabled until ChronoOS can prove a
new scoped authorization relationship for a restored PID/start-time identity.

## Likely faculty questions

**Why use CRIU?** CRIU already implements Linux task image dumping and restore,
including process memory and kernel-managed resources. Reimplementing those
kernel interfaces is outside ChronoOS's scope.

**What does ChronoOS contribute beyond invoking CRIU?** It maintains registered
process identities, supervises them persistently, schedules per-workload
checkpoints, validates/publishes checkpoint records, maintains latest-valid
references and retention, and detects unexpected disappearance.

**Does it restore every Linux process?** No. Only explicitly registered
workloads with validated CRIU compatibility and authorization are supported.
The bundled counter is the verified workload.

**Why is `--leave-running` risky?** The snapshot captures a point-in-time
process/kernel state while the live workload continues. Later file or socket
changes may not match that saved state, so replaying the snapshot can conflict
with external side effects. It is validated only for the bundled counter.

**Is automatic recovery complete?** No. Failure detection is present, but
automatic recovery is not. The restored workload must establish a fresh,
verified authorization channel, and ChronoOS must validate and re-register its
new identity before recovery can be claimed.

**What is the next step?** Design and test a restore-time handshake that
re-establishes scoped authorization using the restored PID/start time, then
persist recovery transitions and verify counter continuity before adding
bounded retry behavior.
