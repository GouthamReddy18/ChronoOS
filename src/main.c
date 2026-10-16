#define _POSIX_C_SOURCE 200809L
#include "chrono/process.h"
#include "chrono/checkpoint.h"
#include "chrono/checkpoint_criu.h"

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
    fprintf(stderr, "Usage: %s inspect PID | launch counter [interval 1..60] | status PID STARTTIME | stop PID STARTTIME | checkpoints | checkpoint PID STARTTIME ID | restore ID\n", program);
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
    char error[256];
    chrono_process_info info;

    if (argc == 2 && strcmp(argv[1], "checkpoints") == 0) {
        char directory[128];
        if (checkpoint_directory(directory,sizeof(directory))!=0 ||
            chrono_checkpoint_list(directory,print_checkpoint,directory,error,sizeof(error))!=0) {
            fprintf(stderr,"chrono: %s\n",error); return 1;
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "checkpoint") == 0) {
        unsigned long long pid, start;
        if (argc != 5 || number(argv[2],2147483647ULL,&pid) != 0 ||
            number(argv[3],ULLONG_MAX,&start) != 0 || !chrono_checkpoint_id_valid(argv[4])) {
            usage(argv[0]); return 2;
        }
        int process_fd=chrono_managed_pin((pid_t)pid,(uint64_t)start,error,sizeof(error));
        if (process_fd < 0) {
            fprintf(stderr,"chrono: managed identity validation failed: %s\n",error); return 1;
        }
        char directory[128];
        if (checkpoint_directory(directory,sizeof(directory))!=0) { close(process_fd); fprintf(stderr,"chrono: checkpoint path too long\n"); return 1; }
        chrono_checkpoint_metadata m = {0};
        (void)snprintf(m.id,sizeof(m.id),"%s",argv[4]); m.original_pid=(pid_t)pid;
        m.process_start_time=(uint64_t)start; (void)snprintf(m.workload,sizeof(m.workload),"counter");
        (void)snprintf(m.executable,sizeof(m.executable),"counter_workload");
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
        int result=backend->checkpoint(&m,directory,error,sizeof(error));
        close(process_fd);
        if (result==0 && chrono_checkpoint_transition(directory,m.id,CHRONO_CHECKPOINT_COMPLETE,error,sizeof(error))==0) {
            printf("checkpoint %s completed; CRIU captured the managed process tree\n",m.id); return 0;
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
        if (checkpoint_directory(directory,sizeof(directory))!=0 ||
            chrono_checkpoint_read(directory,argv[2],&m,error,sizeof(error))!=0) { fprintf(stderr,"chrono: %s\n",error); return 1; }
        if (m.state!=CHRONO_CHECKPOINT_COMPLETE) { fprintf(stderr,"chrono: checkpoint is not complete\n"); return 1; }
        if (strcmp(m.backend,"criu")!=0) { fprintf(stderr,"chrono: checkpoint metadata uses an unsupported backend\n"); return 1; }
        const chrono_checkpoint_backend *backend=chrono_checkpoint_backend_get();
        if (backend->restore(&m,directory,&restored,error,sizeof(error))!=0) { fprintf(stderr,"chrono: %s\n",error); return 1; }
        printf("restored PID: %ld (the restored process receives a new PID)\n",(long)restored); return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "launch") == 0) {
        if ((argc != 3 && argc != 4) || strcmp(argv[2], "counter") != 0) { usage(argv[0]); return 2; }
        unsigned long long interval=1;
        if (argc == 4 && (number(argv[3],60,&interval) != 0 || interval < 1)) { fprintf(stderr,"chrono: invalid interval\n"); return 2; }
        char self[PATH_MAX], counter[PATH_MAX], error[256];
        ssize_t self_len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (self_len < 0 || (size_t)self_len >= sizeof(self) - 1) { fprintf(stderr,"chrono: cannot resolve ChronoOS executable\n"); return 1; }
        self[self_len] = '\0';
        char *slash = strrchr(self, '/');
        if (slash == NULL) { fprintf(stderr,"chrono: invalid resolved executable path\n"); return 1; }
        *slash = '\0';
        if (snprintf(counter,sizeof(counter),"%s/counter_workload",self) >= (int)sizeof(counter)) { fprintf(stderr,"chrono: workload path too long\n"); return 1; }
        struct stat workload_stat;
        if (lstat(counter, &workload_stat) != 0 || !S_ISREG(workload_stat.st_mode) || workload_stat.st_uid != getuid() || (workload_stat.st_mode & 0111) == 0) {
            fprintf(stderr,"chrono: bundled counter workload is missing or invalid\n"); return 1;
        }
        pid_t pid; uint64_t start;
        if (chrono_managed_start(counter,(long)interval,&pid,&start,error,sizeof(error)) != 0) { fprintf(stderr,"chrono: %s\n",error); return 1; }
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
