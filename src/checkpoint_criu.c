#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "chrono/checkpoint_criu.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <limits.h>
#include <unistd.h>

static void error_text(char *dst, size_t size, const char *text)
{
    if (dst != NULL && size != 0) (void)snprintf(dst, size, "%s", text);
}

static const char *criu_path(char *error, size_t size)
{
    const char *configured = getenv("CHRONO_CRIU_PATH");
    const char *path = configured != NULL && configured[0] != '\0' ? configured : "/usr/sbin/criu";
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, X_OK) != 0) {
        error_text(error, size, configured != NULL && configured[0] != '\0' ?
                   "configured CRIU executable is missing, not a regular file, or not executable" :
                   "CRIU executable not found (set CHRONO_CRIU_PATH)");
        return NULL;
    }
    return path;
}

static int run_criu(const char *operation, const char *path, char *const args[],
                    char *error, size_t size)
{
    int exec_pipe[2];
    if (pipe2(exec_pipe, O_CLOEXEC) != 0) {
        int pipe_error = errno;
        char message[128];
        (void)snprintf(message, sizeof(message), "cannot create CRIU exec-status pipe: %s", strerror(pipe_error));
        error_text(error, size, message); return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        int fork_error = errno;
        close(exec_pipe[0]); close(exec_pipe[1]);
        char message[128];
        (void)snprintf(message, sizeof(message), "cannot start CRIU for %s: %s", operation, strerror(fork_error));
        error_text(error, size, message); return -1;
    }
    if (child == 0) {
        close(exec_pipe[0]);
        (void)umask(0077);
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
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        char message[128];
        (void)snprintf(message, sizeof(message), "cannot collect CRIU exit status for %s: %s",
                       operation, strerror(errno));
        error_text(error, size, message); return -1;
    }
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

static int check_criu(const char *path, char *error, size_t size)
{
    char *args[] = {(char *)path, "check", NULL};
    return run_criu("kernel feature check", path, args, error, size);
}

int chrono_criu_available(char *error, size_t size)
{
    const char *path = criu_path(error, size);
    if (path == NULL) return 0;
    if (check_criu(path, error, size) != 0) return 0;
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

int chrono_criu_checkpoint(const chrono_checkpoint_metadata *m, const char *base,
                           char *error, size_t size)
{
    char artifact[4096], pid[32];
    if (m == NULL || m->state != CHRONO_CHECKPOINT_PREPARING ||
        m->original_pid <= 0 || m->process_start_time == 0 ||
        artifact_path(base, m->id, artifact, sizeof(artifact)) != 0) {
        error_text(error, size, "invalid checkpoint identity or path"); return -1;
    }
    const char *path = criu_path(error, size);
    if (path == NULL) return -1;
    if (check_criu(path, error, size) != 0) return -1;
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
    char *args[] = {(char *)path, "dump", "--tree", pid, "--images-dir", artifact,
                    "--shell-job", NULL};
    if (run_criu("dump", path, args, error, size) != 0) {
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
    return 0;
}

int chrono_criu_restore(const chrono_checkpoint_metadata *m, const char *base,
                        pid_t *restored_pid, char *error, size_t size)
{
    char artifact[4096], pidfile[4096];
    if (m == NULL || m->state != CHRONO_CHECKPOINT_COMPLETE ||
        artifact_path(base, m->id, artifact, sizeof(artifact)) != 0 ||
        !chrono_checkpoint_artifact_valid(base, m->id)) {
        error_text(error, size, "checkpoint metadata is incomplete or artifacts are missing/insecure"); return -1;
    }
    const char *path = criu_path(error, size);
    if (path == NULL) return -1;
    if (check_criu(path, error, size) != 0) return -1;
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
    char *args[] = {(char *)path, "restore", "--images-dir", artifact, "--shell-job",
                    "--restore-detached", "--pidfile", pidfile, NULL};
    if (run_criu("restore", path, args, error, size) != 0) {
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
    if (restored_pid != NULL) *restored_pid = (pid_t)value;
    return 0;
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
    backend.restore = backend_restore;
    return &backend;
}
