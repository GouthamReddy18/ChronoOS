#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "chrono/process_control.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int publish_counter_name(unsigned long long counter)
{
    char name[16];
    if (counter > 9999999999999ULL)
        (void)snprintf(name, sizeof(name), "c:overflow");
    else
        (void)snprintf(name, sizeof(name), "c:%013llu", counter);
    return prctl(PR_SET_NAME, name, 0, 0, 0);
}

static int valid_identity_token(const char *token)
{
    if (token == NULL || strnlen(token, 33) != 32) return 0;
    for (size_t i = 0; i < 32; ++i) {
        if (!((token[i] >= '0' && token[i] <= '9') ||
              (token[i] >= 'a' && token[i] <= 'f'))) return 0;
    }
    return 1;
}

static int process_start_time(pid_t pid, uint64_t *value)
{
    char path[64], *line = NULL;
    size_t capacity = 0;
    (void)snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (file == NULL) return -1;
    ssize_t length = getline(&line, &capacity, file);
    int failed = ferror(file);
    (void)fclose(file);
    if (length < 0 || failed) { free(line); return -1; }
    char *cursor = strrchr(line, ')');
    if (cursor == NULL || cursor[1] != ' ') { free(line); return -1; }
    cursor += 2;
    for (unsigned field = 3; field <= 22; ++field) {
        while (*cursor == ' ') ++cursor;
        char *begin = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\n') ++cursor;
        if (cursor == begin) { free(line); return -1; }
        if (field == 22) {
            char saved = *cursor;
            *cursor = '\0';
            errno = 0;
            char *end = NULL;
            unsigned long long parsed = strtoull(begin, &end, 10);
            int valid = errno == 0 && end != begin && *end == '\0' && parsed != 0;
            *cursor = saved;
            free(line);
            if (!valid) return -1;
            *value = (uint64_t)parsed;
            return 0;
        }
    }
    free(line);
    return -1;
}

static int make_control_listener(uid_t owner, pid_t pid, uint64_t start_time,
                                 const char token[33])
{
    char name[CHRONO_CONTROL_NAME_MAX];
    if (chrono_control_name(owner, pid, start_time, token, name, sizeof(name)) != 0)
        return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t length = strlen(name);
    if (length + 1 > sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    address.sun_path[0] = '\0';
    memcpy(address.sun_path + 1, name, length);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + length);
    if (bind(fd, (struct sockaddr *)&address, address_length) != 0 || listen(fd, 1) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int send_control_response(int fd, int status, int error_number,
                                 unsigned long long counter_value)
{
    chrono_control_response response = {
        .magic = CHRONO_CONTROL_MAGIC,
        .status = status,
        .error_number = error_number,
        .reserved = 0,
        .value = counter_value
    };
    ssize_t sent;
    do { sent = send(fd, &response, sizeof(response), MSG_NOSIGNAL); }
    while (sent < 0 && errno == EINTR);
    return sent == (ssize_t)sizeof(response) ? 0 : -1;
}

/* Returns 0 for no request, 1 after servicing an authorization, and -1 on
 * an unrecoverable control-channel error. The target closes every channel FD
 * before acknowledging, so CRIU does not inherit ChronoOS control sockets. */
static int service_control(int *listener, uid_t owner, pid_t self,
                           uint64_t self_start, dev_t launcher_device,
                           ino_t launcher_inode, pid_t *tracer,
                           uint64_t *tracer_start, const char token[33],
                           unsigned long long *counter_value)
{
    struct pollfd ready = {.fd = *listener, .events = POLLIN};
    int polled;
    do { polled = poll(&ready, 1, 0); } while (polled < 0 && errno == EINTR);
    if (polled <= 0 || (ready.revents & POLLIN) == 0) return polled < 0 ? -1 : 0;
    int client = accept4(*listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;

    chrono_control_request request;
    struct ucred credentials;
    socklen_t credentials_length = sizeof(credentials);
    struct pollfd client_ready = {.fd = client, .events = POLLIN};
    int client_polled;
    do { client_polled = poll(&client_ready, 1, 2000); }
    while (client_polled < 0 && errno == EINTR);
    ssize_t received = client_polled > 0 && (client_ready.revents & POLLIN) != 0 ?
                       recv(client, &request, sizeof(request), MSG_TRUNC) : -1;
    int valid = received == (ssize_t)sizeof(request) &&
                getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials,
                           &credentials_length) == 0 &&
                credentials_length == sizeof(credentials) &&
                credentials.pid > 1 && credentials.uid == owner &&
                request.magic == CHRONO_CONTROL_MAGIC &&
                request.version == CHRONO_CONTROL_VERSION &&
                (request.operation == CHRONO_CONTROL_AUTHORIZE ||
                 request.operation == CHRONO_CONTROL_QUERY_COUNTER) &&
                request.target_pid == self && request.target_start_time == self_start &&
                request.reserved == 0;
    struct stat peer_executable;
    char peer_path[64];
    if (valid) {
        (void)snprintf(peer_path, sizeof(peer_path), "/proc/%ld/exe", (long)credentials.pid);
        valid = stat(peer_path, &peer_executable) == 0 &&
                peer_executable.st_dev == launcher_device &&
                peer_executable.st_ino == launcher_inode;
    }
    uint64_t peer_start = 0;
    if (valid) valid = process_start_time(credentials.pid, &peer_start) == 0;
    if (valid && request.operation == CHRONO_CONTROL_QUERY_COUNTER) {
        int response_result = send_control_response(client, 0, 0,
                                                    counter_value != NULL ? *counter_value : 0);
        int close_result = close(client);
        return response_result == 0 && close_result == 0 ? 1 : -1;
    }
    int auth_error = valid ? 0 : EPERM;
    if (valid && prctl(PR_SET_PTRACER, (unsigned long)credentials.pid, 0, 0, 0) != 0)
        auth_error = errno;
    if (auth_error == 0) {
        /* Stop accepting concurrent requests and remove the listener from the
         * target before the CRIU child is released. */
        int old_listener = *listener;
        *listener = -1;
        if (close(old_listener) != 0) auth_error = errno;
    }
    (void)send_control_response(client, auth_error == 0 ? 0 : -1, auth_error, 0);
    int close_result = close(client);
    if (close_result != 0 && auth_error == 0) auth_error = errno;
    if (auth_error != 0) {
        if (valid) (void)prctl(PR_SET_PTRACER, 0, 0, 0, 0);
        if (*listener < 0) *listener = make_control_listener(owner, self, self_start, token);
        return *listener < 0 ? -1 : 0;
    }

    *tracer = credentials.pid;
    *tracer_start = peer_start;
    while (!stop_requested) {
        uint64_t current_start = 0;
        if (process_start_time(*tracer, &current_start) != 0 || current_start != *tracer_start)
            break;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    /* A stopped/frozen target reaches this only when the dump resumes it. */
    if (prctl(PR_SET_PTRACER, 0, 0, 0, 0) != 0) return -1;
    *tracer = 0;
    *tracer_start = 0;
    *listener = make_control_listener(owner, self, self_start, token);
    return *listener < 0 ? -1 : 1;
}

int main(int argc, char **argv)
{
    long interval = 1;
    if (argc != 1 && argc != 2 && argc != 3 && argc != 5) {
        fprintf(stderr, "Usage: %s [interval-seconds 1..60 [identity-token [launcher-device launcher-inode]]]\n", argv[0]);
        return 2;
    }
    if (argc >= 2) {
        char *end = NULL;
        errno = 0;
        interval = strtol(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || interval < 1 || interval > 60) {
            fprintf(stderr, "counter_workload: interval must be an integer from 1 to 60\n");
            return 2;
        }
    }
    if ((argc == 3 || argc == 5) && !valid_identity_token(argv[2])) {
        fprintf(stderr, "counter_workload: identity token must be exactly 32 lowercase hexadecimal characters\n");
        return 2;
    }

    char identity_token[33] = {0};
    if (argc == 3 || argc == 5) memcpy(identity_token, argv[2], sizeof(identity_token));
    uint64_t own_start_time = 0;
    struct stat launcher_executable;
    memset(&launcher_executable, 0, sizeof(launcher_executable));
    if (argc == 3 || argc == 5) {
        int launcher_valid = 0;
        if (argc == 5) {
            char *device_end = NULL, *inode_end = NULL;
            errno = 0;
            unsigned long long device = strtoull(argv[3], &device_end, 10);
            int device_ok = errno == 0 && device_end != argv[3] && *device_end == '\0' &&
                            (dev_t)device == device;
            errno = 0;
            unsigned long long inode = strtoull(argv[4], &inode_end, 10);
            int inode_ok = errno == 0 && inode_end != argv[4] && *inode_end == '\0' &&
                           (ino_t)inode == inode;
            if (device_ok && inode_ok && inode != 0) {
                launcher_executable.st_dev = (dev_t)device;
                launcher_executable.st_ino = (ino_t)inode;
                launcher_valid = 1;
            }
        } else {
            /* Compatibility for manually launched token-bearing workloads. */
            pid_t launcher_pid = getppid();
            char launcher_path[64];
            (void)snprintf(launcher_path, sizeof(launcher_path), "/proc/%ld/exe", (long)launcher_pid);
            launcher_valid = stat(launcher_path, &launcher_executable) == 0 &&
                             S_ISREG(launcher_executable.st_mode);
        }
        if (!launcher_valid || process_start_time(getpid(), &own_start_time) != 0) {
        fprintf(stderr, "counter_workload: cannot establish launch identity for control channel\n");
        return 1;
        }
    }
    int control_listener = -1;
    if (argc == 3 || argc == 5) {
        control_listener = make_control_listener(getuid(), getpid(), own_start_time, identity_token);
        if (control_listener < 0) {
            perror("counter_workload: private control channel");
            return 1;
        }
    }

    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        perror("counter_workload: sigaction");
        return 1;
    }

    unsigned long long counter = 0;
    const char *state_path = getenv("CHRONO_TEST_STATE_FILE");
    int state_fd = -1;
    if (state_path != NULL && state_path[0] != '\0') {
        struct stat state_stat;
        state_fd = open(state_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (state_fd < 0 || fstat(state_fd, &state_stat) != 0 || !S_ISREG(state_stat.st_mode) ||
            state_stat.st_uid != getuid() || fchmod(state_fd, 0600) != 0) {
            if (state_fd >= 0) (void)close(state_fd);
            perror("counter_workload: test state file");
            return 1;
        }
    }
    while (!stop_requested) {
        if (publish_counter_name(counter) != 0) {
            perror("counter_workload: publish counter");
            if (state_fd >= 0) close(state_fd);
            if (control_listener >= 0) close(control_listener);
            return 1;
        }
        if (control_listener >= 0) {
            pid_t tracer = 0;
            uint64_t tracer_start = 0;
            int control_result = service_control(&control_listener, getuid(), getpid(), own_start_time,
                                                 launcher_executable.st_dev, launcher_executable.st_ino,
                                                 &tracer, &tracer_start, identity_token, &counter);
            if (control_result < 0) {
                perror("counter_workload: control channel");
                if (control_listener >= 0) close(control_listener);
                if (state_fd >= 0) close(state_fd);
                return 1;
            }
        }
        if (state_fd >= 0) {
            char state[64];
            int state_length = snprintf(state, sizeof(state), "%llu\n", counter);
            ssize_t state_written = -1;
            int state_error = ftruncate(state_fd, 0) != 0 || lseek(state_fd, 0, SEEK_SET) < 0;
            if (!state_error) state_written = write(state_fd, state, (size_t)state_length);
            if (state_written != (ssize_t)state_length) state_error = 1;
            if (state_error) {
                perror("counter_workload: test state file");
                (void)close(state_fd);
                return 1;
            }
        }
        printf("pid=%ld counter=%llu\n", (long)getpid(), counter++);
        fflush(stdout);
        struct timespec remaining = {.tv_sec = interval, .tv_nsec = 0};
        while (!stop_requested && (remaining.tv_sec > 0 || remaining.tv_nsec > 0)) {
            if (control_listener >= 0) {
                int wait_ms = remaining.tv_sec > 0 ? 100 :
                              (int)((remaining.tv_nsec + 999999) / 1000000);
                if (wait_ms > 100) wait_ms = 100;
                struct pollfd request = {.fd = control_listener, .events = POLLIN};
                int polled = poll(&request, 1, wait_ms);
                if (polled < 0 && errno != EINTR) {
                    perror("counter_workload: poll control channel");
                    return 1;
                }
                if (polled > 0 && (request.revents & POLLIN) != 0) {
                    pid_t tracer = 0;
                    uint64_t tracer_start = 0;
                    int result = service_control(&control_listener, getuid(), getpid(), own_start_time,
                                                 launcher_executable.st_dev, launcher_executable.st_ino,
                                                 &tracer, &tracer_start, identity_token, &counter);
                    if (result < 0) {
                        perror("counter_workload: control protocol");
                        return 1;
                    }
                }
                struct timespec elapsed = {.tv_sec = 0, .tv_nsec = (long)wait_ms * 1000000L};
                if (remaining.tv_nsec < elapsed.tv_nsec) {
                    if (remaining.tv_sec > 0) { --remaining.tv_sec; remaining.tv_nsec += 1000000000L; }
                    else elapsed.tv_nsec = remaining.tv_nsec;
                }
                remaining.tv_sec -= elapsed.tv_sec;
                remaining.tv_nsec -= elapsed.tv_nsec;
            } else if (nanosleep(&remaining, &remaining) == 0) {
                break;
            } else if (errno != EINTR) {
                perror("counter_workload: nanosleep");
                return 1;
            }
        }
    }
    printf("pid=%ld stopping counter=%llu\n", (long)getpid(), counter);
    if (state_fd >= 0) (void)close(state_fd);
    if (control_listener >= 0) (void)close(control_listener);
    if (argc == 3 || argc == 5) (void)prctl(PR_SET_PTRACER, 0, 0, 0, 0);
    return 0;
}
