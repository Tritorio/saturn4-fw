/*
 * sgwait.c - dynamic wait after retract + heating in the wait (steps 8.11, 8.12), #included
 * by sgtap.c after sgheat.c and before sgweb.c. See docs/dynamic_wait.txt.
 *
 * The app's wait after retract is a polled check (V1.5.9, print struct P = *(0x2eb6d8)):
 *   P+0x30b (u8) = 2 while waiting, P+0x318 (u64) = wait start (CLOCK_MONOTONIC_RAW ms),
 *   exposure when now - start > P+0x25c + P+0x250 (float ms). P+0x2ac = layer.
 * Dynamic wait: our thread (20 Hz) watches each wait and, once the force is settled
 * (force change over the last WIN s = least-squares slope of the readings * WIN, below GRAD g [user: was GRAD g/s], at least MIN s
 * after the start), writes P+0x25c = elapsed - P+0x250: the app's own next pass starts the
 * exposure. The wait can only get shorter than the slicer's (= the maximum). The original
 * value is put back when the wait is over. Bottom layers: dynamic too, unless DB = 0 (then the
 * slicer wait); their count comes from the print file's settings (CTB v5 "BottomLayerCount";
 * the app keeps the decrypted settings on its heap, found by their 0xCAFEBABE marker and checked
 * against the header the app itself uses, P+0x108), else W_BOTDEF.
 * Wait & heat (needs "heating during print" and the heater ready; works with or without the
 * dynamic wait): heater mode 2 (the app's own print pulse, H_DEV+0x114; the app's heater
 * thread still applies the target, the allowed error and its time limit) from the start of
 * the wait. Dynamic wait: only if the previous wait was >= 3 s; when settled: heater off,
 * exposure W_REL_MS later. Fixed wait (dynamic off, or a bottom layer with DB = 0): if the
 * wait is > 3 s, until W_GAP_MS + W_POLL_MS before it ends. The heater is never left on when a
 * wait ends (exposure, pause, stop).
 * Tare in the wait (WT): the first time a wait is settled (same test and settings), the live
 * zero (sgweb.c) moves to the mean of those readings; with or without the dynamic wait.
 * Settings: <csvdir>/WAIT "DW WH GRAD WIN MIN [DB [WT]]" (default: off; DB missing = 1, WT = 0).
 * Command (sgweb.c): "wait DW WH GRAD WIN MIN DB WT". Kill switch: /customer/mod/sgwait_disable.
 * Profiles: "settings save NAME" / "settings restore NAME" use <csvdir>/profiles/NAME.txt (same one-line format); without a name
 * the Standard snapshot SETTINGS as before; live.json wait "pf":1 tells the page the names are supported.
 * "settings save|restore|defaults": snapshot <csvdir>/SETTINGS "DW WH GRAD WIN MIN DB WT AUTO HP
 * TPRE TPRINT TIME [N A B C D E F S P R T X Y]" of the wait, tare, auto-tare, heating and tilt
 * settings (not k, not pre-heat now); the tilt part since build 27 (12-field files still load).
 * Restore / defaults also queue the tilt profile (defaults = stock, sgtilt.c). Build 29 adds the
 * slow set (6) and the UI loop sleep (2): "... TIME N A..Y SA SB SC SD SE SF UI_IDLE UI_PRINT"
 * (12-, 25- and 33-field files load). Build 34 adds the gauge sync (ON AUTO OFFSET_MS), the wait override and the beep option:
 * "... UI_IDLE UI_PRINT GS_ON GS_AUTO GS_OFF_MS WOV BEEP" (38 fields) and then "ZOPT TILT2 SLOWEN JOBLOG" (plate move option, two-stage
 * fix, slow layers on, job logging on: 42 fields) and "DL_ON DL_LIMIT DL_LOWER DL_SLOWER DL_FASTER" (Dynamic lift: 47 fields; the older
 * lengths still load).
 * Only with the V1.5.9 code below (and sgheat's check for the heater part).
 */

#define W_PPTR     0x2eb6d8u   /* -> print struct P */
#define W_HDR      0x108       /* P+0x108 -> file header: +0x1004 res x, +0x1008 res y,
                                  +0x1010 layers, +0x1028 total height (float) */
#define W_BOTDEF   3           /* bottom layers when the print file does not tell */
#define W_STATE    0x30b
#define W_EXPO     0x30c       /* 1 = exposing */
#define W_START    0x318
#define W_EXTRA    0x250
#define W_WAIT     0x25c
#define W_LAYER    0x2ac
#define W_HEATMIN  3.0         /* s: heat in the wait only after a wait this long */
#define W_GAP_MS   1000.0      /* heater off at least this long before the UV */
#define W_POLL_MS  200.0       /* margin for our 20 Hz check when the end is the slicer wait */
#define W_REL_MS   1150.0      /* settled: UV this long after the heater-off request (the
                                  app's heater thread acts on it within 0.1 s) */
#define W_LAST     30          /* layers kept for the page's bar chart */

static const struct { uint32_t a, w; } w_sig[] = {
    {0x7c368, 0xeb015a90}, {0x7c36c, 0xe3a03002}, {0x7c370, 0xe5c4330b}, {0x7c374, 0xe5840318},
    {0x7c378, 0xe584131c}, {0x7d020, 0xe5d4330b}, {0x7d024, 0xe3530002}, {0x7d028, 0x0a0000ef},
    {0x7d3ec, 0xe30f3a30}, {0x7d3f0, 0xe3403039}, {0x7d3f4, 0xed9f7a0f}, {0x7d3f8, 0xedd37a62},
    {0x7d3fc, 0xeef47ac7}, {0x7d400, 0xeef1fa10}, {0x7d448, 0xed948a97}, {0x7d44c, 0xeb015657},
    {0x7d450, 0xe2843fc6}, {0x7d454, 0xe1c320d0}, {0x7d458, 0xe0500002}, {0x7d45c, 0xe0c11003},
    {0x7d460, 0xebfe6acd}, {0x7d464, 0xedd47a94}, {0x7d468, 0xee787a27}, {0x7d46c, 0xee070a10},
    {0x7d470, 0xeeb47ae7}, {0x79e94, 0xe3a03000}, {0x79e98, 0xe58432bc}, {0x7a7dc, 0xe3a00002},
    {0x7a7e0, 0xeb0040be}, {0x7a7e4, 0xe3a03001}, {0x7a7e8, 0xe58432bc},
    {0x79e4c, 0xe5942108}, {0x79e50, 0xe59432ac}, {0x79e54, 0xe2822a01}, {0x79e58, 0xe2833001},
    {0x79e5c, 0xe5922010}, {0x79e60, 0xe1530002},
};

/* Override of the slicer's wait after retract: with the dynamic wait active on a layer, the slicer's wait is ignored,
 * even when it is shorter than what the dynamic wait decides: the wait lasts until the resin has settled (its parameters). A too short
 * slicer wait would otherwise end the wait before. Safety ceiling = 15 min (was 10 min), so a force that
 * never settles cannot stall the printer for ever. <csvdir>/WAITOV "ON", command "waitov ON". The wait must still be running when our
 * thread sees it (20 Hz). */
#define WOV_FILE "WAITOV"
#define WOV_CEIL_MS 9.0e5               /* user: 10 min;: 15 min */
static double wov_ceil_ms(void)
{
    return WOV_CEIL_MS;                  /* always (the old WAITNOMAX test switch is gone; the Wait Times Testing has its own sequencer without a maximum) */
}
static struct { int on; } wov = {0};
/* second minimum wait (changed: layers 1 .. N use wset.min, layers AFTER N use MIN2); N = 0: MIN2 for
 * the whole print (wset.min is then unused: a "wait" command / snapshot restore with N = 0 sets MIN2 to the same value).
 * Own file <csvdir>/WAIT2 "MIN2 N", command "waitmin2 MIN2 N"; not part of the settings snapshots. */
#define WMIN2_FILE "WAIT2"
/* user: two independent sets. SET 1 = card values (change wset.grad over wset.win) for layers 1 .. ng, SET 2 = (grad2 over win2) for the layers after ng;
 * ng = 0: set 2 for the whole print. The MINIMUM wait has its own N (n): min up to layer n, min2 after it. */
static struct { double min2; int n; double grad2, win2; int ng; } wm2 = {1.0, 0, 1.0, 1.0, 0};


static int w_ok;                       /* code verified */
static void sgexpo_step(uintptr_t P, uint64_t now_ms);   /* sgexpo.c */
static void sgexpo_idle(uint64_t now_ms);
static void sgexpo_init(void);
static void sgzmon_step(uintptr_t P, uint64_t now_ms);   /* sgzmon.c */
static void sgzmon_idle(uint64_t now_ms);
static void sgzmon_init(void);
static void sgmx_init(void);                             /* sgmx.c (stand-in in sgnoaa.c) */
static volatile pid_t w_pid;           /* process running the wait thread */
static struct { int dw, wh, db, wt; double grad, win, min; } wset = {0, 0, 1, 0, 1.0, 1.0, 1.0};
static double wait_grad_for(int layer0)
{
    return layer0 + 1 > wm2.ng ? wm2.grad2 : wset.grad;      /* layers 1 .. ng: the change value of the card (wset.grad), after ng: grad2; ng = 0: grad2 for the whole print */
}
static double wait_win_for(int layer0)
{
    return layer0 + 1 > wm2.ng ? wm2.win2 : wset.win;        /* the same for the time window */
}
static double wait_min_for(int layer0)
{
    return layer0 + 1 > wm2.n ? wm2.min2 : wset.min;   /* user: layers 1 .. N (1-based) use wset.min, layers after N use min2; N = 0: min2 for the whole print */
}
typedef struct { int have, dw, wh, db, wt, au, hp, tpre, tprint, tilt, slow, ui, ui_i, ui_p, ext, gs_on, gs_au, gs_off, ov, beep, ext2, zo, t2, sen, jl, ext3, dl[5], ext4, tcon;
                double grad, win, min, tv[13], sv[6]; long t; } wsnap_t;
static wsnap_t wsaved;                 /* the Standard snapshot <csvdir>/SETTINGS (live.json "snap"); named profiles are read when used */

/* the wait in progress (wait thread only, except the copy in wstat) */
static struct {
    int active, layer, bottom, dyn, fixed, handsoff, heat, heating, released, settled;
    uint64_t t0;                       /* app wait start, ms */
    uint64_t us0;                      /* the same moment on our clock (now_us) */
    float orig, extra, written;
    double max_ms, settle_ms, target_ms, off_ms, slicer_ms;
    int ovd;                           /* the override was applied to this wait */
} wc;
static uint64_t w_last_t0, w_last_end; /* app ms */
static int w_last_layer = -1;
static int w_bot = W_BOTDEF, w_bot_file;  /* bottom layers of this job, 1 = from the file */
static double w_prev_s = -1;           /* last wait (settle time if settled), s */

/* shared with the writer thread (live.json, CSV): under wlock */
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int active, layer, bottom, fixed, heating;
    double el, max, grad, diff;        /* s, s, g/s (grad < 0 = not known yet), diff = |mean of the last window - mean of the window before| in g (< 0 = not known yet) */
    int n, ns, nm, nh;                 /* layers, settled, reached max, heated (this job) */
    double saved;                      /* s */
    struct { int layer; float used; int end, heated; } last[W_LAST];
    int nlast;
    int csv_pending;                   /* one CSV row carries the finished wait */
    float csv_used;
    int csv_end, csv_heated;
    int tare_pending, tare_layer;      /* tare in the wait: new zero for sgweb.c */
    double tare_raw;
} wstat;

static uint64_t mono_raw_ms(void)      /* the app's clock (0xd2db0) */
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static int valid_wait(double grad, double win, double min)
{
    return grad >= 0.05 && grad <= 20.0 && win >= 0.6 && win <= 600.0 && min >= 1.0;      /* no upper limit on the minimum wait */
}

/* up to max numbers separated by spaces, then the end of the line: count, -1 = junk */
static int parse_nums(const char *p, double *v, int max)
{
    int n = 0;
    for (; n < max; n++) {
        while (*p == ' ')
            p++;
        if ((*p < '0' || *p > '9') && *p != '.')
            break;
        v[n] = parse_k(p);
        while ((*p >= '0' && *p <= '9') || *p == '.')
            p++;
    }
    while (*p == ' ' || *p == '\n' || *p == '\r')
        p++;
    return *p ? -1 : n;
}

static int is01(double v)
{
    return v == 0 || v == 1;
}

/* "DW WH GRAD WIN MIN [DB [WT]]" (DB missing = 1, WT = 0): 1 = parsed and valid */
static int parse_wait(const char *p, int *dw, int *wh, int *db, int *wt, double *grad,
                      double *win, double *min)
{
    double v[7] = {0, 0, 0, 0, 0, 1, 0};
    int n = parse_nums(p, v, 7);
    if (n < 5)
        return 0;
    *dw = (int)v[0];
    *wh = (int)v[1];
    *db = (int)v[5];
    *wt = (int)v[6];
    *grad = v[2];
    *win = v[3];
    *min = v[4];
    return is01(v[0]) && is01(v[1]) && is01(v[5]) && is01(v[6]) && valid_wait(*grad, *win, *min);
}

/* write a small file atomically (tmp + fsync + rename): 1 = ok */
static int write_small(const char *path, const char *line, int n)
{
    char tmp[176];
    int fd;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    mkdir(sg_csvdir, 0755);
    fd = real_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return 0;
    if (real_write(fd, line, (size_t)n) != n) {
        real_close(fd);
        return 0;
    }
    fsync(fd);
    real_close(fd);
    return rename(tmp, path) == 0;
}

/* a snapshot file (one line, 12-48 numbers) into *o; o->have = 0 if it is missing or not valid */
static void load_snap(const char *path, wsnap_t *o)
{
    char buf[384];
    double v[48];
    int n;
    o->have = 0;
    if (read_small(path, buf, sizeof buf) <= 0 ||
        ((n = parse_nums(buf, v, 48)) != 12 && n != 25 && n != 33 && n != 38 && n != 42 && n != 47 && n != 48) ||
        !is01(v[0]) || !is01(v[1]) || !is01(v[5]) || !is01(v[6]) || !is01(v[7]) ||
        !valid_wait(v[2], v[3], v[4]))
        return;
    o->dw = (int)v[0];
    o->wh = (int)v[1];
    o->grad = v[2];
    o->win = v[3];
    o->min = v[4];
    o->db = (int)v[5];
    o->wt = (int)v[6];
    o->au = (int)v[7];
    o->hp = (int)v[8];             /* 9 = heating not available when saved */
    o->tpre = (int)v[9];
    o->tprint = (int)v[10];
    o->t = (long)v[11];
    o->tilt = n >= 25 && tp_valid(v + 12);   /* + tilt N, A..Y */
    if (o->tilt)
        memcpy(o->tv, v + 12, sizeof o->tv);
    o->slow = n >= 33 && sl_valid(v + 25);    /* + slow set A..F, UI sleep */
    if (o->slow)
        memcpy(o->sv, v + 25, sizeof o->sv);
    o->ui = n >= 33 && u_valid(v[31], v[32]);
    o->ui_i = (int)v[31];
    o->ui_p = (int)v[32];
    o->ext = n >= 38 && is01(v[33]) && is01(v[34]) && v[35] >= 0 && v[35] < 1000 && v[35] == (int)v[35] &&
                 is01(v[36]) && is01(v[37]);   /* + gauge sync ON AUTO OFFSET, wait override, beep */
    if (o->ext) {
        o->gs_on = (int)v[33];
        o->gs_au = (int)v[34];
        o->gs_off = (int)v[35];
        o->ov = (int)v[36];
        o->beep = (int)v[37];
    }
    o->ext2 = o->ext && n >= 42 && is01(v[38]) && is01(v[39]) && is01(v[40]) && is01(v[41]);   /* + plate move, two-stage fix, slow layers on, job log on */
    if (o->ext2) {
        o->zo = (int)v[38];
        o->t2 = (int)v[39];
        o->sen = (int)v[40];
        o->jl = (int)v[41];
    }
    o->ext3 = o->ext2 && n >= 47 && dl_valid(v + 42);   /* + Dynamic lift ON LIMIT LOWER SLOWER FASTER */
    if (o->ext3)
        for (int i = 0; i < 5; i++)
            o->dl[i] = (int)v[42 + i];
    o->ext4 = o->ext3 && n == 48 && is01(v[47]);       /* + custom tilt speeds switch (the tilt values are the custom profile) */
    o->tcon = o->ext4 ? (int)v[47] : 0;
    o->have = 1;
}

static void load_saved(void)
{
    char path[160];
    snprintf(path, sizeof path, "%s/SETTINGS", sg_csvdir);
    load_snap(path, &wsaved);
}

static void sgwait_load(void)
{
    char path[160], buf[64];
    int dw, wh, db, wt;
    double g, w, m;
    snprintf(path, sizeof path, "%s/WAIT", sg_csvdir);
    if (read_small(path, buf, sizeof buf) > 0 && parse_wait(buf, &dw, &wh, &db, &wt, &g, &w, &m)) {
        wset.dw = dw;
        wset.wh = wh;
        wset.db = db;
        wset.wt = wt;
        wset.grad = g;
        wset.win = w;
        wset.min = m;
    }
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, WOV_FILE);
    wov.on = 1;                        /* the slicer wait is always ignored while the dynamic wait is active (no switch; file WAITOV unused) */
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, WMIN2_FILE);
    wm2.min2 = wset.min;               /* no WAIT2 file: the one minimum of the file WAIT for the whole print */
    wm2.grad2 = wset.grad;             /* ... and the one change value / window */
    wm2.win2 = wset.win;
    wm2.ng = wm2.n;
    if (read_small(path, buf, sizeof buf) > 0) {
        double v[5] = {1, 0, wset.grad, wset.win, 0};
        int c = parse_nums(buf, v, 5);
        if (c >= 2 && c <= 5 && v[0] >= 1 && v[1] >= 0 && v[1] <= 99999 && v[2] >= 0.05 && v[2] <= 20.0 && v[3] >= 0.6 && v[3] <= 600.0 && v[4] >= 0 && v[4] <= 99999) {
            wm2.min2 = v[0];
            wm2.n = (int)v[1];
            wm2.grad2 = v[2];
            wm2.win2 = v[3];
            wm2.ng = c >= 5 ? (int)v[4] : wm2.n;     /* an older 2 / 3 field file: the same N for everything */
        }
    }
    load_saved();
}

/* "waitmin2 MIN2 N [GRAD2 [WIN2 [NG]]]" (missing values stay as they are) */
static void sgwait_min2_cmd(const char *c, char *res, size_t len)
{
    char path[160], line[64];
    double v[5] = {1, 0, wm2.grad2, wm2.win2, wm2.ng};
    int n, cnt;
    if (!w_ok) {
        snprintf(res, len, "error: dynamic wait not available (unknown app version or disabled)");
        return;
    }
    cnt = parse_nums(c, v, 5);
    if (cnt < 2 || cnt > 5 || v[0] < 1 || v[1] < 0 || v[1] > 99999 || v[1] != (double)(int)v[1] || v[2] < 0.05 || v[2] > 20.0 || v[3] < 0.6 || v[3] > 600.0 ||
        v[4] < 0 || v[4] > 99999 || v[4] != (double)(int)v[4]) {
        snprintf(res, len, "error: bad waitmin2 command");
        return;
    }
    wm2.min2 = v[0];
    wm2.n = (int)v[1];
    wm2.grad2 = v[2];
    wm2.win2 = v[3];
    wm2.ng = (int)v[4];
    n = snprintf(line, sizeof line, "%.2f %d %.2f %.2f %d\n", wm2.min2, wm2.n, wm2.grad2, wm2.win2, wm2.ng);
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, WMIN2_FILE);
    if (!write_small(path, line, n))
        snprintf(res, len, "error: set, but saving %s failed", path);
}

/* "waitov 0|1" */
static void sgwait_ov_cmd(const char *c, char *res, size_t len)
{
    char path[160], line[8];
    int on, n;
    if (!w_ok) {
        snprintf(res, len, "error: dynamic wait not available (unknown app version or disabled)");
        return;
    }
    if (!parse_int(&c, &on) || on > 1) {
        snprintf(res, len, "error: bad waitov command");
        return;
    }
    wov.on = 1;                        /* kept for old snapshots / pages: the value is ignored, the override is always on */
    (void)on;
    n = snprintf(line, sizeof line, "%d\n", on);
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, WOV_FILE);
    if (!write_small(path, line, n))
        snprintf(res, len, "error: set, but saving %s failed", path);
}

/* set + save the wait settings: 1 = ok, else res = the error */
static int wait_apply(int dw, int wh, int db, int wt, double g, double w, double m, char *res,
                      size_t len)
{
    char path[160], line[64];
    int n;
    if (wh && !h_ok) {
        snprintf(res, len, "error: heating not available");
        return 0;
    }
    wset.dw = dw;                      /* the wait thread reads these at each wait start */
    wset.wh = wh;
    wset.db = db;
    wset.wt = wt;
    wset.grad = g;
    wset.win = w;
    wset.min = m;
    if (wm2.n == 0 && wm2.min2 != m) {      /* one minimum for the whole print: it stays the minimum */
        char l2[64], p2[160];
        wm2.min2 = m;
        snprintf(p2, sizeof p2, "%s/%s", sg_csvdir, WMIN2_FILE);
        write_small(p2, l2, snprintf(l2, sizeof l2, "%.2f %d %.2f %.2f %d\n", wm2.min2, wm2.n, wm2.grad2, wm2.win2, wm2.ng));
    }
    n = snprintf(line, sizeof line, "%d %d %.2f %.2f %.2f %d %d\n", dw, wh, g, w, m, db, wt);
    snprintf(path, sizeof path, "%s/WAIT", sg_csvdir);
    if (!write_small(path, line, n)) {
        snprintf(res, len, "error: set, but saving %s failed", path);
        return 0;
    }
    return 1;
}

static void sgwait_cmd(const char *c, char *res, size_t len)
{
    int dw, wh, db, wt;
    double g, w, m;
    if (!w_ok) {
        snprintf(res, len, "error: dynamic wait not available (unknown app version or disabled)");
        return;
    }
    if (!parse_wait(c, &dw, &wh, &db, &wt, &g, &w, &m)) {
        snprintf(res, len, "error: bad wait command");
        return;
    }
    wait_apply(dw, wh, db, wt, g, w, m, res, len);
}

/* the profile name after "save" / "restore": 0 = none (the Standard snapshot SETTINGS), 1 = valid (name, file path), -1 = bad.
 * Letters, digits, space, - _ . ; 1-32 characters; first and last not a space; "Standard" is the unnamed one (page rule). */
static int prof_name(const char *p, char *name, size_t nlen, char *path, size_t plen)
{
    size_t i = 0;
    while (*p == ' ')
        p++;
    for (; *p && *p != '\n' && *p != '\r'; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == ' ' || *p == '-' || *p == '_' || *p == '.') ||
            i + 1 >= nlen || i >= 32)
            return -1;
        name[i++] = *p;
    }
    while (i && name[i - 1] == ' ')
        i--;
    name[i] = 0;
    if (!i)
        return 0;
    if (name[0] == '.')
        return -1;
    snprintf(path, plen, "%s/profiles/%s.txt", sg_csvdir, name);
    return 1;
}

/* "save [NAME]" | "restore [NAME]" | "defaults": the wait, tare, auto-tare and heating settings; NAME = a profile in <csvdir>/profiles */
static void sgset_cmd(const char *c, char *res, size_t len)
{
    char line[384], path[200], h[48], pname[40];
    int named;
    wsnap_t ws;
    int n, dw, wh, db, wt, au, hp, tpre, tprint, tilt, slow, ui, ui_i, ui_p;
    int ext, gs_on = 0, gs_au = 1, gs_off = 0, ov, beep, ext2, zo = 0, t2 = 0, sen = 1, jl = 1, ext3 = 0, dlv[5] = {0, 2000, 1500, 20, 5}, tcon = 0, ext4_defaults = 0;
    double g, w, m, tv[13], sv[6];
    if (!w_ok) {
        snprintf(res, len, "error: settings not available (unknown app version or disabled)");
        return;
    }
    snprintf(path, sizeof path, "%s/SETTINGS", sg_csvdir);
    named = 0;
    pname[0] = 0;
    if (strncmp(c, "save", 4) == 0 || strncmp(c, "restore", 7) == 0) {
        named = prof_name(c + (c[0] == 's' ? 4 : 7), pname, sizeof pname, path, sizeof path);
        if (named < 0) {
            snprintf(res, len, "error: bad profile name");
            return;
        }
        if (named) {
            char dir[176];
            snprintf(dir, sizeof dir, "%s/profiles", sg_csvdir);
            mkdir(dir, 0755);
        }
    }
    if (strncmp(c, "save", 4) == 0) {
        n = snprintf(line, sizeof line, "%d %d %.2f %.2f %.2f %d %d %d %d %d %d %ld\n", wset.dw,
                     wset.wh, wset.grad, wset.win, wset.min, wset.db, wset.wt, w_auto,
                     h_ok ? HB(H_UI + 0x14) : 9, h_ok ? (int)HI(H_MACH + 0x1a0) : 0,
                     h_ok ? (int)HI(H_MACH + 0x198) : 0, (long)time(NULL));
        if (sgtilt_tc_get(tv, &tcon) && tp_valid(tv)) {   /* the custom tilt profile (Tilt speed card), if in the card's ranges */
            n--;                                /* over the newline */
            for (int i = 0; i < 13 && n < (int)sizeof line - 8; i++)
                n += snprintf(line + n, sizeof line - (size_t)n, " %d", (int)tv[i]);
            sgtilt_slow_get(sv);       /* + slow set, UI sleep */
            for (int i = 0; i < 6 && n < (int)sizeof line - 8; i++)
                n += snprintf(line + n, sizeof line - (size_t)n, " %d", (int)sv[i]);
            n += snprintf(line + n, sizeof line - (size_t)n, " %d %d", u_idle, u_print);
            sgtilt_gsync_get(&gs_on, &gs_au, &gs_off);      /* + gauge sync, wait override, beep */
            n += snprintf(line + n, sizeof line - (size_t)n, " %d %d %d %d %d", gs_on, gs_au, gs_off, wov.on, sgtilt_beep_get());
            n += snprintf(line + n, sizeof line - (size_t)n, " %d %d %d %d", h_ok ? z_opt : 0, sgtilt_fix_get(), sl_enabled(), joblog_on());
            sgtilt_dl_get(dlv);                             /* + Dynamic lift */
            n += snprintf(line + n, sizeof line - (size_t)n, " %d %d %d %d %d %d\n", dlv[0], dlv[1], dlv[2], dlv[3], dlv[4], tcon);
        }
        if (!write_small(path, line, n))
            snprintf(res, len, "error: saving %s failed", path);
        if (!named)
            load_saved();
        return;
    }
    if (strncmp(c, "restore", 7) == 0) {
        if (named) {
            load_snap(path, &ws);
        } else {
            load_saved();
            ws = wsaved;
        }
        if (!ws.have) {
            if (named)
                snprintf(res, len, "error: profile '%s' not found or not valid", pname);
            else
                snprintf(res, len, "error: no saved settings on the printer");
            return;
        }
        dw = ws.dw; wh = ws.wh; db = ws.db; wt = ws.wt; au = ws.au;
        g = ws.grad; w = ws.win; m = ws.min;
        hp = ws.hp; tpre = ws.tpre; tprint = ws.tprint;
        tilt = ws.tilt;
        memcpy(tv, ws.tv, sizeof tv);
        slow = ws.slow;
        memcpy(sv, ws.sv, sizeof sv);
        ui = ws.ui; ui_i = ws.ui_i; ui_p = ws.ui_p;
        ext = ws.ext; gs_on = ws.gs_on; gs_au = ws.gs_au; gs_off = ws.gs_off; ov = ws.ov; beep = ws.beep;
        ext2 = ws.ext2; zo = ws.zo; t2 = ws.t2; sen = ws.sen; jl = ws.jl;
        ext3 = ws.ext3;
        for (int i = 0; i < 5; i++)
            dlv[i] = ws.dl[i];
        tcon = ws.tcon;
        if (!ws.ext4 && ws.tilt) {          /* an older snapshot: custom speeds on when the saved profile is not the stock one */
            double st[13];
            sgtilt_stock(st);
            tcon = 0;
            for (int i = 0; i < 13; i++)
                if (tv[i] != st[i])
                    tcon = 1;
        }
    } else if (strncmp(c, "defaults", 8) == 0) {
        dw = 0; wh = 0; db = 1; wt = 0; au = 1;
        g = 1.0; w = 1.0; m = 1.0;
        hp = 1; tpre = 30; tprint = 40;
        tilt = 0;                      /* the stored custom values stay: only the switch goes off (below) */
        sgtilt_stock(tv);
        slow = 1;
        memcpy(sv, sl_def, sizeof sv);
        ui = 1; ui_i = 50; ui_p = 200;
        ext = 1; gs_on = 1; gs_au = 1; gs_off = 0; ov = 0; beep = 0;   /* gauge sync on (default since ), auto search on, override off, beep off */
        ext2 = 1; zo = 0; t2 = 0; sen = 1; jl = 1;                     /* plate move off, two-stage fix off (stock), slow layers on, job log on */
        ext3 = 1;                                                       /* Dynamic lift off, 2000 g / 1500 g / 20 % / 5 % */
        tcon = 0;                                                       /* custom tilt speeds off: the stock profile applies */
        ext4_defaults = 1;
    } else {
        snprintf(res, len, "error: bad settings command");
        return;
    }
    if (wh && !h_ok)
        wh = 0;
    if (!wait_apply(dw, wh, db, wt, g, w, m, res, len))
        return;
    set_auto(au);
    if (h_ok && (hp == 0 || hp == 1)) {      /* pre-heat now: left as it is */
        if (ext2)                      /* the 5th number = the plate move option; the pre-heat state is unchanged: no plate move */
            snprintf(h, sizeof h, "%d %d %d %d %d", HB(H_UI + 0x15), tpre, hp, tprint, zo);
        else
            snprintf(h, sizeof h, "%d %d %d %d", HB(H_UI + 0x15), tpre, hp, tprint);
        z_quiet = 1;                   /* a restore never moves the plate */
        sgheat_cmd(h, res, len);
        z_quiet = 0;
    }
    if (ext2 && t2)                    /* the fix first: stage angles and the sync need it */
        sgtilt_fix_cmd("1", res, len);
    if (tilt && tp_ok)                 /* the custom profile + switch; applied at the next safe moment (sgtilt.c) */
        sgtilt_tc_set(tv, tcon);
    else if (!tilt && ext4_defaults)   /* defaults: custom speeds off, the stored values stay */
        sgtilt_mode_cmd("0", res, len);
    if (slow && tp_ok) {
        double s2[6];
        memcpy(s2, sv, sizeof s2);
        if (!sl_apply_set(s2, res, len)) {     /* stage angles need the fix: then one stage */
            s2[4] = s2[5] = 0;
            sl_apply_set(s2, res, len);
        }
    }
    if (ui && u_ok)
        u_apply(ui_i, ui_p, res, len);
    if (ext) {                         /* build 34: wait override, gauge sync, beep (an error only for what is not available) */
        char b[32];
        snprintf(b, sizeof b, "%d", ov);
        sgwait_ov_cmd(b, res, len);
        snprintf(b, sizeof b, "%d %d %d", gs_on, gs_au, gs_off);
        sgtilt_gsync_cmd(b, res, len);
        snprintf(b, sizeof b, "%d", beep);
        sgtilt_beep_cmd(b, res, len);
    }
    if (ext2) {                        /* build 34: slow layers, job log, then the fix off (refused while a stage angle is live) */
        sgtilt_slowen_cmd(sen ? "1" : "0", res, len);
        joblog_set(jl);
        if (ext3) {                    /* Dynamic lift before the fix can go off (it needs it) */
            char b[48];
            snprintf(b, sizeof b, "%d %d %d %d %d", dlv[0], dlv[1], dlv[2], dlv[3], dlv[4]);
            sgtilt_dl_cmd(b, res, len);
        }
    }
}

/* wait thread -> sgweb.c: a new zero from the tare in the wait (1 = take it) */
static int sgwait_take_tare(double *raw, int *layer)
{
    int r;
    pthread_mutex_lock(&wlock);
    r = wstat.tare_pending;
    *raw = wstat.tare_raw;
    *layer = wstat.tare_layer;
    wstat.tare_pending = 0;
    pthread_mutex_unlock(&wlock);
    return r;
}

static int sgwait_wt(void)
{
    return w_ok && wset.wt;
}

static uint64_t read_u64(uintptr_t a)  /* the app writes it as two words: read until stable */
{
    uint64_t v, w;
    do {
        v = HW(a) | (uint64_t)HW(a + 4) << 32;
        w = HW(a) | (uint64_t)HW(a + 4) << 32;
    } while (v != w);
    return v;
}

/* the app's heap ranges ([heap] lines of /proc/self/maps) */
static int heap_ranges(uintptr_t lo[], uintptr_t hi[], int max)
{
    char buf[4096], *l, *e;
    int n = 0, len = read_small("/proc/self/maps", buf, sizeof buf);
    for (l = buf; len > 0 && n < max && (e = strchr(l, '\n')); l = e + 1) {
        uintptr_t a = 0, b = 0;
        const char *p = l;
        *e = 0;
        if (!strstr(l, "[heap]"))
            continue;
        for (; *p && *p != '-'; p++)
            a = a * 16 + (uintptr_t)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        for (p++; *p && *p != ' '; p++)
            b = b * 16 + (uintptr_t)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        if (a && b > a) {
            lo[n] = a;
            hi[n++] = b;
        }
    }
    return n;
}

/* the print file's bottom layer count: every CTB settings block on the heap that matches the
 * app's own header (resolution, layer count, height); all matches must agree. 1 = found */
static int find_bottom(uintptr_t P, int *count)
{
    uintptr_t lo[4], hi[4], H = HW(P + W_HDR);
    int n, found = -1;
    if (!H || (H & 3))
        return 0;
    n = heap_ranges(lo, hi, 4);
    if (!n || H < lo[0])
        return 0;
    for (int r = 0; r < n; r++)
        for (uintptr_t c = lo[r]; c + 0x44 <= hi[r]; c += 4) {
            int b;
            if (HW(c) != 0xcafebabeu || HW(c + 4) != 0 || HW(c + 0x38) != HW(H + 0x1004) ||
                HW(c + 0x3c) != HW(H + 0x1008) || HW(c + 0x40) != HW(H + 0x1010) ||
                HW(c + 0x20) != HW(H + 0x1028))
                continue;
            b = HI(c + 0x34);
            if (b < 0 || b > 1000 || (found >= 0 && b != found))
                return 0;
            found = b;
        }
    if (found < 0)
        return 0;
    *count = found;
    return 1;
}

static int heater_on(void)
{
    if (h_ok && HW(H_DEV + 0x114) == 0) {
        HW(H_DEV + 0x114) = 2;         /* = set_heater_state(2), the app's print pulse */
        return 1;
    }
    return 0;
}

/* THERMOSTAT of a heated wait (during the wait test the vat kept rising above the target): the app's heater mode 2 (pulse, what Wait & heat
 * sets) has no temperature control, so a long wait heated without end. While a wait is heated we hold the "heating during print" target (live +0x198):
 * the heater goes OFF (mode 0) at TARGET - 0.5 C; for the next 30 s it stays off (the heat still flowing in lifts the vat by about 0.9 C after the
 * switch-off); after that it goes ON again (mode 2) as soon as the vat is at or below TARGET - 1.0 C. Called at 20 Hz while the wait is heated;
 * *off_t = the time (us) WE switched it off, 0 = we did not. A stop of the wait (heater_off / wt_heater_off) still sets mode 0 only if it is 2. */
#define W_GOV_OFF_BELOW 0.5          /* off at target - this */
#define W_GOV_ON_BELOW  1.0          /* on again at target - this or lower ... */
#define W_GOV_HOLD_US   30000000ull  /* ... but not before this long after the switch-off */
/* PRE-HEAT during a heated wait (wait test with the dashboard Pre-heat on: the app's own pre-heat thermostat (mode 3, its target = pre-heat
 * target live +0x1a0, band +/- I7 R / T) kept cycling the heater through the waits, the vat rose to 42.2 C with a 40 C target and the gauge drifted 1-2 g per
 * 30 s, so the settle rule never passed). A wait that starts while the heater mode is 3 is governed like a pulse wait: gov_mode = 3, the same thermostat
 * (off at the pre-heat target - 0.5, 30 s hold, back to mode 3 at - 1.0, where the app's own thermostat takes over again), and at the end of the wait
 * heat_release puts mode 3 back. gov_mode is 2 for the print's pulse. */
static int gov_mode = 2;
static void heat_govern(uint64_t *off_t)
{
    double c;
    int tgt = HI(H_MACH + (gov_mode == 3 ? 0x1a0 : 0x198));
    if (!h_ok || tgt < 20 || tgt > 60 || !sgheat_temp(&c))
        return;
    if (!*off_t && c >= (double)tgt - W_GOV_OFF_BELOW && (int)HW(H_DEV + 0x114) == gov_mode) {
        HW(H_DEV + 0x114) = 0;
        *off_t = now_us();
    } else if (*off_t && now_us() - *off_t >= W_GOV_HOLD_US && c <= (double)tgt - W_GOV_ON_BELOW && HW(H_DEV + 0x114) == 0) {
        HW(H_DEV + 0x114) = (unsigned)gov_mode;
        *off_t = 0;
    }
}

/* the end of a governed wait: the pre-heat the thermostat switched off goes back on (the app's own pre-heat control continues); the print pulse just ends */
static void heat_release(void)
{
    if (gov_mode == 3 && h_ok && HW(H_DEV + 0x114) == 0 && HB(H_UI + 0x15))
        HW(H_DEV + 0x114) = 3;
    gov_mode = 2;
}

static uint64_t w_gov_off;             /* us: the dynamic wait's heater was switched off by the thermostat (0 = not) */

/* the heater of a heated wait at its start (it switches on only at or below TARGET - 1.0 C): 1 = the wait is heated. A vat that is warmer
 * than that does not get the heater at once: the thermostat takes over in the "off for 30 s already" state and switches it on when the vat has cooled
 * to TARGET - 1.0 C. *off_t = the thermostat's switch-off time (set here). Without a working sensor / target the heater is switched on as before. */
static int heat_start(uint64_t *off_t)
{
    double c;
    int tgt = HI(H_MACH + 0x198);
    *off_t = 0;
    gov_mode = 2;
    if (h_ok && HW(H_DEV + 0x114) == 3 && HB(H_UI + 0x15)) {      /* the app's pre-heat is on: the thermostat takes it over for this wait */
        gov_mode = 3;
        return 1;
    }
    if (h_ok && HW(H_DEV + 0x114) == 0 && tgt >= 20 && tgt <= 60 && sgheat_temp(&c) && c > (double)tgt - W_GOV_ON_BELOW) {
        *off_t = now_us() - W_GOV_HOLD_US - 1;
        return 1;
    }
    return heater_on();
}

static void heater_off(double el)
{
    w_gov_off = 0;
    heat_release();
    if (!wc.heating)
        return;
    if (HW(H_DEV + 0x114) == 2)
        HW(H_DEV + 0x114) = 0;
    wc.heating = 0;
    wc.off_ms = el;
}

/* |gradient| in g/s of the readings of the last win s inside the wait (and their mean raw);
 * -1 = too few */
static double settle_grad(double win, double *mean_raw)
{
    uint64_t now = now_us(), from = now - (uint64_t)(win * 1e6);
    double st = 0, sv = 0, stt = 0, stv = 0, sr = 0, k = w_k < 0 ? -w_k : w_k, g;
    int n = 0;
    if (from < wc.us0)
        return -1;
    pthread_mutex_lock(&glock);
    for (unsigned i = 0; i < HIST && i < hpos; i++) {
        unsigned j = (hpos - 1 - i) % HIST;
        double t, v;
        if (hist[j].ts < from)
            break;
        t = (hist[j].ts - from) / 1e6;
        v = hist[j].raw / k;
        st += t; sv += v; stt += t * t; stv += t * v; sr += hist[j].raw;
        n++;
    }
    pthread_mutex_unlock(&glock);
    if (n < 3 || n * stt - st * st <= 0)
        return -1;
    *mean_raw = sr / n;
    g = (n * stv - st * sv) / (n * stt - st * st);
    return g < 0 ? -g : g;
}

/* the MEASURED difference in g between the mean of the last win s and the mean of the win s before (the number the wait test logs as diff_g); -1 = the two windows
 * do not fit into the wait yet or hold too few readings. Display only: the rule above still uses the slope of the last window. */
static double settle_diff(double win)
{
    uint64_t now = now_us(), b1 = now - (uint64_t)(win * 1e6), b0 = now - (uint64_t)(2 * win * 1e6);
    double s1 = 0, s0 = 0, k = w_k < 0 ? -w_k : w_k, d;
    int n1 = 0, n0 = 0, need = (int)(3.0 * win);
    if (b0 < wc.us0 || k <= 0)
        return -1;
    pthread_mutex_lock(&glock);
    for (unsigned i = 0; i < HIST && i < hpos; i++) {
        unsigned j = (hpos - 1 - i) % HIST;
        if (hist[j].ts < b0)
            break;
        if (hist[j].ts >= b1) { s1 += hist[j].raw; n1++; }
        else { s0 += hist[j].raw; n0++; }
    }
    pthread_mutex_unlock(&glock);
    if (n1 < (need > 3 ? need : 3) || n0 < (need > 3 ? need : 3))
        return -1;
    d = (s1 / n1 - s0 / n0) / k;
    return d < 0 ? -d : d;
}

static void begin_wait(uintptr_t P, uint64_t t0, uint64_t now_ms)
{
    int layer = HI(P + W_LAYER);
    /* a new job: the layer went down, or no wait for 10 min -> fresh statistics */
    if (layer < w_last_layer - 1 || !w_last_end || now_ms - w_last_end > 600000) {
        pthread_mutex_lock(&wlock);
        wstat.n = wstat.ns = wstat.nm = wstat.nh = wstat.nlast = 0;
        wstat.saved = 0;
        pthread_mutex_unlock(&wlock);
        w_prev_s = -1;
        w_bot_file = 0;
    }
    if (!w_bot_file) {                 /* until found: each wait tries again */
        w_bot = W_BOTDEF;
        w_bot_file = find_bottom(P, &w_bot);
    }
    w_last_layer = layer;
    sgtilt_slow_wait(layer);           /* slow layers (8.20): before this layer's tilt moves */
    memset(&wc, 0, sizeof wc);
    wc.active = 1;
    wc.t0 = t0;
    wc.us0 = now_us() - (now_ms > t0 ? (now_ms - t0) * 1000u : 0);
    wc.layer = layer;
    wc.bottom = wc.layer < w_bot;
    int off = 0;
    wc.orig = HF(P + W_WAIT);
    wc.extra = HF(P + W_EXTRA);
    wc.max_ms = (double)wc.orig + wc.extra;
    /* sanity: a plausible wait (< 1 h) and the layer the app log shows */
    if (!(wc.orig >= 0 && wc.orig < 3.6e6f && wc.extra >= 0 && wc.extra < 3.6e6f) ||
        (s_active && s_layer >= 0 && (wc.layer < s_layer - 1 || wc.layer > s_layer + 1)))
        off = 1, wc.max_ms = 0;                    /* hands off this wait */
    wc.slicer_ms = (double)wc.orig + wc.extra;
    if (!off && wov.on && wset.dw && (!wc.bottom || wset.db)) {
        double ceil_ms = wov_ceil_ms();
        float v = (float)(ceil_ms - wc.extra);
        HF(P + W_WAIT) = v;            /* the slicer wait is ignored: the wait runs until it has settled (or the ceiling) */
        wc.written = v;                /* finish_wait puts the slicer value back */
        wc.max_ms = ceil_ms;
        wc.ovd = 1;
    }
    wc.handsoff = off;
    wc.dyn = !off && wset.dw && (!wc.bottom || wset.db) && wc.max_ms > wait_min_for(wc.layer) * 1000;
    wc.fixed = !wc.dyn;                /* the app's own wait runs to its end */
    if (!off && wset.wh && h_ok && HB(H_UI + 0x14) && HW(H_DEV) && HB(H_DEV + 0x118)) {
        double prev = w_prev_s >= 0 ? w_prev_s : wc.max_ms / 1000;
        if (wc.dyn ? prev >= W_HEATMIN : wc.max_ms > W_HEATMIN * 1000)
            wc.heat = 1;
    }
    if (wc.heat && wc.max_ms > W_GAP_MS + W_POLL_MS)
        wc.heating = heat_start(&w_gov_off);
    wc.heat = wc.heating;              /* = this wait was heated */
    pthread_mutex_lock(&wlock);
    wstat.active = 1;
    wstat.layer = wc.layer;
    wstat.bottom = wc.bottom;
    wstat.fixed = wc.fixed;
    wstat.heating = wc.heating;
    wstat.el = 0;
    wstat.max = (wov.on && wc.slicer_ms > 0 ? wc.slicer_ms : wc.max_ms) / 1000;   /* the chart scale: the slicer's wait */
    wstat.grad = -1;
    wstat.diff = -1;
    pthread_mutex_unlock(&wlock);
}

static void step_wait(uintptr_t P, uint64_t now_ms)
{
    if (wc.ovd && !wov.on) {           /* the override was switched off during the wait: the slicer wait applies again */
        HF(P + W_WAIT) = wc.orig;
        wc.written = 0;
        wc.ovd = 0;
        wc.max_ms = wc.slicer_ms;
        wc.dyn = wc.dyn && wc.max_ms > wait_min_for(wc.layer) * 1000;
    }
    double el = now_ms > wc.t0 ? (double)(now_ms - wc.t0) : 0, g = -1, mean = 0;
    int watch = wc.dyn || (wset.wt && !wc.handsoff);  /* the settle test is needed */
    if (wc.heating && !wc.settled)
        heat_govern(&w_gov_off);
    if (wc.heating && el >= wc.max_ms - W_GAP_MS - W_POLL_MS)  /* slicer wait about to end */
        heater_off(el);
    double win_eff = wait_win_for(wc.layer);     /* the set (change + window) of this layer */
    if (watch && !wc.settled) {
        double dm = settle_diff(win_eff);
        pthread_mutex_lock(&wlock);
        wstat.diff = dm;
        pthread_mutex_unlock(&wlock);
    }
    if (watch && !wc.settled && el >= win_eff * 1000) {
        g = settle_grad(win_eff, &mean);
        if (g >= 0 && g * win_eff < wait_grad_for(wc.layer) && el >= wait_min_for(wc.layer) * 1000) {   /* force change over the window (g) = slope * window */
            wc.settled = 1;
            wc.settle_ms = el;
            if (wset.wt) {             /* tare in the wait: new zero at the settled level */
                pthread_mutex_lock(&wlock);
                wstat.tare_pending = 1;
                wstat.tare_raw = mean;
                wstat.tare_layer = wc.layer;
                pthread_mutex_unlock(&wlock);
            }
            if (wc.dyn) {
                heater_off(el);
                wc.target_ms = el;
                if (wc.heat && wc.target_ms < wc.off_ms + W_REL_MS)
                    wc.target_ms = wc.off_ms + W_REL_MS;  /* UV >= 1 s after the heater off */
            }
        }
    }
    if (wc.dyn && wc.settled && !wc.released && wc.target_ms < wc.max_ms) {
        float v = (float)(wc.target_ms - wc.extra);
        if (v < 0)
            v = 0;
        if (v < (wc.written != 0 ? wc.written : wc.orig)) {   /* shorter than what is in force (slicer, or the override) */
            HF(P + W_WAIT) = v;        /* aligned float store: the app's next pass sees it */
            wc.written = v;
        }
        wc.released = 1;
    }
    pthread_mutex_lock(&wlock);
    wstat.el = el / 1000;
    wstat.heating = wc.heating;
    if (g >= 0)
        wstat.grad = g;
    pthread_mutex_unlock(&wlock);
}

static void finish_wait(uintptr_t P, uint64_t now_ms)
{
    double used = now_ms > wc.t0 ? (now_ms - wc.t0) / 1000.0 : 0;
    int end = !wc.dyn ? 2 : wc.settled ? 0 : 1, heated = wc.heat;
    heater_off(used * 1000);
    if (P && wc.written != 0 && HF(P + W_WAIT) == wc.written)
        HF(P + W_WAIT) = wc.orig;      /* the slicer value back for the rest of the app */
    if (wc.max_ms > 0)
        w_prev_s = wc.dyn && wc.settled ? wc.settle_ms / 1000 : used;
    pthread_mutex_lock(&wlock);
    wstat.active = 0;
    wstat.heating = 0;
    if (wc.dyn) {
        wstat.n++;
        wstat.ns += end == 0;
        wstat.nm += end == 1;
        if ((wc.written != 0 && wc.slicer_ms > 0 ? wc.slicer_ms : wc.max_ms) / 1000 > used)
            wstat.saved += (wc.written != 0 && wc.slicer_ms > 0 ? wc.slicer_ms : wc.max_ms) / 1000 - used;
    }
    wstat.nh += heated;
    if (wstat.nlast == W_LAST) {
        memmove(wstat.last, wstat.last + 1, sizeof wstat.last[0] * (W_LAST - 1));
        wstat.nlast--;
    }
    wstat.last[wstat.nlast].layer = wc.layer;
    wstat.last[wstat.nlast].used = (float)used;
    wstat.last[wstat.nlast].end = end;
    wstat.last[wstat.nlast].heated = heated;
    wstat.nlast++;
    wstat.csv_pending = 1;
    wstat.csv_used = (float)used;
    wstat.csv_end = end;
    wstat.csv_heated = heated;
    pthread_mutex_unlock(&wlock);
    w_last_end = now_ms;
    wc.active = 0;
}

/* ---- TIMELAPSE trigger: in each wait the app starts (plate at rest after the retract) one datagram "T <layer> <job base>" goes to
 * sgcam (127.0.0.1:8092), which takes one camera picture in a low-priority child process. Nothing here waits for it: a non-blocking sendto.
 * The switch (file <csvdir>/TIMELAPSE, command "tl on|off") is read once at the start of a job (the first wait with a new job log). Pictures only while
 * the plate is above TL_MIN_Z (machine coordinate: the user wants the timelapse to start from Z > 40 mm). */
#define TL_FLAG  "TIMELAPSE"
#define TL_MIN_Z 40.0
#define TL_PORT  8092
static double zm_now_mm(int *ok);
static char tl_job[256];
static int tl_on, tl_sock = -1;
static double tl_minz = TL_MIN_Z;
/* WHEN the trigger goes out (the picture was not synchronised with the end of the retract). The camera needs about TL_LAG_MS from the
 * trigger to a picture (RTSP session + the encoder's next key frame, measured 2.26 s after PLAY), so a trigger sent when the wait starts gives a picture
 * from the exposure. The time from the END OF THE EXPOSURE to the start of the next wait (lift + peel + retract) is mechanical and repeats from layer to
 * layer: it is measured every cycle and the next trigger is sent (that time + TL_AFTER_MS - lag) after the next exposure end, so the key frame lands
 * TL_AFTER_MS after the wait started. The first cycle of a job has no measurement: its trigger goes out when the wait starts (late, as before).
 * <csvdir>/TLLAG = lag in ms (tuning without a rebuild, read at job start). */
#define TL_LAG_MS   2300
#define TL_AFTER_MS 400
static long tl_lag = TL_LAG_MS;
static uint64_t tl_ee;            /* app ms of the last exposure end (0 = none) */
static long tl_D = -1;            /* ms from an exposure end to the next wait start, measured in the last cycle (-1 = unknown) */
static uint64_t tl_fire;          /* app ms the scheduled trigger goes out (0 = none) */
static uint64_t tl_ideal;         /* app ms the picture should show */
static int tl_fire_layer, tl_sent, tl_ex_prev;
/* Z anchor (Z moves once per layer, about one layer thickness, whatever the tilt speeds): the first tick the Z position differs from the
 * exposure's Z is "Z step seen". The time from there to the next wait start is measured too; when it is long enough for the camera lag the trigger is
 * scheduled from the Z step (the release tilt, the part dynamic lift changes, is then not in the interval), else from the exposure end. Both intervals
 * go to /tmp/sgtl.log, one line per wait. */
static double tl_zrest;
static int tl_zok, tl_use_z;
static long tl_Dz = -1;
static uint64_t tl_zs;

static void sgtl_trigger(int layer, long lead_ms)
{
    char flag[160], msg[300];
    const char *b;
    size_t bl;
    int ok = 0, n;
    double z;
    if (!s_active || !txt_name[0])
        return;
    if (strcmp(tl_job, txt_name) != 0) {
        snprintf(tl_job, sizeof tl_job, "%s", txt_name);
        snprintf(flag, sizeof flag, "%s/%s", sg_csvdir, TL_FLAG);
        tl_on = access(flag, F_OK) == 0;
        /* optional <csvdir>/TLMINZ = whole mm (e.g. -1000 = from layer 0, for tests); no file = TL_MIN_Z */
        tl_minz = TL_MIN_Z;
        snprintf(flag, sizeof flag, "%s/TLMINZ", sg_csvdir);
        {
            int fd = real_open(flag, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                char buf[16];
                ssize_t r = read(fd, buf, sizeof buf - 1);
                real_close(fd);
                if (r > 0) {
                    int i = 0, neg = 0, v = 0, dig = 0;
                    buf[r] = 0;
                    if (buf[i] == '-') { neg = 1; i++; }
                    for (; buf[i] >= '0' && buf[i] <= '9'; i++) { v = v * 10 + (buf[i] - '0'); dig++; }
                    if (dig)
                        tl_minz = neg ? -v : v;
                }
            }
            tl_lag = TL_LAG_MS;
            snprintf(flag, sizeof flag, "%s/TLLAG", sg_csvdir);
            fd = real_open(flag, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                char buf[16];
                ssize_t r = read(fd, buf, sizeof buf - 1);
                real_close(fd);
                if (r > 0) {
                    int i = 0, v = 0, dig = 0;
                    buf[r] = 0;
                    for (; buf[i] >= '0' && buf[i] <= '9'; i++) { v = v * 10 + (buf[i] - '0'); dig++; }
                    if (dig && dig <= 6)
                        tl_lag = v;
                }
            }
        }
    }
    if (!tl_on)
        return;
    z = zm_now_mm(&ok);
    if (!ok || z <= tl_minz)
        return;
    b = strrchr(txt_name, '/');
    b = b ? b + 1 : txt_name;
    bl = strlen(b);
    if (bl > 4 && strcmp(b + bl - 4, ".txt") == 0)
        bl -= 4;
    n = snprintf(msg, sizeof msg, "T %d %ld %.*s", layer, lead_ms > 0 ? lead_ms : 0, (int)bl, b);
    if (tl_sock < 0)
        tl_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (tl_sock >= 0) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(TL_PORT);
        a.sin_addr.s_addr = htonl(0x7f000001u);
        (void)sendto(tl_sock, msg, (size_t)n, MSG_DONTWAIT, (struct sockaddr *)&a, sizeof a);
    }
}

/* "tl on|off" */
static void sgtl_cmd(const char *c, char *res, size_t len)
{
    char path[160];
    int fd;
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, TL_FLAG);
    if (strncmp(c, "on", 2) == 0) {
        if ((fd = real_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644)) >= 0)
            real_close(fd);
        else
            snprintf(res, len, "error: timelapse flag not written");
    } else if (strncmp(c, "off", 3) == 0) {
        unlink(path);
    } else {
        snprintf(res, len, "error: bad tl command");
    }
}

static void sgwtest_poll(void);

static void *wait_thread(void *arg)
{
    struct timespec next;
    (void)arg;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        uintptr_t P = HW(W_PPTR);
        uint64_t now_ms = mono_raw_ms();
        if (!P || (P & 3)) {
            tl_ee = tl_fire = tl_zs = 0;
            tl_D = tl_Dz = -1;
            if (wc.active)
                finish_wait(0, now_ms);
            sgexpo_idle(now_ms);
            sgzmon_idle(now_ms);
        } else {
            int st = HB(P + W_STATE);
            uint64_t t0 = read_u64(P + W_START);
            if (wc.active && (st != 2 || t0 != wc.t0))
                finish_wait(P, now_ms);
            /* only a wait the app has just started (we look 20 times a second) */
            {
                int ex = HB(P + W_EXPO);                         /* end of an exposure: schedule the next timelapse trigger */
                if (tl_ex_prev == 1 && ex == 0) {
                    tl_ee = now_ms;
                    tl_fire = 0;
                    tl_sent = 0;
                    tl_zs = 0;
                    {
                        int zk = 0;
                        tl_zrest = zm_now_mm(&zk);
                        tl_zok = zk;
                    }
                    tl_use_z = tl_on && tl_zok && tl_Dz > 0 && tl_Dz + TL_AFTER_MS >= tl_lag;
                    if (tl_on && !tl_use_z && tl_D > 0) {
                        long lead = tl_D + TL_AFTER_MS;          /* ms from the exposure end to the moment the picture should show */
                        tl_ideal = now_ms + (uint64_t)lead;
                        tl_fire = tl_ideal > (uint64_t)tl_lag + now_ms ? tl_ideal - (uint64_t)tl_lag : now_ms;
                        tl_fire_layer = s_layer >= 0 ? s_layer + 1 : 0;
                        tl_sent = 0;
                    }
                }
                tl_ex_prev = ex;
                if (tl_ee && !tl_zs && tl_zok) {                 /* Z step seen */
                    int zk = 0;
                    double z = zm_now_mm(&zk);
                    if (zk && (z > tl_zrest + 0.003 || z < tl_zrest - 0.003)) {
                        tl_zs = now_ms;
                        if (tl_use_z) {
                            tl_ideal = now_ms + (uint64_t)(tl_Dz + TL_AFTER_MS);
                            tl_fire = tl_ideal > (uint64_t)tl_lag + now_ms ? tl_ideal - (uint64_t)tl_lag : now_ms;
                            tl_fire_layer = s_layer >= 0 ? s_layer + 1 : 0;
                        }
                    }
                }
                if (tl_fire && now_ms >= tl_fire) {
                    tl_fire = 0;
                    tl_sent = 1;
                    sgtl_trigger(tl_fire_layer, tl_ideal > now_ms ? (long)(tl_ideal - now_ms) : 0);
                }
            }
            if (!wc.active && st == 2 && t0 != w_last_t0 && t0 <= now_ms && now_ms - t0 < 3000) {
                w_last_t0 = t0;
                begin_wait(P, t0, now_ms);
                if (tl_ee && t0 > tl_ee && t0 - tl_ee < 120000)
                    tl_D = (long)(t0 - tl_ee);                   /* this cycle's exposure end -> wait start, used for the next trigger */
                else
                    tl_D = -1;
                tl_Dz = (tl_zs && t0 > tl_zs && t0 - tl_zs < 120000) ? (long)(t0 - tl_zs) : -1;
                {
                    char ln[160];
                    int fd, n = snprintf(ln, sizeof ln, "layer %d exp_end->wait %ld  exp_end->Zstep %ld  Zstep->wait %ld  anchor %s  trigger %s\n", s_layer,
                                         tl_D, tl_zs && tl_ee ? (long)(tl_zs - tl_ee) : -1L, tl_Dz, tl_use_z ? "Z" : "exp_end", tl_sent ? "early" : "late");
                    if (tl_on && (fd = real_open("/tmp/sgtl.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)) >= 0) {
                        (void)!write(fd, ln, (size_t)n);
                        real_close(fd);
                    }
                }
                tl_ee = 0;
                tl_zs = 0;
                if (!tl_sent)                                    /* nothing was scheduled (first cycle, or no measurement yet): late, at the wait start */
                    sgtl_trigger(s_layer, 0);
                tl_sent = 0;
                tl_fire = 0;
            }
            if (wc.active)
                step_wait(P, now_ms);
            sgexpo_step(P, now_ms);
            sgzmon_step(P, now_ms);
        }
        sgwtest_poll();                           /* Wait Times Testing sequencer (sgwtest.c), 20 Hz */
        next.tv_nsec += 50000000;                 /* 20 Hz */
        if (next.tv_nsec >= 1000000000) {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
            ;
    }
    return NULL;
}

static void sgwait_init(void)
{
    if (!in_chitu || access("/customer/mod/sgwait_disable", F_OK) == 0)
        return;
    for (unsigned i = 0; i < sizeof w_sig / sizeof w_sig[0]; i++)
        if (HW(w_sig[i].a) != w_sig[i].w)
            return;
    w_ok = 1;
    sgexpo_init();
    sgzmon_init();
    sgmx_init();
}

/* writer thread, every 0.5 s: settings + the wait thread (once per process) */
static void sgwait_tick(int reload)
{
    pid_t me = getpid();
    if (!w_ok)
        return;
    if (reload)
        sgwait_load();
    if (w_pid != me) {
        pthread_t th;
        pthread_attr_t at;
        w_pid = me;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 64 * 1024);
        if (pthread_create(&th, &at, wait_thread, NULL) != 0)
            w_ok = 0;
        pthread_attr_destroy(&at);
    }
}

/* ",wait_s,wait_end,heat_w" for the CSV row: filled once per finished wait */
static void sgwait_csv(char *buf, size_t len)
{
    static const char *const ends[] = {"settled", "max", "off"};
    pthread_mutex_lock(&wlock);
    if (wstat.csv_pending)
        snprintf(buf, len, ",%.2f,%s,%d", wstat.csv_used, ends[wstat.csv_end], wstat.csv_heated);
    else
        snprintf(buf, len, ",,,");
    wstat.csv_pending = 0;
    pthread_mutex_unlock(&wlock);
}

/* "wait":{...} or "wait":null for live.json */
static int sgwait_json(char *buf, size_t len)
{
    int n;
    if (!w_ok)
        return snprintf(buf, len, ",\"wait\":null");
    pthread_mutex_lock(&wlock);
    n = snprintf(buf, len, ",\"wait\":{\"dw\":%d,\"wh\":%d,\"grad\":%.2f,\"win\":%.2f,\"min\":%.2f,\"min2\":%.2f,\"min2n\":%d,\"grad2\":%.2f,\"win2\":%.2f,\"ng\":%d,"
                 "\"db\":%d,\"wt\":%d,\"ov\":%d,\"pf\":1,\"heat_ok\":%d,\"botn\":%d,\"botf\":%d,\"active\":%d,\"layer\":%d,\"bottom\":%d,\"fixed\":%d,\"heating\":%d,"
                 "\"el\":%.2f,\"max\":%.2f,\"g\":%.2f,\"d\":%.2f,\"prev\":%.2f,\"n\":%d,\"ns\":%d,\"nm\":%d,"
                 "\"nh\":%d,\"saved\":%.1f,\"last\":[",
                 wset.dw, wset.wh, wset.grad, wset.win, wset.min, wm2.min2, wm2.n, wm2.grad2, wm2.win2, wm2.ng, wset.db, wset.wt, wov.on, h_ok, w_bot, w_bot_file, wstat.active,
                 wstat.layer, wstat.bottom, wstat.fixed, wstat.heating, wstat.el, wstat.max, wstat.grad, wstat.diff,
                 w_prev_s, wstat.n, wstat.ns, wstat.nm, wstat.nh, wstat.saved);
    for (int i = 0; i < wstat.nlast && n < (int)len; i++)
        n += snprintf(buf + n, len - (size_t)n, "[%d,%.2f,%d,%d]%s", wstat.last[i].layer,
                      wstat.last[i].used, wstat.last[i].end, wstat.last[i].heated,
                      i + 1 < wstat.nlast ? "," : "");
    pthread_mutex_unlock(&wlock);
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "]");
    if (n < (int)len && wsaved.have) {
        n += snprintf(buf + n, len - (size_t)n, ",\"snap\":{\"t\":%ld,\"dw\":%d,\"wh\":%d,"
                      "\"grad\":%.2f,\"win\":%.2f,\"min\":%.2f,\"db\":%d,\"wt\":%d,\"auto\":%d,"
                      "\"hp\":%d,\"tpre\":%d,\"tprint\":%d", wsaved.t, wsaved.dw, wsaved.wh,
                      wsaved.grad, wsaved.win, wsaved.min, wsaved.db, wsaved.wt, wsaved.au,
                      wsaved.hp, wsaved.tpre, wsaved.tprint);
        if (wsaved.tilt)               /* "tilt":[N, A..Y] */
            for (int i = 0; i < 13 && n < (int)len; i++)
                n += snprintf(buf + n, len - (size_t)n, "%s%d", i ? "," : ",\"tilt\":[",
                              (int)wsaved.tv[i]);
        if (n < (int)len && wsaved.tilt)
            n += snprintf(buf + n, len - (size_t)n, "]");
        if (n < (int)len && wsaved.slow)
            n += snprintf(buf + n, len - (size_t)n, ",\"slow\":[%d,%d,%d,%d,%d,%d]", (int)wsaved.sv[0],
                          (int)wsaved.sv[1], (int)wsaved.sv[2], (int)wsaved.sv[3], (int)wsaved.sv[4],
                          (int)wsaved.sv[5]);
        if (n < (int)len && wsaved.ui)
            n += snprintf(buf + n, len - (size_t)n, ",\"ui\":[%d,%d]", wsaved.ui_i, wsaved.ui_p);
        if (n < (int)len && wsaved.ext)
            n += snprintf(buf + n, len - (size_t)n, ",\"gs\":[%d,%d,%d],\"ov\":%d,\"beep\":%d", wsaved.gs_on, wsaved.gs_au,
                          wsaved.gs_off, wsaved.ov, wsaved.beep);
        if (n < (int)len && wsaved.ext3)
            n += snprintf(buf + n, len - (size_t)n, ",\"dl\":[%d,%d,%d,%d,%d]", wsaved.dl[0], wsaved.dl[1], wsaved.dl[2], wsaved.dl[3], wsaved.dl[4]);
        if (n < (int)len && wsaved.ext2)
            n += snprintf(buf + n, len - (size_t)n, ",\"zo\":%d,\"t2\":%d,\"sen\":%d,\"jl\":%d", wsaved.zo, wsaved.t2, wsaved.sen, wsaved.jl);
        if (n < (int)len && wsaved.ext4)
            n += snprintf(buf + n, len - (size_t)n, ",\"tcs\":%d", wsaved.tcon);
        if (n < (int)len)
            n += snprintf(buf + n, len - (size_t)n, "}");
    } else if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, ",\"snap\":null");
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "}");
    return n;
}
