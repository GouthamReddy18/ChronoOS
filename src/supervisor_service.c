#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "chrono/supervisor_service.h"
#include "chrono/supervisor.h"
#include "chrono/checkpoint.h"
#include "chrono/checkpoint_criu.h"
#include "chrono/process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/random.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVICE_SOCKET ".supervisor.sock"
#define SERVICE_LOCK ".supervisor.lock"
#define SERVICE_PROTOCOL_VERSION 1
#define SERVICE_REQUEST_MAX 32
#define SCHEDULER_PROCESS_MAX 32
#define SCHEDULER_WORKER_MAX 4
#define CHECKPOINT_RETENTION_MAX 100

static volatile sig_atomic_t shutdown_requested;

static void set_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size != 0) (void)snprintf(error, size, "%s", message);
}

static void set_errno_error(char *error, size_t size, const char *prefix)
{
    if (error != NULL && size != 0)
        (void)snprintf(error, size, "%s: %s", prefix, strerror(errno));
}

static int socket_path(const char *directory, char *path, size_t size)
{
    int n = snprintf(path, size, "%s/%s", directory, SERVICE_SOCKET);
    if (n < 0 || (size_t)n >= size || (size_t)n >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int send_packet(int fd, const char *message)
{
    size_t size = strlen(message) + 1;
    ssize_t sent;
    do { sent = send(fd, message, size, MSG_NOSIGNAL); }
    while (sent < 0 && errno == EINTR);
    return sent == (ssize_t)size ? 0 : -1;
}

static int receive_packet(int fd, char *buffer, size_t size)
{
    struct iovec iov = {.iov_base = buffer, .iov_len = size};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    ssize_t received;
    do { received = recvmsg(fd, &message, 0); }
    while (received < 0 && errno == EINTR);
    if (received <= 0 || (message.msg_flags & MSG_TRUNC) != 0 ||
        buffer[(size_t)received - 1] != '\0' || memchr(buffer, '\0', (size_t)received - 1) != NULL) {
        errno = received < 0 ? errno : EPROTO;
        return -1;
    }
    return 0;
}

static int reply_to_client(int listener, int *shutdown, char *error, size_t error_size)
{
    int client;
    do { client = accept4(listener, NULL, NULL, SOCK_CLOEXEC); }
    while (client < 0 && errno == EINTR && !shutdown_requested);
    if (client < 0) {
        if (errno == EINTR && shutdown_requested) return 0;
        set_errno_error(error, error_size, "cannot accept supervisor request");
        return -1;
    }
    struct ucred credentials;
    socklen_t credentials_size = sizeof(credentials);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0 ||
        credentials_size != sizeof(credentials) || credentials.uid != getuid()) {
        (void)send_packet(client, "ERROR unauthorized");
        close(client);
        return 0;
    }
    struct pollfd client_ready = {.fd = client, .events = POLLIN};
    int client_polled;
    do { client_polled = poll(&client_ready, 1, 500); }
    while (client_polled < 0 && errno == EINTR);
    if (client_polled <= 0 || (client_ready.revents & POLLIN) == 0) {
        (void)send_packet(client, "ERROR request-timeout");
        close(client);
        return 0;
    }
    char request[SERVICE_REQUEST_MAX];
    int result = 0;
    if (receive_packet(client, request, sizeof(request)) != 0) {
        (void)send_packet(client, "ERROR malformed-request");
    } else if (strcmp(request, "STATUS 1") == 0) {
        char response[96];
        (void)snprintf(response, sizeof(response), "RUNNING pid=%ld protocol=%d",
                       (long)getpid(), SERVICE_PROTOCOL_VERSION);
        result = send_packet(client, response);
    } else if (strcmp(request, "SHUTDOWN 1") == 0) {
        result = send_packet(client, "STOPPING");
        *shutdown = 1;
    } else {
        (void)send_packet(client, "ERROR unsupported-request");
    }
    int saved = errno;
    if (close(client) != 0 && result == 0) { result = -1; saved = errno; }
    if (result != 0) {
        errno = saved;
        set_errno_error(error, error_size, "cannot reply to supervisor request");
        return -1;
    }
    return 0;
}

typedef struct {
    chrono_supervised_process process;
    uint64_t deadline_ns;
    uint64_t last_attempt_ns;
    uint64_t last_success_ns;
    pid_t worker_pid;
    int last_result;
    char checkpoint_id[CHRONO_CHECKPOINT_ID_MAX + 1];
} scheduler_entry;

typedef struct {
    scheduler_entry entries[SCHEDULER_PROCESS_MAX];
    size_t count;
} scheduler_context;

static int monitor_record(const chrono_supervised_process *process, void *opaque)
{
    scheduler_context *context = opaque;
    if (context->count >= SCHEDULER_PROCESS_MAX) return -1;
    scheduler_entry *entry = &context->entries[context->count++];
    memset(entry, 0, sizeof(*entry));
    entry->process = *process;
    return 0;
}

static int scheduler_refresh(scheduler_context *current, const char *directory,
                             char *error, size_t error_size)
{
    scheduler_context fresh = {0};
    if (chrono_supervisor_list(directory, monitor_record, &fresh, error, error_size) != 0)
        return -1;
    for (size_t i = 0; i < fresh.count; ++i) {
        for (size_t j = 0; j < current->count; ++j) {
            if (strcmp(fresh.entries[i].process.id, current->entries[j].process.id) == 0) {
                fresh.entries[i].deadline_ns = current->entries[j].deadline_ns;
                fresh.entries[i].last_attempt_ns = current->entries[j].last_attempt_ns;
                fresh.entries[i].last_success_ns = current->entries[j].last_success_ns;
                fresh.entries[i].worker_pid = current->entries[j].worker_pid;
                fresh.entries[i].last_result = current->entries[j].last_result;
                (void)snprintf(fresh.entries[i].checkpoint_id,
                               sizeof(fresh.entries[i].checkpoint_id), "%s",
                               current->entries[j].checkpoint_id);
                break;
            }
        }
    }
    *current = fresh;
    return 0;
}

static int monotonic_nanoseconds(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (UINT64_MAX - (uint64_t)now.tv_nsec) / UINT64_C(1000000000))
        return -1;
    *value = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return 0;
}

static int checkpoint_root(char *directory, size_t size)
{
    const char *configured = getenv("CHRONO_CHECKPOINT_DIR");
    int n;
    if (configured != NULL && configured[0] != '\0') {
        if (configured[0] != '/') return -1;
        n = snprintf(directory, size, "%s", configured);
    } else {
        n = snprintf(directory, size, "/tmp/chronoos-checkpoints-%lu", (unsigned long)getuid());
    }
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static int checkpoint_retention_limit(size_t *limit)
{
    const char *configured = getenv("CHRONO_CHECKPOINT_RETENTION");
    if (configured == NULL || configured[0] == '\0') {
        *limit = CHRONO_CHECKPOINT_RETENTION_DEFAULT;
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)configured; *p != '\0'; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(configured, &end, 10);
    if (errno != 0 || end == configured || *end != '\0' || value == 0 ||
        value > CHECKPOINT_RETENTION_MAX) return -1;
    *limit = (size_t)value;
    return 0;
}

static int scheduler_start_worker(scheduler_entry *entry, const char *directory,
                                  int listener, int lockfd, int dirfd,
                                  uint64_t now_ns, char *error, size_t error_size)
{
    char id[CHRONO_CHECKPOINT_ID_MAX + 1];
    int n = snprintf(id, sizeof(id), "%s-%016llx", entry->process.id,
                     (unsigned long long)now_ns);
    if (n < 0 || (size_t)n >= sizeof(id) || !chrono_checkpoint_id_valid(id)) {
        set_error(error, error_size, "cannot generate a valid checkpoint identifier");
        return -1;
    }
    pid_t parent = getpid();
    pid_t worker = fork();
    if (worker < 0) { set_errno_error(error, error_size, "cannot start checkpoint worker"); return -1; }
    if (worker == 0) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        (void)sigaction(SIGTERM, &action, NULL);
        (void)sigaction(SIGINT, &action, NULL);
        close(listener); close(lockfd); close(dirfd);
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) _exit(125);
        chrono_checkpoint_metadata metadata;
        memset(&metadata, 0, sizeof(metadata));
        (void)snprintf(metadata.id, sizeof(metadata.id), "%s", id);
        metadata.original_pid = entry->process.pid;
        metadata.process_start_time = entry->process.start_time;
        (void)snprintf(metadata.workload, sizeof(metadata.workload), "%s", entry->process.id);
        if (snprintf(metadata.executable, sizeof(metadata.executable), "%s",
                     entry->process.executable) >= (int)sizeof(metadata.executable)) {
            fprintf(stderr, "chrono: checkpoint %s failed: executable path exceeds metadata limit\n", id);
            _exit(1);
        }
        (void)snprintf(metadata.backend, sizeof(metadata.backend), "criu");
        metadata.state = CHRONO_CHECKPOINT_PREPARING;
        char worker_error[256] = {0};
        if (chrono_checkpoint_begin(directory, &metadata, worker_error, sizeof(worker_error)) != 0) {
            fprintf(stderr, "chrono: checkpoint %s metadata begin failed: %s\n", id, worker_error);
            _exit(1);
        }
        const chrono_checkpoint_backend *backend = chrono_checkpoint_backend_get();
        int result = backend->checkpoint_live == NULL ? -1 :
                     backend->checkpoint_live(&metadata, directory, worker_error, sizeof(worker_error));
        if (result == 0) {
            if (chrono_checkpoint_transition(directory, id, CHRONO_CHECKPOINT_COMPLETE,
                                             worker_error, sizeof(worker_error)) == 0) {
                size_t retention = 0;
                if (chrono_checkpoint_publish_latest(directory, entry->process.id, id,
                                                     worker_error, sizeof(worker_error)) == 0 &&
                    checkpoint_retention_limit(&retention) == 0 &&
                    chrono_checkpoint_retain(directory, entry->process.id, retention,
                                             worker_error, sizeof(worker_error)) == 0)
                    _exit(0);
                if (worker_error[0] == '\0')
                    (void)snprintf(worker_error, sizeof(worker_error),
                                   "invalid CHRONO_CHECKPOINT_RETENTION (expected 1..100)");
                fprintf(stderr, "chrono: checkpoint %s published but latest/retention failed: %s\n",
                        id, worker_error);
                _exit(1);
            }
            fprintf(stderr, "chrono: checkpoint %s publication failed: %s\n", id, worker_error);
        } else {
            if (worker_error[0] == '\0') (void)snprintf(worker_error, sizeof(worker_error), "CRIU live checkpoint failed");
            fprintf(stderr, "chrono: checkpoint %s failed: %s\n", id, worker_error);
        }
        char transition_error[256] = {0};
        if (chrono_checkpoint_transition(directory, id, CHRONO_CHECKPOINT_FAILED,
                                         transition_error, sizeof(transition_error)) != 0)
            fprintf(stderr, "chrono: checkpoint %s failure metadata update failed: %s\n", id, transition_error);
        _exit(1);
    }
    entry->worker_pid = worker;
    entry->last_attempt_ns = now_ns;
    entry->last_result = 0;
    (void)snprintf(entry->checkpoint_id, sizeof(entry->checkpoint_id), "%s", id);
    fprintf(stderr, "chrono: checkpoint start workload=%s checkpoint=%s pid=%ld\n",
            entry->process.id, id, (long)entry->process.pid);
    return 0;
}

static void scheduler_reap_workers(scheduler_context *context)
{
    for (size_t i = 0; i < context->count; ++i) {
        scheduler_entry *entry = &context->entries[i];
        if (entry->worker_pid <= 0) continue;
        int status;
        pid_t waited;
        do { waited = waitpid(entry->worker_pid, &status, WNOHANG); }
        while (waited < 0 && errno == EINTR);
        if (waited == 0) continue;
        if (waited < 0 && errno != ECHILD) {
            fprintf(stderr, "chrono: cannot reap checkpoint worker pid=%ld: %s\n",
                    (long)entry->worker_pid, strerror(errno));
            continue;
        }
        int succeeded = waited == entry->worker_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        entry->last_result = succeeded ? 1 : -1;
        if (succeeded && monotonic_nanoseconds(&entry->last_success_ns) != 0)
            entry->last_success_ns = 0;
        fprintf(stderr, "chrono: checkpoint %s workload=%s result=%s\n",
                entry->checkpoint_id, entry->process.id, succeeded ? "COMPLETE" : "FAILED");
        entry->worker_pid = 0;
        entry->checkpoint_id[0] = '\0';
    }
}

static size_t scheduler_worker_count(const scheduler_context *context)
{
    size_t count = 0;
    for (size_t i = 0; i < context->count; ++i)
        if (context->entries[i].worker_pid > 0) ++count;
    return count;
}

static void scheduler_schedule(scheduler_context *context, const char *directory,
                               int listener, int lockfd, int dirfd, uint64_t now_ns)
{
    scheduler_reap_workers(context);
    for (size_t i = 0; i < context->count; ++i) {
        scheduler_entry *entry = &context->entries[i];
        chrono_supervised_process *process = &entry->process;
        if (entry->deadline_ns == 0) {
            uint64_t interval = (uint64_t)process->checkpoint_interval_seconds * UINT64_C(1000000000);
            entry->deadline_ns = UINT64_MAX - now_ns < interval ? UINT64_MAX : now_ns + interval;
            continue;
        }
        if (entry->worker_pid > 0 || process->state != CHRONO_SUPERVISOR_RUNNING ||
            now_ns < entry->deadline_ns) continue;
        uint64_t interval = (uint64_t)process->checkpoint_interval_seconds * UINT64_C(1000000000);
        /* Skip missed ticks: never queue a burst to catch up after slow dumps. */
        entry->deadline_ns = UINT64_MAX - now_ns < interval ? UINT64_MAX : now_ns + interval;
        if (scheduler_worker_count(context) >= SCHEDULER_WORKER_MAX) continue;
        chrono_process_info current;
        char identity_error[256] = {0};
        if (chrono_process_inspect(process->pid, &current, identity_error, sizeof(identity_error)) != 0 ||
            current.start_time != process->start_time || current.owner != process->owner ||
            current.executable_device != process->executable_device ||
            current.executable_inode != process->executable_inode || current.state == 'Z') {
            fprintf(stderr, "chrono: checkpoint skipped workload=%s: process identity not verified%s%s\n",
                    process->id, identity_error[0] ? ": " : "", identity_error);
            continue;
        }
        char worker_error[256] = {0};
        if (scheduler_start_worker(entry, directory, listener, lockfd, dirfd,
                                   now_ns, worker_error, sizeof(worker_error)) != 0)
            fprintf(stderr, "chrono: checkpoint start failed workload=%s: %s\n",
                    process->id, worker_error);
    }
}

static void signal_shutdown(int signal_number)
{
    (void)signal_number;
    shutdown_requested = 1;
}

int chrono_supervisor_service_run(const char *directory, char *error, size_t error_size)
{
    if (directory == NULL || directory[0] != '/') {
        set_error(error, error_size, "supervisor registry path must be absolute");
        return -1;
    }
    char path[CHRONO_SUPERVISOR_PATH_MAX];
    if (socket_path(directory, path, sizeof(path)) != 0) {
        set_errno_error(error, error_size, "supervisor socket path is invalid");
        return -1;
    }
    int dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) { set_errno_error(error, error_size, "cannot open supervisor registry"); return -1; }
    struct stat directory_stat;
    if (fstat(dirfd, &directory_stat) != 0 || !S_ISDIR(directory_stat.st_mode) ||
        directory_stat.st_uid != getuid() || (directory_stat.st_mode & 0777) != 0700) {
        close(dirfd);
        set_error(error, error_size, "supervisor registry must be owned mode 0700");
        return -1;
    }
    int lockfd = openat(dirfd, SERVICE_LOCK, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockfd < 0) { close(dirfd); set_errno_error(error, error_size, "cannot open supervisor lock"); return -1; }
    struct stat lock_stat;
    if (fstat(lockfd, &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode) ||
        lock_stat.st_uid != getuid() || lock_stat.st_nlink != 1 || (lock_stat.st_mode & 0777) != 0600) {
        close(lockfd); close(dirfd);
        set_error(error, error_size, "supervisor lock has unsafe type, owner, link count, or mode");
        return -1;
    }
    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        close(lockfd); close(dirfd);
        set_error(error, error_size, errno == EWOULDBLOCK ?
                  "another ChronoOS supervisor is already running" : "cannot acquire supervisor lock");
        return -1;
    }
    scheduler_context scheduler = {0};
    char registry_error[256] = {0};
    if (chrono_supervisor_list(directory, monitor_record, &scheduler,
                               registry_error, sizeof(registry_error)) != 0) {
        set_error(error, error_size, registry_error[0] != '\0' ? registry_error :
                  "cannot load and validate registered workloads");
        flock(lockfd, LOCK_UN); close(lockfd); close(dirfd);
        return -1;
    }
    struct stat socket_stat;
    if (fstatat(dirfd, SERVICE_SOCKET, &socket_stat, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISSOCK(socket_stat.st_mode) || socket_stat.st_uid != getuid()) {
            flock(lockfd, LOCK_UN); close(lockfd); close(dirfd);
            set_error(error, error_size, "refusing to remove an unsafe stale supervisor socket");
            return -1;
        }
        if (unlinkat(dirfd, SERVICE_SOCKET, 0) != 0) {
            set_errno_error(error, error_size, "cannot remove stale supervisor socket");
            flock(lockfd, LOCK_UN); close(lockfd); close(dirfd); return -1;
        }
    } else if (errno != ENOENT) {
        set_errno_error(error, error_size, "cannot inspect supervisor socket path");
        flock(lockfd, LOCK_UN); close(lockfd); close(dirfd); return -1;
    }
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener < 0) {
        set_errno_error(error, error_size, "cannot create supervisor socket");
        flock(lockfd, LOCK_UN); close(lockfd); close(dirfd); return -1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(path, 0600) != 0 || listen(listener, 8) != 0) {
        set_errno_error(error, error_size, "cannot bind or secure supervisor socket");
        close(listener); (void)unlinkat(dirfd, SERVICE_SOCKET, 0);
        flock(lockfd, LOCK_UN); close(lockfd); close(dirfd); return -1;
    }

    struct sigaction action, old_int, old_term;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_shutdown;
    sigemptyset(&action.sa_mask);
    shutdown_requested = 0;
    if (sigaction(SIGINT, &action, &old_int) != 0) {
        set_errno_error(error, error_size, "cannot install supervisor shutdown handlers");
        close(listener); (void)unlinkat(dirfd, SERVICE_SOCKET, 0);
        flock(lockfd, LOCK_UN); close(lockfd); close(dirfd); return -1;
    }
    if (sigaction(SIGTERM, &action, &old_term) != 0) {
        set_errno_error(error, error_size, "cannot install supervisor shutdown handlers");
        (void)sigaction(SIGINT, &old_int, NULL);
        close(listener); (void)unlinkat(dirfd, SERVICE_SOCKET, 0);
        flock(lockfd, LOCK_UN); close(lockfd); close(dirfd); return -1;
    }
    fprintf(stderr, "chrono: supervisor running pid=%ld socket=%s\n", (long)getpid(), path);
    char checkpoints[CHRONO_SUPERVISOR_PATH_MAX];
    if (checkpoint_root(checkpoints, sizeof(checkpoints)) != 0) {
        set_error(error, error_size, "checkpoint directory path is invalid or too long");
        close(listener); (void)unlinkat(dirfd, SERVICE_SOCKET, 0);
        (void)sigaction(SIGINT, &old_int, NULL);
        (void)sigaction(SIGTERM, &old_term, NULL);
        flock(lockfd, LOCK_UN); close(lockfd); close(dirfd);
        return -1;
    }
    int failed = 0, stopping = 0;
    while (!shutdown_requested && !stopping) {
        struct pollfd descriptor = {.fd = listener, .events = POLLIN};
        int ready;
        do { ready = poll(&descriptor, 1, 1000); }
        while (ready < 0 && errno == EINTR && !shutdown_requested);
        if (ready < 0) {
            if (errno == EINTR && shutdown_requested) break;
            set_errno_error(error, error_size, "supervisor poll failed"); failed = 1; break;
        }
        if (ready > 0 && (descriptor.revents & POLLIN) != 0 &&
            reply_to_client(listener, &stopping, error, error_size) != 0) {
            fprintf(stderr, "chrono: supervisor request error: %s\n", error != NULL ? error : "unknown");
        }
        char monitor_error[256] = {0};
        if (scheduler_refresh(&scheduler, directory, monitor_error,
                              sizeof(monitor_error)) != 0) {
            /* Registry inspection errors are reported, never treated as exit. */
            fprintf(stderr, "chrono: supervisor registry monitor error: %s\n", monitor_error);
        }
        uint64_t now_ns;
        if (monotonic_nanoseconds(&now_ns) != 0) {
            fprintf(stderr, "chrono: cannot read monotonic clock; checkpoint scheduling paused\n");
            continue;
        }
        scheduler_schedule(&scheduler, checkpoints, listener, lockfd, dirfd, now_ns);
    }
    fprintf(stderr, "chrono: supervisor stopping\n");
    for (size_t i = 0; i < scheduler.count; ++i) {
        pid_t worker = scheduler.entries[i].worker_pid;
        if (worker <= 0) continue;
        (void)kill(worker, SIGTERM);
    }
    for (size_t i = 0; i < scheduler.count; ++i) {
        pid_t worker = scheduler.entries[i].worker_pid;
        if (worker <= 0) continue;
        int status;
        pid_t waited;
        do { waited = waitpid(worker, &status, 0); }
        while (waited < 0 && errno == EINTR);
        if (waited < 0 && errno != ECHILD)
            fprintf(stderr, "chrono: cannot reap checkpoint worker pid=%ld: %s\n",
                    (long)worker, strerror(errno));
        scheduler.entries[i].worker_pid = 0;
    }
    (void)sigaction(SIGINT, &old_int, NULL);
    (void)sigaction(SIGTERM, &old_term, NULL);
    close(listener);
    struct stat final_socket;
    if (fstatat(dirfd, SERVICE_SOCKET, &final_socket, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISSOCK(final_socket.st_mode) && final_socket.st_uid == getuid())
        (void)unlinkat(dirfd, SERVICE_SOCKET, 0);
    flock(lockfd, LOCK_UN);
    close(lockfd);
    close(dirfd);
    return failed ? -1 : 0;
}

int chrono_supervisor_service_request(const char *directory, const char *request,
                                     char *response, size_t response_size,
                                     char *error, size_t error_size)
{
    if (directory == NULL || request == NULL || response == NULL || response_size == 0 ||
        (strcmp(request, "STATUS") != 0 && strcmp(request, "SHUTDOWN") != 0)) {
        set_error(error, error_size, "invalid supervisor request"); return -1;
    }
    char path[CHRONO_SUPERVISOR_PATH_MAX];
    if (socket_path(directory, path, sizeof(path)) != 0) {
        set_errno_error(error, error_size, "supervisor socket path is invalid"); return -1;
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) { set_errno_error(error, error_size, "cannot create supervisor client socket"); return -1; }
    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        set_errno_error(error, error_size, "cannot configure supervisor client timeout");
        close(fd); return -1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        set_errno_error(error, error_size, "cannot connect to supervisor"); close(fd); return -1;
    }
    char message[32];
    (void)snprintf(message, sizeof(message), "%s %d", request, SERVICE_PROTOCOL_VERSION);
    if (send_packet(fd, message) != 0 || receive_packet(fd, response, response_size) != 0) {
        set_errno_error(error, error_size, "supervisor protocol exchange failed"); close(fd); return -1;
    }
    close(fd);
    if (strncmp(response, "ERROR ", 6) == 0) {
        set_error(error, error_size, response); return -1;
    }
    return 0;
}
