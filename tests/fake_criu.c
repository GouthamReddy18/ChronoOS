#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    const char *log_path=getenv("CHRONO_FAKE_CRIU_LOG");
    if(log_path!=NULL) {
        FILE *log=fopen(log_path,"a");
        if(log==NULL) return 89;
        (void)fprintf(log,"invoked\n");
        if(fclose(log)!=0) return 88;
    }
    const char *fail = getenv("CHRONO_FAKE_CRIU_FAIL");
    if (argc < 2) return 90;
    if (strcmp(argv[1], "check") == 0) return fail != NULL && strcmp(fail,"check")==0 ? 1 : 0;
    if (fail != NULL && strcmp(fail,argv[1])==0 &&
        !(strcmp(argv[1], "dump") == 0 && getenv("CHRONO_FAKE_CRIU_PARTIAL") != NULL)) return 7;
    const char *dir = NULL, *pidfile = NULL;
    for (int i=2;i+1<argc;i++) {
        if(strcmp(argv[i],"--images-dir")==0) dir=argv[i+1];
        if(strcmp(argv[i],"--pidfile")==0) pidfile=argv[i+1];
    }
    if (dir == NULL) return 91;
    if (strcmp(argv[1],"dump")==0) {
        char path[4096];
        if(snprintf(path,sizeof(path),"%s/inventory.img",dir)>=(int)sizeof(path)) return 92;
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0 || write(fd,"fake",4)!=4 || close(fd)!=0) return 93;
        if(snprintf(path,sizeof(path),"%s/core-1.img",dir)>=(int)sizeof(path)) return 92;
        fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
        if(fd<0 || write(fd,"fake-core",9)!=9 || close(fd)!=0) return 93;
        if(fail != NULL && strcmp(fail,"dump")==0) return 7;
        return 0;
    }
    if (strcmp(argv[1],"restore")==0 && pidfile != NULL) {
        const char *pidmode=getenv("CHRONO_FAKE_CRIU_PIDFILE");
        if(pidmode!=NULL && strcmp(pidmode,"missing")==0) return 0;
        int fd=open(pidfile,O_WRONLY|O_CREAT|O_EXCL,0600);
        const char *value = "424242\n";
        if (pidmode != NULL && strcmp(pidmode, "malformed") == 0) value = "bad\n";
        else if (pidmode != NULL && strcmp(pidmode, "trailing") == 0) value = "424242\nextra\n";
        else if (pidmode != NULL && strcmp(pidmode, "oversized") == 0)
            value = "9999999999999999999999999999999999999999999999999999999999999999\n";
        size_t len=strlen(value);
        if(fd<0 || write(fd,value,len)!=(ssize_t)len || close(fd)!=0) return 94;
        return 0;
    }
    return 95;
}
