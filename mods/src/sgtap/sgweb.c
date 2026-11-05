/*
 * sgweb.c - live feed for the dashboard hosted on the printer (step 8.6), #included by
 * sgtap.c after sglog.c. Runs in the writer thread: sgweb_tick every 0.5 s.
 *
 * - <livedir>/live.json (tmpfs; written to live.tmp, then renamed): the readings of the
 *   last ~4 s with sequence numbers, print session phase/layer/CSV, the zero reference,
 *   k, auto-tare state and the result of the last command. The app's own web server
 *   (port 3030) serves it to the page, read only.
 * - Zero reference of the live grams, (raw - ref) / k: a tare (manual or auto) wins;
 *   else the calibrate mean of the current/last job; else the job's start zero.
 *   A job's calibrate mean replaces a tare made before that job started.
 * - Auto-tare: no print session for 10 s -> every 10 s zero = mean of the last 10 s, only
 *   if >= 30 readings, sd < 1 g and within 25 g of the current zero (a weight put on the
 *   plate is never tared away). Off = <csvdir>/AUTOTARE_OFF exists.
 * - Commands: files <livedir>/cmd.* (sgwebd.tcl writes a temp file and renames it), one
 *   line: "tare", "untare", "auto 0|1", "k <raw counts per gram>",
 *   "heat P TPRE H TPRINT" (sgheat.c), "wait DW WH GRAD WIN MIN DB WT" and
 *   "settings save|restore|defaults" (sgwait.c), "slowset A B C D E F" (sgtilt.c, slow layers),
 *   "uisleep IDLE PRINT" (sgsys.c, UI loop sleep ms). Tare in the wait (WT, sgwait.c): the zero moves
 *   to the settled level of each wait after retract (src "wait tare"). k goes to <csvdir>/K
 *   (read by the CSV logger at each session start) and is used live right away; both
 *   files are re-read every 10 s.
 * Env (tests): SGTAP_LIVEDIR.
 */

#define AUTO_IDLE_US  10000000u
#define AUTO_SD_G     1.0
#define AUTO_MAX_G    25.0
#define LIVE_N        20              /* readings per live.json, ~4 s at 5 Hz */

/* sgwait.c, #included after this file */
static void sgwait_init(void);
static void sgwait_tick(int reload);
static void sgwait_cmd(const char *c, char *res, size_t len);
static void sgwait_min2_cmd(const char *c, char *res, size_t len);
static void sgtl_cmd(const char *c, char *res, size_t len);
static void sgset_cmd(const char *c, char *res, size_t len);
static int sgwait_json(char *buf, size_t len);
static int sgwait_take_tare(double *raw, int *layer);
static int sgwait_wt(void);
/* sgtilt.c, #included after sgwait.c */
static void sgtilt_init(void);
static void sgtilt_tick(int reload);
static int sgtilt_json(char *buf, size_t len);
static int sgexpo_json(char *buf, size_t len);
static int sgzmon_json(char *buf, size_t len);
static void sgexpo_cmd(const char *c, char *res, size_t len);
static void sgexpo_resin_cmd(const char *c, char *res, size_t len);
static void sgtilt_cmd(const char *c, char *res, size_t len);
static int sgtilt_queue(const double v[13], char *res, size_t len);
static int sgtilt_read(double v[13]);
static int tp_valid(const double v[13]);
static void sgtilt_stock(double v[13]);
static int tp_ok;
static void sgtilt_slow_wait(int layer0);
static void sgtilt_slow_cmd(const char *c, char *res, size_t len);
static void sgtilt_beep_cmd(const char *c, char *res, size_t len);
static void sgtilt_slowen_cmd(const char *c, char *res, size_t len);
static void sgtilt_dump_cmd(char *res, size_t len);
static void sgtilt_reg_cmd(const char *c, char *res, size_t len);
static void sgtilt_fix_cmd(const char *c, char *res, size_t len);
static void sgwait_ov_cmd(const char *c, char *res, size_t len);
static void sgtilt_gsync_cmd(const char *c, char *res, size_t len);
static void sgtilt_gsync_get(int *on, int *au, int *off);
static void sgtilt_dl_cmd(const char *c, char *res, size_t len);
static void sgtilt_mode_cmd(const char *c, char *res, size_t len);
static void sgtilt_xmove_cmd(const char *c, char *res, size_t len);
static int sgtilt_tc_get(double v[13], int *on);
static void sgtilt_tc_set(const double v[13], int on);
static void sgtilt_dl_get(int v[5]);
static int dl_valid(const double v[5]);
static int sgtilt_fix_get(void);
static int sl_enabled(void);
static int sgtilt_beep_get(void);
static void sgtilt_slow_get(double v[6]);
static int sl_valid(const double v[6]);
static int sl_apply_set(const double v[6], char *res, size_t len);
static const double sl_def[6];
/* sgsys.c */
static void sgsys_tick(void);
static int sgsys_json(char *buf, size_t len);
static void sgsys_ui_cmd(const char *c, char *res, size_t len);
static int u_valid(double i, double p);
static int u_apply(int i, int p, char *res, size_t len);
/* sgmach.c */
static void sgmach_cmd(const char *c, char *res, size_t len);
static int sgmach_json(char *buf, size_t len);
static void sgflex_cmd(const char *c, char *res, size_t len);
static int sgflex_json(char *buf, size_t len);
/* sgspi.c */
static void sgspi_tick(void);
static int sgspi_json(char *buf, size_t len);
/* sgdisp.c */
static void sgdisp_cmd(const char *c, char *res, size_t len);
static void sgdisp_swap(const char *c, char *res, size_t len);
static void sgdisp_seq(const char *c, char *res, size_t len);
static void sgdisp_tick(void);
static void sgmx_cmd(const char *c, char *res, size_t len);
static void sgmx_tick(void);
static int sgmx_json(char *buf, size_t len);
static int sgdisp_json(char *buf, size_t len);
static int u_idle, u_print;
static volatile int u_ok;

static char sg_livedir[128] = "/tmp/sgweb";
static int w_init, w_auto = 1;
static double w_k = 100.33;
static struct { int have, is_auto, layer; double raw; time_t t; } tare;  /* is_auto: 0 manual,
                                                                          1 idle, 2 in the wait */
static struct { int have; double raw; } jref;   /* calibrate mean of the last job */
static uint64_t idle_since, auto_last;
static time_t s_start_wall;
static char auto_note[96] = "", cmd_id[48] = "", cmd_res[96] = "";

static double wall_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double sq_root(double x)      /* no libm in the link */
{
    double r = x > 1 ? x : 1;
    if (x <= 0)
        return 0;
    for (int i = 0; i < 40; i++)
        r = (r + x / r) / 2;
    return r;
}

static int read_small(const char *path, char *buf, int len)
{
    int fd = real_open(path, O_RDONLY | O_CLOEXEC);
    ssize_t r;
    if (fd < 0)
        return -1;
    r = real_read(fd, buf, (size_t)len - 1);
    real_close(fd);
    buf[r > 0 ? r : 0] = 0;
    return r > 0 ? (int)r : 0;
}

/* "-100.33" -> -100.33 (parse_k with a sign) */
static double parse_sk(const char *p)
{
    while (*p == ' ')
        p++;
    return *p == '-' ? -parse_k(p + 1) : parse_k(p);
}

static int k_ok(double k)
{
    return (k >= 1 && k <= 100000) || (k <= -1 && k >= -100000);
}

/* k and the auto-tare switch from their files. Re-read every 10 s: at app start the
 * data partition (mmcblk0p2) may not be mounted yet, and K can be edited by hand. */
static void load_settings(void)
{
    char path[160], buf[32];
    snprintf(path, sizeof path, "%s/K", sg_csvdir);
    if (read_small(path, buf, sizeof buf) > 0 && k_ok(parse_sk(buf)))
        w_k = parse_sk(buf);
    snprintf(path, sizeof path, "%s/AUTOTARE_OFF", sg_csvdir);
    w_auto = access(path, F_OK) != 0;
}

static void sgweb_init(void)
{
    const char *s;
    if ((s = getenv("SGTAP_LIVEDIR")) && *s)
        snprintf(sg_livedir, sizeof sg_livedir, "%s", s);
    mkdir(sg_livedir, 0755);
    sgheat_init();
    sgwait_init();
    sgtilt_init();
    w_init = 1;
}

/* sglog.c: a session started / its calibrate phase is over */
static void sgweb_session_start(void)
{
    s_start_wall = time(NULL);
}

static void sgweb_calib(double ref)
{
    jref.have = 1;
    jref.raw = ref;
    if (tare.have && tare.t < s_start_wall)
        tare.have = 0;
}

/* mean (and sd) of the readings newer than now - span_us; returns the count */
static int recent(uint64_t span_us, double *mean, double *sd)
{
    uint64_t now = now_us();
    double sum = 0, sum2 = 0;
    int n = 0;
    pthread_mutex_lock(&glock);
    for (unsigned i = 0; i < HIST && i < hpos; i++) {
        unsigned k = (hpos - 1 - i) % HIST;
        if (hist[k].ts + span_us < now)
            break;
        sum += hist[k].raw;
        sum2 += (double)hist[k].raw * hist[k].raw;
        n++;
    }
    pthread_mutex_unlock(&glock);
    if (n) {
        *mean = sum / n;
        *sd = sq_root(sum2 / n - *mean * *mean);
    }
    return n;
}

/* the reference the live grams use: 1 = have one */
static int cur_ref(double *raw, const char **src)
{
    if (tare.have) {
        *raw = tare.raw;
        *src = tare.is_auto == 2 ? "wait tare" : tare.is_auto ? "auto-tare" : "tare";
    } else if (s_active && !cal_done) {
        *raw = s_ref;                             /* start zero until calibrate is over */
        *src = "job start";
    } else if (jref.have) {
        *raw = jref.raw;
        *src = "calibrate";
    } else {
        return 0;
    }
    return 1;
}

static void auto_tare(void)
{
    uint64_t now = now_us();
    double m, sd, ref;
    const char *src;
    int n;
    if (s_active) {
        idle_since = 0;
        snprintf(auto_note, sizeof auto_note, "paused: printing");
        return;
    }
    if (!idle_since)
        idle_since = now;
    if (!w_auto) {
        snprintf(auto_note, sizeof auto_note, "off");
        return;
    }
    if (now - idle_since < AUTO_IDLE_US) {
        snprintf(auto_note, sizeof auto_note, "waiting for 10 s idle");
        return;
    }
    if (auto_last && now - auto_last < AUTO_IDLE_US)
        return;
    auto_last = now;
    n = recent(AUTO_IDLE_US, &m, &sd);
    if (n < 30) {
        snprintf(auto_note, sizeof auto_note, n ? "not enough readings" : "no live readings");
        return;
    }
    sd /= w_k < 0 ? -w_k : w_k;
    if (sd > AUTO_SD_G) {
        snprintf(auto_note, sizeof auto_note, "skipped: readings not steady (%.1f g)", sd);
        return;
    }
    if (cur_ref(&ref, &src) && ((m - ref) / w_k > AUTO_MAX_G || (m - ref) / w_k < -AUTO_MAX_G)) {
        snprintf(auto_note, sizeof auto_note, "skipped: %.0f g from zero (load on the plate?)",
                 (m - ref) / w_k);
        return;
    }
    snprintf(auto_note, sizeof auto_note, "corrected %+.1f g",
             cur_ref(&ref, &src) ? (m - ref) / w_k : 0.0);
    tare.have = 1;
    tare.is_auto = 1;
    tare.layer = -1;
    tare.raw = m;
    tare.t = time(NULL);
}

static void write_k(double k)
{
    char path[160], tmp[168], line[32];
    int fd, n = snprintf(line, sizeof line, "%.3f\n", k);
    snprintf(path, sizeof path, "%s/K", sg_csvdir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    mkdir(sg_csvdir, 0755);
    fd = real_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (real_write(fd, line, (size_t)n) != n)
        n = -1;
    fsync(fd);
    real_close(fd);
    if (n > 0)
        rename(tmp, path);
}

static void set_auto(int on)
{
    char path[160];
    int fd;
    w_auto = on;
    auto_last = 0;
    snprintf(path, sizeof path, "%s/AUTOTARE_OFF", sg_csvdir);
    if (w_auto) {
        unlink(path);
    } else if ((fd = real_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644)) >= 0) {
        real_close(fd);
    }
}

static void joblog_set(int on)         /* "joblog 0|1" and the settings snapshot */
{
    char fl[160];
    int fd;
    snprintf(fl, sizeof fl, "%s/DISABLE", sg_csvdir);
    if (on)
        unlink(fl);
    else if ((fd = real_open(fl, O_WRONLY | O_CREAT | O_CLOEXEC, 0644)) >= 0)
        real_close(fd);
}

static int joblog_on(void)             /* <csvdir>/DISABLE absent = job logging on */
{
    char fl[160];
    snprintf(fl, sizeof fl, "%s/DISABLE", sg_csvdir);
    return access(fl, F_OK) != 0;
}

static void run_cmd(const char *id, char *c)
{
    double m, sd;
    snprintf(cmd_id, sizeof cmd_id, "%s", id);
    snprintf(cmd_res, sizeof cmd_res, "ok");
    if (strncmp(c, "tare", 4) == 0) {
        if (recent(2000000u, &m, &sd)) {
            tare.have = 1;
            tare.is_auto = 0;
            tare.layer = -1;
            tare.raw = m;
            tare.t = time(NULL);
        } else {
            snprintf(cmd_res, sizeof cmd_res, "error: no live readings");
        }
    } else if (strncmp(c, "untare", 6) == 0) {
        tare.have = 0;
    } else if (strncmp(c, "auto ", 5) == 0) {
        set_auto(c[5] == '1');
    } else if (strncmp(c, "k ", 2) == 0) {
        double k = parse_sk(c + 2);
        if (k_ok(k)) {
            w_k = k;
            write_k(k);
        } else {
            snprintf(cmd_res, sizeof cmd_res, "error: k out of range");
        }
    } else if (strncmp(c, "heat ", 5) == 0) {
        sgheat_cmd(c + 5, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "tl ", 3) == 0) {
        sgtl_cmd(c + 3, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "mix ", 4) == 0) {
        sgheat_mix_cmd(c + 4, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "waitmin2 ", 9) == 0) {
        sgwait_min2_cmd(c + 9, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "waitov ", 7) == 0) {
        sgwait_ov_cmd(c + 7, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "resin ", 6) == 0) {
        sgexpo_resin_cmd(c + 6, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "expo ", 5) == 0) {
        sgexpo_cmd(c + 5, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "wait ", 5) == 0) {
        sgwait_cmd(c + 5, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "tilt ", 5) == 0) {
        sgtilt_cmd(c + 5, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "settings ", 9) == 0) {
        sgset_cmd(c + 9, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "slowset ", 8) == 0) {
        sgtilt_slow_cmd(c + 8, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "gsync ", 6) == 0) {
        sgtilt_gsync_cmd(c + 6, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "slowlayers ", 11) == 0) {
        sgtilt_slowen_cmd(c + 11, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "tilt2 ", 6) == 0) {
        sgtilt_fix_cmd(c + 6, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "joblog ", 7) == 0) {
        if (c[7] == '0' || c[7] == '1')
            joblog_set(c[7] == '1');
    } else if (strncmp(c, "xmove ", 6) == 0) {
        sgtilt_xmove_cmd(c + 6, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "tiltmode ", 9) == 0) {
        sgtilt_mode_cmd(c + 9, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "dynlift ", 8) == 0) {
        sgtilt_dl_cmd(c + 8, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "tiltreg ", 8) == 0) {
        sgtilt_reg_cmd(c + 8, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "tiltdump", 8) == 0) {
        sgtilt_dump_cmd(cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "slowbeep ", 9) == 0) {
        sgtilt_beep_cmd(c + 9, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "uisleep ", 8) == 0) {
        sgsys_ui_cmd(c + 8, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "flex ", 5) == 0) {
        sgflex_cmd(c + 5, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "mset ", 5) == 0) {
        sgmach_cmd(c + 5, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "lcdshow ", 8) == 0) {
        sgdisp_cmd(c + 8, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "lcdswap ", 8) == 0) {
        sgdisp_swap(c + 8, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "lcdseq ", 7) == 0) {
        sgdisp_seq(c + 7, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "mx ", 3) == 0) {
        sgmx_cmd(c + 3, cmd_res, sizeof cmd_res);
    } else if (strncmp(c, "lcdoff", 6) == 0) {
        sgdisp_cmd("off", cmd_res, sizeof cmd_res);
    } else {
        snprintf(cmd_res, sizeof cmd_res, "error: unknown command");
    }
}

/* commands in the order sgwebd wrote them (ids sort by time) */
static void poll_cmds(void)
{
    static char names[8][64];
    static char *ptr[8];
    DIR *d = opendir(sg_livedir);
    struct dirent *e;
    char path[256], buf[64];
    int n = 0;
    if (!d) {
        mkdir(sg_livedir, 0755);
        return;
    }
    while ((e = readdir(d)) && n < 8)
        if (strncmp(e->d_name, "cmd.", 4) == 0 && strlen(e->d_name) < sizeof names[0]) {
            snprintf(names[n], sizeof names[n], "%s", e->d_name);
            ptr[n] = names[n];
            n++;
        }
    closedir(d);
    qsort(ptr, (size_t)n, sizeof ptr[0], cmp_str);
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", sg_livedir, ptr[i]);
        if (read_small(path, buf, sizeof buf) >= 0)
            run_cmd(ptr[i] + 4, buf);
        unlink(path);
    }
}

/* JSON string body: drop quotes, backslashes and control characters */
static void js(char *out, size_t len, const char *s)
{
    size_t i = 0;
    for (; *s && i + 1 < len; s++)
        if ((unsigned char)*s >= 0x20 && *s != '"' && *s != '\\')
            out[i++] = *s;
    out[i] = 0;
}

/* "alerts":[...] oldest first (see alert_add in sglog.c): n = running number of this boot, t = unix time, k = kind, m = text, d = the app's log line */
static int sgalerts_json(char *buf, size_t len)
{
    int n = snprintf(buf, len, ",\"alerts\":[");
    unsigned first = al_seq > AL_N ? al_seq - AL_N : 0, i;
    pthread_mutex_lock(&al_lock);
    for (i = first; i < al_seq && n < (int)len - 400; i++) {
        char k[24], m[200], d[220];
        unsigned x = i % AL_N;
        js(k, sizeof k, al_ring[x].kind);
        js(m, sizeof m, al_ring[x].msg);
        js(d, sizeof d, al_ring[x].raw);
        n += snprintf(buf + n, len - (size_t)n, "%s{\"n\":%u,\"t\":%ld,\"layer\":%d,\"k\":\"%s\",\"m\":\"%s\",\"d\":\"%s\"}",
                      i > first ? "," : "", al_ring[x].n, al_ring[x].t, al_ring[x].layer, k, m, d);
    }
    pthread_mutex_unlock(&al_lock);
    n += snprintf(buf + n, len - (size_t)n, "]");
    return n;
}

static void write_live(void)
{
    static char buf[12288];
    char path[160], tmp[168], csv[128], file[160];
    struct { unsigned seq; uint64_t ts; int32_t raw; } r[LIVE_N];
    unsigned seq, frames, bad;
    int n = 0, len, have, fd;
    uint64_t now = now_us();
    double wall = wall_now(), ref = 0;
    const char *src = NULL;

    pthread_mutex_lock(&glock);
    seq = hpos;
    frames = g_frames;
    bad = g_crc_bad;
    have = g_have;
    for (unsigned i = 0; i < LIVE_N && i < hpos && i < HIST; i++) {
        unsigned k = hpos - 1 - i;
        r[n].seq = k + 1;
        r[n].ts = hist[k % HIST].ts;
        r[n].raw = hist[k % HIST].raw;
        n++;
    }
    snprintf(file, sizeof file, "%s", print_file);
    pthread_mutex_unlock(&glock);

    csv[0] = 0;
    if (txt_name[0]) {                            /* current / last CSV of this boot */
        const char *b = strrchr(txt_name, '/');
        js(csv, sizeof csv, b ? b + 1 : txt_name);
        if (has_suffix(csv, ".txt"))
            memcpy(csv + strlen(csv) - 4, ".csv", 4);
    }
    len = snprintf(buf, sizeof buf, "{\"v\":1,\"now\":%.3f,\"seq\":%u,\"have\":%d,\"frames\":%u,"
                   "\"crc_bad\":%u,\"k\":%.3f,\"r\":[", wall, seq, have, frames, bad, w_k);
    for (int i = n - 1; i >= 0; i--)              /* oldest first */
        len += snprintf(buf + len, sizeof buf - (size_t)len, "[%u,%.3f,%d]%s", r[i].seq,
                        wall - (now - r[i].ts) / 1e6, (int)r[i].raw, i ? "," : "");
    len += snprintf(buf + len, sizeof buf - (size_t)len, "],\"ref\":");
    if (cur_ref(&ref, &src))
        len += snprintf(buf + len, sizeof buf - (size_t)len,
                        "{\"raw\":%.1f,\"src\":\"%s\",\"t\":%ld,\"layer\":%d}", ref, src,
                        tare.have ? (long)tare.t : 0L, tare.have ? tare.layer : -1);
    else
        len += snprintf(buf + len, sizeof buf - (size_t)len, "null");
    js(file, sizeof file, file);
    len += snprintf(buf + len, sizeof buf - (size_t)len,
                    ",\"auto\":{\"on\":%d,\"note\":\"%s\"},\"job\":{\"active\":%d,\"phase\":\"%s\","
                    "\"layer\":%d,\"csv\":\"%s\",\"file\":\"%s\",\"start\":%ld,\"log\":%d},"
                    "\"cmd\":{\"id\":\"%s\",\"res\":\"%s\"}",
                    w_auto, auto_note, s_active, s_active ? s_phase : "", s_active ? s_layer : -1,
                    csv, file, (long)s_start_wall, joblog_on(), cmd_id, cmd_res);
    if (len < (int)sizeof buf)
        len += sgalerts_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgheat_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgwait_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgtilt_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgexpo_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgzmon_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgsys_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgmach_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgflex_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgspi_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgmx_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += sgdisp_json(buf + len, sizeof buf - (size_t)len);
    if (len < (int)sizeof buf)
        len += snprintf(buf + len, sizeof buf - (size_t)len, "}\n");
    if (len >= (int)sizeof buf)
        return;
    snprintf(path, sizeof path, "%s/live.json", sg_livedir);
    snprintf(tmp, sizeof tmp, "%s/live.tmp", sg_livedir);
    fd = real_open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (real_write(fd, buf, (size_t)len) == len) {
        real_close(fd);
        rename(tmp, path);
    } else {
        real_close(fd);
    }
}

/* every 0.5 s from the writer thread, after the CSV row */
static void sgweb_tick(void)
{
    static unsigned ticks;
    if (!w_init)
        sgweb_init();
    if (ticks % 20 == 0)
        load_settings();
    sgwait_tick(ticks % 20 == 0);
    sgtilt_tick(ticks++ % 20 == 0);
    sgsys_tick();
    {
        double raw;
        int layer;
        if (sgwait_take_tare(&raw, &layer)) {   /* settled in the wait: new zero */
            tare.have = 1;
            tare.is_auto = 2;
            tare.layer = layer;
            tare.raw = raw;
            tare.t = time(NULL);
        }
        if (tare.have && tare.is_auto == 2 && !sgwait_wt())
            tare.have = 0;                        /* switched off: back to the job's zero */
    }
    poll_cmds();
    auto_tare();
    write_live();
}
