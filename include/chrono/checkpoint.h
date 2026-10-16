#ifndef CHRONO_CHECKPOINT_H
#define CHRONO_CHECKPOINT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CHRONO_CHECKPOINT_ID_MAX 64
#define CHRONO_CHECKPOINT_TEXT_MAX 256

typedef enum {
    CHRONO_CHECKPOINT_PREPARING,
    CHRONO_CHECKPOINT_COMPLETE,
    CHRONO_CHECKPOINT_FAILED
} chrono_checkpoint_state;

typedef struct {
    char id[CHRONO_CHECKPOINT_ID_MAX + 1];
    pid_t original_pid;
    uint64_t process_start_time;
    int64_t created_at;
    char workload[CHRONO_CHECKPOINT_TEXT_MAX];
    char executable[CHRONO_CHECKPOINT_TEXT_MAX];
    chrono_checkpoint_state state;
    char backend[64];
} chrono_checkpoint_metadata;

/*
 * Transitions are metadata-only. Terminal transitions are serialized across
 * callers sharing the same private directory; at most one can move a record
 * out of PREPARING. COMPLETE means the backend reported success and its
 * structural artifact checks passed; it does not guarantee restorability.
 */

typedef int (*chrono_checkpoint_list_fn)(const chrono_checkpoint_metadata *, void *);
typedef struct {
    const char *name;
    int available;
    int (*checkpoint)(const chrono_checkpoint_metadata *, const char *, char *, size_t);
    int (*restore)(const chrono_checkpoint_metadata *, const char *, pid_t *, char *, size_t);
} chrono_checkpoint_backend;

int chrono_checkpoint_id_valid(const char *id);
int chrono_checkpoint_backend_available(void);
const char *chrono_checkpoint_backend_name(void);
const chrono_checkpoint_backend *chrono_checkpoint_backend_get(void);
int chrono_checkpoint_begin(const char *directory, const chrono_checkpoint_metadata *metadata,
                            char *error, size_t error_size);
int chrono_checkpoint_transition(const char *directory, const char *id,
                                 chrono_checkpoint_state state,
                                 char *error, size_t error_size);
/* Missing directory is an empty, successful listing; listing never creates it. */
int chrono_checkpoint_list(const char *directory, chrono_checkpoint_list_fn callback,
                           void *context, char *error, size_t error_size);
int chrono_checkpoint_read(const char *directory, const char *id,
                           chrono_checkpoint_metadata *metadata,
                           char *error, size_t error_size);
/*
 * Checks the adapter's private flat image-file layout and inventory presence,
 * not image integrity or compatibility with a particular CRIU version. The
 * flat-layout assumption is based on the --images-dir interface used here and
 * has not been verified against real CRIU in this project.
 */
int chrono_checkpoint_artifact_valid(const char *directory, const char *id);

#endif
