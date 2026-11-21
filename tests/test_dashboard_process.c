#define _POSIX_C_SOURCE 200809L
#include "../src/demo_dashboard.h"

#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *message)
{
    fprintf(stderr, "test_dashboard_process: %s\n", message);
    return 1;
}

static int capture_render(chrono_dashboard_view *view, int tty_mode,
                          char *buffer, size_t buffer_size)
{
    FILE *stream = tmpfile();
    int saved_stdout = dup(STDOUT_FILENO);
    if (stream == NULL || saved_stdout < 0 || fflush(stdout) != 0 ||
        dup2(fileno(stream), STDOUT_FILENO) < 0) {
        if (saved_stdout >= 0) close(saved_stdout);
        if (stream != NULL) fclose(stream);
        return -1;
    }
    chrono_dashboard_render(view, tty_mode);
    int failed = fflush(stdout) != 0 || fseek(stream, 0, SEEK_SET) != 0;
    size_t length = failed ? 0 : fread(buffer, 1, buffer_size - 1, stream);
    if (ferror(stream)) failed = 1;
    buffer[length] = '\0';
    if (dup2(saved_stdout, STDOUT_FILENO) < 0) failed = 1;
    close(saved_stdout);
    if (fclose(stream) != 0) failed = 1;
    return failed ? -1 : 0;
}

static size_t visible_line_widths(const char *text, size_t *longest)
{
    size_t lines = 0, column = 0;
    *longest = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if ((unsigned char)text[i] == 0x1b && text[i + 1] == '[') {
            i += 2;
            while (text[i] != '\0' &&
                   !((unsigned char)text[i] >= 0x40 && (unsigned char)text[i] <= 0x7e)) ++i;
            continue;
        }
        if (text[i] == '\n') {
            if (column > *longest) *longest = column;
            column = 0;
            ++lines;
        } else {
            ++column;
        }
    }
    if (column > *longest) *longest = column;
    return lines;
}

int main(void)
{
    pid_t children[5] = {-1, -1, -1, -1, -1};
    chrono_dashboard_process *snapshot = NULL;
    size_t count = 0, inaccessible = 0;
    int result = 1;
    const char *failure = "process discovery, pagination, stale identity, or narrow rendering failed";
    for (size_t i = 0; i < sizeof(children) / sizeof(children[0]); ++i) {
        children[i] = fork();
        if (children[i] < 0) goto cleanup;
        if (children[i] == 0) {
            for (;;) pause();
        }
    }
    if (chrono_dashboard_process_snapshot(&snapshot, &count, &inaccessible) != 0 ||
        count < sizeof(children) / sizeof(children[0]))
        goto cleanup;
    for (size_t child = 0; child < sizeof(children) / sizeof(children[0]); ++child) {
        int found = 0;
        for (size_t i = 0; i < count; ++i)
            if (snapshot[i].pid == children[child] && snapshot[i].start_time != 0)
                found = 1;
        if (!found) goto cleanup;
    }
    for (size_t i = 1; i < count; ++i)
        if (snapshot[i - 1].pid >= snapshot[i].pid) goto cleanup;

    uint64_t stale_start = 0;
    for (size_t i = 0; i < count; ++i)
        if (snapshot[i].pid == children[0]) stale_start = snapshot[i].start_time;
    pid_t disappeared_pid = children[0];
    if (stale_start == 0 || kill(children[0], SIGTERM) != 0 ||
        waitpid(children[0], NULL, 0) != children[0]) goto cleanup;
    children[0] = -1;
    chrono_dashboard_process stale;
    char error[160];
    if (disappeared_pid <= 0 ||
        chrono_dashboard_process_refresh(disappeared_pid, stale_start, &stale,
                                         error, sizeof(error)) == 0)
        goto cleanup;
    chrono_dashboard_process_snapshot_free(snapshot);
    snapshot = NULL;

    chrono_dashboard_process rows[10] = {0};
    for (size_t i = 0; i < 10; ++i) {
        rows[i].pid = (pid_t)(100 + i);
        rows[i].parent_pid = 1;
        rows[i].owner = getuid();
        rows[i].start_time = 1000 + i;
        rows[i].state = 'S';
        (void)snprintf(rows[i].user, sizeof(rows[i].user), "demo-user");
        (void)snprintf(rows[i].command, sizeof(rows[i].command), "process-%zu", i);
        (void)snprintf(rows[i].executable, sizeof(rows[i].executable), "/demo/process-%zu", i);
    }
    chrono_dashboard_view view = {
        .phase = "PROCESS BROWSER",
        .processes = rows,
        .process_count = 10,
        .process_page = 1,
        .process_page_size = 8,
        .inaccessible_processes = inaccessible,
        .process_browser = 1,
        .controls = "[1-8] monitor row"
    };
    char output[16384];
    if (capture_render(&view, 0, output, sizeof(output)) != 0 ||
        strstr(output, "Accessible: 10") == NULL ||
        strstr(output, "Page 2/2") == NULL ||
        strstr(output, "process-8") == NULL ||
        strstr(output, "Checkpoint compatibility is not evaluated") == NULL ||
        strstr(output, "[1-8] monitor row") == NULL)
        goto cleanup;

    view.process_page = 0;
    view.process_help = 1;
    if (capture_render(&view, 0, output, sizeof(output)) != 0 ||
        strstr(output, "Monitoring never signals") == NULL ||
        strstr(output, "Only the dedicated bundled ChronoOS counter") == NULL)
        goto cleanup;
    view.process_help = 0;
    view.process_count = 0;
    if (capture_render(&view, 0, output, sizeof(output)) != 0 ||
        strstr(output, "Page 1/1") == NULL ||
        strstr(output, "No readable processes were discovered") == NULL)
        goto cleanup;
    view.process_count = 10;
    view.process_page = 1;
    view.terminal_width = 40;
    chrono_dashboard_stop(1);
    if (capture_render(&view, 1, output, sizeof(output)) != 0)
        goto cleanup;
    size_t longest = 0;
    (void)visible_line_widths(output, &longest);
    if (longest > 39) goto cleanup;

    view.process_browser = 0;
    view.terminal_width = 100;
    view.terminal_height = 18;
    view.checkpoint_directory = "/tmp/chronoos-dashboard-checkpoints";
    view.criu_status = "blocked";
    view.criu_version = "CRIU 4.3";
    view.criu_diagnostic = "preflight diagnostic";
    chrono_dashboard_stop(1);
    if (capture_render(&view, 1, output, sizeof(output)) != 0 ||
        strstr(output, "PROCESS CONTROL CENTER") == NULL ||
        strstr(output, "User:") == NULL ||
        strstr(output, "read-only") == NULL ||
        strstr(output, "target /tmp/chronoos-dashboard-checkpoints") == NULL) {
        failure = "compact dashboard omitted required status";
        goto cleanup;
    }
    size_t lines = visible_line_widths(output, &longest);
    if (lines > view.terminal_height || longest > 99 ||
        strstr(output, "preflight diagnostic") != NULL) {
        failure = "compact dashboard exceeded its height/width or failed to hide diagnostics";
        goto cleanup;
    }

    chrono_checkpoint_metadata checkpoint = {0};
    (void)snprintf(checkpoint.id, sizeof(checkpoint.id), "compact-checkpoint");
    checkpoint.original_pid = 100;
    checkpoint.process_start_time = 1000;
    checkpoint.state = CHRONO_CHECKPOINT_COMPLETE;
    (void)snprintf(checkpoint.backend, sizeof(checkpoint.backend), "criu");
    chrono_process_info restored = {0};
    restored.pid = 101;
    restored.start_time = 2000;
    restored.owner = getuid();
    (void)snprintf(restored.executable, sizeof(restored.executable), "/demo/counter_workload");
    view.terminal_width = 160;
    view.checkpoint = &checkpoint;
    view.checkpoint_artifact_valid = 1;
    view.restored_process = &restored;
    view.counter_status = "in-memory counter advanced 10 -> 11";
    chrono_dashboard_stop(1);
    if (capture_render(&view, 1, output, sizeof(output)) != 0 ||
        strstr(output, "compact-checkpoint") == NULL ||
        strstr(output, "/tmp/chronoos-dashboard-checkpoints/compact-checkpoint") == NULL ||
        strstr(output, "RESTORED: PID=101 START=2000") == NULL ||
        strstr(output, "in-memory counter advanced 10 -> 11") == NULL)
        goto cleanup;
    lines = visible_line_widths(output, &longest);
    if (lines > view.terminal_height || longest > 159) {
        failure = "compact dashboard omitted restore metadata or exceeded terminal dimensions";
        goto cleanup;
    }
    chrono_dashboard_stop(1);
    result = 0;
cleanup:
    chrono_dashboard_process_snapshot_free(snapshot);
    for (size_t i = 0; i < sizeof(children) / sizeof(children[0]); ++i) {
        if (children[i] <= 1) continue;
        (void)kill(children[i], SIGTERM);
        while (waitpid(children[i], NULL, 0) < 0) {
            if (errno != EINTR) break;
        }
    }
    if (result != 0) return fail(failure);
    puts("dashboard process discovery, pagination, stale identity, help, and narrow rendering tests passed");
    return 0;
}
