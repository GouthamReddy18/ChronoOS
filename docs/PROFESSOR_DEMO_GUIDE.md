# ChronoOS Professor Demonstration Guide

This guide covers the current numbered-menu workflow for the bundled ChronoOS
counter. The underlying genuine CRIU 4.3 path was verified earlier through the
single-key workflow; the numbered menu has so far been regression-tested with
fake CRIU only and must not be presented as a fresh real-CRIU integration run.
Neither interface demonstrates automatic crash recovery or arbitrary
application support.

## Preparation

Use the native Ubuntu Linux environment that passed the CRIU integration. The
current verified host reports CRIU 4.3. Check the environment before the
presentation:

```sh
uname -a
criu --version
```

`make -B all` writes executables under `build/` and replaces existing build
outputs. If preserving an existing workspace build is important, first make a
separate project copy and build there. In that copy:

```sh
make -B all
make test
sh tests/test_counter_workload.sh
make test-criu-integration
```

The integration test is opt-in and uses a private temporary directory and its
own bundled counter workload. It retains its logs and checkpoint artifacts for
inspection. A preflight failure is an environment block, not a successful real
CRIU test. Fake-CRIU tests in `make test` are simulation-only and must not be
presented as genuine restoration.

Before the presentation:

1. Confirm that the terminal is on the tested native Linux host and `criu
   --version` reports the expected installed version.
2. Ensure the project binaries were built in the copy you intend to run.
3. Check terminal size with `stty size`; a terminal at least 45 rows high and
   100 columns wide is comfortable for the complete dashboard. Short or narrow
   terminals clip the frame to the available size.
4. Inspect existing counter processes without signaling them:

   ```sh
   ps -eo pid,uid,comm,args | grep '[c]ounter_workload'
   ```

   Do not stop a process unless its ChronoOS identity is known and you intend
   to manage it. The dashboard starts and cleans up its own workload.
5. Before starting the interactive session, confirm the dashboard command is
   available from the presentation terminal:

   ```sh
   ./build/chrono dashboard --help
   ```

6. Use a terminal with working keyboard input. The workflow requires explicit
   confirmation before terminating the original counter.

Expected behavior: each menu choice opens its own screen. The dashboard
identifies the bundled program by executable path, PID/start time, UID, and
executable identity; reports the counter from the verified managed control
channel; and displays the selected checkpoint and actual artifact directory.
The menu is limited to the bundled counter. The separate `demo-manual` command
retains the earlier single-key walkthrough.

## Exact live demonstration

Run the primary command:

```sh
CHRONO_CRIU_UNPRIVILEGED=1 ./build/chrono dashboard
```

Use the numbered menu in this order; each step is displayed separately:

1. Select **1** to launch the bundled counter. Review its name, absolute
   executable path, PID/parent PID, UID, process state, start time, executable
   identity, working directory, redacted launch arguments, and live counter.
2. Select **3** to revisit process details, then **4** to monitor live state.
   Press `r` to refresh and `q` to return to the menu.
3. Select **5** and confirm with `y` to create a checkpoint. Observe each
   identity, storage, CRIU, artifact-validation, and metadata step. The original
   remains alive after `--leave-running`.
4. Select **7** and choose the checkpoint from the displayed session list.
   Structural artifact validation does not prove that a later restore will
   succeed.
5. Select **6** to review the exact original process and request termination.
   Confirm with `y`; any other key cancels. ChronoOS rechecks identity before
   signaling and confirms that the process is terminated and reaped.
6. Select **8** to review the exact selected checkpoint and confirm restoration
   with `y`. The restored PID may equal the original numeric PID; start time,
   UID, executable device/inode, and liveness are checked independently.
7. Review the saved and live in-memory counter values. Recovery is reported
   verified only if the restored counter resumes beyond the saved sample and
   continues to advance.
8. Select **9** to reverify the restored process and **10** for the structured
   recovery report. Select **11** to exit; cleanup is restricted to this
   session's verified workload identities.

The separate `demo-manual` command retains the earlier key-driven walkthrough:

```sh
CHRONO_CRIU_UNPRIVILEGED=1 ./build/chrono demo-manual
```

`./build/chrono demo` is a separate bounded demonstration. The script
`sh tests/demo_chronoos.sh` demonstrates scheduled checkpointing and failure
detection, not automatic recovery. Keep those demonstrations distinct.

## Student speaking script

1. “ChronoOS demonstrates how Linux process state can be checkpointed and
   restored under supervision. We use a small counter because its application
   state is easy to observe and verify.”
2. “Checkpointing records process memory and supported kernel-managed
   resources so execution can resume later. ChronoOS delegates that low-level
   work to CRIU rather than implementing a checkpoint engine.”
3. “ChronoOS adds workload registration, process identity checks, checkpoint
   metadata, a foreground scheduler, and a supervised operator workflow.”
4. “The dashboard’s process table is read-only. The managed counter is shown
   separately, and its real in-memory value comes from a credential- and
   executable-verified control channel.”
5. “This checkpoint uses CRIU’s leave-running behavior. The original continues
   running after the image is written, so we can observe that it is still
   alive and its counter continues to change.”
6. “A saved process image and the still-running original are two process
   instances that may diverge. Before restoring the older saved state, the
   dashboard asks me explicitly and verifies the original identity again.”
7. “ChronoOS uses PID together with `/proc` start time, UID, and executable
   identity. A PID alone is not enough because Linux can reuse PIDs.”
8. “After CRIU returns, ChronoOS independently checks that the candidate
   process is live, non-zombie, owned by this user, and running the expected
   executable.”
9. “The counter starts from the saved in-memory value and advances again.
   That is evidence of application-state continuity for this workload; it is
   not proof that arbitrary applications or their external side effects were
   restored.”
10. “The verified scope is the bundled counter on this CRIU-enabled host.
    Scheduling and failure detection exist, but automatic crash recovery and
    arbitrary-process checkpoint authorization do not.”

## Professor viva: questions and answers

### 1. What is a Linux process?

A process is an executing program instance with an address space and associated
kernel-managed resources such as credentials, file descriptors, signal state,
and scheduling information. A process may contain multiple threads that share
many of those resources.

### 2. What information does `/proc` provide?

`/proc` is a pseudo-filesystem exposing kernel-maintained process and system
information. ChronoOS reads entries such as `stat`, `status`, `cmdline`, and
`exe` to inspect process state and identity. Access may be restricted, and an
unreadable entry is not treated as proof that the process is gone.

### 3. Why is a PID insufficient as an identity?

PIDs are reusable. A process can exit and a later unrelated process can
receive the same number. ChronoOS combines PID with process start time, UID,
executable device/inode, and workload-specific identity data where applicable.

### 4. What is the process start time used here?

ChronoOS reads the start-time tick count from `/proc/<pid>/stat`. It
distinguishes process instances that happen to use the same PID. It is an
identity component, not a wall-clock timestamp.

### 5. What does SIGTERM do?

SIGTERM requests termination and can be handled by the process. It does not
prove that a process exited. The dashboard waits for the verified original to
disappear and be reaped before allowing restoration.

### 6. Why use pidfds?

A pidfd is a file descriptor referring to a particular process instance. It
avoids sending a signal to a newly reused PID after validation. ChronoOS uses
pidfd-backed verified signaling where supported and fails closed where the
required safe operation is unavailable.

### 7. What does CRIU do?

CRIU collects process and resource state, writes image files, and later
reconstructs the process using Linux facilities. ChronoOS invokes CRIU with a
bounded adapter and validates metadata/artifact layout; it does not reimplement
CRIU internals.

### 8. What is stored in a checkpoint?

CRIU writes image files describing supported process state, including memory
pages and process/resource metadata. Exact files depend on CRIU and kernel
versions. ChronoOS stores separate metadata about the workload, checkpoint ID,
original identity, backend, timestamp, and lifecycle state.

### 9. What does `--leave-running` mean?

It asks CRIU to create a checkpoint while leaving the target process running.
This project uses it for the bundled counter so the original can visibly
continue after the snapshot. It can be inconsistent for workloads that mutate
external files, sockets, or other resources; it is not generalized as safe for
arbitrary applications.

### 10. Why explicitly terminate the original before restore?

The checkpoint contains an earlier state. Restoring while the original is
still running can create duplicate executions and conflicting side effects.
The demonstration therefore requires explicit confirmation, revalidates the
original identity, sends a verified SIGTERM, and waits for termination before
restoring.

### 11. Does restore necessarily use a different PID?

No. CRIU may reuse the original numeric PID after that process has terminated.
ChronoOS checks the new start time and executable identity rather than
assuming that a different PID proves restoration.

### 12. How is restored identity checked?

ChronoOS waits for the candidate to appear, then verifies a live non-zombie
process, current-user UID, start time, and expected executable device/inode and
command identity. A candidate is not adopted or signaled when identity is
ambiguous.

### 13. How does the demo prove application state continuity?

It queries the counter variable in the restored workload through the
peer-credential- and executable-verified control channel. The value is
compared to the checkpoint-time sample and must advance after restore. The
display-only `/proc/<pid>/comm` counter hint is not used as proof.

### 14. What role does the supervisor play?

The foreground supervisor owns the per-user registry lock and private control
socket, refreshes registered process identities, and schedules bounded
periodic checkpoint workers using monotonic deadlines. It detects failures
and records state, but it does not automatically restore failed workloads.

### 15. What is the difference between checkpoint scheduling and recovery?

Scheduling periodically requests snapshots. Recovery requires selecting a
valid image, deciding when and whether to restore, verifying the restored
process, and handling retries and external state. ChronoOS implements
supervised scheduling and a separate manually confirmed restore demo; it does
not implement automatic crash recovery.

### 16. What race conditions remain?

ChronoOS revalidates process identity immediately before an operation and uses
pidfds for signaling. CRIU’s dump interface takes a numeric PID, so validation
and CRIU attaching to the target cannot be made atomic by the current adapter.
The identity checks reduce but cannot eliminate that interval.

### 17. Why are inherited file descriptors relevant?

An inherited descriptor can expose unrelated resources to a child and may
prevent CRIU from dumping a process that holds unsupported handles. The
managed launcher marks unrelated descriptors close-on-exec and a regression
test checks that behavior.

### 18. Does ChronoOS checkpoint arbitrary processes?

No. Generic registration can manage process metadata, but it does not grant
checkpoint authorization or guarantee CRIU compatibility. The real end-to-end
validated workload is the bundled counter.

### 19. What is the difference between fake and real CRIU tests?

Fake-CRIU tests use a controlled executable to exercise adapter arguments,
failure handling, metadata transitions, and rejection behavior. They do not
create a genuine restored process. Only the opt-in integration test and live
dashboard on a CRIU-capable host provide real dump/restore evidence.

### 20. Does restoring memory roll back external side effects?

No. A process image cannot undo a database update, network request, or file
write that occurred outside the captured process state. Application-level
coordination is needed for those resources.

### 21. Is the dashboard supervisor persistent?

No. The dashboard runs a bounded, interactive demonstration session and
manages its own counter identities. The foreground supervisor is a separate
command and persists while its service process is running.

### 22. How does bpfCP relate to ChronoOS?

bpfCP is a research checkpointing approach that uses eBPF-related mechanisms.
ChronoOS does not implement bpfCP or its eBPF mechanisms; it delegates process
dump and restore to CRIU. See the report’s related-work note and citation.

### 23. What would be required for automatic recovery?

At minimum: a restore-time handshake that establishes fresh scoped
authorization, verification and registration of the restored process
identity, persistent recovery history, bounded retry policy, and explicit
handling of external resources. Those features need separate design and
testing before automatic recovery can be claimed.

## References for the presentation

- CRIU checkpoint/restore design: <https://criu.org/Checkpoint/Restore>
- CRIU kernel requirements: <https://criu.org/Linux_kernel>
- Deng and Kell, “bpfCP: Efficient and Extensible Process Checkpointing via
  eBPF,” ACM Workshop on eBPF and Kernel Extensions, 2025,
  <https://doi.org/10.1145/3748355.3748373>.
- The task brief names “Flashback” without identifying a specific paper or
  implementation. Because multiple unrelated systems use that name, do not
  attribute a particular design or result to it without the intended citation.
