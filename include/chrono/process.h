#ifndef CHRONO_PROCESS_H
#define CHRONO_PROCESS_H

#include <sys/types.h>
#include <stdint.h>

#define CHRONO_COMMAND_MAX 4096

typedef struct {
    pid_t pid;
    uid_t owner;
    char state;
    char command[CHRONO_COMMAND_MAX];
} chrono_process_info;

int chrono_process_inspect(pid_t pid, chrono_process_info *info,
                           char *error, unsigned long error_size);
int chrono_managed_start(const char *counter_path, long interval, pid_t *pid,
                         uint64_t *start_time, char *error, unsigned long error_size);
int chrono_managed_status(pid_t pid, uint64_t start_time,
                          char *error, unsigned long error_size);
int chrono_managed_stop(pid_t pid, uint64_t start_time,
                        char *error, unsigned long error_size);

#endif
