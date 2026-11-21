#define _POSIX_C_SOURCE 200809L
#include "chrono/checkpoint_criu.h"
#include "chrono/process.h"
#include "chrono/process_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>

static pid_t managed_pid = -1;
static uint64_t managed_start;
static int stop_managed_test_workload(void)
{
    if (managed_pid <= 1) return 0;
    char error[128];
    if (chrono_managed_stop(managed_pid, managed_start, error, sizeof(error)) != 0) {
        fprintf(stderr, "test_criu: refusing unsafe workload cleanup: %s\n", error);
        return -1;
    }
    int status = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        pid_t waited = waitpid(managed_pid, &status, WNOHANG);
        if (waited == managed_pid || (waited < 0 && errno == ECHILD)) {
            managed_pid = -1;
            return 0;
        }
        if (waited < 0 && errno != EINTR) break;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    fprintf(stderr, "test_criu: managed workload did not exit after verified stop; identity retained\n");
    return -1;
}

static void cleanup_managed_test_workload(void)
{
    (void)stop_managed_test_workload();
}

static int fail(const char *s) { fprintf(stderr,"test_criu: %s\n",s); return 1; }
static void remove_tree(const char *path)
{
    DIR *d=opendir(path); if(d==NULL) return;
    struct dirent *e; char item[4096];
    while((e=readdir(d))!=NULL) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        struct stat st;
        if(snprintf(item,sizeof(item),"%s/%s",path,e->d_name)>0 && lstat(item,&st)==0) {
            if(S_ISDIR(st.st_mode)) { remove_tree(item); (void)rmdir(item); }
            else (void)unlink(item);
        }
    }
    closedir(d); (void)rmdir(path);
}

static int reject_malformed_control_request(char *error, size_t error_size)
{
    char endpoint[CHRONO_CONTROL_NAME_MAX];
    if (chrono_managed_control_endpoint(managed_pid, managed_start, endpoint,
                                        sizeof(endpoint), error,
                                        (unsigned long)error_size) != 0) return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t length = strlen(endpoint);
    if (length + 1 > sizeof(address.sun_path)) return -1;
    address.sun_path[0] = '\0';
    memcpy(address.sun_path + 1, endpoint, length);
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + length);
    int fd = -1;
    for (int attempt = 0; attempt < 30; ++attempt) {
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&address, address_length) == 0) break;
        close(fd);
        fd = -1;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    if (fd < 0) return -1;
    chrono_control_request request = {
        .magic = CHRONO_CONTROL_MAGIC,
        .version = CHRONO_CONTROL_VERSION + 1,
        .operation = CHRONO_CONTROL_AUTHORIZE,
        .target_pid = (int32_t)managed_pid,
        .reserved = 0,
        .target_start_time = managed_start
    };
    if (send(fd, &request, sizeof(request), 0) != (ssize_t)sizeof(request)) {
        close(fd);
        return -1;
    }
    chrono_control_response response;
    ssize_t received = recv(fd, &response, sizeof(response), 0);
    close(fd);
    return received == (ssize_t)sizeof(response) && response.magic == CHRONO_CONTROL_MAGIC &&
           response.status != 0 && response.error_number == EPERM ? 0 : -1;
}
int main(int argc,char **argv)
{
    if(argc!=2) return fail("fake executable path required");
    char base[]="/tmp/chrono-criu-test-XXXXXX", error[256];
    if(mkdtemp(base)==NULL || chmod(base,0700)!=0) return fail("temporary directory setup failed");
    (void)setenv("CHRONO_CRIU_PATH",argv[1],1);
    (void)setenv("CHRONO_TEST_ALLOW_FAKE_CRIU","1",1);
    (void)setenv("CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED","auto",1);
    (void)unsetenv("CHRONO_CRIU_UNPRIVILEGED");
    (void)unsetenv("CHRONO_FAKE_CRIU_FAIL");
    (void)unsetenv("CHRONO_FAKE_CRIU_LOG");
    (void)unsetenv("CHRONO_FAKE_CRIU_EXIT_STATUS");
    (void)unsetenv("CHRONO_FAKE_CRIU_SIGNAL");
    (void)unsetenv("CHRONO_FAKE_CRIU_PARTIAL");
    (void)unsetenv("CHRONO_FAKE_CRIU_PIDFILE");
    (void)atexit(cleanup_managed_test_workload);
    char inherited_path[4096];
    int inherited_path_length = snprintf(inherited_path, sizeof(inherited_path),
                                         "%s/inherited-fd-sentinel", base);
    if (inherited_path_length < 0 || (size_t)inherited_path_length >= sizeof(inherited_path)) {
        remove_tree(base);
        return fail("inherited descriptor sentinel path is too long");
    }
    int inherited_fd = open(inherited_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (inherited_fd < 0) {
        remove_tree(base);
        return fail("could not create inheritable descriptor sentinel");
    }
    if (chrono_managed_start_bundled(1, &managed_pid, &managed_start,
                                     error, sizeof(error)) != 0) {
        close(inherited_fd);
        remove_tree(base);
        return fail("could not launch managed workload for fake adapter authorization tests");
    }
    char child_fd_path[96], child_fd_target[4096];
    int fd_path_length = snprintf(child_fd_path, sizeof(child_fd_path), "/proc/%ld/fd/%d",
                                  (long)managed_pid, inherited_fd);
    ssize_t target_length = fd_path_length < 0 || (size_t)fd_path_length >= sizeof(child_fd_path) ?
                            -1 : readlink(child_fd_path, child_fd_target, sizeof(child_fd_target) - 1);
    int sentinel_inherited = 0;
    if (target_length >= 0) {
        child_fd_target[target_length] = '\0';
        sentinel_inherited = strcmp(child_fd_target, inherited_path) == 0;
    }
    int target_errno = errno;
    close(inherited_fd);
    if (sentinel_inherited || (target_length < 0 && target_errno != ENOENT)) {
        remove_tree(base);
        return fail("managed counter inherited an unrelated parent file descriptor");
    }
    if (reject_malformed_control_request(error, sizeof(error)) != 0 ||
        chrono_managed_status(managed_pid, managed_start, error, sizeof(error)) != 0) {
        remove_tree(base);
        return fail("malformed control request was not rejected while preserving the workload");
    }
    unsigned long long initial_counter = 0, current_counter = 0;
    if (chrono_managed_counter_query(managed_pid, managed_start, managed_start,
                                     &initial_counter,
                                     error, sizeof(error)) != 0) {
        remove_tree(base);
        return fail("authenticated query could not read the workload's in-memory counter");
    }
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (chrono_managed_counter_query(managed_pid, managed_start, managed_start,
                         &current_counter,
                                         error, sizeof(error)) == 0 && current_counter > initial_counter)
            break;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    if (current_counter <= initial_counter) {
        remove_tree(base);
        return fail("authenticated in-memory counter query did not observe progression");
    }
    if (chrono_managed_counter_query(managed_pid, managed_start + 1, managed_start,
                                     &current_counter, error, sizeof(error)) == 0) {
        remove_tree(base);
        return fail("counter query accepted a mismatched current process start time");
    }
    (void)setenv("CHRONO_FAKE_CRIU_SIGNAL","check",1);
    if(chrono_criu_available(error,sizeof(error)) ||
       strstr(error,"CRIU kernel feature check terminated by signal 15")==NULL) {
        remove_tree(base); return fail("signal termination was not reported accurately");
    }
    (void)unsetenv("CHRONO_FAKE_CRIU_SIGNAL");
    if(!chrono_criu_available(error,sizeof(error))) { remove_tree(base); return fail("fake CRIU availability check failed"); }
    (void)setenv("CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED","0",1);
    (void)setenv("CHRONO_CRIU_UNPRIVILEGED","0",1);
    if(!chrono_criu_available(error,sizeof(error))) { remove_tree(base); return fail("explicit default CRIU mode failed"); }
    (void)unsetenv("CHRONO_CRIU_UNPRIVILEGED");
    (void)setenv("CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED","auto",1);
    (void)setenv("CHRONO_FAKE_CRIU_EXIT_STATUS","127",1);
    if(chrono_criu_available(error,sizeof(error)) ||
       strstr(error,"kernel feature check failed (exit status 127)")==NULL ||
       strstr(error,"cannot execute CRIU")!=NULL) {
        remove_tree(base); return fail("genuine CRIU exit status 127 was confused with exec failure");
    }
    (void)unsetenv("CHRONO_FAKE_CRIU_EXIT_STATUS");
    (void)setenv("CHRONO_FAKE_CRIU_EXIT_STATUS","999999999999999999999999999999",1);
    if(chrono_criu_available(error,sizeof(error)) ||
       strstr(error,"kernel feature check failed (exit status 97)")==NULL) {
        remove_tree(base); return fail("fake CRIU accepted an out-of-range exit status");
    }
    (void)setenv("CHRONO_FAKE_CRIU_EXIT_STATUS","+1",1);
    if(chrono_criu_available(error,sizeof(error)) ||
       strstr(error,"kernel feature check failed (exit status 97)")==NULL) {
        remove_tree(base); return fail("fake CRIU accepted malformed exit status syntax");
    }
    (void)unsetenv("CHRONO_FAKE_CRIU_EXIT_STATUS");
    if(!chrono_criu_available(NULL,0)) { remove_tree(base); return fail("null zero-size diagnostic buffer was not supported"); }
    (void)setenv("CHRONO_FAKE_CRIU_EXIT_STATUS","1",1);
    if(chrono_criu_available(NULL,0)) { remove_tree(base); return fail("failed availability check mishandled null diagnostic buffer"); }
    (void)unsetenv("CHRONO_FAKE_CRIU_EXIT_STATUS");
    chrono_checkpoint_metadata m={0}; strcpy(m.id,"sample"); m.original_pid=managed_pid;
    m.process_start_time=managed_start; m.state=CHRONO_CHECKPOINT_PREPARING;
    chrono_process_info managed_info;
    if (chrono_process_inspect(managed_pid, &managed_info, error, sizeof(error)) != 0 ||
        snprintf(m.executable, sizeof(m.executable), "%s", managed_info.executable) < 0 ||
        strlen(managed_info.executable) >= sizeof(m.executable)) {
        remove_tree(base); return fail("managed executable identity could not be recorded for restore checks");
    }
    char artifact[256], linkpath[280];
    if(chrono_criu_checkpoint(&m,base,error,sizeof(error))!=0 ||
       !chrono_checkpoint_artifact_valid(base,m.id)) { remove_tree(base); return fail("fake dump/artifact validation failed"); }
    (void)snprintf(linkpath,sizeof(linkpath),"%s/alias",base);
    if(symlink("sample",linkpath)!=0 || chrono_checkpoint_artifact_valid(base,"alias")) {
        remove_tree(base); return fail("artifact directory symlink was accepted");
    }
    (void)unlink(linkpath);
    (void)snprintf(artifact,sizeof(artifact),"%s/%s",base,m.id);
    (void)snprintf(linkpath,sizeof(linkpath),"%s/extra.img",artifact);
    if(symlink("inventory.img",linkpath)!=0 || chrono_checkpoint_artifact_valid(base,m.id)) {
        remove_tree(base); return fail("artifact symlink was accepted");
    }
    (void)unlink(linkpath);
    (void)snprintf(linkpath,sizeof(linkpath),"%s/unexpected.txt",artifact);
    int extra_fd=open(linkpath,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(extra_fd<0 || close(extra_fd)!=0 || chrono_checkpoint_artifact_valid(base,m.id)) {
        remove_tree(base); return fail("unexpected artifact layout was accepted");
    }
    (void)unlink(linkpath);
    (void)snprintf(linkpath,sizeof(linkpath),"%s/special",artifact);
    if(mkfifo(linkpath,0600)!=0 || chrono_checkpoint_artifact_valid(base,m.id)) {
        remove_tree(base); return fail("artifact special file was accepted");
    }
    (void)unlink(linkpath);
    (void)snprintf(linkpath,sizeof(linkpath),"%s/inventory.img",artifact);
    if(chmod(linkpath,0644)!=0 || chrono_checkpoint_artifact_valid(base,m.id) || chmod(linkpath,0600)!=0) {
        remove_tree(base); return fail("unsafe image permissions were accepted");
    }
    if(chrono_criu_checkpoint(&m,base,error,sizeof(error))==0) { remove_tree(base); return fail("existing artifact directory overwritten"); }
    pid_t restored=0;
    m.state=CHRONO_CHECKPOINT_COMPLETE;
    if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0 ||
         strstr(error,"test-only backend restore output cannot be adopted") == NULL) {
        remove_tree(base); return fail("fake restore PID was accepted without a verified live process");
    }
    if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0) { remove_tree(base); return fail("repeated restore was accepted"); }

    char argument_log[256];
    int argument_log_length=snprintf(argument_log,sizeof(argument_log),"%s/arguments.log",base);
    if(argument_log_length<0 || (size_t)argument_log_length>=sizeof(argument_log)) {
        remove_tree(base); return fail("argument log path construction failed");
    }
    (void)setenv("CHRONO_FAKE_CRIU_LOG",argument_log,1);
    chrono_checkpoint_metadata stale = m;
    strcpy(stale.id, "stale-target");
    stale.state = CHRONO_CHECKPOINT_PREPARING;
    ++stale.process_start_time;
    int stale_result = chrono_criu_checkpoint(&stale, base, error, sizeof(error));
    if (stale_result == 0 || strstr(error, "managed process identity") == NULL) {
        remove_tree(base);
        return fail("stale managed identity was allowed to reach the dump operation");
    }
    (void)snprintf(artifact, sizeof(artifact), "%s/%s", base, stale.id);
    if (access(artifact, F_OK) == 0) {
        remove_tree(base);
        return fail("stale workload identity created a checkpoint artifact directory");
    }
    (void)unlink(argument_log);
    const char *invalid_modes[]={"", "true", "2", "01"};
    for(size_t i=0;i<sizeof(invalid_modes)/sizeof(invalid_modes[0]);i++) {
        (void)setenv("CHRONO_CRIU_UNPRIVILEGED",invalid_modes[i],1);
        if(chrono_criu_available(error,sizeof(error)) ||
           strcmp(error,"invalid CHRONO_CRIU_UNPRIVILEGED (expected 0 or 1)")) {
            remove_tree(base); return fail("invalid unprivileged mode was not rejected");
        }
        strcpy(m.id,"invalid-mode"); m.state=CHRONO_CHECKPOINT_PREPARING;
        if(chrono_criu_checkpoint(&m,base,error,sizeof(error))==0 ||
           strcmp(error,"invalid CHRONO_CRIU_UNPRIVILEGED (expected 0 or 1)")) {
            remove_tree(base); return fail("invalid mode allowed a dump attempt");
        }
        m.state=CHRONO_CHECKPOINT_COMPLETE;
        strcpy(m.id,"sample");
        if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0 ||
           strcmp(error,"invalid CHRONO_CRIU_UNPRIVILEGED (expected 0 or 1)")) {
            remove_tree(base); return fail("invalid mode allowed a restore attempt");
        }
        if(access(argument_log,F_OK)==0) { remove_tree(base); return fail("invalid mode invoked fake CRIU"); }
    }
    (void)setenv("CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED","1",1);
    (void)setenv("CHRONO_CRIU_UNPRIVILEGED","1",1);
    if(!chrono_criu_available(error,sizeof(error))) { remove_tree(base); return fail("unprivileged availability check failed"); }
    strcpy(m.id,"unpriv-mode"); m.state=CHRONO_CHECKPOINT_PREPARING;
    if(chrono_criu_checkpoint(&m,base,error,sizeof(error))!=0) { remove_tree(base); return fail("unprivileged dump invocation failed"); }
    m.state=CHRONO_CHECKPOINT_COMPLETE;
    if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0 ||
         strstr(error,"test-only backend restore output cannot be adopted") == NULL) {
        remove_tree(base); return fail("unprivileged fake restore PID was accepted without identity verification");
    }
    char expected_log[2048], actual_log[2048];
    int expected_length=snprintf(expected_log,sizeof(expected_log),
        "check\t--unprivileged\n"
        "check\t--unprivileged\n"
        "dump\t--unprivileged\t--tree\t%ld\t--images-dir\t%s/unpriv-mode\t--shell-job\n"
        "check\t--unprivileged\n"
        "restore\t--unprivileged\t--images-dir\t%s/unpriv-mode\t--shell-job\t--restore-detached\t--pidfile\t%s/unpriv-mode/restore.pid\n",
        (long)managed_pid,base,base,base);
    FILE *argument_file=fopen(argument_log,"r");
    size_t actual_length=argument_file==NULL?0:fread(actual_log,1,sizeof(actual_log)-1,argument_file);
    int read_error=argument_file==NULL || ferror(argument_file);
    if(argument_file!=NULL && fclose(argument_file)!=0) read_error=1;
    if(actual_length>=sizeof(actual_log) || expected_length<0 || (size_t)expected_length>=sizeof(expected_log) ||
       read_error || actual_length!=(size_t)expected_length) {
        remove_tree(base); return fail("fake CRIU operation log was incomplete");
    }
    actual_log[actual_length]='\0';
    if(strcmp(actual_log,expected_log)!=0) { remove_tree(base); return fail("CRIU argument order or operation identity was incorrect"); }
    (void)unlink(argument_log);
    strcpy(m.id,"live-mode"); m.state=CHRONO_CHECKPOINT_PREPARING;
    if(chrono_criu_checkpoint_leave_running(&m,base,error,sizeof(error))!=0 ||
       chrono_managed_status(managed_pid,managed_start,error,sizeof(error))!=0) {
        remove_tree(base); return fail("leave-running checkpoint failed or stopped the managed workload");
    }
    argument_file=fopen(argument_log,"r");
    actual_length=argument_file==NULL?0:fread(actual_log,1,sizeof(actual_log)-1,argument_file);
    read_error=argument_file==NULL || ferror(argument_file);
    if(argument_file!=NULL && fclose(argument_file)!=0) read_error=1;
    if(read_error || actual_length>=sizeof(actual_log)) { remove_tree(base); return fail("leave-running CRIU log could not be read"); }
    actual_log[actual_length]='\0';
    char live_arguments[768];
    int live_length=snprintf(live_arguments,sizeof(live_arguments),
        "check\t--unprivileged\n"
        "dump\t--unprivileged\t--tree\t%ld\t--images-dir\t%s/live-mode\t--shell-job\t--leave-running\n",
        (long)managed_pid,base);
    if(live_length<0 || (size_t)live_length>=sizeof(live_arguments) ||
       strcmp(actual_log,live_arguments)!=0) {
        remove_tree(base); return fail("leave-running CRIU option or argument order was incorrect");
    }
    (void)unsetenv("CHRONO_FAKE_CRIU_LOG");
    (void)setenv("CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED","0",1);
    (void)unsetenv("CHRONO_CRIU_UNPRIVILEGED");
    (void)setenv("CHRONO_FAKE_CRIU_EXPECT_UNPRIVILEGED","auto",1);

    strcpy(m.id,"restore-failure");
    m.state=CHRONO_CHECKPOINT_PREPARING;
    if(chrono_criu_checkpoint(&m,base,error,sizeof(error))!=0) { remove_tree(base); return fail("restore failure setup dump failed"); }
    m.state=CHRONO_CHECKPOINT_COMPLETE;
    (void)setenv("CHRONO_FAKE_CRIU_FAIL","restore",1);
    if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0 ||
       strstr(error,"partial restoration may have occurred") == NULL ||
       strstr(error,"retry is unsafe") == NULL ||
       chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0) {
        remove_tree(base); return fail("restore warning was lost or failed restore was retried");
    }
    (void)unsetenv("CHRONO_FAKE_CRIU_FAIL");
    strcpy(m.id,"failed"); m.state=CHRONO_CHECKPOINT_PREPARING;
    (void)setenv("CHRONO_FAKE_CRIU_FAIL","dump",1);
    (void)setenv("CHRONO_FAKE_CRIU_PARTIAL","1",1);
    if(chrono_criu_checkpoint(&m,base,error,sizeof(error))==0 ||
       strstr(error,"CRIU dump failed (exit status 7)")==NULL ||
       strstr(error,"partial artifacts retained for inspection")==NULL) {
        remove_tree(base); return fail("failed dump reported success or lost its diagnostic");
    }
    if(!chrono_checkpoint_artifact_valid(base,m.id)) { remove_tree(base); return fail("partial output evidence was not retained"); }
    m.state=CHRONO_CHECKPOINT_FAILED;
    if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0) { remove_tree(base); return fail("restore accepted failed checkpoint metadata"); }
    (void)unsetenv("CHRONO_FAKE_CRIU_PARTIAL");
    strcpy(m.id,"check-failure");
    m.state=CHRONO_CHECKPOINT_PREPARING;
    (void)setenv("CHRONO_FAKE_CRIU_FAIL","check",1);
    if(chrono_criu_checkpoint(&m,base,error,sizeof(error))==0 ||
       strstr(error,"kernel feature check failed (exit status 1)")==NULL) { remove_tree(base); return fail("failed check allowed dump or lost its diagnostic"); }
    (void)snprintf(artifact,sizeof(artifact),"%s/%s",base,m.id);
    if(access(artifact,F_OK)==0) { remove_tree(base); return fail("capability failure created artifact directory"); }
    if(chrono_criu_available(error,sizeof(error)) ||
       strstr(error,"kernel feature check failed (exit status 1)")==NULL) { remove_tree(base); return fail("failed capability check reported available or lost its diagnostic"); }
    (void)unsetenv("CHRONO_FAKE_CRIU_FAIL");
    strcpy(m.id,"restore-missing");
    m.state=CHRONO_CHECKPOINT_PREPARING;
    if(chrono_criu_checkpoint(&m,base,error,sizeof(error))!=0) { remove_tree(base); return fail("missing PID setup dump failed"); }
    m.state=CHRONO_CHECKPOINT_COMPLETE;
    (void)setenv("CHRONO_FAKE_CRIU_PIDFILE","missing",1);
    if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0 || strstr(error,"may have occurred")==NULL ||
       chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0) { remove_tree(base); return fail("missing PID outcome or one-attempt policy failed"); }
    (void)unsetenv("CHRONO_FAKE_CRIU_PIDFILE");
    strcpy(m.id,"restore-malformed");
    m.state=CHRONO_CHECKPOINT_PREPARING;
    if(chrono_criu_checkpoint(&m,base,error,sizeof(error))!=0) { remove_tree(base); return fail("malformed PID setup dump failed"); }
    m.state=CHRONO_CHECKPOINT_COMPLETE;
    (void)setenv("CHRONO_FAKE_CRIU_PIDFILE","malformed",1);
    if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0 || strstr(error,"may have occurred")==NULL) { remove_tree(base); return fail("malformed PID outcome was not uncertain"); }
    (void)unsetenv("CHRONO_FAKE_CRIU_PIDFILE");
    const char *bad_pid_modes[] = {"trailing", "oversized"};
    const char *bad_pid_ids[] = {"restore-trailing", "restore-oversized"};
    for(size_t i=0;i<sizeof(bad_pid_modes)/sizeof(bad_pid_modes[0]);i++) {
        strcpy(m.id,bad_pid_ids[i]); m.state=CHRONO_CHECKPOINT_PREPARING;
        if(chrono_criu_checkpoint(&m,base,error,sizeof(error))!=0) { remove_tree(base); return fail("bad PID setup dump failed"); }
        m.state=CHRONO_CHECKPOINT_COMPLETE;
        (void)setenv("CHRONO_FAKE_CRIU_PIDFILE",bad_pid_modes[i],1);
        if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0 || strstr(error,"may have occurred")==NULL ||
           chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0) { remove_tree(base); return fail("invalid PID file was accepted or restore retried"); }
    }
    (void)unsetenv("CHRONO_FAKE_CRIU_PIDFILE");
    strcpy(m.id,"inventory-only");
    (void)snprintf(artifact,sizeof(artifact),"%s/%s",base,m.id);
    if(mkdir(artifact,0700)!=0) { remove_tree(base); return fail("inventory-only directory setup failed"); }
    (void)snprintf(linkpath,sizeof(linkpath),"%s/inventory.img",artifact);
    int fd=open(linkpath,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0 || write(fd,"fake",4)!=4 || close(fd)!=0 || chrono_checkpoint_artifact_valid(base,m.id)) { remove_tree(base); return fail("inventory-only artifact was accepted"); }
    (void)unlink(linkpath); (void)rmdir(artifact);
    (void)setenv("CHRONO_CRIU_PATH","/no/such/criu",1);
    if(chrono_criu_available(error,sizeof(error))) { remove_tree(base); return fail("missing CRIU reported available"); }
    strcpy(m.id,"missing-executable");
    m.state=CHRONO_CHECKPOINT_PREPARING;
    if(chrono_criu_checkpoint(&m,base,error,sizeof(error))==0) { remove_tree(base); return fail("missing CRIU allowed checkpoint"); }
    (void)snprintf(artifact,sizeof(artifact),"%s/%s",base,m.id);
    if(access(artifact,F_OK)==0) { remove_tree(base); return fail("missing CRIU created artifact directory"); }
    (void)setenv("CHRONO_CRIU_PATH",base,1);
    if(chrono_criu_available(error,sizeof(error))) { remove_tree(base); return fail("CRIU directory was accepted as executable"); }
    (void)snprintf(linkpath,sizeof(linkpath),"%s/not-executable",base);
    fd=open(linkpath,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0 || close(fd)!=0) { remove_tree(base); return fail("non-executable setup failed"); }
    (void)setenv("CHRONO_CRIU_PATH",linkpath,1);
    if(chrono_criu_available(error,sizeof(error))) { remove_tree(base); return fail("non-executable CRIU was accepted"); }
    (void)snprintf(linkpath,sizeof(linkpath),"%s/bad-executable",base);
    fd=open(linkpath,O_WRONLY|O_CREAT|O_EXCL,0700);
    if(fd<0) {
        remove_tree(base); return fail("exec failure setup open failed");
    }
    ssize_t written=write(fd,"not an executable format\n",25);
    int close_result=close(fd);
    if(written!=25 || close_result!=0 || chmod(linkpath,0700)!=0) {
        remove_tree(base); return fail("exec failure setup failed");
    }
    (void)setenv("CHRONO_CRIU_PATH",linkpath,1);
    if(chrono_criu_available(error,sizeof(error)) || strstr(error,"cannot execute CRIU for kernel feature check: ")==NULL) {
        remove_tree(base); return fail("execv failure was not reported distinctly");
    }
    (void)unsetenv("CHRONO_FAKE_CRIU_LOG");
    (void)unsetenv("CHRONO_FAKE_CRIU_FAIL");
    (void)unsetenv("CHRONO_FAKE_CRIU_EXIT_STATUS");
    (void)unsetenv("CHRONO_FAKE_CRIU_SIGNAL");
    (void)unsetenv("CHRONO_FAKE_CRIU_PARTIAL");
    (void)unsetenv("CHRONO_FAKE_CRIU_PIDFILE");
    (void)unsetenv("CHRONO_CRIU_PATH");
    if (stop_managed_test_workload() != 0) {
        remove_tree(base);
        return fail("managed fake-adapter workload cleanup could not be verified");
    }
    remove_tree(base);
    puts("CRIU adapter tests passed (fake executable only)");
    return 0;
}
