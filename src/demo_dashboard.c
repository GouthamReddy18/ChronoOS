#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "demo_dashboard.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <stdint.h>

#define SYSTEM_ROW_MAX 8
#define SYSTEM_PAGE_SIZE 8
#define DASHBOARD_HISTORY_LIMIT 6

#define ANSI_RESET "\033[0m"
#define ANSI_CYAN "\033[36m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_RED "\033[31m"

static struct termios saved_terminal;
static int terminal_saved;
static char *previous_tty_frame;
static size_t previous_tty_frame_length;
static size_t terminal_columns = 80;
static size_t terminal_rows;
static size_t previous_terminal_columns;
static size_t previous_terminal_rows;
static char last_tty_key;

int chrono_dashboard_input_feedback(char key, char *message, size_t message_size)
{
    if (message == NULL || message_size == 0) return -1;
    const char *action = NULL;
    switch ((unsigned char)tolower((unsigned char)key)) {
        case 'p': action = "PROCESS BROWSER"; break;
        case 'h': action = "HELP"; break;
        case 'l': action = "LAUNCH"; break;
        case 'c': action = "CHECKPOINT"; break;
        case 't': action = "STOP REQUEST"; break;
        case 'y': action = "CONFIRM"; break;
        case 'r': action = "RESTORE / REFRESH"; break;
        case 'q': action = "QUIT"; break;
        case 'n': action = "NEXT PAGE"; break;
        case 'b': action = "BACK"; break;
        case 'x': action = "CLOSE"; break;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8':
            action = "MONITOR ROW";
            break;
        default: action = "UNRECOGNIZED COMMAND"; break;
    }
    int length = snprintf(message, message_size, "[INPUT] Received: %c - %s", key, action);
    return length < 0 || (size_t)length >= message_size ? -1 : 0;
}

static int parse_pid(const char *text, pid_t *pid)
{
    if (text == NULL || *text == '\0') return -1;
    unsigned long value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return -1;
        unsigned digit = (unsigned)(*p - '0');
        if (value > ((unsigned long)INT_MAX - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    if (value == 0) return -1;
    *pid = (pid_t)value;
    return 0;
}

static void safe_text(char *destination, size_t size, const char *source)
{
    if (size == 0) return;
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)source;
         p != NULL && *p != '\0' && used + 1 < size; ++p) {
        destination[used++] = isprint(*p) ? (char)*p : '?';
    }
    destination[used] = '\0';
}

static void user_name(uid_t uid, char *name, size_t size)
{
    char buffer[16384];
    struct passwd entry, *result = NULL;
    if (getpwuid_r(uid, &entry, buffer, sizeof(buffer), &result) == 0 && result != NULL)
        safe_text(name, size, entry.pw_name);
    else
        (void)snprintf(name, size, "%lu", (unsigned long)uid);
}

static int parse_process_stat(pid_t pid, chrono_dashboard_process *process)
{
    char path[64];
    char *line = NULL;
    size_t capacity = 0;
    (void)snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (file == NULL) return -1;
    ssize_t length = getline(&line, &capacity, file);
    int failed = ferror(file);
    (void)fclose(file);
    if (length <= 0 || failed) { free(line); return -1; }
    char *close = strrchr(line, ')');
    if (close == NULL || close[1] != ' ') { free(line); return -1; }
    const char *cursor = close + 2;
    char *end = NULL;
    errno = 0;
    long parsed_parent;
    if (*cursor == '\0' || *cursor == '\n') { free(line); return -1; }
    process->state = *cursor++;
    while (*cursor == ' ') ++cursor;
    errno = 0;
    parsed_parent = strtol(cursor, &end, 10);
    if (errno != 0 || end == cursor || parsed_parent < 0 || parsed_parent > INT_MAX) {
        free(line);
        return -1;
    }
    process->parent_pid = (pid_t)parsed_parent;
    cursor = end;
    for (unsigned field = 5; field <= 22; ++field) {
        while (*cursor == ' ') ++cursor;
        const char *begin = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\n') ++cursor;
        if (cursor == begin) { free(line); return -1; }
        if (field == 22) {
            errno = 0;
            unsigned long long start = strtoull(begin, &end, 10);
            if (errno != 0 || end != cursor) { free(line); return -1; }
            process->start_time = (uint64_t)start;
        }
    }
    free(line);
    process->pid = pid;
    return 0;
}

static int read_dashboard_process(pid_t pid, chrono_dashboard_process *process)
{
    char path[64], line[512];
    memset(process, 0, sizeof(*process));
    if (parse_process_stat(pid, process) != 0) return -1;
    uint64_t initial_start = process->start_time;
    (void)snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *status = fopen(path, "r");
    if (status == NULL) return -1;
    int have_uid = 0;
    while (fgets(line, sizeof(line), status) != NULL) {
        if (strncmp(line, "Uid:", 4) != 0) continue;
        errno = 0;
        char *end = NULL;
        unsigned long value = strtoul(line + 4, &end, 10);
        if (errno == 0 && end != line + 4 && value <= UINT_MAX) {
            process->owner = (uid_t)value;
            have_uid = 1;
        }
        break;
    }
    int read_error = ferror(status);
    (void)fclose(status);
    if (!have_uid || read_error) return -1;
    (void)snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    FILE *command = fopen(path, "r");
    if (command == NULL) return -1;
    if (fgets(line, sizeof(line), command) == NULL) {
        (void)fclose(command);
        return -1;
    }
    read_error = ferror(command);
    (void)fclose(command);
    if (read_error) return -1;
    line[strcspn(line, "\r\n")] = '\0';
    safe_text(process->command, sizeof(process->command), line);
    user_name(process->owner, process->user, sizeof(process->user));
    (void)snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    ssize_t executable_length = readlink(path, process->executable,
                                         sizeof(process->executable) - 1);
    if (executable_length >= 0) process->executable[executable_length] = '\0';
    if (parse_process_stat(pid, process) != 0 || process->start_time != initial_start)
        return -1;
    return 0;
}

static int compare_processes(const void *left, const void *right)
{
    const chrono_dashboard_process *a = left;
    const chrono_dashboard_process *b = right;
    return a->pid < b->pid ? -1 : a->pid > b->pid ? 1 : 0;
}

int chrono_dashboard_process_snapshot(chrono_dashboard_process **processes,
                                      size_t *count, size_t *inaccessible)
{
    if (processes == NULL || count == NULL || inaccessible == NULL) return -1;
    *processes = NULL;
    *count = 0;
    *inaccessible = 0;
    DIR *proc = opendir("/proc");
    if (proc == NULL) return -1;
    size_t capacity = 0;
    struct dirent *entry;
    int read_error = 0;
    for (;;) {
        errno = 0;
        entry = readdir(proc);
        if (entry == NULL) {
            read_error = errno;
            break;
        }
        pid_t pid;
        if (parse_pid(entry->d_name, &pid) != 0) continue;
        chrono_dashboard_process process;
        if (read_dashboard_process(pid, &process) != 0) {
            ++*inaccessible;
            continue;
        }
        if (*count == capacity) {
            size_t next = capacity == 0 ? 64 : capacity * 2;
            if (next < capacity || next > SIZE_MAX / sizeof(**processes)) {
                (void)closedir(proc);
                free(*processes);
                *processes = NULL;
                *count = 0;
                return -1;
            }
            chrono_dashboard_process *grown = realloc(*processes, next * sizeof(**processes));
            if (grown == NULL) {
                (void)closedir(proc);
                free(*processes);
                *processes = NULL;
                *count = 0;
                return -1;
            }
            *processes = grown;
            capacity = next;
        }
        (*processes)[(*count)++] = process;
    }
    if (closedir(proc) != 0) read_error = errno;
    if (read_error != 0) {
        free(*processes);
        *processes = NULL;
        *count = 0;
        return -1;
    }
    qsort(*processes, *count, sizeof(**processes), compare_processes);
    return 0;
}

int chrono_dashboard_process_refresh(pid_t pid, uint64_t expected_start_time,
                                     chrono_dashboard_process *process,
                                     char *error, size_t error_size)
{
    if (pid <= 0 || process == NULL ||
        read_dashboard_process(pid, process) != 0 ||
        (expected_start_time != 0 && process->start_time != expected_start_time)) {
        if (error != NULL && error_size != 0)
            (void)snprintf(error, error_size, "process exited, is inaccessible, or its PID identity changed");
        return -1;
    }
    return 0;
}

void chrono_dashboard_process_snapshot_free(chrono_dashboard_process *processes)
{
    free(processes);
}

static const char *checkpoint_state(const chrono_checkpoint_metadata *metadata,
                                   int artifact_valid)
{
    if (metadata == NULL) return "NOT CREATED";
    if (metadata->state == CHRONO_CHECKPOINT_COMPLETE)
        return artifact_valid ? "COMPLETE / VALID" : "INVALID ARTIFACTS";
    if (metadata->state == CHRONO_CHECKPOINT_FAILED) return "FAILED";
    return "PREPARING";
}

static unsigned long long artifact_size(const char *directory, const char *id)
{
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, id);
    if (length < 0 || (size_t)length >= sizeof(path)) return 0;
    DIR *artifacts = opendir(path);
    if (artifacts == NULL) return 0;
    unsigned long long total = 0;
    struct dirent *entry;
    while ((entry = readdir(artifacts)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char item[PATH_MAX];
        int item_length = snprintf(item, sizeof(item), "%s/%s", path, entry->d_name);
        if (item_length < 0 || (size_t)item_length >= sizeof(item)) continue;
        struct stat st;
        if (lstat(item, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            total += (unsigned long long)st.st_size;
    }
    (void)closedir(artifacts);
    return total;
}

static void print_created_at(FILE *output, int64_t timestamp)
{
    time_t value = (time_t)timestamp;
    struct tm local;
    char text[40];
    if (localtime_r(&value, &local) != NULL && strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local) != 0)
        (void)fprintf(output, "%s", text);
    else
        (void)fprintf(output, "%lld", (long long)timestamp);
}

static const char *status_color(const char *status)
{
    if (strcasestr(status, "FAILED") != NULL || strcasestr(status, "INVALID") != NULL ||
        strcasestr(status, "BLOCKED") != NULL || strcasestr(status, "ERROR") != NULL)
        return ANSI_RED;
    if (strcasestr(status, "COMPLETE") != NULL || strcasestr(status, "VERIFIED") != NULL ||
        strcasestr(status, "RUNNING") != NULL || strcasestr(status, "PASSED") != NULL ||
        strstr(status, "passed") != NULL)
        return ANSI_GREEN;
    return ANSI_YELLOW;
}

static void print_status(FILE *output, const char *status, int tty_mode)
{
    if (tty_mode) (void)fputs(status_color(status), output);
    (void)fputs(status, output);
    if (tty_mode) (void)fputs(ANSI_RESET, output);
}

static const char *process_kind(const chrono_dashboard_view *view,
                                const chrono_dashboard_process *process)
{
    if (process->pid == getpid()) return "CHRONOOS";
    if (view->managed_process != NULL && process->pid == view->managed_process->pid &&
        process->start_time == view->managed_process->start_time) return "MANAGED";
    return process->owner == getuid() ? "USER" : "SYSTEM";
}

static void render_process_browser(FILE *output, const chrono_dashboard_view *view,
                                   int tty_mode, const char *system_name)
{
    (void)fprintf(output, "CHRONOOS PROCESS BROWSER | %s\n", system_name);
    if (view->process_help) {
        (void)fputs("READ-ONLY PROCESS DISCOVERY AND MONITORING\n"
                    "  1-8  Select a listed row for read-only identity monitoring.\n"
                    "  n    Show the next process page; b shows the previous page.\n"
                    "  r    Refresh /proc. Processes may disappear during inspection.\n"
                    "  h    Return to browsing; x closes the browser.\n"
                    "Monitoring never signals, checkpoints, or changes the selected process.\n"
                    "Only the dedicated bundled ChronoOS counter is checkpoint-tested.\n"
                    "[b]ack to browser\n", output);
        return;
    }
    if (view->monitored_process != NULL) {
        const chrono_dashboard_process *process = view->monitored_process;
        (void)fprintf(output, "READ-ONLY MONITOR | %s\n", view->monitored_status != NULL ?
                      view->monitored_status : "identity checked");
        (void)fprintf(output, "PID=%ld PPID=%ld USER=%s UID=%lu STATE=%c START=%llu\n",
                      (long)process->pid, (long)process->parent_pid, process->user,
                      (unsigned long)process->owner, process->state,
                      (unsigned long long)process->start_time);
        (void)fprintf(output, "CLASS=%s\nCOMMAND (DISPLAY ONLY)=%s\n",
                      process_kind(view, process), process->command);
        (void)fprintf(output, "EXECUTABLE (IF ACCESSIBLE)=%s\n",
                      process->executable[0] != '\0' ? process->executable : "not accessible");
        (void)fputs("CHECKPOINT COMPATIBILITY=NOT EVALUATED; only the bundled counter is supported\n"
                    "[r]efresh identity  [b]ack to process list  [x]close browser\n", output);
        return;
    }
    size_t page_size = view->process_page_size == 0 ? SYSTEM_PAGE_SIZE :
                       view->process_page_size;
    size_t page_count = view->process_count == 0 ? 1 :
                        (view->process_count + page_size - 1) / page_size;
    size_t page = view->process_page < page_count ? view->process_page : page_count - 1;
    size_t first = page * page_size;
    size_t end = first + page_size;
    if (end > view->process_count) end = view->process_count;
    (void)fprintf(output, "Accessible: %zu | Skipped (transient/inaccessible): %zu | Page %zu/%zu\n",
                  view->process_count, view->inaccessible_processes, page + 1, page_count);
    (void)fputs("Selection monitors only; PID reuse is checked using start time.\n"
                "ROW PID     PPID    USER             UID    S COMMAND | CLASS\n", output);
    for (size_t i = first; i < end; ++i) {
        const chrono_dashboard_process *process = &view->processes[i];
        (void)fprintf(output, "%-3zu %-7ld %-7ld %-16.16s %-6lu %c %-24.24s %s\n",
                      i - first + 1, (long)process->pid, (long)process->parent_pid,
                      process->user, (unsigned long)process->owner, process->state,
                      process->command, process_kind(view, process));
    }
    if (first == end) (void)fputs("No readable processes were discovered.\n", output);
    (void)fprintf(output, "Checkpoint compatibility is not evaluated for listed processes; only the bundled counter is supported.\n"
                  "[1-8] monitor row  [n]ext page  [b]ack page  [r]efresh  [h]elp  [x]close\n");
    if (!tty_mode) (void)fputs("Non-TTY input accepts one command per line.\n", output);
}

static void render_compact_dashboard(FILE *output, const chrono_dashboard_view *view,
                                    int tty_mode, size_t rows, const char *system_name)
{
    char user[CHRONO_DASHBOARD_USER_MAX];
    user_name(getuid(), user, sizeof(user));
    char status[64], version[64], diagnostic[512], stamp[40] = "time unavailable";
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) != NULL)
        (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    safe_text(status, sizeof(status), view->criu_status != NULL ? view->criu_status : "unknown");
    safe_text(version, sizeof(version), view->criu_version != NULL ? view->criu_version : "version unavailable");
    (void)fprintf(output, "CHRONOOS | PROCESS CONTROL CENTER | COMPACT\nHost: %s | User: %s\nTime: %s | Phase: %s\n",
                  system_name, user, stamp, view->phase != NULL ? view->phase : "idle");
    (void)fprintf(output, "Dashboard: ACTIVE | Supervisor: %s\n",
                  view->managed_process != NULL ? "managed workload active" : "session-only");
    (void)fputs("CRIU: ", output);
    print_status(output, status, tty_mode);
    (void)fprintf(output, " (%s; %s)\n", version,
                  view->criu_mode != NULL ? view->criu_mode : "mode unavailable");
    if (rows >= 24 && view->criu_diagnostic != NULL && view->criu_diagnostic[0] != '\0') {
        safe_text(diagnostic, sizeof(diagnostic), view->criu_diagnostic);
        (void)fprintf(output, "CRIU diagnostic: %s\n", diagnostic);
    }
    (void)fprintf(output, "PROCESSES (read-only): %zu accessible, %zu skipped; press p to browse all\n",
                  view->process_count, view->inaccessible_processes);
    size_t preview = rows >= 18 ? (view->process_count < 2 ? view->process_count : 2) : 0;
    for (size_t i = 0; i < preview; ++i) {
        const chrono_dashboard_process *process = &view->processes[i];
        (void)fprintf(output, "  %ld %-12.12s %c %-18.18s %s\n",
                      (long)process->pid, process->user, process->state,
                      process->command, process_kind(view, process));
    }
    (void)fputs("WORKLOAD: ", output);
    if (view->managed_process != NULL) {
        (void)fprintf(output, "PID=%ld UID=%lu STATE=%c START=%llu COUNTER=",
                      (long)view->managed_process->pid, (unsigned long)view->managed_process->owner,
                      view->managed_process->state,
                      (unsigned long long)view->managed_process->start_time);
        if (view->counter_available) (void)fprintf(output, "%llu\n", view->counter);
        else (void)fputs("unavailable\n", output);
        if (view->display_counter_available)
            (void)fprintf(output, "  /proc comm (display only): %llu\n", view->display_counter);
    } else {
        (void)fprintf(output, "%s; l launches the separate bundled counter\n",
                      view->managed_status != NULL ? view->managed_status : "not launched");
    }
    (void)fputs("CHECKPOINT: ", output);
    if (view->checkpoint != NULL) {
        char safe_id[64], safe_directory[PATH_MAX], image_path[PATH_MAX];
        safe_text(safe_id, sizeof(safe_id), view->checkpoint->id);
        if (view->checkpoint_directory != NULL) {
            (void)snprintf(image_path, sizeof(image_path), "%s/%s",
                           view->checkpoint_directory, view->checkpoint->id);
            safe_text(safe_directory, sizeof(safe_directory), image_path);
        } else {
            (void)snprintf(safe_directory, sizeof(safe_directory), "unavailable");
        }
        (void)fprintf(output, "%s ID=%s; artifacts %s\n",
                      checkpoint_state(view->checkpoint, view->checkpoint_artifact_valid),
                      safe_id, view->checkpoint_artifact_valid ? "valid" : "not validated");
        (void)fprintf(output, "  IMAGES %s\n", safe_directory);
        if (view->checkpoint_directory != NULL && rows >= 20) {
            char metadata_path[PATH_MAX], safe_metadata[PATH_MAX];
            (void)snprintf(metadata_path, sizeof(metadata_path), "%s/%s.meta",
                           view->checkpoint_directory, view->checkpoint->id);
            safe_text(safe_metadata, sizeof(safe_metadata), metadata_path);
            (void)fprintf(output, "  META %s\n", safe_metadata);
        }
    } else {
        char safe_directory[PATH_MAX];
        safe_text(safe_directory, sizeof(safe_directory),
                  view->checkpoint_directory != NULL ? view->checkpoint_directory : "unavailable");
        (void)fprintf(output, "not created; target %s\n", safe_directory);
        if (rows >= 24)
            (void)fputs("  CRIU --leave-running keeps the bundled counter alive; no unrelated process is signaled.\n",
                        output);
    }
    (void)fprintf(output, "RESTORE: %s\n",
                  view->restore_status != NULL ? view->restore_status : "NOT ATTEMPTED");
    if (view->restored_process != NULL)
        (void)fprintf(output, "RESTORED: PID=%ld START=%llu UID=%lu; %s\n",
                      (long)view->restored_process->pid,
                      (unsigned long long)view->restored_process->start_time,
                      (unsigned long)view->restored_process->owner,
                      view->counter_status != NULL ? view->counter_status : "counter checking");
    else
        (void)fputs("RESTORED: not yet verified\n", output);
    (void)fputs("Keys: p browser | h help | l launch | c checkpoint | t stop request | y confirm | r restore | q quit\n",
                output);
    if (view->feedback != NULL && view->feedback[0] != '\0')
        (void)fprintf(output, "%s\n", view->feedback);
    size_t history_limit = rows >= 18 ? 1 : 0;
    size_t history_count = view->event_count < history_limit ? view->event_count : history_limit;
    if (history_count != 0) {
        size_t index = view->event_count - history_count;
        char safe_timestamp[40], safe_message[192];
        safe_text(safe_timestamp, sizeof(safe_timestamp), view->events[index].timestamp);
        safe_text(safe_message, sizeof(safe_message), view->events[index].message);
        (void)fprintf(output, "LATEST EVENT: %s %s\n", safe_timestamp, safe_message);
    }
    if (rows >= 24)
        (void)fputs("Only the bundled counter is checkpoint-tested; full history is in the diagnostic log.\n",
                    output);
}

static void render_workflow_process(FILE *output, const chrono_process_info *process,
                                   const char *program_name, const char *cwd,
                                   const char *arguments)
{
    char executable[CHRONO_COMMAND_MAX], safe_name[CHRONO_COMMAND_MAX];
    char safe_cwd[CHRONO_COMMAND_MAX], safe_arguments[CHRONO_COMMAND_MAX];
    safe_text(executable, sizeof(executable), process->executable);
    safe_text(safe_name, sizeof(safe_name),
              program_name != NULL ? program_name : process->executable);
    safe_text(safe_cwd, sizeof(safe_cwd), cwd != NULL ? cwd : "unavailable");
    safe_text(safe_arguments, sizeof(safe_arguments),
              arguments != NULL ? arguments : process->command);
    (void)fprintf(output, "  Program:          %s\n", safe_name);
    (void)fprintf(output, "  Executable:       %s\n", executable);
    (void)fprintf(output, "  PID / parent PID: %ld / %ld\n",
                  (long)process->pid, (long)process->parent_pid);
    if (process->state == '-') {
        (void)fprintf(output, "  UID / state:      %lu / TERMINATED AND REAPED\n",
                      (unsigned long)process->owner);
    } else {
        (void)fprintf(output, "  UID / state:      %lu / %c\n",
                      (unsigned long)process->owner, process->state);
    }
    (void)fprintf(output, "  Start time:       %llu (Linux proc start-time ticks)\n",
                  (unsigned long long)process->start_time);
    (void)fprintf(output, "  Executable ID:    device=%llu inode=%llu\n",
                  (unsigned long long)process->executable_device,
                  (unsigned long long)process->executable_inode);
    (void)fprintf(output, "  Working dir:      %s\n", safe_cwd);
    (void)fprintf(output, "  Launch arguments: %s\n", safe_arguments);
}

static void render_workflow_checkpoint(FILE *output,
                                       const chrono_checkpoint_metadata *metadata,
                                       const char *directory, int artifacts_valid,
                                       size_t index, unsigned long long saved_counter)
{
    char id[CHRONO_CHECKPOINT_ID_MAX + 1], executable[CHRONO_CHECKPOINT_TEXT_MAX];
    char workload[CHRONO_CHECKPOINT_TEXT_MAX], artifact_path[PATH_MAX];
    safe_text(id, sizeof(id), metadata->id);
    safe_text(executable, sizeof(executable), metadata->executable);
    safe_text(workload, sizeof(workload), metadata->workload);
    if (directory == NULL ||
        snprintf(artifact_path, sizeof(artifact_path), "%s/%s", directory, id) < 0)
        (void)snprintf(artifact_path, sizeof(artifact_path), "unavailable");
    (void)fprintf(output, "  Selection:       %zu (%s)\n", index + 1, id);
    (void)fprintf(output, "  Program:         %s\n", workload);
    (void)fprintf(output, "  Executable:      %s\n", executable);
    (void)fprintf(output, "  Original PID:    %ld\n", (long)metadata->original_pid);
    (void)fprintf(output, "  Start time:      %llu\n",
                  (unsigned long long)metadata->process_start_time);
    (void)fputs("  Created:         ", output);
    print_created_at(output, metadata->created_at);
    (void)fprintf(output, " (%lld)\n", (long long)metadata->created_at);
    (void)fprintf(output, "  Saved counter:   %llu (in-memory workload state)\n", saved_counter);
    (void)fprintf(output, "  Checkpoint dir:  %s\n", artifact_path);
    (void)fprintf(output, "  Artifact check:  %s (structure only; not restorability)\n",
                  artifacts_valid ? "PASS" : "FAIL");
}

static void render_workflow_screen(FILE *output, const chrono_dashboard_view *view,
                                   int tty_mode)
{
    char phase[96], message[512], step[256], selection[32];
    char safe_kernel[160], criu_version[160], criu_status[96], criu_mode[96];
    char stamp[40] = "time unavailable";
    struct utsname system;
    time_t now = time(NULL);
    struct tm local;
    char kernel[160] = "Linux version unavailable";
    if (localtime_r(&now, &local) != NULL)
        (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    if (uname(&system) == 0)
        (void)snprintf(kernel, sizeof(kernel), "%s %s", system.sysname, system.release);
    safe_text(safe_kernel, sizeof(safe_kernel), kernel);
    safe_text(criu_version, sizeof(criu_version),
              view->criu_version != NULL ? view->criu_version : "version unavailable");
    safe_text(criu_status, sizeof(criu_status),
              view->criu_status != NULL ? view->criu_status : "preflight not run");
    safe_text(criu_mode, sizeof(criu_mode),
              view->criu_mode != NULL ? view->criu_mode : "mode unavailable");
    safe_text(phase, sizeof(phase), view->phase != NULL ? view->phase : "SESSION");
    safe_text(message, sizeof(message), view->screen_message != NULL ? view->screen_message : "");
    safe_text(step, sizeof(step), view->operation_step != NULL ? view->operation_step : "");
    safe_text(selection, sizeof(selection), view->selection != NULL ? view->selection : "");
    if (tty_mode) (void)fputs(ANSI_CYAN, output);
    (void)fprintf(output, "CHRONOOS  |  PROCESS CONTROL CENTER\n%s\n", phase);
    if (tty_mode) (void)fputs(ANSI_RESET, output);
    (void)fprintf(output, "%s | %s | CRIU %s | %s | Mode: %s\n", safe_kernel, stamp,
        criu_version, criu_status, criu_mode);

    switch (view->screen) {
        case CHRONO_DASHBOARD_SCREEN_MENU:
            (void)fputs("\nMAIN MENU\n", output);
            if (view->terminal_width != 0 && view->terminal_width < 52) {
                (void)fputs(" 1 Launch demo       2 Custom: unavailable\n"
                            " 3 Process details   4 Monitor\n"
                            " 5 Checkpoint        6 Stop original\n"
                            " 7 Browse images     8 Restore selected\n"
                            " 9 Verify restore   10 Recovery report\n"
                            "11 Exit\n", output);
            } else {
                (void)fputs("  1  Launch bundled counter demonstration\n"
                    "  2  Launch custom executable       UNAVAILABLE (counter-only authorization)\n"
                    "  3  View selected process details\n"
                    "  4  Monitor selected process\n"
                    "  5  Create checkpoint\n"
                    "  6  Stop original process (confirmation required)\n"
                    "  7  Browse available checkpoints\n"
                    "  8  Restore selected checkpoint\n"
                    "  9  Verify restored process\n"
                    " 10  View recovery report\n"
                    " 11  Exit\n", output);
            }
            (void)fprintf(output, "\nSelection: %s\n", selection);
            break;
        case CHRONO_DASHBOARD_SCREEN_PROGRAM:
            (void)fputs("\nPROGRAM IDENTIFICATION\n", output);
            if (view->managed_process != NULL)
                render_workflow_process(output, view->managed_process,
                    view->program_name, view->working_directory, view->launch_arguments);
            else
                (void)fputs("  Bundled workload: not launched\n", output);
            (void)fputs("  Custom executables: disabled; checkpoint authorization is counter-scoped.\n",
                        output);
            break;
        case CHRONO_DASHBOARD_SCREEN_DETAILS:
            (void)fputs("\nSELECTED PROCESS DETAILS\n", output);
            if (view->managed_process != NULL)
                render_workflow_process(output, view->managed_process,
                    view->program_name, view->working_directory, view->launch_arguments);
            else
                (void)fputs("  No ChronoOS-managed process is selected.\n", output);
            (void)fputs("  Identity is PID + start time + UID + executable device/inode.\n", output);
            break;
        case CHRONO_DASHBOARD_SCREEN_MONITOR:
            (void)fputs("\nPROCESS MONITORING\n", output);
            if (view->managed_process != NULL) {
                render_workflow_process(output, view->managed_process,
                    view->program_name, view->working_directory, view->launch_arguments);
                (void)fputs("  Live in-memory counter: ", output);
                if (view->counter_available)
                    (void)fprintf(output, "%llu\n", view->counter);
                else (void)fputs("unavailable\n", output);
                (void)fputs("  /proc/comm is display-only, never counter state.\n", output);
            } else (void)fputs("  No managed process is available to monitor.\n", output);
            break;
        case CHRONO_DASHBOARD_SCREEN_CHECKPOINT:
            (void)fputs("\nCHECKPOINT MANAGEMENT\n", output);
            if (view->managed_process != NULL)
                render_workflow_process(output, view->managed_process,
                    view->program_name, view->working_directory, view->launch_arguments);
            if (view->checkpoint != NULL)
                render_workflow_checkpoint(output, view->checkpoint,
                    view->checkpoint_directory, view->checkpoint_artifact_valid, 0,
                    view->checkpoint_row_count > view->selected_checkpoint_index ?
                        view->checkpoint_rows[view->selected_checkpoint_index].saved_counter : 0);
            if (view->checkpoint != NULL &&
                view->checkpoint_row_count > view->selected_checkpoint_index) {
                const chrono_dashboard_checkpoint_row *row =
                    &view->checkpoint_rows[view->selected_checkpoint_index];
                (void)fprintf(output, "  Origin UID:      %lu | executable device=%llu inode=%llu\n",
                    (unsigned long)row->original_uid,
                    (unsigned long long)row->executable_device,
                    (unsigned long long)row->executable_inode);
                (void)fprintf(output, "  Original state:  %s\n",
                    view->managed_status != NULL ? view->managed_status : "not verified");
            }
            else
                (void)fprintf(output, "  Checkpoint directory: %s\n",
                    view->checkpoint_directory != NULL ? view->checkpoint_directory : "unavailable");
            if (step[0] != '\0') (void)fprintf(output, "\n  Current step: %s\n", step);
            (void)fputs("  Structural validation does not prove restorability.\n", output);
            break;
        case CHRONO_DASHBOARD_SCREEN_TERMINATION:
            (void)fputs("\nPROCESS TERMINATION\nEXACT ORIGINAL PROCESS\n", output);
            if (view->managed_process != NULL)
                render_workflow_process(output, view->managed_process,
                    view->program_name, view->working_directory, view->launch_arguments);
            else (void)fputs("  Original process is not live or not verified.\n", output);
            if (step[0] != '\0') (void)fprintf(output, "\n  Current step: %s\n", step);
            break;
        case CHRONO_DASHBOARD_SCREEN_CHECKPOINT_BROWSER:
            (void)fputs("\nCHECKPOINT BROWSER (this session)\n", output);
            if (view->checkpoint_row_count == 0) {
                (void)fputs("  No completed checkpoints are available.\n", output);
            } else {
                for (size_t i = 0; i < view->checkpoint_row_count; ++i) {
                    const chrono_dashboard_checkpoint_row *row = &view->checkpoint_rows[i];
                    char workload[CHRONO_CHECKPOINT_TEXT_MAX];
                    char executable[CHRONO_CHECKPOINT_TEXT_MAX];
                    char artifact[PATH_MAX];
                    safe_text(workload, sizeof(workload), row->metadata.workload);
                    safe_text(executable, sizeof(executable), row->metadata.executable);
                    (void)snprintf(artifact, sizeof(artifact), "%s/%s",
                        view->checkpoint_directory != NULL ? view->checkpoint_directory : "",
                        row->metadata.id);
                    (void)fprintf(output, "%s%zu. %s | %s | PID=%ld UID=%lu | created=",
                        view->has_selected_checkpoint &&
                            i == view->selected_checkpoint_index ? "> " : "  ", i + 1,
                        workload, executable,
                        (long)row->metadata.original_pid, (unsigned long)row->original_uid);
                    print_created_at(output, row->metadata.created_at);
                    char safe_artifact[PATH_MAX];
                    safe_text(safe_artifact, sizeof(safe_artifact), artifact);
                    (void)fprintf(output, " | saved counter=%llu | artifacts=%s (%s) | recovery=%s\n",
                        row->saved_counter, row->artifacts_valid ? "valid structure" : "invalid",
                        safe_artifact, row->restored ? "restored" : "not restored");
                }
            }
            break;
        case CHRONO_DASHBOARD_SCREEN_RESTORE:
            (void)fputs("\nSELECTED CHECKPOINT — RESTORE MANAGEMENT\n", output);
            if (view->checkpoint != NULL)
                render_workflow_checkpoint(output, view->checkpoint,
                    view->checkpoint_directory, view->checkpoint_artifact_valid, 0,
                    view->checkpoint_row_count > view->selected_checkpoint_index ?
                        view->checkpoint_rows[view->selected_checkpoint_index].saved_counter : 0);
            if (view->checkpoint != NULL &&
                view->checkpoint_row_count > view->selected_checkpoint_index) {
                const chrono_dashboard_checkpoint_row *row =
                    &view->checkpoint_rows[view->selected_checkpoint_index];
                (void)fprintf(output, "  Origin UID:      %lu | executable device=%llu inode=%llu\n",
                    (unsigned long)row->original_uid,
                    (unsigned long long)row->executable_device,
                    (unsigned long long)row->executable_inode);
            }
            if (step[0] != '\0') (void)fprintf(output, "\n  Current step: %s\n", step);
            break;
        case CHRONO_DASHBOARD_SCREEN_VERIFY:
            (void)fputs("\nRESTORED PROCESS VERIFICATION\n", output);
            if (view->restored_process != NULL)
                render_workflow_process(output, view->restored_process,
                    view->program_name, view->working_directory, view->launch_arguments);
            else
                (void)fputs("  No restored process has been independently verified.\n", output);
            if (step[0] != '\0') (void)fprintf(output, "\n  Current step: %s\n", step);
            break;
        case CHRONO_DASHBOARD_SCREEN_REPORT:
            (void)fputs("\nRECOVERY REPORT\nPROGRAM IDENTIFICATION\n", output);
            if (view->managed_process != NULL) {
                char executable[CHRONO_COMMAND_MAX];
                safe_text(executable, sizeof(executable), view->managed_process->executable);
                (void)fprintf(output, "  Program: %s | Executable: %s\n  Original PID: %ld | Restored PID: ",
                    view->program_name != NULL ? view->program_name : "counter_workload",
                    executable, (long)view->managed_process->pid);
                if (view->restored_process != NULL)
                    (void)fprintf(output, "%ld\n", (long)view->restored_process->pid);
                else
                    (void)fputs("not restored\n", output);
            } else (void)fputs("  Program: not launched\n", output);
            (void)fputs("\nCHECKPOINT\n", output);
            if (view->checkpoint != NULL)
                (void)fprintf(output, "  ID: %s | Saved state: counter=%s | Directory: %s/%s\n  Validation: %s\n",
                    view->checkpoint->id,
                    view->counter_status != NULL ? view->counter_status : "not sampled",
                    view->checkpoint_directory != NULL ? view->checkpoint_directory : "unavailable",
                    view->checkpoint->id,
                    view->checkpoint_artifact_valid ? "structural artifacts valid" : "not validated");
            else (void)fputs("  Checkpoint: not created\n", output);
            (void)fputs("\nRECOVERY\n", output);
            (void)fprintf(output, "  Original termination: %s\n  Restore: %s\n  Executable identity: %s\n"
                                  "  Process identity: %s\n  State continuity: %s\n",
                view->managed_status != NULL ? view->managed_status : "not verified",
                view->restore_status != NULL ? view->restore_status : "not attempted",
                view->restored_process != NULL ? "verified" : "not verified",
                view->restored_process != NULL ? "verified" : "not verified",
                view->counter_status != NULL ? view->counter_status : "not verified");
            (void)fprintf(output, "\nFINAL RESULT: %s\n",
                view->recovery_verified ?
                "RECOVERY VERIFIED" : "RECOVERY NOT VERIFIED");
            break;
        case CHRONO_DASHBOARD_SCREEN_MESSAGE:
        case CHRONO_DASHBOARD_SCREEN_DEFAULT:
            (void)fputs("\nSTATUS\n", output);
            break;
    }

    if (message[0] != '\0') (void)fprintf(output, "\n%s\n", message);
    if (view->input_acknowledgement != NULL &&
        view->input_acknowledgement[0] != '\0') {
        char acknowledgement[192];
        safe_text(acknowledgement, sizeof(acknowledgement),
                  view->input_acknowledgement);
        (void)fprintf(output, "\n%s\n", acknowledgement);
    }
    if (view->feedback != NULL && view->feedback[0] != '\0') {
        char feedback[192];
        safe_text(feedback, sizeof(feedback), view->feedback);
        (void)fprintf(output, "\n%s\n", feedback);
    }
    if (view->event_count != 0) {
        size_t first = view->event_count > 3 ? view->event_count - 3 : 0;
        (void)fputs("\nRECENT EVENTS\n", output);
        for (size_t i = first; i < view->event_count; ++i) {
            char timestamp[40], event[192];
            safe_text(timestamp, sizeof(timestamp), view->events[i].timestamp);
            safe_text(event, sizeof(event), view->events[i].message);
            (void)fprintf(output, "  %s  %s\n", timestamp, event);
        }
    }
    if (view->controls != NULL) {
        char controls[192];
        safe_text(controls, sizeof(controls), view->controls);
        (void)fprintf(output, "\n%s\n", controls);
    }
}

static int terminal_write(const char *text, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(STDOUT_FILENO, text + written, length - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        written += (size_t)result;
    }
    return 0;
}

static const char *line_after(const char *line, const char *end, size_t *length)
{
    if (line >= end) return end;
    const char *newline = memchr(line, '\n', (size_t)(end - line));
    if (newline == NULL) {
        *length = (size_t)(end - line);
        return end;
    }
    *length = (size_t)(newline - line);
    return newline + 1;
}

static int write_tty_frame(const char *frame, size_t length)
{
    if (previous_tty_frame == NULL) {
        static const char clear[] = "\033[2J\033[H";
        if (terminal_write(clear, sizeof(clear) - 1) != 0 ||
            terminal_write(frame, length) != 0)
            return -1;
    } else {
        const char *current = frame, *current_end = frame + length;
        const char *previous = previous_tty_frame;
        const char *previous_end = previous_tty_frame + previous_tty_frame_length;
        size_t row = 1;
        while (current < current_end) {
            size_t current_length = 0, previous_length = 0;
            const char *next_current = line_after(current, current_end, &current_length);
            const char *next_previous = line_after(previous, previous_end, &previous_length);
            if (current_length != previous_length ||
                memcmp(current, previous, current_length) != 0) {
                char position[48];
                int count = snprintf(position, sizeof(position), "\033[%zu;1H\033[2K", row);
                if (count < 0 || (size_t)count >= sizeof(position) ||
                    terminal_write(position, (size_t)count) != 0 ||
                    terminal_write(current, current_length) != 0)
                    return -1;
            }
            current = next_current;
            previous = next_previous;
            ++row;
        }
        while (previous < previous_end) {
            size_t ignored = 0;
            previous = line_after(previous, previous_end, &ignored);
            char position[48];
            int count = snprintf(position, sizeof(position), "\033[%zu;1H\033[2K", row++);
            if (count < 0 || (size_t)count >= sizeof(position) ||
                terminal_write(position, (size_t)count) != 0)
                return -1;
        }
        char position[48];
        int count = snprintf(position, sizeof(position), "\033[%zu;1H", row);
        if (count < 0 || (size_t)count >= sizeof(position) ||
            terminal_write(position, (size_t)count) != 0)
            return -1;
    }

    char *next_frame = malloc(length + 1);
    if (next_frame == NULL) {
        free(previous_tty_frame);
        previous_tty_frame = NULL;
        previous_tty_frame_length = 0;
        (void)fprintf(stderr, "chrono: could not cache dashboard frame: %s\n", strerror(errno));
        return 0;
    }
    memcpy(next_frame, frame, length);
    next_frame[length] = '\0';
    free(previous_tty_frame);
    previous_tty_frame = next_frame;
    previous_tty_frame_length = length;
    return 0;
}

static char *clip_tty_frame(const char *frame, size_t length, size_t columns,
                            size_t max_rows, size_t *clipped_length)
{
    size_t lines = 1;
    for (size_t i = 0; i < length; ++i) if (frame[i] == '\n') ++lines;
    if (lines > (SIZE_MAX - length - 1) / (sizeof(ANSI_RESET) - 1)) return NULL;
    size_t capacity = length + lines * (sizeof(ANSI_RESET) - 1) + 1;
    char *result = malloc(capacity);
    if (result == NULL) return NULL;
    size_t used = 0, column = 0, row = 1;
    for (size_t i = 0; i < length;) {
        if (row > max_rows) break;
        unsigned char ch = (unsigned char)frame[i];
        if (ch == '\033' && i + 1 < length && frame[i + 1] == '[') {
            size_t end = i + 2;
            while (end < length && !((unsigned char)frame[end] >= 0x40 &&
                                     (unsigned char)frame[end] <= 0x7e)) ++end;
            if (end < length) {
                if (column < columns) {
                    size_t sequence_length = end + 1 - i;
                    memcpy(result + used, frame + i, sequence_length);
                    used += sequence_length;
                }
                i = end + 1;
                continue;
            }
        }
        if (ch == '\n') {
            memcpy(result + used, ANSI_RESET, sizeof(ANSI_RESET) - 1);
            used += sizeof(ANSI_RESET) - 1;
            result[used++] = '\n';
            column = 0;
            ++row;
            ++i;
            continue;
        }
        if (column < columns) {
            result[used++] = frame[i];
            ++column;
        } else {
            /* Ignore the rest of the physical line; cursor rows stay aligned. */
        }
        ++i;
    }
    result[used] = '\0';
    *clipped_length = used;
    return result;
}

static void render_content(FILE *output, const chrono_dashboard_view *view, int tty_mode)
{
    time_t now = time(NULL);
    struct tm local;
    char stamp[40] = "time unavailable";
    if (localtime_r(&now, &local) != NULL)
        (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    struct utsname system;
    char system_name[160] = "Linux system information unavailable";
    if (uname(&system) == 0) {
        char raw_system[sizeof(system.sysname) + sizeof(system.release) +
                        sizeof(system.machine) + 4];
        (void)snprintf(raw_system, sizeof(raw_system), "%s %s (%s)",
                       system.sysname, system.release, system.machine);
        safe_text(system_name, sizeof(system_name), raw_system);
    }
    if (view->screen != CHRONO_DASHBOARD_SCREEN_DEFAULT) {
        render_workflow_screen(output, view, tty_mode);
        return;
    }
    if (view->process_browser) {
        render_process_browser(output, view, tty_mode, system_name);
        return;
    }
    size_t rows = view->terminal_height != 0 ? view->terminal_height : terminal_rows;
    if (tty_mode && (rows == 0 || rows < 46)) {
        render_compact_dashboard(output, view, tty_mode, rows, system_name);
        return;
    }
    char safe_version[160], safe_status[64], safe_mode[64], safe_supervisor[96];
    safe_text(safe_version, sizeof(safe_version),
              view->criu_version != NULL ? view->criu_version : "version unavailable");
    safe_text(safe_status, sizeof(safe_status),
              view->criu_status != NULL ? view->criu_status : "unknown");
    safe_text(safe_mode, sizeof(safe_mode),
              view->criu_mode != NULL ? view->criu_mode : "mode unavailable");
    safe_text(safe_supervisor, sizeof(safe_supervisor),
              view->supervisor_status != NULL ? view->supervisor_status : "interactive demo; not persistent");

    if (tty_mode) (void)fputs(ANSI_CYAN, output);
    (void)fputs("CHRONOOS  |  PROCESS CONTROL CENTER\n", output);
    if (tty_mode) (void)fputs(ANSI_RESET, output);
    (void)fprintf(output, "Linux: %s | Time: %s\n", system_name, stamp);
    (void)fprintf(output, "Dashboard: ACTIVE | Phase: %s\n", view->phase != NULL ? view->phase : "idle");
    (void)fprintf(output, "Supervisor: %s\n", safe_supervisor);
    (void)fprintf(output, "CRIU: %s | Mode: %s\n", safe_version, safe_mode);
    (void)fputs("SYSTEM STATUS\n", output);
    (void)fprintf(output, "  Preflight: %s | Processes: %zu accessible, %zu skipped | p opens read-only browser\n",
                  safe_status, view->process_count, view->inaccessible_processes);
    if (view->criu_diagnostic != NULL && view->criu_diagnostic[0] != '\0') {
        char diagnostic[512];
        safe_text(diagnostic, sizeof(diagnostic), view->criu_diagnostic);
        (void)fprintf(output, "  CRIU diagnostic: %s\n", diagnostic);
    }

    (void)fputs("\nMANAGED WORKLOAD\n", output);
    if (view->managed_process != NULL) {
        char executable[CHRONO_COMMAND_MAX];
        safe_text(executable, sizeof(executable), view->managed_process->executable);
        (void)fprintf(output, "  PID=%ld UID=%lu STATE=%c START=%llu\n",
                      (long)view->managed_process->pid, (unsigned long)view->managed_process->owner,
                      view->managed_process->state,
                      (unsigned long long)view->managed_process->start_time);
        (void)fprintf(output, "  Executable: %s\n", executable);
        (void)fputs("  Live in-memory counter: ", output);
        if (view->counter_available) {
            if (tty_mode) (void)fputs(ANSI_GREEN, output);
            (void)fprintf(output, "%llu\n", view->counter);
            if (tty_mode) (void)fputs(ANSI_RESET, output);
        } else (void)fputs("unavailable\n", output);
        (void)fputs("  /proc comm (display only): ", output);
        if (view->display_counter_available)
            (void)fprintf(output, "%llu\n", view->display_counter);
        else
            (void)fputs("unavailable\n", output);
        (void)fprintf(output, "  Monitoring: %s\n",
                      view->managed_status != NULL ? view->managed_status : "identity checked");
    } else {
        (void)fprintf(output, "  Status: %s\n",
                      view->managed_status != NULL ? view->managed_status : "not launched");
    }

    (void)fputs("\nCHECKPOINT\n", output);
    int original_running = view->managed_process != NULL && view->checkpoint != NULL &&
        view->managed_process->pid == view->checkpoint->original_pid &&
        view->managed_process->start_time == view->checkpoint->process_start_time;
    if (view->checkpoint != NULL) {
        char safe_id[64], safe_artifacts[PATH_MAX], display_artifacts[PATH_MAX];
        safe_text(safe_id, sizeof(safe_id), view->checkpoint->id);
        if (view->checkpoint_directory != NULL)
            (void)snprintf(safe_artifacts, sizeof(safe_artifacts), "%s/%s",
                           view->checkpoint_directory, view->checkpoint->id);
        else
            (void)snprintf(safe_artifacts, sizeof(safe_artifacts), "unavailable");
        safe_text(display_artifacts, sizeof(display_artifacts), safe_artifacts);
        char metadata_path[PATH_MAX], display_metadata[PATH_MAX];
        if (view->checkpoint_directory != NULL)
            (void)snprintf(metadata_path, sizeof(metadata_path), "%s/%s.meta",
                           view->checkpoint_directory, view->checkpoint->id);
        else
            (void)snprintf(metadata_path, sizeof(metadata_path), "unavailable");
        safe_text(display_metadata, sizeof(display_metadata), metadata_path);
        (void)fprintf(output, "  Status: %s | ID: %s\n",
                      checkpoint_state(view->checkpoint, view->checkpoint_artifact_valid), safe_id);
        (void)fprintf(output, "  Artifact directory: %s\n", display_artifacts);
        (void)fprintf(output, "  Metadata file: %s\n", display_metadata);
        (void)fprintf(output, "  Validation: %s",
                      view->checkpoint_artifact_valid ? "VALIDATED" : "NOT VALIDATED");
        if (view->checkpoint_artifact_valid && view->checkpoint_directory != NULL)
            (void)fprintf(output, " (%llu bytes)",
                          artifact_size(view->checkpoint_directory, view->checkpoint->id));
        (void)fputs(" | Original process: ", output);
        (void)fprintf(output, "%s\n",
                      original_running ? "STILL RUNNING" :
                      (view->restored_process != NULL || strcmp(view->restore_status != NULL ?
                       view->restore_status : "", "NOT ATTEMPTED") == 0 ? "STOPPED/REAPED" : "NOT VERIFIED"));
        (void)fputs("  Created: ", output);
        print_created_at(output, view->checkpoint->created_at);
        (void)fputc('\n', output);
        (void)fputs("  Created: ", output);
        print_created_at(output, view->checkpoint->created_at);
        (void)fputc('\n', output);
    } else {
        (void)fprintf(output, "  Status: not created | Original process: %s\n",
                      view->managed_process != NULL ? "RUNNING" : "not launched");
        (void)fprintf(output, "  Artifact target: %s\n",
                      view->checkpoint_directory != NULL ? view->checkpoint_directory : "unavailable");
    }

    (void)fputs("\nRESTORE\n", output);
    (void)fprintf(output, "  Status: %s\n",
                  view->restore_status != NULL ? view->restore_status : "NOT ATTEMPTED");
    if (view->restored_process != NULL) {
        char executable[CHRONO_COMMAND_MAX];
        safe_text(executable, sizeof(executable), view->restored_process->executable);
        (void)fprintf(output, "  Identity: VERIFIED | PID=%ld START=%llu UID=%lu\n",
                      (long)view->restored_process->pid,
                      (unsigned long long)view->restored_process->start_time,
                      (unsigned long)view->restored_process->owner);
        (void)fprintf(output, "  Executable: %s\n", executable);
        (void)fprintf(output, "  Counter continuity: %s\n",
                      view->counter_status != NULL ? view->counter_status : "checking");
    } else {
        (void)fputs("  Identity: not yet verified | Counter continuity: not yet verified\n", output);
    }

    (void)fputs("\nCOMMAND CENTER\n"
                "  p  Process browser       h  Help\n"
                "  l  Launch workload      c  Create checkpoint\n"
                "  t  Request stop         y  Confirm original termination\n"
                "  r  Restore / refresh    q  Quit / cleanup\n", output);
    if (view->feedback != NULL && view->feedback[0] != '\0')
        (void)fprintf(output, "  %s\n", view->feedback);
    else if (view->controls != NULL)
        (void)fprintf(output, "  Next: %s\n", view->controls);

    (void)fputs("\nEVENT HISTORY (latest events; session only)\n", output);
    size_t history_count = view->event_count < DASHBOARD_HISTORY_LIMIT ?
                           view->event_count : DASHBOARD_HISTORY_LIMIT;
    if (history_count == 0) (void)fputs("  No events yet.\n", output);
    for (size_t i = view->event_count - history_count; i < view->event_count; ++i) {
        const chrono_dashboard_event *event = &view->events[i];
        char safe_timestamp[40], safe_message[192];
        safe_text(safe_timestamp, sizeof(safe_timestamp), event->timestamp);
        safe_text(safe_message, sizeof(safe_message), event->message);
        (void)fprintf(output, "  %s  %s\n", safe_timestamp, safe_message);
    }
    if (view->session_log != NULL)
        (void)fprintf(output, "Diagnostics: %s\n", view->session_log);
    if (tty_mode) (void)fputs(ANSI_RESET, output);
}

int chrono_dashboard_start(int *tty_mode)
{
    if (tty_mode == NULL) return -1;
    *tty_mode = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (!*tty_mode) return 0;
    struct winsize window = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col > 1)
        terminal_columns = (size_t)window.ws_col - 1;
    else
        terminal_columns = 79;
    terminal_rows = window.ws_row;
    free(previous_tty_frame);
    previous_tty_frame = NULL;
    previous_tty_frame_length = 0;
    if (tcgetattr(STDIN_FILENO, &saved_terminal) != 0) return -1;
    struct termios raw = saved_terminal;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_lflag |= ISIG;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return -1;
    terminal_saved = 1;
    last_tty_key = '\0';
    return 0;
}

void chrono_dashboard_stop(int tty_mode)
{
    if (tty_mode && terminal_saved) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
        terminal_saved = 0;
        (void)fputs("\033[0m\n", stdout);
        (void)fflush(stdout);
    }
    if (tty_mode) {
        free(previous_tty_frame);
        previous_tty_frame = NULL;
        previous_tty_frame_length = 0;
        last_tty_key = '\0';
    }
}

int chrono_dashboard_read_key(int tty_mode, char *key)
{
    if (key == NULL) return -1;
    struct pollfd ready = {.fd = STDIN_FILENO, .events = POLLIN | POLLHUP};
    if (tty_mode) {
        for (;;) {
            int polled;
            do { polled = poll(&ready, 1, 1000); } while (polled < 0 && errno == EINTR);
            if (polled == 0) {
                last_tty_key = '\0';
                return 0;
            }
            if (polled < 0) return -1;
            char value;
            ssize_t count;
            do { count = read(STDIN_FILENO, &value, 1); } while (count < 0 && errno == EINTR);
            if (count != 1) return -1;
            char normalized = (char)tolower((unsigned char)value);
            if (normalized == last_tty_key) continue;
            last_tty_key = normalized;
            *key = normalized;
            return 1;
        }
    }
    int polled;
    do { polled = poll(&ready, 1, 1000); } while (polled < 0 && errno == EINTR);
    if (polled == 0) return 0;
    if (polled < 0) return -1;
    char value = '\0';
    int have_value = 0;
    for (;;) {
        char byte;
        ssize_t count;
        do { count = read(STDIN_FILENO, &byte, 1); } while (count < 0 && errno == EINTR);
        if (count == 0) return have_value ? (*key = (char)tolower((unsigned char)value), 1) : -1;
        if (count < 0) return -1;
        if (byte == '\n' || byte == '\r') break;
        if (!have_value && byte != ' ' && byte != '\t') {
            value = byte;
            have_value = 1;
        }
    }
    if (!have_value) return 0;
    *key = (char)tolower((unsigned char)value);
    return 1;
}

int chrono_dashboard_read_input_char(int tty_mode, char *key)
{
    if (key == NULL) return -1;
    if (!tty_mode) return chrono_dashboard_read_key(0, key);
    for (;;) {
        ssize_t length = read(STDIN_FILENO, key, 1);
        if (length == 1) return 1;
        if (length == 0) return -1;
        if (errno == EINTR) return -1;
        return -1;
    }
}

int chrono_dashboard_read_counter(pid_t pid, unsigned long long *counter)
{
    if (pid <= 1 || counter == NULL) return -1;
    char path[64], text[32];
    (void)snprintf(path, sizeof(path), "/proc/%ld/comm", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    ssize_t length;
    do { length = read(fd, text, sizeof(text) - 1); } while (length < 0 && errno == EINTR);
    int saved = errno;
    if (close(fd) != 0 && length >= 0) return -1;
    errno = saved;
    if (length < 4 || length >= (ssize_t)sizeof(text) - 1 || text[0] != 'c' || text[1] != ':') return -1;
    text[length] = '\0';
    char *endline = strpbrk(text, "\r\n");
    if (endline != NULL) *endline = '\0';
    const char *digits = text + 2;
    if (*digits == '\0') return -1;
    for (const char *p = digits; *p != '\0'; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(digits, &end, 10);
    if (errno != 0 || end == digits || *end != '\0') return -1;
    *counter = value;
    return 0;
}

void chrono_dashboard_render(const chrono_dashboard_view *view, int tty_mode)
{
    if (view == NULL) return;
    if (!tty_mode) {
        render_content(stdout, view, 0);
        (void)fflush(stdout);
        return;
    }

    struct winsize window = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0) {
        if (window.ws_col > 1) terminal_columns = (size_t)window.ws_col - 1;
        if (window.ws_row > 0) terminal_rows = window.ws_row;
    }
    size_t columns = view->terminal_width > 1 ? view->terminal_width - 1 :
                     terminal_columns > 1 ? terminal_columns : 79;
    size_t rows = view->terminal_height != 0 ? view->terminal_height : terminal_rows;
    if (rows == 0) rows = 24;
    if (previous_terminal_columns != columns || previous_terminal_rows != rows) {
        free(previous_tty_frame);
        previous_tty_frame = NULL;
        previous_tty_frame_length = 0;
        previous_terminal_columns = columns;
        previous_terminal_rows = rows;
    }
    chrono_dashboard_view sized_view = *view;
    sized_view.terminal_width = columns + 1;
    sized_view.terminal_height = rows;

    char *frame = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&frame, &length);
    if (stream == NULL) {
        (void)fprintf(stderr, "chrono: could not allocate dashboard frame: %s\n", strerror(errno));
        return;
    }
    render_content(stream, &sized_view, 1);
    if (fclose(stream) != 0) {
        (void)fprintf(stderr, "chrono: could not finish dashboard frame: %s\n", strerror(errno));
        free(frame);
        return;
    }
    size_t clipped_length = 0;
    char *clipped = clip_tty_frame(frame, length, columns, rows > 1 ? rows - 1 : 1,
                                   &clipped_length);
    if (clipped == NULL) {
        (void)fprintf(stderr, "chrono: could not constrain dashboard frame to terminal width\n");
        free(frame);
        return;
    }
    if (write_tty_frame(clipped, clipped_length) != 0)
        (void)fprintf(stderr, "chrono: dashboard terminal write failed: %s\n", strerror(errno));
    free(clipped);
    free(frame);
}