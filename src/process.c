#define _GNU_SOURCE
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
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/random.h>

extern char **environ;

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

static int is_counter(pid_t pid, const char *token)
{
    char path[64], data[512];
    (void)snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    size_t n = fread(data, 1, sizeof(data) - 1, f);
    int failed = ferror(f);
    (void)fclose(f);
    if (failed || n == 0) return -1;
    data[n] = '\0';
    size_t args = 0;
    char *arg0 = data, *arg2 = NULL;
    for (size_t i = 0; i < n; ++i) if (data[i] == '\0') {
        ++args;
        if (args == 2) arg2 = data + i + 1;
    }
    char *base = strrchr(arg0, '/');
    base = base == NULL ? data : base + 1;
    return args >= 3 && arg2 != NULL && strcmp(base, "counter_workload") == 0 &&
           strcmp(arg2, token) == 0;
}

/* The directory is opened once per operation; all names below are relative to
 * that descriptor.  A private lock serializes readers with stale cleanup and
 * creation, so a record cannot be replaced between validation and use. */
static int registry_open_locked(int *dirfd_out, int *lockfd_out)
{
    char name[64];
    struct stat st;
    int tmpfd = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (tmpfd < 0) return -1;
    (void)snprintf(name, sizeof(name), "chronoos-%lu", (unsigned long)getuid());
    if (mkdirat(tmpfd, name, 0700) != 0 && errno != EEXIST) { close(tmpfd); return -1; }
    int dirfd = openat(tmpfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(tmpfd);
    if (dirfd < 0) return -1;
    if (fstat(dirfd, &st) != 0) { close(dirfd); return -1; }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0777) != 0700) {
        close(dirfd); errno = EPERM; return -2;
    }
    int lockfd = openat(dirfd, ".lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockfd < 0 || fstat(lockfd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0777) != 0600 || st.st_nlink != 1 ||
        flock(lockfd, LOCK_EX) != 0) {
        if (lockfd >= 0) close(lockfd);
        close(dirfd); errno = EPERM; return -1;
    }
    *dirfd_out = dirfd; *lockfd_out = lockfd;
    return 0;
}

static int registry_read_at(int dirfd, pid_t pid, uint64_t *recorded,
                            struct stat *exe, char token[33])
{
    char name[32], contents[128];
    struct stat st;
    (void)snprintf(name, sizeof(name), "%ld", (long)pid);
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0777) != 0600 || st.st_nlink != 1) { close(fd); errno = EPERM; return -1; }
    size_t length = 0;
    for (;;) {
        if (length == sizeof(contents)) { close(fd); errno = EINVAL; return -1; }
        ssize_t n = read(fd, contents + length, sizeof(contents) - length);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { int saved = errno; close(fd); errno = saved; return -1; }
        if (n == 0) break;
        length += (size_t)n;
    }
    close(fd);
    if (length == 0 || contents[length - 1] != '\n') { errno = EINVAL; return -1; }

    const char *cursor = contents;
    const char *limit = contents + length;
    uintmax_t values[4];
    for (size_t field = 0; field < 4; ++field) {
        if (cursor == limit || *cursor < '0' || *cursor > '9') { errno = EINVAL; return -1; }
        if (*cursor == '0' && cursor + 1 < limit && cursor[1] >= '0' && cursor[1] <= '9') {
            errno = EINVAL; return -1;
        }
        uintmax_t value = 0;
        do {
            unsigned digit = (unsigned)(*cursor - '0');
            if (value > (UINTMAX_MAX - digit) / 10) { errno = ERANGE; return -1; }
            value = value * 10 + digit;
            ++cursor;
        } while (cursor < limit && *cursor >= '0' && *cursor <= '9');
        values[field] = value;
        if (field < 3) {
            if (cursor == limit || *cursor != ' ') { errno = EINVAL; return -1; }
            ++cursor;
        } else {
            if (cursor == limit || *cursor != ' ') { errno = EINVAL; return -1; }
            ++cursor;
        }
    }
    char stored_token[33];
    for (size_t i = 0; i < 32; ++i) {
        if (cursor == limit || !((*cursor >= '0' && *cursor <= '9') ||
                                (*cursor >= 'a' && *cursor <= 'f'))) {
            errno = EINVAL; return -1;
        }
        stored_token[i] = *cursor++;
    }
    stored_token[32] = '\0';
    if (cursor == limit || (size_t)(limit - cursor) != 1 || *cursor != '\n' ||
        (uint64_t)values[0] != values[0] ||
        (dev_t)values[1] != values[1] || (ino_t)values[2] != values[2] ||
        (uid_t)values[3] != values[3]) { errno = EINVAL; return -1; }

    *recorded = (uint64_t)values[0];
    if (exe != NULL) { exe->st_dev = (dev_t)values[1]; exe->st_ino = (ino_t)values[2]; exe->st_uid = (uid_t)values[3]; }
    if (token != NULL) memcpy(token, stored_token, sizeof(stored_token));
    return 0;
}

static int registry_file(pid_t pid, uint64_t start, struct stat *exe, const char *token,
                         int create, char stored_token[33])
{
    int dirfd, lockfd, result = -1;
    char name[32], contents[128];
    if (pid <= 1) return -1;
    int open_result = registry_open_locked(&dirfd, &lockfd);
    if (open_result != 0) return open_result == -2 ? -2 : -1;
    (void)snprintf(name, sizeof(name), "%ld", (long)pid);
    if (create) {
        int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0 && errno == EEXIST) {
            uint64_t old_start;
            char old_token[33];
            if (registry_read_at(dirfd, pid, &old_start, NULL, old_token) != 0) goto done;
            uint64_t current;
            errno = 0;
            int identity_result = read_start_time(pid, &current);
            int identity_error = errno;
            int token_match = identity_result == 0 ? is_counter(pid, old_token) : -1;
            if (identity_result == 0 && current == old_start && token_match == 1) { errno = EEXIST; goto done; }
            /* Only unlink after proving the recorded start time is no longer
             * this PID's identity. If /proc cannot establish that, fail closed. */
            if (identity_result != 0 && identity_error != ENOENT && identity_error != ESRCH) {
                errno = identity_error; goto done;
            }
            if (identity_result == 0 && token_match < 0) { errno = EACCES; goto done; }
            if (unlinkat(dirfd, name, 0) != 0) goto done;
            fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        }
        if (fd < 0) goto done;
        int n = snprintf(contents, sizeof(contents), "%llu %llu %llu %lu %s\n",
                         (unsigned long long)start, (unsigned long long)exe->st_dev,
                         (unsigned long long)exe->st_ino, (unsigned long)exe->st_uid, token);
        ssize_t written = write(fd, contents, (size_t)n);
        int saved = errno;
        if (written != n || fchmod(fd, 0600) != 0) {
            if (written == n) saved = errno;
            close(fd); (void)unlinkat(dirfd, name, 0); errno = saved; goto done;
        }
        close(fd); result = 0;
    } else {
        uint64_t recorded;
        if (registry_read_at(dirfd, pid, &recorded, exe, stored_token) == 0) {
            uint64_t current;
            errno = 0;
            int identity_result = read_start_time(pid, &current);
            int identity_error = errno;
            int token_match = identity_result == 0 ? is_counter(pid, stored_token) : -1;
            if (identity_result == 0 && (current != recorded || token_match == 0)) {
                (void)unlinkat(dirfd, name, 0);
                errno = ESRCH;
            } else if (identity_result != 0 && (identity_error == ENOENT || identity_error == ESRCH)) {
                (void)unlinkat(dirfd, name, 0);
                errno = ESRCH;
            } else if (identity_result == 0 && token_match == 1 && recorded == start) {
                result = 0;
            } else if (identity_result != 0 || token_match < 0) {
                errno = identity_result != 0 ? identity_error : EACCES;
            } else {
                errno = EINVAL;
            }
        }
    }
done:
    close(lockfd); close(dirfd); return result;
}

int chrono_managed_start(const char *counter_path, long interval, pid_t *pid,
                         uint64_t *start_time, char *error, unsigned long error_size)
{
    int pipefd[2], workload_fd;
    struct stat workload_stat;
    unsigned char random_token[16];
    char token[33];
    if (counter_path == NULL || pid == NULL || start_time == NULL || interval < 1 || interval > 60) {
        set_error(error, error_size, "invalid counter launch parameters"); return -1;
    }
    workload_fd = open(counter_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (workload_fd < 0 || fstat(workload_fd, &workload_stat) != 0 || !S_ISREG(workload_stat.st_mode) ||
        workload_stat.st_uid != getuid() || (workload_stat.st_mode & 0111) == 0) {
        if (workload_fd >= 0) close(workload_fd);
        set_error(error, error_size, "bundled counter executable could not be validated"); return -1;
    }
    size_t random_used = 0;
    while (random_used < sizeof(random_token)) {
        ssize_t n = getrandom(random_token + random_used, sizeof(random_token) - random_used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(workload_fd); set_error(error, error_size, "cannot create managed process identity token"); return -1; }
        random_used += (size_t)n;
    }
    for (size_t i = 0; i < sizeof(random_token); ++i)
        (void)snprintf(token + i * 2, sizeof(token) - i * 2, "%02x", random_token[i]);
    if (pipe2(pipefd, O_CLOEXEC) != 0) { close(workload_fd); set_error(error, error_size, "cannot create close-on-exec launch handshake"); return -1; }
    pid_t child = fork();
    if (child < 0) { close(pipefd[0]); close(pipefd[1]); close(workload_fd); set_error(error, error_size, "fork failed"); return -1; }
    if (child == 0) {
        close(pipefd[0]);
        if (setsid() < 0) { int e = errno; ssize_t ignored = write(pipefd[1], &e, sizeof(e)); (void)ignored; _exit(127); }
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd < 0) { int e = errno; ssize_t ignored = write(pipefd[1], &e, sizeof(e)); (void)ignored; _exit(127); }
        (void)dup2(nullfd, STDIN_FILENO); (void)dup2(nullfd, STDOUT_FILENO); (void)dup2(nullfd, STDERR_FILENO);
        if (nullfd > STDERR_FILENO) close(nullfd);
        char interval_arg[16]; (void)snprintf(interval_arg, sizeof(interval_arg), "%ld", interval);
        char *const args[] = {(char *)"counter_workload", interval_arg, token, NULL};
        /* Execute the already validated open inode, avoiding a path replacement race. */
        fexecve(workload_fd, args, environ);
        int e = errno; ssize_t ignored = write(pipefd[1], &e, sizeof(e)); (void)ignored; _exit(127);
    }
    close(pipefd[1]); close(workload_fd);
    int child_error = 0;
    ssize_t got;
    do { got = read(pipefd[0], &child_error, sizeof(child_error)); } while (got < 0 && errno == EINTR);
    close(pipefd[0]);
    if (got != 0) {
        (void)waitpid(child, NULL, 0);
        set_error(error, error_size, "counter process setup or exec failed"); return -1;
    }
    for (int tries = 0; tries < 100; ++tries) {
        if (read_start_time(child, start_time) == 0 && is_counter(child, token)) {
            *pid = child;
            if (registry_file(child, *start_time, &workload_stat, token, 1, NULL) != 0) {
                /* It is still our unreaped direct child, so its PID cannot be
                 * reused while we terminate and reap it. */
                (void)kill(child, SIGTERM);
                (void)waitpid(child, NULL, 0);
                set_error(error, error_size, "cannot record managed process identity"); return -1;
            }
            (void)waitpid(child, NULL, WNOHANG); return 0;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000}; (void)nanosleep(&pause, NULL);
    }
    (void)kill(child, SIGTERM);
    (void)waitpid(child, NULL, 0);
    set_error(error, error_size, "launched counter exited or identity could not be read"); return -1;
}

static int verify_managed(pid_t pid, uint64_t start_time, int pidfd, char *error, unsigned long size)
{
    uint64_t actual;
    struct stat expected, actual_exe;
    struct pollfd pfd = {.fd = pidfd, .events = POLLIN};
    char path[96], line[128], token[33];
    if (pid <= 1 || pidfd < 0 || poll(&pfd, 1, 0) != 0) goto invalid;
    int registry_result = registry_file(pid, start_time, &expected, NULL, 0, token);
    if (registry_result == -2) {
        set_error(error, size, "registry directory ownership or permissions are invalid (expected owned mode 0700)");
        return -1;
    }
    if (registry_result != 0 || read_start_time(pid, &actual) != 0 || actual != start_time) goto invalid;
    (void)snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    if (stat(path, &actual_exe) != 0 || actual_exe.st_dev != expected.st_dev || actual_exe.st_ino != expected.st_ino) goto invalid;
    (void)snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *status = fopen(path, "r");
    if (status == NULL) goto invalid;
    uid_t owner = (uid_t)-1; int got_uid = 0;
    while (fgets(line, sizeof(line), status) != NULL) {
        if (strncmp(line, "Uid:", 4) == 0) { got_uid = chrono_parse_uid_line(line + 4, &owner) == 0; break; }
    }
    fclose(status);
    if (!got_uid || owner != getuid() || owner != expected.st_uid || poll(&pfd, 1, 0) != 0) goto invalid;
    if (!is_counter(pid, token)) goto invalid;
    return 0;
invalid:
        set_error(error, size, "managed process identity does not match a running counter"); return -1;
}

int chrono_managed_status(pid_t pid, uint64_t start_time, char *error, unsigned long size)
{
#ifdef SYS_pidfd_open
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) {
        if (errno == ESRCH) set_error(error, size, "managed process is no longer running");
        else set_error(error, size, "pidfd unavailable; cannot verify process identity");
        return -1;
    }
    int result = verify_managed(pid, start_time, fd, error, size); close(fd); return result;
#else
    (void)pid; (void)start_time; set_error(error, size, "pidfd unavailable; cannot verify process identity"); return -1;
#endif
}

int chrono_managed_stop(pid_t pid, uint64_t start_time, char *error, unsigned long size)
{
#ifdef SYS_pidfd_open
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) { set_error(error, size, "pidfd unavailable; refusing race-prone signal"); return -1; }
    if (verify_managed(pid, start_time, fd, error, size) != 0) { close(fd); return -1; }
#ifdef SYS_pidfd_send_signal
    int result = (int)syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0);
    close(fd);
    if (result != 0) { set_error(error, size, "cannot signal managed counter"); return -1; }
    return 0;
#else
    close(fd); set_error(error, size, "pidfd signal interface unavailable; refusing to signal"); return -1;
#endif
#else
    set_error(error, size, "pidfd unavailable; refusing race-prone signal"); return -1;
#endif
}

int chrono_managed_pin(pid_t pid, uint64_t start_time, char *error, unsigned long size)
{
#ifdef SYS_pidfd_open
    if (pid <= 1 || start_time == 0) {
        set_error(error, size, "invalid managed process identity");
        return -1;
    }
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) {
        set_error(error, size, "pidfd unavailable; cannot pin managed process identity");
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || verify_managed(pid, start_time, fd, error, size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
#else
    (void)pid; (void)start_time;
    set_error(error, size, "pidfd unavailable; cannot pin managed process identity");
    return -1;
#endif
}
