#include "demo_internal.h"

static const char *status_name(int status)
{
    if (status > 0) return "PASS";
    if (status < 0) return "FAIL";
    return "NOT ATTEMPTED";
}

void chrono_demo_print_summary(FILE *stream, const chrono_demo_result *result,
                               int test_fixture)
{
    if (stream == NULL || result == NULL) return;
    if (test_fixture)
        (void)fprintf(stream, "SIMULATION/TEST FIXTURE: formatter only; no process or CRIU operation occurred.\n");
    (void)fprintf(stream, "\nDEMO SUMMARY\n");
    (void)fprintf(stream, "  Process launch:       %s\n", status_name(result->launch));
    (void)fprintf(stream, "  Live monitoring:      %s\n", status_name(result->monitoring));
    (void)fprintf(stream, "  Checkpoint creation:  %s\n", status_name(result->checkpoint));
    (void)fprintf(stream, "  Original termination: %s\n", status_name(result->original_stop));
    (void)fprintf(stream, "  Restore:              %s\n", status_name(result->restore));
    (void)fprintf(stream, "  Executable identity:  %s\n", status_name(result->executable));
    (void)fprintf(stream, "  Counter continuity:   %s\n", status_name(result->counter_continuity));
    (void)fprintf(stream, "  Cleanup:              %s\n", status_name(result->cleanup));
    if (result->checkpoint != 0 || result->restore != 0) {
        (void)fprintf(stream, "\n  Property          Pre-dump sample        Restored process\n");
        (void)fprintf(stream, "  PID               %ld                    %s",
                      (long)result->original_pid,
                      result->restore > 0 ? "" : "not restored\n");
        if (result->restore > 0) (void)fprintf(stream, "%ld\n", (long)result->restored_pid);
        (void)fprintf(stream, "  Executable        %s\n                    %s\n",
                      result->checkpoint_executable != NULL ? result->checkpoint_executable : "not recorded",
                      result->restored_executable != NULL ? result->restored_executable : "not verified");
        if (result->checkpoint_sampled)
            (void)fprintf(stream, "  Counter           %llu                    ", result->checkpoint_counter);
        else
            (void)fprintf(stream, "  Counter           not sampled             ");
        if (result->restore > 0) (void)fprintf(stream, "%llu\n", result->restored_counter);
        else (void)fprintf(stream, "not restored\n");
    }
}
