#define _POSIX_C_SOURCE 200809L
#include "chrono/checkpoint.h"
#include "chrono/checkpoint_criu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>
#include <errno.h>

static int listed;
static char test_dir[128];
static void cleanup(void)
{
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
