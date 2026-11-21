#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "chrono/supervisor.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RECORD_LIMIT (PATH_MAX * 8U + CHRONO_COMMAND_MAX * 2U + 2048U)
#define SEQUENCE_FILE ".sequence"
#define LOCK_FILE ".lock"

static void set_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size > 0) (void)snprintf(error, size, "%s", message);
}

static void clear_error(char *error, size_t size)
{
    if (error != NULL && size > 0) error[0] = '\0';
}

static void set_errno_error(char *error, size_t size, const char *prefix, int value)
{
    if (error != NULL && size > 0) (void)snprintf(error, size, "%s: %s", prefix, strerror(value));
}

const char *chrono_supervisor_state_name(chrono_supervisor_state state)
{
    switch (state) {
    case CHRONO_SUPERVISOR_STARTING: return "STARTING";
    case CHRONO_SUPERVISOR_RUNNING: return "RUNNING";
    case CHRONO_SUPERVISOR_STOPPING: return "STOPPING";
    case CHRONO_SUPERVISOR_EXITED: return "EXITED";
    case CHRONO_SUPERVISOR_FAILED: return "FAILED";
    }
    return NULL;
}

static int state_parse(const char *text, chrono_supervisor_state *state)
{
    for (int candidate = CHRONO_SUPERVISOR_STARTING; candidate <= CHRONO_SUPERVISOR_FAILED; ++candidate) {
        chrono_supervisor_state item = (chrono_supervisor_state)candidate;
        if (strcmp(text, chrono_supervisor_state_name(item)) == 0) { *state = item; return 0; }
    }
    return -1;
}

int chrono_supervisor_id_valid(const char *id)
{
    if (id == NULL) return 0;
    size_t length = strnlen(id, CHRONO_SUPERVISOR_ID_MAX + 1);
    if (length < 9 || length > CHRONO_SUPERVISOR_ID_MAX || strncmp(id, "cp-", 3) != 0) return 0;
    if (length > 9 && id[3] == '0') return 0;
    for (size_t i = 3; i < length; ++i) if (id[i] < '0' || id[i] > '9') return 0;
    return 1;
}

static int directory_component_safe(const struct stat *st, uid_t uid, int final_component)
{
    if (!S_ISDIR(st->st_mode)) return 0;
    if (final_component) return st->st_uid == uid && (st->st_mode & 0777) == 0700;
    if (st->st_uid != uid && st->st_uid != 0) return 0;
    if (st->st_uid == uid && (st->st_mode & 0022) != 0) return 0;
    if (st->st_uid == 0 && (st->st_mode & 0022) != 0 &&
        !((st->st_mode & S_ISVTX) != 0 && (st->st_mode & 0002) != 0)) return 0;
    return 1;
}

static int ensure_directory_path(const char *path, char *error, size_t error_size)
{
    if (path == NULL || path[0] != '/') { set_error(error, error_size, "registry path must be absolute"); return -1; }
    size_t length = strnlen(path, CHRONO_SUPERVISOR_PATH_MAX);
    if (length == 0 || length >= CHRONO_SUPERVISOR_PATH_MAX) {
        set_error(error, error_size, "registry path is empty or too long"); return -1;
    }
    char copy[CHRONO_SUPERVISOR_PATH_MAX]; memcpy(copy, path, length + 1);
    int dirfd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) { set_errno_error(error, error_size, "cannot open root directory", errno); return -1; }
    uid_t uid = getuid(); char *cursor = copy + 1; int failed = 0;
    while (*cursor != '\0') {
        char *slash = strchr(cursor, '/');
        if (slash != NULL) *slash = '\0';
        int final_component = slash == NULL || slash[1] == '\0';
        if (cursor[0] == '\0' || strcmp(cursor, ".") == 0 || strcmp(cursor, "..") == 0) {
            set_error(error, error_size, "registry path contains an invalid component"); failed = 1; break;
        }
        if (mkdirat(dirfd, cursor, 0700) != 0 && errno != EEXIST) {
            set_errno_error(error, error_size, "cannot create registry directory", errno); failed = 1; break;
        }
        int next = openat(dirfd, cursor, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) { set_errno_error(error, error_size, "registry path component is not a safe directory", errno); failed = 1; break; }
        struct stat st;
        if (fstat(next, &st) != 0 || !directory_component_safe(&st, uid, final_component)) {
            close(next); set_error(error, error_size, final_component ?
                                   "registry directory must be owned by this user with mode 0700" :
                                   "registry parent directory has unsafe ownership or permissions"); failed = 1; break;
        }
        close(dirfd); dirfd = next;
        if (slash == NULL || final_component) break;
        cursor = slash + 1;
    }
    close(dirfd);
    return failed ? -1 : 0;
}

int chrono_supervisor_default_directory(char *path, size_t path_size,
                                        char *error, size_t error_size)
{
    clear_error(error, error_size);
    const char *state_home = getenv("XDG_STATE_HOME");
    char base[CHRONO_SUPERVISOR_PATH_MAX];
    if (state_home != NULL && state_home[0] != '\0') {
        if (state_home[0] != '/') { set_error(error, error_size, "XDG_STATE_HOME must be an absolute path"); return -1; }
        int n = snprintf(base, sizeof(base), "%s", state_home);
        if (n < 0 || (size_t)n >= sizeof(base)) { set_error(error, error_size, "XDG_STATE_HOME path is too long"); return -1; }
        size_t base_length = strlen(base);
        while (base_length > 1 && base[base_length - 1] == '/') base[--base_length] = '\0';
    } else {
        long hint = sysconf(_SC_GETPW_R_SIZE_MAX); if (hint < 1024) hint = 16384;
        char *storage = malloc((size_t)hint);
        if (storage == NULL) { set_error(error, error_size, "cannot allocate user account lookup buffer"); return -1; }
        struct passwd entry, *found = NULL;
        int lookup = getpwuid_r(getuid(), &entry, storage, (size_t)hint, &found);
        if (lookup != 0 || found == NULL || entry.pw_dir == NULL || entry.pw_dir[0] != '/') {
            free(storage); set_error(error, error_size, "cannot determine the current user's home directory"); return -1;
        }
        int n = snprintf(base, sizeof(base), "%s/.local/state", entry.pw_dir);
        free(storage);
        if (n < 0 || (size_t)n >= sizeof(base)) { set_error(error, error_size, "user state path is too long"); return -1; }
    }
    int n = snprintf(path, path_size, "%s/chronoos", base);
    if (n < 0 || (size_t)n >= path_size) { set_error(error, error_size, "ChronoOS registry path is too long"); return -1; }
    return ensure_directory_path(path, error, error_size);
}

static int open_registry(const char *path, int *dirfd_out, int *lockfd_out,
                         char *error, size_t error_size)
{
    struct stat st;
    int dirfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) { set_errno_error(error, error_size, "cannot open ChronoOS process registry", errno); return -1; }
    if (fstat(dirfd, &st) != 0) {
        int saved = errno;
        close(dirfd);
        set_errno_error(error, error_size, "cannot inspect ChronoOS process registry", saved);
        return -1;
    }
    if (!directory_component_safe(&st, getuid(), 1)) {
        close(dirfd); set_error(error, error_size, "process registry must be owned mode 0700"); return -1;
    }
    int lockfd = openat(dirfd, LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockfd < 0) {
        int saved = errno;
        close(dirfd);
        set_errno_error(error, error_size, "cannot open private process registry lock", saved);
        return -1;
    }
    if (fstat(lockfd, &st) != 0) {
        int saved = errno;
        close(lockfd); close(dirfd);
        set_errno_error(error, error_size, "cannot inspect private process registry lock", saved);
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1 ||
        (st.st_mode & 0777) != 0600) {
        close(lockfd); close(dirfd);
        set_error(error, error_size, "private process registry lock has unsafe type, owner, link count, or mode");
        return -1;
    }
    if (flock(lockfd, LOCK_EX) != 0) {
        int saved = errno;
        close(lockfd); close(dirfd);
        set_errno_error(error, error_size, "cannot lock private process registry", saved);
        return -1;
    }
    *dirfd_out = dirfd; *lockfd_out = lockfd; return 0;
}

static void close_registry(int dirfd, int lockfd)
{
    (void)flock(lockfd, LOCK_UN); (void)close(lockfd); (void)close(dirfd);
}

static int cleanup_child_bounded(pid_t child, const chrono_supervised_process *expected,
                                 int *signal_requested, char *error, size_t error_size);

static int write_all(int fd, const char *data, size_t size)
{
    while (size > 0) {
        ssize_t count = write(fd, data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        data += (size_t)count; size -= (size_t)count;
    }
    return 0;
}

static int atomic_write_at(int dirfd, const char *name, const char *data, size_t length,
                           int exclusive, char *error, size_t error_size)
{
    char temporary[96]; int fd = -1;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        int n = snprintf(temporary, sizeof(temporary), ".tmp-%ld-%lld-%u",
                         (long)getpid(), (long long)time(NULL), attempt);
        if (n < 0 || (size_t)n >= sizeof(temporary)) { set_error(error, error_size, "temporary registry filename overflow"); return -1; }
        fd = openat(dirfd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) { set_errno_error(error, error_size, "cannot create atomic registry temporary", errno); return -1; }
    }
    if (fd < 0) { set_error(error, error_size, "cannot allocate unique registry temporary file"); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd); (void)unlinkat(dirfd, temporary, 0);
        set_errno_error(error, error_size, "cannot inspect atomic registry temporary", saved);
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_nlink != 1) {
        close(fd); (void)unlinkat(dirfd, temporary, 0);
        set_error(error, error_size, "atomic registry temporary has unsafe type, ownership, or link count");
        return -1;
    }
    if (fchmod(fd, 0600) != 0) {
        int saved = errno;
        close(fd); (void)unlinkat(dirfd, temporary, 0);
        set_errno_error(error, error_size, "cannot set atomic registry temporary mode", saved);
        return -1;
    }
    int ok = 1;
    int saved = 0;
    if (ok && write_all(fd, data, length) != 0) { ok = 0; saved = errno; }
    if (ok && fsync(fd) != 0) { ok = 0; saved = errno; }
    if (close(fd) != 0 && ok) { ok = 0; saved = errno; }
    if (!ok) {
        (void)unlinkat(dirfd, temporary, 0); set_errno_error(error, error_size, "cannot write atomic registry file", saved); return -1;
    }
    int published;
    if (exclusive) {
        published = linkat(dirfd, temporary, dirfd, name, 0);
        saved = errno;
        /* Once the exclusive hard link is visible, a stale hidden temporary
         * file is harmless; do not report failed registration after publish. */
        (void)unlinkat(dirfd, temporary, 0);
    } else {
        published = renameat(dirfd, temporary, dirfd, name); saved = errno;
        if (published != 0) (void)unlinkat(dirfd, temporary, 0);
    }
    if (published != 0) { set_errno_error(error, error_size, "cannot publish registry update", saved); return -1; }
    if (fsync(dirfd) != 0) { set_errno_error(error, error_size, "registry changed but directory sync failed", errno); return -1; }
    return 0;
}

static int encode_hex(const char *input, size_t input_length, char *output, size_t output_size)
{
    static const char digits[] = "0123456789abcdef";
    if (input_length > (output_size - 1) / 2) return -1;
    for (size_t i = 0; i < input_length; ++i) {
        unsigned char value = (unsigned char)input[i];
        output[i * 2] = digits[value >> 4]; output[i * 2 + 1] = digits[value & 15];
    }
    output[input_length * 2] = '\0'; return 0;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static int decode_hex(const char *input, char *output, size_t output_size, int allow_empty)
{
    size_t length = strlen(input);
    if ((length & 1U) != 0 || length / 2 >= output_size || (!allow_empty && length == 0)) return -1;
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_value(input[i]), low = hex_value(input[i + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) return -1;
        output[i / 2] = (char)((high << 4) | low);
    }
    output[length / 2] = '\0'; return 0;
}

static int serialize_record(const chrono_supervised_process *p, char *data, size_t size)
{
    char exe[CHRONO_SUPERVISOR_PATH_MAX * 2], command[CHRONO_COMMAND_MAX * 2];
    char cwd[CHRONO_SUPERVISOR_PATH_MAX * 2], checkpoint[CHRONO_SUPERVISOR_PATH_MAX * 2];
    if (!chrono_supervisor_id_valid(p->id) || p->pid <= 1 || p->start_time == 0 ||
        p->executable_inode == 0 || p->registered_at <= 0 || p->state_changed_at <= 0 ||
        p->owner != getuid() || chrono_supervisor_state_name(p->state) == NULL ||
        p->intentional_stop < 0 || p->intentional_stop > 1 || p->exit_status_known < 0 || p->exit_status_known > 1 ||
        p->exit_code < 0 || p->exit_code > 255 || p->term_signal < 0 || p->term_signal >= NSIG ||
        p->executable[0] != '/' || p->working_directory[0] != '/' || p->command[0] == '\0' ||
        p->checkpoint_interval_seconds < CHRONO_CHECKPOINT_INTERVAL_MIN ||
        p->checkpoint_interval_seconds > CHRONO_CHECKPOINT_INTERVAL_MAX ||
        (p->checkpoint_directory[0] != '\0' && p->checkpoint_directory[0] != '/') ||
        encode_hex(p->executable, strlen(p->executable), exe, sizeof(exe)) != 0 ||
        encode_hex(p->command, strlen(p->command), command, sizeof(command)) != 0 ||
        encode_hex(p->working_directory, strlen(p->working_directory), cwd, sizeof(cwd)) != 0 ||
        encode_hex(p->checkpoint_directory, strlen(p->checkpoint_directory), checkpoint, sizeof(checkpoint)) != 0) return -1;
    int n = snprintf(data, size,
        "CHRONO-PROCESS-2\n%s\n%ld\n%llu\n%lu\n%llu\n%llu\n%lld\n%lld\n%s\n%d\n%d\n%d\n%d\n%s\n%s\n%s\n%s\n%u\n",
        p->id, (long)p->pid, (unsigned long long)p->start_time, (unsigned long)p->owner,
        (unsigned long long)p->executable_device, (unsigned long long)p->executable_inode,
        (long long)p->registered_at, (long long)p->state_changed_at,
        chrono_supervisor_state_name(p->state), p->intentional_stop, p->exit_status_known,
        p->exit_code, p->term_signal, exe, command, cwd, checkpoint,
        p->checkpoint_interval_seconds);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

static int decimal_u64(const char *text, uint64_t *value, int allow_zero)
{
    if (text == NULL || text[0] == '\0') return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end; unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || (!allow_zero && parsed == 0)) return -1;
    *value = (uint64_t)parsed; return 0;
}

static int signed_decimal(const char *text, int64_t *value)
{
    if (text == NULL || text[0] == '\0') return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end; long long parsed = strtoll(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed <= 0) return -1;
    *value = (int64_t)parsed; return 0;
}

static int parse_record(char *data, size_t length, const char *expected_id,
                        chrono_supervised_process *p)
{
    char *lines[19]; size_t count = 0; char *cursor = data; char *limit = data + length;
    if (length == 0 || length >= RECORD_LIMIT || data[length - 1] != '\n' || memchr(data, '\0', length) != NULL) return -1;
    while (cursor < limit) {
        if (count >= sizeof(lines) / sizeof(lines[0])) return -1;
        lines[count++] = cursor;
        char *newline = memchr(cursor, '\n', (size_t)(limit - cursor));
        if (newline == NULL) return -1;
        *newline = '\0'; cursor = newline + 1;
    }
    int version_one = count == 18 && strcmp(lines[0], "CHRONO-PROCESS-1") == 0;
    int version_two = count == 19 && strcmp(lines[0], "CHRONO-PROCESS-2") == 0;
    if ((!version_one && !version_two) || strcmp(lines[1], expected_id) != 0 ||
        !chrono_supervisor_id_valid(lines[1])) return -1;
    memset(p, 0, sizeof(*p)); (void)snprintf(p->id, sizeof(p->id), "%s", lines[1]);
    uint64_t values[6]; int64_t timestamps[2];
    if (decimal_u64(lines[2], &values[0], 0) != 0 || values[0] <= 1 || values[0] > INT_MAX ||
        decimal_u64(lines[3], &values[1], 0) != 0 ||
        decimal_u64(lines[4], &values[2], 1) != 0 ||
        decimal_u64(lines[5], &values[3], 1) != 0 ||
        decimal_u64(lines[6], &values[4], 0) != 0 ||
        signed_decimal(lines[7], &timestamps[0]) != 0 || signed_decimal(lines[8], &timestamps[1]) != 0 ||
        state_parse(lines[9], &p->state) != 0) return -1;
    uid_t owner = (uid_t)values[2]; dev_t device = (dev_t)values[3]; ino_t inode = (ino_t)values[4];
    if ((uint64_t)owner != values[2] || (uint64_t)device != values[3] || (uint64_t)inode != values[4]) return -1;
    p->pid = (pid_t)values[0]; p->start_time = values[1]; p->owner = owner;
    p->executable_device = device; p->executable_inode = inode;
    p->registered_at = timestamps[0]; p->state_changed_at = timestamps[1];
    uint64_t small_unsigned;
    if (decimal_u64(lines[10], &small_unsigned, 1) != 0 || small_unsigned > 1) return -1;
    int64_t small = (int64_t)small_unsigned;
    p->intentional_stop = (int)small;
    if (decimal_u64(lines[11], &small_unsigned, 1) != 0 || small_unsigned > 1) return -1;
    small = (int64_t)small_unsigned;
    p->exit_status_known = (int)small;
    /* Lifecycle values allow zero but remain unsigned decimal fields. */
    if (decimal_u64(lines[12], &small_unsigned, 1) != 0 || small_unsigned > 255) return -1;
    p->exit_code = (int)small_unsigned;
    if (decimal_u64(lines[13], &small_unsigned, 1) != 0 || small_unsigned >= NSIG) return -1;
    p->term_signal = (int)small_unsigned;
    if (decode_hex(lines[14], p->executable, sizeof(p->executable), 0) != 0 ||
        decode_hex(lines[15], p->command, sizeof(p->command), 0) != 0 ||
        decode_hex(lines[16], p->working_directory, sizeof(p->working_directory), 0) != 0 ||
        decode_hex(lines[17], p->checkpoint_directory, sizeof(p->checkpoint_directory), 1) != 0 ||
        p->executable[0] != '/' || p->working_directory[0] != '/' || p->command[0] == '\0' ||
        (p->checkpoint_directory[0] != '\0' && p->checkpoint_directory[0] != '/')) return -1;
    p->checkpoint_interval_seconds = CHRONO_CHECKPOINT_INTERVAL_DEFAULT;
    if (version_two) {
        uint64_t interval;
        if (decimal_u64(lines[18], &interval, 0) != 0 ||
            interval < CHRONO_CHECKPOINT_INTERVAL_MIN || interval > CHRONO_CHECKPOINT_INTERVAL_MAX)
            return -1;
        p->checkpoint_interval_seconds = (uint32_t)interval;
    }
    if ((p->state == CHRONO_SUPERVISOR_STOPPING && !p->intentional_stop) ||
        ((p->state == CHRONO_SUPERVISOR_STARTING || p->state == CHRONO_SUPERVISOR_RUNNING) &&
         (p->intentional_stop || p->exit_status_known || p->term_signal != 0)) ||
        (p->exit_status_known && p->state != CHRONO_SUPERVISOR_EXITED && p->state != CHRONO_SUPERVISOR_FAILED)) return -1;
    return 0;
}

static int record_filename(const char *id, char *name, size_t size)
{
    if (!chrono_supervisor_id_valid(id)) return -1;
    int n = snprintf(name, size, "%s.rec", id);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static int load_record_at(int dirfd, const char *id, chrono_supervised_process *p,
                          char *error, size_t error_size)
{
    char name[CHRONO_SUPERVISOR_ID_MAX + 8];
    if (record_filename(id, name, sizeof(name)) != 0) { set_error(error, error_size, "invalid ChronoOS process ID"); return -1; }
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { set_errno_error(error, error_size, "cannot open process record", errno); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0777) != 0600 || st.st_nlink != 1 || st.st_size <= 0 || (uintmax_t)st.st_size >= RECORD_LIMIT) {
        close(fd); set_error(error, error_size, "process record has unsafe type, ownership, mode, or size"); return -1;
    }
    size_t capacity = (size_t)st.st_size + 1; char *data = malloc(capacity);
    if (data == NULL) { close(fd); set_error(error, error_size, "cannot allocate process record buffer"); return -1; }
    size_t used = 0;
    while (used < capacity - 1) {
        ssize_t n = read(fd, data + used, capacity - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { int saved = errno; free(data); close(fd); set_errno_error(error, error_size, "cannot read process record", saved); return -1; }
        if (n == 0) break;
        used += (size_t)n;
    }
    char extra; ssize_t more = read(fd, &extra, 1); int read_error = errno;
    int close_result = close(fd);
    if (more != 0 || close_result != 0 || used != capacity - 1 || parse_record(data, used, id, p) != 0) {
        free(data); errno = read_error; set_error(error, error_size, "process record is malformed or uses an unsupported version"); return -1;
    }
    free(data); return 0;
}

static int store_record_at(int dirfd, const chrono_supervised_process *p, int exclusive,
                           char *error, size_t error_size)
{
    char name[CHRONO_SUPERVISOR_ID_MAX + 8], data[RECORD_LIMIT];
    if (record_filename(p->id, name, sizeof(name)) != 0) { set_error(error, error_size, "invalid process ID in registry update"); return -1; }
    int length = serialize_record(p, data, sizeof(data));
    if (length < 0) { set_error(error, error_size, "process metadata cannot be serialized"); return -1; }
    return atomic_write_at(dirfd, name, data, (size_t)length, exclusive, error, error_size);
}

static int sequence_read(int dirfd, uint64_t *sequence, char *error, size_t error_size)
{
    int fd = openat(dirfd, SEQUENCE_FILE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        int duplicate = dup(dirfd); DIR *directory = duplicate < 0 ? NULL : fdopendir(duplicate);
        if (directory == NULL) { if (duplicate >= 0) close(duplicate); set_error(error, error_size, "cannot inspect process registry before initializing sequence"); return -1; }
        struct dirent *entry; int has_records = 0;
        while ((entry = readdir(directory)) != NULL) {
            size_t length = strlen(entry->d_name);
            if (length >= 4 && strcmp(entry->d_name + length - 4, ".rec") == 0) { has_records = 1; break; }
        }
        (void)closedir(directory);
        if (has_records) { set_error(error, error_size, "process ID sequence is missing while records exist; refusing to reuse IDs"); return -1; }
        *sequence = 0; return 0;
    }
    if (fd < 0) { set_errno_error(error, error_size, "cannot open process ID sequence", errno); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0777) != 0600 || st.st_nlink != 1 || st.st_size <= 0 || st.st_size > 64) {
        close(fd); set_error(error, error_size, "process ID sequence file is unsafe or malformed"); return -1;
    }
    char data[65]; ssize_t n;
    do { n = read(fd, data, sizeof(data) - 1); } while (n < 0 && errno == EINTR);
    int saved = errno; close(fd); errno = saved;
    if (n <= 0) { set_error(error, error_size, "cannot read process ID sequence"); return -1; }
    data[n] = '\0';
    if (strncmp(data, "CHRONOSEQ1\n", 11) != 0) { set_error(error, error_size, "process ID sequence uses an unsupported version"); return -1; }
    char *value_text = data + 11; char *newline = strchr(value_text, '\n');
    if (newline == NULL || newline[1] != '\0') { set_error(error, error_size, "process ID sequence is malformed"); return -1; }
    *newline = '\0';
    if (decimal_u64(value_text, sequence, 1) != 0) { set_error(error, error_size, "process ID sequence is malformed"); return -1; }
    return 0;
}

static int sequence_store(int dirfd, uint64_t value, char *error, size_t error_size)
{
    char data[96]; int n = snprintf(data, sizeof(data), "CHRONOSEQ1\n%llu\n", (unsigned long long)value);
    if (n < 0 || (size_t)n >= sizeof(data)) { set_error(error, error_size, "process ID sequence overflow"); return -1; }
    return atomic_write_at(dirfd, SEQUENCE_FILE, data, (size_t)n, 0, error, error_size);
}

static int read_exec_error(int fd, int *child_error)
{
    size_t used = 0;
    while (used < sizeof(*child_error)) {
        ssize_t n = read(fd, (char *)child_error + used, sizeof(*child_error) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) return used == 0 ? 0 : -1;
        used += (size_t)n;
    }
    return 1;
}

static int launch_argv(char *const argv[], pid_t *pid, char *error, size_t error_size)
{
    if (argv == NULL || argv[0] == NULL || argv[0][0] == '\0') { set_error(error, error_size, "protect requires a nonempty executable"); return -1; }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) { set_errno_error(error, error_size, "cannot create exec handshake", errno); return -1; }
    pid_t child = fork();
    if (child < 0) { int saved = errno; close(pipefd[0]); close(pipefd[1]); set_errno_error(error, error_size, "cannot fork protected workload", saved); return -1; }
    if (child == 0) {
        close(pipefd[0]);
        if (setsid() < 0) { int e = errno; ssize_t ignored = write(pipefd[1], &e, sizeof(e)); (void)ignored; _exit(127); }
        execvp(argv[0], argv);
        int e = errno; ssize_t ignored = write(pipefd[1], &e, sizeof(e)); (void)ignored; _exit(127);
    }
    close(pipefd[1]); int child_error = 0;
    int handshake = read_exec_error(pipefd[0], &child_error); int read_error = errno; close(pipefd[0]);
    if (handshake != 0) {
        *pid = child;
        if (handshake > 0) set_errno_error(error, error_size, "cannot execute protected workload", child_error);
        else set_errno_error(error, error_size, "cannot read protected workload exec handshake", read_error);
        return -2;
    }
    *pid = child; return 0;
}

static int collect_identity(pid_t pid, chrono_supervised_process *p,
                            char *error, size_t error_size)
{
    chrono_process_info info;
    if (chrono_process_inspect(pid, &info, error, error_size) != 0) return -1;
    if (info.pid != pid || info.start_time == 0 || info.owner != getuid() ||
        info.executable[0] != '/' || info.executable_inode == 0 || info.command[0] == '\0') {
        set_error(error, error_size, "protected process identity is incomplete or does not belong to this user"); return -1;
    }
    char cwd_path[64]; (void)snprintf(cwd_path, sizeof(cwd_path), "/proc/%ld/cwd", (long)pid);
    ssize_t cwd_length = readlink(cwd_path, p->working_directory, sizeof(p->working_directory) - 1);
    if (cwd_length < 0 || (size_t)cwd_length >= sizeof(p->working_directory) - 1) {
        set_error(error, error_size, "cannot capture protected process working directory"); return -1;
    }
    p->working_directory[cwd_length] = '\0';
    p->pid = pid; p->start_time = info.start_time; p->owner = info.owner;
    p->executable_device = info.executable_device; p->executable_inode = info.executable_inode;
    if (snprintf(p->executable, sizeof(p->executable), "%s", info.executable) >= (int)sizeof(p->executable) ||
        snprintf(p->command, sizeof(p->command), "%s", info.command) >= (int)sizeof(p->command)) {
        set_error(error, error_size, "protected process identity text is too long"); return -1;
    }
    return 0;
}

static int update_time(chrono_supervised_process *p)
{
    time_t now = time(NULL);
    if (now <= 0) return -1;
    p->state_changed_at = (int64_t)now; return 0;
}

static int record_matches_process(const chrono_supervised_process *record,
                                 const chrono_process_info *info)
{
    return info->pid == record->pid && info->start_time == record->start_time &&
           info->owner == record->owner && info->executable_device == record->executable_device &&
           info->executable_inode == record->executable_inode &&
           strcmp(info->executable, record->executable) == 0 &&
           strcmp(info->command, record->command) == 0;
}

static int reap_if_our_child(chrono_supervised_process *p, int *reaped)
{
    int status; pid_t result;
    do { result = waitpid(p->pid, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0 || (result < 0 && errno == ECHILD)) { *reaped = 0; return 0; }
    if (result < 0) return -1;
    *reaped = 1;
    if (WIFEXITED(status)) {
        p->exit_status_known = 1; p->exit_code = WEXITSTATUS(status); p->term_signal = 0;
        p->state = (p->intentional_stop || p->exit_code == 0) ? CHRONO_SUPERVISOR_EXITED : CHRONO_SUPERVISOR_FAILED;
    } else if (WIFSIGNALED(status)) {
        p->exit_status_known = 0; p->term_signal = WTERMSIG(status);
        p->state = p->intentional_stop ? CHRONO_SUPERVISOR_EXITED : CHRONO_SUPERVISOR_FAILED;
    } else {
        p->state = CHRONO_SUPERVISOR_FAILED;
    }
    return update_time(p);
}

/* 1 means the /proc entry exists, 0 means ENOENT/ESRCH confirmed absence,
 * and -1 means absence could not be established. */
static int proc_entry_present(pid_t pid, int *present)
{
    char path[64];
    (void)snprintf(path, sizeof(path), "/proc/%ld", (long)pid);
    struct stat st;
    if (stat(path, &st) == 0) {
        *present = 1;
        return 0;
    }
    if (errno == ENOENT || errno == ESRCH) {
        *present = 0;
        return 0;
    }
    return -1;
}

static int persist_terminal_state(int dirfd, chrono_supervised_process *p,
                                  char *error, size_t error_size)
{
    p->state = p->intentional_stop ? CHRONO_SUPERVISOR_EXITED : CHRONO_SUPERVISOR_FAILED;
    p->exit_status_known = 0;
    p->exit_code = 0;
    p->term_signal = 0;
    if (update_time(p) != 0) {
        set_error(error, error_size, "cannot timestamp confirmed process termination");
        return -1;
    }
    return store_record_at(dirfd, p, 0, error, error_size);
}

/* Registration cleanup only considers the direct child returned by fork().
 * It sends SIGTERM through the existing verified pidfd helper, then waits a
 * bounded five seconds. No unverified escalation is attempted. */
static int cleanup_child_bounded(pid_t child, const chrono_supervised_process *expected,
                                 int *signal_requested, char *error, size_t error_size)
{
    if (signal_requested != NULL) *signal_requested = 0;
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
    if (waited == child) return 0;
    if (waited < 0 && errno != ECHILD) {
        set_errno_error(error, error_size, "cannot inspect protected child during cleanup", errno);
        return -1;
    }

    chrono_process_info current;
    char inspect_error[256] = {0};
    if (chrono_process_inspect(child, &current, inspect_error, sizeof(inspect_error)) != 0) {
        int present;
        if (proc_entry_present(child, &present) != 0) {
            set_errno_error(error, error_size, "cannot establish child state during cleanup", errno);
            return -1;
        }
        if (!present) return 0;
        (void)snprintf(error, error_size,
                       "child PID %ld remains present but identity could not be verified; no signal sent (%s)",
                       (long)child, inspect_error[0] ? inspect_error : "inspection failed");
        return -1;
    }
    if (current.parent_pid != getpid() || current.owner != getuid() || current.state == 'Z' ||
        (expected != NULL && !record_matches_process(expected, &current))) {
        (void)snprintf(error, error_size,
                       "child PID %ld identity/parent check failed; no signal sent",
                       (long)child);
        return -1;
    }
    uint64_t child_start = current.start_time;
    if (chrono_process_stop_verified(child, child_start, current.executable_device,
                                     current.executable_inode, current.command,
                                     error, error_size) != 0) {
        char detail[256];
        (void)snprintf(detail, sizeof(detail), "%s",
                       error != NULL && error_size ? error : "verified SIGTERM failed");
        (void)snprintf(error, error_size, "child PID %ld STARTTIME %llu was not signaled: %.170s",
                       (long)child, (unsigned long long)child_start, detail);
        return -1;
    }
    if (signal_requested != NULL) *signal_requested = 1;

    for (int attempt = 0; attempt < 100; ++attempt) {
        do { waited = waitpid(child, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
        if (waited == child) return 0;
        if (waited < 0 && errno != ECHILD) {
            set_errno_error(error, error_size, "cannot reap protected child during bounded cleanup", errno);
            return -1;
        }
        if (chrono_process_inspect(child, &current, inspect_error, sizeof(inspect_error)) != 0) {
            int present;
            if (proc_entry_present(child, &present) != 0) {
                set_errno_error(error, error_size, "cannot establish child state during bounded cleanup", errno);
                return -1;
            }
            if (!present) return 0;
        } else if (current.start_time != child_start) {
            /* A different start time proves the launched child identity ended. */
            return 0;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    if (chrono_process_inspect(child, &current, inspect_error, sizeof(inspect_error)) == 0)
        (void)snprintf(error, error_size,
                       "verified SIGTERM did not terminate child PID %ld STARTTIME %llu within 5 seconds; no escalation is supported",
                       (long)child, (unsigned long long)child_start);
    else
        (void)snprintf(error, error_size,
                       "child PID %ld STARTTIME %llu may remain after verified SIGTERM; identity could not be rechecked within 5 seconds; no escalation is supported",
                       (long)child, (unsigned long long)child_start);
    return -1;
}

static const chrono_supervised_process *complete_identity_or_null(
    const chrono_supervised_process *process)
{
    return process->start_time != 0 && process->owner == getuid() &&
           process->executable_inode != 0 && process->command[0] != '\0' ?
           process : NULL;
}

static void combine_primary_and_cleanup_error(char *error, size_t error_size,
                                             const char *primary,
                                             const char *cleanup)
{
    if (error == NULL || error_size == 0) return;
    (void)snprintf(error, error_size, "%s; cleanup: %s",
                   primary != NULL && primary[0] ? primary : "operation failed",
                   cleanup != NULL && cleanup[0] ? cleanup : "child cleanup outcome unavailable");
}

static int request_stop_locked(int dirfd, chrono_supervised_process *process,
                               char *error, size_t error_size)
{
    process->intentional_stop = 1;
    process->state = CHRONO_SUPERVISOR_STOPPING;
    if (update_time(process) != 0) {
        set_error(error, error_size, "cannot timestamp registration cleanup request");
        return -1;
    }
    return store_record_at(dirfd, process, 0, error, error_size);
}

static int refresh_locked(int dirfd, chrono_supervised_process *p,
                          char *error, size_t error_size)
{
    if (p->state != CHRONO_SUPERVISOR_STARTING && p->state != CHRONO_SUPERVISOR_RUNNING &&
        p->state != CHRONO_SUPERVISOR_STOPPING) return 0;
    chrono_process_info current;
    char inspect_error[256] = {0};
    if (chrono_process_inspect(p->pid, &current, inspect_error, sizeof(inspect_error)) == 0) {
        if (current.start_time != p->start_time) {
            if (persist_terminal_state(dirfd, p, error, error_size) != 0) return -1;
            return 0;
        }
        if (current.state == 'Z') {
            int reaped = 0;
            if (reap_if_our_child(p, &reaped) != 0) {
                set_errno_error(error, error_size, "cannot collect protected child status", errno);
                return -1;
            }
            if (reaped) return store_record_at(dirfd, p, 0, error, error_size);
            return persist_terminal_state(dirfd, p, error, error_size);
        }
        if (!record_matches_process(p, &current)) {
            if (p->state == CHRONO_SUPERVISOR_STOPPING) {
                set_error(error, error_size, "process identity changed while STOPPING; state preserved and no signal sent");
                return -1;
            }
            p->state = CHRONO_SUPERVISOR_FAILED;
            if (update_time(p) != 0 || store_record_at(dirfd, p, 0, error, error_size) != 0) return -1;
            set_error(error, error_size, "live process identity no longer matches the registry record");
            return 1;
        }
        if (p->state == CHRONO_SUPERVISOR_STARTING) {
            p->state = CHRONO_SUPERVISOR_RUNNING;
            if (update_time(p) != 0 || store_record_at(dirfd, p, 0, error, error_size) != 0) return -1;
        }
        return 0;
    } else {
        int present;
        if (proc_entry_present(p->pid, &present) != 0) {
            set_errno_error(error, error_size, "cannot establish protected process presence", errno);
            return -1;
        }
        if (present) {
            set_error(error, error_size, inspect_error[0] ? inspect_error : "process metadata inspection failed; lifecycle state preserved");
            return -1;
        }
    }
    return persist_terminal_state(dirfd, p, error, error_size);
}

static int id_from_sequence(uint64_t value, char *id, size_t size)
{
    int n = snprintf(id, size, "cp-%06llu", (unsigned long long)value);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static int supervisor_protect_internal(const char *registry_directory,
                                      char *const argv[], int bundled_counter,
                                      uint32_t checkpoint_interval_seconds,
                                      chrono_supervised_process *process,
                                      char *error, size_t error_size)
{
    clear_error(error, error_size);
    if (checkpoint_interval_seconds < CHRONO_CHECKPOINT_INTERVAL_MIN ||
        checkpoint_interval_seconds > CHRONO_CHECKPOINT_INTERVAL_MAX) {
        set_error(error, error_size, "checkpoint interval must be between 1 and 3600 seconds");
        return -1;
    }
    if (process == NULL) { set_error(error, error_size, "missing process result storage"); return -1; }
    chrono_supervised_process candidate; memset(&candidate, 0, sizeof(candidate));
    candidate.checkpoint_interval_seconds = checkpoint_interval_seconds;
    char cwd[CHRONO_SUPERVISOR_PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) { set_errno_error(error, error_size, "cannot capture current working directory", errno); return -1; }
    pid_t child;
    uint64_t bundled_start = 0;
    int launch_result;
    if (bundled_counter) {
        launch_result = chrono_managed_start_bundled(1, &child, &bundled_start,
                                                     error, error_size);
    } else {
        launch_result = launch_argv(argv, &child, error, error_size);
    }
    if (launch_result != 0) {
        if (launch_result == -2) {
            char primary[256], cleanup_error[256] = {0};
            (void)snprintf(primary, sizeof(primary), "%s",
                           error != NULL && error_size && error[0] ? error : "workload exec handshake failed");
            if (cleanup_child_bounded(child, NULL, NULL, cleanup_error, sizeof(cleanup_error)) != 0)
                combine_primary_and_cleanup_error(error, error_size, primary, cleanup_error);
            else
                set_error(error, error_size, primary);
        }
        return -1;
    }
    int identity_found = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (collect_identity(child, &candidate, error, error_size) == 0 &&
            (!bundled_counter || candidate.start_time == bundled_start)) { identity_found = 1; break; }
        int status; pid_t waited;
        do { waited = waitpid(child, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
        if (waited == child) {
            if (WIFEXITED(status)) (void)snprintf(error, error_size, "protected process exited during startup with status %d", WEXITSTATUS(status));
            else if (WIFSIGNALED(status)) (void)snprintf(error, error_size, "protected process was signaled during startup (%d)", WTERMSIG(status));
            else set_error(error, error_size, "protected process exited during startup");
            return -1;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    if (!identity_found) {
        char primary[256] = "could not establish protected process identity";
        char cleanup_error[256] = {0};
        (void)snprintf(primary, sizeof(primary), "%s", error != NULL && error_size && error[0] ?
                       error : "could not establish protected process identity");
        if (cleanup_child_bounded(child, complete_identity_or_null(&candidate), NULL,
                                  cleanup_error, sizeof(cleanup_error)) != 0)
            combine_primary_and_cleanup_error(error, error_size, primary, cleanup_error);
        else
            set_error(error, error_size, primary);
        return -1;
    }
    clear_error(error, error_size);
    (void)snprintf(candidate.working_directory, sizeof(candidate.working_directory), "%s", cwd);
    candidate.owner = getuid(); candidate.state = CHRONO_SUPERVISOR_STARTING;
    candidate.intentional_stop = 0; candidate.exit_status_known = 0;
    time_t now = time(NULL);
    if (now <= 0) { set_error(error, error_size, "cannot timestamp process registration"); goto terminate_unregistered; }
    candidate.registered_at = candidate.state_changed_at = (int64_t)now;

    int dirfd, lockfd;
    if (open_registry(registry_directory, &dirfd, &lockfd, error, error_size) != 0) goto terminate_unregistered;
    uint64_t sequence;
    if (sequence_read(dirfd, &sequence, error, error_size) != 0) {
        close_registry(dirfd, lockfd);
        goto terminate_unregistered;
    }
    if (sequence == UINT64_MAX) {
        close_registry(dirfd, lockfd);
        set_error(error, error_size, "process ID sequence exhausted");
        goto terminate_unregistered;
    }
    candidate.state = CHRONO_SUPERVISOR_STARTING;
    int free_id_found = 0;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        if (sequence == UINT64_MAX) { close_registry(dirfd, lockfd); set_error(error, error_size, "process ID sequence exhausted"); goto terminate_unregistered; }
        ++sequence;
        if (id_from_sequence(sequence, candidate.id, sizeof(candidate.id)) != 0) {
            close_registry(dirfd, lockfd); set_error(error, error_size, "ChronoOS process ID overflow"); goto terminate_unregistered;
        }
        char name[CHRONO_SUPERVISOR_ID_MAX + 8]; (void)record_filename(candidate.id, name, sizeof(name));
        struct stat existing;
        if (fstatat(dirfd, name, &existing, AT_SYMLINK_NOFOLLOW) == 0) continue;
        if (errno != ENOENT) { close_registry(dirfd, lockfd); set_errno_error(error, error_size, "cannot check ChronoOS process ID", errno); goto terminate_unregistered; }
        free_id_found = 1;
        break;
    }
    if (!free_id_found) { close_registry(dirfd, lockfd); set_error(error, error_size, "could not allocate a unique ChronoOS process ID"); goto terminate_unregistered; }
    if (sequence_store(dirfd, sequence, error, error_size) != 0 ||
        store_record_at(dirfd, &candidate, 1, error, error_size) != 0) {
        close_registry(dirfd, lockfd); goto terminate_unregistered;
    }
    int reaped = 0;
    if (reap_if_our_child(&candidate, &reaped) != 0) {
        char primary[256], cleanup_error[256] = {0};
        (void)snprintf(primary, sizeof(primary), "cannot collect protected process state after publishing %s", candidate.id);
        if (request_stop_locked(dirfd, &candidate, cleanup_error, sizeof(cleanup_error)) != 0)
            (void)snprintf(cleanup_error, sizeof(cleanup_error), "cannot persist STOPPING state: %.180s",
                           error != NULL && error_size ? error : "registry update failed");
        close_registry(dirfd, lockfd);
        char bounded_error[256] = {0};
        if (cleanup_child_bounded(child, &candidate, NULL, bounded_error, sizeof(bounded_error)) != 0) {
            if (cleanup_error[0] != '\0') {
                char combined[256];
                (void)snprintf(combined, sizeof(combined), "%s; %.160s", cleanup_error, bounded_error);
                (void)snprintf(cleanup_error, sizeof(cleanup_error), "%s", combined);
            } else (void)snprintf(cleanup_error, sizeof(cleanup_error), "%s", bounded_error);
        }
        combine_primary_and_cleanup_error(error, error_size, primary, cleanup_error);
        return -1;
    }
    if (!reaped) {
        chrono_process_info verified;
        char inspect_error[256] = {0};
        int inspect_result = chrono_process_inspect(child, &verified, inspect_error, sizeof(inspect_error));
        if (inspect_result == 0 && verified.state != 'Z' &&
            record_matches_process(&candidate, &verified)) {
            candidate.state = CHRONO_SUPERVISOR_RUNNING;
        } else if (inspect_result == 0 && verified.state == 'Z') {
            if (reap_if_our_child(&candidate, &reaped) != 0) {
                set_errno_error(error, error_size, "cannot collect exited protected child", errno);
                candidate.state = CHRONO_SUPERVISOR_FAILED;
            } else if (!reaped) {
                candidate.state = CHRONO_SUPERVISOR_FAILED;
                candidate.exit_status_known = 0;
            }
        } else if (inspect_result == 0 && verified.start_time != candidate.start_time) {
            candidate.state = CHRONO_SUPERVISOR_FAILED;
            candidate.exit_status_known = 0;
        } else if (inspect_result == 0) {
            /* Same PID/start time but changed owner or executable identity.
             * Retain evidence and never signal the changed identity. */
            candidate.state = CHRONO_SUPERVISOR_FAILED;
            candidate.exit_status_known = 0;
            (void)snprintf(error, error_size,
                           "live child PID %ld STARTTIME %llu identity changed after record %s was published; no signal sent and process may remain",
                           (long)child, (unsigned long long)candidate.start_time, candidate.id);
        } else {
            int present;
            if (proc_entry_present(child, &present) == 0 && !present) {
                candidate.state = CHRONO_SUPERVISOR_FAILED;
                candidate.exit_status_known = 0;
            } else {
                char primary[256], cleanup_error[256] = {0};
                (void)snprintf(primary, sizeof(primary),
                               "could not verify RUNNING state for registration %s: %.170s",
                               candidate.id, inspect_error[0] ? inspect_error : "inspection failed");
                if (request_stop_locked(dirfd, &candidate, cleanup_error, sizeof(cleanup_error)) != 0) {
                    char request_error[256];
                    (void)snprintf(request_error, sizeof(request_error), "%s", cleanup_error);
                    (void)snprintf(cleanup_error, sizeof(cleanup_error),
                                   "could not persist stop request: %.180s", request_error);
                }
                close_registry(dirfd, lockfd);
                char bounded_error[256] = {0};
                if (cleanup_child_bounded(child, &candidate, NULL,
                                          bounded_error, sizeof(bounded_error)) != 0) {
                    if (cleanup_error[0] != '\0') {
                        char combined[256];
                        (void)snprintf(combined, sizeof(combined), "%.100s; %.140s",
                                       cleanup_error, bounded_error);
                        (void)snprintf(cleanup_error, sizeof(cleanup_error), "%s", combined);
                    } else (void)snprintf(cleanup_error, sizeof(cleanup_error), "%s", bounded_error);
                } else if (cleanup_error[0] == '\0')
                    (void)snprintf(cleanup_error, sizeof(cleanup_error), "verified cleanup completed");
                combine_primary_and_cleanup_error(error, error_size, primary, cleanup_error);
                return -1;
            }
        }
    }
    if (update_time(&candidate) != 0 || store_record_at(dirfd, &candidate, 0, error, error_size) != 0) {
        char primary[256], cleanup_error[256] = {0};
        (void)snprintf(primary, sizeof(primary), "cannot publish final lifecycle state for registration %s: %.170s",
                       candidate.id, error != NULL && error_size ? error : "registry update failed");
        if (candidate.state == CHRONO_SUPERVISOR_RUNNING || candidate.state == CHRONO_SUPERVISOR_STARTING)
            (void)request_stop_locked(dirfd, &candidate, cleanup_error, sizeof(cleanup_error));
        close_registry(dirfd, lockfd);
        if (candidate.intentional_stop) {
            char bounded_error[256] = {0};
            if (cleanup_child_bounded(child, &candidate, NULL, bounded_error, sizeof(bounded_error)) != 0)
                (void)snprintf(cleanup_error, sizeof(cleanup_error), "%s", bounded_error);
            else if (cleanup_error[0] == '\0')
                (void)snprintf(cleanup_error, sizeof(cleanup_error), "verified cleanup completed");
        }
        combine_primary_and_cleanup_error(error, error_size, primary, cleanup_error);
        return -1;
    }
    close_registry(dirfd, lockfd);
    *process = candidate;
    if (candidate.state == CHRONO_SUPERVISOR_FAILED) {
        (void)snprintf(error, error_size,
                       "workload did not reach a verifiable RUNNING state; record %s is retained as FAILED",
                       candidate.id);
        return -1;
    }
    if (candidate.state != CHRONO_SUPERVISOR_RUNNING &&
        candidate.state != CHRONO_SUPERVISOR_EXITED) {
        (void)snprintf(error, error_size,
                       "registration %s has unresolved lifecycle state %s",
                       candidate.id, chrono_supervisor_state_name(candidate.state));
        return -1;
    }
    return 0;

terminate_unregistered:
    {
        char primary[256] = {0};
        char cleanup_error[256] = {0};
        if (error != NULL && error_size != 0 && error[0] != '\0')
            (void)snprintf(primary, sizeof(primary), "%s", error);
        else
            (void)snprintf(primary, sizeof(primary), "registration failed");
        if (cleanup_child_bounded(child, complete_identity_or_null(&candidate), NULL,
                                  cleanup_error, sizeof(cleanup_error)) != 0)
            combine_primary_and_cleanup_error(error, error_size, primary, cleanup_error);
        else
            set_error(error, error_size, primary);
    }
    return -1;
}

int chrono_supervisor_protect_with_interval(const char *registry_directory,
                                            char *const argv[],
                                            uint32_t checkpoint_interval_seconds,
                                            chrono_supervised_process *process,
                                            char *error, size_t error_size)
{
    return supervisor_protect_internal(registry_directory, argv, 0,
                                       checkpoint_interval_seconds, process,
                                       error, error_size);
}

int chrono_supervisor_protect(const char *registry_directory, char *const argv[],
                              chrono_supervised_process *process,
                              char *error, size_t error_size)
{
    return chrono_supervisor_protect_with_interval(registry_directory, argv,
                                                   CHRONO_CHECKPOINT_INTERVAL_DEFAULT,
                                                   process, error, error_size);
}

int chrono_supervisor_protect_counter(const char *registry_directory,
                                      uint32_t checkpoint_interval_seconds,
                                      chrono_supervised_process *process,
                                      char *error, size_t error_size)
{
    return supervisor_protect_internal(registry_directory, NULL, 1,
                                       checkpoint_interval_seconds, process,
                                       error, error_size);
}

static int refresh_record_locked(int dirfd, chrono_supervised_process *p,
                                 char *error, size_t error_size)
{
    return refresh_locked(dirfd, p, error, error_size);
}

int chrono_supervisor_get(const char *registry_directory, const char *id,
                          chrono_supervised_process *process,
                          char *error, size_t error_size)
{
    clear_error(error, error_size);
    if (!chrono_supervisor_id_valid(id) || process == NULL) { set_error(error, error_size, "invalid ChronoOS process ID"); return -1; }
    int dirfd, lockfd;
    if (open_registry(registry_directory, &dirfd, &lockfd, error, error_size) != 0) return -1;
    int result = load_record_at(dirfd, id, process, error, error_size);
    if (result == 0) result = refresh_record_locked(dirfd, process, error, error_size);
    close_registry(dirfd, lockfd); return result < 0 ? -1 : 0;
}

static int id_filename_parse(const char *name, char *id, size_t size)
{
    size_t length = strlen(name);
    if (length < 13 || strcmp(name + length - 4, ".rec") != 0 || length - 4 >= size) return -1;
    memcpy(id, name, length - 4); id[length - 4] = '\0';
    return chrono_supervisor_id_valid(id) ? 0 : -1;
}

int chrono_supervisor_list(const char *registry_directory,
                           chrono_supervisor_list_fn callback, void *context,
                           char *error, size_t error_size)
{
    clear_error(error, error_size);
    if (callback == NULL) { set_error(error, error_size, "missing process listing callback"); return -1; }
    int dirfd, lockfd;
    if (open_registry(registry_directory, &dirfd, &lockfd, error, error_size) != 0) return -1;
    int duplicate = dup(dirfd); DIR *directory = duplicate < 0 ? NULL : fdopendir(duplicate);
    if (directory == NULL) {
        if (duplicate >= 0) close(duplicate);
        close_registry(dirfd, lockfd);
        set_error(error, error_size, "cannot list private process registry"); return -1;
    }
    int result = 0; struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                set_errno_error(error, error_size, "cannot read process registry directory", errno);
                result = -1;
            }
            break;
        }
        if (entry->d_name[0] == '.') continue;
        char id[CHRONO_SUPERVISOR_ID_MAX + 1];
        if (id_filename_parse(entry->d_name, id, sizeof(id)) != 0) {
            set_error(error, error_size, "unexpected filename in process registry"); result = -1; break;
        }
        chrono_supervised_process process;
        if (load_record_at(dirfd, id, &process, error, error_size) != 0 ||
            refresh_record_locked(dirfd, &process, error, error_size) != 0) { result = -1; break; }
        if (callback(&process, context) != 0) {
            if (error == NULL || error_size == 0 || error[0] == '\0')
                set_error(error, error_size, "process listing callback failed");
            result = -1;
            break;
        }
    }
    if (closedir(directory) != 0 && result == 0) { set_errno_error(error, error_size, "cannot close process registry listing", errno); result = -1; }
    close_registry(dirfd, lockfd);
    return result;
}

int chrono_supervisor_stop(const char *registry_directory, const char *id,
                           chrono_supervised_process *process,
                           char *error, size_t error_size)
{
    clear_error(error, error_size);
    if (!chrono_supervisor_id_valid(id) || process == NULL) { set_error(error, error_size, "invalid ChronoOS process ID"); return -1; }
    int dirfd, lockfd;
    if (open_registry(registry_directory, &dirfd, &lockfd, error, error_size) != 0) return -1;
    if (load_record_at(dirfd, id, process, error, error_size) != 0) { close_registry(dirfd, lockfd); return -1; }
    if (process->state == CHRONO_SUPERVISOR_EXITED) { close_registry(dirfd, lockfd); return 0; }
    if (process->state == CHRONO_SUPERVISOR_FAILED) { close_registry(dirfd, lockfd); set_error(error, error_size, "process is FAILED; refusing to signal it"); return -1; }
    chrono_process_info current; memset(&current, 0, sizeof(current));
    char inspect_error[256] = {0};
    int inspect_result = chrono_process_inspect(process->pid, &current,
                                                 inspect_error, sizeof(inspect_error));
    if (inspect_result != 0) {
        int present;
        if (proc_entry_present(process->pid, &present) != 0) {
            set_errno_error(error, error_size, "cannot establish process presence before stop", errno);
            close_registry(dirfd, lockfd);
            return -1;
        }
        if (present) {
            set_error(error, error_size, inspect_error[0] ? inspect_error :
                      "process inspection failed; lifecycle state preserved and no signal sent");
            close_registry(dirfd, lockfd);
            return -1;
        }
        int was_intentional = process->intentional_stop;
        if (persist_terminal_state(dirfd, process, error, error_size) != 0) {
            close_registry(dirfd, lockfd);
            return -1;
        }
        close_registry(dirfd, lockfd);
        if (!was_intentional) {
            set_error(error, error_size, "process is confirmed absent; recorded as FAILED because no stop request had been made");
            return -1;
        }
        return 0;
    }
    if (current.start_time != process->start_time) {
        int had_stop_request = process->intentional_stop;
        if (persist_terminal_state(dirfd, process, error, error_size) != 0) {
            close_registry(dirfd, lockfd);
            return -1;
        }
        close_registry(dirfd, lockfd);
        if (!had_stop_request) {
            set_error(error, error_size, "process start time changed; no signal was sent and the record is FAILED");
            return -1;
        }
        return 0;
    }
    if (current.state == 'Z') {
        int reaped = 0;
        if (reap_if_our_child(process, &reaped) != 0) {
            set_errno_error(error, error_size, "cannot collect protected child status", errno);
            close_registry(dirfd, lockfd);
            return -1;
        }
        if (!reaped && persist_terminal_state(dirfd, process, error, error_size) != 0) {
            close_registry(dirfd, lockfd);
            return -1;
        }
        if (reaped && store_record_at(dirfd, process, 0, error, error_size) != 0) {
            close_registry(dirfd, lockfd);
            return -1;
        }
        close_registry(dirfd, lockfd);
        if (process->state == CHRONO_SUPERVISOR_FAILED) {
            set_error(error, error_size, "process had already exited unexpectedly; no signal was sent");
            return -1;
        }
        return 0;
    }
    if (!record_matches_process(process, &current)) {
        if (process->state != CHRONO_SUPERVISOR_STOPPING) {
            process->state = CHRONO_SUPERVISOR_FAILED;
            if (update_time(process) == 0) (void)store_record_at(dirfd, process, 0, NULL, 0);
        }
        close_registry(dirfd, lockfd);
        set_error(error, error_size, process->intentional_stop ?
                  "live process identity mismatch while STOPPING; state preserved and no signal sent" :
                  "live process identity mismatch; no signal sent");
        return -1;
    }
    if (process->state != CHRONO_SUPERVISOR_STOPPING &&
        request_stop_locked(dirfd, process, error, error_size) != 0) {
        close_registry(dirfd, lockfd);
        return -1;
    }
    if (chrono_process_stop_verified(process->pid, process->start_time, process->executable_device,
                                     process->executable_inode, process->command, error, error_size) != 0) {
        close_registry(dirfd, lockfd); return -1;
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        int reaped = 0;
        if (reap_if_our_child(process, &reaped) != 0) { close_registry(dirfd, lockfd); set_errno_error(error, error_size, "cannot reap protected child", errno); return -1; }
        if (reaped) {
            if (store_record_at(dirfd, process, 0, error, error_size) != 0) { close_registry(dirfd, lockfd); return -1; }
            close_registry(dirfd, lockfd); return 0;
        }
        inspect_error[0] = '\0';
        if (chrono_process_inspect(process->pid, &current, inspect_error, sizeof(inspect_error)) == 0) {
            if (current.start_time != process->start_time) {
                if (persist_terminal_state(dirfd, process, error, error_size) != 0) {
                    close_registry(dirfd, lockfd); return -1;
                }
                close_registry(dirfd, lockfd); return 0;
            }
            if (current.state == 'Z') {
                if (reap_if_our_child(process, &reaped) != 0) {
                    set_errno_error(error, error_size, "cannot collect terminated protected child", errno);
                    close_registry(dirfd, lockfd); return -1;
                }
                if (reaped) {
                    if (store_record_at(dirfd, process, 0, error, error_size) != 0) {
                        close_registry(dirfd, lockfd); return -1;
                    }
                } else if (persist_terminal_state(dirfd, process, error, error_size) != 0) {
                    close_registry(dirfd, lockfd); return -1;
                }
                close_registry(dirfd, lockfd); return 0;
            }
            if (!record_matches_process(process, &current)) {
                /* The exact pid/start-time still identifies the same task,
                 * but executable details may disappear while it exits or may
                 * have changed via exec. Do not signal it again; keep waiting
                 * for confirmed disappearance within this bounded window. */
            }
        } else {
            int present;
            if (proc_entry_present(process->pid, &present) != 0) {
                set_errno_error(error, error_size, "cannot establish process presence while STOPPING", errno);
                close_registry(dirfd, lockfd); return -1;
            }
            if (!present) {
                if (persist_terminal_state(dirfd, process, error, error_size) != 0) {
                    close_registry(dirfd, lockfd); return -1;
                }
                close_registry(dirfd, lockfd); return 0;
            }
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
    close_registry(dirfd, lockfd);
    set_error(error, error_size, "termination was requested but not confirmed within 5 seconds; STOPPING state preserved");
    return -1;
}
