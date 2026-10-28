#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int parse_unsigned(const char *text, unsigned long long *value)
{
    if (text == NULL || text[0] == '\0') return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0') return -1;
    *value = parsed;
    return 0;
}

#if !defined(CHRONO_DISABLE_PIDFD) && defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
static int process_start_time(pid_t pid, unsigned long long *start_time)
{
    char path[64], line[4096];
    int length = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    if (length < 0 || (size_t)length >= sizeof(path)) return -1;
    FILE *file = fopen(path, "r");
    if (file == NULL) return -1;
    if (fgets(line, sizeof(line), file) == NULL) {
        int saved = errno != 0 ? errno : EIO;
        (void)fclose(file);
        errno = saved;
        return -1;
    }
    int close_result = fclose(file);
    if (close_result != 0) return -1;

    /* The final ')' is the comm delimiter even when comm itself contains ')'. */
    char *fields = strrchr(line, ')');
    if (fields == NULL || fields[1] != ' ') { errno = EINVAL; return -1; }
    fields += 2; /* field 3 (state) */
    char *save = NULL;
    char *field = strtok_r(fields, " ", &save);
    for (int index = 0; field != NULL; ++index, field = strtok_r(NULL, " ", &save)) {
        if (index == 19) return parse_unsigned(field, start_time);
    }
    errno = EINVAL;
    return -1;
}
#endif

int main(int argc, char **argv)
{
    unsigned long long pid_value, expected_start;
    if (argc != 4 || parse_unsigned(argv[1], &pid_value) != 0 ||
        parse_unsigned(argv[2], &expected_start) != 0 || pid_value <= 1 ||
        pid_value > INT_MAX || expected_start == 0 || argv[3][0] != '/') {
        fprintf(stderr, "verified_stop: usage: PID STARTTIME ABSOLUTE_EXECUTABLE\n");
        return 2;
    }
    pid_t pid = (pid_t)pid_value;
    if (pid == getpid()) {
        fprintf(stderr, "verified_stop: refusing to signal the helper process\n");
        return 2;
    }
#if !defined(CHRONO_DISABLE_PIDFD) && defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        perror("verified_stop: pidfd_open");
        return 1;
    }

    char proc_path[64], executable[PATH_MAX + 1];
    int length = snprintf(proc_path, sizeof(proc_path), "/proc/%ld", (long)pid);
    struct stat process_stat;
    if (length < 0 || (size_t)length >= sizeof(proc_path) || stat(proc_path, &process_stat) != 0 ||
        process_stat.st_uid != getuid()) {
        fprintf(stderr, "verified_stop: process UID validation failed; no signal sent\n");
        close(pidfd);
        return 1;
    }
    length = snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid);
    ssize_t exe_length = length < 0 || (size_t)length >= sizeof(proc_path) ? -1 :
                         readlink(proc_path, executable, sizeof(executable) - 1);
    if (exe_length < 0 || (size_t)exe_length >= sizeof(executable) - 1) {
        fprintf(stderr, "verified_stop: process executable could not be validated; no signal sent\n");
        close(pidfd);
        return 1;
    }
    executable[exe_length] = '\0';
    if (strcmp(executable, argv[3]) != 0) {
        fprintf(stderr, "verified_stop: executable mismatch; no signal sent\n");
        close(pidfd);
        return 1;
    }
    unsigned long long actual_start;
    if (process_start_time(pid, &actual_start) != 0 || actual_start != expected_start) {
        fprintf(stderr, "verified_stop: process start-time mismatch; no signal sent\n");
        close(pidfd);
        return 1;
    }
    int result = (int)syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, NULL, 0);
    int saved = errno;
    close(pidfd);
    if (result != 0) {
        errno = saved;
        perror("verified_stop: pidfd_send_signal");
        return 1;
    }
    return 0;
#else
    fprintf(stderr, "verified_stop: pidfd operations are unavailable in this build\n");
    return 1;
#endif
}
