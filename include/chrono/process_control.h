#ifndef CHRONO_PROCESS_CONTROL_H
#define CHRONO_PROCESS_CONTROL_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define CHRONO_CONTROL_MAGIC UINT32_C(0x4348524f)
#define CHRONO_CONTROL_VERSION 2U
#define CHRONO_CONTROL_AUTHORIZE 1U
#define CHRONO_CONTROL_QUERY_COUNTER 2U
#define CHRONO_CONTROL_NAME_MAX 96

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t operation;
    int32_t target_pid;
    uint32_t reserved;
    uint64_t target_start_time;
} chrono_control_request;

typedef struct {
    uint32_t magic;
    int32_t status;
    int32_t error_number;
    uint32_t reserved;
    uint64_t value;
} chrono_control_response;

static inline int chrono_control_name(uid_t owner, pid_t pid, uint64_t start_time,
                                     const char token[33], char *name, size_t size)
{
    if (pid <= 1 || start_time == 0 || token == NULL || name == NULL || size == 0 ||
        strnlen(token, 33) != 32) return -1;
    for (size_t i = 0; i < 32; ++i) {
        if (!((token[i] >= '0' && token[i] <= '9') ||
              (token[i] >= 'a' && token[i] <= 'f'))) return -1;
    }
    int n = snprintf(name, size, "chronoos.%lu.%ld.%llu.%s", (unsigned long)owner,
                     (long)pid, (unsigned long long)start_time, token);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

#endif
