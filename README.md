# ChronoOS

A lightweight Linux process checkpoint management layer built on CRIU.

## Overview

ChronoOS is an Operating Systems project that manages selected Linux process
checkpoints through CRIU. ChronoOS validates managed process identities and
checkpoint metadata, then delegates process capture and restoration to CRIU;
it does not implement an independent checkpoint engine or bpfCP.

The project explores practical Linux operating-system concepts including
process management, process state, memory management, system calls, signals,
file handling, and process restoration.

The primary goal is to understand how process state can be captured,
stored, and restored while studying the performance and limitations of
checkpoint-based process recovery.

## Objectives

- Understand Linux process management and process state.
- Study the information required to checkpoint a running process.
- Capture and store relevant process state.
- Restore a process from a previously created checkpoint.
- Support rollback to an earlier process state.
- Handle process termination and recovery scenarios.
- Measure checkpointing and restoration overhead.
- Analyze storage requirements and recovery time.
- Study the trade-offs involved in checkpoint frequency and system overhead.

## Key OS Concepts

ChronoOS focuses on the following Operating Systems concepts:

- Process Management
- Process Control Block (PCB)
- Process State
- Process Identification (PID)
- Virtual Memory
- Memory Management
- System Calls
- Signals
- File Descriptors
- Inter-Process Communication
- Process Creation and Termination
- Process Recovery
- Checkpointing and Rollback

## Phase 1: CLI and process inspection

Phase 1 provides a small C CLI that reads metadata for an explicitly selected
process through `/proc`, plus a counter workload for later checkpoint/restore
experiments. It does not stop processes or invoke CRIU.

Build with `make`. The binaries are written under `build/`:

Run the Phase 1 regression suite with `make test`. It builds the programs and
checks process inspection, PID argument validation, UID parsing, and counter
shutdown on SIGINT and SIGTERM.

```sh
./build/chrono inspect PID
./build/counter_workload [interval-seconds]
```

The inspection command rejects PID 1, itself, and processes owned by another
user (unless run with effective UID 0). Run the counter workload as your own
user; it prints an incrementing counter and exits cleanly on SIGINT or SIGTERM.

Restoring a process does not reverse external effects such as completed file
writes, database changes, or network requests.

## Phase 2: controlled counter lifecycle

`chrono launch counter [interval]` launches only the bundled
`counter_workload` file beside the actual ChronoOS executable (interval
defaults to one second and must be 1..60). ChronoOS resolves its own executable
through Linux `/proc/self/exe`, so PATH invocation and symlink invocation use
the real executable's directory, independent of the current working directory.
The sibling workload must be a regular executable owned by the current UID and
must not be a symlink. ChronoOS opens and executes the validated file inode;
launch fails if that file cannot be identified or validated.
It prints a PID and Linux `/proc` start-time tick count. Keep both values as the
process identity:

```sh
./build/chrono launch counter 1
./build/chrono status PID STARTTIME
./build/chrono stop PID STARTTIME
```

Status and stop verify the start time, workload executable device/inode, owner
UID, a random per-launch token in the process command line, and the
`counter_workload` command line. Stop opens a Linux pidfd before validation and
sends SIGTERM only through that descriptor. If pidfd support is unavailable,
status and stop refuse to act. ChronoOS records launched identities in a
per-user mode-0700 directory under `/tmp`, using mode-0600 records and a lock
file. Stale records are removed only when `/proc` proves the recorded process
is gone, the PID has a different start time, or its live command line lacks the
recorded per-launch token; malformed or unreadable records fail closed.
Callers must retain both printed values, and records may
disappear at reboot. The token and registry protect against accidental PID
reuse and other users, but processes running as the same UID can inspect the
token in `/proc` and modify their own registry; this is not a boundary against
hostile same-UID processes. The CLI does not accept arbitrary commands or
paths.

The launcher forks and execs the counter in a new session, closes its inherited
terminal streams, and exits after receiving the exec handshake. The counter is
therefore not terminated when the CLI returns. Once the CLI exits, the counter
is reparented to the system's init process, or to the nearest configured child
subreaper. That adopter is responsible for reaping it when it exits; ChronoOS
cannot collect its exit status after detaching. While the launching CLI is
alive, it performs a nonblocking wait if the child already exited. There is no
supervisor or persistent wait-status service in this phase. The start-time
identity, separate session, and retained process state provide the basis for
the Phase 3B CRIU adapter; Phase 2 alone does not dump or restore processes.

Run `make test` for process inspection and controlled lifecycle coverage.

## Phase 3A: checkpoint metadata

Phase 3A adds private per-user metadata records with validated IDs, original
PID and `/proc` start-time identity, creation time, workload/executable fields,
backend name, and `PREPARING`, `COMPLETE`, or `FAILED` lifecycle states. Only
`COMPLETE` records appear in `chrono checkpoints`. Metadata files are written
with mode 0600 in a mode-0700 directory under `/tmp`; updates use a temporary
file and atomic rename. The directory is checked for ownership and permissions
and files are opened relative to its directory descriptor without following
symlinks. Listing is read-only; if the directory does not exist, it succeeds
with an empty result. A reported write error after publication can leave the
new metadata visible while directory durability remains uncertain.

The CHRONO1 metadata format is retained in Phase 3B. This metadata layer alone
does not capture process state. The artifact directory is derived from the
validated checkpoint ID as `CHECKPOINT_DIRECTORY/ID`.

Transitions out of `PREPARING` are serialized with a directory-local lock, so
among callers using the same directory at most one can successfully make the
transition; the lock relies on local filesystem `flock` semantics and does not
coordinate independent copies of the metadata directory. `COMPLETE` describes
the metadata transition; the CLI reports artifact validity separately and
validates CRIU inventory and file ownership/modes before restore.

## Phase 3B: optional CRIU backend

The CRIU adapter executes the CRIU binary directly with an argument vector; it
does not use a shell. Set `CHRONO_CRIU_PATH` when CRIU is not at the documented
fallback path `/usr/sbin/criu`. Before each operation ChronoOS runs plain
`criu check` and fails closed when the required kernel feature check does not
pass. This checks CRIU's required kernel features in the current execution
context; it does not guarantee that a particular process can be dumped or
restored. ChronoOS does not install CRIU or change system
capabilities. CRIU needs access to the target's `/proc` information and uses
ptrace while collecting process state. Kernel support such as
`CONFIG_CHECKPOINT_RESTORE` and, depending on the process and operation,
capabilities such as `CAP_CHECKPOINT_RESTORE` may be required. The exact access
requirements depend on the target resources and security policy; a passing
basic CRIU check does not guarantee every workload can be dumped or restored.
See the [CRIU kernel check guide](https://criu.org/CLI/cmd/check), [kernel
requirements](https://criu.org/Linux_kernel), and [checkpoint/restore
design](https://criu.org/Checkpoint/Restore).

`chrono checkpoint PID STARTTIME ID` requires the managed PID/start-time pair
to pass the Phase 2 registry, executable, token, and process checks. It creates
an exclusive private artifact directory, invokes CRIU dump for the process
tree, validates `inventory.img` and artifact ownership/modes, and only then
transitions metadata to `COMPLETE`. Failed or partial directories are retained
for inspection and never treated as usable. Existing IDs and artifact
directories are not overwritten.

The CLI retains the pidfd and repeats the registry, PID start-time, executable,
owner, and token checks immediately before dispatch. CRIU receives a numeric
PID rather than the pidfd and runs its capability check before dumping, so a
small race remains between validation and CRIU opening the target. The current
CRIU interface cannot make that operation atomic with the pinned pidfd; the
identity checks reduce the window but do not eliminate it.

`chrono checkpoints` prints metadata state separately from `ARTIFACT=valid` or
`ARTIFACT=missing-or-invalid`. The supported workload is the bundled detached
`counter_workload` process tree. Artifact validation requires a private
owner-controlled directory and a flat set of nonempty regular `.img` files
including `inventory.img` plus at least one payload image; it rejects links, special files, unsafe ownership
or permissions, and unexpected entries. These checks establish filesystem
layout and basic presence only. ChronoOS does not decode CRIU's
version-dependent inventory or prove cross-image consistency, completeness,
or restorability. The accepted flat image-directory layout follows the
`--images-dir` interface but has not been verified against a real CRIU run;
the adapter tests use a fake executable and do not establish CRIU format
compatibility. A failed dump can leave structurally plausible partial
images, so it remains `FAILED` and is not eligible for restore; those files are
retained as diagnostic evidence. Completed metadata alone does not establish that CRIU files
exist or can be restored. Restore requires a completed record and structurally
secure artifacts. A private one-time marker prevents repeated restore
attempts. If CRIU reports success but the PID file is absent or invalid,
ChronoOS reports that restoration may have occurred and does not retry. The
restored PID is reported from CRIU's PID file; restore normally
uses a new PID and does not replace the original Phase 2 registry entry. A
successful CRIU dump normally terminates the original process.

The managed counter is detached into its own session, while CRIU operates on
process trees. Restore may depend on namespace, kernel feature, capability,
security policy, file, socket, mount, and network support for the captured
tree. A successful dump does not prove that restore will work. Use an authorized
disposable Linux environment with suitable Linux capabilities, CRIU
permissions, and kernel support
for end-to-end experiments. Build with `make` and run `make test`, then set
`CHRONO_CRIU_PATH` and use a managed counter:

```sh
export CHRONO_CRIU_PATH="$HOME/criu-source/criu/criu"
./build/chrono launch counter 1
./build/chrono checkpoint PID STARTTIME trial-1
./build/chrono checkpoints
./build/chrono restore trial-1
```

Replace `PID` and `STARTTIME` with the values printed by `launch`. Observe
CRIU's diagnostics and both PIDs. Restore is attempted once per checkpoint ID.

The fake executable in `make test` covers argument dispatch, exit and signal
failures, availability checks, artifacts, and PID reporting. Passing these
fake-CRIU tests does not establish that real checkpoint or restore works. Real
CRIU checkpoint and restore remain unvalidated. In the inspected WSL2
environment, `/usr/sbin/criu` reports version 3.16.1 and `criu check` exits 1
after `Unable to create a socket: Operation not permitted` while initializing
`kerndat_tcp_repair`. The current process has zero effective, permitted, and
bounding capabilities; `/proc/self/status` reports seccomp mode 2 with one
filter; and `unshare -n true` also fails with `Operation not permitted`.
These observations establish that CRIU's kernel-feature preflight cannot run
in this execution context. They do not isolate seccomp, WSL, or another
container/host policy as the sole cause. Do not change shared machine security
configuration to work around that limit.

For real integration work, use a disposable Linux machine or VM with CRIU
supported kernel features, accessible procfs, and the privileges required by
the selected workload and kernel policy. Keep integration tests separate from
`make test`: use a dedicated managed counter, a fresh checkpoint ID, and an
empty private checkpoint directory; run `criu check` first; checkpoint the
counter; verify it stopped and inspect the checkpoint listing; restore once;
verify the reported restored PID is live and its counter continues from the
captured value; record its `/proc` start time, confirm its command line is the
counter workload, and send SIGTERM only to that verified test PID. Preserve
failed artifacts for diagnosis, and remove only the test's own checkpoint
directory after confirming it contains no needed evidence. Never run this plan
against a production process or shared state.

## Planned Architecture

```text
CLI → managed process identity validation → metadata and artifact validation
→ CRIU adapter → CRIU dump/restore → Linux process and image files
