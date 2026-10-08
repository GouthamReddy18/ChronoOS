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
