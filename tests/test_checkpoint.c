#define _POSIX_C_SOURCE 200809L
#include "chrono/checkpoint.h"
#include "chrono/checkpoint_criu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

static int listed;
static char test_dir[128];
static void remove_artifact(const char *id)
{
    static const char *const files[] = {
        "inventory.img", "core-1.img", "stats-dump", "stats-restore"
    };
    char directory[256], path[512];
    (void)snprintf(directory, sizeof(directory), "%s/%s", test_dir, id);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        (void)snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        (void)unlink(path);
    }
    (void)rmdir(directory);
}

static void cleanup(void)
{
    remove_artifact("cp-a"); remove_artifact("cp-b");
    remove_artifact("cp-c"); remove_artifact("cp-d");
    DIR *d=opendir(test_dir);
    if(d!=NULL) {
        struct dirent *e;
        while((e=readdir(d))!=NULL) {
            if(strcmp(e->d_name,".")==0 || strcmp(e->d_name,"..")==0) continue;
            char path[256];
            if(snprintf(path,sizeof(path),"%s/%s",test_dir,e->d_name)>0) (void)unlink(path);
        }
        closedir(d);
    }
    (void)rmdir(test_dir);
}

static int create_complete_fixture(const char *id, const char *workload)
{
    chrono_checkpoint_metadata metadata = {0};
    (void)snprintf(metadata.id, sizeof(metadata.id), "%s", id);
    metadata.original_pid = getpid(); metadata.process_start_time = 99;
    (void)snprintf(metadata.workload, sizeof(metadata.workload), "%s", workload);
    (void)snprintf(metadata.executable, sizeof(metadata.executable), "/test/counter");
    (void)snprintf(metadata.backend, sizeof(metadata.backend), "criu");
    metadata.state = CHRONO_CHECKPOINT_PREPARING;
    char error[256];
    if (chrono_checkpoint_begin(test_dir, &metadata, error, sizeof(error)) != 0) return -1;
    char directory[256], path[512];
    (void)snprintf(directory, sizeof(directory), "%s/%s", test_dir, id);
    if (mkdir(directory, 0700) != 0) return -1;
    const char *files[] = {"inventory.img", "core-1.img", "stats-restore"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        (void)snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || write(fd, "fixture", 7) != 7 || close(fd) != 0) return -1;
    }
    return chrono_checkpoint_transition(test_dir, id, CHRONO_CHECKPOINT_COMPLETE,
                                        error, sizeof(error));
}
static int collect(const chrono_checkpoint_metadata *m, void *context)
{
    (void)context;
    if (m->state != CHRONO_CHECKPOINT_COMPLETE) return -1;
    ++listed;
    return 0;
}

static int fail(const char *message)
{
    fprintf(stderr,"test_checkpoint: %s\n",message);
    return 1;
}

int main(void)
{
    char error[256];
    strcpy(test_dir,"/tmp/chrono-checkpoint-test-XXXXXX");
    if(mkdtemp(test_dir)==NULL) return fail("mkdtemp failed");
    (void)atexit(cleanup);
    if(chmod(test_dir,0700)!=0) return fail("chmod failed");
    char missing[160];
    if(snprintf(missing,sizeof(missing),"%s/missing",test_dir)<0) return fail("path construction failed");
    listed=0;
    if(chrono_checkpoint_list(missing,collect,NULL,error,sizeof(error))!=0 || access(missing,F_OK)==0 || listed!=0)
        return fail("listing missing directory was not read-only");
    char directory_link[256];
    (void)snprintf(directory_link,sizeof(directory_link),"%s/directory-link",test_dir);
    if(symlink(test_dir,directory_link)!=0 || chrono_checkpoint_list(directory_link,collect,NULL,error,sizeof(error))==0)
        return fail("checkpoint directory symlink was accepted");
    if(chrono_checkpoint_id_valid("good-ID_1")==0 || chrono_checkpoint_id_valid("../bad") ||
       chrono_checkpoint_id_valid("") || chrono_checkpoint_id_valid("a/b")) return fail("ID validation failed");
    (void)setenv("CHRONO_CRIU_PATH","/no/such/criu",1);
    const chrono_checkpoint_backend *backend=chrono_checkpoint_backend_get();
    if(chrono_checkpoint_backend_available() || backend==NULL || backend->available ||
       strcmp(backend->name,"criu") || backend->checkpoint(NULL,test_dir,error,sizeof(error))==0 ||
       backend->restore(NULL,test_dir,NULL,error,sizeof(error))==0) return fail("unavailable backend reported success");
    chrono_checkpoint_metadata m={0};
    strcpy(m.id,"pending"); m.original_pid=getpid(); m.process_start_time=99;
    strcpy(m.workload,"counter"); strcpy(m.executable,"/usr/bin/counter_workload");
    strcpy(m.backend,"unavailable");
    m.state=CHRONO_CHECKPOINT_PREPARING;
    if(chrono_checkpoint_begin(directory_link,&m,error,sizeof(error))==0)
        return fail("checkpoint creation followed a directory symlink");
    if(unlink(directory_link)!=0) return fail("directory symlink cleanup failed");
    chrono_checkpoint_metadata invalid=m;
    strcpy(invalid.id,"invalid"); invalid.workload[0]='x'; invalid.workload[1]='\n'; invalid.workload[2]='\0';
    if(chrono_checkpoint_begin(missing,&invalid,error,sizeof(error))==0 || access(missing,F_OK)==0)
        return fail("newline injection was accepted or created a directory");
    invalid=m; invalid.executable[0]='x'; invalid.executable[1]='\x01'; invalid.executable[2]='\0';
    if(chrono_checkpoint_begin(missing,&invalid,error,sizeof(error))==0 || access(missing,F_OK)==0)
        return fail("control character in executable was accepted");
    invalid=m; invalid.backend[0]='x'; invalid.backend[1]='\x7f'; invalid.backend[2]='\0';
    if(chrono_checkpoint_begin(missing,&invalid,error,sizeof(error))==0 || access(missing,F_OK)==0)
        return fail("control character in backend was accepted");
    if(chrono_checkpoint_begin(test_dir,&m,error,sizeof(error))!=0) return fail("metadata creation failed");
    if(chrono_checkpoint_begin(test_dir,&m,error,sizeof(error))==0) return fail("duplicate ID accepted");
    struct stat st;
    if(stat(test_dir,&st)!=0 || (st.st_mode&0777)!=0700) return fail("directory permissions changed");
    if(chmod(test_dir,0755)!=0) return fail("directory permission setup failed");
    int insecure_list_result=chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error));
    if(chmod(test_dir,0700)!=0) return fail("directory permission restoration failed");
    if(insecure_list_result==0) return fail("insecure directory permissions were accepted");
    if(chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))!=0 || listed!=0) return fail("preparing metadata was listed");
    /* Two processes race the same PREPARING -> FAILED transition. Exactly one may win. */
    int ready[2], gate[2];
    if(pipe(ready)!=0 || pipe(gate)!=0) return fail("pipe failed");
    pid_t children[2];
    for(int i=0;i<2;i++) {
        children[i]=fork();
        if(children[i]==0) { char token; close(ready[0]); close(gate[1]);
            if(write(ready[1],"r",1)!=1 || read(gate[0],&token,1)!=1) _exit(2);
            _exit(chrono_checkpoint_transition(test_dir,"pending",CHRONO_CHECKPOINT_FAILED,error,sizeof(error))==0?0:1); }
        if(children[i]<0) return fail("fork failed");
    }
    close(ready[1]); close(gate[0]); char ready_tokens[2]; size_t ready_count=0;
    while(ready_count<sizeof(ready_tokens)) {
        ssize_t n=read(ready[0],ready_tokens+ready_count,sizeof(ready_tokens)-ready_count);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return fail("children did not reach transition barrier");
        ready_count+=(size_t)n;
    }
    close(ready[0]); ssize_t nwritten=write(gate[1],"xx",2); close(gate[1]);
    if(nwritten!=2) return fail("gate release failed");
    int wins=0;
    for(int i=0;i<2;i++) { int status; if(waitpid(children[i],&status,0)<0) return fail("waitpid failed"); if(WIFEXITED(status)&&WEXITSTATUS(status)==0) wins++; }
    if(wins!=1) return fail("competing transitions were not serialized");
    if(chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))!=0 || listed!=0) return fail("failed metadata was listed");
    char lockpath[256]; (void)snprintf(lockpath,sizeof(lockpath),"%s/.chrono.lock",test_dir);
    strcpy(m.id,"locktest");
    if(chrono_checkpoint_begin(test_dir,&m,error,sizeof(error))!=0) return fail("lock test record creation failed");
    if(unlink(lockpath)!=0 || symlink("finished.meta",lockpath)!=0 ||
       chrono_checkpoint_transition(test_dir,"locktest",CHRONO_CHECKPOINT_FAILED,error,sizeof(error))==0)
        return fail("lock symlink accepted");
    if(unlink(lockpath)!=0) return fail("lock symlink cleanup failed");
    if(chrono_checkpoint_transition(test_dir,"locktest",CHRONO_CHECKPOINT_FAILED,error,sizeof(error))!=0 || chmod(lockpath,0644)!=0 ||
       chrono_checkpoint_transition(test_dir,"locktest",CHRONO_CHECKPOINT_COMPLETE,error,sizeof(error))==0 || chmod(lockpath,0600)!=0)
        return fail("invalid lock permissions were accepted");
    strcpy(m.id,"finished");
    if(chrono_checkpoint_begin(test_dir,&m,error,sizeof(error))!=0 ||
       chrono_checkpoint_transition(test_dir,"finished",CHRONO_CHECKPOINT_COMPLETE,error,sizeof(error))!=0) return fail("complete transition failed");
    if(chrono_checkpoint_transition(test_dir,"finished",CHRONO_CHECKPOINT_FAILED,error,sizeof(error))==0) return fail("terminal state changed");
    if(chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))!=0 || listed!=1) return fail("complete metadata not listed");
    const char *retained_ids[] = {"cp-a", "cp-b", "cp-c"};
    for (size_t i = 0; i < sizeof(retained_ids) / sizeof(retained_ids[0]); ++i)
        if (create_complete_fixture(retained_ids[i], "cp-000001") != 0)
            return fail("could not create complete retention fixture");
    for (size_t i = 0; i < sizeof(retained_ids) / sizeof(retained_ids[0]); ++i)
        if (chrono_checkpoint_publish_latest(test_dir, "cp-000001", retained_ids[i], error, sizeof(error)) != 0)
            return fail("could not publish a complete checkpoint as latest");
    if (chrono_checkpoint_publish_latest(test_dir, "cp-000001", "cp-a", error, sizeof(error)) != 0)
        return fail("publishing an older checkpoint should leave latest unchanged");
    chrono_checkpoint_metadata latest;
    if (chrono_checkpoint_latest(test_dir, "cp-000001", &latest, error, sizeof(error)) != 0 ||
        strcmp(latest.id, "cp-c") != 0) return fail("latest-valid reference did not select the newest checkpoint");
    strcpy(m.id, "cp-fail"); m.state = CHRONO_CHECKPOINT_PREPARING;
    strcpy(m.workload, "cp-000001");
    if (chrono_checkpoint_begin(test_dir, &m, error, sizeof(error)) != 0 ||
        chrono_checkpoint_transition(test_dir, "cp-fail", CHRONO_CHECKPOINT_FAILED, error, sizeof(error)) != 0 ||
        chrono_checkpoint_publish_latest(test_dir, "cp-000001", "cp-fail", error, sizeof(error)) == 0 ||
        chrono_checkpoint_latest(test_dir, "cp-000001", &latest, error, sizeof(error)) != 0 ||
        strcmp(latest.id, "cp-c") != 0) return fail("failed checkpoint replaced latest-valid reference");
    char latest_path[256];
    (void)snprintf(latest_path, sizeof(latest_path), "%s/latest-cp-000001", test_dir);
    int latest_fd = open(latest_path, O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    if (latest_fd < 0 || write(latest_fd, "invalid\n", 8) != 8 || close(latest_fd) != 0 ||
        chrono_checkpoint_latest(test_dir, "cp-000001", &latest, error, sizeof(error)) != 0 ||
        strcmp(latest.id, "cp-c") != 0) return fail("corrupt latest reference was not safely reconstructed");
    if (create_complete_fixture("cp-d", "cp-000001") != 0 ||
        chrono_checkpoint_publish_latest(test_dir, "cp-000001", "cp-d", error, sizeof(error)) != 0 ||
        chrono_checkpoint_retain(test_dir, "cp-000001", 2, error, sizeof(error)) != 0)
        return fail("checkpoint retention failed");
    if (!chrono_checkpoint_artifact_valid(test_dir, "cp-c") ||
        !chrono_checkpoint_artifact_valid(test_dir, "cp-d") ||
        chrono_checkpoint_read(test_dir, "cp-b", &latest, error, sizeof(error)) == 0)
        return fail("retention did not preserve latest and enforce the configured limit");
    int use_lock = chrono_checkpoint_use_lock(test_dir, 0, error, sizeof(error));
    int ready_pipe[2];
    if (use_lock < 0 || pipe(ready_pipe) != 0) return fail("could not establish restore-use lock test");
    pid_t retention_child = fork();
    if (retention_child == 0) {
        close(ready_pipe[0]);
        ssize_t ready_written = write(ready_pipe[1], "r", 1);
        close(ready_pipe[1]);
        if (ready_written != 1) _exit(2);
        _exit(chrono_checkpoint_retain(test_dir, "cp-000001", 1, error, sizeof(error)) == 0 ? 0 : 1);
    }
    close(ready_pipe[1]); char ready_byte;
    if (retention_child < 0 || read(ready_pipe[0], &ready_byte, 1) != 1)
        return fail("retention synchronization child failed");
    close(ready_pipe[0]);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000};
    (void)nanosleep(&pause, NULL);
    int child_status;
    if (waitpid(retention_child, &child_status, WNOHANG) != 0)
        return fail("retention did not wait for an active restore-use lock");
    (void)flock(use_lock, LOCK_UN); close(use_lock);
    if (waitpid(retention_child, &child_status, 0) != retention_child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
        return fail("retention did not complete after restore-use lock release");
    if (!chrono_checkpoint_artifact_valid(test_dir, "cp-d") ||
        chrono_checkpoint_artifact_valid(test_dir, "cp-c"))
        return fail("latest checkpoint was not protected during retention");
    if(chrono_checkpoint_begin("/path/that/does/not/exist/chrono",&m,error,sizeof(error))==0) return fail("directory error ignored");
    char record[256]; (void)snprintf(record,sizeof(record),"%s/broken.meta",test_dir);
    int fd=open(record,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0 || write(fd,"bad\n",4)!=4 || close(fd)!=0 ||
       chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))==0) return fail("file error ignored");
    (void)unlink(record);
    (void)snprintf(record,sizeof(record),"%s/link.meta",test_dir);
    if(symlink("finished.meta",record)!=0 || chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))==0) return fail("metadata symlink accepted");
    (void)unlink(record);
    (void)snprintf(record,sizeof(record),"%s/finished.meta",test_dir);
    if(chmod(record,0644)!=0 || chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))==0 || chmod(record,0600)!=0)
        return fail("metadata permissions were not enforced");
    const char *malformed="CHRONO1\nfinished\n999999999999999999999999999999999\n99\n1\nw\ne\nCOMPLETE\nunavailable\n";
    fd=open(record,O_WRONLY|O_TRUNC);
    if(fd<0 || write(fd,malformed,strlen(malformed))!=(ssize_t)strlen(malformed) || close(fd)!=0 ||
       chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))==0) return fail("malformed numeric field accepted");
    const char *wrong_id="CHRONO1\nother\n1\n99\n1\nw\ne\nCOMPLETE\nunavailable\n";
    fd=open(record,O_WRONLY|O_TRUNC);
    if(fd<0 || write(fd,wrong_id,strlen(wrong_id))!=(ssize_t)strlen(wrong_id) || close(fd)!=0 ||
       chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))==0) return fail("metadata ID mismatch accepted");
    const char *extra="CHRONO1\nfinished\n1\n99\n1\nw\ne\nCOMPLETE\nunavailable\nextra\n";
    fd=open(record,O_WRONLY|O_TRUNC);
    if(fd<0 || write(fd,extra,strlen(extra))!=(ssize_t)strlen(extra) || close(fd)!=0 ||
       chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))==0) return fail("extra metadata field accepted");
    fd=open(record,O_WRONLY|O_TRUNC);
    const char *truncated="CHRONO1\nfinished\n1\n99\n";
    if(fd<0 || write(fd,truncated,strlen(truncated))!=(ssize_t)strlen(truncated) || close(fd)!=0 ||
       chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))==0) return fail("truncated metadata accepted");
    fd=open(record,O_WRONLY|O_TRUNC);
    if(fd<0) return fail("oversized record open failed");
    char oversized[4096]; memset(oversized,'x',sizeof(oversized));
    if(write(fd,oversized,sizeof(oversized))!=(ssize_t)sizeof(oversized) || close(fd)!=0 ||
       chrono_checkpoint_list(test_dir,collect,NULL,error,sizeof(error))==0) return fail("oversized metadata accepted");
    puts("checkpoint metadata tests passed");
    return 0;
}
