/*
 * sgsys.c - CPU, RAM and storage use for the page's Status card (step 8.18), #included by
 * sgtap.c after sgtilt.c. Writer thread only (sgweb_tick, every 0.5 s):
 *   CPU %  = busy share of all cores over the last 2 s (/proc/stat "cpu" line, idle + iowait
 *            = not busy), cores = "cpuN" lines, load = /proc/loadavg (1 min);
 *   RAM    = MemTotal and MemAvailable of /proc/meminfo (kB), every 2 s;
 *   storage = statfs of the print files, logs, machine data and the app volume, every 10 s.
 * live.json "sys":{cpu,cores,load,mt,ma,fs:[[name,used_kB,total_kB],..],ui:{..}}, "sys":null
 * before the first CPU sample. Read only, nothing in the app is touched.
 *
 * UI loop sleep (step 8.22): tools/patch_cpu.py (8.21) routes chitu's main loop (UI update +
 * lv_timer_handler, 0x188ac..0x188b4) through a stub at 0x2c14b4: movw r0, #us; bl usleep;
 * b 0x188ac. Our usleep below sees that call by its return address (U_RET) and sleeps the
 * dashboard's value instead: one value while printing (printer state 10), one when idle.
 * Nothing in the app is written; with libsgtap off the stub's own value applies. Only if the
 * stub's words are the patch's (else pass-through). Settings <csvdir>/UISLEEP "IDLE PRINT" (ms,
 * 5-500, default 50 200), command "uisleep IDLE PRINT"; live.json sys.ui {ok,idle,print,cur}.
 */
#include <sys/statfs.h>

#define U_LOOP     0x188acu    /* main loop: bl 0xbd980; bl 0x145e8c (LVGL); b -> stub */
#define U_BACK     0x188b4u
#define U_STUB     0x2c14b4u   /* patch_cpu.py: movw r0, #us; bl usleep@plt; b U_LOOP */
#define U_PLT      0x18598u    /* usleep@plt */
#define U_RET      (U_STUB + 8)
#define U_MIN      5
#define U_MAX      500
#define U_DEF_IDLE 50          /* touch screen in use */
#define U_DEF_PRINT 200        /* nobody touches the screen during a print */

static int (*real_usleep)(useconds_t);
static volatile int u_ok;              /* stub found: the wrapper may act */
static volatile unsigned u_cur_us;     /* what the UI loop sleeps now (0 = the stub's value) */
static int u_idle = U_DEF_IDLE, u_print = U_DEF_PRINT, u_printing;

static const struct { const char *name, *path; } s_fs[] = {
    {"files", "/media/mmcblk0p3"}, {"logs", "/media/mmcblk0p2"},
    {"machine", "/media/mmcblk0p1"}, {"app", "/customer"},
};
#define S_NFS (int)(sizeof s_fs / sizeof s_fs[0])

static struct {
    int have, cores;
    double cpu, load;
    unsigned long long busy0, total0;
    unsigned long mt, ma;
    struct { int ok; unsigned long used, total; } fs[S_NFS];
} s_sys;

/* next unsigned number in p (advances p), 0 at the end */
static unsigned long long s_num(const char **p)
{
    unsigned long long v = 0;
    while (**p && (**p < '0' || **p > '9') && **p != '\n')
        (*p)++;
    while (**p >= '0' && **p <= '9')
        v = v * 10 + (unsigned)(*(*p)++ - '0');
    return v;
}

/* kB value of "Key:" in /proc/meminfo text, 0 = not found */
static unsigned long s_mem(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (!p)
        return 0;
    p += strlen(key);
    return (unsigned long)s_num(&p);
}

/* the CPU use for the job log (sglog.c csv_row): 1 = value in *cpu */
static int sgsys_cpu(double *cpu)
{
    *cpu = s_sys.cpu;
    return s_sys.have;
}

static void s_cpu(void)
{
    char buf[512];
    const char *p = buf, *l;
    unsigned long long f[8], total = 0, busy;
    int cores = 0;
    if (read_small("/proc/stat", buf, sizeof buf) <= 0 || strncmp(buf, "cpu ", 4) != 0)
        return;
    for (int i = 0; i < 8; i++) {           /* user nice system idle iowait irq softirq steal */
        f[i] = s_num(&p);
        total += f[i];
    }
    busy = total - f[3] - f[4];
    for (l = strchr(buf, '\n'); l && strncmp(l + 1, "cpu", 3) == 0; l = strchr(l + 1, '\n'))
        cores++;
    if (s_sys.total0 && total > s_sys.total0) {
        s_sys.cpu = 100.0 * (double)(busy - s_sys.busy0) / (double)(total - s_sys.total0);
        s_sys.have = 1;
    }
    s_sys.busy0 = busy;
    s_sys.total0 = total;
    if (cores)
        s_sys.cores = cores;
    if (read_small("/proc/loadavg", buf, sizeof buf) > 0)
        s_sys.load = parse_k(buf);
    if (read_small("/proc/meminfo", buf, sizeof buf) > 0) {
        s_sys.mt = s_mem(buf, "MemTotal:");
        s_sys.ma = s_mem(buf, "MemAvailable:");
    }
}

static void s_disks(void)
{
    for (int i = 0; i < S_NFS; i++) {
        struct statfs st;
        s_sys.fs[i].ok = statfs(s_fs[i].path, &st) == 0 && st.f_blocks > 0;
        if (s_sys.fs[i].ok) {
            unsigned long kb = (unsigned long)st.f_bsize / 1024u;
            s_sys.fs[i].total = (unsigned long)st.f_blocks * kb;
            s_sys.fs[i].used = (unsigned long)(st.f_blocks - st.f_bfree) * kb;
        }
    }
}

/* ARM b / bl from a to t */
static uint32_t u_branch(uint32_t a, uint32_t t, int link)
{
    return (link ? 0xeb000000u : 0xea000000u) | (((t - (a + 8)) >> 2) & 0xffffffu);
}

static int u_valid(double i, double p)
{
    return i == (int)i && p == (int)p && i >= U_MIN && i <= U_MAX && p >= U_MIN && p <= U_MAX;
}

/* "IDLE PRINT" ms: 1 = parsed and valid */
static int u_parse(const char *c, int *i, int *p)
{
    double v[2];
    if (parse_nums(c, v, 2) != 2 || !u_valid(v[0], v[1]))
        return 0;
    *i = (int)v[0];
    *p = (int)v[1];
    return 1;
}

static void u_load(void)
{
    char path[160], buf[32];
    int i, p;
    snprintf(path, sizeof path, "%s/UISLEEP", sg_csvdir);
    if (read_small(path, buf, sizeof buf) > 0 && u_parse(buf, &i, &p)) {
        u_idle = i;
        u_print = p;
    }
}

/* set + save: 1 = ok, else res = the error */
static int u_apply(int i, int p, char *res, size_t len)
{
    char path[160], line[32];
    int n;
    if (!u_ok) {
        snprintf(res, len, "error: UI loop sleep not available (chitu without the CPU patch)");
        return 0;
    }
    u_idle = i;
    u_print = p;
    n = snprintf(line, sizeof line, "%d %d\n", i, p);
    snprintf(path, sizeof path, "%s/UISLEEP", sg_csvdir);
    if (!write_small(path, line, n)) {
        snprintf(res, len, "error: set, but saving %s failed", path);
        return 0;
    }
    return 1;
}

static void sgsys_ui_cmd(const char *c, char *res, size_t len)
{
    int i, p;
    if (!u_parse(c, &i, &p)) {
        snprintf(res, len, "error: bad uisleep command");
        return;
    }
    u_apply(i, p, res, len);
}

static void sgsys_tick(void)
{
    static unsigned ticks;
    if (ticks == 0 && in_chitu && access("/customer/mod/sguisleep_disable", F_OK) != 0)
        u_ok = HW(U_BACK) == u_branch(U_BACK, U_STUB, 0) &&
               (HW(U_STUB) & 0xfff0f000u) == 0xe3000000u &&      /* movw r0, #imm */
               HW(U_STUB + 4) == u_branch(U_STUB + 4, U_PLT, 1) &&
               HW(U_STUB + 8) == u_branch(U_STUB + 8, U_LOOP, 0);
    if (ticks % 20 == 0)
        u_load();
    u_printing = h_ok ? HW(H_PSTATE) == 10 : s_active;
    u_cur_us = u_ok ? 1000u * (unsigned)(u_printing ? u_print : u_idle) : 0;
    sgspi_tick();
    sgdisp_tick();
    sgmx_tick();
    if (ticks % 4 == 0)
        s_cpu();
    if (ticks % 20 == 0)
        s_disks();
    ticks++;
}

static int sgsys_json(char *buf, size_t len)
{
    int n, first = 1;
    if (!s_sys.have)
        return snprintf(buf, len, ",\"sys\":null");
    n = snprintf(buf, len, ",\"sys\":{\"cpu\":%.1f,\"cores\":%d,\"load\":%.2f,\"mt\":%lu,\"ma\":%lu,\"fs\":[",
                 s_sys.cpu, s_sys.cores, s_sys.load, s_sys.mt, s_sys.ma);
    for (int i = 0; i < S_NFS && n < (int)len; i++) {
        if (!s_sys.fs[i].ok)
            continue;
        n += snprintf(buf + n, len - (size_t)n, "%s[\"%s\",%lu,%lu]", first ? "" : ",", s_fs[i].name,
                      s_sys.fs[i].used, s_sys.fs[i].total);
        first = 0;
    }
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "],\"ui\":{\"ok\":%d,\"idle\":%d,\"print\":%d,\"cur\":%u}}",
                      u_ok, u_idle, u_print, u_cur_us / 1000u);
    return n;
}

/* chitu's usleep: the UI loop's call (return address in the stub) sleeps the set value */
int usleep(useconds_t us)
{
    if (!real_usleep)
        real_usleep = (int (*)(useconds_t))dlsym(RTLD_NEXT, "usleep");
    if (u_ok && u_cur_us && __builtin_return_address(0) == (void *)(uintptr_t)U_RET)
        us = u_cur_us;
    if (real_usleep)
        return real_usleep(us);
    struct timespec t = {(time_t)(us / 1000000u), (long)(us % 1000000u) * 1000};
    return nanosleep(&t, NULL);
}
