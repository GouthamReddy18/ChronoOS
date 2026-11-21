#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "chrono/demo.h"
#include "chrono/process.h"
#include "chrono/checkpoint.h"
#include "chrono/checkpoint_criu.h"
#include "demo_internal.h"
#include "demo_dashboard.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DASHBOARD_EVENT_LIMIT 64

static volatile sig_atomic_t manual_interrupted;
static int dashboard_tty_mode;
static char dashboard_feedback[256];
static uint64_t dashboard_counter_endpoint_start;
static const char *dashboard_criu_status = "NOT CHECKED";
static char dashboard_criu_diagnostic[512];
static char dashboard_criu_version[160] = "version unavailable";
static chrono_dashboard_event dashboard_events[DASHBOARD_EVENT_LIMIT];
static char dashboard_event_times[DASHBOARD_EVENT_LIMIT][32];
static char dashboard_event_messages[DASHBOARD_EVENT_LIMIT][192];
static size_t dashboard_event_count;
static chrono_dashboard_process *dashboard_processes;
static size_t dashboard_process_count;
static size_t dashboard_inaccessible_processes;
static size_t dashboard_process_page;
static int dashboard_process_browser;
static int dashboard_process_help;
static chrono_dashboard_process dashboard_monitored_process;
static int dashboard_has_monitored_process;
static const char *dashboard_monitored_status;
static int dashboard_process_view_active;

static void manual_signal(int signo)
{
    (void)signo;
    manual_interrupted = 1;
}

static void log_event(FILE *log, const char *format, ...)
{
    char text[1024], stamp[40];
    struct timespec now;
    struct tm local;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0 && localtime_r(&now.tv_sec, &local) != NULL)
        (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    else (void)snprintf(stamp, sizeof(stamp), "time-unavailable");
    va_list args;
    va_start(args, format);
    (void)vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (dashboard_event_count == DASHBOARD_EVENT_LIMIT) {
        memmove(dashboard_events, dashboard_events + 1,
                (DASHBOARD_EVENT_LIMIT - 1) * sizeof(dashboard_events[0]));
        memmove(dashboard_event_times, dashboard_event_times + 1,
                (DASHBOARD_EVENT_LIMIT - 1) * sizeof(dashboard_event_times[0]));
        memmove(dashboard_event_messages, dashboard_event_messages + 1,
                (DASHBOARD_EVENT_LIMIT - 1) * sizeof(dashboard_event_messages[0]));
        --dashboard_event_count;
    }
    size_t event_index = dashboard_event_count++;
    size_t stamp_length = strnlen(stamp, sizeof(dashboard_event_times[event_index]) - 1);
    memcpy(dashboard_event_times[event_index], stamp, stamp_length);
    dashboard_event_times[event_index][stamp_length] = '\0';
    size_t message_length = strnlen(text, sizeof(dashboard_event_messages[event_index]) - 1);
    memcpy(dashboard_event_messages[event_index], text, message_length);
    dashboard_event_messages[event_index][message_length] = '\0';
    dashboard_events[event_index].timestamp = dashboard_event_times[event_index];
    dashboard_events[event_index].message = dashboard_event_messages[event_index];
    if (!dashboard_tty_mode) {
        (void)printf("[%s] %s\n", stamp, text);
        (void)fflush(stdout);
    }
    if (log != NULL) { (void)fprintf(log, "[%s] %s\n", stamp, text); (void)fflush(log); }
}

static void dashboard_set_feedback(const char *message)
{
    (void)snprintf(dashboard_feedback, sizeof(dashboard_feedback), "%s",
                   message != NULL ? message : "");
}

typedef struct { const char *name; char *value; int was_set; } env_value;

static int env_save(env_value *item, const char *name)
{
    const char *value = getenv(name);
    item->name = name;
    item->was_set = value != NULL;
    item->value = value == NULL ? NULL : strdup(value);
    return value == NULL || item->value != NULL ? 0 : -1;
}

static int env_restore(env_value *item)
{
    int rc = item->was_set ? setenv(item->name, item->value, 1) : unsetenv(item->name);
    free(item->value);
    item->value = NULL;
    return rc;
}

static FILE *private_log(const char *directory, char *path, size_t size)
{
    int n = snprintf(path, size, "%s/demo-manual.log", directory);
    if (n < 0 || (size_t)n >= size) return NULL;
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        st.st_nlink != 1 || fchmod(fd, 0600) != 0) { close(fd); return NULL; }
    FILE *file = fdopen(fd, "a");
    if (file == NULL) close(fd);
    return file;
}

typedef struct { int out, err; off_t offset; int fd; } capture;

static int capture_begin(int fd, capture *c)
{
    (void)fflush(stdout); (void)fflush(stderr);
    c->out = dup(STDOUT_FILENO); c->err = dup(STDERR_FILENO); c->fd = fd;
    c->offset = lseek(fd, 0, SEEK_END);
    if (c->out < 0 || c->err < 0 || c->offset < 0) {
        if (c->out >= 0) close(c->out);
        if (c->err >= 0) close(c->err);
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        (void)dup2(c->out, STDOUT_FILENO); (void)dup2(c->err, STDERR_FILENO);
        close(c->out); close(c->err); return -1;
    }
    return 0;
}

static int capture_end(capture *c)
{
    (void)fflush(stdout); (void)fflush(stderr);
    int failed = dup2(c->out, STDOUT_FILENO) != STDOUT_FILENO;
    if (dup2(c->err, STDERR_FILENO) != STDERR_FILENO) failed = 1;
    close(c->out); close(c->err);
    if (dashboard_tty_mode) {
        if (lseek(c->fd, 0, SEEK_END) < 0) return -1;
        return failed ? -1 : 0;
    }
    if (lseek(c->fd, c->offset, SEEK_SET) < 0) return -1;
    char buffer[1024];
    for (;;) {
        ssize_t n = read(c->fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) break;
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(STDOUT_FILENO, buffer + off, (size_t)n - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return -1;
            off += (size_t)w;
        }
    }
    (void)lseek(c->fd, 0, SEEK_END);
    return failed ? -1 : 0;
}

static int counter_read(pid_t pid, uint64_t start_time, unsigned long long *counter)
{
    return chrono_managed_counter_query(pid, start_time, dashboard_counter_endpoint_start,
                                        counter, NULL, 0);
}

static int version_read(const char *path, char *version, size_t size)
{
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0) return -1;
    pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); return -1; }
    if (child == 0) {
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0) _exit(126);
        close(pipes[1]);
        int nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nullfd >= 0) { (void)dup2(nullfd, STDERR_FILENO); close(nullfd); }
        char *args[] = {(char *)path, (char *)"--version", NULL};
        execv(path, args); _exit(127);
    }
    close(pipes[1]); size_t used = 0; int eof = 0, status = 0;
    struct timespec deadline; (void)clock_gettime(CLOCK_MONOTONIC, &deadline); deadline.tv_sec += 3;
    while (!eof) {
        struct timespec now; (void)clock_gettime(CLOCK_MONOTONIC, &now);
        int ms = (int)((deadline.tv_sec - now.tv_sec) * 1000 + (deadline.tv_nsec - now.tv_nsec) / 1000000);
        if (ms <= 0) break;
        struct pollfd pfd = {.fd = pipes[0], .events = POLLIN | POLLHUP};
        int r = poll(&pfd, 1, ms);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        for (;;) {
            char buf[128]; ssize_t n = read(pipes[0], buf, sizeof(buf));
            if (n > 0) { size_t copy = (size_t)n; if (copy > size - 1 - used) copy = size - 1 - used;
                memcpy(version + used, buf, copy); used += copy; continue; }
            if (n == 0) eof = 1;
            else if (errno != EAGAIN && errno != EINTR) eof = 1;
            break;
        }
    }
    close(pipes[0]); version[used] = '\0';
    pid_t waited;
    do { waited = waitpid(child, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
    if (waited == 0) {
        (void)kill(child, SIGTERM);
        for (int i = 0; i < 10; ++i) {
            do { waited = waitpid(child, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
            if (waited != 0) break;
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
            while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
        }
        if (waited == 0) {
            (void)kill(child, SIGKILL); /* This is only the version-query child created above. */
            do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        }
    }
    if (!eof || waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || used == 0) {
        version[0] = '\0'; return -1;
    }
    while (used > 0 && (version[used - 1] == '\n' || version[used - 1] == '\r')) version[--used] = '\0';
    return 0;
}

/* Read one complete line without blocking indefinitely; signals and EOF abort safely. */
static int wait_counter(pid_t pid, uint64_t start_time, unsigned long long *value, int seconds)
{
    for (int i = 0; i <= seconds * 4 && !manual_interrupted; ++i) {
        if (counter_read(pid, start_time, value) == 0) return 0;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !manual_interrupted) {}
    }
    return -1;
}

static int wait_counter_above(pid_t pid, uint64_t start_time, unsigned long long baseline,
                              unsigned long long *value, int seconds)
{
    for (int attempt = 0; attempt <= seconds * 4 && !manual_interrupted; ++attempt) {
        if (counter_read(pid, start_time, value) == 0 && *value > baseline) return 0;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !manual_interrupted) {}
    }
    return -1;
}

static int original_gone(pid_t pid, uint64_t start)
{
    chrono_process_info info; char error[192];
    if (chrono_process_inspect(pid, &info, error, sizeof(error)) != 0) {
        char path[64]; (void)snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
        return access(path, F_OK) != 0 && errno == ENOENT;
    }
    return info.start_time != start || info.state == 'Z';
}

static int wait_original_stop(pid_t pid, uint64_t start, FILE *log)
{
    for (int i = 0; i < 40; ++i) {
        if (original_gone(pid, start)) {
            int status; pid_t waited;
            do { waited = waitpid(pid, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
            if (waited == pid || (waited < 0 && errno == ECHILD)) return 0;
            if (waited < 0) return -1;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    log_event(log, "Original process termination could not be verified; restore is unsafe."); return -1;
}

static int identity_matches(const chrono_process_info *got, const chrono_process_info *expected)
{
    return got->pid > 1 && got->start_time != 0 && got->owner == getuid() && got->state != 'Z' &&
           got->executable_device == expected->executable_device &&
           got->executable_inode == expected->executable_inode &&
           strcmp(got->command, expected->command) == 0;
}

static int stop_verified(const chrono_process_info *info, FILE *log)
{
    char error[256];
    if (chrono_process_stop_verified(info->pid, info->start_time, info->executable_device,
                                     info->executable_inode, info->command,
                                     error, sizeof(error)) != 0) {
        log_event(log, "Cleanup refused to signal PID %ld: %s", (long)info->pid, error); return -1;
    }
    for (int i = 0; i < 40; ++i) {
        chrono_process_info current;
        if (chrono_process_inspect(info->pid, &current, error, sizeof(error)) != 0 ||
            current.start_time != info->start_time || current.state == 'Z') return 0;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    log_event(log, "Verified PID %ld did not stop after cleanup request.", (long)info->pid); return -1;
}

static const char *criu_path(void)
{
    const char *path = getenv("CHRONO_CRIU_PATH");
    return path != NULL && path[0] != '\0' ? path : "/usr/sbin/criu";
}

static const char *criu_status_label(int preflight_passed)
{
    const char *configured = getenv("CHRONO_CRIU_PATH");
    if (configured != NULL && configured[0] != '\0')
        return preflight_passed ? "custom backend; identity unverified" : "custom backend; preflight blocked";
    return preflight_passed ? "system CRIU preflight passed" : "system CRIU preflight blocked";
}

static const char *criu_mode_label(void)
{
    const char *mode = getenv("CHRONO_CRIU_UNPRIVILEGED");
    if (mode == NULL) return geteuid() != 0 ? "unprivileged (automatic)" : "default";
    if (strcmp(mode, "1") == 0) return "unprivileged (--unprivileged)";
    if (strcmp(mode, "0") == 0) return "default";
    return "invalid configuration";
}

static void dashboard_render(const char *phase, const char *controls,
                             pid_t managed_pid, uint64_t managed_start,
                             const char *managed_status,
                             const chrono_checkpoint_metadata *checkpoint,
                             const char *checkpoint_directory,
                             int checkpoint_artifact_valid,
                             const chrono_process_info *restored,
                             const char *restore_status, const char *counter_status,
                             const char *session_log)
{
    chrono_process_info managed;
    const chrono_process_info *managed_pointer = NULL;
    unsigned long long counter = 0;
    unsigned long long display_counter = 0;
    int counter_available = 0;
    int display_counter_available = 0;
    if (managed_pid > 1) {
        char error[128];
        if (chrono_process_inspect(managed_pid, &managed, error, sizeof(error)) == 0 &&
            (managed_start == 0 || managed.start_time == managed_start)) {
            managed_pointer = &managed;
            counter_available = counter_read(managed_pid, managed.start_time, &counter) == 0;
            display_counter_available = chrono_dashboard_read_counter(managed_pid, &display_counter) == 0;
        }
    }
    chrono_dashboard_view view = {
        .phase = phase,
        .criu_path = criu_path(),
        .criu_version = dashboard_criu_version,
        .criu_status = dashboard_criu_status,
        .criu_mode = criu_mode_label(),
        .supervisor_status = "interactive demo session; not persistent",
        .feedback = dashboard_feedback,
        .criu_diagnostic = dashboard_criu_diagnostic,
        .controls = controls,
        .checkpoint_directory = checkpoint_directory,
        .restore_status = restore_status,
        .counter_status = counter_status,
        .session_log = session_log,
        .managed_status = managed_status,
        .processes = dashboard_processes,
        .process_count = dashboard_process_count,
        .process_page = dashboard_process_page,
        .process_page_size = 8,
        .inaccessible_processes = dashboard_inaccessible_processes,
        .monitored_process = dashboard_has_monitored_process ? &dashboard_monitored_process : NULL,
        .monitored_status = dashboard_monitored_status,
        .process_browser = dashboard_process_browser,
        .process_help = dashboard_process_help,
        .managed_process = managed_pointer,
        .counter_available = counter_available,
        .counter = counter,
        .display_counter_available = display_counter_available,
        .display_counter = display_counter,
        .checkpoint = checkpoint,
        .checkpoint_artifact_valid = checkpoint_artifact_valid,
        .restored_process = restored,
        .events = dashboard_events,
        .event_count = dashboard_event_count
    };
    chrono_dashboard_render(&view, dashboard_tty_mode);
}

static int refresh_dashboard_processes(FILE *log)
{
    chrono_dashboard_process *fresh = NULL;
    size_t count = 0, inaccessible = 0;
    if (chrono_dashboard_process_snapshot(&fresh, &count, &inaccessible) != 0) {
        int saved = errno;
        log_event(log, "FAIL read-only /proc process discovery: %s",
                  saved != 0 ? strerror(saved) : "could not allocate or read the process snapshot");
        return -1;
    }
    chrono_dashboard_process_snapshot_free(dashboard_processes);
    dashboard_processes = fresh;
    dashboard_process_count = count;
    dashboard_inaccessible_processes = inaccessible;
    size_t pages = count == 0 ? 1 : (count + 7) / 8;
    if (dashboard_process_page >= pages) dashboard_process_page = pages - 1;
    log_event(log, "Read-only process snapshot refreshed: %zu accessible, %zu skipped.",
              count, inaccessible);
    return 0;
}

static int dashboard_next_key(const char *phase, const char *controls,
                              pid_t managed_pid, uint64_t managed_start,
                              const char *managed_status,
                              const chrono_checkpoint_metadata *checkpoint,
                              const char *checkpoint_directory,
                              int checkpoint_artifact_valid,
                              const chrono_process_info *restored,
                              const char *restore_status, const char *counter_status,
                              const char *session_log);

static int dashboard_process_view(FILE *log,
                                  pid_t managed_pid, uint64_t managed_start,
                                  const char *managed_status,
                                  const chrono_checkpoint_metadata *checkpoint,
                                  const char *checkpoint_directory,
                                  int checkpoint_artifact_valid,
                                  const chrono_process_info *restored,
                                  const char *restore_status, const char *counter_status,
                                  const char *session_log, int show_help)
{
    dashboard_process_view_active = 1;
    dashboard_process_browser = 1;
    dashboard_process_help = show_help;
    dashboard_process_page = 0;
    dashboard_has_monitored_process = 0;
    int result = 0;
    for (;;) {
        char phase[48];
        const char *controls;
        if (dashboard_process_help) {
            (void)snprintf(phase, sizeof(phase), "KEYBOARD HELP");
            controls = "[b]ack to browser  [x]close";
        } else if (dashboard_has_monitored_process) {
            (void)snprintf(phase, sizeof(phase), "READ-ONLY PROCESS MONITOR");
            controls = "[r]efresh identity  [b]ack to process list  [x]close browser";
        } else {
            (void)snprintf(phase, sizeof(phase), "PROCESS BROWSER");
            controls = "[1-8] monitor row  [n]ext  [b]ack page  [r]efresh  [h]elp  [x]close";
        }
        int key = dashboard_next_key(phase, controls, managed_pid, managed_start,
                                     managed_status, checkpoint, checkpoint_directory,
                                     checkpoint_artifact_valid, restored, restore_status,
                                     counter_status, session_log);
        if (key < 0 || key == 'x' || key == 'q') break;
        if (dashboard_process_help) {
            if (key == 'b' || key == 'h') dashboard_process_help = 0;
            continue;
        }
        if (dashboard_has_monitored_process) {
            if (key == 'b') {
                dashboard_has_monitored_process = 0;
                dashboard_monitored_status = NULL;
            } else if (key == 'r') {
                char error[160];
                if (chrono_dashboard_process_refresh(dashboard_monitored_process.pid,
                        dashboard_monitored_process.start_time, &dashboard_monitored_process,
                        error, sizeof(error)) == 0) {
                    dashboard_monitored_status = "identity verified; read-only monitoring";
                    log_event(log, "Read-only monitor refreshed PID=%ld STARTTIME=%llu.",
                              (long)dashboard_monitored_process.pid,
                              (unsigned long long)dashboard_monitored_process.start_time);
                } else {
                    dashboard_monitored_status = "STALE / NOT ACCESSIBLE; no action taken";
                    log_event(log, "Read-only monitor could not revalidate PID=%ld: %s",
                              (long)dashboard_monitored_process.pid, error);
                }
            }
            continue;
        }
        if (key == 'h') {
            dashboard_process_help = 1;
        } else if (key == 'r') {
            (void)refresh_dashboard_processes(log);
        } else if (key == 'n') {
            size_t pages = dashboard_process_count == 0 ? 1 : (dashboard_process_count + 7) / 8;
            if (dashboard_process_page + 1 < pages) ++dashboard_process_page;
        } else if (key == 'b') {
            if (dashboard_process_page > 0) --dashboard_process_page;
        } else if (key >= '1' && key <= '8') {
            size_t index = dashboard_process_page * 8 + (size_t)(key - '1');
            if (index < dashboard_process_count) {
                dashboard_monitored_process = dashboard_processes[index];
                dashboard_has_monitored_process = 1;
                dashboard_monitored_status = "identity checked; read-only monitoring";
                log_event(log, "Selected PID=%ld STARTTIME=%llu for read-only monitoring; checkpoint eligibility not evaluated.",
                          (long)dashboard_monitored_process.pid,
                          (unsigned long long)dashboard_monitored_process.start_time);
            }
        }
    }
    dashboard_process_help = 0;
    dashboard_process_browser = 0;
    dashboard_has_monitored_process = 0;
    dashboard_monitored_status = NULL;
    dashboard_process_view_active = 0;
    return result;
}

static int dashboard_next_key(const char *phase, const char *controls,
                              pid_t managed_pid, uint64_t managed_start,
                              const char *managed_status,
                              const chrono_checkpoint_metadata *checkpoint,
                              const char *checkpoint_directory,
                              int checkpoint_artifact_valid,
                              const chrono_process_info *restored,
                              const char *restore_status, const char *counter_status,
                              const char *session_log)
{
    while (!manual_interrupted) {
        dashboard_render(phase, controls, managed_pid, managed_start, managed_status, checkpoint,
                         checkpoint_directory, checkpoint_artifact_valid, restored,
                         restore_status, counter_status, session_log);
        char key = '\0';
        int result = chrono_dashboard_read_key(dashboard_tty_mode, &key);
        if (result > 0) {
            char feedback[sizeof(dashboard_feedback)];
            if (chrono_dashboard_input_feedback(key, feedback, sizeof(feedback)) == 0) {
                dashboard_set_feedback(feedback);
                dashboard_render(phase, controls, managed_pid, managed_start, managed_status,
                                 checkpoint, checkpoint_directory, checkpoint_artifact_valid,
                                 restored, restore_status, counter_status, session_log);
            }
            if (!dashboard_process_view_active && (key == 'p' || key == 'h')) {
                (void)dashboard_process_view(NULL, managed_pid, managed_start,
                    managed_status, checkpoint, checkpoint_directory,
                    checkpoint_artifact_valid, restored, restore_status,
                    counter_status, session_log, key == 'h');
                continue;
            }
            return (unsigned char)key;
        }
        if (result < 0) return -1;
    }
    return -1;
}

static void print_summary(const chrono_demo_result *r)
{
    chrono_demo_print_summary(stdout, r, 0);
}

static void print_final_history(int complete)
{
    (void)printf("\nCHRONOOS DEMONSTRATION %s\n",
                 complete ? "COMPLETE" : "INCOMPLETE");
    (void)puts("FINAL RECOVERY HISTORY (this session only; full diagnostic log retained):");
    for (size_t i = 0; i < dashboard_event_count; ++i) {
        (void)printf("  %s ", dashboard_events[i].timestamp);
        const unsigned char *p = (const unsigned char *)dashboard_events[i].message;
        for (; *p != '\0'; ++p)
            (void)putchar(*p >= 0x20 && *p != 0x7f ? *p : '?');
        (void)putchar('\n');
    }
}

typedef struct {
    FILE *log;
    char *checkpoint_directory;
    char *log_path;
    chrono_process_info original;
    chrono_process_info original_identity;
    chrono_process_info restored;
    pid_t original_pid;
    uint64_t original_start;
    pid_t restored_pid;
    unsigned long long checkpoint_counter;
    unsigned long long resumed_counter;
    char working_directory[PATH_MAX];
    char launch_arguments[128];
    char last_input[128];
    chrono_dashboard_checkpoint_row checkpoints[16];
    chrono_dashboard_checkpoint_row previous_checkpoints[16];
    size_t checkpoint_count;
    size_t previous_checkpoint_count;
    size_t selected_checkpoint;
    int has_original;
    int original_stopped;
    int has_selected_checkpoint;
    int has_restored;
    int restore_attempted;
    int restore_failed;
    int restore_verified;
    int restore_ambiguous;
    int checkpoint_passed;
    int termination_passed;
    int counter_continuity_passed;
    int exited;
} dashboard_menu_session;

static int dashboard_menu_read_working_directory(pid_t pid, uint64_t start_time,
                                                  char *directory, size_t size)
{
    char path[64];
    chrono_process_info before, after;
    if (size == 0 || chrono_process_inspect(pid, &before, NULL, 0) != 0 ||
        before.start_time != start_time) return -1;
    int n = snprintf(path, sizeof(path), "/proc/%ld/cwd", (long)pid);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    ssize_t length = readlink(path, directory, size - 1);
    if (length < 0 || (size_t)length >= size - 1 ||
        chrono_process_inspect(pid, &after, NULL, 0) != 0 ||
        after.start_time != start_time ||
        before.executable_device != after.executable_device ||
        before.executable_inode != after.executable_inode) {
        directory[0] = '\0';
        return -1;
    }
    directory[length] = '\0';
    return 0;
}

static int dashboard_menu_collect_checkpoint(const chrono_checkpoint_metadata *metadata,
                                             void *opaque)
{
    dashboard_menu_session *session = opaque;
    if (session->checkpoint_count >= sizeof(session->checkpoints) /
                                    sizeof(session->checkpoints[0]))
        return -1;
    chrono_dashboard_checkpoint_row *row =
        &session->checkpoints[session->checkpoint_count++];
    unsigned long long saved_counter = 0;
    uid_t original_uid = 0;
    dev_t executable_device = 0;
    ino_t executable_inode = 0;
    int was_restored = 0;
    for (size_t i = 0; i < session->previous_checkpoint_count; ++i) {
        const chrono_dashboard_checkpoint_row *previous = &session->previous_checkpoints[i];
        if (strcmp(previous->metadata.id, metadata->id) == 0) {
            saved_counter = previous->saved_counter;
            original_uid = previous->original_uid;
            executable_device = previous->executable_device;
            executable_inode = previous->executable_inode;
            was_restored = previous->restored;
            break;
        }
    }
    memset(row, 0, sizeof(*row));
    row->metadata = *metadata;
    row->artifacts_valid =
        chrono_checkpoint_artifact_valid(session->checkpoint_directory, metadata->id);
    row->restored = was_restored || (session->restore_verified &&
        session->has_selected_checkpoint &&
        strcmp(session->checkpoints[session->selected_checkpoint].metadata.id,
               metadata->id) == 0);
    row->saved_counter = saved_counter;
    row->original_uid = original_uid;
    row->executable_device = executable_device;
    row->executable_inode = executable_inode;
    return 0;
}

static int dashboard_menu_refresh_checkpoints(dashboard_menu_session *session,
                                              char *error, size_t error_size)
{
    session->previous_checkpoint_count = session->checkpoint_count;
    memcpy(session->previous_checkpoints, session->checkpoints,
           session->previous_checkpoint_count * sizeof(session->checkpoints[0]));
    session->checkpoint_count = 0;
    if (chrono_checkpoint_list(session->checkpoint_directory,
            dashboard_menu_collect_checkpoint, session, error, error_size) != 0) {
        session->previous_checkpoint_count = 0;
        return -1;
    }
    session->previous_checkpoint_count = 0;
    return 0;
}

static void dashboard_menu_render(dashboard_menu_session *session,
                                  chrono_dashboard_screen screen,
                                  const char *phase, const char *controls,
                                  const char *message, const char *step,
                                  const char *selection)
{
    chrono_process_info current_original, current_restored;
    const chrono_process_info *original = NULL, *restored = NULL;
    unsigned long long counter = 0;
    char counter_status[96];
    int counter_available = 0;
    if (session->counter_continuity_passed)
        (void)snprintf(counter_status, sizeof(counter_status),
                       "resumed beyond saved value %llu; latest %llu",
                       session->checkpoint_counter, session->resumed_counter);
    else if (session->checkpoint_passed)
        (void)snprintf(counter_status, sizeof(counter_status),
                       "saved in-memory counter %llu",
                       session->checkpoint_counter);
    else
        (void)snprintf(counter_status, sizeof(counter_status), "not sampled");
    if (session->has_original) {
        if (!session->original_stopped &&
            chrono_process_inspect(session->original_pid, &current_original, NULL, 0) == 0 &&
            current_original.start_time == session->original_start &&
            identity_matches(&current_original, &session->original_identity))
            original = &current_original;
        else
            original = &session->original;
        if (!session->original_stopped &&
            counter_read(session->original_pid, session->original_start, &counter) == 0)
            counter_available = 1;
    }
    if (session->has_restored &&
        chrono_process_inspect(session->restored_pid, &current_restored, NULL, 0) == 0 &&
        current_restored.start_time == session->restored.start_time &&
        identity_matches(&current_restored, &session->original_identity))
        restored = &current_restored;

    chrono_dashboard_view view = {
        .screen = screen,
        .phase = phase,
        .program_name = "counter_workload",
        .working_directory = session->working_directory,
        .launch_arguments = session->has_original ? session->launch_arguments : NULL,
        .operation_step = step,
        .screen_message = message,
        .selection = selection,
        .criu_path = criu_path(),
        .criu_version = dashboard_criu_version,
        .criu_status = dashboard_criu_status,
        .criu_mode = criu_mode_label(),
        .supervisor_status = "interactive managed-counter session",
        .feedback = dashboard_feedback,
        .input_acknowledgement = session->last_input,
        .controls = controls,
        .checkpoint_directory = session->checkpoint_directory,
        .restore_status = session->restore_verified ? "VERIFIED" :
            session->restore_attempted ? (session->restore_failed ?
                "FAILED / NOT VERIFIED" : "IN PROGRESS") : "NOT ATTEMPTED",
        .counter_status = counter_status,
        .session_log = session->log_path,
        .managed_status = !session->has_original ? "not launched" :
            session->original_stopped ? "TERMINATED AND REAPED" :
            original == NULL ? "identity not currently verified" : "RUNNING",
        .processes = dashboard_processes,
        .process_count = dashboard_process_count,
        .process_page = dashboard_process_page,
        .process_page_size = 8,
        .inaccessible_processes = dashboard_inaccessible_processes,
        .managed_process = original,
        .counter_available = counter_available,
        .counter = counter,
        .checkpoint = session->has_selected_checkpoint ?
            &session->checkpoints[session->selected_checkpoint].metadata : NULL,
        .checkpoint_artifact_valid = session->has_selected_checkpoint &&
            session->checkpoints[session->selected_checkpoint].artifacts_valid,
        .restored_process = restored,
        .events = dashboard_events,
        .event_count = dashboard_event_count,
        .checkpoint_rows = session->checkpoints,
        .checkpoint_row_count = session->checkpoint_count,
        .selected_checkpoint_index = session->selected_checkpoint,
        .has_selected_checkpoint = session->has_selected_checkpoint,
        .recovery_verified = session->restore_verified &&
            session->counter_continuity_passed && session->termination_passed
    };
    chrono_dashboard_render(&view, dashboard_tty_mode);
}

static int dashboard_menu_parse_choice(const char *text, int minimum, int maximum)
{
    if (text == NULL || text[0] == '\0') return -1;
    unsigned value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return -1;
        unsigned digit = (unsigned)(*p - '0');
        if (value > (UINT_MAX - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    return value >= (unsigned)minimum && value <= (unsigned)maximum ? (int)value : -1;
}

static int dashboard_menu_read_line(char *line, size_t size)
{
    if (line == NULL || size < 2) return -1;
    size_t used = 0;
    int overflow = 0;
    for (;;) {
        char byte;
        ssize_t length = read(STDIN_FILENO, &byte, 1);
        if (length < 0 && errno == EINTR) {
            if (manual_interrupted) return -1;
            continue;
        }
        if (length <= 0) {
            if (length == 0 && used != 0 && !overflow) {
                line[used] = '\0';
                return 1;
            }
            return -1;
        }
        if (byte == '\n') break;
        if (byte == '\r') continue;
        if (used + 1 < size) line[used++] = byte;
        else overflow = 1;
    }
    line[used] = '\0';
    return overflow ? -1 : 1;
}

static int dashboard_menu_read_choice(dashboard_menu_session *session,
                                      chrono_dashboard_screen screen,
                                      const char *phase, const char *controls,
                                      const char *message, const char *step,
                                      int minimum, int maximum)
{
    char selection[16] = {0};
    size_t used = 0;
    if (!dashboard_tty_mode) {
        char line[64];
        dashboard_menu_render(session, screen, phase, controls, message, step, "");
        if (dashboard_menu_read_line(line, sizeof(line)) != 1) return -1;
        int choice = dashboard_menu_parse_choice(line, minimum, maximum);
        if (choice >= 0) dashboard_menu_ack_choice(session, line);
        return choice;
    }
    for (;;) {
        dashboard_menu_render(session, screen, phase, controls, message, step, selection);
        char key;
        int result = chrono_dashboard_read_key(1, &key);
        if (result == 0) continue;
        if (result < 0) return -1;
        if (key == '\r' || key == '\n') {
            int choice = dashboard_menu_parse_choice(selection, minimum, maximum);
            if (choice >= 0) {
                dashboard_menu_ack_choice(session, selection);
                return choice;
            }
            dashboard_set_feedback("[INPUT] Enter a listed menu number.");
            selection[0] = '\0';
            used = 0;
        } else if ((key == 127 || key == '\b') && used != 0) {
            selection[--used] = '\0';
        } else if (key >= '0' && key <= '9' && used + 1 < sizeof(selection)) {
            selection[used++] = key;
            selection[used] = '\0';
        }
    }
}

static int dashboard_menu_read_key(dashboard_menu_session *session,
                                   chrono_dashboard_screen screen,
                                   const char *phase, const char *controls,
                                   const char *message, const char *step)
{
    dashboard_menu_render(session, screen, phase, controls, message, step, NULL);
    char key;
    int result;
    do {
        key = '\n';
        result = chrono_dashboard_read_key(dashboard_tty_mode, &key);
        if (result == 0)
            dashboard_menu_render(session, screen, phase, controls, message, step, NULL);
    } while (result == 0 && dashboard_tty_mode && !manual_interrupted);
    if (result < 0 || manual_interrupted) return -1;
    if (result == 0) return '\n';
    char feedback[128];
    if (chrono_dashboard_input_feedback(key, feedback, sizeof(feedback)) == 0) {
        (void)snprintf(session->last_input, sizeof(session->last_input), "%s", feedback);
        dashboard_set_feedback(feedback);
    }
    return (unsigned char)key;
}

static void dashboard_menu_ack_choice(dashboard_menu_session *session,
                                      const char *selection)
{
    (void)snprintf(session->last_input, sizeof(session->last_input),
                   "[INPUT] Received: %s - MENU SELECTION", selection);
}

static int dashboard_menu_checkpoint_associated(const dashboard_menu_session *session,
                                                const chrono_checkpoint_metadata *metadata,
                                                const chrono_dashboard_checkpoint_row *row)
{
    return session->has_original && row != NULL &&
        metadata->original_pid == session->original_pid &&
        metadata->process_start_time == session->original_start &&
        strcmp(metadata->workload, "counter") == 0 &&
        strcmp(metadata->executable, session->original_identity.executable) == 0 &&
        row->original_uid == session->original_identity.owner &&
        row->executable_device == session->original_identity.executable_device &&
        row->executable_inode == session->original_identity.executable_inode &&
        session->original_identity.executable_device != 0 &&
        session->original_identity.executable_inode != 0;
}

static void dashboard_menu_report(dashboard_menu_session *session)
{
    dashboard_menu_render(session, CHRONO_DASHBOARD_SCREEN_REPORT,
                          "RECOVERY REPORT", "[any key] return to main menu",
                          NULL, NULL, NULL);
    (void)dashboard_menu_read_key(session, CHRONO_DASHBOARD_SCREEN_REPORT,
                                  "RECOVERY REPORT", "[any key] return to main menu",
                                  NULL, NULL);
}

int chrono_demo_manual_run(void)
{
    char directory[PATH_MAX], log_path[PATH_MAX], checkpoint_dir[PATH_MAX];
    char checkpoint_id[CHRONO_CHECKPOINT_ID_MAX + 1], error[512] = {0};
    FILE *log = NULL; env_value old_checkpoint = {0}, old_state = {0};
    int have_old_checkpoint = 0, have_old_state = 0, checkpoint_env = 0, state_env = 0;
    int dashboard_started = 0;
    int launched = 0, stopped = 0, have_restored = 0, restore_ambiguous = 0, complete = 0;
    int checkpoint_started = 0, result_code = 1;
    pid_t original_pid = -1, restored_pid = -1; uint64_t original_start = 0;
    chrono_process_info original = {0}, restored = {0};
    unsigned long long saved_counter = 0, first_restored = 0, later_counter = 0;
    chrono_checkpoint_metadata metadata = {0};
    chrono_demo_result summary = {0}; struct sigaction action = {0}, old_int, old_term;
    (void)setvbuf(stdout, NULL, _IOLBF, 0); manual_interrupted = 0;
    dashboard_tty_mode = 0;
    dashboard_feedback[0] = '\0';
    chrono_dashboard_process_snapshot_free(dashboard_processes);
    dashboard_processes = NULL;
    dashboard_process_count = 0;
    dashboard_inaccessible_processes = 0;
    dashboard_process_page = 0;
    dashboard_process_browser = 0;
    dashboard_process_help = 0;
    dashboard_process_view_active = 0;
    dashboard_has_monitored_process = 0;
    dashboard_monitored_status = NULL;
    dashboard_counter_endpoint_start = 0;
    dashboard_event_count = 0;
    dashboard_criu_status = "NOT CHECKED";
    dashboard_criu_diagnostic[0] = '\0';
    (void)snprintf(dashboard_criu_version, sizeof(dashboard_criu_version), "version unavailable");
    action.sa_handler = manual_signal; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int) != 0) return 1;
    if (sigaction(SIGTERM, &action, &old_term) != 0) { (void)sigaction(SIGINT, &old_int, NULL); return 1; }

    puts("\nChronoOS — Interactive Checkpoint and Restore Demonstration\n");
    puts("This walkthrough uses the configured CRIU adapter and a dedicated managed counter.");
    puts("A fake CRIU executable can test orchestration, but cannot demonstrate real checkpoint or restore.");
    if (chrono_criu_test_backend_enabled())
        puts("TEST SIMULATION ONLY: the selected fake backend cannot create a real CRIU checkpoint or restored process.");
    puts("CRIU preflight is not proof that dump or restore will succeed.\n");
    struct utsname system;
    if (uname(&system) == 0) (void)printf("Linux: %s %s (%s)\n", system.sysname, system.release, system.machine);
    else puts("Linux: unavailable");
    const char *mode = getenv("CHRONO_CRIU_UNPRIVILEGED");
    if (mode == NULL && geteuid() != 0) puts("CRIU mode: automatic unprivileged (--unprivileged)");
    else if (mode == NULL || strcmp(mode, "0") == 0) puts("CRIU mode: default");
    else if (strcmp(mode, "1") == 0) puts("CRIU mode: unprivileged (--unprivileged)");
    else { puts("CRIU mode: invalid CHRONO_CRIU_UNPRIVILEGED value"); goto done; }
    struct stat cst;
    if (!chrono_criu_test_backend_enabled() && chrono_criu_path_trusted(criu_path()) &&
        lstat(criu_path(), &cst) == 0 && S_ISREG(cst.st_mode) && access(criu_path(), X_OK) == 0) {
        char version[160];
        if (version_read(criu_path(), version, sizeof(version)) == 0) {
            (void)snprintf(dashboard_criu_version, sizeof(dashboard_criu_version), "%s", version);
            (void)printf("CRIU executable: %s (%s)\n", criu_path(), version);
        } else (void)printf("CRIU executable: %s (version query unavailable)\n", criu_path());
    } else if (chrono_criu_test_backend_enabled()) {
        (void)printf("CRIU executable: TEST-ONLY untrusted backend at %s\n", criu_path());
    } else {
        (void)printf("CRIU executable: untrusted or unavailable at %s\n", criu_path());
    }
    char *base = getenv("TMPDIR"); if (base == NULL || base[0] != '/') base = "/tmp";
    int n = snprintf(directory, sizeof(directory), "%s/chronoos-manual-XXXXXX", base);
    if (n < 0 || (size_t)n >= sizeof(directory) || mkdtemp(directory) == NULL || chmod(directory, 0700) != 0) {
        puts("Could not create a private demonstration directory."); goto done;
    }
    struct stat dst;
    if (lstat(directory, &dst) != 0 || !S_ISDIR(dst.st_mode) || dst.st_uid != getuid() ||
        (dst.st_mode & 0777) != 0700) { puts("Private demonstration directory validation failed."); goto done; }
    log = private_log(directory, log_path, sizeof(log_path));
    if (log == NULL) { puts("Could not create private diagnostic log."); goto done; }
    int m = snprintf(checkpoint_dir, sizeof(checkpoint_dir), "%s/checkpoints", directory);
    if (m < 0 || (size_t)m >= sizeof(checkpoint_dir)) goto cleanup;
    (void)snprintf(checkpoint_id, sizeof(checkpoint_id), "manual-%ld-%lld", (long)getpid(), (long long)time(NULL));
    (void)printf("Checkpoint directory: %s\nDiagnostic log: %s\n", checkpoint_dir, log_path);
    log_event(log, "Interactive run directory: %s", directory);
    if (env_save(&old_checkpoint, "CHRONO_CHECKPOINT_DIR") != 0) goto cleanup;
    have_old_checkpoint = 1;
    if (env_save(&old_state, "CHRONO_TEST_STATE_FILE") != 0) goto cleanup;
    have_old_state = 1;
    if (setenv("CHRONO_CHECKPOINT_DIR", checkpoint_dir, 1) != 0) goto cleanup;
    checkpoint_env = 1;
    if (unsetenv("CHRONO_TEST_STATE_FILE") != 0) goto cleanup;
    state_env = 1;

    if (chrono_dashboard_start(&dashboard_tty_mode) != 0) {
        log_event(log, "FAIL could not initialize dashboard input mode.");
        goto cleanup;
    }
    dashboard_started = 1;
    (void)refresh_dashboard_processes(log);

    capture cap = {.out = -1, .err = -1};
    int captured = capture_begin(fileno(log), &cap) == 0;
    int ready = captured ? chrono_criu_available(error, sizeof(error)) : 0;
    int replayed = captured && capture_end(&cap) == 0;
    dashboard_criu_status = criu_status_label(ready && replayed);
    if (ready && replayed) log_event(log, "CRIU preflight passed in the selected mode; real operation remains unverified.");
    else {
        const char *unprivileged = mode == NULL ? (geteuid() != 0 ? " --unprivileged (automatic)" : "") :
                                   strcmp(mode, "1") == 0 ? " --unprivileged" : "";
        (void)snprintf(dashboard_criu_diagnostic, sizeof(dashboard_criu_diagnostic),
                       "Command: %s check%s; %s", criu_path(), unprivileged,
                       error[0] ? error : "diagnostic capture failed");
        log_event(log, "CRIU preflight BLOCKED: %s", dashboard_criu_diagnostic);
    }
    if (manual_interrupted) goto cleanup;

    log_event(log, "Dashboard ready; awaiting explicit sample launch command.");
    int command = -1;
    while (!manual_interrupted) {
        command = dashboard_next_key("READY", "[p]rocess browser  [h]elp  [l]aunch sample  [r]efresh  [q]uit",
                                     0, 0, "not launched", NULL, checkpoint_dir, 0, NULL,
                                     "NOT ATTEMPTED", "not started", log_path);
        if (command == 'l') break;
        if (command == 'r') (void)refresh_dashboard_processes(log);
        if (command == 'q' || command < 0) goto cleanup;
    }
    if (manual_interrupted) goto cleanup;

    dashboard_set_feedback("[ACTION] Launching the managed counter workload...");
    dashboard_render("LAUNCHING WORKLOAD", "Verifying the bundled counter identity",
                     0, 0, "launch in progress", NULL, checkpoint_dir, 0, NULL,
                     "NOT ATTEMPTED", "not started", log_path);
    log_event(log, "Launching dedicated counter workload.");
    if (chrono_managed_start_bundled(1, &original_pid, &original_start, error, sizeof(error)) != 0) {
        dashboard_set_feedback("[FAILURE] Workload launch failed; no checkpoint was attempted.");
        dashboard_render("WORKLOAD LAUNCH FAILED", "[q]uit", 0, 0, "launch failed",
                         NULL, checkpoint_dir, 0, NULL, "NOT ATTEMPTED", "not started", log_path);
        log_event(log, "FAIL launch: %s", error); summary.launch = -1; goto cleanup;
    }
    launched = 1; summary.launch = 1; summary.original_pid = original_pid;
    dashboard_counter_endpoint_start = original_start;
    if (chrono_process_inspect(original_pid, &original, error, sizeof(error)) != 0 ||
        original.start_time != original_start || original.owner != getuid() || original.executable_inode == 0 ||
        wait_counter(original_pid, original_start, &saved_counter, 5) != 0) {
        log_event(log, "FAIL initial workload identity or counter verification: %s", error[0] ? error : "state unavailable");
        goto cleanup;
    }
    summary.checkpoint_executable = original.executable;
    log_event(log, "Managed workload: PID=%ld PPID=%ld STARTTIME=%llu UID=%lu state=%c executable=%s counter=%llu",
              (long)original.pid, (long)original.parent_pid, (unsigned long long)original.start_time,
              (unsigned long)original.owner, original.state, original.executable, saved_counter);
    log_event(log, "Sample launched; dashboard reads the in-memory counter over the managed control channel; /proc comm is display-only.");
    dashboard_set_feedback("[SUCCESS] Managed workload launched; process identity and counter verified.");
    while (!manual_interrupted) {
        command = dashboard_next_key("MONITORING", "[p]rocess browser  [h]elp  [c]heckpoint  [r]efresh  [q]uit",
                                     original_pid, original_start, "RUNNING", NULL,
                                     checkpoint_dir, 0, NULL, "NOT ATTEMPTED",
                                     "managed counter query; /proc comm is display-only", log_path);
        if (command == 'c') break;
        if (command == 'r') (void)refresh_dashboard_processes(log);
        if (command == 'q') {
            command = dashboard_next_key("CONFIRM EXIT AND CLEANUP",
                                         "SIGTERM the verified original and quit? [y]es  [n]o",
                                         original_pid, original_start, "RUNNING", NULL,
                                         checkpoint_dir, 0, NULL, "NOT ATTEMPTED",
                                         "cleanup requires confirmation", log_path);
            if (command == 'y') {
                dashboard_set_feedback("[ACTION] Cleaning up the verified original workload...");
                dashboard_render("CONFIRMED EXIT CLEANUP", "Stopping only this run's managed process",
                                 original_pid, original_start, "RUNNING", NULL,
                                 checkpoint_dir, 0, NULL, "NOT ATTEMPTED",
                                 "cleanup in progress", log_path);
                goto cleanup;
            }
            if (command < 0) goto cleanup;
        } else if (command < 0) goto cleanup;
    }
    if (manual_interrupted) goto cleanup;
    dashboard_set_feedback("[ACTION] Creating a genuine CRIU --leave-running checkpoint...");
    dashboard_render("CREATING CHECKPOINT", "Original process remains live during CRIU dump",
                     original_pid, original_start, "RUNNING", NULL, checkpoint_dir, 0,
                     NULL, "NOT ATTEMPTED", "managed counter query", log_path);
    summary.monitoring = 1; summary.checkpoint = -1; summary.checkpoint_sampled = 0;
    if (counter_read(original_pid, original_start, &saved_counter) != 0) {
        log_event(log, "FAIL the last persisted counter sample is unavailable; checkpoint was not attempted.");
        goto cleanup;
    }
    if (saved_counter == 0 && wait_counter_above(original_pid, original_start, 0,
                                                 &saved_counter, 3) != 0) {
        log_event(log, "FAIL the in-memory counter did not advance beyond zero; checkpoint was not attempted.");
        goto cleanup;
    }
    summary.checkpoint_counter = saved_counter; summary.checkpoint_sampled = 1;
    log_event(log, "In-memory counter sample before checkpoint: %llu.", saved_counter);
    memset(&metadata, 0, sizeof(metadata));
    (void)snprintf(metadata.id, sizeof(metadata.id), "%s", checkpoint_id);
    metadata.original_pid = original_pid; metadata.process_start_time = original_start;
    (void)snprintf(metadata.workload, sizeof(metadata.workload), "counter");
    if (snprintf(metadata.executable, sizeof(metadata.executable), "%s", original.executable) < 0 ||
        strlen(original.executable) >= sizeof(metadata.executable)) { log_event(log, "FAIL executable path exceeds metadata capacity."); goto cleanup; }
    (void)snprintf(metadata.backend, sizeof(metadata.backend), "criu"); metadata.state = CHRONO_CHECKPOINT_PREPARING;
    if (chrono_checkpoint_begin(checkpoint_dir, &metadata, error, sizeof(error)) != 0) {
        log_event(log, "FAIL checkpoint metadata creation: %s", error); goto cleanup;
    }
    checkpoint_started = 1;
    int process_fd = chrono_managed_pin(original_pid, original_start, error, sizeof(error));
    if (process_fd < 0 || chrono_managed_status(original_pid, original_start, error, sizeof(error)) != 0) {
        if (process_fd >= 0) close(process_fd);
        log_event(log, "FAIL managed workload identity before dump: %s", error);
        (void)chrono_checkpoint_transition(checkpoint_dir, checkpoint_id, CHRONO_CHECKPOINT_FAILED, error, sizeof(error));
        goto checkpoint_status;
    }
    log_event(log, "Creating a real CRIU --leave-running checkpoint while the managed process remains live.");
    int cap_ok = capture_begin(fileno(log), &cap) == 0;
    int dump_ok = cap_ok && chrono_criu_checkpoint_leave_running(&metadata, checkpoint_dir,
                                                                  error, sizeof(error)) == 0;
    int output_ok = cap_ok && capture_end(&cap) == 0;
    close(process_fd);
    if (dump_ok && output_ok && chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id) &&
        chrono_checkpoint_transition(checkpoint_dir, checkpoint_id, CHRONO_CHECKPOINT_COMPLETE,
                                     error, sizeof(error)) == 0) {
        summary.checkpoint = 1;
        dashboard_set_feedback("[SUCCESS] Checkpoint created; metadata and image artifacts validated.");
        log_event(log, "PASS configured backend leave-running operation; metadata and artifact structure validated. Backend identity is not attested.");
    } else {
        summary.checkpoint = -1;
        dashboard_set_feedback("[FAILURE] Checkpoint failed validation; termination and restore remain blocked.");
        if (dump_ok && output_ok) (void)snprintf(error, sizeof(error), "checkpoint artifact or metadata validation failed");
        dashboard_criu_status = "CRIU BLOCKED";
        (void)snprintf(dashboard_criu_diagnostic, sizeof(dashboard_criu_diagnostic), "%s",
                       error[0] != '\0' ? error : "checkpoint operation failed; inspect retained session log");
        if (checkpoint_started) (void)chrono_checkpoint_transition(checkpoint_dir, checkpoint_id,
                                          CHRONO_CHECKPOINT_FAILED, error, sizeof(error));
        log_event(log, "FAIL checkpoint: %s", error[0] ? error : "CRIU dump or diagnostic capture failed");
    }
checkpoint_status:
    if (checkpoint_started) {
        chrono_checkpoint_metadata observed;
        if (chrono_checkpoint_read(checkpoint_dir, checkpoint_id, &observed, error, sizeof(error)) == 0) {
            metadata = observed;
            log_event(log, "Checkpoint metadata state: %s; artifacts: %s.",
                      observed.state == CHRONO_CHECKPOINT_COMPLETE ? "COMPLETE" :
                      observed.state == CHRONO_CHECKPOINT_FAILED ? "FAILED" : "PREPARING",
                      chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id) ? "valid" : "invalid/missing");
        }
    }
    if (summary.checkpoint <= 0 || manual_interrupted) {
        if (!manual_interrupted)
            dashboard_render("CHECKPOINT FAILED", "[q]uit; the original remains managed",
                             original_pid, original_start, "RUNNING", &metadata,
                             checkpoint_dir, 0, NULL, "NOT ATTEMPTED",
                             "checkpoint not validated", log_path);
        goto cleanup;
    }

    log_event(log, "Checkpoint validated; original is still live. Waiting for explicit termination confirmation.");
    for (;;) {
        log_event(log, "Checkpoint validated; original remains live pending explicit confirmation.");
        command = dashboard_next_key("CHECKPOINT VALIDATED; ORIGINAL LIVE",
                                     "[t]erminate original  [q]uit",
                                     original_pid, original_start, "RUNNING", &metadata,
                                     checkpoint_dir, chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                                     NULL, "NOT ATTEMPTED", "saved managed counter in CRIU image", log_path);
        if (command == 'q') {
            command = dashboard_next_key("CONFIRM EXIT AND CLEANUP",
                                         "SIGTERM the verified original and quit? [y]es  [n]o",
                                         original_pid, original_start, "RUNNING", &metadata,
                                         checkpoint_dir, chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                                         NULL, "NOT ATTEMPTED", "cleanup requires confirmation", log_path);
            if (command == 'y') {
                dashboard_set_feedback("[ACTION] Cleaning up the verified original workload...");
                dashboard_render("CONFIRMED EXIT CLEANUP", "Stopping only this run's managed process",
                                 original_pid, original_start, "RUNNING", &metadata,
                                 checkpoint_dir, chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                                 NULL, "NOT ATTEMPTED", "cleanup in progress", log_path);
                goto cleanup;
            }
            if (command < 0) goto cleanup;
            continue;
        }
        if (command < 0) goto cleanup;
        if (command != 't') continue;
        dashboard_set_feedback("[WAITING] Press y to confirm termination of the original process, or n to cancel.");
        command = dashboard_next_key("CONFIRM ORIGINAL TERMINATION",
                                     "SIGTERM only the verified original? [y]es  [n]o",
                                     original_pid, original_start, "RUNNING", &metadata,
                                     checkpoint_dir, chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                                     NULL, "NOT ATTEMPTED", "confirmation required", log_path);
        if (command == 'y') break;
        if (command == 'q' || command < 0) goto cleanup;
        if (command == 'n') {
            dashboard_set_feedback("[INFO] Termination cancelled; the original process remains running.");
            log_event(log, "Termination confirmation declined; original process remains running.");
        }
    }
    dashboard_set_feedback("[ACTION] Rechecking managed process identity before SIGTERM...");
    dashboard_render("VERIFYING TERMINATION REQUEST", "Explicit y confirmation received",
                     original_pid, original_start, "RUNNING", &metadata,
                     checkpoint_dir, chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                     NULL, "NOT ATTEMPTED", "saved counter retained in checkpoint", log_path);
    if (chrono_managed_status(original_pid, original_start, error, sizeof(error)) != 0 ||
        chrono_managed_stop(original_pid, original_start, error, sizeof(error)) != 0) {
        summary.original_stop = -1;
        dashboard_set_feedback("[FAILURE] Termination identity check or verified SIGTERM failed.");
        dashboard_render("TERMINATION FAILED", "[q]uit; restore remains blocked",
                         original_pid, original_start, "RUNNING / NOT VERIFIED", &metadata,
                         checkpoint_dir, chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                         NULL, "NOT ATTEMPTED", "not checked", log_path);
        log_event(log, "FAIL verified SIGTERM for original process: %s", error);
        goto cleanup;
    }
    log_event(log, "SIGTERM sent through the verified managed-process identity.");
    if (wait_original_stop(original_pid, original_start, log) != 0) { summary.original_stop = -1; goto cleanup; }
    stopped = 1; summary.original_stop = 1;
    dashboard_set_feedback("[SUCCESS] Original process terminated and reaped after confirmation.");
    log_event(log, "PASS original PID=%ld STARTTIME=%llu terminated and was reaped after checkpoint validation.",
              (long)original_pid, (unsigned long long)original_start);
    for (;;) {
        command = dashboard_next_key("ORIGINAL TERMINATED AND REAPED",
                                     "[r]estore from validated checkpoint  [q]uit",
                                     0, 0, "TERMINATED AND REAPED", &metadata,
                                     checkpoint_dir, chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                                     NULL, "NOT ATTEMPTED", "awaiting restore", log_path);
        if (command == 'r') break;
        if (command == 'q' || command < 0) goto cleanup;
    }
    chrono_checkpoint_metadata completed_meta;
    if (chrono_checkpoint_read(checkpoint_dir, checkpoint_id, &completed_meta, error, sizeof(error)) != 0 ||
        completed_meta.state != CHRONO_CHECKPOINT_COMPLETE || !chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id)) {
        log_event(log, "FAIL restore eligibility: checkpoint metadata/artifacts are not valid."); goto cleanup;
    }
    summary.restore = -1;
    dashboard_set_feedback("[ACTION] Restoring from the validated CRIU checkpoint...");
    dashboard_render("RESTORING FROM VALIDATED CRIU IMAGE",
                     "CRIU restore is in progress; waiting for process verification",
                     0, 0, "TERMINATED AND REAPED", &completed_meta, checkpoint_dir,
                     chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                     NULL, "RESTORING; NOT YET VERIFIED", "not yet checked", log_path);
    int restore_capture = capture_begin(fileno(log), &cap) == 0;
    int restore_ok = restore_capture && chrono_criu_restore(&completed_meta, checkpoint_dir,
                                                            &restored_pid, error, sizeof(error)) == 0;
    output_ok = restore_capture && capture_end(&cap) == 0;
    if (!restore_ok) {
        restore_ambiguous = 1;
        dashboard_set_feedback("[FAILURE] CRIU restore failed; no process was adopted.");
        dashboard_render("RESTORE FAILED", "[q]uit; no restored process identity was accepted",
                         0, 0, "TERMINATED AND REAPED", &completed_meta, checkpoint_dir,
                         chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                         NULL, "FAILED / NOT VERIFIED", "not verified", log_path);
        log_event(log, "FAIL CRIU restore adapter: %s. No retry or unverified signal will be attempted.", error[0] ? error : "diagnostic capture/adapter failure");
        goto cleanup;
    }
    summary.restored_pid = restored_pid; have_restored = 1;
    if (!output_ok) log_event(log, "CRIU returned success; output replay failed but the diagnostic log is retained.");
    log_event(log, "CRIU returned candidate PID %ld; verifying its live identity before adoption.", (long)restored_pid);
    int verified = 0;
    for (int i = 0; i < 20 && !manual_interrupted; ++i) {
        if (chrono_process_inspect(restored_pid, &restored, error, sizeof(error)) == 0 &&
            identity_matches(&restored, &original)) { verified = 1; break; }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !manual_interrupted) {}
    }
    if (!verified || manual_interrupted) {
        dashboard_set_feedback("[FAILURE] Restored process identity was not verified; it will not be adopted.");
        if (!manual_interrupted)
            dashboard_render("RESTORED IDENTITY NOT VERIFIED", "[q]uit; candidate will not be adopted",
                             0, 0, "TERMINATED AND REAPED", &completed_meta, checkpoint_dir,
                             chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                             NULL, "FAILED / NOT VERIFIED", "not verified", log_path);
        log_event(log, "FAIL restored process identity: %s", error[0] ? error : "identity mismatch");
        goto cleanup;
    }
    summary.executable = 1; summary.restored_executable = restored.executable;
    log_event(log, "PASS restored identity: PID=%ld STARTTIME=%llu UID=%lu state=%c executable=%s",
              (long)restored.pid, (unsigned long long)restored.start_time, (unsigned long)restored.owner,
              restored.state, restored.executable);
    dashboard_set_feedback("[SUCCESS] Restored process identity independently verified.");
    if (restored.pid == original_pid && restored.start_time == original_start) {
        summary.executable = -1; log_event(log, "FAIL restored identity reused the original PID and start time."); goto cleanup;
    }
    log_event(log, "Dashboard verified and adopted the restored process identity for this demo session.");
    if (!original_gone(original_pid, original_start)) {
        summary.original_stop = -1; log_event(log, "FAIL original process identity remains live beside restored workload."); goto cleanup;
    }
    unsigned long long raw_restored = 0;
    if (counter_read(restored_pid, restored.start_time, &raw_restored) == 0)
        log_event(log, "Restored in-memory counter sampled through the managed control channel: %llu.", raw_restored);
    else log_event(log, "Restored in-memory counter is not yet available through the managed control channel.");
    int advanced = 0;
    for (int i = 0; i < 32 && !manual_interrupted; ++i) {
        chrono_process_info current;
        unsigned long long value;
        if (chrono_process_inspect(restored_pid, &current, error, sizeof(error)) != 0 ||
            current.start_time != restored.start_time || !identity_matches(&current, &original)) {
            log_event(log, "FAIL restored workload identity changed while awaiting its next persisted state.");
            goto cleanup;
        }
        if (counter_read(restored_pid, restored.start_time, &value) == 0 && value > saved_counter) {
            first_restored = value; advanced = 1; break;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !manual_interrupted) {}
    }
    if (!advanced || manual_interrupted) {
        log_event(log, "FAIL restored in-memory counter did not advance beyond checkpoint sample %llu.", saved_counter);
        goto cleanup;
    }
    summary.counter_continuity = 1; summary.restored_counter = first_restored;
    log_event(log, "First resumed in-memory counter=%llu, beyond the checkpoint sample=%llu.",
              first_restored, saved_counter);
    advanced = 0;
    for (int i = 0; i < 20 && !manual_interrupted; ++i) {
        chrono_process_info current;
        unsigned long long value;
        if (chrono_process_inspect(restored_pid, &current, error, sizeof(error)) != 0 ||
            current.start_time != restored.start_time || !identity_matches(&current, &original)) break;
        if (counter_read(restored_pid, restored.start_time, &value) == 0 && value > first_restored) {
            later_counter = value; advanced = 1; break;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !manual_interrupted) {}
    }
    if (!advanced || manual_interrupted) {
        summary.counter_continuity = -1;
        log_event(log, "FAIL restored in-memory counter did not advance after its first resumed sample.");
        goto cleanup;
    }
    summary.restore = 1;
    summary.restored_counter = later_counter;
    log_event(log, "PASS restored process verified; in-memory counter advanced again: %llu to %llu.", first_restored, later_counter);
    log_event(log, "Restore identity and in-memory counter continuity verified.");
    dashboard_set_feedback("[SUCCESS] Restored identity verified; in-memory counter continuity confirmed.");
    while (!manual_interrupted) {
        command = dashboard_next_key("RESTORED PROCESS VERIFIED; MONITORING",
                                     "[r]efresh  [q]uit and terminate restored workload",
                                     restored_pid, restored.start_time, "RUNNING (session-adopted)",
                                     &completed_meta, checkpoint_dir,
                                     chrono_checkpoint_artifact_valid(checkpoint_dir, checkpoint_id),
                                     &restored, "VERIFIED LIVE PROCESS",
                                     "managed counter advancing; /proc comm is display-only", log_path);
        if (command == 'q' || command < 0) break;
        unsigned long long value;
        if (counter_read(restored_pid, restored.start_time, &value) == 0 && value > first_restored) later_counter = value;
    }
    if (later_counter > first_restored) {
        summary.counter_continuity = 1; summary.restored_counter = later_counter;
        log_event(log, "PASS restored counter advanced from %llu to %llu.", first_restored, later_counter);
    } else if (!manual_interrupted) {
        summary.counter_continuity = -1; log_event(log, "FAIL no later counter advance was observed before cleanup.");
    }

cleanup:
    dashboard_process_browser = 0;
    dashboard_process_help = 0;
    dashboard_has_monitored_process = 0;
    if (dashboard_started) {
        chrono_dashboard_stop(dashboard_tty_mode);
        dashboard_started = 0;
    }
    if (log != NULL) log_event(log, "Cleanup considers only this run's recorded process identities.");
    struct sigaction ignore = {0}; ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    (void)sigaction(SIGINT, &ignore, NULL); (void)sigaction(SIGTERM, &ignore, NULL);
    int cleanup_ok = 1;
    if (launched && !stopped) {
        if (chrono_managed_status(original_pid, original_start, error, sizeof(error)) == 0) {
            if (chrono_managed_stop(original_pid, original_start, error, sizeof(error)) != 0 ||
                wait_original_stop(original_pid, original_start, log) != 0) {
                cleanup_ok = 0; log_event(log, "Unresolved original process PID=%ld STARTTIME=%llu; identity-verified stop failed: %s",
                                          (long)original_pid, (unsigned long long)original_start, error);
            } else {
                stopped = 1;
                if (summary.original_stop == 0) {
                    summary.original_stop = 1;
                    log_event(log, "Original process terminated by verified SIGTERM during cleanup.");
                }
            }
        } else if (original_gone(original_pid, original_start)) {
            if (wait_original_stop(original_pid, original_start, log) == 0) stopped = 1;
            else cleanup_ok = 0;
        } else { cleanup_ok = 0; log_event(log, "Original identity status is ambiguous; no signal sent: %s", error); }
    }
    if (have_restored) {
        chrono_process_info current;
        int inspected = chrono_process_inspect(restored_pid, &current, error, sizeof(error)) == 0;
        if (inspected && restored.start_time != 0 && current.start_time == restored.start_time &&
            identity_matches(&current, &original)) {
            if (stop_verified(&current, log) != 0) cleanup_ok = 0;
            else log_event(log, "Restored workload was terminated by verified SIGTERM.");
        } else if (inspected && current.state != 'Z' &&
                   (restored.start_time == 0 || current.start_time == restored.start_time)) {
            cleanup_ok = 0;
            log_event(log, "Restored candidate PID=%ld remains live but its identity was not verified; no signal sent.",
                      (long)restored_pid);
        } else if (inspected && restored.start_time != 0 && current.start_time != restored.start_time) {
            log_event(log, "Restored PID was reused; refusing cleanup signal.");
        }
    }
    if (restore_ambiguous) { cleanup_ok = 0; log_event(log, "Restore outcome is ambiguous; no process was guessed or signaled."); }
    if (checkpoint_env && env_restore(&old_checkpoint) != 0) cleanup_ok = 0;
    if (state_env && env_restore(&old_state) != 0) cleanup_ok = 0;
    summary.cleanup = cleanup_ok ? 1 : -1;
    if (manual_interrupted) log_event(log, "Interrupted. Remaining stages are not reported as passed.");
    if (summary.launch > 0 && summary.monitoring > 0 && summary.checkpoint > 0 &&
        summary.original_stop > 0 && summary.restore > 0 && summary.executable > 0 &&
        summary.counter_continuity > 0 && summary.cleanup > 0 && !manual_interrupted)
        complete = 1;
    if (log != NULL) {
        log_event(log, "Final stage summary:"); print_summary(&summary); chrono_demo_print_summary(log, &summary, 0);
        log_event(log, "Recovery history:");
        log_event(log, "Checkpoint %s: %s; storage=%s/%s", checkpoint_id,
                  summary.checkpoint > 0 ? "COMPLETE" : summary.checkpoint < 0 ? "FAILED" : "NOT ATTEMPTED",
                  checkpoint_dir, checkpoint_id);
        log_event(log, "Original PID=%ld STARTTIME=%llu: %s", (long)original_pid,
                  (unsigned long long)original_start,
                  summary.original_stop > 0 ? "terminated and identity verified" :
                  summary.original_stop < 0 ? "termination failed or unverified" : "not terminated");
        if (summary.executable > 0)
            log_event(log, "Restored PID=%ld STARTTIME=%llu UID=%lu executable=%s: identity verified",
                      (long)restored.pid, (unsigned long long)restored.start_time,
                      (unsigned long)restored.owner, restored.executable);
        else if (have_restored)
            log_event(log, "Restore candidate PID=%ld: identity not verified; not adopted or signaled",
                      (long)restored_pid);
        else
            log_event(log, "Restore: %s", summary.restore < 0 ? "failed; no restored process adopted" : "not attempted");
        log_event(log, "Final status: %s", complete ? "DEMO COMPLETE" : "DEMO INCOMPLETE");
        log_event(log, "Checkpoint artifacts and diagnostics retained in %s", directory);
        print_final_history(complete);
    }
    chrono_dashboard_process_snapshot_free(dashboard_processes);
    dashboard_processes = NULL;
    dashboard_process_count = 0;
    dashboard_inaccessible_processes = 0;
    result_code = complete ? 0 : 1;
    if (log != NULL) { fclose(log); log = NULL; }
done:
    if (have_old_checkpoint && old_checkpoint.value != NULL) free(old_checkpoint.value);
    if (have_old_state && old_state.value != NULL) free(old_state.value);
    (void)sigaction(SIGINT, &old_int, NULL); (void)sigaction(SIGTERM, &old_term, NULL);
    if (manual_interrupted) return 130;
    return result_code;
}

int chrono_dashboard_menu_run(void)
{
    char directory[PATH_MAX] = {0}, log_path[PATH_MAX] = {0};
    char checkpoint_directory[PATH_MAX] = {0}, error[512] = {0};
    char checkpoint_id[CHRONO_CHECKPOINT_ID_MAX + 1];
    FILE *log = NULL;
    env_value old_checkpoint = {0}, old_state = {0};
    dashboard_menu_session session = {0};
    struct sigaction action = {0}, old_int, old_term;
    int have_old_checkpoint = 0, have_old_state = 0;
    int checkpoint_env = 0, state_env = 0, dashboard_started = 0;
    int result = 0, cleanup_ok = 1, capture_active = 0;
    capture cap = {.out = -1, .err = -1};
    unsigned checkpoint_sequence = 0;

    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    manual_interrupted = 0;
    dashboard_tty_mode = 0;
    dashboard_feedback[0] = '\0';
    dashboard_counter_endpoint_start = 0;
    dashboard_event_count = 0;
    dashboard_criu_status = "NOT CHECKED";
    dashboard_criu_diagnostic[0] = '\0';
    (void)snprintf(dashboard_criu_version, sizeof(dashboard_criu_version),
                   "version unavailable");
    chrono_dashboard_process_snapshot_free(dashboard_processes);
    dashboard_processes = NULL;
    dashboard_process_count = 0;
    dashboard_inaccessible_processes = 0;
    action.sa_handler = manual_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int) != 0) return 1;
    if (sigaction(SIGTERM, &action, &old_term) != 0) {
        (void)sigaction(SIGINT, &old_int, NULL);
        return 1;
    }

    const char *mode = getenv("CHRONO_CRIU_UNPRIVILEGED");
    if (mode != NULL && strcmp(mode, "0") != 0 && strcmp(mode, "1") != 0) {
        (void)fprintf(stderr, "chrono: invalid CHRONO_CRIU_UNPRIVILEGED value\n");
        result = 1;
        goto menu_done;
    }
    char *base = getenv("TMPDIR");
    if (base == NULL || base[0] != '/') base = "/tmp";
    int n = snprintf(directory, sizeof(directory), "%s/chronoos-dashboard-XXXXXX", base);
    if (n < 0 || (size_t)n >= sizeof(directory) ||
        mkdtemp(directory) == NULL || chmod(directory, 0700) != 0) {
        (void)fprintf(stderr, "chrono: could not create private dashboard storage\n");
        result = 1;
        goto menu_done;
    }
    struct stat directory_stat;
    if (lstat(directory, &directory_stat) != 0 || !S_ISDIR(directory_stat.st_mode) ||
        directory_stat.st_uid != getuid() || (directory_stat.st_mode & 0777) != 0700) {
        (void)fprintf(stderr, "chrono: private dashboard storage validation failed\n");
        result = 1;
        goto menu_done;
    }
    log = private_log(directory, log_path, sizeof(log_path));
    if (log == NULL) {
        (void)fprintf(stderr, "chrono: could not create private dashboard log\n");
        result = 1;
        goto menu_done;
    }
    n = snprintf(checkpoint_directory, sizeof(checkpoint_directory), "%s/checkpoints", directory);
    if (n < 0 || (size_t)n >= sizeof(checkpoint_directory)) {
        result = 1;
        goto menu_cleanup;
    }
    session.log = log;
    session.checkpoint_directory = checkpoint_directory;
    session.log_path = log_path;
    if (env_save(&old_checkpoint, "CHRONO_CHECKPOINT_DIR") != 0) {
        result = 1;
        goto menu_cleanup;
    }
    have_old_checkpoint = 1;
    if (env_save(&old_state, "CHRONO_TEST_STATE_FILE") != 0) {
        result = 1;
        goto menu_cleanup;
    }
    have_old_state = 1;
    if (setenv("CHRONO_CHECKPOINT_DIR", checkpoint_directory, 1) != 0) {
        result = 1;
        goto menu_cleanup;
    }
    checkpoint_env = 1;
    if (unsetenv("CHRONO_TEST_STATE_FILE") != 0) {
        result = 1;
        goto menu_cleanup;
    }
    state_env = 1;
    if (chrono_dashboard_start(&dashboard_tty_mode) != 0) {
        log_event(log, "FAIL initializing terminal dashboard");
        result = 1;
        goto menu_cleanup;
    }
    dashboard_started = 1;
    (void)refresh_dashboard_processes(log);

    if (!chrono_criu_test_backend_enabled() && chrono_criu_path_trusted(criu_path())) {
        char version[160];
        if (version_read(criu_path(), version, sizeof(version)) == 0)
            (void)snprintf(dashboard_criu_version, sizeof(dashboard_criu_version), "%s", version);
    }
    if (capture_begin(fileno(log), &cap) == 0) {
        capture_active = 1;
        int ready = chrono_criu_available(error, sizeof(error));
        int output_ok = capture_end(&cap) == 0;
        capture_active = 0;
        if (ready && output_ok) {
            dashboard_criu_status = chrono_criu_test_backend_enabled() ?
                "TEST SIMULATION ONLY" : criu_status_label(1);
            log_event(log, "CRIU preflight passed; operation results are not yet established.");
        } else {
            dashboard_criu_status = chrono_criu_test_backend_enabled() ?
                "TEST SIMULATION ONLY; operation preflight failed" : "CRIU PREFLIGHT BLOCKED";
            (void)snprintf(dashboard_criu_diagnostic,
                sizeof(dashboard_criu_diagnostic), "%s",
                error[0] != '\0' ? error : "preflight output capture failed");
            log_event(log, "CRIU preflight blocked: %s", dashboard_criu_diagnostic);
        }
    } else {
        dashboard_criu_status = chrono_criu_test_backend_enabled() ?
            "TEST SIMULATION ONLY" : "CRIU PREFLIGHT BLOCKED";
        (void)snprintf(dashboard_criu_diagnostic, sizeof(dashboard_criu_diagnostic),
                       "could not capture preflight diagnostics");
    }
    log_event(log, "Progressive dashboard session ready; only the bundled counter is checkpoint-authorized.");

    dashboard_set_feedback("");
    while (!manual_interrupted && !session.exited) {
        int choice = dashboard_menu_read_choice(&session, CHRONO_DASHBOARD_SCREEN_MENU,
            "MAIN MENU", "Select 1-11 and press ENTER", NULL, NULL, 1, 11);
        if (choice < 0) {
            if (!manual_interrupted) result = 1;
            break;
        }
        char selection[16];
        (void)snprintf(selection, sizeof(selection), "%d", choice);
        dashboard_menu_ack_choice(&session, selection);

        if (choice == 1) {
            if (session.has_original) {
                dashboard_set_feedback("[INFO] The bundled demonstration process is already selected.");
            } else {
                dashboard_set_feedback("[ACTION] Launching the bundled counter workload...");
                dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_PROGRAM,
                    "PROGRAM SELECTION", "Launching only the bundled counter", NULL,
                    "Verifying launched process identity", NULL);
                log_event(log, "Launching the bundled counter workload.");
                if (chrono_managed_start_bundled(1, &session.original_pid,
                        &session.original_start, error, sizeof(error)) != 0) {
                    dashboard_set_feedback("[FAILURE] Bundled workload launch failed.");
                    log_event(log, "FAIL bundled workload launch: %s", error);
                } else {
                    session.has_original = 1;
                    dashboard_counter_endpoint_start = session.original_start;
                    if (chrono_process_inspect(session.original_pid, &session.original,
                            error, sizeof(error)) != 0 ||
                        session.original.start_time != session.original_start ||
                        session.original.owner != getuid() ||
                        session.original.executable_inode == 0 ||
                        wait_counter(session.original_pid, session.original_start,
                                     &session.checkpoint_counter, 5) != 0) {
                        dashboard_set_feedback("[FAILURE] Process identity or in-memory counter verification failed.");
                        log_event(log, "FAIL launch identity/counter verification: %s",
                                  error[0] != '\0' ? error : "counter unavailable");
                    } else {
                        session.original_identity = session.original;
                        (void)snprintf(session.launch_arguments,
                            sizeof(session.launch_arguments),
                            "counter_workload 1 <launch authorization redacted> %llu %llu",
                            (unsigned long long)session.original.executable_device,
                            (unsigned long long)session.original.executable_inode);
                        (void)dashboard_menu_read_working_directory(session.original_pid,
                            session.original_start, session.working_directory,
                            sizeof(session.working_directory));
                        log_event(log,
                            "Selected bundled program PID=%ld PPID=%ld UID=%lu START=%llu executable=%s device=%llu inode=%llu.",
                            (long)session.original.pid, (long)session.original.parent_pid,
                            (unsigned long)session.original.owner,
                            (unsigned long long)session.original.start_time,
                            session.original.executable,
                            (unsigned long long)session.original.executable_device,
                            (unsigned long long)session.original.executable_inode);
                        dashboard_set_feedback("[SUCCESS] Bundled counter identified by executable identity and start time.");
                    }
                }
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_PROGRAM,
                "PROGRAM IDENTIFICATION", "[any key] return to main menu",
                NULL, NULL, NULL);
            (void)dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_PROGRAM,
                "PROGRAM IDENTIFICATION", "[any key] return to main menu", NULL, NULL);
        } else if (choice == 2) {
            const char *message =
                "Custom executable launch is disabled: only the bundled counter has scoped CRIU authorization.";
            dashboard_set_feedback("[UNAVAILABLE] No custom executable was launched or authorized.");
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_MESSAGE,
                "CUSTOM EXECUTABLE", "[any key] return to main menu",
                message, NULL, NULL);
            (void)dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_MESSAGE,
                "CUSTOM EXECUTABLE", "[any key] return to main menu", message, NULL);
        } else if (choice == 3) {
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_DETAILS,
                "PROCESS DETAILS", "[any key] return to main menu",
                session.has_original ? NULL : "Launch the bundled demonstration first.",
                NULL, NULL);
            (void)dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_DETAILS,
                "PROCESS DETAILS", "[any key] return to main menu",
                session.has_original ? NULL : "Launch the bundled demonstration first.", NULL);
        } else if (choice == 4) {
            if (!session.has_original) {
                dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_MONITOR,
                    "PROCESS MONITORING", "[any key] return to main menu",
                    "No managed process has been launched.", NULL, NULL);
                (void)dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_MONITOR,
                    "PROCESS MONITORING", "[any key] return to main menu",
                    "No managed process has been launched.", NULL);
                continue;
            }
            for (;;) {
                int key = dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_MONITOR,
                    "PROCESS MONITORING", "[r]efresh  [q]return to main menu",
                    NULL, NULL);
                if (key == 'q' || key < 0) break;
                if (chrono_process_inspect(session.original_pid, &session.original,
                        error, sizeof(error)) != 0 ||
                    session.original.start_time != session.original_start ||
                    !identity_matches(&session.original, &session.original_identity)) {
                    dashboard_set_feedback("[FAILURE] Selected process identity is no longer available.");
                } else {
                    unsigned long long current_counter;
                    if (counter_read(session.original_pid, session.original_start,
                                     &current_counter) == 0)
                        dashboard_set_feedback("[INFO] Live in-memory counter refreshed; /proc/comm is display-only.");
                    else
                        dashboard_set_feedback("[WARNING] Process is present but its in-memory counter is unavailable.");
                }
            }
        } else if (choice == 5) {
            if (!session.has_original || session.original_stopped) {
                dashboard_set_feedback("[BLOCKED] Launch the bundled counter before creating a checkpoint.");
                continue;
            }
            int confirm = dashboard_menu_read_key(&session,
                CHRONO_DASHBOARD_SCREEN_CHECKPOINT, "CHECKPOINT CONFIRMATION",
                "[y] begin checkpoint  [n] return",
                "Review the exact selected program and executable before proceeding.", NULL);
            if (confirm != 'y') {
                dashboard_set_feedback("[INFO] Checkpoint creation cancelled.");
                continue;
            }
            ++checkpoint_sequence;
            (void)snprintf(checkpoint_id, sizeof(checkpoint_id),
                "dashboard-%ld-%lld-%u", (long)session.original_pid,
                (long long)time(NULL), checkpoint_sequence);
            chrono_checkpoint_metadata metadata = {0};
            (void)snprintf(metadata.id, sizeof(metadata.id), "%s", checkpoint_id);
            metadata.original_pid = session.original_pid;
            metadata.process_start_time = session.original_start;
            (void)snprintf(metadata.workload, sizeof(metadata.workload), "counter");
            if (snprintf(metadata.executable, sizeof(metadata.executable), "%s",
                          session.original.executable) < 0 ||
                strlen(session.original.executable) >= sizeof(metadata.executable)) {
                dashboard_set_feedback("[FAILURE] Executable path exceeds checkpoint metadata capacity.");
                continue;
            }
            (void)snprintf(metadata.backend, sizeof(metadata.backend), "criu");
            metadata.state = CHRONO_CHECKPOINT_PREPARING;
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT,
                "CHECKPOINT MANAGEMENT", "Original remains running during leave-running dump",
                NULL, "[1] Verify selected process identity", NULL);
            if (chrono_managed_status(session.original_pid, session.original_start,
                                      error, sizeof(error)) != 0 ||
                chrono_process_inspect(session.original_pid, &session.original,
                                       error, sizeof(error)) != 0 ||
                !identity_matches(&session.original, &session.original_identity) ||
                session.original.owner != getuid()) {
                dashboard_set_feedback("[FAILURE] Selected process identity verification failed; dump not attempted.");
                log_event(log, "FAIL pre-checkpoint managed identity: %s", error);
                continue;
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT,
                "CHECKPOINT MANAGEMENT", "Original remains running during leave-running dump",
                NULL, "[1] PASS — selected process identity verified", NULL);
            if (counter_read(session.original_pid, session.original_start,
                             &session.checkpoint_counter) != 0 ||
                (session.checkpoint_counter == 0 &&
                 wait_counter_above(session.original_pid, session.original_start, 0,
                                    &session.checkpoint_counter, 3) != 0)) {
                dashboard_set_feedback("[FAILURE] In-memory counter sample unavailable; checkpoint not attempted.");
                log_event(log, "FAIL checkpoint counter sampling");
                continue;
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT,
                "CHECKPOINT MANAGEMENT", "Original remains running during leave-running dump",
                NULL, "[2] Preparing private checkpoint storage", NULL);
            if (chrono_checkpoint_begin(checkpoint_directory, &metadata, error,
                                        sizeof(error)) != 0) {
                dashboard_set_feedback("[FAILURE] Could not record checkpoint metadata.");
                log_event(log, "FAIL checkpoint metadata preparation: %s", error);
                continue;
            }
            int process_fd = chrono_managed_pin(session.original_pid,
                session.original_start, error, sizeof(error));
            if (process_fd < 0 ||
                chrono_managed_status(session.original_pid, session.original_start,
                                      error, sizeof(error)) != 0) {
                if (process_fd >= 0) close(process_fd);
                (void)chrono_checkpoint_transition(checkpoint_directory, checkpoint_id,
                    CHRONO_CHECKPOINT_FAILED, error, sizeof(error));
                dashboard_set_feedback("[FAILURE] Identity changed before CRIU dump.");
                log_event(log, "FAIL identity immediately before dump: %s", error);
                continue;
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT,
                "CHECKPOINT MANAGEMENT", "The original should remain alive",
                NULL, chrono_criu_test_backend_enabled() ?
                    "[2] PASS — private metadata prepared\n[3] SIMULATION ONLY — invoking fake backend" :
                    "[2] PASS — private metadata prepared\n[3] Invoking genuine CRIU --leave-running",
                NULL);
            int capture_ok = capture_begin(fileno(log), &cap) == 0;
            int dump_ok = capture_ok &&
                chrono_criu_checkpoint_leave_running(&metadata, checkpoint_directory,
                                                     error, sizeof(error)) == 0;
            int output_ok = capture_ok && capture_end(&cap) == 0;
            capture_active = 0;
            close(process_fd);
            if (!dump_ok || !output_ok) {
                (void)chrono_checkpoint_transition(checkpoint_directory, checkpoint_id,
                    CHRONO_CHECKPOINT_FAILED, error, sizeof(error));
                dashboard_set_feedback("[FAILURE] Genuine checkpoint adapter did not complete successfully.");
                log_event(log, "FAIL CRIU checkpoint: %s",
                    error[0] != '\0' ? error : "adapter/output capture failure");
                continue;
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT,
                "CHECKPOINT MANAGEMENT", "CRIU adapter returned; checking artifacts",
                NULL, "[3] PASS — adapter returned\n[4] Validating checkpoint artifact structure", NULL);
            if (!chrono_checkpoint_artifact_valid(checkpoint_directory, checkpoint_id)) {
                (void)chrono_checkpoint_transition(checkpoint_directory, checkpoint_id,
                    CHRONO_CHECKPOINT_FAILED, error, sizeof(error));
                dashboard_set_feedback("[FAILURE] Checkpoint artifact structure is invalid.");
                log_event(log, "FAIL checkpoint structural artifact validation");
                continue;
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT,
                "CHECKPOINT MANAGEMENT", "Structural validity is not proof of restorability",
                NULL, "[4] PASS — expected private image structure found\n[5] Recording COMPLETE metadata", NULL);
            if (chrono_checkpoint_transition(checkpoint_directory, checkpoint_id,
                    CHRONO_CHECKPOINT_COMPLETE, error, sizeof(error)) != 0 ||
                chrono_checkpoint_read(checkpoint_directory, checkpoint_id,
                    &metadata, error, sizeof(error)) != 0 ||
                metadata.state != CHRONO_CHECKPOINT_COMPLETE) {
                (void)chrono_checkpoint_transition(checkpoint_directory, checkpoint_id,
                    CHRONO_CHECKPOINT_FAILED, error, sizeof(error));
                dashboard_set_feedback("[FAILURE] Checkpoint metadata could not be completed.");
                log_event(log, "FAIL completing checkpoint metadata: %s", error);
                continue;
            }
            session.checkpoint_passed = 1;
            if (dashboard_menu_refresh_checkpoints(&session, error, sizeof(error)) != 0 ||
                session.checkpoint_count == 0) {
                dashboard_set_feedback("[FAILURE] Completed checkpoint could not be listed.");
                log_event(log, "FAIL listing newly completed checkpoint: %s", error);
                continue;
            }
            session.selected_checkpoint = session.checkpoint_count - 1;
            session.has_selected_checkpoint = 1;
            int found_created_checkpoint = 0;
            for (size_t i = 0; i < session.checkpoint_count; ++i) {
                if (strcmp(session.checkpoints[i].metadata.id, checkpoint_id) == 0) {
                    session.selected_checkpoint = i;
                    session.checkpoints[i].saved_counter = session.checkpoint_counter;
                    session.checkpoints[i].original_uid = session.original_identity.owner;
                    session.checkpoints[i].executable_device =
                        session.original_identity.executable_device;
                    session.checkpoints[i].executable_inode =
                        session.original_identity.executable_inode;
                    found_created_checkpoint = 1;
                    break;
                }
            }
            if (!found_created_checkpoint) {
                session.has_selected_checkpoint = 0;
                dashboard_set_feedback("[FAILURE] Newly created checkpoint was not found in its session browser.");
                log_event(log, "FAIL newly created checkpoint ID %s missing from metadata listing",
                          checkpoint_id);
                continue;
            }
            session.checkpoints[session.selected_checkpoint].metadata = metadata;
            dashboard_set_feedback(chrono_criu_test_backend_enabled() ?
                "[SIMULATION] Fake CRIU checkpoint fixture; not a genuine CRIU checkpoint." :
                "[SUCCESS] Checkpoint metadata recorded; artifacts passed structural validation.");
            log_event(log, "%s checkpoint %s for PID=%ld executable=%s counter=%llu.",
                chrono_criu_test_backend_enabled() ? "SIMULATION" : "PASS",
                checkpoint_id, (long)session.original_pid,
                session.original.executable, session.checkpoint_counter);
            int live_after_dump = chrono_managed_status(session.original_pid,
                session.original_start, error, sizeof(error)) == 0;
            if (!live_after_dump) {
                dashboard_set_feedback("[FAILURE] Checkpoint returned but original process is not verified live.");
                log_event(log, "FAIL original process did not remain live after --leave-running dump");
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT,
                "CHECKPOINT RESULT", "[any key] return to main menu",
                live_after_dump ? dashboard_feedback :
                    "[FAILURE] Original process was not verified running after checkpoint.",
                "[5] PASS — checkpoint record persisted", NULL);
            (void)dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT,
                "CHECKPOINT RESULT", "[any key] return to main menu",
                live_after_dump ? dashboard_feedback :
                    "[FAILURE] Original process was not verified running after checkpoint.",
                "[5] PASS — checkpoint record persisted");
            if (!live_after_dump) session.checkpoint_passed = 0;
        } else if (choice == 6) {
            if (!session.has_original || session.original_stopped ||
                !session.has_selected_checkpoint || !session.checkpoint_passed) {
                dashboard_set_feedback("[BLOCKED] Stop is enabled only after a validated checkpoint exists.");
                continue;
            }
            chrono_checkpoint_metadata selected_metadata;
            const char *selected_id =
                session.checkpoints[session.selected_checkpoint].metadata.id;
            if (!session.checkpoints[session.selected_checkpoint].artifacts_valid ||
                chrono_checkpoint_read(checkpoint_directory, selected_id,
                    &selected_metadata, error, sizeof(error)) != 0 ||
                selected_metadata.state != CHRONO_CHECKPOINT_COMPLETE ||
                !chrono_checkpoint_artifact_valid(checkpoint_directory, selected_id) ||
                !dashboard_menu_checkpoint_associated(&session, &selected_metadata,
                    &session.checkpoints[session.selected_checkpoint])) {
                dashboard_set_feedback("[BLOCKED] Selected checkpoint is no longer valid; original process remains running.");
                log_event(log, "Rejected stop request because selected checkpoint %s failed revalidation.",
                          selected_id);
                continue;
            }
            int confirm = dashboard_menu_read_key(&session,
                CHRONO_DASHBOARD_SCREEN_TERMINATION, "CONFIRM ORIGINAL PROCESS",
                "[y] terminate this process  [n] cancel",
                "No signal is sent before explicit confirmation.", NULL);
            if (confirm != 'y') {
                dashboard_set_feedback("[INFO] Original process termination cancelled.");
                continue;
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_TERMINATION,
                "VERIFYING ORIGINAL IDENTITY", "Explicit confirmation received",
                NULL, "Rechecking PID, start time, UID and executable identity", NULL);
            chrono_process_info before_stop;
            if (chrono_managed_status(session.original_pid, session.original_start,
                    error, sizeof(error)) != 0 ||
                chrono_process_inspect(session.original_pid, &before_stop,
                    error, sizeof(error)) != 0 ||
                !identity_matches(&before_stop, &session.original_identity) ||
                before_stop.owner != session.original_identity.owner) {
                dashboard_set_feedback("[FAILURE] Original identity changed; no termination signal sent.");
                log_event(log, "FAIL termination identity recheck: %s", error);
                continue;
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_TERMINATION,
                "STOPPING ORIGINAL PROCESS", "Verified identity matched",
                NULL, "Sending SIGTERM through the managed-process pidfd path", NULL);
            if (chrono_managed_stop(session.original_pid, session.original_start,
                    error, sizeof(error)) != 0 ||
                wait_original_stop(session.original_pid, session.original_start, log) != 0) {
                dashboard_set_feedback("[FAILURE] The exact managed process was not confirmed stopped and reaped.");
                log_event(log, "FAIL verified termination: %s", error);
                continue;
            }
            session.original_stopped = 1;
            session.termination_passed = 1;
            session.original.state = '-';
            dashboard_set_feedback("[SUCCESS] Original process identity verified, terminated and reaped.");
            log_event(log, "Original PID=%ld START=%llu terminated and reaped after confirmation.",
                (long)session.original_pid, (unsigned long long)session.original_start);
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_TERMINATION,
                "ORIGINAL PROCESS TERMINATED", "[any key] return to main menu",
                dashboard_feedback, "Termination and reaping verified", NULL);
            (void)dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_TERMINATION,
                "ORIGINAL PROCESS TERMINATED", "[any key] return to main menu",
                dashboard_feedback, "Termination and reaping verified");
        } else if (choice == 7) {
            if (dashboard_menu_refresh_checkpoints(&session, error, sizeof(error)) != 0) {
                dashboard_set_feedback("[FAILURE] Checkpoint directory could not be read safely.");
                continue;
            }
            int selected = dashboard_menu_read_choice(&session,
                CHRONO_DASHBOARD_SCREEN_CHECKPOINT_BROWSER, "CHECKPOINT BROWSER",
                "Select a listed checkpoint number, or 0 to return",
                NULL, NULL, 0, (int)session.checkpoint_count);
            if (selected <= 0) continue;
            session.selected_checkpoint = (size_t)(selected - 1);
            session.has_selected_checkpoint = 1;
            chrono_checkpoint_metadata selected_metadata;
            if (!session.checkpoints[session.selected_checkpoint].artifacts_valid ||
                chrono_checkpoint_read(checkpoint_directory,
                    session.checkpoints[session.selected_checkpoint].metadata.id,
                    &selected_metadata, error, sizeof(error)) != 0 ||
                selected_metadata.state != CHRONO_CHECKPOINT_COMPLETE ||
                !dashboard_menu_checkpoint_associated(&session, &selected_metadata,
                    &session.checkpoints[session.selected_checkpoint])) {
                session.has_selected_checkpoint = 0;
                dashboard_set_feedback("[BLOCKED] Selected checkpoint is invalid or not associated with this process.");
                log_event(log, "Rejected checkpoint selection due to metadata/artifact/origin identity mismatch.");
                continue;
            }
            session.checkpoints[session.selected_checkpoint].metadata = selected_metadata;
            dashboard_set_feedback("[SUCCESS] Explicit checkpoint selection recorded; restorability remains unproven.");
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT_BROWSER,
                "CHECKPOINT SELECTED", "[any key] return to main menu",
                dashboard_feedback, NULL, NULL);
            (void)dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_CHECKPOINT_BROWSER,
                "CHECKPOINT SELECTED", "[any key] return to main menu",
                dashboard_feedback, NULL);
        } else if (choice == 8) {
            if (!session.has_selected_checkpoint || !session.original_stopped) {
                dashboard_set_feedback("[BLOCKED] Select a checkpoint and explicitly stop the original first.");
                continue;
            }
            if (session.restore_attempted) {
                dashboard_set_feedback("[BLOCKED] A restore has already been attempted in this session; use verification or the report.");
                continue;
            }
            if (session.checkpoints[session.selected_checkpoint].saved_counter == 0) {
                dashboard_set_feedback("[BLOCKED] Selected checkpoint has no in-session saved counter sample.");
                continue;
            }
            session.checkpoint_counter =
                session.checkpoints[session.selected_checkpoint].saved_counter;
            chrono_checkpoint_metadata selected_metadata;
            const char *selected_id =
                session.checkpoints[session.selected_checkpoint].metadata.id;
            if (chrono_checkpoint_read(checkpoint_directory, selected_id,
                    &selected_metadata, error, sizeof(error)) != 0 ||
                selected_metadata.state != CHRONO_CHECKPOINT_COMPLETE ||
                !chrono_checkpoint_artifact_valid(checkpoint_directory, selected_id) ||
                !dashboard_menu_checkpoint_associated(&session, &selected_metadata,
                    &session.checkpoints[session.selected_checkpoint])) {
                dashboard_set_feedback("[BLOCKED] Selected checkpoint failed pre-restore identity or artifact checks.");
                log_event(log, "Rejected restore selection %s: %s", selected_id, error);
                continue;
            }
            int confirm = dashboard_menu_read_key(&session,
                CHRONO_DASHBOARD_SCREEN_RESTORE, "RESTORE CONFIRMATION",
                "[y] restore this checkpoint  [n] cancel",
                "Review the exact checkpoint and originating executable.", NULL);
            if (confirm != 'y') {
                dashboard_set_feedback("[INFO] Restore cancelled; no CRIU restore was attempted.");
                continue;
            }
            session.restore_attempted = 1;
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_RESTORE,
                "RESTORING SELECTED CHECKPOINT",
                chrono_criu_test_backend_enabled() ?
                    "TEST SIMULATION ONLY; no genuine restore claim" :
                    "Genuine CRIU restore will be verified before adoption",
                NULL, "[1] PASS — selected metadata and image structure validated", NULL);
            int restore_capture = capture_begin(fileno(log), &cap) == 0;
            int restore_ok = restore_capture &&
                chrono_criu_restore(&selected_metadata, checkpoint_directory,
                    &session.restored_pid, error, sizeof(error)) == 0;
            int output_ok = restore_capture && capture_end(&cap) == 0;
            capture_active = 0;
            if (!restore_ok) {
                session.restore_failed = 1;
                session.restore_ambiguous = 1;
                dashboard_set_feedback(chrono_criu_test_backend_enabled() ?
                    "[SIMULATION] Fake restore result rejected; no restored process adopted." :
                    "[FAILURE] CRIU restore did not complete; no process was adopted.");
                log_event(log, "Restore adapter failed or is simulation-only: %s",
                    error[0] != '\0' ? error : "adapter failure");
                continue;
            }
            session.has_restored = 1;
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_RESTORE,
                "VERIFYING RESTORED PROCESS", NULL, NULL,
                chrono_criu_test_backend_enabled() ?
                    "[2] SIMULATION — fake backend returned a candidate\n[3] Rejecting unless live identity verifies" :
                    "[2] PASS — CRIU adapter returned\n[3] Identifying candidate process", NULL);
            int identity_ok = 0;
            for (int attempt = 0; attempt < 20 && !manual_interrupted; ++attempt) {
                if (chrono_process_inspect(session.restored_pid, &session.restored,
                        error, sizeof(error)) == 0 &&
                    identity_matches(&session.restored, &session.original_identity)) {
                    identity_ok = 1;
                    break;
                }
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
                while (nanosleep(&pause, &pause) != 0 && errno == EINTR &&
                       !manual_interrupted) {}
            }
            if (!identity_ok) {
                session.restore_failed = 1;
                session.restore_ambiguous = 1;
                dashboard_set_feedback("[FAILURE] Restored candidate failed independent UID/executable identity verification.");
                log_event(log, "Restored candidate PID=%ld rejected: %s",
                    (long)session.restored_pid, error);
                continue;
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_RESTORE,
                "VERIFYING RESTORED PROCESS", NULL, NULL,
                "[3] PASS — candidate PID/start time independently inspected\n"
                "[4] PASS — UID and executable device/inode match\n"
                "[5] PASS — restored process is alive\n[6] Verifying in-memory counter continuity", NULL);
            unsigned long long resumed = 0;
            int advanced = 0;
            for (int attempt = 0; attempt < 32 && !manual_interrupted; ++attempt) {
                chrono_process_info current;
                if (chrono_process_inspect(session.restored_pid, &current,
                        error, sizeof(error)) != 0 ||
                    current.start_time != session.restored.start_time ||
                !identity_matches(&current, &session.original_identity)) break;
                if (counter_read(session.restored_pid, session.restored.start_time,
                        &resumed) == 0 && resumed > session.checkpoint_counter) {
                    advanced = 1;
                    break;
                }
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
                while (nanosleep(&pause, &pause) != 0 && errno == EINTR &&
                       !manual_interrupted) {}
            }
            if (!advanced || manual_interrupted) {
                session.restore_failed = 1;
                dashboard_set_feedback("[FAILURE] Restored counter did not advance beyond the saved in-memory value.");
                log_event(log, "FAIL counter continuity after restore; saved=%llu.",
                    session.checkpoint_counter);
                continue;
            }
            unsigned long long first_resumed = resumed;
            advanced = 0;
            for (int attempt = 0; attempt < 20 && !manual_interrupted; ++attempt) {
                chrono_process_info current;
                if (chrono_process_inspect(session.restored_pid, &current,
                        error, sizeof(error)) != 0 ||
                    current.start_time != session.restored.start_time ||
                    !identity_matches(&current, &session.original_identity)) break;
                if (counter_read(session.restored_pid, session.restored.start_time,
                        &resumed) == 0 && resumed > first_resumed) {
                    advanced = 1;
                    break;
                }
                struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
                while (nanosleep(&pause, &pause) != 0 && errno == EINTR &&
                       !manual_interrupted) {}
            }
            if (!advanced || manual_interrupted) {
                session.restore_failed = 1;
                dashboard_set_feedback("[FAILURE] Restored counter did not continue advancing after its first resumed sample.");
                log_event(log, "FAIL counter progression after resume: first=%llu.",
                          first_resumed);
                continue;
            }
            session.resumed_counter = resumed;
            session.restore_verified = 1;
            session.counter_continuity_passed = 1;
            session.checkpoints[session.selected_checkpoint].restored = 1;
            if (!output_ok)
                log_event(log, "CRIU restore succeeded but diagnostic capture completion failed.");
            dashboard_set_feedback(chrono_criu_test_backend_enabled() ?
                "[SIMULATION] Test backend identity appeared compatible; this is not genuine CRIU recovery." :
                "[SUCCESS] Restored identity verified and in-memory counter resumed beyond the saved value.");
            log_event(log, "Verified restored PID=%ld START=%llu UID=%lu executable=%s; counter %llu -> %llu.",
                (long)session.restored.pid,
                (unsigned long long)session.restored.start_time,
                (unsigned long)session.restored.owner, session.restored.executable,
                session.checkpoint_counter, resumed);
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_VERIFY,
                "RESTORE VERIFICATION", "[any key] return to main menu",
                dashboard_feedback, "Process identity and counter continuity verified", NULL);
            (void)dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_VERIFY,
                "RESTORE VERIFICATION", "[any key] return to main menu",
                dashboard_feedback, "Process identity and counter continuity verified");
        } else if (choice == 9) {
            if (!session.has_restored) {
                dashboard_set_feedback("[INFO] No restored process candidate is available.");
            } else {
                chrono_process_info current;
                if (chrono_process_inspect(session.restored_pid, &current,
                        error, sizeof(error)) != 0 ||
                    current.pid != session.restored_pid ||
                    current.start_time != session.restored.start_time ||
                    !identity_matches(&current, &session.original_identity)) {
                    session.restore_verified = 0;
                    session.restore_failed = 1;
                    dashboard_set_feedback("[FAILURE] Restored process identity is no longer verified.");
                    log_event(log, "Restored process re-verification failed: %s", error);
                } else {
                    session.restored = current;
                    unsigned long long value;
                    if (counter_read(session.restored_pid, session.restored.start_time,
                                     &value) == 0 && value > session.checkpoint_counter) {
                        session.resumed_counter = value;
                        session.counter_continuity_passed = 1;
                        session.restore_verified = 1;
                        dashboard_set_feedback("[SUCCESS] Live restored process identity and counter progression reverified.");
                    } else {
                        session.restore_failed = 1;
                        session.counter_continuity_passed = 0;
                        dashboard_set_feedback("[FAILURE] Process identity matched, but state continuity was not verified.");
                    }
                }
            }
            dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_VERIFY,
                "RESTORED PROCESS VERIFICATION", "[any key] return to main menu",
                dashboard_feedback, NULL, NULL);
            (void)dashboard_menu_read_key(&session, CHRONO_DASHBOARD_SCREEN_VERIFY,
                "RESTORED PROCESS VERIFICATION", "[any key] return to main menu",
                dashboard_feedback, NULL);
        } else if (choice == 10) {
            dashboard_menu_report(&session);
        } else if (choice == 11) {
            int live_original = session.has_original && !session.original_stopped &&
                chrono_managed_status(session.original_pid, session.original_start,
                                      error, sizeof(error)) == 0;
            chrono_process_info live_restored_info;
            int live_restored = session.has_restored &&
                chrono_process_inspect(session.restored_pid, &live_restored_info,
                                       error, sizeof(error)) == 0 &&
                live_restored_info.start_time == session.restored.start_time &&
                identity_matches(&live_restored_info, &session.original_identity);
            if (live_original || live_restored) {
                char exit_message[256];
                (void)snprintf(exit_message, sizeof(exit_message),
                    "%s%s%s",
                    live_original ? "Original process is still running." : "",
                    live_original && live_restored ? " " : "",
                    live_restored ? "Restored process is still running." : "");
                dashboard_menu_render(&session, CHRONO_DASHBOARD_SCREEN_TERMINATION,
                    "EXIT CONFIRMATION", "[y] exit and clean up this session's process  [n] return",
                    exit_message,
                    "Cleanup will target only a process identity launched/restored by this session.",
                    NULL);
                int confirm = dashboard_menu_read_key(&session,
                    CHRONO_DASHBOARD_SCREEN_TERMINATION, "EXIT CONFIRMATION",
                    "[y] exit and clean up this session's process  [n] return",
                    exit_message, NULL);
                if (confirm != 'y') continue;
                dashboard_set_feedback("[INPUT] Exit and cleanup explicitly confirmed.");
            }
            session.exited = 1;
        }
    }

    if (manual_interrupted) result = 130;
menu_cleanup:
    if (capture_active) (void)capture_end(&cap);
    if (dashboard_started) {
        chrono_dashboard_stop(dashboard_tty_mode);
        dashboard_started = 0;
    }
    if (session.has_original && !session.original_stopped) {
        if (chrono_managed_status(session.original_pid, session.original_start,
                                  error, sizeof(error)) == 0) {
            if (chrono_managed_stop(session.original_pid, session.original_start,
                    error, sizeof(error)) != 0 ||
                wait_original_stop(session.original_pid, session.original_start, log) != 0) {
                cleanup_ok = 0;
                log_event(log, "FAIL cleanup of this session's original workload: %s", error);
            } else {
                session.original_stopped = 1;
                log_event(log, "Cleanup reaped only the bundled process launched by this session.");
            }
        } else if (!original_gone(session.original_pid, session.original_start)) {
            cleanup_ok = 0;
            log_event(log, "Original process identity ambiguous; no cleanup signal sent.");
        }
    }
    if (session.has_restored && session.restore_verified) {
        chrono_process_info current;
        if (chrono_process_inspect(session.restored_pid, &current, error, sizeof(error)) == 0 &&
            current.start_time == session.restored.start_time &&
            identity_matches(&current, &session.original_identity)) {
            if (stop_verified(&current, log) != 0) cleanup_ok = 0;
        } else if (chrono_process_inspect(session.restored_pid, &current, NULL, 0) == 0 &&
                   current.start_time == session.restored.start_time) {
            cleanup_ok = 0;
            log_event(log, "Restored process identity changed; no cleanup signal sent.");
        }
    }
    if (session.restore_ambiguous) {
        cleanup_ok = 0;
        log_event(log, "Restore outcome remains ambiguous; no candidate process was guessed or signaled.");
    }
    if (!cleanup_ok) result = 1;
    if (checkpoint_env && env_restore(&old_checkpoint) != 0) cleanup_ok = 0;
    if (state_env && env_restore(&old_state) != 0) cleanup_ok = 0;
    if (log != NULL) {
        log_event(log, "Dashboard session ended; recovery verified=%s; cleanup=%s.",
            session.restore_verified && session.counter_continuity_passed ? "yes" : "no",
            cleanup_ok ? "PASS" : "UNRESOLVED");
        if (!session.exited && !manual_interrupted)
            (void)fprintf(stderr, "chrono: dashboard input ended; session cleanup was attempted\n");
        fclose(log);
        log = NULL;
    }
    session.log = NULL;
    chrono_dashboard_process_snapshot_free(dashboard_processes);
    dashboard_processes = NULL;
    dashboard_process_count = 0;
    dashboard_inaccessible_processes = 0;
menu_done:
    if (log != NULL) fclose(log);
    if (have_old_checkpoint) free(old_checkpoint.value);
    if (have_old_state) free(old_state.value);
    (void)sigaction(SIGINT, &old_int, NULL);
    (void)sigaction(SIGTERM, &old_term, NULL);
    if (manual_interrupted) return 130;
    return result;
}
