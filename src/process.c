#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "chrono/process.h"
#include "chrono/process_control.h"
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
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/random.h>
#include <stddef.h>

extern char **environ;

static void set_error(char *error, unsigned long size, const char *message)
{
    if (error != NULL && size > 0) {
        (void)snprintf(error, (size_t)size, "%s", message);
    }
}

static int child_fds_cloexec(void)
{
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3U, UINT_MAX, 1U << 2) == 0) return 0;
#endif
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0) return -1;
    for (int fd = 3; (long)fd < maximum; ++fd) {
        int flags;
        do { flags = fcntl(fd, F_GETFD); } while (flags < 0 && errno == EINTR);
        if (flags < 0) {
            if (errno == EBADF) continue;
            return -1;
        }
        if ((flags & FD_CLOEXEC) != 0) continue;
        int result;
        do { result = fcntl(fd, F_SETFD, flags | FD_CLOEXEC); }
        while (result < 0 && errno == EINTR);
        if (result < 0) return -1;
    }
    return 0;
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
    int have_parent = 0;
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
        } else if (strncmp(line, "PPid:", 5) == 0) {
            char *end;
            errno = 0;
            long reported = strtol(line + 5, &end, 10);
            while (isspace((unsigned char)*end)) ++end;
            if (!have_parent && errno == 0 && end != line + 5 && *end == '\0' &&
                reported >= 0 && reported <= INT_MAX) {
                info->parent_pid = (pid_t)reported;
                have_parent = 1;
            } else have_parent = -1;
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
    if (!have_pid || !have_uid || !have_state || !have_parent) {
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
    (void)snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    ssize_t executable_length = readlink(path, info->executable, sizeof(info->executable) - 1);
    struct stat executable_stat;
    if (executable_length >= 0 && (size_t)executable_length < sizeof(info->executable) - 1 &&
        stat(path, &executable_stat) == 0) {
        info->executable[executable_length] = '\0';
        info->executable_device = executable_stat.st_dev;
        info->executable_inode = executable_stat.st_ino;
    }
    info->start_time = start_before;
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
    struct stat workload_stat, launcher_stat;
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
    if (stat("/proc/self/exe", &launcher_stat) != 0 || !S_ISREG(launcher_stat.st_mode)) {
        close(workload_fd);
        set_error(error, error_size, "cannot establish ChronoOS executable identity");
        return -1;
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
        if (child_fds_cloexec() != 0) {
            int e = errno;
            ssize_t ignored = write(pipefd[1], &e, sizeof(e));
            (void)ignored;
            _exit(127);
        }
        char interval_arg[16], launcher_device[32], launcher_inode[32];
        (void)snprintf(interval_arg, sizeof(interval_arg), "%ld", interval);
        (void)snprintf(launcher_device, sizeof(launcher_device), "%llu",
                       (unsigned long long)launcher_stat.st_dev);
        (void)snprintf(launcher_inode, sizeof(launcher_inode), "%llu",
                       (unsigned long long)launcher_stat.st_ino);
        char *const args[] = {(char *)"counter_workload", interval_arg, token,
                              launcher_device, launcher_inode, NULL};
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

int chrono_managed_start_bundled(long interval, pid_t *pid, uint64_t *start_time,
                                 char *error, unsigned long error_size)
{
    char self[PATH_MAX], counter[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length < 0 || (size_t)length >= sizeof(self) - 1) {
        set_error(error, error_size, "cannot resolve ChronoOS executable");
        return -1;
    }
    self[length] = '\0';
    char *slash = strrchr(self, '/');
    if (slash == NULL) {
        set_error(error, error_size, "invalid resolved executable path");
        return -1;
    }
    *slash = '\0';
    int n = snprintf(counter, sizeof(counter), "%s/counter_workload", self);
    if (n < 0 || (size_t)n >= sizeof(counter)) {
        set_error(error, error_size, "workload path too long");
        return -1;
    }
    struct stat workload;
    if (lstat(counter, &workload) != 0 || !S_ISREG(workload.st_mode) ||
        workload.st_uid != getuid() || (workload.st_mode & 0111) == 0) {
        set_error(error, error_size, "bundled counter workload is missing or invalid");
        return -1;
    }
    return chrono_managed_start(counter, interval, pid, start_time, error, error_size);
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

int chrono_managed_control_endpoint(pid_t pid, uint64_t start_time, char *name,
                                    size_t name_size, char *error,
                                    unsigned long error_size)
{
    if (name == NULL || name_size == 0 || pid <= 1 || start_time == 0) {
        set_error(error, error_size, "invalid managed control-channel identity");
        return -1;
    }
    int pidfd = chrono_managed_pin(pid, start_time, error, error_size);
    if (pidfd < 0) return -1;
    char token[33];
    struct stat executable;
    int registry_result = registry_file(pid, start_time, &executable, NULL, 0, token);
    close(pidfd);
    if (registry_result != 0 ||
        chrono_control_name(getuid(), pid, start_time, token, name, name_size) != 0) {
        set_error(error, error_size, "cannot derive a verified managed control endpoint");
        return -1;
    }
    return 0;
}

int chrono_managed_counter_query(pid_t pid, uint64_t current_start_time,
                                 uint64_t checkpoint_start_time,
                                 unsigned long long *counter,
                                 char *error, unsigned long error_size)
{
#if defined(SYS_pidfd_open)
    if (pid <= 1 || current_start_time == 0 || checkpoint_start_time == 0 || counter == NULL) {
        set_error(error, error_size, "invalid managed counter query identity");
        return -1;
    }
    int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        set_error(error, error_size, "pidfd unavailable; cannot pin managed counter identity");
        return -1;
    }
    if (fcntl(pidfd, F_SETFD, FD_CLOEXEC) != 0) {
        close(pidfd);
        set_error(error, error_size, "cannot secure managed counter pidfd");
        return -1;
    }

    chrono_process_info before;
    struct pollfd pinned = {.fd = pidfd, .events = POLLIN};
    if (chrono_process_inspect(pid, &before, error, error_size) != 0 ||
        before.start_time != current_start_time || before.owner != getuid() ||
        before.executable_inode == 0 ||
        poll(&pinned, 1, 0) != 0) {
        close(pidfd);
        set_error(error, error_size, "managed counter process identity is not live and verified");
        return -1;
    }

    char command_path[64], command_line[CHRONO_COMMAND_MAX];
    (void)snprintf(command_path, sizeof(command_path), "/proc/%ld/cmdline", (long)pid);
    int command_fd = open(command_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (command_fd < 0) {
        close(pidfd);
        set_error(error, error_size, "cannot inspect managed counter launch token");
        return -1;
    }
    ssize_t command_length;
    do { command_length = read(command_fd, command_line, sizeof(command_line) - 1); }
    while (command_length < 0 && errno == EINTR);
    int command_close = close(command_fd);
    if (command_length <= 0 || command_length >= (ssize_t)sizeof(command_line) - 1 || command_close != 0) {
        close(pidfd);
        set_error(error, error_size, "managed counter launch command is unreadable or oversized");
        return -1;
    }
    command_line[command_length] = '\0';
    char *argument = command_line;
    size_t remaining = (size_t)command_length;
    for (unsigned index = 0; index < 3; ++index) {
        char *terminator = memchr(argument, '\0', remaining);
        if (terminator == NULL) {
            close(pidfd);
            set_error(error, error_size, "managed counter command does not contain its launch token");
            return -1;
        }
        if (index == 0) {
            char *base = strrchr(argument, '/');
            base = base == NULL ? argument : base + 1;
            if (strcmp(base, "counter_workload") != 0) {
                close(pidfd);
                set_error(error, error_size, "process is not the ChronoOS counter workload");
                return -1;
            }
        }
        if (index == 2) {
            size_t token_length = (size_t)(terminator - argument);
            if (token_length != 32) {
                close(pidfd);
                set_error(error, error_size, "managed counter launch token is invalid");
                return -1;
            }
            char token[33];
            memcpy(token, argument, token_length);
            token[token_length] = '\0';
            if (is_counter(pid, token) != 1) {
                close(pidfd);
                set_error(error, error_size, "managed counter launch token did not verify");
                return -1;
            }
            break;
        }
        size_t consumed = (size_t)(terminator - argument) + 1;
        argument = terminator + 1;
        remaining -= consumed;
    }

    char endpoint[CHRONO_CONTROL_NAME_MAX];
    char token[33];
    memcpy(token, argument, 32);
    token[32] = '\0';
    if (chrono_control_name(getuid(), pid, checkpoint_start_time, token,
                            endpoint, sizeof(endpoint)) != 0) {
        close(pidfd);
        set_error(error, error_size, "cannot derive managed counter control endpoint");
        return -1;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t name_length = strlen(endpoint);
    if (name_length + 1 > sizeof(address.sun_path)) {
        close(pidfd);
        set_error(error, error_size, "managed counter control endpoint is too long");
        return -1;
    }
    address.sun_path[0] = '\0';
    memcpy(address.sun_path + 1, endpoint, name_length);
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_length);
    int channel = -1;
    for (int attempt = 0; attempt < 30; ++attempt) {
        channel = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (channel < 0) break;
        if (connect(channel, (struct sockaddr *)&address, address_length) == 0) break;
        int saved = errno;
        close(channel);
        channel = -1;
        if (saved != ECONNREFUSED && saved != ENOENT && saved != EINTR) {
            errno = saved;
            break;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    if (channel < 0) {
        close(pidfd);
        set_error(error, error_size, "managed workload counter query channel is unavailable");
        return -1;
    }
    chrono_control_request request = {
        .magic = CHRONO_CONTROL_MAGIC,
        .version = CHRONO_CONTROL_VERSION,
        .operation = CHRONO_CONTROL_QUERY_COUNTER,
        .target_pid = (int32_t)pid,
        .reserved = 0,
        .target_start_time = checkpoint_start_time
    };
    ssize_t sent;
    do { sent = send(channel, &request, sizeof(request), MSG_NOSIGNAL); }
    while (sent < 0 && errno == EINTR);
    chrono_control_response response = {0};
    ssize_t received = sent == (ssize_t)sizeof(request) ?
                       recv(channel, &response, sizeof(response), 0) : -1;
    int saved = errno;
    struct ucred peer_credentials;
    socklen_t peer_credentials_size = sizeof(peer_credentials);
    int peer_verified = getsockopt(channel, SOL_SOCKET, SO_PEERCRED, &peer_credentials,
                                   &peer_credentials_size) == 0 &&
                        peer_credentials_size == sizeof(peer_credentials) &&
                        peer_credentials.pid == pid && peer_credentials.uid == getuid();
    (void)close(channel);
    int result = -1;
    if (received == (ssize_t)sizeof(response) && response.magic == CHRONO_CONTROL_MAGIC &&
        response.reserved == 0 && response.status == 0 && response.error_number == 0 && peer_verified) {
        chrono_process_info after;
        if (chrono_process_inspect(pid, &after, error, error_size) == 0 &&
            after.start_time == current_start_time && after.owner == getuid() &&
            after.executable_device == before.executable_device &&
            after.executable_inode == before.executable_inode &&
            strcmp(after.command, before.command) == 0 && poll(&pinned, 1, 0) == 0) {
            *counter = response.value;
            result = 0;
        } else {
            set_error(error, error_size, "managed counter process identity changed during query");
        }
    } else {
        if (received == (ssize_t)sizeof(response) && response.error_number > 0)
            saved = response.error_number;
        char message[160];
        (void)snprintf(message, sizeof(message), "managed in-memory counter query failed: %s",
                       strerror(saved != 0 ? saved : peer_verified ? EPROTO : EPERM));
        set_error(error, error_size, message);
    }
    close(pidfd);
    return result;
#else
    (void)pid; (void)current_start_time; (void)checkpoint_start_time; (void)counter;
    set_error(error, error_size, "pidfd unavailable; cannot verify managed counter identity");
    return -1;
#endif
}

int chrono_process_stop_verified(pid_t pid, uint64_t start_time, dev_t executable_device,
                                 ino_t executable_inode, const char *expected_command,
                                 char *error, unsigned long size)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    if (pid <= 1 || start_time == 0 || executable_inode == 0 || expected_command == NULL) {
        set_error(error, size, "invalid process identity; refusing to signal");
        return -1;
    }
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) {
        set_error(error, size, "pidfd unavailable; refusing race-prone signal");
        return -1;
    }
    chrono_process_info info;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (chrono_process_inspect(pid, &info, error, size) != 0 ||
        info.start_time != start_time || info.owner != getuid() ||
        info.executable_device != executable_device || info.executable_inode != executable_inode ||
        strcmp(info.command, expected_command) != 0 || poll(&pfd, 1, 0) != 0) {
        close(fd);
        set_error(error, size, "process identity changed; no signal sent");
        return -1;
    }
    int result = (int)syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0);
    int saved = errno;
    close(fd);
    if (result != 0) {
        errno = saved;
        set_error(error, size, "cannot signal verified restored process");
        return -1;
    }
    return 0;
#else
    (void)pid; (void)start_time; (void)executable_device; (void)executable_inode; (void)expected_command;
    set_error(error, size, "pidfd signaling unavailable; refusing to signal");
    return -1;
#endif
}
