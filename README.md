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
