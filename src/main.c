#define _POSIX_C_SOURCE 200809L
#include "chrono/process.h"
#include "chrono/checkpoint.h"
#include "chrono/checkpoint_criu.h"
#include "chrono/demo.h"
#include "chrono/supervisor.h"
#include "chrono/supervisor_service.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s inspect PID | launch counter [interval 1..60] | status PID STARTTIME | counter PID CURRENT_STARTTIME [CHECKPOINT_STARTTIME] | stop PID STARTTIME | protect -- PROGRAM [ARG ...] | protect --checkpoint-interval 1..3600 -- PROGRAM [ARG ...] | protect --counter [--checkpoint-interval 1..3600] | list | status CHRONO_PROCESS_ID | stop CHRONO_PROCESS_ID | supervisor run|status|shutdown | checkpoints | checkpoint-latest CHRONO_PROCESS_ID | checkpoint PID STARTTIME ID [--leave-running] | restore ID | dashboard | demo | demo-interactive | demo-manual\n", program);
}

static int supervisor_directory(char *directory, size_t size, char *error, size_t error_size)
{
    return chrono_supervisor_default_directory(directory, size, error, error_size);
}

static void print_safe_field(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p >= 0x20 && *p != 0x7f) putchar(*p);
        else printf("\\x%02x", *p);
    }
}

static void print_supervised_process(const chrono_supervised_process *process)
{
    printf("ChronoOS Process ID: %s\nLinux PID: %ld\nStart Time: %llu\nUID: %lu\nState: %s\n",
           process->id, (long)process->pid, (unsigned long long)process->start_time,
           (unsigned long)process->owner, chrono_supervisor_state_name(process->state));
    printf("Executable: "); print_safe_field(process->executable); putchar('\n');
    printf("Command line: "); print_safe_field(process->command); putchar('\n');
    printf("Working directory: "); print_safe_field(process->working_directory); putchar('\n');
    printf("Registered: %lld\nState changed: %lld\nCheckpoint interval: %u seconds\nIntentional stop: %s\n",
           (long long)process->registered_at, (long long)process->state_changed_at,
           process->checkpoint_interval_seconds,
           process->intentional_stop ? "yes" : "no");
    if (process->exit_status_known) printf("Exit code: %d\n", process->exit_code);
    else if (process->term_signal != 0) printf("Termination signal: %d\n", process->term_signal);
    else if (process->state == CHRONO_SUPERVISOR_FAILED)
        puts("Exit status: unavailable (process ended or identity changed while ChronoOS was not its parent)");
    if (process->checkpoint_directory[0] != '\0') {
        printf("Checkpoint directory: "); print_safe_field(process->checkpoint_directory); putchar('\n');
    } else puts("Checkpoint directory: not assigned");
}

typedef struct { size_t count; } list_context;

static int print_supervised_row(const chrono_supervised_process *process, void *opaque)
{
    list_context *context = opaque;
    printf("%-14s %-8ld %-12llu %-9s ", process->id, (long)process->pid,
           (unsigned long long)process->start_time, chrono_supervisor_state_name(process->state));
    print_safe_field(process->command); putchar('\n'); ++context->count; return 0;
}

static int print_checkpoint(const chrono_checkpoint_metadata *m, void *context)
{
    const char *directory = context;
    printf("%s METADATA_STATE=COMPLETE ARTIFACT=%s PID=%ld STARTTIME=%llu CREATED=%lld BACKEND=%s WORKLOAD=%s EXECUTABLE=%s\n",
           m->id, chrono_checkpoint_artifact_valid(directory, m->id) ? "valid" : "missing-or-invalid",
           (long)m->original_pid, (unsigned long long)m->process_start_time,
           (long long)m->created_at, m->backend, m->workload, m->executable);
    return 0;
}

static int checkpoint_directory(char *path, size_t size)
{
    const char *configured = getenv("CHRONO_CHECKPOINT_DIR");
    const char *directory = configured != NULL && configured[0] != '\0' ? configured : NULL;
    if (directory != NULL) {
        if (directory[0] != '/') return -1;
        int n = snprintf(path, size, "%s", directory);
        return n < 0 || (size_t)n >= size ? -1 : 0;
    }
    int n=snprintf(path,size,"/tmp/chronoos-checkpoints-%lu",(unsigned long)getuid());
    return n<0 || (size_t)n>=size ? -1 : 0;
}

static int number(const char *s, unsigned long long max, unsigned long long *out)
{
    if (s == NULL || *s == '\0') return -1;
    for (const char *p=s; *p; ++p) if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end; unsigned long long n = strtoull(s, &end, 10);
    if (errno || *end || n == 0 || n > max) return -1;
    *out = n; return 0;
}

int main(int argc, char **argv)
{
    long parsed_pid;
    char error[256] = {0};
    chrono_process_info info;

    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    if (argc == 2 && (strcmp(argv[1], "demo") == 0 ||
                      strcmp(argv[1], "demo-interactive") == 0)) return chrono_demo_run();
    if (argc == 2 && strcmp(argv[1], "dashboard") == 0)
        return chrono_dashboard_menu_run();
    if (argc == 2 && strcmp(argv[1], "demo-manual") == 0)
        return chrono_demo_manual_run();
    if (argc == 3 && (strcmp(argv[1], "dashboard") == 0 || strcmp(argv[1], "demo-manual") == 0) &&
        strcmp(argv[2], "--help") == 0) {
        if (strcmp(argv[1], "dashboard") == 0)
            puts("Usage: chrono dashboard\nLaunch and monitor the bundled counter, create a real CRIU checkpoint, confirm termination, restore, verify, and clean up.");
        else
            puts("Usage: chrono demo-manual\nRun the separate key-driven bundled-counter checkpoint/restore walkthrough.");
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "dashboard") == 0 || strcmp(argv[1], "demo-manual") == 0)) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 3 && (strcmp(argv[1], "demo") == 0 ||
                      strcmp(argv[1], "demo-interactive") == 0) &&
        strcmp(argv[2], "--help") == 0) {
        puts("Usage: chrono demo | chrono demo-interactive\nRuns the bounded managed-counter checkpoint/restore demonstration.");
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "demo") == 0 ||
                      strcmp(argv[1], "demo-interactive") == 0)) {
        usage(argv[0]);
        return 2;
    }

    if (argc >= 2 && strcmp(argv[1], "protect") == 0) {
        if (argc >= 3 && strcmp(argv[2], "--counter") == 0) {
            unsigned long long interval = CHRONO_CHECKPOINT_INTERVAL_DEFAULT;
            if ((argc != 3 && (argc != 5 || strcmp(argv[3], "--checkpoint-interval") != 0 ||
                               number(argv[4], CHRONO_CHECKPOINT_INTERVAL_MAX, &interval) != 0)) ||
                interval < CHRONO_CHECKPOINT_INTERVAL_MIN) {
                fprintf(stderr, "chrono: usage: protect --counter [--checkpoint-interval 1..3600]\n");
                return 2;
            }
            char directory[CHRONO_SUPERVISOR_PATH_MAX]; chrono_supervised_process process;
            if (supervisor_directory(directory, sizeof(directory), error, sizeof(error)) != 0 ||
                chrono_supervisor_protect_counter(directory, (uint32_t)interval, &process,
                                                  error, sizeof(error)) != 0) {
                fprintf(stderr, "chrono: protect failed: %s\n", error[0] ? error : "unknown registration error");
                return 1;
            }
            print_supervised_process(&process);
            return 0;
        }
        int command_index = 3;
        unsigned long long interval = CHRONO_CHECKPOINT_INTERVAL_DEFAULT;
        if (argc >= 3 && strcmp(argv[2], "--checkpoint-interval") == 0) {
            if (argc < 6 || number(argv[3], CHRONO_CHECKPOINT_INTERVAL_MAX, &interval) != 0 ||
                interval < CHRONO_CHECKPOINT_INTERVAL_MIN || strcmp(argv[4], "--") != 0) {
                fprintf(stderr, "chrono: invalid checkpoint interval or protect syntax\n");
                return 2;
            }
            command_index = 5;
        } else if (argc < 4 || strcmp(argv[2], "--") != 0) {
            usage(argv[0]); return 2;
        }
        char directory[CHRONO_SUPERVISOR_PATH_MAX]; chrono_supervised_process process;
        if (supervisor_directory(directory, sizeof(directory), error, sizeof(error)) != 0 ||
            chrono_supervisor_protect_with_interval(directory, &argv[command_index], (uint32_t)interval,
                                                    &process, error, sizeof(error)) != 0) {
            fprintf(stderr, "chrono: protect failed: %s\n", error[0] ? error : "unknown registration error"); return 1;
        }
        print_supervised_process(&process);
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "supervisor") == 0) {
        char directory[CHRONO_SUPERVISOR_PATH_MAX];
        if (supervisor_directory(directory, sizeof(directory), error, sizeof(error)) != 0) {
            fprintf(stderr, "chrono: cannot initialize supervisor state: %s\n", error);
            return 1;
        }
        if (strcmp(argv[2], "run") == 0) {
            if (chrono_supervisor_service_run(directory, error, sizeof(error)) != 0) {
                fprintf(stderr, "chrono: supervisor failed: %s\n", error[0] ? error : "unknown error");
                return 1;
            }
            return 0;
        }
        if (strcmp(argv[2], "status") == 0 || strcmp(argv[2], "shutdown") == 0) {
            char response[128];
            const char *request = strcmp(argv[2], "status") == 0 ? "STATUS" : "SHUTDOWN";
            if (chrono_supervisor_service_request(directory, request, response,
                                                  sizeof(response), error, sizeof(error)) != 0) {
                fprintf(stderr, "chrono: supervisor request failed: %s\n", error[0] ? error : "unknown error");
                return 1;
            }
            puts(response);
            return 0;
        }
    }

    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        char directory[CHRONO_SUPERVISOR_PATH_MAX]; list_context context = {0};
        if (supervisor_directory(directory, sizeof(directory), error, sizeof(error)) != 0) {
            fprintf(stderr, "chrono: %s\n", error); return 1;
        }
        puts("ChronoOS ID    Linux PID Start Time   State     Command");
        if (chrono_supervisor_list(directory, print_supervised_row, &context, error, sizeof(error)) != 0) {
            fprintf(stderr, "chrono: cannot list protected processes: %s\n", error); return 1;
        }
        if (context.count == 0) puts("(no registered processes)");
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "status") == 0 && chrono_supervisor_id_valid(argv[2])) {
        char directory[CHRONO_SUPERVISOR_PATH_MAX]; chrono_supervised_process process;
        if (supervisor_directory(directory, sizeof(directory), error, sizeof(error)) != 0 ||
            chrono_supervisor_get(directory, argv[2], &process, error, sizeof(error)) != 0) {
            fprintf(stderr, "chrono: cannot read protected process: %s\n", error); return 1;
        }
        print_supervised_process(&process); return 0;
    }

    if (argc == 3 && strcmp(argv[1], "stop") == 0 && chrono_supervisor_id_valid(argv[2])) {
        char directory[CHRONO_SUPERVISOR_PATH_MAX]; chrono_supervised_process process;
        if (supervisor_directory(directory, sizeof(directory), error, sizeof(error)) != 0 ||
            chrono_supervisor_stop(directory, argv[2], &process, error, sizeof(error)) != 0) {
            fprintf(stderr, "chrono: cannot stop protected process: %s\n", error); return 1;
        }
        printf("%s stopped; state=%s\n", process.id, chrono_supervisor_state_name(process.state)); return 0;
    }

    if (argc == 2 && strcmp(argv[1], "checkpoints") == 0) {
        char directory[128];
        if (checkpoint_directory(directory,sizeof(directory))!=0) {
            fprintf(stderr,"chrono: invalid or too-long checkpoint directory path\n"); return 1;
        }
        if (chrono_checkpoint_list(directory,print_checkpoint,directory,error,sizeof(error))!=0) {
            if (error[0] == '\0') (void)snprintf(error,sizeof(error),"cannot read checkpoint metadata");
            fprintf(stderr,"chrono: %s\n",error); return 1;
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "checkpoint-latest") == 0 &&
        chrono_supervisor_id_valid(argv[2])) {
        char directory[128]; chrono_checkpoint_metadata metadata;
        if (checkpoint_directory(directory, sizeof(directory)) != 0 ||
            chrono_checkpoint_latest(directory, argv[2], &metadata, error, sizeof(error)) != 0) {
            fprintf(stderr, "chrono: cannot resolve latest valid checkpoint: %s\n",
                    error[0] ? error : "no valid checkpoint");
            return 1;
        }
        print_checkpoint(&metadata, directory);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "checkpoint") == 0) {
        unsigned long long pid, start;
        int leave_running = argc == 6 && strcmp(argv[5], "--leave-running") == 0;
        if ((argc != 5 && !leave_running) || number(argv[2],2147483647ULL,&pid) != 0 ||
            number(argv[3],ULLONG_MAX,&start) != 0 || !chrono_checkpoint_id_valid(argv[4])) {
            usage(argv[0]); return 2;
        }
        int process_fd=chrono_managed_pin((pid_t)pid,(uint64_t)start,error,sizeof(error));
        if (process_fd < 0) {
            fprintf(stderr,"chrono: managed identity validation failed: %s\n",error); return 1;
        }
        chrono_process_info managed_info;
        if (chrono_process_inspect((pid_t)pid, &managed_info, error, sizeof(error)) != 0 ||
            managed_info.start_time != (uint64_t)start || managed_info.owner != getuid() ||
            managed_info.executable[0] == '\0' || managed_info.executable_inode == 0) {
            close(process_fd);
            fprintf(stderr, "chrono: managed executable identity could not be verified: %s\n",
                    error[0] ? error : "identity mismatch");
            return 1;
        }
        char directory[128];
        if (checkpoint_directory(directory,sizeof(directory))!=0) { close(process_fd); fprintf(stderr,"chrono: checkpoint path too long\n"); return 1; }
        chrono_checkpoint_metadata m = {0};
        (void)snprintf(m.id,sizeof(m.id),"%s",argv[4]); m.original_pid=(pid_t)pid;
        m.process_start_time=(uint64_t)start; (void)snprintf(m.workload,sizeof(m.workload),"counter");
        int executable_length = snprintf(m.executable, sizeof(m.executable), "%s",
                                         managed_info.executable);
        if (executable_length < 0 || (size_t)executable_length >= sizeof(m.executable)) {
            close(process_fd);
            fprintf(stderr, "chrono: managed executable path exceeds checkpoint metadata capacity\n");
            return 1;
        }
        (void)snprintf(m.backend,sizeof(m.backend),"criu"); m.state=CHRONO_CHECKPOINT_PREPARING;
        if (chrono_checkpoint_begin(directory,&m,error,sizeof(error))!=0) { close(process_fd); fprintf(stderr,"chrono: %s\n",error); return 1; }
        const chrono_checkpoint_backend *backend=chrono_checkpoint_backend_get();
        /* CRIU receives a numeric PID, not the pidfd. Recheck immediately before dispatch. */
        if (chrono_managed_status((pid_t)pid,(uint64_t)start,error,sizeof(error))!=0) {
            close(process_fd);
            char transition_error[256];
            if (chrono_checkpoint_transition(directory,m.id,CHRONO_CHECKPOINT_FAILED,
                                             transition_error,sizeof(transition_error))!=0)
                fprintf(stderr,"chrono: managed identity changed before dump: %s (metadata failure: %s)\n",
                        error,transition_error);
            else fprintf(stderr,"chrono: managed identity changed before dump: %s\n",error);
            return 1;
        }
        int result=leave_running ? backend->checkpoint_live(&m,directory,error,sizeof(error)) :
                                   backend->checkpoint(&m,directory,error,sizeof(error));
        close(process_fd);
        if (result==0 && chrono_checkpoint_transition(directory,m.id,CHRONO_CHECKPOINT_COMPLETE,error,sizeof(error))==0) {
            printf(leave_running ?
                   "checkpoint %s completed; CRIU captured the workload with --leave-running\n" :
                   "checkpoint %s completed; CRIU captured the managed process tree\n",m.id);
            return 0;
        }
        if (result==0) (void)snprintf(error,sizeof(error),"artifact captured but metadata completion failed");
        char transition_error[256];
        if (chrono_checkpoint_transition(directory,m.id,CHRONO_CHECKPOINT_FAILED,transition_error,sizeof(transition_error))!=0)
            fprintf(stderr,"chrono: checkpoint failed: %s (metadata failure: %s)\n",error,transition_error);
        else fprintf(stderr,"chrono: checkpoint failed: %s\n",error);
        return 1;
    }
    if (argc >= 2 && strcmp(argv[1], "restore") == 0) {
        if (argc != 3 || !chrono_checkpoint_id_valid(argv[2])) { usage(argv[0]); return 2; }
        char directory[128]; chrono_checkpoint_metadata m; pid_t restored;
        if (checkpoint_directory(directory,sizeof(directory))!=0) {
            fprintf(stderr,"chrono: invalid or too-long checkpoint directory path\n"); return 1;
        }
        if (chrono_checkpoint_read(directory,argv[2],&m,error,sizeof(error))!=0) {
            if (error[0] == '\0') (void)snprintf(error,sizeof(error),"cannot read checkpoint metadata");
            fprintf(stderr,"chrono: %s\n",error); return 1;
        }
        if (m.state!=CHRONO_CHECKPOINT_COMPLETE) { fprintf(stderr,"chrono: checkpoint is not complete\n"); return 1; }
        if (strcmp(m.backend,"criu")!=0) { fprintf(stderr,"chrono: checkpoint metadata uses an unsupported backend\n"); return 1; }
        const chrono_checkpoint_backend *backend=chrono_checkpoint_backend_get();
        if (backend->restore(&m,directory,&restored,error,sizeof(error))!=0) { fprintf(stderr,"chrono: %s\n",error); return 1; }
        chrono_process_info restored_info;
        if (chrono_process_inspect(restored, &restored_info, error, sizeof(error)) != 0 ||
            restored_info.owner != getuid() || restored_info.state == 'Z' ||
            restored_info.executable[0] == '\0' || restored_info.start_time == 0 ||
            strcmp(restored_info.executable, m.executable) != 0) {
            fprintf(stderr, "chrono: restore returned, but the process identity is no longer verifiable: %s\n",
                error[0] ? error : "identity mismatch");
            return 1;
        }
        printf("restored PID: %ld\nSTARTTIME: %llu\nUID: %lu\nExecutable: ",
               (long)restored_info.pid, (unsigned long long)restored_info.start_time,
               (unsigned long)restored_info.owner);
        print_safe_field(restored_info.executable);
        putchar('\n');
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "launch") == 0) {
        if ((argc != 3 && argc != 4) || strcmp(argv[2], "counter") != 0) { usage(argv[0]); return 2; }
        unsigned long long interval=1;
        if (argc == 4 && (number(argv[3],60,&interval) != 0 || interval < 1)) { fprintf(stderr,"chrono: invalid interval\n"); return 2; }
        char error[256];
        pid_t pid; uint64_t start;
        if (chrono_managed_start_bundled((long)interval,&pid,&start,error,sizeof(error)) != 0) { fprintf(stderr,"chrono: %s\n",error); return 1; }
        printf("PID: %ld\nSTARTTIME: %llu\n",(long)pid,(unsigned long long)start); return 0;
    }
    if ((argc == 4) && (strcmp(argv[1],"status")==0 || strcmp(argv[1],"stop")==0)) {
        unsigned long long p, start;
        if (number(argv[2],2147483647ULL,&p) != 0 || number(argv[3],ULLONG_MAX,&start) != 0) { fprintf(stderr,"chrono: invalid managed identity\n"); return 2; }
        char error[256];
        int result = strcmp(argv[1],"status")==0 ? chrono_managed_status((pid_t)p,(uint64_t)start,error,sizeof(error)) : chrono_managed_stop((pid_t)p,(uint64_t)start,error,sizeof(error));
        if (result != 0) { fprintf(stderr,"chrono: %s\n",error); return 1; }
        puts(strcmp(argv[1],"status")==0 ? "running" : "termination requested"); return 0;
    }
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "counter") == 0) {
        unsigned long long parsed_process, parsed_start, parsed_checkpoint_start, counter;
        if (number(argv[2], 2147483647ULL, &parsed_process) != 0 ||
            number(argv[3], ULLONG_MAX, &parsed_start) != 0) {
            fprintf(stderr, "chrono: invalid managed counter identity\n");
            return 2;
        }
        parsed_checkpoint_start = parsed_start;
        if (argc == 5 && number(argv[4], ULLONG_MAX, &parsed_checkpoint_start) != 0) {
            fprintf(stderr, "chrono: invalid checkpoint start time for counter query\n");
            return 2;
        }
        if (chrono_managed_counter_query((pid_t)parsed_process, (uint64_t)parsed_start,
                                         (uint64_t)parsed_checkpoint_start, &counter,
                                         error, sizeof(error)) != 0) {
            fprintf(stderr, "chrono: %s\n", error);
            return 1;
        }
        printf("COUNTER: %llu\n", counter);
        return 0;
    }
    if (argc != 3 || argv[1][0] == '\0' || argv[1][0] == '-') {
        usage(argv[0]);
        return 2;
    }
    unsigned long long parsed;
    if (number(argv[2],2147483647ULL,&parsed) != 0) {
        fprintf(stderr, "chrono: invalid PID '%s'\n", argv[2]);
        usage(argv[0]);
        return 2;
    }
    parsed_pid = (long)parsed;
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
