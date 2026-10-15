#ifndef CHRONO_PROCESS_H
#define CHRONO_PROCESS_H

#include <sys/types.h>

#define CHRONO_COMMAND_MAX 4096

typedef struct {
    pid_t pid;
    uid_t owner;
    char state;
    char command[CHRONO_COMMAND_MAX];
} chrono_process_info;

int chrono_process_inspect(pid_t pid, chrono_process_info *info,
                           char *error, unsigned long error_size);

#endif
