#define _POSIX_C_SOURCE 200809L
#include "../src/demo_internal.h"
#include "../src/demo_dashboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int check_output(const chrono_demo_result *result, const char *required,
                        const char *forbidden)
{
    FILE *stream = tmpfile();
    if (stream == NULL) return -1;
    chrono_demo_print_summary(stream, result, 1);
    if (fflush(stream) != 0 || fseek(stream, 0, SEEK_SET) != 0) { fclose(stream); return -1; }
    char output[4096];
    size_t length = fread(output, 1, sizeof(output) - 1, stream);
    int failed = ferror(stream) || fclose(stream) != 0;
    output[length] = '\0';
    if (failed || strstr(output, "SIMULATION/TEST FIXTURE") == NULL ||
        strstr(output, required) == NULL || (forbidden != NULL && strstr(output, forbidden) != NULL)) return -1;
    return 0;
}

static int render_dashboard(char *output, size_t output_size, int tty_mode)
{
    chrono_dashboard_event events[8];
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        events[i].timestamp = "2026-01-02 03:04:05";
        events[i].message = i == 0 ? "Oldest evicted event" :
                            i == 7 ? "Latest checkpoint event" : "Intermediate event";
    }
    chrono_process_info process = {
        .pid = 4242, .owner = 1000, .state = 'S', .start_time = 987654
    };
    chrono_process_info restored = {
        .pid = 4343, .owner = 1000, .state = 'S', .start_time = 987999
    };
    char long_path[1024];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    (void)snprintf(process.executable, sizeof(process.executable), "%s", long_path);
    (void)snprintf(restored.executable, sizeof(restored.executable), "%s", long_path);
    chrono_checkpoint_metadata checkpoint = {
        .original_pid = 4242,
        .process_start_time = 987654,
        .state = CHRONO_CHECKPOINT_COMPLETE
    };
    (void)snprintf(checkpoint.id, sizeof(checkpoint.id), "test-checkpoint");
    (void)snprintf(checkpoint.backend, sizeof(checkpoint.backend), "criu");
    chrono_dashboard_view view = {
        .phase = "CHECKPOINT VALIDATED; ORIGINAL LIVE",
        .criu_path = "/usr/sbin/criu",
        .criu_version = "CRIU 4.3",
        .criu_status = "system CRIU preflight passed",
        .criu_mode = "unprivileged (--unprivileged)",
        .supervisor_status = "interactive demo session; not persistent",
        .feedback = "[INPUT] Received: c - CHECKPOINT",
        .controls = "[t]erminate original",
        .checkpoint_directory = "/tmp/chronoos-dashboard-checkpoints",
        .restore_status = "VERIFIED LIVE PROCESS",
        .counter_status = "in-memory counter advanced 23 -> 24",
        .managed_status = "RUNNING",
        .managed_process = &process,
        .counter_available = 1,
        .counter = 23,
        .checkpoint = &checkpoint,
        .checkpoint_artifact_valid = 1,
        .restored_process = &restored,
        .events = events,
        .event_count = sizeof(events) / sizeof(events[0]),
        .terminal_width = 120,
        .terminal_height = 50
    };
    FILE *stream = tmpfile();
    if (stream == NULL) return -1;
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0 || fflush(stdout) != 0 ||
        dup2(fileno(stream), STDOUT_FILENO) < 0) {
        if (saved_stdout >= 0) close(saved_stdout);
        fclose(stream);
        return -1;
    }
    chrono_dashboard_render(&view, tty_mode);
    if (tty_mode) {
        view.counter = 24;
        chrono_dashboard_render(&view, tty_mode);
    }
    int failed = fflush(stdout) != 0 || fseek(stream, 0, SEEK_SET) != 0;
    size_t length = failed ? 0 : fread(output, 1, output_size - 1, stream);
    if (ferror(stream)) failed = 1;
    output[length] = '\0';
    if (dup2(saved_stdout, STDOUT_FILENO) < 0) failed = 1;
    close(saved_stdout);
    if (fclose(stream) != 0) failed = 1;
    return failed ? -1 : 0;
}

int main(void)
{
    chrono_demo_result fixture = {
        .launch = 1, .monitoring = 1, .checkpoint = 1, .original_stop = 1,
        .restore = 1, .executable = 1, .counter_continuity = 1, .cleanup = 1,
        .original_pid = 1201, .restored_pid = 1302,
        .checkpoint_executable = "/fixture/counter_workload",
        .restored_executable = "/fixture/counter_workload",
        .checkpoint_sampled = 1,
        .checkpoint_counter = 8, .restored_counter = 10
    };
    if (check_output(&fixture, "Restore:              PASS", "Restore:              NOT ATTEMPTED") != 0)
        return fprintf(stderr, "test_demo_format: successful fixture summary failed\n"), 1;
    fixture.checkpoint = -1;
    fixture.restore = 0;
    if (check_output(&fixture, "Restore:              NOT ATTEMPTED", "Restore:              PASS") != 0)
        return fprintf(stderr, "test_demo_format: skipped-stage summary was misleading\n"), 1;
    char dashboard[16384];
    if (render_dashboard(dashboard, sizeof(dashboard), 0) != 0 ||
        strstr(dashboard, "PROCESS CONTROL CENTER") == NULL ||
        strstr(dashboard, "Linux: ") == NULL ||
        strstr(dashboard, "Dashboard: ACTIVE") == NULL ||
        strstr(dashboard, "SYSTEM STATUS") == NULL ||
        strstr(dashboard, "MANAGED WORKLOAD") == NULL ||
        strstr(dashboard, "CHECKPOINT") == NULL ||
        strstr(dashboard, "RESTORE") == NULL ||
        strstr(dashboard, "COMMAND CENTER") == NULL ||
        strstr(dashboard, "EVENT HISTORY") == NULL ||
        strstr(dashboard, "Live in-memory counter: 23") == NULL ||
        strstr(dashboard, "Validation: VALIDATED") == NULL ||
        strstr(dashboard, "Identity: VERIFIED | PID=4343") == NULL ||
        strstr(dashboard, "Latest checkpoint event") == NULL ||
        strstr(dashboard, "Oldest evicted event") != NULL ||
        strstr(dashboard, "[INPUT] Received: c - CHECKPOINT") == NULL ||
        strstr(dashboard, "\033[") != NULL)
        return fprintf(stderr, "test_demo_format: non-TTY dashboard output is incomplete or styled\n"), 1;
    if (render_dashboard(dashboard, sizeof(dashboard), 1) != 0 ||
        strstr(dashboard, "\033[36m") == NULL || strstr(dashboard, "\033[32m") == NULL ||
        strstr(dashboard, "\033[2J") == NULL || strstr(dashboard, "24") == NULL ||
        strstr(strstr(dashboard, "\033[2J") + 4, "\033[2J") != NULL)
        return fprintf(stderr, "test_demo_format: TTY colors or incremental rendering failed\n"), 1;
    chrono_dashboard_stop(1);
    chrono_dashboard_view compact = {
        .phase = "SMALL TERMINAL",
        .criu_version = "CRIU 4.3",
        .criu_status = "preflight passed",
        .criu_mode = "unprivileged",
        .feedback = "[INPUT] Received: r - RESTORE / REFRESH",
        .terminal_width = 40,
        .terminal_height = 18
    };
    FILE *stream = tmpfile();
    if (stream == NULL) return 1;
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0 || dup2(fileno(stream), STDOUT_FILENO) < 0) {
        if (saved_stdout >= 0) close(saved_stdout);
        fclose(stream);
        return 1;
    }
    chrono_dashboard_render(&compact, 1);
    int compact_failed = fflush(stdout) != 0 || fseek(stream, 0, SEEK_SET) != 0;
    size_t compact_length = compact_failed ? 0 : fread(dashboard, 1, sizeof(dashboard) - 1, stream);
    dashboard[compact_length] = '\0';
    if (ferror(stream)) compact_failed = 1;
    (void)dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    fclose(stream);
    if (compact_failed || strstr(dashboard, "PROCESS CONTROL CENTER") == NULL ||
        strstr(dashboard, "[INPUT] Received: r") == NULL)
        return fprintf(stderr, "test_demo_format: compact layout or feedback is missing\n"), 1;
    char feedback[128];
    if (chrono_dashboard_input_feedback('c', feedback, sizeof(feedback)) != 0 ||
        strstr(feedback, "Received: c") == NULL || strstr(feedback, "CHECKPOINT") == NULL ||
        chrono_dashboard_input_feedback('?', feedback, sizeof(feedback)) != 0 ||
        strstr(feedback, "UNRECOGNIZED") == NULL)
        return fprintf(stderr, "test_demo_format: command feedback recognition failed\n"), 1;
    puts("dashboard and demo summary formatting tests passed (test fixtures only)");
    return 0;
}
