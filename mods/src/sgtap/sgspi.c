/*
 * sgspi.c - passive tracer of the link between the app and the display / motion board (step 8.25),
 * #included by sgtap.c after sgmach.c. The app talks to that board with plain write / read on
 * /dev/spidev0.0 (transport 0x88ef4 / 0x88f30, SPI mode ioctls in 0x88f6c): command frames such as 0x29
 * (exposure on), 0x28 (exposure off), 0x1a (8-byte parameter command) and the layer image as 0x800-byte
 * packets "0x3c + data" (0x83690). This module only LOOKS: every wrapper still calls the real function
 * first and returns its result unchanged; nothing is written to the board, nothing is changed.
 *
 * OFF by default. Switch: <dir>/FPGATRACE (dir = /media/mmcblk0p2/sgcap, like the serial capture)
 * created / deleted by hand over telnet (checked every ~1 s). While it exists a CSV is written:
 *   <dir>/fpga_<bootid>_<mono_s>.csv    ms,tid,kind,len,info
 *     ms   = CLOCK_MONOTONIC in ms (the same clock as the app log's [tick]), tid = thread id
 *     kind = W write, R read (info = first 32 bytes as hex), I image burst = consecutive 0x3c packets
 *            of >= 0x400 bytes: len = total bytes, info = "<packets>p <duration ms>ms <first 24 bytes hex>"
 * File stops growing at 20 MB. IMAGE DUMP (for the multi exposure design): while <dir>/FPGADUMP exists AND the trace is on, the payload of the
 * first SPI_DUMP_FILES image bursts (all 0x3c packets without the command byte, i.e. the layer's compressed stream) is written to
 * <dir>/fpga_img_<t0 ms>_<packets>p.bin (t0 = the burst's first packet, same clock as the CSV and the app log [tick]); 6 MB buffer, bigger bursts are skipped
 * (noted in the CSV). Remove the file to stop and free the buffer. Kill switch (never traces): /customer/mod/sgspi_disable.
 * live.json "spi":{"fd":n,"on":0|1,"kb":written} (fd -1 = the board's device was not opened by the app).
 */
#define SPI_FILE_MAX   (20u << 20)
#define SPI_RING       (256u * 1024u)
#define SPI_BURST_MIN  0x400u
#define SPI_IMG_CMD    0x3cu
#define SPI_DUMP_MAX   (6u << 20)
#define SPI_DUMP_FILES 6u

static char spi_match[64] = "/dev/spidev";   /* env SGTAP_SPIMATCH overrides (tests) */
static volatile int spi_fd = -1;       /* the app's fd on /dev/spidev*, -1 = none */
static volatile int spi_on;            /* switch file present and not disabled */
static int spi_out = -1;
static uint32_t spi_written;
static pthread_mutex_t spi_lock = PTHREAD_MUTEX_INITIALIZER;
static char spi_ring[SPI_RING];
static uint32_t spi_len, spi_drop;
static struct {
    unsigned n;
    uint32_t bytes;
    uint64_t t0, t1;
    uint32_t tid;
    unsigned char first[24];
} spi_burst;

static volatile int spi_dump_on;       /* FPGADUMP present */
static unsigned char *spi_dbuf;        /* capture buffer of the burst in progress (allocated by the tick) */
static uint32_t spi_dlen;
static int spi_dover;
static unsigned char *spi_dready;      /* finished burst waiting for the tick to write it */
static uint32_t spi_dreadylen, spi_dreadyn;
static uint64_t spi_dreadyt0;
static unsigned spi_dfiles;

static void spi_put(const char *s, int n)
{
    if (n <= 0)
        return;
    if (spi_len + (uint32_t)n > SPI_RING) {
        spi_drop++;
        return;
    }
    memcpy(spi_ring + spi_len, s, (size_t)n);
    spi_len += (uint32_t)n;
}

static int spi_hex(char *o, size_t max, const unsigned char *b, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n && 2 * i + 2 < max; i++) {
        o[2 * i] = hx[b[i] >> 4];
        o[2 * i + 1] = hx[b[i] & 15];
    }
    o[2 * i] = 0;
    return (int)(2 * i);
}

/* a finished image burst -> one line (spi_lock held) */
static void spi_flush_burst(void)
{
    char line[200], hex[64];
    int n;
    if (!spi_burst.n)
        return;
    spi_hex(hex, sizeof hex, spi_burst.first, sizeof spi_burst.first);
    n = snprintf(line, sizeof line, "%llu,%u,I,%u,%up %llums %s\n", (unsigned long long)(spi_burst.t0 / 1000u),
                 spi_burst.tid, spi_burst.bytes, spi_burst.n,
                 (unsigned long long)((spi_burst.t1 - spi_burst.t0) / 1000u), hex);
    spi_put(line, n);
    if (spi_dump_on && spi_dbuf) {
        if (spi_dover) {
            n = snprintf(line, sizeof line, "# image burst of %u packets not dumped (over %u MB)\n", spi_burst.n, SPI_DUMP_MAX >> 20);
            spi_put(line, n);
        } else if (spi_dlen && !spi_dready && spi_dfiles < SPI_DUMP_FILES) {
            spi_dready = spi_dbuf;
            spi_dbuf = NULL;             /* the tick writes it and allocates the next buffer */
            spi_dreadylen = spi_dlen;
            spi_dreadyn = spi_burst.n;
            spi_dreadyt0 = spi_burst.t0;
        }
    }
    spi_dlen = 0;
    spi_dover = 0;
    spi_burst.n = 0;
    spi_burst.bytes = 0;
}

/* called by the write / read wrappers after the real call, only for the board's fd */
static void sgspi_log(int is_write, const void *buf, size_t n)
{
    const unsigned char *b = buf;
    uint64_t t;
    if (!spi_on || n == 0)
        return;
    t = now_us();
    pthread_mutex_lock(&spi_lock);
    if (is_write && n >= SPI_BURST_MIN && b[0] == SPI_IMG_CMD) {
        if (!spi_burst.n) {
            spi_burst.t0 = t;
            spi_burst.tid = (uint32_t)syscall(SYS_gettid);
            memcpy(spi_burst.first, b, sizeof spi_burst.first);
        }
        if (spi_dump_on && spi_dbuf && !spi_dover) {
            if (spi_dlen + (uint32_t)n - 1 <= SPI_DUMP_MAX) {
                memcpy(spi_dbuf + spi_dlen, b + 1, n - 1);
                spi_dlen += (uint32_t)n - 1;
            } else {
                spi_dover = 1;
            }
        }
        spi_burst.n++;
        spi_burst.bytes += (uint32_t)n;
        spi_burst.t1 = t;
    } else {
        char line[160], hex[72];
        int k;
        spi_flush_burst();
        spi_hex(hex, sizeof hex, b, n < 32 ? n : 32);
        k = snprintf(line, sizeof line, "%llu,%u,%c,%u,%s\n", (unsigned long long)(t / 1000u),
                     (uint32_t)syscall(SYS_gettid), is_write ? 'W' : 'R', (unsigned)n, hex);
        spi_put(line, k);
    }
    pthread_mutex_unlock(&spi_lock);
}

static void spi_open_file(void)
{
    char path[200], head[160], boot[40] = "0";
    int b, n;
    uint64_t m = now_us();
    b = real_open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (b >= 0) {
        if (real_read(b, boot, 8) != 8)
            snprintf(boot, sizeof boot, "0");
        boot[8] = 0;
        real_close(b);
    }
    snprintf(path, sizeof path, "%s/fpga_%s_%llu.csv", dir, boot, (unsigned long long)(m / 1000000u));
    spi_out = real_open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    spi_written = 0;
    if (spi_out < 0)
        return;
    n = snprintf(head, sizeof head, "# sgspi v1 wall=%ld mono_ms=%llu fd=%d\nms,tid,kind,len,info\n", (long)time(NULL),
                 (unsigned long long)(m / 1000u), spi_fd);
    if (real_write(spi_out, head, (size_t)n) == (ssize_t)n)
        spi_written += (uint32_t)n;
}

/* writer thread, every 0.5 s (from sgsys_tick) */
static void sgspi_tick(void)
{
    static unsigned ticks;
    static int disabled = -1;
    int want;
    if (disabled < 0)
        disabled = (!in_chitu && !getenv("SGTAP_FORCE")) || access("/customer/mod/sgspi_disable", F_OK) == 0;
    if (!disabled && (ticks++ % 2) == 0) {
        char flag[160];
        snprintf(flag, sizeof flag, "%s/FPGATRACE", dir);
        want = access(flag, F_OK) == 0 && spi_fd >= 0;
        if (want && spi_out < 0) {
            spi_open_file();
            spi_on = spi_out >= 0;
        }
        if (!want && spi_on)
            spi_on = 0;
        snprintf(flag, sizeof flag, "%s/FPGADUMP", dir);
        spi_dump_on = spi_on && access(flag, F_OK) == 0;
    }
    if (spi_dump_on && !spi_dbuf && !spi_burst.n && spi_dfiles < SPI_DUMP_FILES) {
        unsigned char *nb = malloc(SPI_DUMP_MAX);        /* between two bursts only */
        pthread_mutex_lock(&spi_lock);
        if (!spi_dbuf && !spi_burst.n)
            spi_dbuf = nb;
        else
            free(nb);
        pthread_mutex_unlock(&spi_lock);
    }
    if (!spi_dump_on) {
        unsigned char *f1 = NULL, *f2 = NULL;
        pthread_mutex_lock(&spi_lock);
        if (!spi_burst.n) {
            f1 = spi_dbuf;
            f2 = spi_dready;
            spi_dbuf = spi_dready = NULL;
            spi_dfiles = 0;
        }
        pthread_mutex_unlock(&spi_lock);
        free(f1);
        free(f2);
    }
    if (spi_dready) {
        unsigned char *buf;
        uint32_t ln, pk;
        uint64_t t0;
        pthread_mutex_lock(&spi_lock);
        buf = spi_dready;
        ln = spi_dreadylen;
        pk = spi_dreadyn;
        t0 = spi_dreadyt0;
        pthread_mutex_unlock(&spi_lock);
        if (buf) {
            char path[220];
            int fd;
            snprintf(path, sizeof path, "%s/fpga_img_%llu_%up.bin", dir, (unsigned long long)(t0 / 1000u), pk);
            fd = real_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd >= 0) {
                real_write(fd, buf, ln);
                real_close(fd);
            }
            pthread_mutex_lock(&spi_lock);
            spi_dready = NULL;
            spi_dfiles++;
            pthread_mutex_unlock(&spi_lock);
            free(buf);
        }
    }
    pthread_mutex_lock(&spi_lock);
    if (spi_burst.n && now_us() - spi_burst.t1 > 100000u)
        spi_flush_burst();               /* the last burst of a transfer */
    if (spi_len && spi_out >= 0) {
        if (spi_written < SPI_FILE_MAX && real_write(spi_out, spi_ring, spi_len) == (ssize_t)spi_len)
            spi_written += spi_len;
        spi_len = 0;
    } else if (spi_len) {
        spi_len = 0;
    }
    if (spi_drop && spi_out >= 0) {
        char l[64];
        int k = snprintf(l, sizeof l, "# dropped %u line(s) (ring full)\n", spi_drop);
        if (real_write(spi_out, l, (size_t)k) == (ssize_t)k)
            spi_written += (uint32_t)k;
        spi_drop = 0;
    }
    pthread_mutex_unlock(&spi_lock);
    if (!spi_on && spi_out >= 0) {
        real_close(spi_out);
        spi_out = -1;
    }
}

static int sgspi_json(char *buf, size_t len)
{
    return snprintf(buf, len, ",\"spi\":{\"fd\":%d,\"on\":%d,\"kb\":%u,\"dump\":%u}", spi_fd, spi_on ? 1 : 0, spi_written >> 10, spi_dump_on ? spi_dfiles : 0);
}
