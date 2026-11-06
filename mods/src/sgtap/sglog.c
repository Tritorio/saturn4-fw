/*
 * sglog.c - strain-gauge datalogger (step 8.3c), #included by sgtap.c (shares its
 * real_* pointers, owner checks and writer thread). See docs/strain_gauge_protocol.txt.
 *
 * - Parses the gauge board replies on the gauge tty (default /dev/ttyS1):
 *     01 03 04 v0 v1 v2 v3 c_hi c_lo, value = int32 LE, CRC-16/KERMIT (high byte first)
 *   Keeps the latest value, min/max of the current 0.5 s window and a short history.
 * - Follows the app's own log (/media/mmcblk0p2/log -> log1/log2, read only) for the
 *   print session (printer_main_routine enter/exit), layer number and phase. The log
 *   tick [..][..][..][TICK] is CLOCK_MONOTONIC in ms.
 * - Per session: <csvdir>/<YYYYmmdd_HHMMSS>_<printfile>.csv (+ .txt with metadata),
 *   one row every 0.5 s:
 *     time_utc,t_s,layer,phase,raw_last,value,value_min,value_max,n,raw_min,raw_max,g,g_min,g_max,
 *     tank_c,wait_s,wait_end,heat_w,cpu,rel_v1,rel_v2,sep_deg,sep_kind,ret_v1,ret_v2,dl_peak_g,dl_f,expo_file,expo_set,expo_used,z_ret_mm,z_step_um
 *   value = raw/100 - zero (chitu's own units, same as its thresholds), zero = median
 *   of raw/100 over the 2 s before the session start. n = readings in the window.
 *   raw_min/raw_max = extremes of the window. g = (raw - ref) / k in grams, ref = mean
 *   raw_last of the calibrate rows once that phase is over (before: zero * 100, as
 *   noted in the .txt), k = raw counts per gram: 100.33 (calibrated ) or the
 *   number in <csvdir>/K (sign allowed), read at each session start. Weight on the plate
 *   -> negative with a positive k. tank_c = vat temperature in C (sgheat.c, empty when
 *   heating is not available). wait_s/wait_end/heat_w (sgwait.c): on the first row after each
 *   wait after retract: the wait used (s), settled|max|off, 1 = heated in the wait.
 *   cpu = the printer's CPU use in % (busy share of all cores over the last 2 s, sgsys.c; empty before the first value).
 *   rel_v1,rel_v2,sep_deg,sep_kind,ret_v1,ret_v2,dl_peak_g,dl_f (sgtilt.c): on the first row after a release / return: X tilt speeds in deg/s as sent
 *   to the chip (v2 empty = one stage; Dynamic lift: v1 = its start speed, v2 = the fast speed), Dynamic lift's separation angle in deg
 *   and how it ended (sep | nopeel | none), dl_peak_g = the peel peak of that release in g, dl_f = its start speed / the set speed, all empty when not applicable. csv_printf lines are limited to 383 chars.
 *   z_ret_mm,z_step_um (sgzmon.c): on the first row after the start of each exposure: the print height (mm: the app's Z step counter minus the Z zero of the machine settings) after the retract, and the step from the previous layer in um (empty on the first layer).
 *   expo_file,expo_set,expo_used (sgexpo.c): on the first row after each finished exposure: the print file's exposure time, the value set by the
 *   exposure override (empty = none) and the measured exposure (s).
 *   Line 2 of the CSV = one quoted description per column (sgcolumns.h, same text in the .txt as column_help); data from line 3.
 * - <csvdir>/DISABLE stops new sessions. Oldest files deleted above 200 MB in total.
 * Env (tests): SGTAP_GAUGE (tty path prefix), SGTAP_LOG, SGTAP_CSVDIR.
 */
#include <dirent.h>
#include <sys/stat.h>
#include "sgcolumns.h"

#define CSV_TOTAL_MAX  (200u << 20)
#define CSV_FILE_MAX   (50u << 20)
#define HIST           4096           /* ~13.6 min of readings at 5 Hz (user: the Dynamic wait window has no 3 s limit any more; was 64 = ~12 s) */

/* sgweb.c (live feed, tare), #included after this file */
static void sgweb_session_start(void);
static void sgweb_calib(double ref);
static void sgweb_tick(void);
static double parse_sk(const char *p);
static int k_ok(double k);
/* sgheat.c */
static int sgheat_temp(double *c);
static void sgheat_tick(void);
/* sgwait.c */
static void sgwait_csv(char *buf, size_t len);
static void sgtilt_csv(char *buf, size_t len);
static void sgexpo_csv(char *buf, size_t len);
static void sgzmon_csv(char *buf, size_t len);
static int sgsys_cpu(double *cpu);

static char sg_gauge[64] = "/dev/ttyS1";
static char sg_logpath[128] = "/media/mmcblk0p2/log";
static char sg_csvdir[128] = "/media/mmcblk0p2/sglog";

/* ---- gauge state, written by the app's reader thread, read by our writer thread ---- */
static pthread_mutex_t glock = PTHREAD_MUTEX_INITIALIZER;
static int gauge_fd = -1;
static unsigned char gbuf[64];
static int glen;
static int g_have;
static int32_t g_last, w_min, w_max;
static int w_n;
static uint32_t g_crc_bad, g_frames;
static struct { uint64_t ts; int32_t raw; } hist[HIST];
static unsigned hpos;
/* the gauge readings for the tilt code (Dynamic lift, sgtilt.c): position counter, readings since a position, mean of the last n */
static unsigned sg_hist_pos(void)
{
    unsigned p;
    pthread_mutex_lock(&glock);
    p = hpos;
    pthread_mutex_unlock(&glock);
    return p;
}

static int sg_hist_since(unsigned *pos, int32_t *raw, int max)
{
    int n = 0;
    pthread_mutex_lock(&glock);
    if (hpos - *pos > HIST)
        *pos = hpos - HIST;
    while (*pos != hpos && n < max) {
        raw[n++] = hist[*pos % HIST].raw;
        (*pos)++;
    }
    pthread_mutex_unlock(&glock);
    return n;
}

static int sg_hist_mean(int n, double *mean)      /* 1 = ok (at least one reading) */
{
    double s = 0;
    int k = 0;
    pthread_mutex_lock(&glock);
    for (unsigned p = hpos; k < n && hpos - p < HIST && p != 0; k++) {
        p--;
        s += hist[p % HIST].raw;
    }
    pthread_mutex_unlock(&glock);
    if (!k)
        return 0;
    *mean = s / k;
    return 1;
}

static volatile uint64_t gp_last_us;      /* gauge poll clock for the release sync (sgtilt.c): time of the app's last request */
static volatile double gp_dt_us = 196700; /* its interval, learned (150-260 ms replies only) */
static unsigned gp_n;
static volatile unsigned lg_lift2;        /* app log "lifting = 2" lines (end of a release) seen */
static char print_file[160];         /* last print file opened by the app */

static uint16_t crc_kermit(const unsigned char *p, int n)
{
    uint16_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    return c;
}

/* own decimal parser: with _GNU_SOURCE the new glibc headers turn strtoull/atoi into
 * __isoc23_* symbols that the printer's glibc 2.28 does not have */
static unsigned long long parse_u(const char *p)
{
    unsigned long long v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (unsigned)(*p++ - '0');
    return v;
}

static int is_print_file(const char *p);

/* "100.33" -> 100.33 (no strtod: keep the libc imports minimal) */
static double parse_k(const char *p)
{
    double v = 0, f = 0.1;
    while (*p == ' ')
        p++;
    for (; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (*p - '0');
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++, f /= 10)
            v += (*p - '0') * f;
    return v;
}

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

/* called (owner process only) for every successful open of the app */
static void sglog_on_open(int fd, const char *path)
{
    if (!path)
        return;
    if (fd >= 0 && strncmp(path, sg_gauge, strlen(sg_gauge)) == 0) {
        pthread_mutex_lock(&glock);
        gauge_fd = fd;
        glen = 0;
        pthread_mutex_unlock(&glock);
        return;
    }
    if (is_print_file(path)) {
        pthread_mutex_lock(&glock);
        snprintf(print_file, sizeof print_file, "%s", path);
        pthread_mutex_unlock(&glock);
    }
}

static void sglog_on_close(int fd)
{
    if (fd == gauge_fd) {
        pthread_mutex_lock(&glock);
        gauge_fd = -1;
        glen = 0;
        pthread_mutex_unlock(&glock);
    }
}

/* app -> board: a new request starts a new reply */
static void sglog_on_write(int fd)
{
    if (fd == gauge_fd) {
        uint64_t t = now_us();
        if (gp_last_us) {
            double d = (double)(t - gp_last_us);
            if (d > 150000 && d < 260000) {
                gp_dt_us += (d - gp_dt_us) * (gp_n < 30 ? 0.2 : 0.01);
                gp_n++;
            }
        }
        gp_last_us = t;
        pthread_mutex_lock(&glock);
        glen = 0;
        pthread_mutex_unlock(&glock);
    }
}

/* board -> app bytes: reassemble 9-byte replies (they arrive as "01" + 8 bytes) */
static void sglog_on_read(int fd, const unsigned char *p, size_t n)
{
    if (fd != gauge_fd)
        return;
    pthread_mutex_lock(&glock);
    while (n) {
        size_t k = sizeof gbuf - (size_t)glen;
        if (k > n)
            k = n;
        memcpy(gbuf + glen, p, k);
        glen += (int)k;
        p += k;
        n -= k;
        for (;;) {
            while (glen > 0 && gbuf[0] != 0x01) {      /* resync on the start byte */
                memmove(gbuf, gbuf + 1, (size_t)--glen);
            }
            if (glen < 9)
                break;
            if (gbuf[1] == 0x03 && gbuf[2] == 0x04) {
                uint16_t c = crc_kermit(gbuf, 7);
                if (gbuf[7] == (c >> 8) && gbuf[8] == (c & 0xff)) {
                    int32_t v = (int32_t)((uint32_t)gbuf[3] | (uint32_t)gbuf[4] << 8 |
                                          (uint32_t)gbuf[5] << 16 | (uint32_t)gbuf[6] << 24);
                    g_last = v;
                    g_have = 1;
                    if (!w_n || v < w_min)
                        w_min = v;
                    if (!w_n || v > w_max)
                        w_max = v;
                    w_n++;
                    g_frames++;
                    hist[hpos % HIST].ts = now_us();
                    hist[hpos % HIST].raw = v;
                    hpos++;
                    glen -= 9;
                    memmove(gbuf, gbuf + 9, (size_t)glen);
                    continue;
                }
                g_crc_bad++;
            }
            memmove(gbuf, gbuf + 1, (size_t)--glen);   /* not a reply: drop one byte */
        }
    }
    pthread_mutex_unlock(&glock);
}

/* ---- session / CSV (writer thread only) ---- */
static int s_active, s_pending;       /* pending: CSV opens 1 s after the start line */
static volatile int lg_heat_on;                 /* the app's tank heater output: 1 after "heat_control on", 0 after "heat_control off" (shown as the yellow dot in the Status card) */
static volatile unsigned lg_z_idle, lg_x_idle, lg_z_trig;   /* app log: "axis 0 is idle" / "sg_trig" lines seen (sgheat.c plate move) */
static uint64_t s_idle_tick;           /* log tick of the first of the consecutive IDLE state lines, 0 = none */
static uint64_t s_start_us;
static int s_layer = -1;
static char s_phase[16] = "start";
static int32_t s_zero;
static double s_ref, s_k = 100.33, cal_sum;   /* grams: (raw - s_ref) / s_k */
static int cal_n, cal_done;
static char txt_name[256];
static int csv_fd = -1;
static uint32_t csv_bytes;

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static long file_size(const char *path)
{
    int fd = real_open(path, O_RDONLY | O_CLOEXEC);
    long n;
    if (fd < 0)
        return 0;
    n = lseek(fd, 0, SEEK_END);
    real_close(fd);
    return n < 0 ? 0 : n;
}

/* delete the oldest files (names start with the date) while the total is too big */
static void retention(void)
{
    static char names[512][80];
    static char *ptr[512];
    char path[256];
    unsigned long total = 0;
    int n = 0, i;
    DIR *d = opendir(sg_csvdir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) && n < 512)
        if (has_suffix(e->d_name, ".csv") || has_suffix(e->d_name, ".txt")) {
            snprintf(names[n], sizeof names[n], "%s", e->d_name);
            ptr[n] = names[n];
            n++;
        }
    closedir(d);
    qsort(ptr, (size_t)n, sizeof ptr[0], cmp_str);
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", sg_csvdir, ptr[i]);
        total += (unsigned long)file_size(path);
    }
    for (i = 0; i < n && total > CSV_TOTAL_MAX; i++) {
        snprintf(path, sizeof path, "%s/%s", sg_csvdir, ptr[i]);
        total -= (unsigned long)file_size(path);
        unlink(path);
    }
}

static int csv_printf(const char *fmt, ...)
{
    char line[384];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (csv_fd < 0 || n <= 0 || csv_bytes > CSV_FILE_MAX)
        return -1;
    if (n > (int)sizeof line - 1)
        n = sizeof line - 1;
    if (real_write(csv_fd, line, (size_t)n) == n)
        csv_bytes += (uint32_t)n;
    return n;
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static void session_start(uint64_t start_us)
{
    int32_t v[64];                                /* the readings of 2 s (~10); not HIST: it is 4096 now */
    int n = 0;
    s_active = 1;
    s_idle_tick = 0;
    s_pending = 1;
    s_start_us = start_us;
    s_layer = -1;
    snprintf(s_phase, sizeof s_phase, "start");
    /* zero = median of raw/100 over the 2 s before the start line */
    pthread_mutex_lock(&glock);
    for (unsigned i = 0; i < HIST && i < hpos; i++) {
        unsigned k = (hpos - 1 - i) % HIST;
        if (n < 64 && hist[k].ts <= start_us && hist[k].ts + 2000000u >= start_us)
            v[n++] = hist[k].raw / 100;
    }
    s_zero = n ? 0 : (g_have ? g_last / 100 : 0);
    pthread_mutex_unlock(&glock);
    if (n) {
        qsort(v, (size_t)n, sizeof v[0], cmp_i32);
        s_zero = v[n / 2];
    }
    s_ref = s_zero * 100.0;
    cal_sum = 0;
    cal_n = cal_done = 0;
    s_k = 100.33;
    {
        char path[160], buf[32];
        int fd;
        ssize_t r;
        snprintf(path, sizeof path, "%s/K", sg_csvdir);
        fd = real_open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            r = real_read(fd, buf, sizeof buf - 1);
            real_close(fd);
            buf[r > 0 ? r : 0] = 0;
            if (k_ok(parse_sk(buf)))
                s_k = parse_sk(buf);
        }
    }
    sgweb_session_start();
}

static int is_print_file(const char *p)
{
    static const char *const ext[] = {".ctb", ".goo", ".prz", ".cbddlp", ".jxs", ".pwmx",
                                      ".pm4u", ".photon", ".photons", NULL};
    for (int i = 0; ext[i]; i++)
        if (has_suffix(p, ext[i]))
            return 1;
    return 0;
}

/* fallback: the print file the app has open right now (/proc/self/fd) */
static void print_file_from_fds(char *out, size_t len)
{
    char link[48], target[160];
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    ssize_t n;
    if (!d)
        return;
    while ((e = readdir(d))) {
        snprintf(link, sizeof link, "/proc/self/fd/%s", e->d_name);
        n = readlink(link, target, sizeof target - 1);
        if (n <= 0)
            continue;
        target[n] = 0;
        if (is_print_file(target)) {
            snprintf(out, len, "%s", target);
            break;
        }
    }
    closedir(d);
}

static void csv_open(void)
{
    char base[64], name[256], ts[32], file[160];
    const char *b;
    struct timespec rt;
    struct tm tm;
    int i;

    file[0] = 0;
    print_file_from_fds(file, sizeof file);       /* what is open now wins */
    if (!*file) {
        pthread_mutex_lock(&glock);
        snprintf(file, sizeof file, "%s", print_file);
        pthread_mutex_unlock(&glock);
    }
    b = strrchr(file, '/');
    snprintf(base, sizeof base, "%s", b ? b + 1 : (*file ? file : "unknown"));
    if ((b = strrchr(base, '.')))
        base[b - base] = 0;
    for (i = 0; base[i]; i++)                     /* keep file names Excel/FAT friendly */
        if (!((base[i] >= 'a' && base[i] <= 'z') || (base[i] >= 'A' && base[i] <= 'Z') ||
              (base[i] >= '0' && base[i] <= '9') || base[i] == '-' || base[i] == '.'))
            base[i] = '_';
    clock_gettime(CLOCK_REALTIME, &rt);
    gmtime_r(&rt.tv_sec, &tm);
    if (tm.tm_year + 1900 >= 2020)
        strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tm);
    else                                          /* clock not set yet */
        snprintf(ts, sizeof ts, "boot%llu", (unsigned long long)(now_us() / 1000000u));

    mkdir(sg_csvdir, 0755);
    retention();
    snprintf(name, sizeof name, "%s/%s_%s.txt", sg_csvdir, ts, base);
    snprintf(txt_name, sizeof txt_name, "%s", name);
    csv_fd = real_open(name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    csv_bytes = 0;
    /* several writes: csv_printf lines are limited to 383 chars */
    csv_printf("print_file=%s\nstart_mono_s=%.3f\nzero=%d\n", file, s_start_us / 1e6, (int)s_zero);
    csv_printf("units=value = raw/100 - zero (chitu sg units, e.g. probe ths 55, level 4000)\n");
    csv_printf("k=%.2f\ngrams=g = (raw - ref) / k, ref = zero*100 until calibrate ends\n", s_k);
    csv_printf("columns=time_utc,t_s,layer,phase,raw_last,value,value_min,value_max,n,"
               "raw_min,raw_max,g,g_min,g_max,tank_c,wait_s,wait_end,heat_w,cpu,rel_v1,rel_v2,sep_deg,sep_kind,ret_v1,ret_v2,dl_peak_g,dl_f,expo_file,expo_set,expo_used,z_ret_mm,z_step_um\n");
    csv_printf("column_help:\n");
    for (unsigned i = 0; i < sizeof csv_cols / sizeof csv_cols[0]; i++)
        csv_printf("  %-10s = %s\n", csv_cols[i][0], csv_cols[i][1]);
    if (csv_fd >= 0)
        real_close(csv_fd);
    snprintf(name, sizeof name, "%s/%s_%s.csv", sg_csvdir, ts, base);
    csv_fd = real_open(name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    csv_bytes = 0;
    csv_printf("time_utc,t_s,layer,phase,raw_last,value,value_min,value_max,n,"
               "raw_min,raw_max,g,g_min,g_max,tank_c,wait_s,wait_end,heat_w,cpu,rel_v1,rel_v2,sep_deg,sep_kind,ret_v1,ret_v2,dl_peak_g,dl_f,expo_file,expo_set,expo_used,z_ret_mm,z_step_um\n");
    /* line 2: one quoted description per column; data starts on line 3 */
    for (unsigned i = 0; i < sizeof csv_cols / sizeof csv_cols[0]; i++)
        csv_printf("%s\"%s\"", i ? "," : "", csv_cols[i][1]);
    csv_printf("\n");
}

/* calibrate phase over: its mean raw becomes the grams reference (noted in the .txt) */
static void calib_done(void)
{
    int fd;
    cal_done = 1;
    if (!cal_n)
        return;
    s_ref = cal_sum / cal_n;
    sgweb_calib(s_ref);
    fd = real_open(txt_name, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd >= 0) {
        char line[96];
        int n = snprintf(line, sizeof line, "ref_calibrate=%.1f (mean raw_last of %d rows)\n",
                         s_ref, cal_n);
        if (real_write(fd, line, (size_t)n) < 0)
            n = 0;
        real_close(fd);
    }
}

static void session_end(void)
{
    if (csv_fd >= 0)
        real_close(csv_fd);
    csv_fd = -1;
    s_active = s_pending = 0;
}

static void csv_row(void)
{
    int32_t last, mn, mx;
    int n, have;
    char ts[32], layer[12], tank[16] = "", wait[40], cpu[12] = "", tilt[96], expo[48], zmon[40];
    double cp;
    double tc;
    struct timespec rt;
    struct tm tm;
    uint64_t now = now_us();

    pthread_mutex_lock(&glock);
    last = g_last; mn = w_min; mx = w_max; n = w_n; have = g_have;
    w_n = 0;                                      /* next window */
    pthread_mutex_unlock(&glock);
    if (!s_active)
        return;
    if (s_pending) {
        if (now < s_start_us + 1000000u)          /* let the app open the print file */
            return;
        s_pending = 0;
        csv_open();
    }
    if (!n)
        mn = mx = last;
    clock_gettime(CLOCK_REALTIME, &rt);
    gmtime_r(&rt.tv_sec, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    if (s_layer >= 0)
        snprintf(layer, sizeof layer, "%d", s_layer);
    else
        layer[0] = 0;
    if (!cal_done && cal_n && strcmp(s_phase, "calibrate") != 0)
        calib_done();
    if (have && !cal_done && strcmp(s_phase, "calibrate") == 0) {
        cal_sum += last;
        cal_n++;
    }
    if (sgheat_temp(&tc))
        snprintf(tank, sizeof tank, "%.2f", tc);
    sgwait_csv(wait, sizeof wait);
    sgtilt_csv(tilt, sizeof tilt);
    sgexpo_csv(expo, sizeof expo);
    sgzmon_csv(zmon, sizeof zmon);
    if (sgsys_cpu(&cp))
        snprintf(cpu, sizeof cpu, "%.1f", cp);
    if (have)
        csv_printf("%s.%03ld,%.3f,%s,%s,%d,%d,%d,%d,%d,%d,%d,%.1f,%.1f,%.1f,%s%s,%s%s%s%s\n", ts,
                   rt.tv_nsec / 1000000, (now - s_start_us) / 1e6, layer, s_phase, (int)last,
                   (int)(last / 100 - s_zero), (int)(mn / 100 - s_zero),
                   (int)(mx / 100 - s_zero), n, (int)mn, (int)mx,
                   (last - s_ref) / s_k, (mn - s_ref) / s_k, (mx - s_ref) / s_k, tank, wait, cpu, tilt, expo, zmon);
    else
        csv_printf("%s.%03ld,%.3f,%s,%s,,,,,0,,,,,,%s%s,%s%s%s%s\n", ts, rt.tv_nsec / 1000000,
                   (now - s_start_us) / 1e6, layer, s_phase, tank, wait, cpu, tilt, expo, zmon);
}

/* ---- app log follower (writer thread only) ---- */
static int lf_fd = -1, lf_started;
static long lf_off;
static char lf_target[160];
static char lf_part[512];
static int lf_partlen;

static void set_phase(const char *p)
{
    snprintf(s_phase, sizeof s_phase, "%s", p);
}

/* ---- alerts: lines of the app log that mean a job was stopped or refused (foreign matter, resin too high / too low, probe /
 * level / calibration / homing failed, "print error code"). The dashboard shows a pop-up for every alert it has not shown yet (live.json
 * "alerts"). Only while a job session is open (s_active), so the plate move of the dashboard (sgheat.c) and the start-up are not reported.
 * One alert per kind in 15 s (the app repeats its lines). Writer thread adds, the json builder reads: al_lock. */
#define AL_N 8
static struct { unsigned n; long t; int layer; char kind[12]; char msg[96]; char raw[104]; } al_ring[AL_N];
static unsigned al_seq;
static long al_kind_t[8];
static pthread_mutex_t al_lock = PTHREAD_MUTEX_INITIALIZER;

static void alert_add(int ki, const char *kind, const char *msg, const char *raw)
{
    struct timespec rt;
    unsigned i;
    clock_gettime(CLOCK_REALTIME, &rt);
    if (ki >= 0 && ki < 8) {
        if (al_kind_t[ki] && rt.tv_sec - al_kind_t[ki] < 15)
            return;
        al_kind_t[ki] = rt.tv_sec;
    }
    pthread_mutex_lock(&al_lock);
    i = al_seq % AL_N;
    al_ring[i].n = ++al_seq;
    al_ring[i].t = (long)rt.tv_sec;
    al_ring[i].layer = s_layer;
    snprintf(al_ring[i].kind, sizeof al_ring[i].kind, "%s", kind);
    snprintf(al_ring[i].msg, sizeof al_ring[i].msg, "%s", msg);
    snprintf(al_ring[i].raw, sizeof al_ring[i].raw, "%s", raw);
    pthread_mutex_unlock(&al_lock);
}

static void alert_scan(const char *m)
{
    if (strncmp(m, "sg_trig ", 8) == 0)
        alert_add(0, "foreign", "Foreign matter detected: the force limit was exceeded while the plate moved down", m);
    else if (strstr(m, "the resin pos is abnormal high"))
        alert_add(1, "resin_high", "Too much resin: the resin level is above the maximum line", m);
    else if (strstr(m, "the resin pos is abnormal low"))
        alert_add(2, "resin_low", "Too little resin: the resin level is below the minimum line", m);
    else if (strstr(m, "probe1 failed") || strstr(m, "probe failed") || strstr(m, "probe error"))
        alert_add(3, "probe", "Resin probe failed: no resin detected, or something on the plate", m);
    else if (strstr(m, "level fail") || strstr(m, "level error"))
        alert_add(4, "level", "Auto-leveling failed", m);
    else if (strstr(m, "calibrate error"))
        alert_add(5, "calibrate", "Force sensor calibration failed", m);
    else if (strstr(m, "motor home failed") || strstr(m, "home fail"))
        alert_add(6, "home", "Homing failed: check the motor or the limit switch", m);
    else if (strstr(m, "print error code"))
        alert_add(7, "error", "The printer reports a print error", m);
}

static void log_line(char *l)
{
    char *m = strstr(l, "]:"), *b;
    uint64_t tick_ms;
    if (!m)
        return;
    *m = 0;                                       /* l = "[mod][lvl][line][tick" */
    b = strrchr(l, '[');
    if (!b)
        return;
    tick_ms = parse_u(b + 1);
    m += 2;
    if (strstr(m, "printer_main_routine enter")) {
        char flag[160];
        if (s_active)
            session_end();
        snprintf(flag, sizeof flag, "%s/DISABLE", sg_csvdir);
        if (access(flag, F_OK) != 0)
            session_start(tick_ms * 1000u);
        return;
    }
    if (strstr(m, "lifting = 2"))
        lg_lift2++;
    if (strstr(m, "axis 0 is idle"))
        lg_z_idle++;
    else if (strstr(m, "axis 1 is idle"))
        lg_x_idle++;
    else if (strncmp(m, "sg_trig ", 8) == 0)       /* a REAL trip ("sg_trig 1" / "sg_trig 2"); NOT the per-check lines "foregin zero .. sg_trigered 0" */
        lg_z_trig++;
    if (strncmp(m, "heat_control on", 15) == 0)
        lg_heat_on = 1;
    else if (strstr(m, "heat_control off"))
        lg_heat_on = 0;
    if (!s_active)
        return;
    alert_scan(m);
    if (strstr(m, "printer_main_routine exit")) {
        csv_row();                                /* last row, then close */
        session_end();
    } else if (strncmp(m, "layer ", 6) == 0 && strchr(m, ':')) {
        s_layer = (int)parse_u(m + 6);
        set_phase("layer");
    } else if (strstr(m, "curr : printer->")) {
        /* the app logs its state every 10 s. After a stop / finish it can take minutes until "main_routine exit"
         * (the print report to the blocked cloud times out), so an app state of IDLE for 20 s ends the session too */
        if (strstr(m, "printer->IDLE")) {
            if (!s_idle_tick)
                s_idle_tick = tick_ms ? tick_ms : 1;
            else if (tick_ms - s_idle_tick >= 20000) {
                csv_row();
                session_end();
            }
        } else {
            s_idle_tick = 0;
        }
    } else if (strstr(m, "start calibrate")) {
        set_phase("calibrate");
    } else if (strstr(m, "start probe")) {
        set_phase("probe");
    } else if (strstr(m, "start level")) {
        set_phase("level");
    } else if (strstr(m, "printer start exposure")) {
        set_phase("exposure");
    } else if (strstr(m, "cs1237_recoder_stop 0")) {
        set_phase("rest");                        /* end of the exposure: the wait after exposure until the lift */
    } else if (strstr(m, "lifting = 1")) {
        set_phase("lift");
    } else if (strstr(m, "lifting = 2")) {
        set_phase("drop");
    } else if (strstr(m, "lifting = 3")) {
        set_phase("settle");
    }
}

/* all complete lines of the open log file from lf_off on */
static void lf_read(void)
{
    char buf[16384];
    ssize_t n;
    long size;
    int rounds;

    size = lseek(lf_fd, 0, SEEK_END);
    if (size < lf_off) {                          /* truncated / rotated in place */
        lf_off = 0;
        lf_partlen = 0;
    }
    for (rounds = 0; rounds < 8 && lf_off < size; rounds++) {
        char *s, *e;
        if (lseek(lf_fd, lf_off, SEEK_SET) < 0)
            return;
        n = real_read(lf_fd, buf, sizeof buf - 1);
        if (n <= 0)
            return;
        lf_off += n;
        buf[n] = 0;
        for (s = buf; (e = memchr(s, '\n', (size_t)(buf + n - s))); s = e + 1) {
            *e = 0;
            if (lf_partlen) {                     /* line continued from last read */
                char line[1024];
                snprintf(line, sizeof line, "%.*s%s", lf_partlen, lf_part, s);
                lf_partlen = 0;
                log_line(line);
            } else {
                log_line(s);
            }
        }
        if (s < buf + n) {                        /* keep the unfinished line */
            int k = (int)(buf + n - s);
            if (k > (int)sizeof lf_part - lf_partlen)
                k = (int)sizeof lf_part - lf_partlen;
            memcpy(lf_part + lf_partlen, s, (size_t)k);
            lf_partlen += k;
        }
    }
}

static void follow_log(void)
{
    char target[160];
    ssize_t n;

    n = readlink(sg_logpath, target, sizeof target - 1);
    if (n > 0) {
        target[n] = 0;
    } else {
        snprintf(target, sizeof target, "%s", sg_logpath);
    }
    if (lf_fd < 0 || strcmp(target, lf_target) != 0) {
        if (lf_fd >= 0) {
            lf_read();                            /* the rest of the old file first (an end line may be in it) */
            real_close(lf_fd);
        }
        lf_fd = real_open(target, O_RDONLY | O_CLOEXEC);
        if (lf_fd < 0)
            return;
        snprintf(lf_target, sizeof lf_target, "%s", target);
        /* first file: start at its end (no old sessions); after a switch: at 0 */
        lf_off = lf_started ? 0 : lseek(lf_fd, 0, SEEK_END);
        lf_started = 1;
        lf_partlen = 0;
    }
    lf_read();
}

/* every 0.5 s from the writer thread */
static void sglog_tick(void)
{
    follow_log();
    csv_row();
    sgheat_tick();
    sgweb_tick();
}

static void sglog_atfork_child(void)
{
    pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
    memcpy(&glock, &fresh, sizeof glock);
    gauge_fd = -1;
    glen = 0;
    s_active = s_pending = 0;
    if (csv_fd >= 0)
        real_close(csv_fd);
    csv_fd = -1;
    if (lf_fd >= 0)
        real_close(lf_fd);
    lf_fd = -1;
}

static void sglog_init(void)
{
    const char *s;
    if ((s = getenv("SGTAP_GAUGE")) && *s)
        snprintf(sg_gauge, sizeof sg_gauge, "%s", s);
    if ((s = getenv("SGTAP_LOG")) && *s)
        snprintf(sg_logpath, sizeof sg_logpath, "%s", s);
    if ((s = getenv("SGTAP_CSVDIR")) && *s)
        snprintf(sg_csvdir, sizeof sg_csvdir, "%s", s);
}
