/* Linux and Android platform layer. Uses the platform C library and pthreads. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "rf.h"

void *rf_alloc(size_t size) {
    if (!size || size > RF_MAX_FILE * 2) return 0;
    void *raw = 0;
    if (posix_memalign(&raw, 64, size + 64)) return 0;
    uint8_t *p = (uint8_t *)raw + 64;
    ((size_t *)p)[-1] = size;
    memset(p, 0, size);
    rf_account_add(size);
    return p;
}

void rf_free(void *p) {
    if (!p) return;
    rf_account_remove(((size_t *)p)[-1]);
    free((uint8_t *)p - 64);
}

int rf_read_file(const rf_path_char *path, uint8_t **data, size_t *size, rf_error *err) {
    *data = 0; *size = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return rf_fail(err, RF_ERR_IO, "cannot read input");
    struct stat st;
    int rc = RF_OK;
    uint8_t *buffer = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) rc = rf_fail(err, RF_ERR_IO, "cannot read input");
    else if (st.st_size <= 0) rc = rf_fail(err, RF_ERR_FORMAT, "invalid input: empty file");
    else if ((uint64_t)st.st_size > RF_MAX_FILE) rc = rf_fail(err, RF_ERR_LIMIT, "input exceeds size limit");
    else if (!(buffer = rf_alloc((size_t)st.st_size + 8))) rc = rf_fail(err, RF_ERR_LIMIT, "out of memory");
    else {
        size_t done = 0, want = (size_t)st.st_size;
        while (done < want) {
            ssize_t got = read(fd, buffer + done, want - done);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) { rc = rf_fail(err, RF_ERR_IO, "cannot read input"); break; }
            done += (size_t)got;
        }
    }
    close(fd);
    if (rc) { rf_free(buffer); return rc; }
    *data = buffer; *size = (size_t)st.st_size;
    return RF_OK;
}

int rf_path_blocked(const rf_path_char *path, rf_error *err) {
    struct stat st;
    if (!lstat(path, &st)) return rf_fail(err, RF_ERR_IO, "output file already exists");
    if (errno == ENOENT) return RF_OK;
    return rf_fail(err, RF_ERR_IO, "cannot access output path");
}

int rf_write_atomic(const rf_path_char *path, const uint8_t *data, size_t size, rf_error *err) {
    int rc = rf_path_blocked(path, err);
    if (rc) return rc;
    size_t n = strlen(path);
    if (!n || n >= 1024) return rf_fail(err, RF_ERR_IO, "path too long");
    char parent[1024];
    memcpy(parent, path, n + 1);
    size_t slash = n;
    while (slash && parent[slash - 1] != '/') slash--;
    const char *leaf = path + slash;
    if (!*leaf || (leaf[0] == '.' && (!leaf[1] || (leaf[1] == '.' && !leaf[2]))))
        return rf_fail(err, RF_ERR_IO, "cannot write output");
    size_t leaf_n = n - slash;
    char tmp_leaf[1033];
    memcpy(tmp_leaf, leaf, leaf_n); memcpy(tmp_leaf + leaf_n, ".partial", 9);
    if (!slash) { parent[0] = '.'; parent[1] = 0; }
    else parent[slash == 1 ? 1 : slash - 1] = 0;
    int dir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return rf_fail(err, RF_ERR_IO, "cannot write output");
    struct stat st;
    if (!fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW)) {
        close(dir);
        return rf_fail(err, RF_ERR_IO, "output file already exists");
    }
    if (errno != ENOENT) {
        close(dir);
        return rf_fail(err, RF_ERR_IO, "cannot access output path");
    }
    int fd = openat(dir, tmp_leaf, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        int exists = errno == EEXIST;
        close(dir);
        return rf_fail(err, RF_ERR_IO, exists ? "temporary output already exists" : "cannot write output");
    }
    size_t done = 0;
    int ok = 1;
    while (done < size) {
        ssize_t put = write(fd, data + done, size - done);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) { ok = 0; break; }
        done += (size_t)put;
    }
    if (ok && fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    /* Linux's no-replace rename works on filesystems without hard links.
     * The syscall avoids requiring a newer Android libc wrapper; device and
     * sandbox support remain runtime properties and require device testing.
     * Older kernels/filesystems use exclusive link publication; never rename(). */
    int published = 0, renamed = 0;
    if (ok) {
#if defined(SYS_renameat2)
        if (!syscall(SYS_renameat2, dir, tmp_leaf, dir, leaf, 1u /* RENAME_NOREPLACE */))
            published = renamed = 1;
        else if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP || errno == EPERM)
            published = linkat(dir, tmp_leaf, dir, leaf, 0) == 0;
#else
        published = linkat(dir, tmp_leaf, dir, leaf, 0) == 0;
#endif
    }
    int cleaned = renamed || unlinkat(dir, tmp_leaf, 0) == 0;
    /* Some filesystems do not implement directory fsync. File contents were
     * flushed before publication; directory persistence is best effort there. */
    int durable = published && (fsync(dir) == 0 || errno == EINVAL);
    close(dir);
    if (!published) return rf_fail(err, RF_ERR_IO, "cannot write output");
    if (!cleaned) return rf_fail(err, RF_ERR_IO, "output published but temporary cleanup failed");
    if (!durable) return rf_fail(err, RF_ERR_IO, "output published but directory flush failed");
    return RF_OK;
}

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int started;
    void (*fn)(void *);
    void *arg;
} thread_start_t;

static void *thread_entry(void *p) {
    thread_start_t *s = p;
    pthread_mutex_lock(&s->mutex);
    while (!s->started) pthread_cond_wait(&s->condition, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
    s->fn(s->arg);
    return 0;
}

void *rf_thread_create(void (*fn)(void *), void *arg) {
    thread_start_t *s = rf_alloc(sizeof *s);
    if (!s) return 0;
    s->fn = fn; s->arg = arg;
    if (pthread_mutex_init(&s->mutex, 0)) { rf_free(s); return 0; }
    if (pthread_cond_init(&s->condition, 0)) { pthread_mutex_destroy(&s->mutex); rf_free(s); return 0; }
    if (pthread_create(&s->thread, 0, thread_entry, s)) {
        pthread_cond_destroy(&s->condition); pthread_mutex_destroy(&s->mutex); rf_free(s); return 0;
    }
    return s;
}
void rf_thread_resume(void *thread) {
    thread_start_t *s = thread;
    pthread_mutex_lock(&s->mutex);
    s->started = 1;
    pthread_cond_signal(&s->condition);
    pthread_mutex_unlock(&s->mutex);
}
void rf_thread_join(void *thread) {
    thread_start_t *s = thread;
    pthread_join(s->thread, 0);
    pthread_cond_destroy(&s->condition); pthread_mutex_destroy(&s->mutex); rf_free(s);
}
void rf_yield(void) { sched_yield(); }
int rf_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 && n < 65536 ? (int)n : 1; }
uint64_t rf_ticks(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
uint64_t rf_elapsed_us(uint64_t start, uint64_t end) { return (end - start) / 1000; }
uint64_t rf_peak_working_set(void) {
    struct rusage usage;
    return getrusage(RUSAGE_SELF, &usage) ? 0 : (uint64_t)usage.ru_maxrss * 1024;
}
void rf_print(int to_stderr, const char *s) {
    size_t size = strlen(s);
    while (size) {
        ssize_t n = write(to_stderr ? STDERR_FILENO : STDOUT_FILENO, s, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        s += n; size -= (size_t)n;
    }
}
