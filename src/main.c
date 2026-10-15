#define _POSIX_C_SOURCE 200809L
#include "chrono/process.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s inspect PID\n", program);
}

int main(int argc, char **argv)
{
    char *end = NULL;
    long parsed_pid;
    char error[256];
    chrono_process_info info;

    if (argc != 3 || argv[1][0] == '\0' || argv[1][0] == '-') {
        usage(argv[0]);
        return 2;
    }
    for (const char *p = argv[2]; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            fprintf(stderr, "chrono: invalid PID '%s'\n", argv[2]);
            usage(argv[0]);
            return 2;
        }
    }
    errno = 0;
    parsed_pid = strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || parsed_pid <= 0 || parsed_pid > 2147483647L) {
        fprintf(stderr, "chrono: invalid PID '%s'\n", argv[2]);
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "inspect") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (chrono_process_inspect((pid_t)parsed_pid, &info, error, sizeof(error)) != 0) {
        fprintf(stderr, "chrono: %s\n", error);
        return 1;
    }
    printf("PID: %ld\nOwner UID: %lu\nState: %c\nCommand: %s\n",
           (long)info.pid, (unsigned long)info.owner, info.state, info.command);
    return 0;
}
