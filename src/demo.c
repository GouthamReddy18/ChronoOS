#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "chrono/demo.h"
#include "chrono/process.h"
#include "chrono/checkpoint.h"
#include "chrono/checkpoint_criu.h"
#include "demo_internal.h"

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

static volatile sig_atomic_t interrupted;

static void handle_interrupt(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static void event(FILE *log, const char *format, ...)
{
    char message[1024], timestamp[40];
    struct timespec now;
    struct tm local;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0 && localtime_r(&now.tv_sec, &local) != NULL)
        (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    else
        (void)snprintf(timestamp, sizeof(timestamp), "time-unavailable");
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    (void)fprintf(stdout, "[%s] %s\n", timestamp, message);
    (void)fflush(stdout);
    if (log != NULL) {
        (void)fprintf(log, "[%s] %s\n", timestamp, message);
        (void)fflush(log);
    }
}

static int make_demo_directory(char *directory, size_t size)
{
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') base = "/tmp";
    if (base[0] != '/') return -1;
    int n = snprintf(directory, size, "%s/chronoos-demo-XXXXXX", base);
    if (n < 0 || (size_t)n >= size) return -1;
    if (mkdtemp(directory) == NULL) return -1;
    struct stat st;
    if (chmod(directory, 0700) != 0 || lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0777) != 0700) return -1;
    return 0;
}

static FILE *open_demo_log(const char *directory, char *path, size_t path_size)
{
    int n = snprintf(path, path_size, "%s/demo.log", directory);
    if (n < 0 || (size_t)n >= path_size) return NULL;
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        st.st_nlink != 1 || fchmod(fd, 0600) != 0) {
        close(fd);
        return NULL;
    }
    FILE *log = fdopen(fd, "a");
    if (log == NULL) close(fd);
    return log;
}

typedef struct {
    const char *name;
    char *value;
    int was_set;
} saved_environment;

static int save_environment(saved_environment *saved, const char *name)
{
    const char *value = getenv(name);
    saved->name = name;
    saved->was_set = value != NULL;
    saved->value = value == NULL ? NULL : strdup(value);
    return value == NULL || saved->value != NULL ? 0 : -1;
}

static int restore_environment(saved_environment *saved)
{
    int result = saved->was_set ? setenv(saved->name, saved->value, 1) : unsetenv(saved->name);
    free(saved->value);
    saved->value = NULL;
    return result;
}

static const char *criu_selected_path(void)
{
    const char *configured = getenv("CHRONO_CRIU_PATH");
    return configured != NULL && configured[0] != '\0' ? configured : "/usr/sbin/criu";
}

static int read_criu_version(const char *path, char *version, size_t size)
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
        execv(path, args);
        _exit(127);
    }
    close(pipes[1]);
    size_t used = 0;
    int done = 0, status = 0;
    struct timespec deadline;
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 3;
    while (!done) {
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        int remaining = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
                              (deadline.tv_nsec - now.tv_nsec) / 1000000);
        if (remaining <= 0) break;
        struct pollfd pfd = {.fd = pipes[0], .events = POLLIN | POLLHUP};
        int ready = poll(&pfd, 1, remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        for (;;) {
            char buffer[128];
            ssize_t count = read(pipes[0], buffer, sizeof(buffer));
            if (count > 0) {
                size_t copy = (size_t)count;
                if (copy > size - 1 - used) copy = size - 1 - used;
                if (copy > 0) { memcpy(version + used, buffer, copy); used += copy; }
                continue;
            }
            if (count == 0) done = 1;
            else if (errno != EAGAIN && errno != EINTR) done = 1;
            break;
        }
    }
    close(pipes[0]);
    version[used] = '\0';
    pid_t waited;
    int reaped = 0;
    if (!done) {
        (void)kill(child, SIGTERM);
        for (int attempt = 0; attempt < 10; ++attempt) {
            waited = waitpid(child, &status, WNOHANG);
            if (waited == child) { reaped = 1; break; }
            if (waited < 0 && errno != EINTR) break;
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
            while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
        }
        if (!reaped) (void)kill(child, SIGKILL);
    }
    if (!reaped) do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    else waited = child;
    if (!done || waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || used == 0) {
        version[0] = '\0';
        return -1;
    }
    while (used > 0 && (version[used - 1] == '\n' || version[used - 1] == '\r'))
        version[--used] = '\0';
    return 0;
}

typedef struct {
    int saved_stdout;
    int saved_stderr;
    int log_fd;
    off_t offset;
} output_capture;

static int capture_begin(int log_fd, output_capture *capture)
{
    (void)fflush(stdout);
    (void)fflush(stderr);
    capture->saved_stdout = dup(STDOUT_FILENO);
    capture->saved_stderr = dup(STDERR_FILENO);
    capture->log_fd = log_fd;
    capture->offset = lseek(log_fd, 0, SEEK_END);
    if (capture->saved_stdout < 0 || capture->saved_stderr < 0 || capture->offset < 0) {
        if (capture->saved_stdout >= 0) close(capture->saved_stdout);
        if (capture->saved_stderr >= 0) close(capture->saved_stderr);
        return -1;
    }
    if (dup2(log_fd, STDOUT_FILENO) < 0) {
        close(capture->saved_stdout);
        close(capture->saved_stderr);
        return -1;
    }
    if (dup2(log_fd, STDERR_FILENO) < 0) {
        (void)dup2(capture->saved_stdout, STDOUT_FILENO);
        close(capture->saved_stdout);
        close(capture->saved_stderr);
        return -1;
    }
    return 0;
}

static int capture_end(output_capture *capture)
{
    (void)fflush(stdout);
    (void)fflush(stderr);
    int restore_error = 0;
    if (dup2(capture->saved_stdout, STDOUT_FILENO) != STDOUT_FILENO) restore_error = 1;
    if (dup2(capture->saved_stderr, STDERR_FILENO) != STDERR_FILENO) restore_error = 1;
    close(capture->saved_stdout);
    close(capture->saved_stderr);
    if (lseek(capture->log_fd, capture->offset, SEEK_SET) < 0) return -1;
    char buffer[1024];
    for (;;) {
        ssize_t count = read(capture->log_fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (count == 0) break;
        size_t offset = 0;
        while (offset < (size_t)count) {
            ssize_t written = write(STDOUT_FILENO, buffer + offset, (size_t)count - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return -1;
            offset += (size_t)written;
        }
    }
    (void)lseek(capture->log_fd, 0, SEEK_END);
    return restore_error ? -1 : 0;
}

static int read_counter(const char *path, unsigned long long *counter)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0777) != 0600 || st.st_nlink != 1) { close(fd); return -1; }
    char text[64];
    ssize_t length;
    do { length = read(fd, text, sizeof(text) - 1); } while (length < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    errno = saved;
    if (length <= 0 || length >= (ssize_t)sizeof(text) - 1) return -1;
    text[length] = '\0';
    char *end = text;
    while (*end >= '0' && *end <= '9') ++end;
    if (end == text || (*end != '\n' && *end != '\0') || (*end == '\n' && end[1] != '\0')) return -1;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || (*end != '\n' && *end != '\0')) return -1;
    *counter = value;
    return 0;
}

static int wait_for_counter(const char *path, unsigned long long *value, int seconds)
{
    for (int attempt = 0; attempt <= seconds * 4 && !interrupted; ++attempt) {
        if (read_counter(path, value) == 0) return 0;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !interrupted) {}
    }
    return -1;
}

static int process_is_original_gone(pid_t pid, uint64_t start_time)
{
    chrono_process_info info;
    char error[192];
    if (chrono_managed_status(pid, start_time, error, sizeof(error)) != 0 &&
        strstr(error, "no longer running") != NULL) return 1;
    if (chrono_process_inspect(pid, &info, error, sizeof(error)) != 0) {
        char path[64];
        (void)snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
        return access(path, F_OK) != 0 && errno == ENOENT;
    }
    return info.start_time != start_time || info.state == 'Z';
}

static int stop_original(pid_t pid, uint64_t start_time, FILE *log)
{
    char error[256];
    if (chrono_managed_status(pid, start_time, error, sizeof(error)) == 0) {
        event(log, "Original PID %ld is still running; requesting identity-verified SIGTERM termination.", (long)pid);
        if (chrono_managed_stop(pid, start_time, error, sizeof(error)) != 0) {
            event(log, "FAIL original stop: %s", error);
            return -1;
        }
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (process_is_original_gone(pid, start_time)) {
            /* chrono demo launched this workload as its direct child. Reap
             * that exact child after a successful CRIU dump; otherwise its
             * zombie retains the original PID and CRIU cannot restore it. */
            int child_status = 0;
            pid_t waited;
            do { waited = waitpid(pid, &child_status, WNOHANG); }
            while (waited < 0 && errno == EINTR);
            if (waited == pid) {
                if (!WIFEXITED(child_status) && !WIFSIGNALED(child_status)) {
                    event(log, "FAIL original managed child PID %ld was reaped in an unexpected state.",
                        (long)pid);
                    return -1;
                }
                event(log, "Reaped the demo-owned original child PID %ld after verifying CRIU termination.",
                      (long)pid);
                return 0;
            }
            if (waited < 0 && errno == ECHILD) return 0;
            if (waited < 0) {
                event(log, "FAIL cannot reap the verified demo-owned original child PID %ld: %s",
                      (long)pid, strerror(errno));
                return -1;
            }
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    event(log, "FAIL original PID %ld was not verified terminated; restore is unsafe.", (long)pid);
    return -1;
}

static int restored_still_matches(pid_t pid, uint64_t start, dev_t device, ino_t inode,
                                  const char *command, chrono_process_info *info)
{
    char error[192];
    return chrono_process_inspect(pid, info, error, sizeof(error)) == 0 &&
           info->start_time == start && info->owner == getuid() &&
           info->executable_device == device && info->executable_inode == inode &&
           strcmp(info->command, command) == 0 &&
           info->state != 'Z';
}

static int stop_restored(const chrono_process_info *info, FILE *log)
{
    char error[256];
    if (info == NULL || info->pid <= 1 || info->start_time == 0 || info->executable_inode == 0) return -1;
    if (chrono_process_stop_verified(info->pid, info->start_time, info->executable_device,
                                     info->executable_inode, info->command,
                                     error, sizeof(error)) != 0) {
        event(log, "FAIL cleanup could not verify restored PID %ld: %s; no fallback signal sent.",
              (long)info->pid, error);
        return -1;
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        chrono_process_info current;
        if (chrono_process_inspect(info->pid, &current, error, sizeof(error)) != 0 ||
            current.start_time != info->start_time || current.state == 'Z') return 0;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    event(log, "FAIL verified restored PID %ld did not terminate after SIGTERM.", (long)info->pid);
    return -1;
}

int chrono_demo_run(void)
{
    char directory[PATH_MAX] = {0}, log_path[PATH_MAX] = {0}, state_path[PATH_MAX] = {0};
    char checkpoint_directory[PATH_MAX] = {0}, checkpoint_id[CHRONO_CHECKPOINT_ID_MAX + 1];
    FILE *log = NULL;
    int log_fd = -1, exit_code = 1, launched = 0, restored_owned = 0;
    int checkpoint_metadata_started = 0, original_stopped = 0, completed = 0;
    int restore_ambiguous = 0;
    pid_t original_pid = -1, restored_pid = -1;
    uint64_t original_start = 0, restored_start = 0;
    unsigned long long checkpoint_counter = 0, restored_counter = 0, advancing_counter = 0;
    chrono_process_info original_info, restored_info;
    int have_restored_info = 0;
    chrono_demo_result result = {0};
    struct sigaction action = {0}, old_int, old_term;
    saved_environment saved_checkpoint = {0}, saved_state = {0};
    int checkpoint_env_saved = 0, state_env_saved = 0;
    char error[512] = {0};

    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    interrupted = 0;
    action.sa_handler = handle_interrupt;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int) != 0 || sigaction(SIGTERM, &action, &old_term) != 0) {
        fprintf(stderr, "chrono demo: cannot install safe interruption handlers\n");
        return 1;
    }
    puts("\nChronoOS — Process Checkpoint and Rollback Demonstration\n");
    puts("This demo uses a dedicated managed counter and the configured CRIU adapter.");
    if (chrono_criu_test_backend_enabled())
        puts("TEST SIMULATION ONLY: the selected fake backend cannot create a real CRIU checkpoint or restored process.");
    puts("A successful CRIU preflight does not guarantee that dump or restore will work.\n");
    puts("Stage 1/8 — Initialization");
    struct utsname system;
    if (uname(&system) == 0)
        (void)printf("Operating system: %s %s (%s)\n", system.sysname, system.release, system.machine);
    else
        puts("Operating system: unavailable");
    const char *mode = getenv("CHRONO_CRIU_UNPRIVILEGED");
    const char *mode_name;
    if (mode == NULL || strcmp(mode, "0") == 0) {
        mode_name = "default";
        puts("CRIU mode: default");
    } else if (strcmp(mode, "1") == 0) {
        mode_name = "unprivileged";
        puts("CRIU mode: unprivileged (--unprivileged)");
    } else {
        mode_name = "invalid";
        puts("CRIU mode: invalid CHRONO_CRIU_UNPRIVILEGED value");
    }
    const char *criu_path = criu_selected_path();
    struct stat criu_stat;
    if (!chrono_criu_test_backend_enabled() && chrono_criu_path_trusted(criu_path) &&
        lstat(criu_path, &criu_stat) == 0 && S_ISREG(criu_stat.st_mode) && access(criu_path, X_OK) == 0) {
        char version[160];
        if (read_criu_version(criu_path, version, sizeof(version)) == 0)
            (void)printf("CRIU executable: %s (%s)\n", criu_path, version);
        else
            (void)printf("CRIU executable: %s (version query unavailable)\n", criu_path);
    } else {
        (void)printf("CRIU executable: %s backend at %s\n",
                     chrono_criu_test_backend_enabled() ? "TEST-ONLY untrusted" : "untrusted or unavailable",
                     criu_path);
    }
    if (make_demo_directory(directory, sizeof(directory)) != 0) {
        fprintf(stderr, "chrono demo: could not create a private demo directory\n");
        goto finish;
    }
    log = open_demo_log(directory, log_path, sizeof(log_path));
    if (log == NULL) {
        fprintf(stderr, "chrono demo: could not create a private diagnostic log\n");
        goto finish;
    }
    log_fd = fileno(log);
    int n1 = snprintf(state_path, sizeof(state_path), "%s/counter.state", directory);
    int n2 = snprintf(checkpoint_directory, sizeof(checkpoint_directory), "%s/checkpoints", directory);
    if (n1 < 0 || (size_t)n1 >= sizeof(state_path) || n2 < 0 ||
        (size_t)n2 >= sizeof(checkpoint_directory)) {
        event(log, "FAIL could not prepare isolated demo paths or environment.");
        goto cleanup;
    }
    if (save_environment(&saved_checkpoint, "CHRONO_CHECKPOINT_DIR") != 0) {
        event(log, "FAIL could not preserve the existing checkpoint-directory setting.");
        goto cleanup;
    }
    checkpoint_env_saved = 1;
    if (save_environment(&saved_state, "CHRONO_TEST_STATE_FILE") != 0) {
        event(log, "FAIL could not preserve the existing workload-state setting.");
        goto cleanup;
    }
    state_env_saved = 1;
    if (setenv("CHRONO_CHECKPOINT_DIR", checkpoint_directory, 1) != 0 ||
        setenv("CHRONO_TEST_STATE_FILE", state_path, 1) != 0) {
        event(log, "FAIL could not set isolated demo paths.");
        goto cleanup;
    }
    (void)snprintf(checkpoint_id, sizeof(checkpoint_id), "demo-%ld-%lld",
                   (long)getpid(), (long long)time(NULL));
    event(log, "Private demo directory: %s", directory);
    event(log, "Private checkpoint directory: %s", checkpoint_directory);
    event(log, "Diagnostic log: %s", log_path);
    event(log, "Preflight is a readiness check only; it does not prove process dump or restore.");
    output_capture capture = {0};
    int preflight_log_ok = capture_begin(log_fd, &capture) == 0;
    int preflight = preflight_log_ok ? chrono_criu_available(error, sizeof(error)) : 0;
    int capture_ok = preflight_log_ok && capture_end(&capture) == 0;
    if (!capture_ok) {
        error[0] = '\0';
        (void)snprintf(error, sizeof(error), "could not capture CRIU preflight diagnostics");
    }
    if (preflight && capture_ok) event(log, "CRIU preflight: PASS in selected mode; real workload support remains unverified.");
    else event(log, "CRIU preflight: BLOCKED — %s", error[0] != '\0' ? error : "unknown preflight error");

    if (interrupted) goto cleanup;
    event(log, "Stage 2/8 — Launch managed counter");
    if (chrono_managed_start_bundled(1, &original_pid, &original_start, error, sizeof(error)) != 0) {
        result.launch = -1;
        event(log, "FAIL managed launch: %s", error);
        goto cleanup;
    }
    launched = 1;
    result.launch = 1;
    result.original_pid = original_pid;
    event(log, "Launched managed workload PID %ld, STARTTIME %llu.", (long)original_pid,
          (unsigned long long)original_start);
    error[0] = '\0';
    if (chrono_process_inspect(original_pid, &original_info, error, sizeof(error)) != 0 ||
        original_info.start_time != original_start || original_info.owner != getuid() ||
        original_info.executable[0] == '\0' || original_info.executable_inode == 0) {
        event(log, "FAIL initial process inspection: %s", error[0] != '\0' ? error : "identity mismatch");
        goto cleanup;
    }
    event(log, "Process details: PID=%ld PPID=%ld state=%c executable=%s",
          (long)original_info.pid, (long)original_info.parent_pid, original_info.state,
          original_info.executable);
    if (wait_for_counter(state_path, &checkpoint_counter, 4) != 0) {
        event(log, "FAIL initial application counter was not readable%s.", interrupted ? " before interruption" : "");
        goto cleanup;
    }
    event(log, "Initial application counter: %llu.", checkpoint_counter);

    event(log, "Stage 3/8 — Live monitoring (bounded to four seconds)");
    result.monitoring = -1;
    (void)puts("     PID     PPID  STATE  COUNTER      ELAPSED");
    struct timespec monitor_start;
    (void)clock_gettime(CLOCK_MONOTONIC, &monitor_start);
    int monitor_ok = 1;
    for (int sample = 0; sample < 4 && !interrupted; ++sample) {
        chrono_process_info current;
        unsigned long long value;
        if (chrono_process_inspect(original_pid, &current, error, sizeof(error)) != 0 ||
            current.start_time != original_start || read_counter(state_path, &value) != 0) {
            monitor_ok = 0;
            event(log, "FAIL live monitoring: %s", error[0] != '\0' ? error : "process or counter sample unavailable");
            break;
        }
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - monitor_start.tv_sec) +
                         (double)(now.tv_nsec - monitor_start.tv_nsec) / 1000000000.0;
        (void)printf("%8ld %8ld  %5c  %8llu  %7.2fs\n", (long)current.pid,
                     (long)current.parent_pid, current.state, value, elapsed);
        struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !interrupted) {}
    }
    if (monitor_ok && !interrupted) {
        result.monitoring = 1;
        event(log, "Live monitoring completed with real /proc and workload-state samples.");
    }
    if (interrupted) goto cleanup;

    event(log, "Stage 4/8 — Checkpoint creation");
    result.checkpoint = -1;
    event(log, "Checkpoint ID=%s PID=%ld directory=%s mode=%s.", checkpoint_id,
          (long)original_pid, checkpoint_directory,
          mode_name);
    if (wait_for_counter(state_path, &checkpoint_counter, 2) != 0) {
        event(log, "FAIL could not sample the application counter before checkpoint.");
        goto cleanup;
    }
    result.checkpoint_counter = checkpoint_counter;
    result.checkpoint_sampled = 1;
    result.checkpoint_executable = original_info.executable;
    event(log, "Last persisted application counter sample before dump: %llu.", checkpoint_counter);
    chrono_checkpoint_metadata metadata = {0};
    (void)snprintf(metadata.id, sizeof(metadata.id), "%s", checkpoint_id);
    metadata.original_pid = original_pid;
    metadata.process_start_time = original_start;
    (void)snprintf(metadata.workload, sizeof(metadata.workload), "counter");
    int executable_length = snprintf(metadata.executable, sizeof(metadata.executable), "%s",
                                     original_info.executable);
    if (executable_length < 0 || (size_t)executable_length >= sizeof(metadata.executable)) {
        event(log, "FAIL managed executable path is too long for checkpoint metadata.");
        goto cleanup;
    }
    (void)snprintf(metadata.backend, sizeof(metadata.backend), "criu");
    metadata.state = CHRONO_CHECKPOINT_PREPARING;
    if (chrono_checkpoint_begin(checkpoint_directory, &metadata, error, sizeof(error)) != 0) {
        event(log, "FAIL checkpoint metadata initialization: %s", error);
        goto cleanup;
    }
    checkpoint_metadata_started = 1;
    int process_fd = chrono_managed_pin(original_pid, original_start, error, sizeof(error));
    if (process_fd < 0) {
        event(log, "FAIL managed process identity validation: %s", error);
        (void)chrono_checkpoint_transition(checkpoint_directory, checkpoint_id,
                                          CHRONO_CHECKPOINT_FAILED, error, sizeof(error));
        goto checkpoint_report;
    }
    if (chrono_managed_status(original_pid, original_start, error, sizeof(error)) != 0) {
        close(process_fd);
        event(log, "FAIL managed process changed before dump: %s", error);
        (void)chrono_checkpoint_transition(checkpoint_directory, checkpoint_id,
                                          CHRONO_CHECKPOINT_FAILED, error, sizeof(error));
        goto checkpoint_report;
    }
    event(log, "CRIU dump in progress; waiting for the real adapter result (no synthetic progress).");
    if (capture_begin(log_fd, &capture) != 0) {
        close(process_fd);
        (void)snprintf(error, sizeof(error), "could not capture checkpoint diagnostics");
        (void)chrono_checkpoint_transition(checkpoint_directory, checkpoint_id,
                                          CHRONO_CHECKPOINT_FAILED, error, sizeof(error));
        event(log, "FAIL checkpoint: %s", error);
        goto checkpoint_report;
    }
    int checkpoint_ok = chrono_criu_checkpoint(&metadata, checkpoint_directory, error, sizeof(error)) == 0;
    capture_ok = capture_end(&capture) == 0;
    close(process_fd);
    if (checkpoint_ok && capture_ok &&
        chrono_checkpoint_transition(checkpoint_directory, checkpoint_id, CHRONO_CHECKPOINT_COMPLETE,
                                     error, sizeof(error)) == 0) {
        result.checkpoint = 1;
        event(log, "PASS checkpoint %s; adapter validated the real CRIU artifact layout.", checkpoint_id);
    } else {
        if (checkpoint_ok && capture_ok)
            (void)snprintf(error, sizeof(error), "checkpoint succeeded but metadata completion failed");
        if (!capture_ok && error[0] == '\0')
            (void)snprintf(error, sizeof(error), "could not replay checkpoint diagnostics");
        char transition_error[256] = {0};
        if (chrono_checkpoint_transition(checkpoint_directory, checkpoint_id, CHRONO_CHECKPOINT_FAILED,
                                         transition_error, sizeof(transition_error)) != 0)
            event(log, "FAIL checkpoint: %s (metadata transition also failed: %s)", error, transition_error);
        else
            event(log, "FAIL checkpoint: %s", error[0] != '\0' ? error : "CRIU dump failed");
    }
checkpoint_report:
    if (checkpoint_metadata_started) {
        chrono_checkpoint_metadata observed;
        if (chrono_checkpoint_read(checkpoint_directory, checkpoint_id, &observed,
                                   error, sizeof(error)) == 0) {
            const char *state = observed.state == CHRONO_CHECKPOINT_COMPLETE ? "COMPLETE" :
                                observed.state == CHRONO_CHECKPOINT_FAILED ? "FAILED" : "PREPARING";
            event(log, "Checkpoint metadata state: %s; artifact=%s.", state,
                  chrono_checkpoint_artifact_valid(checkpoint_directory, checkpoint_id) ? "valid" : "missing-or-invalid");
        } else {
            event(log, "Checkpoint metadata status could not be read: %s", error);
        }
    }
    if (result.checkpoint != 1 || interrupted) goto cleanup;

    event(log, "Stage 5/8 — Verify original process termination");
    if (stop_original(original_pid, original_start, log) != 0) {
        result.original_stop = -1;
        goto cleanup;
    }
    original_stopped = 1;
    result.original_stop = 1;
    event(log, "PASS original process identity PID=%ld STARTTIME=%llu was terminated by CRIU dump.",
          (long)original_pid, (unsigned long long)original_start);
    if (interrupted) goto cleanup;

    event(log, "Stage 6/8 — Restore");
    chrono_checkpoint_metadata completed_metadata;
    if (chrono_checkpoint_read(checkpoint_directory, checkpoint_id, &completed_metadata,
                               error, sizeof(error)) != 0 ||
        completed_metadata.state != CHRONO_CHECKPOINT_COMPLETE ||
        !chrono_checkpoint_artifact_valid(checkpoint_directory, checkpoint_id)) {
        event(log, "FAIL restore eligibility: checkpoint is not complete with valid artifacts.");
        goto cleanup;
    }
    event(log, "CRIU restore in progress for checkpoint %s.", checkpoint_id);
    if (capture_begin(log_fd, &capture) != 0) {
        event(log, "FAIL restore: could not preserve CRIU diagnostics; restore was not invoked.");
        goto cleanup;
    }
    result.restore = -1;
    int restore_ok = chrono_criu_restore(&completed_metadata, checkpoint_directory, &restored_pid,
                                         error, sizeof(error)) == 0;
    capture_ok = capture_end(&capture) == 0;
    if (!restore_ok) {
        restore_ambiguous = 1;
        event(log, "FAIL restore: %s. A partial restore may have occurred; no retry or unverified signal will be attempted.",
              error[0] != '\0' ? error : "diagnostic capture failed");
        goto cleanup;
    }
    result.restore = 1;
    result.restored_pid = restored_pid;
    restored_owned = 1;
    event(log, "CRIU adapter reported restored PID %ld.", (long)restored_pid);
    if (!capture_ok)
        event(log, "CRIU restore returned success, but diagnostic replay failed; the retained log is preserved.");

    event(log, "Stage 7/8 — Verify restored process and application state");
    result.executable = -1;
    int verified = 0;
    for (int attempt = 0; attempt < 20 && !interrupted; ++attempt) {
        if (chrono_process_inspect(restored_pid, &restored_info, error, sizeof(error)) == 0 &&
            restored_info.owner == getuid() && restored_info.state != 'Z' &&
            restored_info.executable_device == original_info.executable_device &&
            restored_info.executable_inode == original_info.executable_inode &&
            strcmp(restored_info.command, original_info.command) == 0 &&
            restored_info.start_time != 0) {
            restored_start = restored_info.start_time;
            verified = 1;
            break;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !interrupted) {}
    }
    if (!verified || interrupted) {
        event(log, "FAIL restored PID could not be verified against owner and executable inode: %s",
              error[0] != '\0' ? error : "process is not verifiably live");
        goto cleanup;
    }
    have_restored_info = 1;
    result.executable = 1;
    result.restored_executable = restored_info.executable;
    event(log, "PASS restored PID=%ld STARTTIME=%llu owner=%lu executable=%s.",
          (long)restored_pid, (unsigned long long)restored_start,
          (unsigned long)restored_info.owner, restored_info.executable);
    if (restored_pid == original_pid && restored_start == original_start) {
        event(log, "FAIL restore returned the original process identity; expected the original process to have terminated.");
        result.executable = -1;
        goto cleanup;
    }
    if (!process_is_original_gone(original_pid, original_start)) {
        event(log, "FAIL the original process identity is still live alongside the restored process.");
        result.original_stop = -1;
        goto cleanup;
    }
    for (int attempt = 0; attempt < 32 && !interrupted; ++attempt) {
        unsigned long long current;
        chrono_process_info still_restored;
        if (!restored_still_matches(restored_pid, restored_start, original_info.executable_device,
                                    original_info.executable_inode, original_info.command,
                                    &still_restored)) {
            event(log, "FAIL restored process identity did not remain live during counter verification.");
            goto cleanup;
        }
        if (read_counter(state_path, &current) == 0 && current > checkpoint_counter) {
            restored_counter = current;
            break;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR && !interrupted) {}
    }
    if (interrupted) goto cleanup;
    if (restored_counter <= checkpoint_counter) {
        event(log, "FAIL restored state did not advance beyond the checkpoint counter sample %llu.",
              checkpoint_counter);
        goto cleanup;
    }
    result.counter_continuity = 1;
    result.restored_counter = restored_counter;
    event(log, "PASS counter continuity: restored value %llu is beyond the pre-dump saved sample %llu.",
          restored_counter, checkpoint_counter);
    struct timespec advance_wait = {.tv_sec = 1, .tv_nsec = 250000000};
    while (nanosleep(&advance_wait, &advance_wait) != 0 && errno == EINTR && !interrupted) {}
    if (!interrupted && read_counter(state_path, &advancing_counter) == 0 &&
        advancing_counter > restored_counter &&
        restored_still_matches(restored_pid, restored_start, original_info.executable_device,
                               original_info.executable_inode, original_info.command,
                               &restored_info)) {
        event(log, "PASS restored workload continued from %llu to %llu.",
              restored_counter, advancing_counter);
        result.counter_continuity = 1;
        result.restored_counter = advancing_counter;
    } else {
        result.counter_continuity = -1;
        event(log, "FAIL restored workload did not remain identity-verified while its counter advanced.");
    }

cleanup:
    if (log != NULL) event(log, "Cleanup: checking only the process identities created by this demo.");
    struct sigaction ignore = {0};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    (void)sigaction(SIGINT, &ignore, NULL);
    (void)sigaction(SIGTERM, &ignore, NULL);
    int cleanup_ok = 1;
    if (launched && !original_stopped) {
        if (stop_original(original_pid, original_start, log) == 0) {
            original_stopped = 1;
            if (result.original_stop == 0 && result.checkpoint > 0) result.original_stop = 1;
        } else cleanup_ok = 0;
    }
    if (restored_owned && have_restored_info) {
        if (stop_restored(&restored_info, log) == 0) {
            event(log, "Restored process PID %ld was terminated by verified SIGTERM during demo cleanup.", (long)restored_pid);
        } else cleanup_ok = 0;
    } else if (restored_owned) {
        event(log, "Possible leftover restored process PID %ld: identity was not verified; it was not signaled.",
              (long)restored_pid);
        cleanup_ok = 0;
    }
    result.cleanup = cleanup_ok ? 1 : -1;
    if (restore_ambiguous) {
        result.cleanup = -1;
        event(log, "Cleanup is unresolved: restore failed without a trusted restored PID; no process was signaled.");
    }
    if (checkpoint_env_saved && restore_environment(&saved_checkpoint) != 0) {
        result.cleanup = -1;
        event(log, "FAIL restoring the prior CHRONO_CHECKPOINT_DIR environment value.");
    }
    if (state_env_saved && restore_environment(&saved_state) != 0) {
        result.cleanup = -1;
        event(log, "FAIL restoring the prior CHRONO_TEST_STATE_FILE environment value.");
    }
    if (interrupted) {
        event(log, "Run interrupted; incomplete stages remain NOT ATTEMPTED or FAIL.");
    }
    if (log != NULL) {
        event(log, "Stage 8/8 — Final summary");
        chrono_demo_print_summary(stdout, &result, 0);
        chrono_demo_print_summary(log, &result, 0);
        if (result.checkpoint <= 0)
            event(log, "DEMO INCOMPLETE: no restore was attempted because no valid real checkpoint was produced.");
        else if (result.restore <= 0 || result.original_stop <= 0)
            event(log, "DEMO INCOMPLETE: real restore was not verified.");
        else if (result.executable <= 0 || result.counter_continuity <= 0 || result.cleanup <= 0)
            event(log, "DEMO INCOMPLETE: one or more verification or cleanup stages failed.");
        else {
            completed = 1;
            event(log, "DEMO COMPLETE: real checkpoint, restore, identity, and counter checks passed.");
        }
        event(log, "Run directory and diagnostics retained: %s", directory);
        (void)fflush(log);
    }
    if (log != NULL) { fclose(log); log = NULL; }
finish:
    (void)sigaction(SIGINT, &old_int, NULL);
    (void)sigaction(SIGTERM, &old_term, NULL);
    if (interrupted) return 130;
    if (completed) return 0;
    return exit_code;
}
