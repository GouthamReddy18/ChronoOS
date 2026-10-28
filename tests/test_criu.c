#define _POSIX_C_SOURCE 200809L
#include "chrono/checkpoint_criu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

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
int main(int argc,char **argv)
{
    if(argc!=2) return fail("fake executable path required");
    char base[]="/tmp/chrono-criu-test-XXXXXX", error[256];
    if(mkdtemp(base)==NULL || chmod(base,0700)!=0) return fail("temporary directory setup failed");
    (void)setenv("CHRONO_CRIU_PATH",argv[1],1);
    (void)unsetenv("CHRONO_FAKE_CRIU_FAIL");
    (void)unsetenv("CHRONO_FAKE_CRIU_LOG");
    (void)unsetenv("CHRONO_FAKE_CRIU_EXIT_STATUS");
    (void)unsetenv("CHRONO_FAKE_CRIU_SIGNAL");
    (void)unsetenv("CHRONO_FAKE_CRIU_PARTIAL");
    (void)unsetenv("CHRONO_FAKE_CRIU_PIDFILE");
    (void)setenv("CHRONO_FAKE_CRIU_SIGNAL","check",1);
    if(chrono_criu_available(error,sizeof(error)) ||
       strstr(error,"CRIU kernel feature check terminated by signal 15")==NULL) {
        remove_tree(base); return fail("signal termination was not reported accurately");
    }
    (void)unsetenv("CHRONO_FAKE_CRIU_SIGNAL");
    if(!chrono_criu_available(error,sizeof(error))) { remove_tree(base); return fail("fake CRIU availability check failed"); }
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
    chrono_checkpoint_metadata m={0}; strcpy(m.id,"sample"); m.original_pid=123;
    m.process_start_time=456; m.state=CHRONO_CHECKPOINT_PREPARING;
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
    if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))!=0 || restored!=424242) { remove_tree(base); return fail("fake restore PID not reported"); }
    if(chrono_criu_restore(&m,base,&restored,error,sizeof(error))==0) { remove_tree(base); return fail("repeated restore was accepted"); }
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
    remove_tree(base);
    puts("CRIU adapter tests passed (fake executable only)");
    return 0;
}
