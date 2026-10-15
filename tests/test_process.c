#define _POSIX_C_SOURCE 200809L
#include "chrono/process.h"
#include "../src/process_internal.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *message)
{
    fprintf(stderr, "test_process: %s\n", message);
    return 1;
}

int main(void)
{
    chrono_process_info info;
    char error[256];
    uid_t parsed_uid;
    if (chrono_parse_uid_line(" 123 123 123 123\n", &parsed_uid) != 0 ||
        parsed_uid != (uid_t)123) return fail("valid UID field rejected");
    if (chrono_parse_uid_line("123oops 123 123 123\n", &parsed_uid) == 0 ||
        chrono_parse_uid_line("123 123 123\n", &parsed_uid) == 0 ||
        chrono_parse_uid_line("123 123 123 123 extra\n", &parsed_uid) == 0 ||
        chrono_parse_uid_line("999999999999999999999999999999 1 1 1\n", &parsed_uid) == 0) {
        return fail("malformed or overflowing UID field accepted");
    }
    pid_t child = fork();
    if (child < 0) return fail("fork failed");
    if (child == 0) {
        for (;;) pause();
    }
    if (chrono_process_inspect(child, &info, error, sizeof(error)) != 0 ||
        info.pid != child || info.owner != getuid() || info.state == '\0' ||
        info.command[0] == '\0') {
        (void)kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        return fail("valid child inspection failed");
    }
    if (chrono_process_inspect(getpid(), &info, error, sizeof(error)) == 0 ||
        strstr(error, "itself") == NULL) {
        (void)kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        return fail("self-inspection was not rejected");
    }
    if (chrono_process_inspect(1, &info, error, sizeof(error)) == 0) {
        (void)kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        return fail("PID 1 was not rejected");
    }
    if (chrono_process_inspect((pid_t)2147483647, &info, error, sizeof(error)) == 0) {
        (void)kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        return fail("nonexistent PID was accepted");
    }
    (void)kill(child, SIGTERM);
    if (waitpid(child, NULL, 0) != child) return fail("waitpid failed");
    if (chrono_process_inspect(child, &info, error, sizeof(error)) == 0) {
        return fail("exited process was accepted");
    }
    puts("process inspection tests passed");
    return 0;
}
