#define _POSIX_C_SOURCE 200809L
#include "chrono/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int write_file(const char *path, const char *data, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used < length) {
        ssize_t count = write(fd, data + used, length - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            (void)close(fd);
            return -1;
        }
        used += (size_t)count;
    }
    return close(fd);
}

static int expect_unprivileged(void)
{
    const char *value = getenv("CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED");
    if (value == NULL) return -1;
    if (strcmp(value, "auto") == 0) return geteuid() != 0;
    if (strcmp(value, "0") == 0) return 0;
    if (strcmp(value, "1") == 0) return 1;
    return -1;
}

static int arguments_valid(int argc, char **argv, int unprivileged)
{
    int index = 2;
    if (unprivileged && (argc <= index || strcmp(argv[index++], "--unprivileged") != 0)) return 0;
    if (strcmp(argv[1], "check") == 0) return argc == index;
    if (strcmp(argv[1], "dump") == 0) {
        int has_leave_running = argc == index + 6 && strcmp(argv[argc - 1], "--leave-running") == 0;
        return (argc == index + 5 || has_leave_running) &&
               strcmp(argv[index], "--tree") == 0 && argv[index + 1][0] != '\0' &&
               strcmp(argv[index + 2], "--images-dir") == 0 && argv[index + 3][0] != '\0' &&
               strcmp(argv[index + 4], "--shell-job") == 0;
    }
    if (strcmp(argv[1], "restore") == 0) {
        return argc == index + 6 &&
               strcmp(argv[index], "--images-dir") == 0 && argv[index + 1][0] != '\0' &&
               strcmp(argv[index + 2], "--shell-job") == 0 &&
               strcmp(argv[index + 3], "--restore-detached") == 0 &&
               strcmp(argv[index + 4], "--pidfile") == 0 && argv[index + 5][0] != '\0';
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *log_path=getenv("CHRONO_FAKE_CRIU_LOG");
    const char *fail = getenv("CHRONO_FAKE_CRIU_FAIL");
    const char *exit_status = getenv("CHRONO_FAKE_CRIU_EXIT_STATUS");
    const char *signal_operation = getenv("CHRONO_FAKE_CRIU_SIGNAL");
    int expected_mode = expect_unprivileged();
    if (argc < 2 || expected_mode < 0 || !arguments_valid(argc, argv, expected_mode)) return 96;
    if(log_path!=NULL) {
        FILE *log=fopen(log_path,"a");
        if(log==NULL) return 89;
        for (int i=1;i<argc;i++) (void)fprintf(log,"%s%s",i==1?"":"\t",argv[i]);
        (void)fputc('\n',log);
        if(fclose(log)!=0) return 88;
    }
    if (signal_operation != NULL && strcmp(signal_operation, argv[1]) == 0) raise(SIGTERM);
    if (strcmp(argv[1], "check") == 0) {
        if (exit_status != NULL) {
            char *end = NULL;
            if (exit_status[0] == '\0') return 97;
            for (const unsigned char *p = (const unsigned char *)exit_status; *p != '\0'; ++p)
                if (*p < '0' || *p > '9') return 97;
            errno = 0;
            long value = strtol(exit_status, &end, 10);
            if (errno != 0 || end == exit_status || *end != '\0' || value < 0 || value > UCHAR_MAX)
                return 97;
            return (int)value;
        }
        return fail != NULL && strcmp(fail,"check")==0 ? 1 : 0;
    }
    if (fail != NULL && strcmp(fail,argv[1])==0 &&
        !(strcmp(argv[1], "dump") == 0 && getenv("CHRONO_FAKE_CRIU_PARTIAL") != NULL)) return 7;
    const char *dir = NULL, *pidfile = NULL;
    for (int i=2;i+1<argc;i++) {
        if (strcmp(argv[i], "--unprivileged") == 0) continue;
        if(strcmp(argv[i],"--images-dir")==0) dir=argv[i+1];
        if(strcmp(argv[i],"--pidfile")==0) pidfile=argv[i+1];
    }
    if (dir == NULL) return 91;
    if (strcmp(argv[1],"dump")==0) {
        char path[4096];
        if(snprintf(path,sizeof(path),"%s/inventory.img",dir)>=(int)sizeof(path)) return 92;
        if (write_file(path, "fake", 4) != 0) return 93;
        if(snprintf(path,sizeof(path),"%s/core-1.img",dir)>=(int)sizeof(path)) return 92;
        if (write_file(path, "fake-core", 9) != 0) return 93;
        if(snprintf(path,sizeof(path),"%s/stats-dump",dir)>=(int)sizeof(path)) return 92;
        if (write_file(path, "{}\n", 3) != 0) return 93;
        if (getenv("CHRONO_FAKE_CRIU_STOP_TARGET") != NULL) {
            const char *target_text = NULL;
            for (int i = 2; i + 1 < argc; ++i)
                if (strcmp(argv[i], "--tree") == 0) target_text = argv[i + 1];
            if (target_text == NULL || target_text[0] == '\0') return 94;
            errno = 0; char *end = NULL; long target_long = strtol(target_text, &end, 10);
            if (errno != 0 || end == target_text || *end != '\0' || target_long <= 1 || target_long > INT_MAX)
                return 94;
            chrono_process_info target; char error[256];
            if (chrono_process_inspect((pid_t)target_long, &target, error, sizeof(error)) != 0 ||
                target.owner != getuid() ||
                chrono_managed_stop(target.pid, target.start_time, error, sizeof(error)) != 0)
                return 94;
        }
        if(fail != NULL && strcmp(fail,"dump")==0) return 7;
        return 0;
    }
    if (strcmp(argv[1],"restore")==0 && pidfile != NULL) {
        const char *pidmode=getenv("CHRONO_FAKE_CRIU_PIDFILE");
        if(pidmode!=NULL && strcmp(pidmode,"missing")==0) return 0;
        const char *value = "424242\n";
        if (pidmode != NULL && strcmp(pidmode, "malformed") == 0) value = "bad\n";
        else if (pidmode != NULL && strcmp(pidmode, "trailing") == 0) value = "424242\nextra\n";
        else if (pidmode != NULL && strcmp(pidmode, "oversized") == 0)
            value = "9999999999999999999999999999999999999999999999999999999999999999\n";
        size_t len=strlen(value);
        if (write_file(pidfile, value, len) != 0) return 94;
        return 0;
    }
    return 95;
}
