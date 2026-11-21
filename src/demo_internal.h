#ifndef CHRONO_DEMO_INTERNAL_H
#define CHRONO_DEMO_INTERNAL_H

#include <stdio.h>
#include <sys/types.h>

typedef struct {
    int launch;
    int monitoring;
    int checkpoint;
    int original_stop;
    int restore;
    int executable;
    int counter_continuity;
    int cleanup;
    int checkpoint_sampled;
    pid_t original_pid;
    pid_t restored_pid;
    const char *checkpoint_executable;
    const char *restored_executable;
    unsigned long long checkpoint_counter;
    unsigned long long restored_counter;
} chrono_demo_result;

void chrono_demo_print_summary(FILE *stream, const chrono_demo_result *result,
                               int test_fixture);

#endif
