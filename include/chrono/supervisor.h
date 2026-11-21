#ifndef CHRONO_SUPERVISOR_H
#define CHRONO_SUPERVISOR_H

#include "chrono/process.h"

#include <stddef.h>
#include <stdint.h>

#define CHRONO_SUPERVISOR_ID_MAX 32
#define CHRONO_SUPERVISOR_PATH_MAX 4096
#define CHRONO_CHECKPOINT_INTERVAL_DEFAULT 10U
#define CHRONO_CHECKPOINT_INTERVAL_MIN 1U
#define CHRONO_CHECKPOINT_INTERVAL_MAX 3600U

typedef enum {
    CHRONO_SUPERVISOR_STARTING,
    CHRONO_SUPERVISOR_RUNNING,
    CHRONO_SUPERVISOR_STOPPING,
    CHRONO_SUPERVISOR_EXITED,
    CHRONO_SUPERVISOR_FAILED
} chrono_supervisor_state;

typedef struct {
    char id[CHRONO_SUPERVISOR_ID_MAX + 1];
    pid_t pid;
    uint64_t start_time;
    uid_t owner;
    dev_t executable_device;
    ino_t executable_inode;
    char executable[CHRONO_SUPERVISOR_PATH_MAX];
    char command[CHRONO_COMMAND_MAX];
    char working_directory[CHRONO_SUPERVISOR_PATH_MAX];
    int64_t registered_at;
    int64_t state_changed_at;
    chrono_supervisor_state state;
    int intentional_stop;
    int exit_status_known;
    int exit_code;
    int term_signal;
    char checkpoint_directory[CHRONO_SUPERVISOR_PATH_MAX];
    uint32_t checkpoint_interval_seconds;
} chrono_supervised_process;

typedef int (*chrono_supervisor_list_fn)(const chrono_supervised_process *, void *);

int chrono_supervisor_default_directory(char *path, size_t path_size,
                                        char *error, size_t error_size);
int chrono_supervisor_protect(const char *registry_directory, char *const argv[],
                              chrono_supervised_process *process,
                              char *error, size_t error_size);
int chrono_supervisor_protect_with_interval(const char *registry_directory,
                                            char *const argv[],
                                            uint32_t checkpoint_interval_seconds,
                                            chrono_supervised_process *process,
                                            char *error, size_t error_size);
int chrono_supervisor_protect_counter(const char *registry_directory,
                                      uint32_t checkpoint_interval_seconds,
                                      chrono_supervised_process *process,
                                      char *error, size_t error_size);
int chrono_supervisor_get(const char *registry_directory, const char *id,
                          chrono_supervised_process *process,
                          char *error, size_t error_size);
/* The callback runs while the registry lock is held. It must not call any
 * chrono_supervisor API or block waiting for another registry operation. */
int chrono_supervisor_list(const char *registry_directory,
                           chrono_supervisor_list_fn callback, void *context,
                           char *error, size_t error_size);
int chrono_supervisor_stop(const char *registry_directory, const char *id,
                           chrono_supervised_process *process,
                           char *error, size_t error_size);
int chrono_supervisor_id_valid(const char *id);
const char *chrono_supervisor_state_name(chrono_supervisor_state state);

#endif
