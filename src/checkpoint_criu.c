#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "chrono/checkpoint_criu.h"
#include "chrono/process.h"
#include "chrono/process_control.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <stddef.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>

static void error_text(char *dst, size_t size, const char *text)
{
    if (dst != NULL && size != 0) (void)snprintf(dst, size, "%s", text);
}

static int unprivileged_mode(int *enabled, char *error, size_t size)
{
    const char *value = getenv("CHRONO_CRIU_UNPRIVILEGED");
    if (value == NULL) {
        *enabled = geteuid() != 0;
        return 0;
    }
    if (strcmp(value, "0") == 0) {
        *enabled = 0;
        return 0;
    }
    if (strcmp(value, "1") == 0) {
        *enabled = 1;
        return 0;
    }
    error_text(error, size, "invalid CHRONO_CRIU_UNPRIVILEGED (expected 0 or 1)");
    return -1;
}

int chrono_criu_test_backend_enabled(void)
{
    const char *value = getenv("CHRONO_TEST_ALLOW_FAKE_CRIU");
    return value != NULL && strcmp(value, "1") == 0;
}

int chrono_criu_path_trusted(const char *path)
{
    struct stat st;
    return path != NULL && lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == 0 && (st.st_mode & 0022) == 0 && access(path, X_OK) == 0;
}

static const char *criu_path(char *error, size_t size)
{
    const char *configured = getenv("CHRONO_CRIU_PATH");
    const char *path = configured != NULL && configured[0] != '\0' ? configured : "/usr/sbin/criu";
    const char *test_backend = getenv("CHRONO_TEST_ALLOW_FAKE_CRIU");
    if (test_backend != NULL && strcmp(test_backend, "0") != 0 && strcmp(test_backend, "1") != 0) {
        error_text(error, size, "invalid CHRONO_TEST_ALLOW_FAKE_CRIU (expected 0 or 1)");
        return NULL;
    }
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, X_OK) != 0) {
        error_text(error, size, configured != NULL && configured[0] != '\0' ?
                   "configured CRIU executable is missing, not a regular file, or not executable" :
                   "CRIU executable not found (set CHRONO_CRIU_PATH)");
        return NULL;
    }
    if (!chrono_criu_test_backend_enabled() && !chrono_criu_path_trusted(path)) {
        error_text(error, size, "CRIU executable must be root-owned and not group/world writable; fake backends require explicit test-only mode");
        return NULL;
    }
    return path;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *data = buffer;
    while (length != 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        data += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, void *buffer, size_t length)
{
    unsigned char *data = buffer;
    while (length != 0) {
        ssize_t received = read(fd, data, length);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return -1;
        data += (size_t)received;
        length -= (size_t)received;
    }
    return 0;
}

static int wait_criu_child(pid_t child, const char *operation, int *status,
                           char *error, size_t size)
{
    struct timespec deadline, now;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        (void)kill(child, SIGKILL);
        while (waitpid(child, status, 0) < 0 && errno == EINTR) {}
        error_text(error, size, "cannot read monotonic clock while waiting for CRIU");
        return -1;
    }
    deadline.tv_sec += 120;
    for (;;) {
        pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child) return 0;
        if (waited < 0 && errno != EINTR) {
            char message[160];
            (void)snprintf(message, sizeof(message), "cannot collect CRIU exit status for %s: %s",
                           operation, strerror(errno));
            error_text(error, size, message);
            return -1;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            (void)kill(child, SIGKILL);
            while (waitpid(child, status, 0) < 0 && errno == EINTR) {}
            error_text(error, size, "cannot read monotonic clock while waiting for CRIU");
            return -1;
        }
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) break;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    (void)kill(child, SIGTERM);
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == 0) deadline.tv_sec += 5;
    for (;;) {
        pid_t waited = waitpid(child, status, WNOHANG);
        if (waited == child) break;
        if (waited < 0 && errno != EINTR) break;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec >= deadline.tv_sec) {
            (void)kill(child, SIGKILL);
            while (waitpid(child, status, 0) < 0 && errno == EINTR) {}
            break;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    char message[160];
    (void)snprintf(message, sizeof(message), "CRIU %s exceeded the 120-second deadline and was terminated",
                   operation);
    error_text(error, size, message);
    return -1;
}

static int authorize_workload(const char *endpoint, pid_t target_pid,
                              uint64_t target_start_time)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t name_length = strlen(endpoint);
    if (name_length == 0 || name_length + 1 > sizeof(address.sun_path)) {
        errno = EINVAL;
        return -1;
    }
    address.sun_path[0] = '\0';
    memcpy(address.sun_path + 1, endpoint, name_length);
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_length);
    int fd = -1;
    for (int attempt = 0; attempt < 50; ++attempt) {
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&address, address_length) == 0) break;
        int saved = errno;
        close(fd);
        fd = -1;
        if (saved != ECONNREFUSED && saved != ENOENT && saved != EINTR) {
            errno = saved;
            return -1;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    if (fd < 0) { errno = ETIMEDOUT; return -1; }
    chrono_control_request request = {
        .magic = CHRONO_CONTROL_MAGIC,
        .version = CHRONO_CONTROL_VERSION,
        .operation = CHRONO_CONTROL_AUTHORIZE,
        .target_pid = (int32_t)target_pid,
        .reserved = 0,
        .target_start_time = target_start_time
    };
    ssize_t sent;
    do { sent = send(fd, &request, sizeof(request), MSG_NOSIGNAL); }
    while (sent < 0 && errno == EINTR);
    chrono_control_response response;
    ssize_t received = sent == (ssize_t)sizeof(request) ? recv(fd, &response, sizeof(response), 0) : -1;
    int saved = errno;
    if (received != (ssize_t)sizeof(response) || response.magic != CHRONO_CONTROL_MAGIC ||
        response.reserved != 0 || response.status != 0) {
        if (received == (ssize_t)sizeof(response) && response.error_number > 0)
            saved = response.error_number;
        close(fd);
        errno = saved != 0 ? saved : EPROTO;
        return -1;
    }
    /* EOF confirms the workload closed its accepted and listening sockets
     * before ChronoOS allows CRIU to inspect/dump the target. */
    unsigned char extra;
    do { received = recv(fd, &extra, 1, 0); } while (received < 0 && errno == EINTR);
    close(fd);
    if (received != 0) { errno = EPROTO; return -1; }
    return 0;
}

static int run_criu(const char *operation, const char *path, char *const args[],
                    const char *control_endpoint, pid_t target_pid,
                    uint64_t target_start_time,
                    char *error, size_t size)
{
    int exec_pipe[2] = {-1, -1}, authorization_pipe[2] = {-1, -1}, release_pipe[2] = {-1, -1};
    if (pipe2(exec_pipe, O_CLOEXEC) != 0 ||
        (control_endpoint != NULL &&
         (pipe2(authorization_pipe, O_CLOEXEC) != 0 || pipe2(release_pipe, O_CLOEXEC) != 0))) {
        int pipe_error = errno;
        if (exec_pipe[0] >= 0) { close(exec_pipe[0]); close(exec_pipe[1]); }
        if (authorization_pipe[0] >= 0) { close(authorization_pipe[0]); close(authorization_pipe[1]); }
        if (release_pipe[0] >= 0) { close(release_pipe[0]); close(release_pipe[1]); }
        char message[128];
        (void)snprintf(message, sizeof(message), "cannot create CRIU exec-status pipe: %s", strerror(pipe_error));
        error_text(error, size, message); return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        int fork_error = errno;
        close(exec_pipe[0]); close(exec_pipe[1]);
        if (authorization_pipe[0] >= 0) { close(authorization_pipe[0]); close(authorization_pipe[1]); }
        if (release_pipe[0] >= 0) { close(release_pipe[0]); close(release_pipe[1]); }
        char message[128];
        (void)snprintf(message, sizeof(message), "cannot start CRIU for %s: %s", operation, strerror(fork_error));
        error_text(error, size, message); return -1;
    }
    if (child == 0) {
        close(exec_pipe[0]);
        pid_t parent = getppid();
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) _exit(125);
        (void)umask(0077);
        if (control_endpoint != NULL) {
            close(authorization_pipe[0]);
            close(release_pipe[1]);
            int authorization_error = authorize_workload(control_endpoint, target_pid,
                                                         target_start_time) == 0 ? 0 : errno;
            if (write_all(authorization_pipe[1], &authorization_error,
                          sizeof(authorization_error)) != 0 || authorization_error != 0)
                _exit(125);
            close(authorization_pipe[1]);
            unsigned char release;
            if (read_all(release_pipe[0], &release, 1) != 0 || release != 1) _exit(125);
            close(release_pipe[0]);
        }
        execv(path, args);
        int exec_error = errno;
        const unsigned char *data = (const unsigned char *)&exec_error;
        size_t remaining = sizeof(exec_error);
        while (remaining != 0) {
            ssize_t written = write(exec_pipe[1], data, remaining);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            data += (size_t)written;
            remaining -= (size_t)written;
        }
        _exit(127);
    }
    close(exec_pipe[1]);
    if (control_endpoint != NULL) {
        close(authorization_pipe[1]);
        close(release_pipe[0]);
        struct pollfd authorization_ready = {.fd = authorization_pipe[0], .events = POLLIN};
        int polled;
        do { polled = poll(&authorization_ready, 1, 7000); } while (polled < 0 && errno == EINTR);
        int authorization_error = ETIMEDOUT;
        if (polled > 0 && read_all(authorization_pipe[0], &authorization_error,
                                   sizeof(authorization_error)) != 0) authorization_error = EPIPE;
        close(authorization_pipe[0]);
        if (polled <= 0 || authorization_error != 0) {
            close(release_pipe[1]);
            int child_status;
            while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {}
            close(exec_pipe[0]);
            char message[192];
            (void)snprintf(message, sizeof(message), "managed workload ptrace authorization failed for %s: %s",
                           operation, strerror(authorization_error));
            error_text(error, size, message);
            return -1;
        }
        const unsigned char release = 1;
        if (write_all(release_pipe[1], &release, 1) != 0) {
            int release_error = errno;
            close(release_pipe[1]);
            int child_status;
            while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {}
            close(exec_pipe[0]);
            char message[160];
            (void)snprintf(message, sizeof(message), "cannot release authorized CRIU child for %s: %s",
                           operation, strerror(release_error));
            error_text(error, size, message);
            return -1;
        }
        close(release_pipe[1]);
    }
    int exec_error = 0;
    size_t exec_used = 0;
    int read_error = 0;
    while (exec_used < sizeof(exec_error)) {
        ssize_t count = read(exec_pipe[0], (unsigned char *)&exec_error + exec_used,
                             sizeof(exec_error) - exec_used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { read_error = errno; break; }
        if (count == 0) break;
        exec_used += (size_t)count;
    }
    close(exec_pipe[0]);
    if (read_error != 0) {
        char message[160];
        (void)snprintf(message, sizeof(message), "cannot read CRIU exec-status pipe for %s: %s",
                       operation, strerror(read_error));
        error_text(error, size, message);
        int ignored_status;
        while (waitpid(child, &ignored_status, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    if (exec_used != 0 && exec_used != sizeof(exec_error)) {
        char message[128];
        (void)snprintf(message, sizeof(message),
                       "unexpected partial read from CRIU exec-status pipe for %s (%zd of %zu bytes)",
                       operation, (ssize_t)exec_used, sizeof(exec_error));
        error_text(error, size, message);
        int ignored_status;
        while (waitpid(child, &ignored_status, 0) < 0 && errno == EINTR) {}
        return -1;
    }
    int status;
    if (wait_criu_child(child, operation, &status, error, size) != 0) return -1;
    if (exec_used == sizeof(exec_error)) {
        char message[160];
        (void)snprintf(message, sizeof(message), "cannot execute CRIU for %s: %s", operation,
                       strerror(exec_error));
        error_text(error, size, message); return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    if (WIFSIGNALED(status)) {
        char message[128];
        (void)snprintf(message, sizeof(message), "CRIU %s terminated by signal %d", operation,
                       WTERMSIG(status));
        error_text(error, size, message);
    }
    else {
        char message[96];
        (void)snprintf(message, sizeof(message), "CRIU %s failed (exit status %d)", operation,
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        error_text(error, size, message);
    }
    return -1;
}

static int check_criu(const char *path, int unprivileged, char *error, size_t size)
{
    char *args[] = {(char *)path, "check", "--unprivileged", NULL};
    char *default_args[] = {(char *)path, "check", NULL};
    if (!unprivileged) return run_criu("kernel feature check", path, default_args,
                                       NULL, 0, 0, error, size);
    return run_criu("kernel feature check", path, args, NULL, 0, 0, error, size);
}

int chrono_criu_available(char *error, size_t size)
{
    int unprivileged;
    if (unprivileged_mode(&unprivileged, error, size) != 0) return 0;
    const char *path = criu_path(error, size);
    if (path == NULL) return 0;
    if (check_criu(path, unprivileged, error, size) != 0) return 0;
    return 1;
}

static int artifact_path(const char *base, const char *id, char *path, size_t size)
{
    if (base == NULL || !chrono_checkpoint_id_valid(id)) return -1;
    int n = snprintf(path, size, "%s/%s", base, id);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static int open_private_base(const char *base)
{
    struct stat st;
    int fd = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0777) != 0700) { int saved = errno ? errno : EPERM; close(fd); errno = saved; return -1; }
    return fd;
}

static int safe_artifact_fd(int basefd, const char *id)
{
    struct stat st;
    int fd = openat(basefd, id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0777) != 0700) { int saved = errno ? errno : EPERM; close(fd); errno = saved; return -1; }
    return fd;
}

static int checkpoint_mode(const chrono_checkpoint_metadata *m, const char *base,
                           int leave_running, char *error, size_t size)
{
    char artifact[4096], pid[32];
    int unprivileged;
    if (m == NULL || m->state != CHRONO_CHECKPOINT_PREPARING ||
        m->original_pid <= 0 || m->process_start_time == 0 ||
        artifact_path(base, m->id, artifact, sizeof(artifact)) != 0) {
        error_text(error, size, "invalid checkpoint identity or path"); return -1;
    }
    if (unprivileged_mode(&unprivileged, error, size) != 0) return -1;
    const char *path = criu_path(error, size);
    if (path == NULL) return -1;
    if (check_criu(path, unprivileged, error, size) != 0) return -1;
    char control_endpoint[CHRONO_CONTROL_NAME_MAX];
    if (chrono_managed_control_endpoint(m->original_pid, m->process_start_time,
                                        control_endpoint, sizeof(control_endpoint),
                                        error, (unsigned long)size) != 0) return -1;
    int basefd = open_private_base(base);
    if (basefd < 0) { error_text(error, size, "checkpoint base directory is missing or insecure"); return -1; }
    if (mkdirat(basefd, m->id, 0700) != 0) {
        close(basefd);
        error_text(error, size, "checkpoint artifact directory already exists or cannot be created"); return -1;
    }
    int dirfd = safe_artifact_fd(basefd, m->id);
    if (dirfd < 0) {
        close(basefd);
        error_text(error, size, "new checkpoint artifact directory failed security validation"); return -1;
    }
    int dir_close = close(dirfd);
    int base_close = close(basefd);
    if (dir_close != 0 || base_close != 0) {
        error_text(error, size, "cannot close checkpoint artifact directory safely"); return -1;
    }
    (void)snprintf(pid, sizeof(pid), "%ld", (long)m->original_pid);
    char *args_live[] = {(char *)path, "dump", "--unprivileged", "--tree", pid,
                         "--images-dir", artifact, "--shell-job", "--leave-running", NULL};
    char *args[] = {(char *)path, "dump", "--unprivileged", "--tree", pid,
                    "--images-dir", artifact, "--shell-job", NULL};
    char *default_args_live[] = {(char *)path, "dump", "--tree", pid,
                                 "--images-dir", artifact, "--shell-job", "--leave-running", NULL};
    char *default_args[] = {(char *)path, "dump", "--tree", pid,
                            "--images-dir", artifact, "--shell-job", NULL};
    char **selected_args = unprivileged ? (leave_running ? args_live : args) :
                                         (leave_running ? default_args_live : default_args);
    if (run_criu("dump", path, selected_args,
                 control_endpoint, m->original_pid, m->process_start_time,
                 error, size) != 0) {
        char prior[256], message[384];
        prior[0] = '\0';
        if (error != NULL && size != 0 && error[0] != '\0')
            (void)snprintf(prior, sizeof(prior), "%s", error);
        if (prior[0] == '\0') (void)snprintf(prior, sizeof(prior), "%s", "CRIU dump failed");
        (void)snprintf(message, sizeof(message), "%s; partial artifacts retained for inspection", prior);
        error_text(error, size, message);
        return -1;
    }
    if (!chrono_checkpoint_artifact_valid(base, m->id)) {
        error_text(error, size, "CRIU dump exited successfully but artifacts failed validation"); return -1;
    }
    if (leave_running) {
        chrono_process_info current;
        if (chrono_process_inspect(m->original_pid, &current, error, (unsigned long)size) != 0 ||
            current.start_time != m->process_start_time || current.owner != getuid() ||
            current.state == 'Z' || current.executable[0] == '\0') {
            error_text(error, size, "CRIU dump completed but the original managed workload was not verified running");
            return -1;
        }
    }
    return 0;
}

int chrono_criu_checkpoint(const chrono_checkpoint_metadata *m, const char *base,
                           char *error, size_t size)
{
    return checkpoint_mode(m, base, 0, error, size);
}

int chrono_criu_checkpoint_leave_running(const chrono_checkpoint_metadata *m, const char *base,
                                         char *error, size_t size)
{
    return checkpoint_mode(m, base, 1, error, size);
}

static int criu_restore_locked(const chrono_checkpoint_metadata *m, const char *base,
                               pid_t *restored_pid, char *error, size_t size)
{
    char artifact[4096], pidfile[4096];
    int unprivileged;
    if (m == NULL || m->state != CHRONO_CHECKPOINT_COMPLETE ||
        artifact_path(base, m->id, artifact, sizeof(artifact)) != 0 ||
        !chrono_checkpoint_artifact_valid(base, m->id)) {
        error_text(error, size, "checkpoint metadata is incomplete or artifacts are missing/insecure"); return -1;
    }
    if (unprivileged_mode(&unprivileged, error, size) != 0) return -1;
    const char *path = criu_path(error, size);
    if (path == NULL) return -1;
    if (check_criu(path, unprivileged, error, size) != 0) return -1;
    int n = snprintf(pidfile, sizeof(pidfile), "%s/restore.pid", artifact);
    if (n < 0 || (size_t)n >= sizeof(pidfile)) { error_text(error,size,"checkpoint path too long"); return -1; }
    int basefd = open_private_base(base);
    int artifactfd = basefd < 0 ? -1 : safe_artifact_fd(basefd, m->id);
    struct stat st;
    if (artifactfd < 0 || fstatat(artifactfd, "restore.pid", &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
        if (artifactfd >= 0) close(artifactfd);
        if (basefd >= 0) close(basefd);
        error_text(error, size, "restore already attempted or PID file is invalid"); return -1;
    }
    int attempt_fd = openat(artifactfd, ".restore-attempted", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    int attempt_ok = attempt_fd >= 0;
    if (attempt_ok && (fchmod(attempt_fd, 0600) != 0 || fsync(attempt_fd) != 0)) attempt_ok = 0;
    if (attempt_fd >= 0 && close(attempt_fd) != 0) attempt_ok = 0;
    if (!attempt_ok) {
        close(artifactfd); close(basefd);
        error_text(error, size, "restore already attempted or attempt marker cannot be secured"); return -1;
    }
    int directory_ok = fsync(artifactfd) == 0;
    if (close(artifactfd) != 0) directory_ok = 0;
    if (close(basefd) != 0) directory_ok = 0;
    if (!directory_ok) {
        error_text(error, size, "restore attempt marker exists but its directory could not be synchronized; retry is unsafe"); return -1;
    }
    char *args[] = {(char *)path, "restore", "--unprivileged", "--images-dir", artifact,
                    "--shell-job", "--restore-detached", "--pidfile", pidfile, NULL};
    char *default_args[] = {(char *)path, "restore", "--images-dir", artifact, "--shell-job",
                            "--restore-detached", "--pidfile", pidfile, NULL};
    if (run_criu("restore", path, unprivileged ? args : default_args,
                 NULL, 0, 0, error, size) != 0) {
        char prior[256];
        const char *diagnostic = error != NULL && size != 0 && error[0] != '\0' ?
                                 error : "CRIU restore failed";
        (void)snprintf(prior, sizeof(prior), "%s", diagnostic);
        char message[320];
        (void)snprintf(message, sizeof(message), "%s; partial restoration may have occurred and retry is unsafe", prior);
        error_text(error, size, message);
        return -1;
    }
    int checkbase = open_private_base(base);
    int checkdir = checkbase < 0 ? -1 : safe_artifact_fd(checkbase, m->id);
    int fd = checkdir < 0 ? -1 : openat(checkdir, "restore.pid", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077) != 0 || (st.st_mode & 0400) == 0 || st.st_nlink != 1) {
        if (fd >= 0) close(fd);
        if (checkdir >= 0) close(checkdir);
        if (checkbase >= 0) close(checkbase);
        error_text(error, size, "CRIU reported restore success, but its PID file is missing or unsafe; restoration may have occurred and automatic retry is unsafe"); return -1;
    }
    char text[32]; size_t used = 0; int read_failed = 0;
    while (used < sizeof(text)) {
        ssize_t count = read(fd, text + used, sizeof(text) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { read_failed = 1; break; }
        if (count == 0) break;
        used += (size_t)count;
    }
    int close_result = close(fd);
    int dir_close = close(checkdir), base_close = close(checkbase);
    if (read_failed || used == 0 || used >= sizeof(text) || close_result != 0 || dir_close != 0 || base_close != 0) {
        error_text(error, size, "CRIU reported restore success, but its PID file is empty, oversized, or unreadable; restoration may have occurred and automatic retry is unsafe"); return -1; }
    text[used] = '\0'; char *end; errno = 0; long value = strtol(text, &end, 10);
    if (errno != 0 || value <= 0 || (*end != '\n' && *end != '\0') ||
        (*end == '\n' && end[1] != '\0') || value > 2147483647L ||
        strspn(text, "0123456789") != (size_t)(end - text)) {
        error_text(error, size, "CRIU reported restore success, but its PID file is malformed; restoration may have occurred and automatic retry is unsafe"); return -1;
    }
    if (chrono_criu_test_backend_enabled()) {
        error_text(error, size, "test-only backend restore output cannot be adopted as a restored process");
        return -1;
    }
    struct stat expected_executable;
    if (m->executable[0] == '\0' || stat(m->executable, &expected_executable) != 0 ||
        !S_ISREG(expected_executable.st_mode)) {
        error_text(error, size, "CRIU reported restore success, but checkpoint executable identity is unavailable; restoration may have occurred and retry is unsafe");
        return -1;
    }
    chrono_process_info restored;
    int verified = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        char inspect_error[192];
        if (chrono_process_inspect((pid_t)value, &restored, inspect_error,
                                   sizeof(inspect_error)) == 0 &&
            restored.pid == (pid_t)value && restored.start_time != 0 &&
            (restored.pid != m->original_pid ||
             restored.start_time != m->process_start_time) &&
            restored.owner == getuid() && restored.state != 'Z' &&
            restored.executable_device == expected_executable.st_dev &&
            restored.executable_inode == expected_executable.st_ino &&
            strcmp(restored.executable, m->executable) == 0) {
            verified = 1;
            break;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    if (!verified) {
        error_text(error, size, "CRIU reported restore success, but its PID does not identify a live same-user process with the checkpoint executable; restoration may have occurred and retry is unsafe");
        return -1;
    }
    if (restored_pid != NULL) *restored_pid = (pid_t)value;
    return 0;
}

int chrono_criu_restore(const chrono_checkpoint_metadata *m, const char *base,
                        pid_t *restored_pid, char *error, size_t size)
{
    int lock = chrono_checkpoint_use_lock(base, 0, error, size);
    if (lock < 0) return -1;
    int result = criu_restore_locked(m, base, restored_pid, error, size);
    int saved = errno;
    if (flock(lock, LOCK_UN) != 0 && result == 0) {
        error_text(error, size, "restore succeeded but checkpoint use lock release failed");
        result = -1;
    }
    if (close(lock) != 0 && result == 0) {
        error_text(error, size, "restore succeeded but checkpoint use lock close failed");
        result = -1;
    }
    if (result != 0) errno = saved;
    return result;
}

static int backend_checkpoint(const chrono_checkpoint_metadata *m, const char *directory,
                              char *error, size_t size)
{
    return chrono_criu_checkpoint(m, directory, error, size);
}

static int backend_restore(const chrono_checkpoint_metadata *m, const char *directory,
                          pid_t *pid, char *error, size_t size)
{
    return chrono_criu_restore(m, directory, pid, error, size);
}

static int backend_checkpoint_live(const chrono_checkpoint_metadata *m, const char *directory,
                                   char *error, size_t size)
{
    return chrono_criu_checkpoint_leave_running(m, directory, error, size);
}

int chrono_checkpoint_backend_available(void)
{
    char error[256];
    return chrono_criu_available(error, sizeof(error));
}

const chrono_checkpoint_backend *chrono_checkpoint_backend_get(void)
{
    static chrono_checkpoint_backend backend;
    backend.name = "criu";
    backend.available = chrono_checkpoint_backend_available();
    backend.checkpoint = backend_checkpoint;
    backend.checkpoint_live = backend_checkpoint_live;
    backend.restore = backend_restore;
    return &backend;
}
