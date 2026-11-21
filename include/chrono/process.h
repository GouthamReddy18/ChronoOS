#ifndef CHRONO_PROCESS_H
#define CHRONO_PROCESS_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

#define CHRONO_COMMAND_MAX 4096

typedef struct {
    pid_t pid;
    uid_t owner;
    pid_t parent_pid;
    uint64_t start_time;
    char state;
    char executable[CHRONO_COMMAND_MAX];
    dev_t executable_device;
    ino_t executable_inode;
    char command[CHRONO_COMMAND_MAX];
} chrono_process_info;

int chrono_process_inspect(pid_t pid, chrono_process_info *info,
                           char *error, unsigned long error_size);
int chrono_managed_start(const char *counter_path, long interval, pid_t *pid,
                         uint64_t *start_time, char *error, unsigned long error_size);
int chrono_managed_start_bundled(long interval, pid_t *pid, uint64_t *start_time,
                                 char *error, unsigned long error_size);
int chrono_managed_status(pid_t pid, uint64_t start_time,
                          char *error, unsigned long error_size);
/* Returns a CLOEXEC pidfd after validating the registry identity. Caller closes it. */
int chrono_managed_pin(pid_t pid, uint64_t start_time,
                       char *error, unsigned long error_size);
/* Returns the private abstract-socket name after validating the managed identity. */
int chrono_managed_control_endpoint(pid_t pid, uint64_t start_time, char *name,
                                    size_t name_size, char *error,
                                    unsigned long error_size);
/* Queries the bundled counter's actual in-memory value through its verified
 * per-launch control channel. current_start_time is checked on every query. */
int chrono_managed_counter_query(pid_t pid, uint64_t current_start_time,
                                 uint64_t checkpoint_start_time,
                                 unsigned long long *counter,
                                 char *error, unsigned long error_size);
int chrono_managed_stop(pid_t pid, uint64_t start_time,
                        char *error, unsigned long error_size);
/* Sends SIGTERM through a pidfd only after matching owner, start time,
 * executable device/inode, and command. Success means the signal request was
 * accepted; this function does not wait for or prove process termination. */
int chrono_process_stop_verified(pid_t pid, uint64_t start_time, dev_t executable_device,
                                 ino_t executable_inode, const char *expected_command,
                                 char *error, unsigned long error_size);

#endif
