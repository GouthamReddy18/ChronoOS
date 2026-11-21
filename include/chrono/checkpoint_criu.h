#ifndef CHRONO_CHECKPOINT_CRIU_H
#define CHRONO_CHECKPOINT_CRIU_H

#include "chrono/checkpoint.h"

int chrono_criu_test_backend_enabled(void);
int chrono_criu_path_trusted(const char *path);
int chrono_criu_available(char *error, size_t error_size);
int chrono_criu_checkpoint(const chrono_checkpoint_metadata *metadata,
                           const char *directory, char *error, size_t error_size);
/* Periodic snapshot mode. The workload must avoid external side effects that
 * would make its saved kernel/filesystem state stale while it keeps running. */
int chrono_criu_checkpoint_leave_running(const chrono_checkpoint_metadata *metadata,
                                         const char *directory,
                                         char *error, size_t error_size);
int chrono_criu_restore(const chrono_checkpoint_metadata *metadata,
                        const char *directory, pid_t *restored_pid,
                        char *error, size_t error_size);
int chrono_checkpoint_backend_available(void);
const chrono_checkpoint_backend *chrono_checkpoint_backend_get(void);

#endif
