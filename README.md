# ChronoOS

A lightweight Linux process checkpoint and rollback system.

## Overview

ChronoOS is an Operating Systems project focused on developing a lightweight
mechanism for checkpointing the state of a running process and restoring the
process to a previously saved checkpoint.

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

CRIU checkpointing and restoration are planned for a later phase. Restoring a
process does not reverse external effects such as completed file writes,
database changes, or network requests.

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
identity, separate session, and retained process state provide a basis for
future CRIU integration; this phase does not dump or restore processes.

Run `make test` for process inspection and controlled lifecycle coverage.

## Planned Architecture

```text
                    ChronoOS
                       |
              +--------+--------+
              |                 |
        Checkpoint Module   Recovery Module
              |                 |
        Capture Process     Restore Process
             State              State
              |                 |
              +--------+--------+
                       |
                Checkpoint Data
                       |
                 Linux File System
