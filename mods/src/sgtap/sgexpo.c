/*
 * sgexpo.c - exposure times: planned (file) vs used per layer, and a manual override of the next layers.
 * #included by sgtap.c after sgwait.c; run from sgwait.c's 20 Hz wait thread (sgexpo_step / sgexpo_idle). See docs/exposure_edit.txt.
 *
 * The app (V1.5.9, print struct P = *(0x2eb6d8)) loads the layer block from the file at the start of every layer (P+0x24c =
 * exposure_time, float ms) and ends the exposure when now - P+0x338 > P+0x24c (0x7d17c..0x7d1a4, re-read on every pass) as long as the
 * word at 0x39fe78+0xa70 is -1 (otherwise the app takes its "cur p_time" sub-step path, which is left alone). Observed live:
 * a write in the wait (P+0x30b == 2) changed that layer's exposure from 4504 to 5008 ms; the next layer was loaded from the file again.
 * So: at the start of each wait (the block is loaded then) the file value is read, the override for the layer's class is written
 * (only while waiting, never during the exposure: a value below the elapsed time would end the UV at once), and the exposure is timed
 * from P+0x30c (1 = exposing) to log "used".
 *   Class of a layer as the app tests it: layer < P+0x110 bottom; < P+0x110 + u16 P+0x114 transition; else normal.
 *   Override = absolute ms for bottom and / or normal layers (command "expo B N", "-" = the file's value, "expo off"); it applies from
 *   the next wait and ends with the job (not saved, not in the Settings snapshot). Transition layers (only while an override is active):
 *   the file's graded value is mapped from [file bottom, file normal] onto [bottom now, normal now]; UNTESTED, needs a job with transition
 *   layers (and the file's own bottom / normal values from the heap settings block, trusted only when its bottom value matches the one
 *   observed on a bottom layer).
 * Data: live.json "expo", CSV columns expo_file,expo_set,expo_used (s, once per finished exposure). Kill switch /customer/mod/sgexpo_disable.
 */
#define E_EXPO     0x24c
#define E_FLAG     0x30c
#define E_START    0x338
#define E_BOT      0x110
#define E_TRANS    0x114
#define E_MODE     (0x39fe78u + 0xa70u)  /* -1: the exposure ends on P+0x24c */
#define E_MAXMS    60000.0
#define E_RUNS     40

static const struct { uint32_t a, w; } e_sig[] = {
    {0x7d038, 0xe30f6e78}, {0x7d03c, 0xe3406039}, {0x7d040, 0xe5963a70}, {0x7d044, 0xe3730001}, {0x7d048, 0x0a00004b},
    {0x7d17c, 0xeb01570b}, {0x7d180, 0xe2843fce}, {0x7d184, 0xe1c320d0}, {0x7d188, 0xe0500002}, {0x7d18c, 0xe0c11003},
    {0x7d190, 0xebfe6b81}, {0x7d194, 0xedd47a93}, {0x7d198, 0xee070a10}, {0x7d19c, 0xeeb47ae7}, {0x7d1a0, 0xeef1fa10},
    {0x7d1a4, 0xdaffff93},
};

static int e_ok;                                      /* code verified */
static pthread_mutex_t elock = PTHREAD_MUTEX_INITIALIZER;
static struct { int bon, non; float b, n; } eset;      /* the override in force (ms), under elock */
static struct {                                        /* shown / logged, under elock */
    int job, mode_ok, layer, bot, trans, total, heap_ok;
    float fb, fn, hb, hn;                              /* file values seen (ms, <0 = not known): observed / from the heap settings */
    float cf, cs;                                      /* the current layer: file value, value set (<0 = none) */
    int inexp, nruns;
    struct { int l0, l1; float file, set; double used; int n; } runs[E_RUNS];
    int csv_pending;
    float csv_file, csv_set, csv_used;
} estat;

/* the wait thread's own state */
static struct {
    uint64_t wait_t0;
    int last_layer;
    float last_orig, last_written;
    int in, layer;
    uint64_t t0;
    float file, set;
    int idle_n;
} ex;


/* ---- RESIN PROFILES: a named table "layer thickness (um) -> bottom / normal exposure (ms)" in
 * <csvdir>/resins/NAME.txt, one line "um bottom_ms normal_ms" each. The selected name is in <csvdir>/RESIN (command "resin NAME|off").
 * In e_latch, when no Manual Exposure override is set, the thickness of the layer (its target Z P+0x220 minus the one of the layer
 * before; the first layer: minus the Z zero) picks the nearest row (within 1 um) and its time is written like a manual override; a layer
 * without a row keeps the file's time and is counted in rp.miss. Variable layer heights are followed layer by layer. Ends with the job. */
#define RP_ROWS 16
#define RP_MISS 8
static int sgmx_active(void);                         /* sgmx.c (stand-in in sgnoaa.c) */
static int zm_zero_mm(double *zero);                  /* sgzmon.c */
static struct {
    char name[40];                                    /* selected profile ("" = none) */
    int n, err;                                       /* rows loaded, 1 = file missing / empty */
    struct { int um; float b, n; } row[RP_ROWS];
    int cnt[RP_ROWS];                                 /* layers of this job per row */
    struct { int um, cnt; } miss[RP_MISS];
    int nmiss;
    int th, rowi;                                     /* the current layer: thickness (um, 0 = unknown) and row (-1 none) */
    int counted;                                      /* last layer counted */
    int cur_layer, prev_layer;                        /* thickness tracking: layer numbers latched */
    double cur_zt, prev_zt;
} rp = {.cur_layer = -1, .prev_layer = -1, .counted = -1, .rowi = -1};

static int rp_name_ok(const char *n)
{
    size_t l = strlen(n);
    if (l < 1 || l > 32 || n[0] == '.')
        return 0;
    for (size_t i = 0; i < l; i++)
        if (!((n[i] >= 'A' && n[i] <= 'Z') || (n[i] >= 'a' && n[i] <= 'z') || (n[i] >= '0' && n[i] <= '9') ||
              n[i] == '_' || n[i] == '.' || n[i] == '-' || n[i] == ' '))
            return 0;
    return 1;
}

static void rp_load(void)                             /* elock held or at init */
{
    char path[200], buf[1024];
    int fd;
    ssize_t r;
    rp.n = 0;
    rp.err = 0;
    if (!rp.name[0])
        return;
    snprintf(path, sizeof path, "%s/resins/%s.txt", sg_csvdir, rp.name);
    fd = real_open(path, O_RDONLY | O_CLOEXEC);
    r = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0)
        real_close(fd);
    if (r <= 0) {
        rp.err = 1;
        return;
    }
    buf[r] = 0;
    for (const char *q = buf; *q && rp.n < RP_ROWS;) {
        long v[3] = {0, 0, 0};
        int k = 0;
        while (*q && *q != '\n') {
            if (*q >= '0' && *q <= '9') {
                long x = 0;
                while (*q >= '0' && *q <= '9' && x < 10000000)
                    x = x * 10 + (*q++ - '0');
                if (k < 3)
                    v[k] = x;
                k++;
            } else {
                q++;
            }
        }
        if (*q == '\n')
            q++;
        if (k == 3 && v[0] >= 1 && v[0] <= 2000 && v[1] <= (long)E_MAXMS && v[2] <= (long)E_MAXMS) {
            rp.row[rp.n].um = (int)v[0];
            rp.row[rp.n].b = (float)v[1];
            rp.row[rp.n].n = (float)v[2];
            rp.n++;
        }
    }
    if (!rp.n)
        rp.err = 1;
}

static void rp_reset_job(void)                        /* elock held */
{
    memset(rp.cnt, 0, sizeof rp.cnt);
    rp.nmiss = 0;
    rp.th = 0;
    rp.rowi = -1;
    rp.counted = -1;
    rp.cur_layer = rp.prev_layer = -1;
}

static void rp_track(uintptr_t P, int layer)          /* thickness of this layer into rp.th */
{
    double zt = HF(P + 0x220), th = 0, zero = 0;
    if (layer != rp.cur_layer) {                      /* a new layer (a resumed wait of the same layer keeps the values) */
        rp.prev_layer = rp.cur_layer;
        rp.prev_zt = rp.cur_zt;
        rp.cur_layer = layer;
        rp.cur_zt = zt;
    }
    if (zt > 0 && rp.prev_layer == layer - 1 && rp.prev_zt > 0)
        th = zt - rp.prev_zt;
    else if (zt > 0 && layer == 0 && zm_zero_mm(&zero))
        th = zt - zero;
    rp.th = th > 0.001 && th < 1.0 ? (int)(th * 1000.0 + 0.5) : 0;
}

static void rp_pick(void)                             /* rp.th -> rp.rowi, counters */
{
    int best = -1, bd = 2;
    for (int i = 0; i < rp.n; i++) {
        int d = rp.row[i].um > rp.th ? rp.row[i].um - rp.th : rp.th - rp.row[i].um;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    rp.rowi = rp.th ? best : -1;
    if (rp.counted == rp.cur_layer || !rp.th)
        return;
    rp.counted = rp.cur_layer;
    if (best >= 0) {
        rp.cnt[best]++;
        return;
    }
    for (int i = 0; i < rp.nmiss; i++)
        if (rp.miss[i].um == rp.th) {
            rp.miss[i].cnt++;
            return;
        }
    if (rp.nmiss < RP_MISS) {
        rp.miss[rp.nmiss].um = rp.th;
        rp.miss[rp.nmiss].cnt = 1;
        rp.nmiss++;
    }
}

static void e_reset_job(void)                         /* elock held */
{
    rp_reset_job();
    memset(&eset, 0, sizeof eset);
    estat.job = estat.layer = 0;
    estat.layer = -1;
    estat.bot = estat.trans = estat.total = estat.heap_ok = 0;
    estat.fb = estat.fn = estat.hb = estat.hn = estat.cf = estat.cs = -1;
    estat.inexp = estat.nruns = 0;
    estat.mode_ok = 1;
}

/* the print file's own exposure (+0x28) and bottom exposure (+0x2c) from the CTB settings block on the heap, s -> ms; checked like
 * sgwait.c find_bottom (same block, same match against the app's header). 1 = found */
static int e_find_file(uintptr_t P, float *expo_ms, float *bexpo_ms, int *layers)
{
    uintptr_t lo[4], hi[4], H = HW(P + W_HDR);
    int n, found = 0;
    float e = 0, b = 0;
    int nl = 0;
    if (!H || (H & 3))
        return 0;
    n = heap_ranges(lo, hi, 4);
    if (!n || H < lo[0])
        return 0;
    for (int r = 0; r < n; r++)
        for (uintptr_t c = lo[r]; c + 0x44 <= hi[r]; c += 4) {
            float e1, b1;
            if (HW(c) != 0xcafebabeu || HW(c + 4) != 0 || HW(c + 0x38) != HW(H + 0x1004) ||
                HW(c + 0x3c) != HW(H + 0x1008) || HW(c + 0x40) != HW(H + 0x1010) ||
                HW(c + 0x20) != HW(H + 0x1028))
                continue;
            e1 = HF(c + 0x28) * 1000.0f;
            b1 = HF(c + 0x2c) * 1000.0f;
            if (!(e1 >= 0 && e1 < 3.6e6f && b1 >= 0 && b1 < 3.6e6f))
                return 0;
            if (found && (e1 != e || b1 != b))
                return 0;
            e = e1;
            b = b1;
            nl = (int)HW(c + 0x40);
            found = 1;
        }
    if (!found)
        return 0;
    *expo_ms = e;
    *bexpo_ms = b;
    *layers = nl;
    return 1;
}

static void e_push_run(int layer, float file, float set, double used_ms)       /* elock held */
{
    int i = estat.nruns - 1;
    if (i >= 0 && estat.runs[i].file == file && estat.runs[i].set == set && estat.runs[i].l1 + 1 == layer) {
        estat.runs[i].l1 = layer;
        estat.runs[i].used += used_ms;
        estat.runs[i].n++;
        return;
    }
    if (estat.nruns == E_RUNS) {
        memmove(estat.runs, estat.runs + 1, sizeof estat.runs[0] * (E_RUNS - 1));
        estat.nruns--;
    }
    i = estat.nruns++;
    estat.runs[i].l0 = estat.runs[i].l1 = layer;
    estat.runs[i].file = file;
    estat.runs[i].set = set;
    estat.runs[i].used = used_ms;
    estat.runs[i].n = 1;
}

/* a wait has just started: read the file's value of this layer, work out and write the override */
static void e_latch(uintptr_t P)
{
    int layer = HI(P + W_LAYER), bot = HI(P + E_BOT), trans = (int)(HW(P + E_TRANS) & 0xffff), cls;
    float orig = HF(P + E_EXPO), target = -1;
    int mode_ok = HW(E_MODE) == 0xffffffffu;
    int bon, non;
    float b, n;
    if (!(orig >= 0 && orig < 3.6e6f) || bot < 0 || bot > 1000 || trans < 0 || trans > 1000)
        return;
    if (layer == ex.last_layer && orig == ex.last_written)    /* resumed wait of the same layer: our own value is not the file's */
        orig = ex.last_orig;
    ex.last_layer = layer;
    ex.last_orig = orig;
    cls = layer < bot ? 0 : layer < bot + trans ? 1 : 2;
    pthread_mutex_lock(&elock);
    if (!estat.job) {                                        /* first wait of a job */
        float he, hb;
        int nl;
        estat.job = 1;
        rp_load();                                           /* the profile file may have been edited since */
        if (e_find_file(P, &he, &hb, &nl)) {
            estat.hb = hb;
            estat.hn = he;
            estat.total = nl;
        }
    }
    estat.layer = layer;
    estat.bot = bot;
    estat.trans = trans;
    estat.mode_ok = mode_ok;
    if (cls == 0)
        estat.fb = orig;
    else if (cls == 2)
        estat.fn = orig;
    if (!estat.heap_ok && estat.hb >= 0 && estat.fb >= 0 && estat.hb - estat.fb < 1.5f && estat.fb - estat.hb < 1.5f)
        estat.heap_ok = 1;                                   /* the heap block agrees with what the app loaded for a bottom layer */
    bon = eset.bon;
    non = eset.non;
    b = eset.b;
    n = eset.n;
    rp.th = 0;
    rp.rowi = -1;
    if (rp.n > 0 && mode_ok) {
        rp_track(P, layer);
        if (!(bon || non))
            rp_pick();
    }
    if (mode_ok && !(bon || non) && rp.n > 0 && rp.rowi >= 0) {   /* resin profile row of this layer's thickness */
        float rb = rp.row[rp.rowi].b, rn = rp.row[rp.rowi].n;
        if (cls == 0)
            target = rb;
        else if (cls == 2)
            target = rn;
        else if (estat.heap_ok) {
            float fb = estat.hb, fn = estat.hn;
            if (fn - fb > 1.0f || fb - fn > 1.0f)
                target = rb + (orig - fb) * (rn - rb) / (fn - fb);
        }
        if (target > (float)E_MAXMS)
            target = (float)E_MAXMS;
        if (target < 0 && target != -1)
            target = 0;
    } else if (mode_ok && (bon || non)) {
        if (cls == 0 && bon)
            target = b;
        else if (cls == 2 && non)
            target = n;
        else if (cls == 1 && estat.heap_ok) {                /* transition: remap the file's grading */
            float fb = estat.hb, fn = estat.hn, nb = bon ? b : fb, nn = non ? n : fn;
            if (fn - fb > 1.0f || fb - fn > 1.0f)
                target = nb + (orig - fb) * (nn - nb) / (fn - fb);
        }
        if (target > (float)E_MAXMS)
            target = (float)E_MAXMS;
        if (target < 0 && target != -1)
            target = 0;
    }
    estat.cf = orig;
    estat.cs = target;
    pthread_mutex_unlock(&elock);
    ex.file = orig;
    ex.set = target;
    ex.last_written = target >= 0 ? target : orig;
}

static void e_close_exposure(uint64_t now_ms)
{
    double used = now_ms > ex.t0 ? (double)(now_ms - ex.t0) : 0;
    ex.in = 0;
    pthread_mutex_lock(&elock);
    estat.inexp = 0;
    e_push_run(ex.layer, ex.file, ex.set, used);
    estat.csv_pending = 1;
    estat.csv_file = ex.file;
    estat.csv_set = ex.set;
    estat.csv_used = (float)used;
    pthread_mutex_unlock(&elock);
}

/* every 50 ms from the wait thread while P is valid */
static void sgexpo_step(uintptr_t P, uint64_t now_ms)
{
    int st, flag, layer;
    uint64_t t0;
    if (!e_ok)
        return;
    ex.idle_n = 0;
    layer = HI(P + W_LAYER);
    if (ex.last_layer > 1 && layer < ex.last_layer - 1) {    /* a new job without P going to 0 */
        pthread_mutex_lock(&elock);
        e_reset_job();
        pthread_mutex_unlock(&elock);
        ex.last_layer = -1;
        ex.in = 0;
    }
    st = HB(P + W_STATE);
    flag = HB(P + E_FLAG);
    t0 = read_u64(P + W_START);
    if (st == 2 && t0 != ex.wait_t0) {
        ex.wait_t0 = t0;
        if (t0 <= now_ms && now_ms - t0 < 3000 && !flag) {   /* a wait the app has just started: its block is loaded */
            ex.file = -1;
            ex.set = -1;
            e_latch(P);
        }
    }
    if (st == 2 && !flag && ex.set >= 0 && ex.last_layer == layer && HF(P + E_EXPO) != ex.set)
        HF(P + E_EXPO) = ex.set;                             /* aligned float store; the app's next pass uses it */
    if (flag == 1 && !ex.in) {
        uint64_t t = read_u64(P + E_START);
        ex.in = 1;
        ex.layer = layer;
        ex.t0 = t <= now_ms && now_ms - t < 1500 ? t : now_ms;
        if (ex.last_layer != layer || ex.file < 0) {         /* the wait was missed: nothing was written, the app's value is the file's */
            ex.file = HF(P + E_EXPO);
            ex.set = -1;
        }
        pthread_mutex_lock(&elock);
        estat.inexp = 1;
        estat.cf = ex.file;
        estat.cs = ex.set;
        pthread_mutex_unlock(&elock);
    } else if (flag == 0 && ex.in) {
        e_close_exposure(now_ms);
    }
}

/* every 50 ms from the wait thread while P is 0 */
static void sgexpo_idle(uint64_t now_ms)
{
    if (!e_ok)
        return;
    if (ex.in)
        e_close_exposure(now_ms);
    if (++ex.idle_n == 60) {                                  /* 3 s without a print job: the override ends with the job */
        pthread_mutex_lock(&elock);
        if (estat.job)                                        /* a job was seen: it is over. Times set before any job wait for the next one */
            e_reset_job();
        pthread_mutex_unlock(&elock);
        ex.last_layer = -1;
        ex.wait_t0 = 0;
    }
}

static void sgexpo_init(void)
{
    if (!w_ok || access("/customer/mod/sgexpo_disable", F_OK) == 0)
        return;
    for (unsigned i = 0; i < sizeof e_sig / sizeof e_sig[0]; i++)
        if (HW(e_sig[i].a) != e_sig[i].w)
            return;
    pthread_mutex_lock(&elock);
    e_reset_job();
    {
        char path[160], nm[40];
        int fd;
        ssize_t r;
        snprintf(path, sizeof path, "%s/RESIN", sg_csvdir);
        fd = real_open(path, O_RDONLY | O_CLOEXEC);
        r = fd >= 0 ? read(fd, nm, sizeof nm - 1) : -1;
        if (fd >= 0)
            real_close(fd);
        if (r > 0) {
            nm[r] = 0;
            nm[strcspn(nm, "\r\n")] = 0;
            if (rp_name_ok(nm)) {
                snprintf(rp.name, sizeof rp.name, "%s", nm);
                rp_load();
            }
        }
    }
    pthread_mutex_unlock(&elock);
    ex.last_layer = -1;
    ex.file = ex.set = -1;
    e_ok = 1;
}

/* "expo off" | "expo B N" (ms 0-60000 each, "-" = the file's value) */
static void sgexpo_cmd(const char *c, char *res, size_t len)
{
    double v[2] = {-1, -1};
    int on[2] = {0, 0};
    if (!e_ok) {
        snprintf(res, len, "error: exposure times not available (unknown app version, disabled, or the library is not loaded yet)");
        return;
    }
    if (sgmx_active() && strncmp(c, "off", 3) != 0) {
        snprintf(res, len, "error: Multi Exposure is on: switch it off first");
        return;
    }
    /* no job needed: the times set while the printer is idle wait for the next job (they end with that job) */
    if (strncmp(c, "off", 3) != 0) {
        for (int i = 0; i < 2; i++) {
            while (*c == ' ')
                c++;
            if (*c == '-') {
                c++;
            } else if ((*c >= '0' && *c <= '9')) {
                v[i] = parse_k(c);
                while (*c >= '0' && *c <= '9')
                    c++;
                if (!(v[i] >= 0 && v[i] <= E_MAXMS)) {
                    snprintf(res, len, "error: exposure time out of range (0-60 s)");
                    return;
                }
                on[i] = 1;
            } else {
                snprintf(res, len, "error: bad expo command");
                return;
            }
        }
        while (*c == ' ' || *c == '\n' || *c == '\r')
            c++;
        if (*c) {
            snprintf(res, len, "error: bad expo command");
            return;
        }
    }
    pthread_mutex_lock(&elock);
    eset.bon = on[0];
    eset.non = on[1];
    eset.b = (float)v[0];
    eset.n = (float)v[1];
    pthread_mutex_unlock(&elock);
}

/* "resin NAME" | "resin off": select the resin profile (saved in <csvdir>/RESIN; the file is read again at each job start) */
static void sgexpo_resin_cmd(const char *c, char *res, size_t len)
{
    char name[40], path[160];
    size_t l;
    int fd;
    while (*c == ' ')
        c++;
    l = strcspn(c, "\r\n");
    if (l >= sizeof name) {
        snprintf(res, len, "error: bad profile name");
        return;
    }
    memcpy(name, c, l);
    name[l] = 0;
    if (!e_ok) {
        snprintf(res, len, "error: exposure times not available");
        return;
    }
    if (strcmp(name, "off") == 0)
        name[0] = 0;
    else if (!rp_name_ok(name)) {
        snprintf(res, len, "error: bad profile name");
        return;
    }
    if (name[0] && sgmx_active()) {
        snprintf(res, len, "error: Multi Exposure is on: switch it off first");
        return;
    }
    pthread_mutex_lock(&elock);
    snprintf(rp.name, sizeof rp.name, "%s", name);
    rp_load();
    pthread_mutex_unlock(&elock);
    if (name[0] && rp.err) {
        snprintf(res, len, "error: profile has no valid rows");
    }
    snprintf(path, sizeof path, "%s/RESIN", sg_csvdir);
    fd = real_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (name[0] && write(fd, name, strlen(name)) < 0) {}
        real_close(fd);
    }
}

/* ",expo_file,expo_set,expo_used" for the CSV row: once per finished exposure (s) */
static void sgexpo_csv(char *buf, size_t len)
{
    pthread_mutex_lock(&elock);
    if (estat.csv_pending) {
        char set[16] = "";
        if (estat.csv_set >= 0)
            snprintf(set, sizeof set, "%.3f", estat.csv_set / 1000.0);
        snprintf(buf, len, ",%.3f,%s,%.3f", estat.csv_file / 1000.0, set, estat.csv_used / 1000.0);
    } else {
        snprintf(buf, len, ",,,");
    }
    estat.csv_pending = 0;
    pthread_mutex_unlock(&elock);
}

static int sgexpo_json(char *buf, size_t len)
{
    int n;
    if (!e_ok)
        return snprintf(buf, len, ",\"expo\":null");
    pthread_mutex_lock(&elock);
    n = snprintf(buf, len, ",\"expo\":{\"job\":%d,\"mode\":%d,\"bon\":%d,\"b\":%.0f,\"non\":%d,\"n\":%.0f,\"fb\":%.0f,\"fn\":%.0f,"
                 "\"hb\":%.0f,\"hn\":%.0f,\"hok\":%d,\"bot\":%d,\"trans\":%d,\"total\":%d,\"layer\":%d,\"cf\":%.0f,\"cs\":%.0f,"
                 "\"inexp\":%d,\"runs\":[",
                 estat.job, estat.mode_ok, eset.bon, eset.b, eset.non, eset.n, estat.fb, estat.fn, estat.hb, estat.hn,
                 estat.heap_ok, estat.bot, estat.trans, estat.total, estat.layer, estat.cf, estat.cs, estat.inexp);
    for (int i = 0; i < estat.nruns && n < (int)len; i++)
        n += snprintf(buf + n, len - (size_t)n, "%s[%d,%d,%.0f,%.0f,%.0f]", i ? "," : "", estat.runs[i].l0, estat.runs[i].l1,
                      estat.runs[i].file, estat.runs[i].set, estat.runs[i].used / estat.runs[i].n);
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "],\"rp\":{\"name\":\"%s\",\"n\":%d,\"err\":%d,\"th\":%d,\"row\":%d,\"cnt\":[",
                      rp.name, rp.n, rp.err, rp.th, rp.rowi);
    for (int i = 0; i < rp.n && n < (int)len; i++)
        n += snprintf(buf + n, len - (size_t)n, "%s%d", i ? "," : "", rp.cnt[i]);
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "],\"miss\":[");
    for (int i = 0; i < rp.nmiss && n < (int)len; i++)
        n += snprintf(buf + n, len - (size_t)n, "%s[%d,%d]", i ? "," : "", rp.miss[i].um, rp.miss[i].cnt);
    pthread_mutex_unlock(&elock);
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "]}}");
    return n;
}
