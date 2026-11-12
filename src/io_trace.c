/*
 * io_trace.c — POSIX and MPI-IO interposition for snapshot mode
 *
 * See io_trace.h for what is counted. Implementation notes:
 *
 * Interposition: libminiprof.so defines open/read/write/... itself; since
 * it is either linked before libc or LD_PRELOADed, the application's (and
 * libgfortran's) calls resolve here, and the real functions are found
 * with dlsym(RTLD_NEXT). glibc's internal calls (e.g. fwrite's own buffer
 * flush) go to private aliases and never come back through these
 * wrappers, so a buffered fwrite is counted once, at the fwrite.
 *
 * _FORTIFY_SOURCE: Ubuntu-style toolchains enable it by default at -O2,
 * which turns some open/read/pread/fread call sites into __open_2,
 * __read_chk, ... Those are wrapped too, or fortified builds would
 * silently report no I/O.
 *
 * Signal safety: sample mode reads hardware counters with read() from its
 * SIGPROF handler. Every real-function pointer is resolved eagerly in a
 * library constructor, so the wrapper never calls dlsym() from a handler;
 * when counting is disabled the wrapper is one load and a tail call.
 *
 * Threads: counters are atomics, the fd table is a bitmap of atomic
 * words, so OpenMP threads doing I/O are counted correctly.
 *
 * Not counted: mmap'd file access, readv/writev on non-tracked fds,
 * dup()'d descriptors, asynchronous I/O (aio_*), and files opened before
 * miniprof initialised (at MPI_Init).
 */

/* This file DEFINES open/read/fread/...; with _FORTIFY_SOURCE active (the
 * default on several distros at -O2) glibc's headers would instead
 * provide always-inline versions of some of them, which clash. */
#undef _FORTIFY_SOURCE
#define _GNU_SOURCE
#include "io_trace.h"
#include "mpi_trace.h"     /* g_miniprof_tool_depth */

#include <mpi.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>

/* ── state ───────────────────────────────────────────────────────────── */

static int       g_enabled = 0;
static io_snap_t g_io;

#define FD_TRACK_MAX 65536
static uint64_t g_fd_bits[FD_TRACK_MAX / 64];

static inline void fd_mark(int fd, int on)
{
    if (fd < 0 || fd >= FD_TRACK_MAX) return;
    uint64_t bit = 1ull << (fd & 63);
    if (on) __atomic_fetch_or (&g_fd_bits[fd >> 6],  bit, __ATOMIC_RELAXED);
    else    __atomic_fetch_and(&g_fd_bits[fd >> 6], ~bit, __ATOMIC_RELAXED);
}
static inline int fd_tracked(int fd)
{
    if (fd < 0 || fd >= FD_TRACK_MAX) return 0;
    return (__atomic_load_n(&g_fd_bits[fd >> 6], __ATOMIC_RELAXED) >> (fd & 63)) & 1;
}

/* Count only while enabled and not inside an MPI/MPI-IO call. */
static inline int counting(void) { return g_enabled && !g_miniprof_tool_depth; }

static inline int path_ok(const char *p)
{
    if (!p) return 0;
    return strncmp(p, "/proc/", 6) != 0 && strncmp(p, "/sys/", 5) != 0 &&
           strncmp(p, "/dev/", 5) != 0;
}

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define ADD(field, v) __atomic_fetch_add(&(field), (uint64_t)(v), __ATOMIC_RELAXED)

static inline void count_read(io_counters_t *c, ssize_t n, uint64_t dt)
{
    ADD(c->read_calls, 1);
    if (n > 0) ADD(c->read_bytes, n);
    ADD(c->read_ns, dt);
}
static inline void count_write(io_counters_t *c, ssize_t n, uint64_t dt)
{
    ADD(c->write_calls, 1);
    if (n > 0) ADD(c->write_bytes, n);
    ADD(c->write_ns, dt);
}

void io_trace_set_enabled(int enabled)
{
    if (enabled) memset(&g_io, 0, sizeof(g_io));
    __atomic_store_n(&g_enabled, enabled, __ATOMIC_RELEASE);
}

void io_trace_get(io_snap_t *out)
{
    uint64_t *dst = (uint64_t *)out;
    const uint64_t *src = (const uint64_t *)&g_io;
    for (size_t i = 0; i < sizeof(g_io) / sizeof(uint64_t); i++)
        dst[i] = __atomic_load_n(&src[i], __ATOMIC_RELAXED);
}

/* ── real-function pointers ──────────────────────────────────────────── */

#define REAL_DECL(ret, name, ...) static ret (*real_##name)(__VA_ARGS__)
REAL_DECL(int,     open,        const char *, int, ...);
REAL_DECL(int,     open64,      const char *, int, ...);
REAL_DECL(int,     openat,      int, const char *, int, ...);
REAL_DECL(int,     openat64,    int, const char *, int, ...);
REAL_DECL(int,     creat,       const char *, mode_t);
REAL_DECL(int,     creat64,     const char *, mode_t);
REAL_DECL(int,     __open_2,    const char *, int);
REAL_DECL(int,     __open64_2,  const char *, int);
REAL_DECL(int,     __openat_2,  int, const char *, int);
REAL_DECL(int,     __openat64_2,int, const char *, int);
REAL_DECL(int,     close,       int);
REAL_DECL(ssize_t, read,        int, void *, size_t);
REAL_DECL(ssize_t, write,       int, const void *, size_t);
REAL_DECL(ssize_t, pread,       int, void *, size_t, off_t);
REAL_DECL(ssize_t, pread64,     int, void *, size_t, off64_t);
REAL_DECL(ssize_t, pwrite,      int, const void *, size_t, off_t);
REAL_DECL(ssize_t, pwrite64,    int, const void *, size_t, off64_t);
REAL_DECL(ssize_t, readv,       int, const struct iovec *, int);
REAL_DECL(ssize_t, writev,      int, const struct iovec *, int);
REAL_DECL(ssize_t, __read_chk,  int, void *, size_t, size_t);
REAL_DECL(ssize_t, __pread_chk, int, void *, size_t, off_t, size_t);
REAL_DECL(ssize_t, __pread64_chk, int, void *, size_t, off64_t, size_t);
REAL_DECL(int,     fsync,       int);
REAL_DECL(int,     fdatasync,   int);
REAL_DECL(FILE *,  fopen,       const char *, const char *);
REAL_DECL(FILE *,  fopen64,     const char *, const char *);
REAL_DECL(int,     fclose,      FILE *);
REAL_DECL(size_t,  fread,       void *, size_t, size_t, FILE *);
REAL_DECL(size_t,  fwrite,      const void *, size_t, size_t, FILE *);
REAL_DECL(size_t,  __fread_chk, void *, size_t, size_t, size_t, FILE *);
/* formatted and line stdio: text input files and logs */
REAL_DECL(char *,  fgets,       char *, int, FILE *);
REAL_DECL(char *,  __fgets_chk, char *, size_t, int, FILE *);
REAL_DECL(ssize_t, getline,     char **, size_t *, FILE *);
REAL_DECL(ssize_t, getdelim,    char **, size_t *, int, FILE *);
REAL_DECL(ssize_t, __getdelim,  char **, size_t *, int, FILE *);
REAL_DECL(int,     fputs,       const char *, FILE *);
REAL_DECL(int,     vfprintf,    FILE *, const char *, va_list);
REAL_DECL(int,     __vfprintf_chk, FILE *, int, const char *, va_list);
REAL_DECL(int,     vfscanf,     FILE *, const char *, va_list);
REAL_DECL(int,     __isoc99_vfscanf, FILE *, const char *, va_list);
REAL_DECL(int,     __isoc23_vfscanf, FILE *, const char *, va_list);

#define RESOLVE(name) \
    do { if (!real_##name) *(void **)(&real_##name) = dlsym(RTLD_NEXT, #name); } while (0)

static void resolve_all(void)
{
    RESOLVE(open);  RESOLVE(open64);  RESOLVE(openat);  RESOLVE(openat64);
    RESOLVE(creat); RESOLVE(creat64);
    RESOLVE(__open_2); RESOLVE(__open64_2); RESOLVE(__openat_2); RESOLVE(__openat64_2);
    RESOLVE(close);
    RESOLVE(read);  RESOLVE(write);
    RESOLVE(pread); RESOLVE(pread64); RESOLVE(pwrite); RESOLVE(pwrite64);
    RESOLVE(readv); RESOLVE(writev);
    RESOLVE(__read_chk); RESOLVE(__pread_chk); RESOLVE(__pread64_chk);
    RESOLVE(fsync); RESOLVE(fdatasync);
    RESOLVE(fopen); RESOLVE(fopen64); RESOLVE(fclose);
    RESOLVE(fread); RESOLVE(fwrite); RESOLVE(__fread_chk);
    RESOLVE(fgets); RESOLVE(__fgets_chk); RESOLVE(getline); RESOLVE(getdelim); RESOLVE(__getdelim);
    RESOLVE(fputs); RESOLVE(vfprintf); RESOLVE(__vfprintf_chk);
    RESOLVE(vfscanf); RESOLVE(__isoc99_vfscanf); RESOLVE(__isoc23_vfscanf);
}

__attribute__((constructor))
static void io_trace_ctor(void) { resolve_all(); }

/* Lazy fallback for calls that arrive before our constructor ran (another
 * library's constructor doing I/O). Never reached from a signal handler:
 * the only handler-context caller (sample mode's hwc read) starts long
 * after constructors have run. */
#define ENSURE(name) do { if (__builtin_expect(!real_##name, 0)) resolve_all(); } while (0)

/* ── open family ─────────────────────────────────────────────────────── */

static inline int needs_mode(int flags)
{
#ifdef O_TMPFILE
    return (flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE);
#else
    return (flags & O_CREAT);
#endif
}

static inline int after_open(const char *path, int fd, uint64_t t0)
{
    if (fd >= 0 && path_ok(path)) {
        fd_mark(fd, 1);
        if (counting()) {
            ADD(g_io.posix.open_calls, 1);
            ADD(g_io.posix.meta_ns, now_ns() - t0);
        }
    }
    return fd;
}

#define OPEN_BODY(REALCALL)                                        \
    do {                                                           \
        uint64_t t0_ = now_ns();                                   \
        int fd_ = REALCALL;                                        \
        return after_open(path, fd_, t0_);                         \
    } while (0)

int open(const char *path, int flags, ...)
{
    ENSURE(open);
    mode_t mode = 0;
    if (needs_mode(flags)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    OPEN_BODY(real_open(path, flags, mode));
}

int open64(const char *path, int flags, ...)
{
    ENSURE(open64);
    mode_t mode = 0;
    if (needs_mode(flags)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    OPEN_BODY(real_open64(path, flags, mode));
}

int openat(int dirfd, const char *path, int flags, ...)
{
    ENSURE(openat);
    mode_t mode = 0;
    if (needs_mode(flags)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    OPEN_BODY(real_openat(dirfd, path, flags, mode));
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    ENSURE(openat64);
    mode_t mode = 0;
    if (needs_mode(flags)) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    OPEN_BODY(real_openat64(dirfd, path, flags, mode));
}

int creat(const char *path, mode_t mode)   { ENSURE(creat);   OPEN_BODY(real_creat(path, mode)); }
int creat64(const char *path, mode_t mode) { ENSURE(creat64); OPEN_BODY(real_creat64(path, mode)); }

int __open_2(const char *path, int flags)    { ENSURE(__open_2);   OPEN_BODY(real___open_2(path, flags)); }
int __open64_2(const char *path, int flags)  { ENSURE(__open64_2); OPEN_BODY(real___open64_2(path, flags)); }
int __openat_2(int dirfd, const char *path, int flags)
{ ENSURE(__openat_2);   OPEN_BODY(real___openat_2(dirfd, path, flags)); }
int __openat64_2(int dirfd, const char *path, int flags)
{ ENSURE(__openat64_2); OPEN_BODY(real___openat64_2(dirfd, path, flags)); }

int close(int fd)
{
    ENSURE(close);
    if (!fd_tracked(fd)) return real_close(fd);
    fd_mark(fd, 0);
    if (!counting()) return real_close(fd);
    uint64_t t0 = now_ns();
    int rc = real_close(fd);
    ADD(g_io.posix.close_calls, 1);
    ADD(g_io.posix.meta_ns, now_ns() - t0);
    return rc;
}

/* ── read / write family ─────────────────────────────────────────────── */

#define RW_BODY(KIND, FD, REALCALL)                                \
    do {                                                           \
        if (!counting() || !fd_tracked(FD)) return REALCALL;       \
        uint64_t t0_ = now_ns();                                   \
        ssize_t ret_ = REALCALL;                                   \
        count_##KIND(&g_io.posix, ret_, now_ns() - t0_);           \
        return ret_;                                               \
    } while (0)

ssize_t read(int fd, void *buf, size_t n)
{ ENSURE(read); RW_BODY(read, fd, real_read(fd, buf, n)); }

ssize_t write(int fd, const void *buf, size_t n)
{ ENSURE(write); RW_BODY(write, fd, real_write(fd, buf, n)); }

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{ ENSURE(pread); RW_BODY(read, fd, real_pread(fd, buf, n, off)); }

ssize_t pread64(int fd, void *buf, size_t n, off64_t off)
{ ENSURE(pread64); RW_BODY(read, fd, real_pread64(fd, buf, n, off)); }

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{ ENSURE(pwrite); RW_BODY(write, fd, real_pwrite(fd, buf, n, off)); }

ssize_t pwrite64(int fd, const void *buf, size_t n, off64_t off)
{ ENSURE(pwrite64); RW_BODY(write, fd, real_pwrite64(fd, buf, n, off)); }

ssize_t readv(int fd, const struct iovec *iov, int cnt)
{ ENSURE(readv); RW_BODY(read, fd, real_readv(fd, iov, cnt)); }

ssize_t writev(int fd, const struct iovec *iov, int cnt)
{ ENSURE(writev); RW_BODY(write, fd, real_writev(fd, iov, cnt)); }

ssize_t __read_chk(int fd, void *buf, size_t n, size_t buflen)
{ ENSURE(__read_chk); RW_BODY(read, fd, real___read_chk(fd, buf, n, buflen)); }

ssize_t __pread_chk(int fd, void *buf, size_t n, off_t off, size_t buflen)
{ ENSURE(__pread_chk); RW_BODY(read, fd, real___pread_chk(fd, buf, n, off, buflen)); }

ssize_t __pread64_chk(int fd, void *buf, size_t n, off64_t off, size_t buflen)
{ ENSURE(__pread64_chk); RW_BODY(read, fd, real___pread64_chk(fd, buf, n, off, buflen)); }

#define SYNC_BODY(FD, REALCALL)                                    \
    do {                                                           \
        if (!counting() || !fd_tracked(FD)) return REALCALL;       \
        uint64_t t0_ = now_ns();                                   \
        int rc_ = REALCALL;                                        \
        ADD(g_io.posix.sync_calls, 1);                             \
        ADD(g_io.posix.meta_ns, now_ns() - t0_);                   \
        return rc_;                                                \
    } while (0)

int fsync(int fd)     { ENSURE(fsync);     SYNC_BODY(fd, real_fsync(fd)); }
int fdatasync(int fd) { ENSURE(fdatasync); SYNC_BODY(fd, real_fdatasync(fd)); }

/* ── stdio ───────────────────────────────────────────────────────────── */

static inline FILE *after_fopen(const char *path, FILE *f, uint64_t t0)
{
    if (f && path_ok(path)) {
        fd_mark(fileno(f), 1);
        if (counting()) {
            ADD(g_io.posix.open_calls, 1);
            ADD(g_io.posix.meta_ns, now_ns() - t0);
        }
    }
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    ENSURE(fopen);
    uint64_t t0 = now_ns();
    return after_fopen(path, real_fopen(path, mode), t0);
}

FILE *fopen64(const char *path, const char *mode)
{
    ENSURE(fopen64);
    uint64_t t0 = now_ns();
    return after_fopen(path, real_fopen64(path, mode), t0);
}

/* fclose flushes the stream: its time is the tail of the write cost. */
int fclose(FILE *f)
{
    ENSURE(fclose);
    int fd = fileno(f);
    if (!fd_tracked(fd)) return real_fclose(f);
    fd_mark(fd, 0);
    if (!counting()) return real_fclose(f);
    uint64_t t0 = now_ns();
    int rc = real_fclose(f);
    ADD(g_io.posix.close_calls, 1);
    ADD(g_io.posix.meta_ns, now_ns() - t0);
    return rc;
}

#define FRW_BODY(KIND, STREAM, SIZE, REALCALL)                     \
    do {                                                           \
        if (!counting() || !fd_tracked(fileno(STREAM)))            \
            return REALCALL;                                       \
        uint64_t t0_ = now_ns();                                   \
        size_t items_ = REALCALL;                                  \
        count_##KIND(&g_io.posix, (ssize_t)(items_ * (SIZE)), now_ns() - t0_); \
        return items_;                                             \
    } while (0)

size_t fread(void *ptr, size_t size, size_t n, FILE *f)
{ ENSURE(fread); FRW_BODY(read, f, size, real_fread(ptr, size, n, f)); }

size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f)
{ ENSURE(fwrite); FRW_BODY(write, f, size, real_fwrite(ptr, size, n, f)); }

size_t __fread_chk(void *ptr, size_t ptrlen, size_t size, size_t n, FILE *f)
{ ENSURE(__fread_chk); FRW_BODY(read, f, size, real___fread_chk(ptr, ptrlen, size, n, f)); }

/* Text stdio: fgets/getline reading input decks, fprintf/fputs writing
 * logs and text dumps. Bytes are those returned (characters written, line
 * length read); fscanf counts the stream position it advanced. Character
 * calls (fgetc/fputc) are not wrapped: their per-call cost would dominate. */

#define TXT_BODY(KIND, STREAM, T, REALCALL, NBYTES)                \
    do {                                                           \
        if (!counting() || !fd_tracked(fileno(STREAM)))            \
            return REALCALL;                                       \
        uint64_t t0_ = now_ns();                                   \
        T ret_ = REALCALL;                                         \
        count_##KIND(&g_io.posix, (ssize_t)(NBYTES), now_ns() - t0_); \
        return ret_;                                               \
    } while (0)

char *fgets(char *buf, int n, FILE *f)
{ ENSURE(fgets); TXT_BODY(read, f, char *, real_fgets(buf, n, f), ret_ ? strlen(ret_) : 0); }

char *__fgets_chk(char *buf, size_t size, int n, FILE *f)
{ ENSURE(__fgets_chk); TXT_BODY(read, f, char *, real___fgets_chk(buf, size, n, f), ret_ ? strlen(ret_) : 0); }

ssize_t getline(char **line, size_t *n, FILE *f)
{ ENSURE(getline); TXT_BODY(read, f, ssize_t, real_getline(line, n, f), ret_ > 0 ? ret_ : 0); }

ssize_t getdelim(char **line, size_t *n, int delim, FILE *f)
{ ENSURE(getdelim); TXT_BODY(read, f, ssize_t, real_getdelim(line, n, delim, f), ret_ > 0 ? ret_ : 0); }

ssize_t __getdelim(char **line, size_t *n, int delim, FILE *f)
{ ENSURE(__getdelim); TXT_BODY(read, f, ssize_t, real___getdelim(line, n, delim, f), ret_ > 0 ? ret_ : 0); }

int fputs(const char *str, FILE *f)
{ ENSURE(fputs); TXT_BODY(write, f, int, real_fputs(str, f), ret_ >= 0 ? strlen(str) : 0); }

int vfprintf(FILE *f, const char *fmt, va_list ap)
{ ENSURE(vfprintf); TXT_BODY(write, f, int, real_vfprintf(f, fmt, ap), ret_ > 0 ? ret_ : 0); }

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int rc = vfprintf(f, fmt, ap);
    va_end(ap);
    return rc;
}

int __vfprintf_chk(FILE *f, int flag, const char *fmt, va_list ap)
{ ENSURE(__vfprintf_chk); TXT_BODY(write, f, int, real___vfprintf_chk(f, flag, fmt, ap), ret_ > 0 ? ret_ : 0); }

int __fprintf_chk(FILE *f, int flag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int rc = __vfprintf_chk(f, flag, fmt, ap);
    va_end(ap);
    return rc;
}

#define SCAN_BODY(STREAM, REALCALL)                                \
    do {                                                           \
        if (!counting() || !fd_tracked(fileno(STREAM)))            \
            return REALCALL;                                       \
        off_t p0_ = ftello(STREAM);                                \
        uint64_t t0_ = now_ns();                                   \
        int ret_ = REALCALL;                                       \
        uint64_t dt_ = now_ns() - t0_;                             \
        off_t p1_ = ftello(STREAM);                                \
        count_read(&g_io.posix, (p0_ >= 0 && p1_ > p0_) ? (ssize_t)(p1_ - p0_) : 0, dt_); \
        return ret_;                                               \
    } while (0)

/* <stdio.h> may redirect fscanf/vfscanf to __isoc99_ or __isoc23_ names
 * with asm labels; give each wrapper its exported name explicitly. */
#define SCAN_V(FN, NAME) \
    int FN(FILE *f, const char *fmt, va_list ap) __asm__(NAME); \
    int FN(FILE *f, const char *fmt, va_list ap)
#define SCAN_F(FN, NAME, VFN) \
    int FN(FILE *f, const char *fmt, ...) __asm__(NAME); \
    int FN(FILE *f, const char *fmt, ...) \
    { va_list ap; va_start(ap, fmt); int rc = VFN(f, fmt, ap); va_end(ap); return rc; }

SCAN_V(mp_vfscanf, "vfscanf")                   { ENSURE(vfscanf); SCAN_BODY(f, real_vfscanf(f, fmt, ap)); }
SCAN_V(mp_isoc99_vfscanf, "__isoc99_vfscanf")   { ENSURE(__isoc99_vfscanf); SCAN_BODY(f, real___isoc99_vfscanf(f, fmt, ap)); }
SCAN_V(mp_isoc23_vfscanf, "__isoc23_vfscanf")   { ENSURE(__isoc23_vfscanf); SCAN_BODY(f, real___isoc23_vfscanf(f, fmt, ap)); }
SCAN_F(mp_fscanf, "fscanf", mp_vfscanf)
SCAN_F(mp_isoc99_fscanf, "__isoc99_fscanf", mp_isoc99_vfscanf)
SCAN_F(mp_isoc23_fscanf, "__isoc23_fscanf", mp_isoc23_vfscanf)

/* ── MPI-IO ──────────────────────────────────────────────────────────── *
 * Recorded as MPI-IO (not MPI time, not POSIX). Raises the tool depth so
 * the MPI-IO back end's own POSIX calls and internal collectives are not
 * counted a second time. Bytes = count × datatype size requested. */

static inline long io_type_size(MPI_Datatype dt)
{
    if (dt == MPI_DATATYPE_NULL) return 0;
    int s = 0;
    if (PMPI_Type_size(dt, &s) != MPI_SUCCESS || s < 0) return 0;
    return s;
}

#define MPIIO_WRAP(KIND, CALL, BYTES)                              \
    do {                                                           \
        if (!counting()) return CALL;                              \
        g_miniprof_tool_depth++;                                   \
        uint64_t t0_ = now_ns();                                   \
        int rc_ = CALL;                                            \
        uint64_t elapsed_ = now_ns() - t0_;                        \
        g_miniprof_tool_depth--;                                   \
        count_##KIND(&g_io.mpiio, (ssize_t)(BYTES), elapsed_);     \
        return rc_;                                                \
    } while (0)

#define MPIIO_META(COUNTER, CALL)                                  \
    do {                                                           \
        if (!counting()) return CALL;                              \
        g_miniprof_tool_depth++;                                   \
        uint64_t t0_ = now_ns();                                   \
        int rc_ = CALL;                                            \
        uint64_t elapsed_ = now_ns() - t0_;                        \
        g_miniprof_tool_depth--;                                   \
        ADD(g_io.mpiio.COUNTER, 1);                                \
        ADD(g_io.mpiio.meta_ns, elapsed_);                         \
        return rc_;                                                \
    } while (0)

/* Collective calls that move no data but can take real time (PnetCDF's
 * ncmpi_enddef / ncmpi_put_vara_*_all go through set_size and set_view):
 * MPI-IO metadata time, not compute. */
#define MPIIO_META_T(CALL)                                         \
    do {                                                           \
        if (!counting()) return CALL;                              \
        g_miniprof_tool_depth++;                                   \
        uint64_t t0_ = now_ns();                                   \
        int rc_ = CALL;                                            \
        uint64_t elapsed_ = now_ns() - t0_;                        \
        g_miniprof_tool_depth--;                                   \
        ADD(g_io.mpiio.meta_ns, elapsed_);                         \
        return rc_;                                                \
    } while (0)

int MPI_File_set_view(MPI_File fh, MPI_Offset disp, MPI_Datatype etype, MPI_Datatype filetype,
                      const char *datarep, MPI_Info info)
{ MPIIO_META_T(PMPI_File_set_view(fh, disp, etype, filetype, datarep, info)); }

int MPI_File_set_size(MPI_File fh, MPI_Offset size)
{ MPIIO_META_T(PMPI_File_set_size(fh, size)); }

int MPI_File_preallocate(MPI_File fh, MPI_Offset size)
{ MPIIO_META_T(PMPI_File_preallocate(fh, size)); }

int MPI_File_set_info(MPI_File fh, MPI_Info info)
{ MPIIO_META_T(PMPI_File_set_info(fh, info)); }

/* Non-blocking MPI-IO: bytes counted when issued, with the time of the
 * issue call; the wait for completion is MPI wait time (MPI_Wait*). */
int MPI_File_iwrite(MPI_File fh, const void *buf, int count, MPI_Datatype dt, MPI_Request *rq)
{ MPIIO_WRAP(write, PMPI_File_iwrite(fh, buf, count, dt, rq), (long)count * io_type_size(dt)); }

int MPI_File_iwrite_at(MPI_File fh, MPI_Offset off, const void *buf, int count,
                       MPI_Datatype dt, MPI_Request *rq)
{ MPIIO_WRAP(write, PMPI_File_iwrite_at(fh, off, buf, count, dt, rq), (long)count * io_type_size(dt)); }

int MPI_File_iread(MPI_File fh, void *buf, int count, MPI_Datatype dt, MPI_Request *rq)
{ MPIIO_WRAP(read, PMPI_File_iread(fh, buf, count, dt, rq), (long)count * io_type_size(dt)); }

int MPI_File_iread_at(MPI_File fh, MPI_Offset off, void *buf, int count,
                      MPI_Datatype dt, MPI_Request *rq)
{ MPIIO_WRAP(read, PMPI_File_iread_at(fh, off, buf, count, dt, rq), (long)count * io_type_size(dt)); }

#if MPI_VERSION > 3 || (MPI_VERSION == 3 && MPI_SUBVERSION >= 1)
int MPI_File_iwrite_all(MPI_File fh, const void *buf, int count, MPI_Datatype dt, MPI_Request *rq)
{ MPIIO_WRAP(write, PMPI_File_iwrite_all(fh, buf, count, dt, rq), (long)count * io_type_size(dt)); }

int MPI_File_iwrite_at_all(MPI_File fh, MPI_Offset off, const void *buf, int count,
                           MPI_Datatype dt, MPI_Request *rq)
{ MPIIO_WRAP(write, PMPI_File_iwrite_at_all(fh, off, buf, count, dt, rq), (long)count * io_type_size(dt)); }

int MPI_File_iread_all(MPI_File fh, void *buf, int count, MPI_Datatype dt, MPI_Request *rq)
{ MPIIO_WRAP(read, PMPI_File_iread_all(fh, buf, count, dt, rq), (long)count * io_type_size(dt)); }

int MPI_File_iread_at_all(MPI_File fh, MPI_Offset off, void *buf, int count,
                          MPI_Datatype dt, MPI_Request *rq)
{ MPIIO_WRAP(read, PMPI_File_iread_at_all(fh, off, buf, count, dt, rq), (long)count * io_type_size(dt)); }
#endif

int MPI_File_open(MPI_Comm comm, const char *filename, int amode,
                  MPI_Info info, MPI_File *fh)
{ MPIIO_META(open_calls, PMPI_File_open(comm, filename, amode, info, fh)); }

int MPI_File_close(MPI_File *fh)
{ MPIIO_META(close_calls, PMPI_File_close(fh)); }

int MPI_File_sync(MPI_File fh)
{ MPIIO_META(sync_calls, PMPI_File_sync(fh)); }

int MPI_File_read(MPI_File fh, void *buf, int count, MPI_Datatype dt, MPI_Status *st)
{ MPIIO_WRAP(read, PMPI_File_read(fh, buf, count, dt, st), (long)count * io_type_size(dt)); }

int MPI_File_read_all(MPI_File fh, void *buf, int count, MPI_Datatype dt, MPI_Status *st)
{ MPIIO_WRAP(read, PMPI_File_read_all(fh, buf, count, dt, st), (long)count * io_type_size(dt)); }

int MPI_File_read_at(MPI_File fh, MPI_Offset off, void *buf, int count,
                     MPI_Datatype dt, MPI_Status *st)
{ MPIIO_WRAP(read, PMPI_File_read_at(fh, off, buf, count, dt, st), (long)count * io_type_size(dt)); }

int MPI_File_read_at_all(MPI_File fh, MPI_Offset off, void *buf, int count,
                         MPI_Datatype dt, MPI_Status *st)
{ MPIIO_WRAP(read, PMPI_File_read_at_all(fh, off, buf, count, dt, st), (long)count * io_type_size(dt)); }

int MPI_File_write(MPI_File fh, const void *buf, int count, MPI_Datatype dt, MPI_Status *st)
{ MPIIO_WRAP(write, PMPI_File_write(fh, buf, count, dt, st), (long)count * io_type_size(dt)); }

int MPI_File_write_all(MPI_File fh, const void *buf, int count, MPI_Datatype dt, MPI_Status *st)
{ MPIIO_WRAP(write, PMPI_File_write_all(fh, buf, count, dt, st), (long)count * io_type_size(dt)); }

int MPI_File_write_at(MPI_File fh, MPI_Offset off, const void *buf, int count,
                      MPI_Datatype dt, MPI_Status *st)
{ MPIIO_WRAP(write, PMPI_File_write_at(fh, off, buf, count, dt, st), (long)count * io_type_size(dt)); }

int MPI_File_write_at_all(MPI_File fh, MPI_Offset off, const void *buf, int count,
                          MPI_Datatype dt, MPI_Status *st)
{ MPIIO_WRAP(write, PMPI_File_write_at_all(fh, off, buf, count, dt, st), (long)count * io_type_size(dt)); }

/* Fortran MPI-IO: forward to the library's Fortran binding (sentinels and
 * the hidden filename length are the library's business). gfortran passes
 * the hidden CHARACTER length as size_t. */
typedef MPI_Fint F;
extern void pmpi_file_open_(F*, char*, F*, F*, F*, F*, size_t);
extern void pmpi_file_close_(F*, F*);
extern void pmpi_file_sync_(F*, F*);
extern void pmpi_file_read_(F*, void*, F*, F*, F*, F*);
extern void pmpi_file_read_all_(F*, void*, F*, F*, F*, F*);
extern void pmpi_file_read_at_(F*, MPI_Offset*, void*, F*, F*, F*, F*);
extern void pmpi_file_read_at_all_(F*, MPI_Offset*, void*, F*, F*, F*, F*);
extern void pmpi_file_write_(F*, const void*, F*, F*, F*, F*);
extern void pmpi_file_write_all_(F*, const void*, F*, F*, F*, F*);
extern void pmpi_file_write_at_(F*, MPI_Offset*, const void*, F*, F*, F*, F*);
extern void pmpi_file_write_at_all_(F*, MPI_Offset*, const void*, F*, F*, F*, F*);

#define FIO_WRAP(KIND, CALL, COUNT, DT)                            \
    do {                                                           \
        if (!counting()) { CALL; return; }                         \
        g_miniprof_tool_depth++;                                   \
        uint64_t t0_ = now_ns();                                   \
        CALL;                                                      \
        uint64_t elapsed_ = now_ns() - t0_;                        \
        g_miniprof_tool_depth--;                                   \
        count_##KIND(&g_io.mpiio,                                  \
            (ssize_t)((long)*(COUNT) * io_type_size(MPI_Type_f2c(*(DT)))), elapsed_); \
    } while (0)

#define FIO_META(COUNTER, CALL)                                    \
    do {                                                           \
        if (!counting()) { CALL; return; }                         \
        g_miniprof_tool_depth++;                                   \
        uint64_t t0_ = now_ns();                                   \
        CALL;                                                      \
        uint64_t elapsed_ = now_ns() - t0_;                        \
        g_miniprof_tool_depth--;                                   \
        ADD(g_io.mpiio.COUNTER, 1);                                \
        ADD(g_io.mpiio.meta_ns, elapsed_);                         \
    } while (0)

void mpi_file_open_(F *comm, char *name, F *amode, F *info, F *fh, F *ierr, size_t len)
{ FIO_META(open_calls, pmpi_file_open_(comm, name, amode, info, fh, ierr, len)); }

void mpi_file_close_(F *fh, F *ierr)
{ FIO_META(close_calls, pmpi_file_close_(fh, ierr)); }

void mpi_file_sync_(F *fh, F *ierr)
{ FIO_META(sync_calls, pmpi_file_sync_(fh, ierr)); }

void mpi_file_read_(F *fh, void *buf, F *count, F *dt, F *st, F *ierr)
{ FIO_WRAP(read, pmpi_file_read_(fh, buf, count, dt, st, ierr), count, dt); }

void mpi_file_read_all_(F *fh, void *buf, F *count, F *dt, F *st, F *ierr)
{ FIO_WRAP(read, pmpi_file_read_all_(fh, buf, count, dt, st, ierr), count, dt); }

void mpi_file_read_at_(F *fh, MPI_Offset *off, void *buf, F *count, F *dt, F *st, F *ierr)
{ FIO_WRAP(read, pmpi_file_read_at_(fh, off, buf, count, dt, st, ierr), count, dt); }

void mpi_file_read_at_all_(F *fh, MPI_Offset *off, void *buf, F *count, F *dt, F *st, F *ierr)
{ FIO_WRAP(read, pmpi_file_read_at_all_(fh, off, buf, count, dt, st, ierr), count, dt); }

void mpi_file_write_(F *fh, const void *buf, F *count, F *dt, F *st, F *ierr)
{ FIO_WRAP(write, pmpi_file_write_(fh, buf, count, dt, st, ierr), count, dt); }

void mpi_file_write_all_(F *fh, const void *buf, F *count, F *dt, F *st, F *ierr)
{ FIO_WRAP(write, pmpi_file_write_all_(fh, buf, count, dt, st, ierr), count, dt); }

void mpi_file_write_at_(F *fh, MPI_Offset *off, const void *buf, F *count, F *dt, F *st, F *ierr)
{ FIO_WRAP(write, pmpi_file_write_at_(fh, off, buf, count, dt, st, ierr), count, dt); }

void mpi_file_write_at_all_(F *fh, MPI_Offset *off, const void *buf, F *count, F *dt, F *st, F *ierr)
{ FIO_WRAP(write, pmpi_file_write_at_all_(fh, off, buf, count, dt, st, ierr), count, dt); }
