#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static volatile sig_atomic_t stop_requested = 0;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

int main(int argc, char **argv)
{
    long interval = 1;
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [interval-seconds 1..60 [identity-token]]\n", argv[0]);
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
    if (argc == 3) {
        if (strlen(argv[2]) != 32) return 2;
        for (size_t i = 0; i < 32; ++i)
            if (!((argv[2][i] >= '0' && argv[2][i] <= '9') ||
                  (argv[2][i] >= 'a' && argv[2][i] <= 'f'))) return 2;
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
        while (!stop_requested) {
            if (nanosleep(&remaining, &remaining) == 0) break;
            if (errno == EINTR) continue;
            perror("counter_workload: nanosleep");
            return 1;
        }
    }
    printf("pid=%ld stopping counter=%llu\n", (long)getpid(), counter);
    if (state_fd >= 0) (void)close(state_fd);
    return 0;
}
