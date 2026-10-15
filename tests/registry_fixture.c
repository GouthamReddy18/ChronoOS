#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int parse_uint(const char *text, uintmax_t *value)
{
    uintmax_t result = 0;
    if (text == NULL || *text == '\0' || (*text == '0' && text[1] != '\0')) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return -1;
        unsigned digit = (unsigned)(*p - '0');
        if (result > (UINTMAX_MAX - digit) / 10) return -1;
        result = result * 10 + digit;
    }
    *value = result;
    return 0;
}

static int same_inode(int dirfd, const char *name, const struct stat *expected)
{
    struct stat current;
    return fstatat(dirfd, name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(current.st_mode) && current.st_dev == expected->st_dev &&
           current.st_ino == expected->st_ino && current.st_uid == getuid() &&
           (current.st_mode & 0777) == 0600 && current.st_nlink == 1;
}

static int write_all(int fd, const char *data, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t n = write(fd, data + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uintmax_t pid_value;
    char directory[64], record[32];
    struct stat st;
    if ((argc != 3 && argc != 4) ||
        (strcmp(argv[1], "create") != 0 && strcmp(argv[1], "check") != 0 && strcmp(argv[1], "remove") != 0) ||
        parse_uint(argv[2], &pid_value) != 0 || pid_value <= 1 || pid_value > 2147483647U) {
        fprintf(stderr, "registry_fixture: invalid arguments\n");
        return 2;
    }
    if ((strcmp(argv[1], "create") == 0 && argc != 3) ||
        (strcmp(argv[1], "create") != 0 && argc != 4)) {
        fprintf(stderr, "registry_fixture: invalid arguments\n");
        return 2;
    }
    (void)snprintf(directory, sizeof(directory), "chronoos-%lu", (unsigned long)getuid());
    int tmpfd = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (tmpfd < 0) return 1;
    int dirfd = openat(tmpfd, directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(tmpfd);
    if (dirfd < 0 || fstat(dirfd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0777) != 0700) {
        if (dirfd >= 0) close(dirfd);
        return 1;
    }
    int lockfd = openat(dirfd, ".lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (lockfd < 0 || fstat(lockfd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 0777) != 0600 || st.st_nlink != 1 ||
        flock(lockfd, LOCK_EX) != 0) {
        if (lockfd >= 0) close(lockfd);
        close(dirfd);
        return 1;
    }
    (void)snprintf(record, sizeof(record), "%ju", pid_value);
    int result = 1;
    if (strcmp(argv[1], "create") == 0) {
        char contents[128];
        size_t length = 0;
        for (;;) {
            if (length == sizeof(contents)) goto done;
            ssize_t n = read(STDIN_FILENO, contents + length, sizeof(contents) - length);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) goto done;
            if (n == 0) break;
            length += (size_t)n;
        }
        if (length == 0 || length == sizeof(contents)) goto done;
        int fd = openat(dirfd, record, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) goto done;
        struct stat created;
        int have_created = fstat(fd, &created) == 0;
        if (!have_created || !S_ISREG(created.st_mode) || created.st_uid != getuid() ||
            created.st_nlink != 1 || fchmod(fd, 0600) != 0 ||
            write_all(fd, contents, length) != 0 || fsync(fd) != 0 || fstat(fd, &created) != 0 ||
            (created.st_mode & 0777) != 0600 || created.st_nlink != 1) {
            int saved = errno;
            close(fd);
            if (have_created && same_inode(dirfd, record, &created)) (void)unlinkat(dirfd, record, 0);
            errno = saved;
            goto done;
        }
        close(fd);
        if (!same_inode(dirfd, record, &created)) goto done;
        printf("%ju:%ju\n", (uintmax_t)created.st_dev, (uintmax_t)created.st_ino);
        result = 0;
    } else {
        uintmax_t dev, ino;
        char *separator = strchr(argv[3], ':');
        if (separator == NULL || strchr(separator + 1, ':') != NULL) goto done;
        *separator = '\0';
        if (parse_uint(argv[3], &dev) != 0 || parse_uint(separator + 1, &ino) != 0) goto done;
        int fd = openat(dirfd, record, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) goto done;
        struct stat fixture;
        int valid = fstat(fd, &fixture) == 0 && S_ISREG(fixture.st_mode) &&
                    fixture.st_uid == getuid() && (fixture.st_mode & 0777) == 0600 &&
                    fixture.st_nlink == 1 && (uintmax_t)fixture.st_dev == dev &&
                    (uintmax_t)fixture.st_ino == ino;
        close(fd);
        if (!valid || !same_inode(dirfd, record, &fixture)) goto done;
        if (strcmp(argv[1], "check") == 0) result = 0;
        else if (unlinkat(dirfd, record, 0) == 0) result = 0;
    }
done:
    close(lockfd);
    close(dirfd);
    return result;
}
