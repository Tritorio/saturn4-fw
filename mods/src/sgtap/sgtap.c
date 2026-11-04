/*
 * libsgtap.so - LD_PRELOAD tap for the chitu app's serial I/O (step 8.3a) and
 * strain-gauge datalogger (step 8.3c, sglog.c: always on, CSV per print session),
 * live feed for the dashboard (sgweb.c), vat heating control (step 8.9, sgheat.c) and the
 * dynamic wait after retract with heating in the wait (steps 8.11/8.12, sgwait.c).
 *
 * Passive: every wrapper calls the real libc function first and returns its result
 * unchanged. For fds opened on a matching tty (default /dev/ttyS1..3) the bytes moved
 * by read/write are copied into a RAM ring; a background thread drains the ring
 * to a capture file. Nothing is captured unless the ENABLE file exists.
 *
 *   <dir>/ENABLE              create to capture, delete to stop (checked every second)
 *   <dir>/cap_<bootid>_<mono_s>.bin  one file per enable period, records below
 *   dir = /media/mmcblk0p2/sgcap (env SGTAP_DIR overrides, for tests)
 *
 * Only active inside /customer/resources/chitu (or with env SGTAP_FORCE=1, tests).
 * chitu daemonizes (fork + setsid) right after start, and threads do not survive a
 * fork: a fork handler resets the state in the child and the writer thread is started
 * on demand in the process that actually does the tty I/O.
 * chitu also uses vfork: a vfork child shares our memory but no fork handler runs,
 * and it closes the inherited fds before exec. So all state changes happen only in the
 * owner process (set at load and in the fork handler); elsewhere pure pass-through.
 * LD_PRELOAD is removed from the environment so children (system, popen) do not
 * load the tap. Env SGTAP_MATCH overrides the tty path prefix (tests: /dev/pts/).
 *
 * Record: struct rec (20 bytes, little endian) + len bytes payload
 *   type 0 READ, 1 WRITE        payload = bytes
 *   type 2 OPEN                 payload = path (fd in field fd)
 *   type 3 CLOSE
 *   type 4 TCSETATTR            payload = struct termios (60 bytes) + u32 out speed
 *   type 5 START                payload = "sgtap v1 wall=<unix s> mono_us=<..>"
 *   (each file starts with START + an OPEN and the last TCSETATTR of every tty already
 *    open, since chitu opens its ports at startup, long before a capture begins)
 *   type 6 DROPPED              payload = u32 bytes lost (ring full)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAGIC      0x31544753u /* "SGT1" */
#define MAX_FD     1024
#define RING_SIZE  (1u << 20)  /* 1 MiB */
#define FILE_MAX   (100u << 20) /* stop writing a capture file after 100 MiB */

struct __attribute__((packed)) rec {
    uint32_t magic;
    uint64_t ts_us;            /* CLOCK_MONOTONIC, microseconds */
    uint32_t tid;
    uint8_t  type;
    int8_t   fd_lo;            /* low 8 bits of fd, for humans */
    uint16_t len;
};

static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);
static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_close)(int);
static FILE *(*real_fopen)(const char *, const char *);
static FILE *(*real_fopen64)(const char *, const char *);
static int (*real_tcsetattr)(int, int, const struct termios *);

static volatile int active;           /* tap enabled for this process */
static volatile int capturing;        /* ENABLE file present */
static unsigned char tracked[MAX_FD]; /* 1 = fd is a matching tty */
static char match[64] = "/dev/ttyS";
static char dir[128] = "/media/mmcblk0p2/sgcap";

/* state of the tracked ttys, replayed at the start of every capture file */
#define NSLOT 8
static struct slot {
    int fd;                           /* -1 = free */
    char path[32];
    int have_tios;
    unsigned char tios[sizeof(struct termios) + 4];
} slots[NSLOT] = {[0 ... NSLOT - 1] = {.fd = -1}};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile pid_t thread_pid;     /* process that owns the writer thread */
static volatile pid_t owner_pid;      /* process allowed to change our state */
static int out_fd = -1;
static uint32_t out_bytes;
static void ensure_thread(void);
/* in glibc 2.28 pthread_atfork is a libc_nonshared.a wrapper around this */
extern int __register_atfork(void (*)(void), void (*)(void), void (*)(void), void *);
static unsigned char ring[RING_SIZE];
static uint32_t head, tail;           /* head = write pos, tail = read pos */
static uint32_t dropped;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}

static void resolve(void)
{
    if (!real_read) {
        real_read = dlsym(RTLD_NEXT, "read");
        real_write = dlsym(RTLD_NEXT, "write");
        real_open = dlsym(RTLD_NEXT, "open");
        real_open64 = dlsym(RTLD_NEXT, "open64");
        real_close = dlsym(RTLD_NEXT, "close");
        real_fopen = dlsym(RTLD_NEXT, "fopen");
        real_fopen64 = dlsym(RTLD_NEXT, "fopen64");
        real_tcsetattr = dlsym(RTLD_NEXT, "tcsetattr");
    }
}

static void ring_put(const void *p, uint32_t n)
{
    const unsigned char *s = p;
    uint32_t pos = head % RING_SIZE, first = RING_SIZE - pos;
    if (first > n)
        first = n;
    memcpy(ring + pos, s, first);
    memcpy(ring, s + first, n - first);
    head += n;
}

/* append one record; drops it (counted) when the ring is full, never blocks long */
static void emit(int type, int fd, const void *data, uint32_t len)
{
    struct rec r;
    if (!capturing)
        return;
    if (len > 0xffff)
        len = 0xffff;
    r.magic = MAGIC;
    r.ts_us = now_us();
    r.tid = (uint32_t)syscall(SYS_gettid);
    r.type = (uint8_t)type;
    r.fd_lo = (int8_t)fd;
    r.len = (uint16_t)len;
    pthread_mutex_lock(&lock);
    if (RING_SIZE - (head - tail) < sizeof r + len) {
        dropped += len;
    } else {
        ring_put(&r, sizeof r);
        ring_put(data, len);
    }
    pthread_mutex_unlock(&lock);
}

static int is_owner(void)
{
    return getpid() == owner_pid;     /* false in vfork children */
}

static int matches(const char *path)
{
    return path && strncmp(path, match, strlen(match)) == 0;
}

#include "sglog.c"
#include "sgheat.c"
#include "sgweb.c"
#include "sgwait.c"
#include "sgexpo.c"
#include "sgzmon.c"
#include "sgdl.c"
#include "sgtilt.c"
#include "sgsys.c"
#include "sgmach.c"
#ifdef SGTAP_AA
#include "sgspi.c"
#include "sgdisp.c"
#include "sgmx.c"
#else
#include "sgnoaa.c"
#endif

static void track_open(int fd, const char *path)
{
    if (active && fd >= 0 && fd < MAX_FD && matches(path) && is_owner()) {
        int i;
        ensure_thread();
        tracked[fd] = 1;
        for (i = 0; i < NSLOT; i++)
            if (slots[i].fd < 0 || slots[i].fd == fd) {
                slots[i].have_tios = 0;
                snprintf(slots[i].path, sizeof slots[i].path, "%s", path);
                slots[i].fd = fd;
                break;
            }
        emit(2, fd, path, strlen(path));
    }
}

/* ---- wrappers: real call first, result returned unchanged ---- */

ssize_t read(int fd, void *buf, size_t n)
{
    ssize_t r;
    resolve();
    r = real_read(fd, buf, n);
    if (r > 0 && fd >= 0 && fd < MAX_FD && tracked[fd] && is_owner()) {
        int e = errno;
        ensure_thread();
        emit(0, fd, buf, (uint32_t)r);
        sglog_on_read(fd, buf, (size_t)r);
        errno = e;
    } else if (r > 0 && fd == spi_fd && spi_on && is_owner()) {
        int e = errno;
        sgspi_log(0, buf, (size_t)r);
        errno = e;
    }
    return r;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    ssize_t r;
    resolve();
    r = real_write(fd, buf, n);
    if (r > 0 && fd >= 0 && fd < MAX_FD && tracked[fd] && is_owner()) {
        int e = errno;
        ensure_thread();
        emit(1, fd, buf, (uint32_t)r);
        sglog_on_write(fd);
        errno = e;
    } else if (r > 0 && fd == spi_fd && (spi_on || mx_cap) && is_owner()) {   /* board link tracer (8.25), look only; Multi Exposure image capture */
        int e = errno;
        if (mx_cap)
            sgmx_capture(buf, (size_t)r);
        if (spi_on)
            sgspi_log(1, buf, (size_t)r);
        errno = e;
    }
    return r;
}

static int do_open(int (*fn)(const char *, int, ...), const char *path, int flags, va_list ap)
{
    int fd, e;
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & __O_TMPFILE) == __O_TMPFILE) /* = glibc __OPEN_NEEDS_MODE */
        mode = va_arg(ap, mode_t);
    fd = fn(path, flags, mode);
    e = errno;
    track_open(fd, path);
    if (fd >= 0 && active && is_owner())
        sglog_on_open(fd, path);
    if (fd >= 0 && active && is_owner() && path && strncmp(path, spi_match, strlen(spi_match)) == 0)
        spi_fd = fd;                  /* the board link: only remembered, traced only with sgcap/FPGATRACE */
    errno = e;
    return fd;
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    int fd;
    resolve();
    va_start(ap, flags);
    fd = do_open(real_open, path, flags, ap);
    va_end(ap);
    return fd;
}

int open64(const char *path, int flags, ...)
{
    va_list ap;
    int fd;
    resolve();
    va_start(ap, flags);
    fd = do_open(real_open64, path, flags, ap);
    va_end(ap);
    return fd;
}

/* chitu opens the print file with fopen64: only its name is of interest (sglog) */
FILE *fopen(const char *path, const char *mode)
{
    FILE *f;
    int e;
    resolve();
    f = real_fopen(path, mode);
    e = errno;
    if (f && active && is_owner())
        sglog_on_open(-1, path);
    errno = e;
    return f;
}

FILE *fopen64(const char *path, const char *mode)
{
    FILE *f;
    int e;
    resolve();
    f = real_fopen64(path, mode);
    e = errno;
    if (f && active && is_owner())
        sglog_on_open(-1, path);
    errno = e;
    return f;
}

int close(int fd)
{
    resolve();
    if (fd >= 0 && fd == spi_fd && is_owner())
        spi_fd = -1;
    if (fd >= 0 && fd < MAX_FD && tracked[fd] && is_owner()) {
        int i;
        tracked[fd] = 0;
        for (i = 0; i < NSLOT; i++)
            if (slots[i].fd == fd)
                slots[i].fd = -1;
        emit(3, fd, NULL, 0);
        sglog_on_close(fd);
    }
    return real_close(fd);
}

int tcsetattr(int fd, int act, const struct termios *t)
{
    int r, e;
    resolve();
    r = real_tcsetattr(fd, act, t);
    e = errno;
    if (r == 0 && t && fd >= 0 && fd < MAX_FD && tracked[fd] && is_owner()) {
        unsigned char p[sizeof *t + 4];
        uint32_t sp = (uint32_t)cfgetospeed(t);
        int i;
        memcpy(p, t, sizeof *t);
        memcpy(p + sizeof *t, &sp, 4);
        for (i = 0; i < NSLOT; i++)
            if (slots[i].fd == fd) {
                memcpy(slots[i].tios, p, sizeof p);
                slots[i].have_tios = 1;
            }
        emit(4, fd, p, sizeof p);
    }
    errno = e;
    return r;
}

/* ---- writer thread ---- */

static void start_file(void)
{
    char path[200], msg[96], boot[40] = "0";
    uint64_t m = now_us();
    int b = real_open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (b >= 0) {                     /* first 8 hex digits: unique per boot */
        if (real_read(b, boot, 8) != 8)
            snprintf(boot, sizeof boot, "0");
        boot[8] = 0;
        real_close(b);
    }
    snprintf(path, sizeof path, "%s/cap_%s_%llu.bin", dir, boot, (unsigned long long)(m / 1000000u));
    out_fd = real_open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    out_bytes = 0;
    if (out_fd < 0)
        return;
    capturing = 1;
    snprintf(msg, sizeof msg, "sgtap v1 wall=%ld mono_us=%llu match=%s",
             (long)time(NULL), (unsigned long long)m, match);
    emit(5, -1, msg, strlen(msg));
    for (int i = 0; i < NSLOT; i++) {
        struct slot sl = slots[i];    /* copy: the app may change it meanwhile */
        if (sl.fd < 0)
            continue;
        emit(2, sl.fd, sl.path, strlen(sl.path));
        if (sl.have_tios)
            emit(4, sl.fd, sl.tios, sizeof sl.tios);
    }
}

static void drain(void)
{
    static unsigned char buf[65536];
    uint32_t n, d;
    pthread_mutex_lock(&lock);
    d = dropped;
    dropped = 0;
    pthread_mutex_unlock(&lock);
    if (d)
        emit(6, -1, &d, 4);
    for (;;) {
        pthread_mutex_lock(&lock);
        n = head - tail;
        if (n > sizeof buf)
            n = sizeof buf;
        {
            uint32_t pos = tail % RING_SIZE, first = RING_SIZE - pos;
            if (first > n)
                first = n;
            memcpy(buf, ring + pos, first);
            memcpy(buf + first, ring, n - first);
        }
        tail += n;
        pthread_mutex_unlock(&lock);
        if (!n)
            break;
        if (out_fd >= 0 && out_bytes < FILE_MAX) {
            if (real_write(out_fd, buf, n) == (ssize_t)n)
                out_bytes += n;
        }
    }
}

static void *writer(void *arg)
{
    char flag[160];
    struct timespec next;
    (void)arg;
    snprintf(flag, sizeof flag, "%s/ENABLE", dir);
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        int want = access(flag, F_OK) == 0;
        sglog_tick();                 /* every 0.5 s: app log follower + CSV row */
        if (want && out_fd < 0)
            start_file();
        drain();
        if (!want && out_fd >= 0) {
            capturing = 0;
            drain();
            real_close(out_fd);
            out_fd = -1;
        }
        next.tv_nsec += 500000000;    /* fixed 0.5 s grid, no drift */
        if (next.tv_nsec >= 1000000000) {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
            ;
    }
    return NULL;
}

/* start the writer thread once per process (cheap check: getpid is one syscall) */
static void ensure_thread(void)
{
    pthread_t th;
    pthread_attr_t at;
    pid_t me = getpid(), old = thread_pid;
    if (!active || me != owner_pid || old == me ||
        !__sync_bool_compare_and_swap(&thread_pid, old, me))
        return;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);   /* the app's savers (sgheat) use ~11 KB */
    if (pthread_create(&th, &at, writer, NULL) != 0)
        active = 0;
    pthread_attr_destroy(&at);
}

/* fork child: no writer thread here, the lock may have been held by it, and the capture
 * file belongs to the parent. Only async-signal-safe work; the thread starts later. */
static void atfork_child(void)
{
    pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
    owner_pid = getpid();
    memcpy(&lock, &fresh, sizeof lock);
    capturing = 0;
    head = tail = dropped = 0;
    if (out_fd >= 0)
        real_close(out_fd);
    out_fd = -1;
    sglog_atfork_child();
}

__attribute__((constructor)) static void sgtap_init(void)
{
    char exe[128];
    ssize_t n;
    const char *s;

    resolve();
    unsetenv("LD_PRELOAD");            /* children must not load the tap */
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    in_chitu = strcmp(exe, "/customer/resources/chitu") == 0;
    if (!in_chitu && !getenv("SGTAP_FORCE"))
        return;
    if ((s = getenv("SGTAP_MATCH")) && *s)
        snprintf(match, sizeof match, "%s", s);
    if ((s = getenv("SGTAP_DIR")) && *s)
        snprintf(dir, sizeof dir, "%s", s);
    if ((s = getenv("SGTAP_SPIMATCH")) && *s)
        snprintf(spi_match, sizeof spi_match, "%s", s);
    sglog_init();
    owner_pid = getpid();
    active = 1;
    __register_atfork(NULL, NULL, atfork_child, NULL);
    ensure_thread();
}
