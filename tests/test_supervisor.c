#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "chrono/supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *message, const char *detail)
{
    fprintf(stderr, "test_supervisor: %s%s%s\n", message,
            detail != NULL && detail[0] != '\0' ? ": " : "",
            detail != NULL ? detail : "");
    return 1;
}

typedef struct { size_t count; char ids[8][CHRONO_SUPERVISOR_ID_MAX + 1]; } rows;

static char test_directory[CHRONO_SUPERVISOR_PATH_MAX];
static char registered_ids[8][CHRONO_SUPERVISOR_ID_MAX + 1];
static char original_first_record[32768];
static size_t original_first_record_size;
static int first_record_tampered;
static char delayed_ready_path[CHRONO_SUPERVISOR_PATH_MAX];
static volatile sig_atomic_t delayed_term_received;

static void delayed_term_handler(int signal_number)
{
    (void)signal_number;
    delayed_term_received = 1;
}

static void cleanup(void)
{
    if (test_directory[0] == '\0') return;
    if (first_record_tampered && registered_ids[0][0] != '\0') {
        char path[CHRONO_SUPERVISOR_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s.rec", test_directory, registered_ids[0]);
        if (n > 0 && (size_t)n < sizeof(path)) {
            int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
            if (fd >= 0) {
                ssize_t written = write(fd, original_first_record, original_first_record_size);
                (void)written;
                (void)close(fd);
            }
        }
    }
    char error[256];
    for (size_t i = 0; i < 8; ++i) {
        if (registered_ids[i][0] == '\0') continue;
        chrono_supervised_process ignored;
        if (chrono_supervisor_stop(test_directory, registered_ids[i], &ignored, error, sizeof(error)) != 0) {
            struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};
            (void)nanosleep(&pause, NULL);
            (void)chrono_supervisor_stop(test_directory, registered_ids[i], &ignored, error, sizeof(error));
        }
        char name[CHRONO_SUPERVISOR_PATH_MAX];
        int n = snprintf(name, sizeof(name), "%s/%s.rec", test_directory, registered_ids[i]);
        if (n > 0 && (size_t)n < sizeof(name)) (void)unlink(name);
    }
    char path[CHRONO_SUPERVISOR_PATH_MAX];
    const char *files[] = {"cp-999999.rec", ".sequence", ".lock"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        int n = snprintf(path, sizeof(path), "%s/%s", test_directory, files[i]);
        if (n > 0 && (size_t)n < sizeof(path)) (void)unlink(path);
    }
    if (delayed_ready_path[0] != '\0') (void)unlink(delayed_ready_path);
    (void)rmdir(test_directory);
}

static int collect(const chrono_supervised_process *process, void *opaque)
{
    rows *result = opaque;
    if (result->count >= 8) return -1;
    (void)snprintf(result->ids[result->count], sizeof(result->ids[result->count]), "%s", process->id);
    ++result->count;
    return 0;
}

static int launch_registration_worker(const char *directory, char *id, size_t id_size)
{
    int channel[2];
    if (pipe(channel) != 0) return -1;
    pid_t worker = fork();
    if (worker < 0) { close(channel[0]); close(channel[1]); return -1; }
    if (worker == 0) {
        close(channel[0]);
        char *argv[] = {"/bin/sleep", "60", NULL};
        chrono_supervised_process process;
        char error[256] = {0};
        int result = chrono_supervisor_protect(directory, argv, &process, error, sizeof(error));
        if (result == 0) {
            char line[CHRONO_SUPERVISOR_ID_MAX + 2];
            int length = snprintf(line, sizeof(line), "%s\n", process.id);
            if (length > 0) {
                ssize_t written = write(channel[1], line, (size_t)length);
                (void)written;
            }
        }
        close(channel[1]);
        _exit(result == 0 ? 0 : 1);
    }
    close(channel[1]);
    size_t used = 0;
    while (used + 1 < id_size) {
        ssize_t count = read(channel[0], id + used, id_size - used - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += (size_t)count;
        if (memchr(id, '\n', used) != NULL) break;
    }
    close(channel[0]);
    int status;
    pid_t waited;
    do { waited = waitpid(worker, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != worker || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || used == 0)
        return -1;
    char *newline = memchr(id, '\n', used);
    if (newline == NULL) return -1;
    *newline = '\0';
    return chrono_supervisor_id_valid(id) ? 0 : -1;
}

int main(int argc, char **main_args)
{
    if (argc == 3 && strcmp(main_args[1], "delayed-exit") == 0) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = delayed_term_handler;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, NULL) != 0) return 2;
        int ready_fd = open(main_args[2], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (ready_fd < 0) return 3;
        char ready = '1';
        ssize_t ready_written = write(ready_fd, &ready, 1);
        int ready_close = close(ready_fd);
        if (ready_written != 1 || ready_close != 0) return 4;
        while (!delayed_term_received) pause();
        struct timespec delay = {.tv_sec = 5, .tv_nsec = 500000000};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
        return 0;
    }
    char template[] = "/tmp/chrono-supervisor-test-XXXXXX";
    char *directory = mkdtemp(template);
    if (directory == NULL) return fail("cannot create isolated registry", strerror(errno));
    (void)snprintf(test_directory, sizeof(test_directory), "%s", directory);
    if (atexit(cleanup) != 0) return fail("cannot install test cleanup", NULL);
    char error[256] = {0};
    chrono_supervised_process first, second, loaded;
    rows empty = {0};
    if (chrono_supervisor_list(directory, collect, &empty, error, sizeof(error)) != 0 || empty.count != 0)
        return fail("new registry was not empty", error);
    char *missing_argv[] = {"/definitely/not/a/chronoos-workload", NULL};
    if (chrono_supervisor_protect(directory, missing_argv, &loaded, error, sizeof(error)) == 0 ||
        strstr(error, "execute protected workload") == NULL)
        return fail("failed executable launch was not rejected", error);
    char *argv[] = {"/bin/sleep", "60", NULL};
    if (chrono_supervisor_protect(directory, argv, &first, error, sizeof(error)) != 0)
        return fail("first protected launch failed", error);
    (void)snprintf(registered_ids[0], sizeof(registered_ids[0]), "%s", first.id);
    if (first.state != CHRONO_SUPERVISOR_RUNNING || first.pid <= 1 || first.start_time == 0 ||
        first.owner != getuid() || first.executable[0] != '/' || first.command[0] == '\0' ||
        first.checkpoint_interval_seconds != CHRONO_CHECKPOINT_INTERVAL_DEFAULT)
        return fail("registered identity is incomplete", NULL);
    if (chrono_supervisor_get(directory, first.id, &loaded, error, sizeof(error)) != 0 ||
        loaded.pid != first.pid || loaded.start_time != first.start_time ||
        strcmp(loaded.executable, first.executable) != 0 || strcmp(loaded.command, first.command) != 0)
        return fail("persisted record did not reload", error);

    if (chrono_supervisor_protect_with_interval(directory, argv, 3, &second, error, sizeof(error)) != 0)
        return fail("second protected launch failed", error);
    (void)snprintf(registered_ids[1], sizeof(registered_ids[1]), "%s", second.id);
    if (strcmp(first.id, second.id) == 0 || first.pid == second.pid || first.start_time == second.start_time)
        return fail("independent workloads did not receive distinct identities", NULL);
    if (chrono_supervisor_get(directory, second.id, &loaded, error, sizeof(error)) != 0 ||
        loaded.checkpoint_interval_seconds != 3)
        return fail("configured checkpoint interval did not persist", error);
    if (chrono_supervisor_protect_with_interval(directory, argv, 0, &loaded, error, sizeof(error)) == 0 ||
        strstr(error, "checkpoint interval") == NULL)
        return fail("out-of-range checkpoint interval was accepted", error);
    for (size_t i = 2; i < 6; ++i) {
        if (launch_registration_worker(directory, registered_ids[i], sizeof(registered_ids[i])) != 0)
            return fail("concurrent registration worker failed", NULL);
        for (size_t j = 0; j < i; ++j)
            if (strcmp(registered_ids[i], registered_ids[j]) == 0)
                return fail("concurrent registrations allocated duplicate IDs", NULL);
    }
    rows listed = {0};
    if (chrono_supervisor_list(directory, collect, &listed, error, sizeof(error)) != 0 || listed.count != 6)
        return fail("listing did not return all concurrent registrations", error);
    char *instant_argv[] = {"/bin/true", NULL};
    int instant_result = chrono_supervisor_protect(directory, instant_argv, &loaded,
                                                   error, sizeof(error));
    if (instant_result == 0) {
        if (loaded.state != CHRONO_SUPERVISOR_RUNNING &&
            loaded.state != CHRONO_SUPERVISOR_EXITED)
            return fail("immediate-exit workload returned an unresolved successful state", error);
        (void)snprintf(registered_ids[7], sizeof(registered_ids[7]), "%s", loaded.id);
        if (loaded.state == CHRONO_SUPERVISOR_RUNNING &&
            chrono_supervisor_stop(directory, loaded.id, &loaded, error, sizeof(error)) != 0)
            return fail("immediate workload cleanup failed", error);
    } else if (strstr(error, "exited during startup") == NULL &&
               strstr(error, "could not establish protected process identity") == NULL) {
        return fail("immediate child exit produced an unrelated registration error", error);
    }

    char record_path[CHRONO_SUPERVISOR_PATH_MAX];
    if (snprintf(record_path, sizeof(record_path), "%s/%s.rec", directory, first.id) >= (int)sizeof(record_path))
        return fail("test path overflow", NULL);
    struct stat record_before, record_after;
    if (stat(record_path, &record_before) != 0) return fail("cannot stat record before replacement", strerror(errno));
    char original[32768], modified[32768];
    int fd = open(record_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail("cannot read process record for stale identity test", strerror(errno));
    ssize_t record_size = read(fd, original, sizeof(original) - 1);
    int read_errno = errno;
    if (close(fd) != 0 || record_size <= 0) return fail("cannot read process record", strerror(read_errno));
    original[record_size] = '\0';
    memcpy(original_first_record, original, (size_t)record_size + 1);
    original_first_record_size = (size_t)record_size;
    if (strncmp(original, "CHRONO-PROCESS-2\n", 17) != 0)
        return fail("new registration did not use process metadata version 2", NULL);
    memcpy(modified, original, (size_t)record_size + 1);
    modified[15] = '1';
    char *line = strrchr(modified, '\n');
    if (line == NULL) return fail("version 2 record has no final newline", NULL);
    *line = '\0';
    line = strrchr(modified, '\n');
    if (line == NULL) return fail("version 2 record has no interval field", NULL);
    line[1] = '\0';
    size_t legacy_length = strlen(modified);
    fd = open(record_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0 || write(fd, modified, legacy_length) != (ssize_t)legacy_length || close(fd) != 0)
        return fail("cannot install version 1 compatibility fixture", strerror(errno));
    if (chrono_supervisor_get(directory, first.id, &loaded, error, sizeof(error)) != 0 ||
        loaded.checkpoint_interval_seconds != CHRONO_CHECKPOINT_INTERVAL_DEFAULT)
        return fail("version 1 registry record did not load with the default interval", error);
    fd = open(record_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0 || write(fd, original, (size_t)record_size) != record_size || close(fd) != 0)
        return fail("cannot restore version 2 compatibility record", strerror(errno));
    char *start_line = strchr(original, '\n');
    if (start_line == NULL || (start_line = strchr(start_line + 1, '\n')) == NULL ||
        (start_line = strchr(start_line + 1, '\n')) == NULL) return fail("unexpected process record layout", NULL);
    ++start_line;
    char *start_end = strchr(start_line, '\n');
    if (start_end == NULL) return fail("missing start time field", NULL);
    char saved_start[32]; size_t start_length = (size_t)(start_end - start_line);
    if (start_length == 0 || start_length >= sizeof(saved_start)) return fail("invalid start time field", NULL);
    memcpy(saved_start, start_line, start_length); saved_start[start_length] = '\0';
    char *parse_end = NULL; errno = 0; unsigned long long old_start = strtoull(saved_start, &parse_end, 10);
    if (errno != 0 || parse_end == saved_start || *parse_end != '\0' || old_start == UINT64_MAX)
        return fail("cannot parse process start time", NULL);
    char replacement[32]; int replacement_length = snprintf(replacement, sizeof(replacement), "%llu", old_start + 1);
    size_t prefix_length = (size_t)(start_line - original);
    size_t suffix_length = strlen(start_end);
    if (replacement_length < 0 || prefix_length + (size_t)replacement_length + suffix_length >= sizeof(modified))
        return fail("modified record overflow", NULL);
    memcpy(modified, original, prefix_length);
    memcpy(modified + prefix_length, replacement, (size_t)replacement_length);
    memcpy(modified + prefix_length + (size_t)replacement_length, start_end, suffix_length + 1);
    fd = open(record_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0 || write(fd, modified, prefix_length + (size_t)replacement_length + suffix_length) !=
                  (ssize_t)(prefix_length + (size_t)replacement_length + suffix_length) || close(fd) != 0)
        return fail("cannot install stale identity fixture", strerror(errno));
    first_record_tampered = 1;
    if (chrono_supervisor_stop(directory, first.id, &loaded, error, sizeof(error)) == 0 ||
        strstr(error, "start time changed") == NULL)
        return fail("stale start time was allowed to stop a process", error);
    chrono_process_info still_running;
    if (chrono_process_inspect(first.pid, &still_running, error, sizeof(error)) != 0 ||
        still_running.start_time != first.start_time || still_running.state == 'Z')
        return fail("stale identity test affected the registered workload", error);
    fd = open(record_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0 || write(fd, original, (size_t)record_size) != record_size || close(fd) != 0)
        return fail("cannot restore test-owned process record", strerror(errno));
    first_record_tampered = 0;

    if (!chrono_supervisor_id_valid(first.id) || chrono_supervisor_id_valid("cp-00000x") ||
        chrono_supervisor_get(directory, "cp-999999", &loaded, error, sizeof(error)) == 0)
        return fail("invalid or unknown ID handling failed", error);

    if (chrono_supervisor_stop(directory, first.id, &loaded, error, sizeof(error)) != 0 ||
        loaded.state != CHRONO_SUPERVISOR_EXITED || !loaded.intentional_stop)
        return fail("verified intentional stop failed", error);
    if (stat(record_path, &record_after) != 0 || record_before.st_ino == record_after.st_ino)
        return fail("lifecycle update did not atomically replace the registry record", strerror(errno));
    if (chrono_supervisor_stop(directory, first.id, &loaded, error, sizeof(error)) != 0 ||
        loaded.state != CHRONO_SUPERVISOR_EXITED)
        return fail("already stopped workload was not handled idempotently", error);
    if (chrono_supervisor_get(directory, second.id, &loaded, error, sizeof(error)) != 0 ||
        loaded.state != CHRONO_SUPERVISOR_RUNNING)
        return fail("stopping one workload affected another", error);

    struct rlimit old_file_limit, tiny_file_limit;
    struct sigaction old_xfsz_action, ignored_xfsz_action;
    if (getrlimit(RLIMIT_FSIZE, &old_file_limit) != 0 ||
        sigaction(SIGXFSZ, NULL, &old_xfsz_action) != 0)
        return fail("cannot inspect test process file-size limit", strerror(errno));
    tiny_file_limit = old_file_limit;
    if (tiny_file_limit.rlim_max < 1) return fail("file-size limit cannot support safe write-failure fixture", NULL);
    tiny_file_limit.rlim_cur = 1;
    memset(&ignored_xfsz_action, 0, sizeof(ignored_xfsz_action));
    ignored_xfsz_action.sa_handler = SIG_IGN;
    sigemptyset(&ignored_xfsz_action.sa_mask);
    if (sigaction(SIGXFSZ, &ignored_xfsz_action, NULL) != 0 ||
        setrlimit(RLIMIT_FSIZE, &tiny_file_limit) != 0)
        return fail("cannot enable test-local write-failure fixture", strerror(errno));
    int failed_stop = chrono_supervisor_stop(directory, second.id, &loaded, error, sizeof(error));
    int limit_restore = setrlimit(RLIMIT_FSIZE, &old_file_limit);
    int action_restore = sigaction(SIGXFSZ, &old_xfsz_action, NULL);
    if (limit_restore != 0 || action_restore != 0)
        return fail("cannot restore test process file-size settings", strerror(errno));
    if (failed_stop == 0 || strstr(error, "atomic registry") == NULL)
        return fail("registry write failure did not block stop before signaling", error);
    if (chrono_supervisor_get(directory, second.id, &loaded, error, sizeof(error)) != 0 ||
        loaded.state != CHRONO_SUPERVISOR_RUNNING)
        return fail("failed STOPPING write changed lifecycle state or signaled the workload", error);
    if (chrono_supervisor_stop(directory, second.id, &loaded, error, sizeof(error)) != 0 ||
        loaded.state != CHRONO_SUPERVISOR_EXITED)
        return fail("second verified stop failed", error);
    for (size_t i = 2; i < 6; ++i) {
        if (chrono_supervisor_stop(directory, registered_ids[i], &loaded, error, sizeof(error)) != 0 ||
            loaded.state != CHRONO_SUPERVISOR_EXITED)
            return fail("concurrently registered workload did not stop safely", error);
    }

    if (snprintf(delayed_ready_path, sizeof(delayed_ready_path), "%s.ready", directory) >=
        (int)sizeof(delayed_ready_path)) return fail("delayed workload ready path overflow", NULL);
    char *delayed_argv[] = {"./build/test_supervisor", "delayed-exit", delayed_ready_path, NULL};
    if (chrono_supervisor_protect(directory, delayed_argv, &loaded, error, sizeof(error)) != 0)
        return fail("delayed-exit workload registration failed", error);
    (void)snprintf(registered_ids[6], sizeof(registered_ids[6]), "%s", loaded.id);
    int ready = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (access(delayed_ready_path, F_OK) == 0) { ready = 1; break; }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        (void)nanosleep(&pause, NULL);
    }
    if (!ready) return fail("delayed-exit workload did not become ready", NULL);
    if (chrono_supervisor_stop(directory, loaded.id, &loaded, error, sizeof(error)) == 0 ||
        loaded.state != CHRONO_SUPERVISOR_STOPPING ||
        strstr(error, "not confirmed within 5 seconds") == NULL)
        return fail("bounded stop timeout did not preserve STOPPING", error);
    struct timespec settle = {.tv_sec = 1, .tv_nsec = 0};
    (void)nanosleep(&settle, NULL);
    if (chrono_supervisor_stop(directory, registered_ids[6], &loaded, error, sizeof(error)) != 0 ||
        loaded.state != CHRONO_SUPERVISOR_EXITED)
        return fail("repeated stop did not resolve delayed termination", error);

    if (getrlimit(RLIMIT_FSIZE, &old_file_limit) != 0 ||
        sigaction(SIGXFSZ, NULL, &old_xfsz_action) != 0)
        return fail("cannot inspect file-size limit for protect failure test", strerror(errno));
    tiny_file_limit = old_file_limit;
    tiny_file_limit.rlim_cur = 1;
    if (sigaction(SIGXFSZ, &ignored_xfsz_action, NULL) != 0 ||
        setrlimit(RLIMIT_FSIZE, &tiny_file_limit) != 0)
        return fail("cannot enable protect write-failure fixture", strerror(errno));
    int failed_protect = chrono_supervisor_protect(directory, argv, &loaded, error, sizeof(error));
    limit_restore = setrlimit(RLIMIT_FSIZE, &old_file_limit);
    action_restore = sigaction(SIGXFSZ, &old_xfsz_action, NULL);
    if (limit_restore != 0 || action_restore != 0)
        return fail("cannot restore file-size settings after protect failure", strerror(errno));
    if (failed_protect == 0 || strstr(error, "atomic registry") == NULL)
        return fail("registration did not report atomic registry write failure", error);

    char sequence_path[CHRONO_SUPERVISOR_PATH_MAX];
    if (snprintf(sequence_path, sizeof(sequence_path), "%s/.sequence", directory) >= (int)sizeof(sequence_path))
        return fail("sequence path overflow", NULL);
    if (unlink(sequence_path) != 0) return fail("cannot remove sequence fixture", strerror(errno));
    if (chrono_supervisor_protect(directory, argv, &loaded, error, sizeof(error)) == 0 ||
        strstr(error, "sequence is missing") == NULL)
        return fail("missing sequence with records was not rejected", error);
    listed.count = 0;
    if (chrono_supervisor_list(directory, collect, &listed, error, sizeof(error)) != 0)
        return fail("registry lock was not released after missing-sequence failure", error);

    fd = open(sequence_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    const char malformed_sequence[] = "CHRONOSEQ1\ninvalid\n";
    if (fd < 0 || write(fd, malformed_sequence, sizeof(malformed_sequence) - 1) !=
                  (ssize_t)(sizeof(malformed_sequence) - 1) || close(fd) != 0)
        return fail("cannot create malformed sequence fixture", strerror(errno));
    if (chrono_supervisor_protect(directory, argv, &loaded, error, sizeof(error)) == 0 ||
        strstr(error, "malformed") == NULL)
        return fail("malformed sequence was not rejected", error);
    listed.count = 0;
    if (chrono_supervisor_list(directory, collect, &listed, error, sizeof(error)) != 0)
        return fail("registry lock was not released after malformed-sequence failure", error);

    fd = open(sequence_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    const char overflow_sequence[] = "CHRONOSEQ1\n18446744073709551615\n";
    if (fd < 0 || write(fd, overflow_sequence, sizeof(overflow_sequence) - 1) !=
                  (ssize_t)(sizeof(overflow_sequence) - 1) || close(fd) != 0)
        return fail("cannot create overflow sequence fixture", strerror(errno));
    if (chrono_supervisor_protect(directory, argv, &loaded, error, sizeof(error)) == 0 ||
        strstr(error, "sequence exhausted") == NULL)
        return fail("sequence overflow was not rejected", error);
    listed.count = 0;
    if (chrono_supervisor_list(directory, collect, &listed, error, sizeof(error)) != 0)
        return fail("registry lock was not released after overflow failure", error);

    if (snprintf(record_path, sizeof(record_path), "%s/cp-999999.rec", directory) >= (int)sizeof(record_path))
        return fail("test path overflow", NULL);
    fd = open(record_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 || write(fd, "CHRONO-PROCESS-999\n", 19) != 19 || close(fd) != 0)
        return fail("cannot create malformed registry fixture", strerror(errno));
    if (chrono_supervisor_get(directory, "cp-999999", &loaded, error, sizeof(error)) == 0 ||
        strstr(error, "malformed") == NULL)
        return fail("malformed registry record was accepted", error);
    fd = open(record_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0 || write(fd, "CHRONO-PROCESS-2\n", 17) != 17 || close(fd) != 0)
        return fail("cannot replace unsupported-version fixture", strerror(errno));
    if (chrono_supervisor_get(directory, "cp-999999", &loaded, error, sizeof(error)) == 0 ||
        strstr(error, "unsupported version") == NULL)
        return fail("unsupported registry version was accepted", error);
    listed.count = 0;
    if (chrono_supervisor_list(directory, collect, &listed, error, sizeof(error)) == 0)
        return fail("list did not fail closed on malformed registry entry", NULL);

    puts("supervisor registry and lifecycle tests passed");
    return 0;
}
