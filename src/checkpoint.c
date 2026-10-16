#define _POSIX_C_SOURCE 200809L
#include "chrono/checkpoint.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static void set_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size > 0) (void)snprintf(error, size, "%s", message);
}

static int safe_text(const char *text, size_t capacity)
{
    if (text == NULL || text[0] == '\0' || strnlen(text, capacity) >= capacity) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p)
        if (iscntrl(*p) || !isprint(*p)) return 0;
    return 1;
}

int chrono_checkpoint_id_valid(const char *id)
{
    if (id == NULL || id[0] == '\0' || strnlen(id, CHRONO_CHECKPOINT_ID_MAX + 1) > CHRONO_CHECKPOINT_ID_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)id; *p != '\0'; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 0;
    return 1;
}

const char *chrono_checkpoint_backend_name(void) { return "criu"; }

static int open_directory(const char *path, int create, int *fd_out)
{
    struct stat st;
    if (path == NULL || path[0] != '/') { errno = EINVAL; return -1; }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && create && errno == ENOENT) {
        if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
        fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0777) != 0700) { close(fd); errno = EPERM; return -1; }
    *fd_out = fd;
    return 0;
}

static const char *state_name(chrono_checkpoint_state state)
{
    if (state == CHRONO_CHECKPOINT_PREPARING) return "PREPARING";
    if (state == CHRONO_CHECKPOINT_COMPLETE) return "COMPLETE";
    if (state == CHRONO_CHECKPOINT_FAILED) return "FAILED";
    return NULL;
}

static int serialize(const chrono_checkpoint_metadata *m, char *buffer, size_t size)
{
    const char *state = state_name(m->state);
    if (!chrono_checkpoint_id_valid(m->id) || m->original_pid <= 0 || m->process_start_time == 0 ||
        m->created_at <= 0 || state == NULL || !safe_text(m->workload, sizeof(m->workload)) ||
        !safe_text(m->executable, sizeof(m->executable)) || !safe_text(m->backend, sizeof(m->backend))) return -1;
    int n = snprintf(buffer, size, "CHRONO1\n%s\n%ld\n%llu\n%lld\n%s\n%s\n%s\n%s\n",
        m->id, (long)m->original_pid, (unsigned long long)m->process_start_time,
        (long long)m->created_at, m->workload, m->executable, state, m->backend);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

static int write_all(int fd, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, data + offset, length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        offset += (size_t)n;
    }
    return 0;
}

static int persist(int dirfd, const chrono_checkpoint_metadata *m, int exclusive)
{
    char data[CHRONO_CHECKPOINT_TEXT_MAX * 2 + 512], name[CHRONO_CHECKPOINT_ID_MAX + 8];
    char temp[CHRONO_CHECKPOINT_ID_MAX + 24];
    struct stat st;
    int length = serialize(m, data, sizeof(data));
    if (length < 0) { errno = EINVAL; return -1; }
    (void)snprintf(name, sizeof(name), "%s.meta", m->id);
    (void)snprintf(temp, sizeof(temp), ".%s.tmp", m->id);
    int fd = openat(dirfd, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
             st.st_nlink == 1 && fchmod(fd, 0600) == 0;
    int saved = errno;
    if (ok && write_all(fd, data, (size_t)length) != 0) { ok = 0; saved = errno; }
    if (ok && fsync(fd) != 0) { ok = 0; saved = errno; }
    if (close(fd) != 0) { if (ok) saved = errno; ok = 0; }
    if (!ok) {
        if (unlinkat(dirfd, temp, 0) != 0 && errno != ENOENT) saved = errno;
        errno = saved; return -1;
    }
    if (exclusive) {
        if (linkat(dirfd, temp, dirfd, name, 0) != 0) {
            saved = errno;
            if (unlinkat(dirfd, temp, 0) != 0 && errno != ENOENT) saved = errno;
            errno = saved; return -1;
        }
        if (unlinkat(dirfd, temp, 0) != 0) return -2;
    } else if (renameat(dirfd, temp, dirfd, name) != 0) {
        saved = errno;
        if (unlinkat(dirfd, temp, 0) != 0 && errno != ENOENT) saved = errno;
        errno = saved; return -1;
    }
    return fsync(dirfd) == 0 ? 0 : -2;
}

int chrono_checkpoint_begin(const char *directory, const chrono_checkpoint_metadata *metadata,
                            char *error, size_t error_size)
{
    int dirfd;
    chrono_checkpoint_metadata item;
    if (metadata == NULL || metadata->state != CHRONO_CHECKPOINT_PREPARING ||
        !chrono_checkpoint_id_valid(metadata->id) || metadata->original_pid <= 0 ||
        metadata->process_start_time == 0 || !safe_text(metadata->workload, sizeof(metadata->workload)) ||
        !safe_text(metadata->executable, sizeof(metadata->executable)) ||
        !safe_text(metadata->backend, sizeof(metadata->backend))) {
        set_error(error, error_size, "invalid checkpoint metadata or ID"); return -1;
    }
    item = *metadata;
    item.created_at = (int64_t)time(NULL);
    (void)snprintf(item.backend, sizeof(item.backend), "%s", chrono_checkpoint_backend_name());
    if (open_directory(directory, 1, &dirfd) != 0) { set_error(error,error_size,"cannot open private checkpoint directory"); return -1; }
    int result = persist(dirfd, &item, 1);
    int saved = errno;
    if (close(dirfd) != 0 && result == 0) { result = -1; saved = errno; }
    if (result != 0) { errno=saved; set_error(error,error_size, saved == EEXIST ? "checkpoint ID already exists" : result == -2 ? "metadata may exist, but directory durability is uncertain" : "cannot create checkpoint metadata"); return -1; }
    return 0;
}

static int read_metadata(int dirfd, const char *id, chrono_checkpoint_metadata *m)
{
    char name[CHRONO_CHECKPOINT_ID_MAX + 8], data[CHRONO_CHECKPOINT_TEXT_MAX * 2 + 512];
    struct stat st;
    (void)snprintf(name, sizeof(name), "%s.meta", id);
    int fd = openat(dirfd, name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=getuid() ||
        (st.st_mode & 0777)!=0600 || st.st_nlink!=1) { close(fd); errno=EPERM; return -1; }
    size_t used=0;
    while (used < sizeof(data)-1) {
        ssize_t n=read(fd,data+used,sizeof(data)-1-used);
        if(n<0 && errno==EINTR) continue;
        if(n<0) { int saved=errno; close(fd); errno=saved; return -1; }
        if(n==0) break;
        used+=(size_t)n;
    }
    char extra;
    ssize_t more=read(fd,&extra,1); int read_error=errno;
    int close_result=close(fd);
    if (more < 0) { errno=read_error; return -1; }
    if (close_result != 0) return -1;
    if(used==0 || used>=sizeof(data)-1 || data[used-1]!='\n' || more!=0) { errno=EINVAL; return -1; }
    if(memchr(data,'\0',used)!=NULL) { errno=EINVAL; return -1; }
    data[used]='\0';
    char *fields[9]; size_t count=0; char *p=data;
    while (p < data + used) {
        if (count == 9) { errno=EINVAL; return -1; }
        fields[count++]=p;
        char *nl=memchr(p,'\n',(size_t)(data+used-p));
        if(nl==NULL) { errno=EINVAL; return -1; }
        *nl='\0'; p=nl+1;
    }
    if (count != 9 || strcmp(fields[0],"CHRONO1") || strcmp(fields[1],id)) { errno=EINVAL; return -1; }
    memset(m,0,sizeof(*m));
    char *end;
    if(fields[2][0]=='\0' || strspn(fields[2],"0123456789")!=strlen(fields[2])) goto malformed;
    errno=0; long long pid=strtoll(fields[2],&end,10);
    if(errno || *end || pid<=0) goto malformed;
    pid_t converted_pid=(pid_t)pid;
    if(converted_pid<=0 || (long long)converted_pid!=pid) goto malformed;
    if(fields[3][0]=='\0' || strspn(fields[3],"0123456789")!=strlen(fields[3])) goto malformed;
    errno=0; unsigned long long start=strtoull(fields[3],&end,10); if(errno || *end || start==0 || start>UINT64_MAX) goto malformed;
    if(fields[4][0]=='\0' || strspn(fields[4],"0123456789")!=strlen(fields[4])) goto malformed;
    errno=0; long long created=strtoll(fields[4],&end,10); if(errno || *end || created<=0 || created>INT64_MAX) goto malformed;
    if (strlen(fields[5])>=sizeof(m->workload)||strlen(fields[6])>=sizeof(m->executable)||strlen(fields[8])>=sizeof(m->backend)) goto malformed;
    if(!safe_text(fields[5],sizeof(m->workload)) || !safe_text(fields[6],sizeof(m->executable)) || !safe_text(fields[8],sizeof(m->backend))) goto malformed;
    strcpy(m->id,id); m->original_pid=converted_pid; m->process_start_time=(uint64_t)start; m->created_at=(int64_t)created;
    strcpy(m->workload,fields[5]); strcpy(m->executable,fields[6]); strcpy(m->backend,fields[8]);
    if(!strcmp(fields[7],"PREPARING")) m->state=CHRONO_CHECKPOINT_PREPARING;
    else if(!strcmp(fields[7],"COMPLETE")) m->state=CHRONO_CHECKPOINT_COMPLETE;
    else if(!strcmp(fields[7],"FAILED")) m->state=CHRONO_CHECKPOINT_FAILED;
    else goto malformed;
    return 0;
malformed: errno=EINVAL; return -1;
}

int chrono_checkpoint_transition(const char *directory, const char *id, chrono_checkpoint_state state,
                                 char *error, size_t error_size)
{
    int dirfd = -1; chrono_checkpoint_metadata m;
    if (!chrono_checkpoint_id_valid(id) || state == CHRONO_CHECKPOINT_PREPARING || state_name(state)==NULL) {
        set_error(error,error_size,"invalid checkpoint transition"); return -1;
    }
    if(open_directory(directory,0,&dirfd)!=0) {
        set_error(error,error_size,"cannot read checkpoint metadata"); return -1;
    }
    int lockfd=openat(dirfd,".chrono.lock",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    int new_lock = lockfd >= 0;
    if (lockfd < 0 && errno == EEXIST)
        lockfd=openat(dirfd,".chrono.lock",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    struct stat lockst;
    if(lockfd<0 || fstat(lockfd,&lockst)!=0 || !S_ISREG(lockst.st_mode) || lockst.st_uid!=getuid() ||
       lockst.st_nlink!=1 || (new_lock && fchmod(lockfd,0600)!=0) || fstat(lockfd,&lockst)!=0 ||
       (lockst.st_mode & 0777)!=0600 || flock(lockfd,LOCK_EX)!=0) {
        if(lockfd>=0) close(lockfd);
        close(dirfd);
        set_error(error,error_size,"cannot lock checkpoint metadata");
        return -1;
    }
    if(read_metadata(dirfd,id,&m)!=0) {
        flock(lockfd,LOCK_UN); close(lockfd); close(dirfd);
        set_error(error,error_size,"cannot read checkpoint metadata"); return -1;
    }
    if(m.state!=CHRONO_CHECKPOINT_PREPARING) { flock(lockfd,LOCK_UN); close(lockfd); close(dirfd); set_error(error,error_size,"checkpoint is not preparing"); return -1; }
    m.state=state;
    int result=persist(dirfd,&m,0); int saved=errno;
    if (flock(lockfd,LOCK_UN)!=0 && result==0) { result=-1; saved=errno; }
    if (close(lockfd)!=0 && result==0) { result=-1; saved=errno; }
    if (close(dirfd)!=0 && result==0) { result=-1; saved=errno; }
    errno=saved;
    if(result!=0) { set_error(error,error_size,result == -2 ? "metadata may have changed, but directory durability is uncertain" : "cannot update checkpoint metadata"); return -1; }
    return 0;
}

int chrono_checkpoint_list(const char *directory, chrono_checkpoint_list_fn callback, void *context,
                           char *error, size_t error_size)
{
    int dirfd;
    if (callback == NULL) { set_error(error,error_size,"cannot open checkpoint directory"); return -1; }
    if (open_directory(directory,0,&dirfd)!=0) {
        if (errno == ENOENT) return 0;
        set_error(error,error_size,"cannot open checkpoint directory"); return -1;
    }
    int duplicate=dup(dirfd); DIR *dir=duplicate<0?NULL:fdopendir(duplicate); if(dir==NULL) { if(duplicate>=0) close(duplicate); close(dirfd); set_error(error,error_size,"cannot list checkpoint directory"); return -1; }
    struct dirent *entry; int result=0;
    while((entry=readdir(dir))!=NULL) {
        size_t n=strlen(entry->d_name);
        if(n<6 || strcmp(entry->d_name+n-5,".meta")!=0) continue;
        char id[CHRONO_CHECKPOINT_ID_MAX+1]; size_t len=n-5;
        if(len>CHRONO_CHECKPOINT_ID_MAX) continue;
        memcpy(id,entry->d_name,len); id[len]='\0'; if(!chrono_checkpoint_id_valid(id)) continue;
        chrono_checkpoint_metadata m;
        if(read_metadata(dirfd,id,&m)!=0) { result=-1; break; }
        if(m.state==CHRONO_CHECKPOINT_COMPLETE && callback(&m,context)!=0) { result=-1; break; }
    }
    closedir(dir); close(dirfd);
    if(result!=0) set_error(error,error_size,"checkpoint metadata listing failed");
    return result;
}

int chrono_checkpoint_read(const char *directory, const char *id,
                           chrono_checkpoint_metadata *metadata,
                           char *error, size_t error_size)
{
    int dirfd;
    if (!chrono_checkpoint_id_valid(id) || metadata == NULL ||
        open_directory(directory, 0, &dirfd) != 0) {
        set_error(error, error_size, "cannot open checkpoint metadata");
        return -1;
    }
    int result = read_metadata(dirfd, id, metadata);
    int saved = errno;
    close(dirfd);
    if (result != 0) {
        errno = saved;
        set_error(error, error_size, "cannot read checkpoint metadata");
        return -1;
    }
    return 0;
}

int chrono_checkpoint_artifact_valid(const char *directory, const char *id)
{
    struct stat st;
    int basefd = -1, artifactfd = -1, scanfd = -1;
    DIR *dir = NULL;
    int result = 0;
    if (directory == NULL || !chrono_checkpoint_id_valid(id)) return 0;
    basefd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (basefd < 0 || fstat(basefd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0777) != 0700) goto done;
    artifactfd = openat(basefd, id, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (artifactfd < 0 || fstat(artifactfd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0777) != 0700) goto done;
    scanfd = dup(artifactfd);
    if (scanfd < 0) goto done;
    dir = fdopendir(scanfd);
    if (dir == NULL) goto done;
    int inventory = 0, image_count = 0, valid = 1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        struct stat item, opened;
        int itemfd = openat(dirfd(dir), entry->d_name,
                            O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (itemfd < 0 || fstat(itemfd, &opened) != 0 ||
            fstatat(dirfd(dir), entry->d_name, &item, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(opened.st_mode) || opened.st_dev != item.st_dev || opened.st_ino != item.st_ino ||
            opened.st_uid != getuid() || opened.st_nlink != 1 || (opened.st_mode & 0077) != 0) {
            if (itemfd >= 0) (void)close(itemfd);
            valid = 0; break;
        }
        item = opened;
        size_t name_len = strlen(entry->d_name);
        if (name_len > 4 && strcmp(entry->d_name + name_len - 4, ".img") == 0 &&
            item.st_size > 0) {
            ++image_count;
            if (strcmp(entry->d_name, "inventory.img") == 0) inventory = 1;
        } else if (strcmp(entry->d_name, ".restore-attempted") == 0) {
            if (item.st_size != 0 || (item.st_mode & 0777) != 0600) { valid = 0; break; }
        } else if (strcmp(entry->d_name, "restore.pid") == 0) {
            if (item.st_size <= 0 || item.st_size > 31 || (item.st_mode & 0077) != 0 ||
                (item.st_mode & 0400) == 0) { valid = 0; break; }
        } else {
            /* This adapter supports flat CRIU image directories only. */
            (void)close(itemfd);
            valid = 0;
            break;
        }
        if (close(itemfd) != 0) { valid = 0; break; }
    }
    if (closedir(dir) != 0) valid = 0;
    dir = NULL; scanfd = -1;
    /* Inventory is only a structural marker; it cannot prove image consistency. */
    result = valid && inventory && image_count >= 2;
done:
    if (dir != NULL) (void)closedir(dir);
    else if (scanfd >= 0) (void)close(scanfd);
    if (artifactfd >= 0 && close(artifactfd) != 0) result = 0;
    if (basefd >= 0 && close(basefd) != 0) result = 0;
    return result;
}
