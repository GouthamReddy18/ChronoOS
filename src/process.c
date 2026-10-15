#define _POSIX_C_SOURCE 200809L
#include "chrono/process.h"
#include "process_internal.h"

#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void set_error(char *error, unsigned long size, const char *message)
{
    if (error != NULL && size > 0) {
        (void)snprintf(error, (size_t)size, "%s", message);
    }
}

static int parse_decimal(const char **cursor, uintmax_t *value)
{
    const char *p = *cursor;
    uintmax_t result = 0;
    if (!isdigit((unsigned char)*p)) return -1;
    do {
        unsigned digit = (unsigned)(*p - '0');
        if (result > (UINTMAX_MAX - digit) / 10) return -1;
        result = result * 10 + digit;
        ++p;
    } while (isdigit((unsigned char)*p));
    *cursor = p;
    *value = result;
    return 0;
}

int chrono_parse_uid_line(const char *p, uid_t *owner)
{
    uintmax_t values[4];
    for (size_t i = 0; i < 4; ++i) {
        while (isspace((unsigned char)*p)) ++p;
        if (parse_decimal(&p, &values[i]) != 0) return -1;
        if ((uintmax_t)(uid_t)values[i] != values[i]) return -1;
    }
    while (isspace((unsigned char)*p)) ++p;
    if (*p != '\0') return -1;
    *owner = (uid_t)values[0];
    return 0;
}

static int read_start_time(pid_t pid, uint64_t *start_time)
{
    char path[64];
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    (void)snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *stat_file = fopen(path, "r");
    if (stat_file == NULL) return -1;
    length = getline(&line, &capacity, stat_file);
    int read_error = ferror(stat_file);
    (void)fclose(stat_file);
    if (length < 0 || read_error) { free(line); return -1; }

    char *close = strrchr(line, ')');
    if (close == NULL || close[1] != ' ') { free(line); return -1; }
    const char *p = close + 2;
    for (unsigned field = 3; field <= 22; ++field) {
        while (*p == ' ') ++p;
        const char *begin = p;
        while (*p != '\0' && *p != ' ' && *p != '\n') ++p;
        if (p == begin) { free(line); return -1; }
        if (field == 22) {
            char saved = * (char *)p;
            char *end;
            *(char *)p = '\0';
            errno = 0;
            unsigned long long parsed = strtoull(begin, &end, 10);
            int valid = errno == 0 && end != begin && *end == '\0';
            *(char *)p = saved;
            free(line);
            if (!valid) return -1;
            *start_time = (uint64_t)parsed;
            return 0;
        }
    }
    free(line);
    return -1;
}

int chrono_process_inspect(pid_t pid, chrono_process_info *info,
                           char *error, unsigned long error_size)
{
    char path[64];
    char line[1024];
    FILE *status;
    int have_uid = 0;
    int have_state = 0;
    int have_pid = 0;
    uint64_t start_before, start_after;

    if (pid <= 0 || info == NULL) {
        set_error(error, error_size, "PID must be a positive integer");
        return -1;
    }
    if (pid == 1) {
        set_error(error, error_size, "refusing to inspect PID 1");
        return -1;
    }
    if (pid == getpid()) {
        set_error(error, error_size, "refusing to inspect ChronoOS itself");
        return -1;
    }

    memset(info, 0, sizeof(*info));
    info->pid = pid;
    if (read_start_time(pid, &start_before) != 0) {
        set_error(error, error_size, "process exited or its identity could not be read");
        return -1;
    }
    (void)snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    status = fopen(path, "r");
    if (status == NULL) {
        if (errno == ENOENT || errno == ESRCH) {
            set_error(error, error_size, "process does not exist");
        } else if (errno == EACCES || errno == EPERM) {
            set_error(error, error_size, "permission denied reading process metadata");
        } else {
            set_error(error, error_size, "cannot read process metadata");
        }
        return -1;
    }

    while (fgets(line, sizeof(line), status) != NULL) {
        if (strncmp(line, "Pid:", 4) == 0) {
            char *end;
            errno = 0;
            long reported = strtol(line + 4, &end, 10);
            while (isspace((unsigned char)*end)) ++end;
            if (!have_pid && errno == 0 && end != line + 4 && *end == '\0' &&
                reported == (long)pid)
                have_pid = 1;
            else have_pid = -1;
        } else if (strncmp(line, "Uid:", 4) == 0) {
            if (!have_uid && chrono_parse_uid_line(line + 4, &info->owner) == 0) have_uid = 1;
            else have_uid = -1;
        } else if (strncmp(line, "State:", 6) == 0) {
            char state;
            if (sscanf(line + 6, " %c", &state) == 1) {
                info->state = state;
                have_state = 1;
            }
        }
    }
    int status_error = ferror(status);
    (void)fclose(status);
    if (status_error) {
        set_error(error, error_size, "error reading process metadata");
        return -1;
    }
    if (!have_pid || !have_uid || !have_state) {
        set_error(error, error_size, "invalid or incomplete process metadata in /proc");
        return -1;
    }
    if (info->owner != getuid() && geteuid() != 0) {
        set_error(error, error_size, "process belongs to another user");
        return -1;
    }

    (void)snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    FILE *cmdline = fopen(path, "r");
    if (cmdline == NULL) {
        set_error(error, error_size, "cannot read process command line");
        return -1;
    }
    size_t used = fread(info->command, 1, sizeof(info->command) - 1, cmdline);
    int cmdline_error = ferror(cmdline);
    (void)fclose(cmdline);
    if (cmdline_error) {
        set_error(error, error_size, "error reading process command line");
        return -1;
    }
    for (size_t i = 0; i < used; ++i) {
        if (info->command[i] == '\0') {
            info->command[i] = ' ';
        }
    }
    while (used > 0 && info->command[used - 1] == ' ') {
        --used;
    }
    info->command[used] = '\0';
    if (used == 0) {
        (void)snprintf(info->command, sizeof(info->command), "[kernel or unavailable]");
    }
    if (read_start_time(pid, &start_after) != 0 || start_before != start_after) {
        set_error(error, error_size, "process exited or changed identity during inspection");
        return -1;
    }
    return 0;
}
