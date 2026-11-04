/*
 * sgtilt.c - two-stage tilt (X) fix (step 8.16b), #included by sgtap.c after sgwait.c.
 *
 * Stock bug (V1.5.9 ): with a stage angle (I200 E/F,
 * X/Y) the app sends stage 1 (wait = 1) and stage 2 3 ms later. The tilt chip (fq6630, a
 * TMC5160-like ramp chip) gets XTARGET = XACTUAL + step, with stage 2's step computed from the
 * app's own position (= stage 1's target) while XACTUAL is still near the start: the release
 * ends ~80 deg instead of 140, the return ~60 deg past zero.
 * Fix: the tilt stepper's ops table (object *(0x2fed30+4), ops pointer at +0x18, rodata
 * 0x210108) is copied with our move op. Stage 1 runs as stock and we keep its target T1. The
 * move right after it (stage 2) gets XTARGET = T1 + step = the target the app believes in, at
 * once, still at stage 1's speed; our thread polls XACTUAL every T_POLL_US and writes only
 * VMAX = stage 2's speed when the stage angle is reached (minus the lead, sglog/TILT2). A late
 * or missing switch only leaves the rest of the move at stage 1's speed. Before the VMAX write
 * XTARGET is read back: any app stop / retarget in between (it writes XTARGET) cancels it.
 * Register access as the app's ops do: under its SPI mutex 0x3a15a8, write 0x84ad0(i, reg, v),
 * read 0x84be4(i, reg, &v) (XACTUAL is a single read there; RAMP_STAT is never read by us,
 * reading it clears its event bits).
 * Opt-in: <csvdir>/TILT2 ("[LEAD_DEG]", empty = 0) installs the fix (checked every 10 s; when
 * removed the move op passes through). <csvdir>/TILTDRV (with TILTTRACE): every ~20 ms during a move a row
 * 'D,<XACTUAL>,<DRV_STATUS>,<PWM_SCALE>' (driver status, see docs/tilt_motor.txt). <csvdir>/TILTTRACE: XACTUAL every T_POLL_US during each
 * tilt move -> <csvdir>/tilt_<YYYYmmdd_HHMMSS>.csv "ms,ev,x,a,b" (app clock ms). Kill switch
 * /customer/mod/sgtilt_disable. Only with the V1.5.9 code words below.
 *
 * Tilt profile (8.17, page Tilt card): command "tilt N A B C D E F S P R T X Y" | "tilt stock"
 * (sgweb.c run_cmd). The profile the stage code reads on every move is the live normal-mode
 * rotate_param T_PROF (floats A..Y = I200 of the machine file, +0x34 N); the S2 (fast) one is
 * not touched. Checked, then written by the writer thread when it is safe: not printing, or in
 * the wait after retract (no tilt move then), followed by the app's own machine-param saver
 * (machine_param_ref.bin, like a machine file). Speeds 10-144 even, angles 0 or 10-120 even
 * (0 = one stage), N 0-999; an angle > 0 only while the fix above is installed.
 *
 * Slow layers (8.20, page: Files card "Slow layers", Tilt card "Slow set"): a layer list per
 * print file, <csvdir>/layertilt/<print file name without extension>.txt (written by
 * sgwebd.tcl): layers 1-based as in Chitubox, "20-35", "120", separated by commas, spaces or
 * new lines, "#" to the end of the line = comment. One slow set for every listed layer,
 * <csvdir>/SLOWTILT "A B C D E F" (return stage 1/2 speed, release stage 1/2 speed, return /
 * release stage angle, the I200 meaning; default 18 18 18 18 0 0 = one stage at 18 deg/s),
 * command "slowset A B C D E F". The wait thread (sgwait.c begin_wait, 20 Hz, at the start of
 * each wait after retract = before that layer's exposure, release and return) writes the set
 * into both blocks (before / after N) of the profile the print uses (T_PROFPTR) when the
 * layer is listed, and puts the profile back (sl_base) at the first unlisted layer, when the
 * printer is no longer printing and before the app's saver runs (tp_apply_due): the slow
 * values are never saved. A stage angle without the fix installed is written as 0 (one stage).
 */

#define T_OBJS     0x2fed30u   /* stepper objects [3]; [1] = tilt */
#define T_OPS_ROM  0x210108u   /* fq6630 ops table, 15 words */
#define T_OPS_N    15
#define T_MUTEX    0x3a15a8u   /* the app's fq6630 SPI mutex */
#define T_WREG     0x84ad0u    /* int wreg(int idx, int reg, int32_t val) */
#define T_RREG     0x84be4u    /* int rreg(int idx, int reg, int32_t *val) */
#define T_MOVE     0x85178u    /* ops+0x0c: move(idx, step, speed, wait) */
#define R_XACTUAL  0x21
#define R_VMAX     0x27
#define R_TZEROW   0x2c
#define R_XTARGET  0x2d
#define R_DRVSTAT  0x6f        /* read once by the app's op (no verify loop) */
#define R_PWMSCALE 0x71        /* read twice + retried until equal: changes slowly */
#define T_DRV_EVERY 5          /* driver rows: every 5th poll (~20 ms) */
#define T_TZW_STOCK 0x1999     /* TZEROWAIT the stock move writes when wait = 0 */
#define T_IDX      1
#define T_DEG      0.00703125  /* deg per step (tilt distance_per_step) */
#define T_POLL_US  4000
#define T_ARM_MS   500         /* stage 2 must follow stage 1 within this */
#define T_TRACE_MAX_MS 12000
#define T_PROF     0x39fb40u   /* live normal-mode rotate_param: 12 floats A..Y, +0x34 N (u32) */
#define T_PROFPTR  0x2fda90u   /* -> the profile in use (set at print start: T_PROF or S2) */
#define T_NMAX     999

typedef int (*t_wreg_fn)(int, int, int32_t);
typedef int (*t_rreg_fn)(int, int, int32_t *);
typedef int (*t_move_fn)(int, int32_t, uint32_t, int);

static const struct { uint32_t a, w; } t_sig[] = {
    {0x85178, 0xe92d40f0}, {0x8517c, 0xe24dd00c}, {0x85180, 0xe1a04000}, {0x85184, 0xe1a07002},
    {0x85188, 0xe30105a8}, {0x8518c, 0xe340003a}, {0x85190, 0xe1a06001}, {0x85194, 0xe1a05003},
    {0x85198, 0xe3a03000}, {0x8519c, 0xe58d3004}, {0x851a0, 0xebfe49c7}, {0x851a4, 0xe1a02007},
    {0x851a8, 0xe3a01027}, {0x851ac, 0xe1a00004}, {0x851b0, 0xebfffe46}, {0x851b4, 0xe28d2004},
    {0x851b8, 0xe3a01021}, {0x851bc, 0xe1a00004}, {0x851c0, 0xebfffe87}, {0x851c4, 0xe59d2004},
    {0x851c8, 0xe1a00004}, {0x851cc, 0xe3a0102d}, {0x851d0, 0xe0862002}, {0x851d4, 0xebfffe3d},
    {0x851d8, 0xe3550000}, {0x851dc, 0xe1a00004}, {0x851e0, 0x0a000006}, {0x851e4, 0xe3a0102c},
    {0x851e8, 0xebfffdf9}, {0x851ec, 0xe30105a8}, {0x851f0, 0xe340003a}, {0x851f4, 0xebfe4ce1},
    {0x851f8, 0xe28dd00c}, {0x851fc, 0xe8bd80f0}, {0x85200, 0xe3012999}, {0x85204, 0xe3a0102c},
    {0x85208, 0xebfffe30}, {0x8520c, 0xeafffff6}, {0x84ad0, 0xe92d4ff0}, {0x84ad4, 0xe24dd02c},
    {0x84ad8, 0xe6ef3072}, {0x84adc, 0xe1a0bc22}, {0x84ae0, 0xe3818080}, {0x84be4, 0xe92d4ff0},
    {0x84be8, 0xe24dd06c}, {0x84bec, 0xe1a09001}, {0x84bf0, 0xe1a0b002}, {0x84bf4, 0xe58d203c},
    {0x84c9c, 0xe359006f}, {0x84ca0, 0x13590035}, {0x84ca4, 0xe2492021}, {0x84ca8, 0x03a03001},
    {0x84cac, 0x13a03000}, {0x84cb0, 0xe3520001}, {0x84cb4, 0x93833001}, {0x84cb8, 0xe153000a},
    {0x84cbc, 0x1a000049}, {0x89dc4, 0xe1853006}, {0x89dc8, 0xe3570001}, {0x89dcc, 0x03530000},
    {0x89dd0, 0x0a000009}, {0x89dd4, 0xe598c018}, {0x89dd8, 0xe1a03007}, {0x89ddc, 0xe1a02006},
    {0x89de0, 0xe1a01005}, {0x89de4, 0xe1a00004}, {0x89de8, 0xe59ce00c}, {0x89dec, 0xe1a0c00e},
    {0x89df0, 0xe28dd010}, {0x89df4, 0xe8bd41f0}, {0x89df8, 0xe12fff1c}, {0x210108, 0x00084e00},
    {0x21010c, 0x00000000}, {0x210110, 0x00085138}, {0x210114, 0x00085178}, {0x210118, 0x00085250},
    {0x21011c, 0x000852b0}, {0x210120, 0x00085390}, {0x210124, 0x000853c8}, {0x210128, 0x000853c0},
    {0x21012c, 0x00000000}, {0x210130, 0x00000000}, {0x210134, 0x0008530c}, {0x210138, 0x00085210},
    {0x21013c, 0x00085490}, {0x210140, 0x0008557c}, {0x55290, 0xe286203c}, {0x55294, 0xe5872034},
    {0x55298, 0xf4430a8f}, {0x5529c, 0xe1a01003}, {0x552a0, 0xeb00d27a}, {0x552a4, 0xe1a00008},
    {0x53de4, 0xe7e030d3}, {0x53de8, 0xeb00d7e1}, {0x53dec, 0xe3a02001},
};

/* the profile: selected at print start (0x7c9a0, 0x7cf34), read by stage3 (0x7a550..),
 * copied live -> ref by the saver (0x65478: 0x65740 = +0x110 A, 0x657f0 = +0x144 N) */
static const struct { uint32_t a, w; } tp_sig[] = {
    {0x7c9b4, 0xe59f2024}, {0x7c9b8, 0xe30d3a88}, {0x7c9bc, 0xe340302f}, {0x7c9c0, 0xe5832008},
    {0x7c9e0, 0x0039fb40}, {0x7c9e4, 0x0039fb78}, {0x7cf34, 0xe59f3074}, {0x7cf38, 0xe30d5a88},
    {0x7cf3c, 0xe340502f}, {0x7cf40, 0xe5853008}, {0x7cfb0, 0x0039fb40}, {0x7a550, 0xe30d7a88},
    {0x7a554, 0xe340702f}, {0x7a560, 0xe597c008}, {0x6549c, 0xe3404039}, {0x65740, 0xedd44a44},
    {0x657f0, 0xe594b144},
    {0x80a40, 0xe30f3a30}, {0x80a44, 0xe3403039}, {0x80a48, 0xe593c0ec}, {0x80a64, 0xe5dee012},
    {0x80a98, 0xe5930000}, {0x80aa8, 0xeb019f0f},    /* the app's beep 0x80a40: queue message to the beeper thread */
};
static int tp_ok;                      /* profile code verified (+ sgheat for state + saver) */
static const double tp_stock[13] = {50, 36, 36, 36, 36, 0, 0, 72, 72, 72, 72, 0, 0};
static struct {
    int pending, layer;                /* a profile waiting for a safe moment */
    double v[13];                      /* N, A..Y */
    char res[80];                      /* the last apply, for live.json */
} tp_req;                              /* under t_lock */

/* Beep before a slow layer (dev option, <csvdir>/SLOWBEEP). The printer's buzzer is PWM channel 1, driven by a thread
 * that reads messages {on_ms, off_ms, count, volume, frequency} from a queue; the app's beep (0x80a40) fills it
 * only when the beep switch of the touch screen is on. We send the same message ourselves (volume and frequency
 * from the machine settings), so the stock beeps can stay off. Nothing is written but the queue message. */
#define B_QUEUE    0x2fdc28u   /* *(u32*) = queue handle (byte +4 is 0 while idle: a beep in progress) */
#define B_QSEND    0xe86ecu    /* int queue_send(queue, msg, 1) */
#define B_FREQ     (0x39fa30u + 0xd8)    /* beep_frequency (Hz) */
#define B_VOL      (0x39fa30u + 0xec)    /* beep_volume (%) */
#define B_FILE     "SLOWBEEP"

static int sgbeep(int on_ms, int off_ms, int count)
{
    int32_t msg[5] = {on_ms, off_ms, count, (int32_t)HI(B_VOL), (int32_t)HI(B_FREQ)};
    int (*send)(uint32_t, void *, int) = (int (*)(uint32_t, void *, int))(uintptr_t)B_QSEND;
    if (!tp_ok || !HW(B_QUEUE) || msg[3] <= 0 || msg[4] <= 0)
        return 0;
    send(HW(B_QUEUE), msg, 1);
    return 1;
}

static int sb_enabled(void)
{
    char path[160];
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, B_FILE);
    return access(path, F_OK) == 0;
}

#define SL_MAXR    512         /* ranges per layer list */
#define SL_MAXF    8192        /* bytes per layer list file */
static const double sl_def[6] = {18, 18, 18, 18, 0, 0};   /* also the Settings defaults */
static pthread_mutex_t sl_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    double set[6];                     /* A B C D E F */
    int nr, nl;                        /* ranges, layers in the list */
    int r[SL_MAXR][2];                 /* sorted, merged, 1-based */
    char stem[128], err[96];           /* list of this print file, its error */
    int active;                        /* slow values in the profile */
    uint32_t prof;                     /* the profile they are in */
    float base[12];                    /* its own values */
    int layer, next;                   /* last wait's layer (1-based), next listed one */
    int nslow;                         /* slow layers done this boot */
    int beeped;                        /* first layer of the last slow range announced by a beep */
} sl = {.set = {18, 18, 18, 18, 0, 0}};

static int t_ok;                       /* code verified */
/* tiltreg: the app's current ramp variables verified (0x817d8) */
static const struct { uint32_t a, w; } tr_sig[] = {
    {0x817dc, 0xe30d4d00}, {0x817e0, 0xe340402f}, {0x817e4, 0xe30f5a30}, {0x817e8, 0xe3405039}, {0x817ec, 0xe5d42000},
    {0x817f0, 0xe5d41001}, {0x817fc, 0xe5d52102}, {0x81808, 0xe5d43000}, {0x8180c, 0xe5d52101},
    {0x81814, 0x03013680}, {0x81818, 0x0340302e},
};
static int tr_ok;
static volatile int t_on;              /* TILT2 present: fix active */
static volatile int t_tracing;         /* TILTTRACE present */
static volatile int t_drv;             /* TILTDRV present (and TILTTRACE): driver status rows 'D' in the trace */
static volatile int t_installed;
static int t_fail;                     /* tilt thread did not start: stays off */
static volatile pid_t t_pid;           /* process running the tilt thread */
static uint32_t t_ops[T_OPS_N];        /* our copy of the ops table */
static double t_lead_deg;

/* stage state: written by the move wrapper (app's axis thread) under T_MUTEX */
static volatile uint32_t t_gen;        /* bumped on every tilt move */
static struct {
    int armed;                         /* last move was a stage 1 */
    int32_t t1, x0;
    uint32_t v1;
    uint64_t ms;
} t_st1;
static struct {
    int pending;                       /* VMAX switch to do */
    uint32_t gen, v2;
    int32_t t1, t2, lead;
    int dir;
    uint64_t deadline_us;
} t_sw;
static struct {
    int active;
    int32_t target;
    uint64_t t0_us;
    int still, ndrv;
    int32_t last_x;
} t_tr;

/* shared with live.json: under t_lock */
static pthread_mutex_t t_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t t_cond = PTHREAD_COND_INITIALIZER;
static int t_kick;                     /* a move came: poll (under t_lock) */
static struct {
    int n2, nsw, ngive, ncancel;       /* stage-2 moves fixed, switched, gave up, cancelled */
    double last_sw_deg, last_late_ms;  /* last switch: position, delay after T1 */
} t_stat;

static char t_tbuf[32768];             /* trace rows not written yet (under T_MUTEX) */
static int t_tlen, t_tfd = -1;

static uint64_t raw_us(void)           /* the app's clock (0xd2db0), in us */
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void t_lock_spi(void)   { pthread_mutex_lock((pthread_mutex_t *)(uintptr_t)T_MUTEX); }
static void t_unlock_spi(void) { pthread_mutex_unlock((pthread_mutex_t *)(uintptr_t)T_MUTEX); }

static int32_t t_read(int reg)
{
    int32_t v = 0;
    ((t_rreg_fn)(uintptr_t)T_RREG)(T_IDX, reg, &v);
    return v;
}

static void t_write(int reg, int32_t v)
{
    ((t_wreg_fn)(uintptr_t)T_WREG)(T_IDX, reg, v);
}

/* ---- gauge sync: the release starts at a fixed phase of the gauge poll clock ----
 * The app polls the gauge every ~197 ms; the layer period is ~68.1 polls, so the sample phase walks across the peel pulse
 * (0.27 s wide) and the logged peak beats (~every 10 layers). With the sync on, the first move of a release is held until
 * the next request of the poll clock + a phase offset (<= one poll, ~+0.1 s per layer), so every layer is read at the same
 * instants after the start of the release. Auto search (once per job, from release 3): 17 releases, even ones at the
 * reference offset, odd ones at 8 trial offsets (poll/8 apart); the trial whose logged peak is highest relative to the mean
 * of its two reference neighbours wins (> 3 %, else the reference stays). Settings <csvdir>/GSYNC "ON AUTO OFFSET_MS",
 * command "gsync ON AUTO OFF" / "gsync search". Needs the two-stage fix installed (our move wrapper). */
static struct dl_cfg dl_cfg = {0, 2000, 1500, 20, 5, 1};   /* Dynamic lift settings (see below); the auto search pauses while it is on */
#define GS_FILE  "GSYNC"
#define GS_NOFF  8
static struct {
    int on, autos, off_ms;             /* switch, auto search, offset in use (manual or found) */
    int state;                         /* 0 idle, 1 searching, 2 found, 3 manual */
    int pos, search_req, nrel, ref_off_ms, job;
    double peak[2 * GS_NOFF + 1], ratio[GS_NOFF];
    double delay_ms, delay_sum;
    int delay_n;
    uint64_t rel_us;                   /* start of the release in progress, 0 = none */
    unsigned lift2_seen;
} gs = {.on = 1, .autos = 1};      /* on by default (a GSYNC file overrides) */

static int gs_off_now_ms(void)
{
    double dt = gp_dt_us / 1000.0;
    if (gs.state == 1)
        return (gs.pos & 1) ? (int)((gs.pos / 2) * dt / GS_NOFF + 0.5) : gs.ref_off_ms;
    return gs.off_ms;
}

/* the move op, before the real call: hold the start of a release for the poll clock */
static void gs_release_delay(void)
{
    uint64_t now, last, tgt;
    double dt, need, k, off;
    struct timespec ts;
    if (!gs.on || HW(H_PSTATE) != 10)
        return;
    if (!s_active || s_layer < 0 || (strcmp(s_phase, "rest") != 0 && strcmp(s_phase, "exposure") != 0))
        return;                        /* only the release of a layer: not the start-up moves (probe, level, homing) */
    last = gp_last_us;
    dt = gp_dt_us;
    now = now_us();
    if (!last || now - last > 2000000u || dt < 100000)
        return;                        /* no poll clock: no delay */
    off = (double)gs_off_now_ms() * 1000.0;
    need = (double)(now + 3000) - ((double)last + off);
    k = (double)(uint64_t)(need / dt);
    if (k * dt < need)
        k += 1;
    tgt = last + (uint64_t)(off + k * dt);
    if (tgt <= now || tgt - now > (uint64_t)(2 * dt))
        return;
    ts.tv_sec = (time_t)(tgt / 1000000u);
    ts.tv_nsec = (long)(tgt % 1000000u) * 1000;
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    gs.delay_ms = (double)(tgt - now) / 1000.0;
    gs.delay_sum += gs.delay_ms;
    gs.delay_n++;
    gs.rel_us = now_us();
}

static void gs_save(void)
{
    char path[160], line[48];
    int fd, n;
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, GS_FILE);
    n = snprintf(line, sizeof line, "%d %d %d\n", gs.on, gs.autos, gs.off_ms);
    if ((fd = real_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) >= 0) {
        if (real_write(fd, line, (size_t)n) < 0)
            n = 0;
        real_close(fd);
    }
}

/* peel peak (g, positive) of the release that started at rel_us: baseline = mean of the readings 1.2 .. 0.2 s before */
static double gs_peak(uint64_t rel)
{
    double base = 0, mn = 0;
    int nb = 0, nm = 0;
    pthread_mutex_lock(&glock);
    for (unsigned i = 0; i < HIST && i < hpos; i++) {
        unsigned k = (hpos - 1 - i) % HIST;
        uint64_t ts = hist[k].ts;
        if (ts + 1200000u >= rel && ts + 200000u <= rel) {
            base += hist[k].raw;
            nb++;
        } else if (ts >= rel) {
            if (!nm || hist[k].raw < mn)
                mn = hist[k].raw;
            nm++;
        }
    }
    pthread_mutex_unlock(&glock);
    if (nb < 2 || nm < 2 || w_k <= 0)
        return 0;
    base /= nb;
    return base > mn ? (base - mn) / w_k : 0;
}

static void gs_finish_scan(void)
{
    int best = -1;
    double bv = 0, dt = gp_dt_us / 1000.0;
    for (int i = 0; i < GS_NOFF; i++) {
        double ref = (gs.peak[2 * i] + gs.peak[2 * i + 2]) / 2;
        gs.ratio[i] = ref > 30 ? gs.peak[2 * i + 1] / ref : 0;
        if (gs.ratio[i] > bv) {
            bv = gs.ratio[i];
            best = i;
        }
    }
    gs.off_ms = (best >= 0 && bv > 1.03) ? (int)(best * dt / GS_NOFF + 0.5) : gs.ref_off_ms;
    gs.state = 2;
    gs_save();
}

/* writer thread, every 0.5 s */
static void gs_tick(void)
{
    int job = HW(H_PSTATE) == 10 || s_active;
    if (!job) {
        if (gs.job) {                  /* job over */
            gs.job = 0;
            gs.nrel = gs.pos = 0;
            gs.rel_us = 0;
            gs.state = 0;
        }
        gs.lift2_seen = lg_lift2;
        return;
    }
    gs.job = 1;
    if (!gs.on) {
        gs.state = 0;
        gs.rel_us = 0;
        gs.lift2_seen = lg_lift2;
        return;
    }
    if (lg_lift2 != gs.lift2_seen) {   /* a release ended */
        gs.lift2_seen = lg_lift2;
        if (gs.rel_us) {
            double pk = gs_peak(gs.rel_us);
            gs.rel_us = 0;
            gs.nrel++;
            if (gs.state == 1) {
                gs.peak[gs.pos++] = pk;
                if (gs.pos > 2 * GS_NOFF)
                    gs_finish_scan();
            }
        }
    }
    /* Dynamic lift changes the start speed from layer to layer: the search (it compares neighbouring layers) would be misled, so
     * it is paused while Dynamic lift is on; a running search is stopped and the offset it started from stays */
    int paused = gs.autos && dl_cfg.on;
    if (paused && gs.state == 1)
        gs.off_ms = gs.ref_off_ms;
    if (!gs.autos || paused) {
        gs.state = 3;
    } else if (gs.state == 3) {
        gs.state = 0;
    }
    if (gs.autos && !paused && gs.state != 1 && (gs.state == 0 || gs.search_req) && gs.nrel >= 3 && t_on && t_installed) {
        gs.state = 1;
        gs.pos = 0;
        gs.search_req = 0;
        gs.ref_off_ms = gs.off_ms;
        memset(gs.peak, 0, sizeof gs.peak);
        memset(gs.ratio, 0, sizeof gs.ratio);
    }
}

static void gs_load(void)
{
    char path[160], buf[32];
    double v[3] = {0, 0, 0};
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, GS_FILE);
    if (read_small(path, buf, sizeof buf) >= 0 && parse_nums(buf, v, 3) == 3) {
        gs.on = v[0] > 0;
        gs.autos = v[1] > 0;
        gs.off_ms = (int)v[2] >= 0 && (int)v[2] < 1000 ? (int)v[2] : 0;
    }
}

/* "gsync ON AUTO OFFSET_MS" | "gsync search" */
static void sgtilt_gsync_cmd(const char *c, char *res, size_t len)
{
    int on, au, off;
    if (strncmp(c, "search", 6) == 0) {
        if (!gs.on || !gs.autos)
            snprintf(res, len, "error: switch the sync and the automatic search on first");
        else if (dl_cfg.on)
            snprintf(res, len, "error: the search is paused while Dynamic lift is on");
        else
            gs.search_req = 1;
        return;
    }
    if (!parse_int(&c, &on) || !parse_int(&c, &au) || !parse_int(&c, &off) || on > 1 || au > 1 || off > 999) {
        snprintf(res, len, "error: bad gsync command");
        return;
    }
    if (on && !(t_on && t_installed)) {
        snprintf(res, len, "error: the gauge sync needs the two-stage fix (sglog/TILT2)");
        return;
    }
    gs.on = on;
    gs.autos = au;
    gs.off_ms = off;
    if (!au)
        gs.state = gs.on && gs.job ? 3 : 0;
    gs_save();
}

/* the gauge sync switch / auto search / offset in use, and the beep option, for the settings snapshot (sgwait.c) */
static void sgtilt_gsync_get(int *on, int *au, int *off)
{
    *on = gs.on;
    *au = gs.autos;
    *off = gs.off_ms;
}

static int sgtilt_fix_get(void)        /* the two-stage fix option (TILT2) */
{
    return t_on;
}

static int sgtilt_beep_get(void)
{
    char path[160];
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, B_FILE);
    return access(path, F_OK) == 0;
}

/* ---- Dynamic lift state (the logic below, after the move wrapper) ---- */
#define DL_FILE     "DYNLIFT"
#define DL_MACH     0x39fa30u              /* machine params: +0x58 = total release angle (deg) */
#define DL_W_PPTR   0x2eb6d8u              /* -> the app's print struct (sgwait.c W_PPTR); +0x2ac = layer (0-based) */
#define DL_W_LAYER  0x2ac
static struct dl_ctl dl_ctl = {1.0, 0};    /* under t_lock */
static volatile int dl_idle = 1;           /* the printer was not printing since the controller last started (set by sgtilt_tick) */
static struct { int layer, kind; double peak, start, sep; } dl_hist[14];   /* newest last, under t_lock; kind 0 separation, 1 no peel, 2 no switch */
static int dl_nh;

/* ---- datalogger columns (sglog.c): the X tilt speeds of each release / return and the Dynamic lift separation angle ----
 * rel_v1,rel_v2,sep_deg,sep_kind,ret_v1,ret_v2,dl_peak_g,dl_f on the first CSV row after the move (deg/s as sent to the chip; v2 empty when the
 * move has one stage). sep_deg / sep_kind (sep | nopeel | none) come from Dynamic lift (empty when it is off). Own lock, never
 * held together with another one. */
static pthread_mutex_t tcsv_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int pend, dlwait, kind; double v1, v2, sep, peak, fac; uint64_t us; } tcsv_rel, tcsv_ret;

static void tcsv_note(int32_t step, uint32_t speed, uint32_t vfast, int wait)
{
    uint64_t now = raw_us();
    double v = speed * T_DEG, vf = vfast ? vfast * T_DEG : -1;
    typeof(tcsv_rel) *r = step > 0 ? &tcsv_rel : &tcsv_ret;
    if (!s_active)
        return;
    pthread_mutex_lock(&tcsv_lock);
    if (!wait && r->pend && r->v2 < 0 && now - r->us < 300000u) {
        r->v2 = vfast ? vf : v;                 /* stage 2 of the same move */
    } else if (step > 200 || step < -200) {
        memset(r, 0, sizeof *r);
        r->pend = 1;
        r->v1 = v;
        r->v2 = vf;
        r->kind = -1;
        r->us = now;
    }
    pthread_mutex_unlock(&tcsv_lock);
}

static void tcsv_dl(int armed, int kind, double sep, double peak, double fac)    /* armed 1 = the release is watched, 0 = Dynamic lift's result */
{
    pthread_mutex_lock(&tcsv_lock);
    if (armed) {
        tcsv_rel.dlwait = 1;
    } else {
        tcsv_rel.dlwait = 0;
        tcsv_rel.kind = kind;
        tcsv_rel.sep = sep;
        tcsv_rel.peak = peak;
        tcsv_rel.fac = fac;
    }
    pthread_mutex_unlock(&tcsv_lock);
}

static void tcsv_fmt(char *buf, size_t len, uint64_t now)
{
    char a[2][24] = {"", ""}, b[2][24] = {"", ""}, sp[24] = "", pk[24] = "", fc[24] = "";
    const char *kn[] = {"sep", "nopeel", "none"};
    const char *kind = "";
    pthread_mutex_lock(&tcsv_lock);
    if (tcsv_rel.pend && !tcsv_rel.dlwait && now - tcsv_rel.us > 300000u) {
        snprintf(a[0], sizeof a[0], "%.1f", tcsv_rel.v1);
        if (tcsv_rel.v2 >= 0)
            snprintf(a[1], sizeof a[1], "%.1f", tcsv_rel.v2);
        if (tcsv_rel.kind >= 0 && tcsv_rel.kind <= 2) {
            kind = kn[tcsv_rel.kind];
            if (tcsv_rel.kind < 2)
                snprintf(sp, sizeof sp, "%.1f", tcsv_rel.sep);
            snprintf(pk, sizeof pk, "%.0f", tcsv_rel.peak);
            snprintf(fc, sizeof fc, "%.3f", tcsv_rel.fac);
        }
        tcsv_rel.pend = 0;
    }
    if (tcsv_ret.pend && now - tcsv_ret.us > 300000u) {
        snprintf(b[0], sizeof b[0], "%.1f", tcsv_ret.v1);
        if (tcsv_ret.v2 >= 0)
            snprintf(b[1], sizeof b[1], "%.1f", tcsv_ret.v2);
        tcsv_ret.pend = 0;
    }
    pthread_mutex_unlock(&tcsv_lock);
    snprintf(buf, len, ",%s,%s,%s,%s,%s,%s,%s,%s", a[0], a[1], sp, kind, b[0], b[1], pk, fc);
}

/* sglog.c: ",rel_v1,rel_v2,sep_deg,sep_kind,ret_v1,ret_v2,dl_peak_g,dl_f" */
static void sgtilt_csv(char *buf, size_t len)
{
    tcsv_fmt(buf, len, raw_us());
}

/* ---- Dynamic lift: settings, command, snapshot access, live.json ---- */
static int dl_valid(const double v[5])
{
    for (int i = 0; i < 5; i++)
        if (v[i] != (int)v[i])
            return 0;
    /* on 0|1, limit g, lower level g (below the limit), slower step %, faster step % */
    return (v[0] == 0 || v[0] == 1) && v[1] >= 100 && v[1] <= 20000 && v[2] >= 50 && v[2] < v[1] && v[3] >= 1 && v[3] <= 90 &&
           v[4] >= 1 && v[4] <= 100;
}

static int dl_start_ok(double v)                /* first layer of Dynamic lift, 1-based */
{
    return v == (int)v && v >= 1 && v <= 100000;
}

static void dl_save(void)
{
    char path[160], line[64];
    int n = snprintf(line, sizeof line, "%d %d %d %d %d %d\n", dl_cfg.on, dl_cfg.limit, dl_cfg.lower, dl_cfg.slower, dl_cfg.faster,
                     dl_cfg.start);
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, DL_FILE);
    write_small(path, line, n);
}

static void dl_load(void)
{
    char path[160], buf[64];
    double v[6];
    int n;
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, DL_FILE);
    if (read_small(path, buf, sizeof buf) > 0 && ((n = parse_nums(buf, v, 6)) == 5 || n == 6) && dl_valid(v) && (n == 5 || dl_start_ok(v[5]))) {
        dl_cfg.on = (int)v[0];
        dl_cfg.limit = (int)v[1];
        dl_cfg.lower = (int)v[2];
        dl_cfg.slower = (int)v[3];
        dl_cfg.faster = (int)v[4];
        dl_cfg.start = n == 6 ? (int)v[5] : 1;           /* an older file has no start layer */
    }
}

static void tc_reconcile(void);

/* "dynlift ON LIMIT LOWER SLOWER FASTER [START]" | "dynlift reset" (the start speed back to the set speed); START = first layer (1-based),
 * left as it is when missing (the settings snapshot has no start layer) */
static void sgtilt_dl_cmd(const char *c, char *res, size_t len)
{
    double v[6];
    int n;
    if (!tp_ok || !h_ok) {
        snprintf(res, len, "error: Dynamic lift not available (unknown app version or disabled)");
        return;
    }
    if (strncmp(c, "reset", 5) == 0) {
        pthread_mutex_lock(&t_lock);
        dl_ctl_reset(&dl_ctl);
        pthread_mutex_unlock(&t_lock);
        return;
    }
    if (((n = parse_nums(c, v, 6)) != 5 && n != 6) || !dl_valid(v) || (n == 6 && !dl_start_ok(v[5]))) {
        snprintf(res, len, "error: bad values (limit 100-20000, lower 50 to limit-1, steps 1-90 and 1-100, start layer 1-100000)");
        return;
    }
    if (v[0] > 0 && !(t_on && t_installed)) {
        snprintf(res, len, "error: Dynamic lift needs the two-stage fix (sglog/TILT2)");
        return;
    }
    pthread_mutex_lock(&t_lock);
    dl_cfg.on = (int)v[0];
    dl_cfg.limit = (int)v[1];
    dl_cfg.lower = (int)v[2];
    dl_cfg.slower = (int)v[3];
    dl_cfg.faster = (int)v[4];
    if (n == 6)
        dl_cfg.start = (int)v[5];
    pthread_mutex_unlock(&t_lock);
    dl_save();
    tc_reconcile();                    /* Dynamic lift works on the custom speeds: they apply while it is on */
}

static void sgtilt_dl_get(int v[5])      /* for the settings snapshot */
{
    v[0] = dl_cfg.on;
    v[1] = dl_cfg.limit;
    v[2] = dl_cfg.lower;
    v[3] = dl_cfg.slower;
    v[4] = dl_cfg.faster;
}

static int sgtilt_dl_json(char *buf, size_t len)
{
    int n;
    pthread_mutex_lock(&t_lock);
    n = snprintf(buf, len, ",\"dl\":{\"on\":%d,\"fix\":%d,\"lim\":%d,\"low\":%d,\"sl\":%d,\"fa\":%d,\"st\":%d,\"f\":%.3f,\"mode\":%d,\"h\":[",
                 dl_cfg.on, t_on && t_installed, dl_cfg.limit, dl_cfg.lower, dl_cfg.slower, dl_cfg.faster, dl_cfg.start, dl_ctl.f, dl_ctl.mode);
    for (int i = 0; i < dl_nh && n < (int)len; i++)
        n += snprintf(buf + n, len - (size_t)n, "%s[%d,%.0f,%.1f,%.1f,%d]", i ? "," : "", dl_hist[i].layer, dl_hist[i].peak,
                      dl_hist[i].start, dl_hist[i].sep, dl_hist[i].kind);
    pthread_mutex_unlock(&t_lock);
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "]}");
    return n;
}

static int sgtilt_gsync_json(char *buf, size_t len)
{
    int n = snprintf(buf, len, ",\"gsync\":{\"on\":%d,\"auto\":%d,\"off\":%d,\"state\":%d,\"pos\":%d,\"fix\":%d,\"paused\":%d,"
                     "\"poll\":%.1f,\"delay\":%.0f,\"avg\":%.0f,\"n\":%d,\"ratio\":[",
                     gs.on, gs.autos, gs.off_ms, gs.state, gs.pos, t_on && t_installed, gs.autos && dl_cfg.on, gp_dt_us / 1000.0, gs.delay_ms,
                     gs.delay_n ? gs.delay_sum / gs.delay_n : 0.0, gs.nrel);
    for (int i = 0; i < GS_NOFF && n < (int)len; i++)
        n += snprintf(buf + n, len - (size_t)n, "%.2f%s", gs.ratio[i], i < GS_NOFF - 1 ? "," : "");
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "]}");
    return n;
}

/* trace row: callers hold T_MUTEX (the wrapper and t_poll), t_flush takes it too */
static void t_row(const char *ev, int32_t x, long a, long b)
{
    if (!t_tracing || t_tlen > (int)sizeof t_tbuf - 64)
        return;
    t_tlen += snprintf(t_tbuf + t_tlen, sizeof t_tbuf - (size_t)t_tlen, "%.1f,%s,%ld,%ld,%ld\n",
                       raw_us() / 1000.0, ev, (long)x, a, b);
}

/* ---- Dynamic lift (sgdl.c ) ----
 * <csvdir>/DYNLIFT "ON LIMIT LOWER SLOWER FASTER" (g, g, %, %), command "dynlift ON LIMIT LOWER SLOWER FASTER" | "dynlift reset".
 * While it is on and a print runs (and the layer is not on a slow-layer list), the release of a layer starts at the adaptive start
 * speed (stage-1 speed set in the Tilt card x f, 10 deg/s .. the set speed) and the chip's VMAX is switched to the stage-2 (fast) speed
 * set in the Tilt card when the layer has separated (gauge, sgdl.c) or when there is no peel. Any stage angle (F / Y) is ignored:
 *   - a release of two stages (angle set): stage 1 gets the start speed, stage 2 only sets the final target (as the two-stage fix does),
 *     the speed switch is made by the separation instead of the angle;
 *   - a release of one stage (angle 0): the single move gets the start speed and the switch is armed after the call.
 * After the release the peak of its force feeds the controller (reset at every job start). Needs the two-stage fix (the move wrapper). */
static volatile uint32_t t_tgen;           /* bumped on every move of the tilt axis */
static struct {                            /* the release being watched: under T_MUTEX */
    int armed, pair_ok, pair_obs, passive, switched, layer;   /* passive = only watched (Dynamic lift not acting): the peak is recorded, nothing is changed */
    uint32_t gen, vfast;
    int32_t x0, target;
    double base_ds, start_ds, base_raw;
    unsigned pos0;
    uint64_t deadline_us;
    struct dl_det det;
} dl;
struct dl_fin {
    int on, kind, layer, passive;
    double peak, base, start, sep;
};

/* the app's layer (0-based), -1 = not known. Dynamic lift uses the app's own state and layer, not the logger session
 * (which does not exist while job logging is off) */
static int dl_app_layer(void)
{
    uint32_t P;
    if (!w_ok)
        return -1;
    P = HW(DL_W_PPTR);
    return P ? (int)HW(P + DL_W_LAYER) : -1;
}

static int dl_ready(void)
{
    return dl_cfg.on && t_on && t_installed && tp_ok && h_ok && w_ok && HW(H_PSTATE) == 10 && !sl.active &&
           (dl_cfg.start <= 1 || dl_app_layer() + 1 >= dl_cfg.start);     /* before the start layer the set speeds are used as they are */
}

static void dl_job_check(void)             /* t_lock held: a new print (the state was not 'printing' in between) starts at the set speed */
{
    if (dl_idle) {
        dl_idle = 0;
        dl_ctl_reset(&dl_ctl);
        dl_nh = 0;
    }
}

/* the stage-1 speed (deg/s) of the block whose stage-2 speed is fast_ds (both blocks: by the layer), 0 = unknown */
static double dl_find_base(double fast_ds)
{
    uint32_t pr = HW(T_PROFPTR);
    int hit[2], b;
    if (pr != T_PROF && pr != T_PROF + 0x38)
        return 0;
    for (b = 0; b < 2; b++) {
        double d = (double)HF(pr + 0x0c + 0x18u * (unsigned)b) - fast_ds;
        hit[b] = d > -0.6 && d < 0.6;
    }
    if (hit[0] && hit[1])
        b = dl_app_layer() < (int)HW(pr + 0x34) ? 0 : 1;
    else if (hit[0])
        b = 0;
    else if (hit[1])
        b = 1;
    else
        return 0;
    return (double)HF(pr + 0x08 + 0x18u * (unsigned)b);
}

/* a printing job with the move wrapper active but Dynamic lift not acting (off, before its start layer, slow layers): the releases are only WATCHED so that
 * the Dynamic lift card shows their peel peaks anyway. Nothing is changed (no speed, no controller). */
static int dl_watch_ok(void)
{
    return t_on && t_installed && tp_ok && h_ok && w_ok && HW(H_PSTATE) == 10;
}

static int dl_observe(uint32_t speed)      /* the base values of a watched release; 0 = not possible */
{
    double raw, ds = speed * T_DEG;
    unsigned pos = sg_hist_pos();
    if (ds <= 0 || !sg_hist_mean(3, &raw))
        return 0;
    pthread_mutex_lock(&t_lock);
    dl_job_check();
    pthread_mutex_unlock(&t_lock);
    dl.base_ds = dl.start_ds = ds;
    dl.base_raw = raw;
    dl.pos0 = pos;
    dl.passive = 1;
    return 1;
}

/* before a release move: kind 1 = stage 1 of a pair (speed = its speed), 2 = the single move (speed = the fast speed).
 * Changes *speed to the start speed; *vfast = the speed to switch to (steps/s), 0 = no switch. 0 = not handled */
static int dl_decide(int kind, uint32_t *speed, uint32_t *vfast)
{
    double base, start, fast_ds = *speed * T_DEG;
    unsigned pos = sg_hist_pos();
    double raw;
    base = kind == 1 ? fast_ds : dl_find_base(fast_ds);
    if (base <= 0 || !sg_hist_mean(3, &raw))
        return 0;
    pthread_mutex_lock(&t_lock);
    dl_job_check();
    start = dl_start_ds(&dl_ctl, base);
    pthread_mutex_unlock(&t_lock);
    dl.passive = 0;
    dl.base_ds = base;
    dl.start_ds = start;
    dl.base_raw = raw;
    dl.pos0 = pos;
    *vfast = 0;
    if (kind == 1) {
        *speed = (uint32_t)(start / T_DEG + 0.5);
    } else if (start + 0.01 < fast_ds) {
        *vfast = *speed;
        *speed = (uint32_t)(start / T_DEG + 0.5);
    }
    return 1;
}

static void dl_arm(int32_t x0, int32_t target, uint32_t vfast)    /* T_MUTEX held */
{
    dl.armed = 1;
    dl.gen = t_tgen;
    dl.x0 = x0;
    dl.target = target;
    dl.vfast = vfast;
    dl.switched = 0;
    dl.layer = dl_app_layer() + 1;
    {   /* the watch ends at 1.5 x the time the slow start speed needs for the whole stroke + 2 s (the floor is 10 deg/s: ~14 s) */
        double span = (double)(target >= x0 ? target - x0 : x0 - target) * T_DEG, v = dl.start_ds > 1 ? dl.start_ds : 1;
        dl.deadline_us = raw_us() + (uint64_t)((span / v * 1.5 + 2.0) * 1e6);
    }
    dl_det_start(&dl.det, dl.base_raw, s_k);
    tcsv_dl(1, 0, 0, 0, 0);
}

/* one poll of the watched release (T_MUTEX held): 1 = still watching, 0 = over (*f says what to feed the controller) */
static int dl_poll(struct dl_fin *f)
{
    int32_t raw[8], x;
    int n, fin = 0, fire = 0;
    double deg;
    if (!dl.armed)
        return 0;
    x = t_read(R_XACTUAL);
    deg = (double)(x - dl.x0) * T_DEG;
    if (t_tgen != dl.gen || raw_us() > dl.deadline_us) {
        fin = 1;
    } else {
        fin = x == dl.target;
        n = sg_hist_since(&dl.pos0, raw, 8);
        for (int i = 0; i < n; i++)
            if (dl_det_feed(&dl.det, (double)raw[i], deg))
                fire = 1;
        if (dl_det_angle(&dl.det, deg))
            fire = 1;
        if (fire && !dl.switched && dl.vfast && !fin) {
            if (t_read(R_XTARGET) == dl.target) {
                t_write(R_VMAX, (int32_t)dl.vfast);
                dl.switched = 1;
                t_row("DL", x, dl.target, (long)dl.vfast);
            } else {
                fin = 1;               /* the app retargeted */
            }
        }
    }
    if (!fin)
        return 1;
    f->on = 1;
    f->passive = dl.passive;
    f->kind = dl.det.sep ? 0 : dl.det.nopeel ? 1 : 2;
    f->layer = dl.layer;
    f->peak = dl.det.peak < 0 ? -dl.det.peak : 0;
    f->base = dl.base_ds;
    f->start = dl.start_ds;
    f->sep = dl.det.sep_deg;
    dl.armed = 0;
    return 0;
}

static void dl_finish(const struct dl_fin *f)     /* no SPI lock held */
{
    tcsv_dl(0, f->kind, f->sep, f->peak, f->base > 0 ? f->start / f->base : 0);
    pthread_mutex_lock(&t_lock);
    dl_job_check();
    if (!f->passive)
        dl_ctl_update(&dl_ctl, &dl_cfg, f->base, f->peak);
    if (dl_nh == 14) {
        memmove(dl_hist, dl_hist + 1, 13 * sizeof dl_hist[0]);
        dl_nh = 13;
    }
    dl_hist[dl_nh].layer = f->layer;
    dl_hist[dl_nh].kind = f->kind;
    dl_hist[dl_nh].peak = f->peak;
    dl_hist[dl_nh].start = f->start;
    dl_hist[dl_nh].sep = f->sep;
    dl_nh++;
    pthread_mutex_unlock(&t_lock);
}

/* the stock move op for every move, except the stage 2 after a stage 1 */
static int t_move(int idx, int32_t step, uint32_t speed, int wait)
{
    uint64_t ms;
    int fixed = 0;
    ms = raw_us() / 1000;
    int dlc = 0, dlo = 0;              /* Dynamic lift: 1 stage 1 of a pair, 2 a single-stage release, 3 stage 2 of a DL pair; dlo = the same classes for a release that is only watched */
    uint32_t dl_vfast = 0;
    /* gauge sync: hold the start of a release (a positive move that is not the stage 2 of a release) for the poll clock.
     * NB the chip's XACTUAL is -19911 at rest and 0 at the end of the release (tilt trace ), so the start cannot
     * be told from the position. */
    if (idx == T_IDX && t_on && step > 200 && !(!wait && t_st1.armed && ms - t_st1.ms <= T_ARM_MS))
        gs_release_delay();
    if (idx != T_IDX || !t_on) {
        if (idx == T_IDX)
            tcsv_note(step, speed, 0, wait);
        t_lock_spi();                  /* a pending switch belongs to an older move */
        t_gen++;
        t_sw.pending = 0;
        t_st1.armed = 0;
        t_unlock_spi();
        return ((t_move_fn)(uintptr_t)T_MOVE)(idx, step, speed, wait);
    }
    t_tgen++;
    if (dl_ready()) {
        int stage2 = !wait && t_st1.armed && ms - t_st1.ms <= T_ARM_MS;
        double tot = (double)HF(DL_MACH + 0x58) / T_DEG;
        if (tot < 5000 || tot > 40000)
            tot = 19911;
        if (wait && step > 200)
            dlc = 1;
        else if (stage2 && dl.pair_ok)
            dlc = 3;
        else if (!wait && !stage2 && step >= 0.9 * tot && step <= 1.05 * tot)
            dlc = 2;
        if ((dlc == 1 || dlc == 2) && !dl_decide(dlc, &speed, &dl_vfast))
            dlc = 0;
        if (dlc == 3)                  /* the stage-2 speed is the fast one, unless the start speed is not lower */
            dl_vfast = dl.start_ds + 0.01 < speed * T_DEG ? speed : 0;
    } else if (dl_watch_ok()) {
        int stage2 = !wait && t_st1.armed && ms - t_st1.ms <= T_ARM_MS;
        double tot = (double)HF(DL_MACH + 0x58) / T_DEG;
        if (tot < 5000 || tot > 40000)
            tot = 19911;
        if (wait && step > 200)
            dlo = dl_observe(speed) ? 1 : 0;
        else if (stage2 && dl.pair_obs)
            dlo = 3;
        else if (!wait && !stage2 && step >= 0.9 * tot && step <= 1.05 * tot)
            dlo = dl_observe(speed) ? 2 : 0;
    }
    tcsv_note(step, speed, dl_vfast, wait);
    t_lock_spi();                      /* the stock op takes it itself: released before */
    t_gen++;
    t_sw.pending = 0;
    if (!wait && t_st1.armed && ms - t_st1.ms <= T_ARM_MS) {
        /* stage 2: the target the app believes in = stage 1's target + step */
        int32_t x = t_read(R_XACTUAL), t2 = t_st1.t1 + step;
        int dir = t_st1.t1 >= t_st1.x0 ? 1 : -1;
        int32_t lead = (int32_t)(t_lead_deg / T_DEG);
        t_write(R_XTARGET, t2);        /* VMAX stays stage 1's for now */
        t_write(R_TZEROW, T_TZW_STOCK);
        if (dlo == 3)                  /* only watched: the stock two-stage handling below stays as it is */
            dl_arm(t_st1.x0, t2, 0);
        if (dlc == 3) {                /* Dynamic lift: the switch comes from the separation, not from the angle */
            dl_arm(t_st1.x0, t2, dl_vfast);
            t_row("DL2", x, t2, (long)dl_vfast);
        } else if (step == 0 || (step > 0 ? 1 : -1) != dir ||
            (x - (t_st1.t1 - dir * lead)) * dir >= 0) {
            t_write(R_VMAX, (int32_t)speed);   /* reversing, or the angle is passed: now */
            t_row("S2N", x, t2, (long)speed);
        } else {
            t_sw.pending = 1;
            t_sw.gen = t_gen;
            t_sw.v2 = speed;
            t_sw.t1 = t_st1.t1;
            t_sw.t2 = t2;
            t_sw.lead = lead;
            t_sw.dir = dir;
            t_sw.deadline_us = raw_us() + 2000000u +
                2u * (uint64_t)((double)((t_st1.t1 - x) * dir) / t_st1.v1 * 1e6);
            t_row("S2", x, t2, (long)speed);
        }
        t_tr.target = t2;
        fixed = 1;
    }
    t_st1.armed = 0;
    t_unlock_spi();
    if (!fixed)
        ((t_move_fn)(uintptr_t)T_MOVE)(idx, step, speed, wait);
    t_lock_spi();
    if (!fixed) {
        int32_t tg = t_read(R_XTARGET);
        if (wait) {                    /* stage 1: keep its target */
            t_st1.armed = 1;
            t_st1.t1 = tg;
            t_st1.x0 = tg - step;
            t_st1.v1 = speed ? speed : 1;
            t_st1.ms = ms;
        }
        if (dlc == 2)
            dl_arm(tg - step, tg, dl_vfast);
        else if (dlo == 2)
            dl_arm(tg - step, tg, 0);
        t_row(wait ? "S1" : "M", tg - step, tg, (long)speed);
        t_tr.target = tg;
    }
    dl.pair_ok = dlc == 1;             /* the next call (stage 2) belongs to this pair */
    dl.pair_obs = dlo == 1;
    if (t_tracing && !t_tr.active) {
        t_tr.active = 1;
        t_tr.t0_us = raw_us();
        t_tr.still = 0;
        t_tr.last_x = 0x7fffffff;
    }
    t_unlock_spi();
    pthread_mutex_lock(&t_lock);
    t_stat.n2 += fixed;
    t_kick = 1;
    pthread_cond_signal(&t_cond);
    pthread_mutex_unlock(&t_lock);
    return 0;
}

static void t_flush(void)
{
    static char out[sizeof t_tbuf];
    int n;
    t_lock_spi();                      /* the rows are written under it */
    n = t_tlen;
    memcpy(out, t_tbuf, (size_t)n);
    t_tlen = 0;
    t_unlock_spi();
    if (n <= 0)
        return;
    if (t_tfd < 0) {
        char path[192], ts[32];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tm);
        snprintf(path, sizeof path, "%s/tilt_%s.csv", sg_csvdir, ts);
        mkdir(sg_csvdir, 0755);
        t_tfd = real_open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (t_tfd >= 0)
            real_write(t_tfd, "ms,ev,x,a,b\n", 12);
    }
    if (t_tfd >= 0)
        real_write(t_tfd, out, (size_t)n);
}

/* one poll: the pending VMAX switch and the trace sample. 1 = still busy */
static int t_poll(void)
{
    int busy = 0, done = 0, give = 0, cancel = 0;
    double sw_deg = 0, late_ms = 0;
    struct dl_fin dlf = {0};
    uint64_t now;
    t_lock_spi();
    now = raw_us();
    if (t_sw.pending) {
        int32_t x = t_read(R_XACTUAL);
        if (t_sw.gen != t_gen) {
            t_sw.pending = 0;
            cancel = 1;
        } else if ((x - (t_sw.t1 - t_sw.dir * t_sw.lead)) * t_sw.dir >= 0) {
            if (t_read(R_XTARGET) == t_sw.t2) {
                t_write(R_VMAX, (int32_t)t_sw.v2);
                done = 1;
                sw_deg = x * T_DEG;
                late_ms = (double)((x - t_sw.t1) * t_sw.dir) * 1000.0 /
                          (t_st1.v1 ? t_st1.v1 : 1);
                t_row("SW", x, t_sw.t2, (long)t_sw.v2);
            } else {
                cancel = 1;
                t_row("CX", x, t_read(R_XTARGET), 0);
            }
            t_sw.pending = 0;
        } else if (now > t_sw.deadline_us) {
            t_sw.pending = 0;
            give = 1;
            t_row("GV", x, t_sw.t2, 0);
        }
        busy |= t_sw.pending;
    }
    if (t_tr.active) {
        int32_t x = t_read(R_XACTUAL);
        t_row("P", x, t_tr.target, 0);
        if (t_drv && ++t_tr.ndrv % T_DRV_EVERY == 0) {   /* x = XACTUAL, a = DRV_STATUS (unsigned), b = PWM_SCALE */
            uint32_t ds = (uint32_t)t_read(R_DRVSTAT);
            t_row("D", x, (long)ds, (long)(t_read(R_PWMSCALE) & 0xff));
        }
        t_tr.still = x == t_tr.last_x && x == t_tr.target ? t_tr.still + 1 : 0;
        t_tr.last_x = x;
        if (t_tr.still >= 3 || now - t_tr.t0_us > T_TRACE_MAX_MS * 1000u || !t_tracing)
            t_tr.active = 0;
        busy |= t_tr.active;
    }
    busy |= dl_poll(&dlf);
    t_unlock_spi();
    if (dlf.on)
        dl_finish(&dlf);
    if (done || give || cancel) {
        pthread_mutex_lock(&t_lock);
        t_stat.nsw += done;
        t_stat.ngive += give;
        t_stat.ncancel += cancel;
        if (done) {
            t_stat.last_sw_deg = sw_deg;
            t_stat.last_late_ms = late_ms;
        }
        pthread_mutex_unlock(&t_lock);
    }
    return busy;
}

static void *tilt_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int busy;
        struct timespec ts;
        pthread_mutex_lock(&t_lock);
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;                /* a finished trace is flushed within 1 s */
        while (!t_kick && pthread_cond_timedwait(&t_cond, &t_lock, &ts) == 0)
            ;
        t_kick = 0;
        pthread_mutex_unlock(&t_lock);
        do {
            busy = t_poll();
            if (busy)
                usleep(T_POLL_US);
        } while (busy);
        t_flush();
    }
    return NULL;
}

static void sgtilt_init(void)
{
    if (!in_chitu || access("/customer/mod/sgtilt_disable", F_OK) == 0)
        return;
    for (unsigned i = 0; i < sizeof t_sig / sizeof t_sig[0]; i++)
        if (HW(t_sig[i].a) != t_sig[i].w)
            return;
    t_ok = 1;
    tr_ok = 1;
    for (unsigned i = 0; i < sizeof tr_sig / sizeof tr_sig[0]; i++)
        if (HW(tr_sig[i].a) != tr_sig[i].w)
            tr_ok = 0;
    for (unsigned i = 0; i < sizeof tp_sig / sizeof tp_sig[0]; i++)
        if (HW(tp_sig[i].a) != tp_sig[i].w)
            return;
    tp_ok = 1;
}

/* v = N, A..Y (the command's order = the profile's order after N): 1 = in range */
static int tp_valid(const double v[13])
{
    static const char ang[13] = {0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1};   /* E F X Y */
    if (v[0] != (int)v[0] || v[0] < 0 || v[0] > T_NMAX)
        return 0;
    for (int i = 1; i < 13; i++) {
        int x = (int)v[i];
        if (v[i] != x || (x & 1))
            return 0;
        if (ang[i] ? x != 0 && (x < 10 || x > 120) : x < 10 || x > 144)
            return 0;
    }
    return 1;
}

static int tp_two_stage(const double v[13])
{
    return v[5] > 0 || v[6] > 0 || v[11] > 0 || v[12] > 0;
}

/* check and queue a profile: 1 = queued, else res = the error */
static int sgtilt_queue(const double v[13], char *res, size_t len)
{
    if (!tp_ok || !h_ok || !w_ok) {
        snprintf(res, len, "error: tilt settings not available (unknown app version or disabled)");
        return 0;
    }
    if (!tp_valid(v)) {
        snprintf(res, len, "error: tilt values out of range");
        return 0;
    }
    if (tp_two_stage(v) && !(t_on && t_installed)) {
        snprintf(res, len, "error: a stage angle needs the two-stage fix (sglog/TILT2)");
        return 0;
    }
    pthread_mutex_lock(&t_lock);
    memcpy(tp_req.v, v, sizeof tp_req.v);
    tp_req.pending = 1;
    snprintf(tp_req.res, sizeof tp_req.res, "waiting for the wait after retract");
    pthread_mutex_unlock(&t_lock);
    return 1;
}

/* "N A B C D E F S P R T X Y" | "stock" */
/* ---- custom tilt speeds ----
 * The profile set in the Tilt speed card (tc_v, <csvdir>/TILTCUSTOM "N A B C D E F S P R T X Y") is what the printer uses when the switch is on
 * (absent <csvdir>/TILTOFF) or while Dynamic lift is on (it works on the set speeds); with the switch off and no Dynamic lift the stock profile applies.
 * The live profile is brought to the wanted one by the old queue (applied in the wait after retract or when idle, saved by the app's saver).
 * First run (no TILTCUSTOM yet): the live profile becomes the custom one, switch on unless it equals stock. */
#define TC_FILE "TILTCUSTOM"
#define TC_OFF  "TILTOFF"
static double tc_v[13];                /* under t_lock */
static int tc_have, tc_on = 1, tc_loaded;     /* tc_loaded: the files were read once (reconcile waits for it) */

static int tc_same(const double *a, const double *b)
{
    for (int i = 0; i < 13; i++)
        if (a[i] - b[i] > 0.01 || b[i] - a[i] > 0.01)
            return 0;
    return 1;
}

static void tc_save(void)              /* from copies: no lock held */
{
    char path[160], line[160];
    double v[13];
    int on, n = 0, fd;
    pthread_mutex_lock(&t_lock);
    memcpy(v, tc_v, sizeof v);
    on = tc_on;
    pthread_mutex_unlock(&t_lock);
    for (int i = 0; i < 13; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, "%s%d", i ? " " : "", (int)v[i]);
    n += snprintf(line + n, sizeof line - (size_t)n, "\n");
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, TC_FILE);
    write_small(path, line, n);
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, TC_OFF);
    if (on)
        unlink(path);
    else if ((fd = real_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644)) >= 0)
        real_close(fd);
}

static void tc_load(void)
{
    char path[160], buf[160];
    double v[13];
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, TC_FILE);
    pthread_mutex_lock(&t_lock);
    if (read_small(path, buf, sizeof buf) > 0 && parse_nums(buf, v, 13) == 13 && tp_valid(v)) {
        memcpy(tc_v, v, sizeof tc_v);
        tc_have = 1;
    }
    if (tc_have) {
        snprintf(path, sizeof path, "%s/%s", sg_csvdir, TC_OFF);
        tc_on = access(path, F_OK) != 0;
    }
    tc_loaded = 1;
    pthread_mutex_unlock(&t_lock);
}

/* bring the live profile to the wanted one (custom or stock): queued, applied in the next wait or when idle */
static void tc_reconcile(void)
{
    double want[13], live[13];
    char res[96];
    int init = 0;
    if (!tp_ok || !h_ok || !w_ok)
        return;
    if (!tc_loaded)                    /* wait for tc_load (the first tick loads before it reconciles) */
        return;
    if (!tc_have) {                    /* first run (no TILTCUSTOM file at all): the printer's profile becomes the custom one; an unreadable file is left alone */
        char fp[160];
        snprintf(fp, sizeof fp, "%s/%s", sg_csvdir, TC_FILE);
        if (access(fp, F_OK) == 0 || sl.active || !sgtilt_read(live) || !tp_valid(live))
            return;
        pthread_mutex_lock(&t_lock);
        memcpy(tc_v, live, sizeof tc_v);
        tc_have = 1;
        tc_on = !tc_same(live, tp_stock);
        pthread_mutex_unlock(&t_lock);
        init = 1;
    }
    if (init)
        tc_save();
    if (!tc_have || sl.active)         /* the live profile holds the slow set while a slow layer runs */
        return;
    pthread_mutex_lock(&t_lock);
    memcpy(want, (tc_on || dl_cfg.on) ? tc_v : tp_stock, sizeof want);
    if (tp_req.pending && tc_same(tp_req.v, want)) {
        pthread_mutex_unlock(&t_lock);
        return;                        /* already waiting to be applied */
    }
    pthread_mutex_unlock(&t_lock);
    if (!sgtilt_read(live) || (!tp_req.pending && tc_same(live, want)))
        return;
    sgtilt_queue(want, res, sizeof res);
}

static int sgtilt_tc_get(double v[13], int *on)     /* for the settings snapshot */
{
    int have;
    pthread_mutex_lock(&t_lock);
    have = tc_have;
    memcpy(v, tc_v, 13 * sizeof v[0]);
    *on = tc_on;
    pthread_mutex_unlock(&t_lock);
    return have;
}

static void sgtilt_tc_set(const double v[13], int on)
{
    pthread_mutex_lock(&t_lock);
    memcpy(tc_v, v, sizeof tc_v);
    tc_have = 1;
    tc_on = on;
    pthread_mutex_unlock(&t_lock);
    tc_save();
    tc_reconcile();
}

/* "tilt N A B C D E F S P R T X Y": the custom values (stored; applied when the switch or Dynamic lift is on) | "tilt stock" = switch off */
static void sgtilt_cmd(const char *c, char *res, size_t len)
{
    double v[13];
    if (strncmp(c, "stock", 5) == 0 && (c[5] == 0 || c[5] == '\n')) {
        pthread_mutex_lock(&t_lock);
        tc_on = 0;
        pthread_mutex_unlock(&t_lock);
        tc_save();
        tc_reconcile();
        return;
    }
    if (parse_nums(c, v, 13) != 13) {
        snprintf(res, len, "error: bad tilt command");
        return;
    }
    if (!tp_ok || !h_ok || !w_ok) {
        snprintf(res, len, "error: tilt settings not available (unknown app version or disabled)");
        return;
    }
    if (!tp_valid(v)) {
        snprintf(res, len, "error: tilt values out of range");
        return;
    }
    if (tp_two_stage(v) && !(t_on && t_installed)) {
        snprintf(res, len, "error: a stage angle needs the two-stage fix (not installed yet)");
        return;
    }
    pthread_mutex_lock(&t_lock);
    memcpy(tc_v, v, sizeof tc_v);
    tc_have = 1;
    pthread_mutex_unlock(&t_lock);
    tc_save();
    tc_reconcile();
}

/* ---- X Move (manual tilt, a test tool ): "xmove DEG" moves the tilt to DEG (0-140) at 36 deg/s, printer idle only ----
 * The app's own absolute move is used (axis_move 0x5361c, as the plate move does), so the app's tracked position (axis struct +0x50, steps; 0 = flat,
 * 19911 = 140 deg) stays right. Idle = printer state 3 (H_PSTATE) and no logger session; the tilt axis must be homed. live.json "xm". */
#define XM_PTR   0x2fa124u + 4u        /* axis structs [axis]; the tilt is axis 1 */
#define XM_SPEED 36.0f                 /* deg/s: the stock speed, no ramp problems */
#define XM_IDLE  3                     /* H_PSTATE at idle */
static int xm_busy;
static uint64_t xm_until;              /* us: the move should be over */
static double xm_target;

static uint32_t xm_axis(double *dps)   /* the tilt axis struct when it looks right, else 0 */
{
    uint32_t p = HW(XM_PTR);
    double d;
    if (p < 0x100000u || p > 0x10000000u || (p & 3))
        return 0;
    d = *(volatile double *)(uintptr_t)(p + 0x18);
    if (!(d > 0.005 && d < 0.009) || HI(p + 0x3c) < 15000 || HI(p + 0x3c) > 25000)
        return 0;
    *dps = d;
    return p;
}

/* NULL = the tilt may be moved by hand; else why not. *pos = the app's tracked position in degrees */
static const char *xm_why(double *pos)
{
    double d;
    uint32_t p;
    if (!h_ok || !in_chitu)
        return "tilt control not available";
    if (HW(H_PSTATE) != XM_IDLE || s_active)
        return "the printer is not idle";
    if (!(p = xm_axis(&d)))
        return "tilt axis not found";
    if (HB(p + 0x49) != 3 || !HB(p + 0x4a))
        return "the tilt axis is not homed";
    *pos = HI(p + 0x50) * d;
    if (*pos < -2 || *pos > 142)
        return "tilt position unknown";
    return NULL;
}

static void sgtilt_xmove_cmd(const char *c, char *res, size_t len)
{
    double v[1], pos = 0, dist;
    const char *why;
    void (*move)(int, int, float, float) = (void (*)(int, int, float, float))(uintptr_t)Z_MOVE;
    if (parse_nums(c, v, 1) != 1 || v[0] < 0 || v[0] > 140) {
        snprintf(res, len, "error: xmove DEG (0-140)");
        return;
    }
    if ((why = xm_why(&pos))) {
        snprintf(res, len, "error: %s", why);
        return;
    }
    if (xm_busy && now_us() < xm_until) {
        snprintf(res, len, "error: the tilt is still moving");
        return;
    }
    dist = v[0] > pos ? v[0] - pos : pos - v[0];
    if (dist < 0.02) {
        snprintf(res, len, "ok: already at %.1f deg", pos);
        return;
    }
    xm_target = v[0];
    xm_until = now_us() + (uint64_t)((dist / XM_SPEED + 1.5) * 1e6);
    xm_busy = 1;
    move(1, 0, (float)v[0], XM_SPEED);
}

static void xm_tick(void)              /* every 0.5 s */
{
    if (xm_busy && (now_us() >= xm_until || HW(H_PSTATE) != XM_IDLE || s_active))
        xm_busy = 0;
}

static int sgtilt_xm_json(char *buf, size_t len)
{
    double pos = 0, d;
    uint32_t p;
    const char *why;
    if (!h_ok)
        return 0;
    why = xm_why(&pos);
    if (!why || xm_axis(&d)) {
        if ((p = xm_axis(&d)))
            pos = HI(p + 0x50) * d;
    }
    return snprintf(buf, len, ",\"xm\":{\"pos\":%.1f,\"idle\":%d,\"busy\":%d,\"why\":\"%s\"}", pos, why == NULL, xm_busy && now_us() < xm_until,
                    why ? why : "");
}

/* "tiltmode 0|1": custom tilt speeds off (stock) / on (the values of the card) */
static void sgtilt_mode_cmd(const char *c, char *res, size_t len)
{
    if (c[0] != '0' && c[0] != '1') {
        snprintf(res, len, "error: bad tiltmode command");
        return;
    }
    if (!tp_ok || !h_ok || !w_ok) {
        snprintf(res, len, "error: tilt settings not available (unknown app version or disabled)");
        return;
    }
    pthread_mutex_lock(&t_lock);
    tc_on = c[0] == '1';
    pthread_mutex_unlock(&t_lock);
    tc_save();
    tc_reconcile();
}

static void sgtilt_stock(double v[13])
{
    memcpy(v, tp_stock, sizeof tp_stock);
}

/* the live profile as N, A..Y (0 = not available) */
static int sgtilt_read(double v[13])
{
    if (!tp_ok)
        return 0;
    v[0] = HW(T_PROF + 0x34);
    for (int i = 0; i < 12; i++)
        v[1 + i] = HF(T_PROF + 4u * i);
    return 1;
}

/* slow set: speeds 10-144 even, angles (E F) 0 or 10-120 even */
static int sl_valid(const double v[6])
{
    for (int i = 0; i < 6; i++) {
        int x = (int)v[i];
        if (v[i] != x || (x & 1))
            return 0;
        if (i >= 4 ? x != 0 && (x < 10 || x > 120) : x < 10 || x > 144)
            return 0;
    }
    return 1;
}

/* the slow set into both blocks of profile pr (caller holds sl_lock) */
static void sl_write(uint32_t pr)
{
    int two = t_on && t_installed;
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 6; i++)
            HF(pr + 4u * (6 * b + i)) = (float)(i >= 4 && !two ? 0 : sl.set[i]);
}

static void sl_restore(void)           /* caller holds sl_lock */
{
    if (!sl.active)
        return;
    for (int i = 0; i < 12; i++)
        HF(sl.prof + 4u * i) = sl.base[i];
    sl.active = 0;
}

static void sl_load_set(void)
{
    char path[160], buf[64];
    double v[6];
    snprintf(path, sizeof path, "%s/SLOWTILT", sg_csvdir);
    if (read_small(path, buf, sizeof buf) > 0 && parse_nums(buf, v, 6) == 6 && sl_valid(v)) {
        pthread_mutex_lock(&sl_lock);
        memcpy(sl.set, v, sizeof sl.set);
        pthread_mutex_unlock(&sl_lock);
    }
}

/* set + save the slow set: 1 = ok, else res = the error */
static int sl_apply_set(const double v[6], char *res, size_t len)
{
    char path[160], line[64];
    int n;
    if (!tp_ok || !h_ok || !w_ok) {
        snprintf(res, len, "error: tilt settings not available (unknown app version or disabled)");
        return 0;
    }
    if (!sl_valid(v)) {
        snprintf(res, len, "error: slow set values out of range");
        return 0;
    }
    if ((v[4] > 0 || v[5] > 0) && !(t_on && t_installed)) {
        snprintf(res, len, "error: a stage angle needs the two-stage fix (sglog/TILT2)");
        return 0;
    }
    pthread_mutex_lock(&sl_lock);
    memcpy(sl.set, v, sizeof sl.set);
    pthread_mutex_unlock(&sl_lock);
    n = snprintf(line, sizeof line, "%d %d %d %d %d %d\n", (int)v[0], (int)v[1], (int)v[2],
                 (int)v[3], (int)v[4], (int)v[5]);
    snprintf(path, sizeof path, "%s/SLOWTILT", sg_csvdir);
    if (!write_small(path, line, n)) {
        snprintf(res, len, "error: set, but saving %s failed", path);
        return 0;
    }
    return 1;
}

static void sgtilt_slow_get(double v[6])
{
    pthread_mutex_lock(&sl_lock);
    memcpy(v, sl.set, sizeof sl.set);
    pthread_mutex_unlock(&sl_lock);
}

static void sgtilt_slow_cmd(const char *c, char *res, size_t len)
{
    double v[6];
    if (parse_nums(c, v, 6) != 6) {
        snprintf(res, len, "error: bad slowset command");
        return;
    }
    sl_apply_set(v, res, len);
}

static int sl_cmp(const void *a, const void *b)
{
    return ((const int *)a)[0] - ((const int *)b)[0];
}

/* parse a layer list (same rules as sgwebd.tcl): ranges sorted + merged into sl.r, the
 * number of layers in sl.nl; 0 = error in sl.err (caller holds sl_lock) */
static int sl_parse(const char *p)
{
    int line = 1, n = 0;
    sl.nr = sl.nl = 0;
    sl.err[0] = 0;
    for (;;) {
        char tok[24];
        int k = 0, a = 0, b = 0, dash = 0, bad = 0;
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n' || *p == '#') {
            if (*p == '#')
                while (*p && *p != '\n')
                    p++;
            if (*p == '\n')
                line++;
            if (*p)
                p++;
        }
        if (!*p)
            break;
        while (*p && !strchr(" \t,\r\n#", *p)) {
            if (k < (int)sizeof tok - 1)
                tok[k++] = *p;
            if (*p == '-' && !dash)
                dash = 1;
            else if (*p >= '0' && *p <= '9' && (dash ? b : a) < 1000000)
                *(dash ? &b : &a) = (dash ? b : a) * 10 + (*p - '0');
            else
                bad = 1;
            p++;
        }
        tok[k] = 0;
        if (!dash)
            b = a;
        if (bad || !a || !b || tok[0] == '0' || (dash && (tok[k - 1] == '-' || strstr(tok, "-0"))) ||
            a > 100000 || b > 100000 || b < a) {
            snprintf(sl.err, sizeof sl.err, "line %d: '%s' is not a layer or range%s", line, tok,
                     !bad && dash && b && b < a ? " (reversed)" : "");
            sl.nr = 0;
            return 0;
        }
        if (n == SL_MAXR) {
            snprintf(sl.err, sizeof sl.err, "more than %d ranges", SL_MAXR);
            sl.nr = 0;
            return 0;
        }
        sl.r[n][0] = a;
        sl.r[n][1] = b;
        n++;
    }
    qsort(sl.r, (size_t)n, sizeof sl.r[0], sl_cmp);
    for (int i = 0; i < n; i++) {      /* merge overlapping / touching ranges */
        if (sl.nr && sl.r[i][0] <= sl.r[sl.nr - 1][1] + 1) {
            if (sl.r[i][1] > sl.r[sl.nr - 1][1])
                sl.r[sl.nr - 1][1] = sl.r[i][1];
            continue;
        }
        sl.r[sl.nr][0] = sl.r[i][0];
        sl.r[sl.nr][1] = sl.r[i][1];
        sl.nr++;
    }
    for (int i = 0; i < sl.nr; i++)
        sl.nl += sl.r[i][1] - sl.r[i][0] + 1;
    return 1;
}

static int sl_listed(int l)            /* caller holds sl_lock */
{
    for (int i = 0; i < sl.nr; i++)
        if (l >= sl.r[i][0] && l <= sl.r[i][1])
            return 1;
    return 0;
}

static int sl_next(int l)              /* first listed layer > l, 0 = none */
{
    for (int i = 0; i < sl.nr; i++)
        if (sl.r[i][1] > l)
            return sl.r[i][0] > l ? sl.r[i][0] : l + 1;
    return 0;
}

/* <csvdir>/SLOWOFF present = slow layers off (the lists are ignored); "slowlayers 0|1" */
static int sl_enabled(void)
{
    char path[160];
    snprintf(path, sizeof path, "%s/SLOWOFF", sg_csvdir);
    return access(path, F_OK) != 0;
}

static void sgtilt_slowen_cmd(const char *c, char *res, size_t len)
{
    char path[160];
    int fd;
    snprintf(path, sizeof path, "%s/SLOWOFF", sg_csvdir);
    if (c[0] == '1')
        unlink(path);
    else if (c[0] == '0') {
        if ((fd = real_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644)) >= 0)
            real_close(fd);
    } else {
        snprintf(res, len, "error: bad slowlayers command");
    }
}

/* "tilt2 0|1": the two-stage fix option file (TILT2). Off is refused while the live profile or the slow set
 * has a stage angle: without the fix the stock two-stage move overshoots. */
static void sgtilt_fix_cmd(const char *c, char *res, size_t len)
{
    /* the two-stage fix is always on since: "tilt2 1" is accepted, "tilt2 0" refused */
    if (c[0] == '1')
        t_on = !t_fail;
    else if (c[0] == '0')
        snprintf(res, len, "error: the two-stage fix is always on");
    else
        snprintf(res, len, "error: bad tilt2 command");
}

/* wait thread, at the start of every wait after retract: layer0 = the app's layer (0-based).
 * The list is read again each time (once per layer), so an edit applies to the next layer. */
/* the layer list of the file being printed -> sl.stem / sl.nr / sl.nl (takes sl_lock itself). Called at every wait and every
 * 2 s while a job runs, so the dashboard never shows the list of the previous print (user bug report ). */
static void sl_read_list(void)
{
    static char buf[SL_MAXF];                     /* under sl_lock */
    char file[160], stem[128], path[320];
    const char *b;
    int n;
    file[0] = 0;
    print_file_from_fds(file, sizeof file);       /* the file being printed (as sglog.c) */
    if (!*file) {
        pthread_mutex_lock(&glock);
        snprintf(file, sizeof file, "%s", print_file);
        pthread_mutex_unlock(&glock);
    }
    b = strrchr(file, '/');
    snprintf(stem, sizeof stem, "%s", b ? b + 1 : file);
    if (strrchr(stem, '.'))
        *strrchr(stem, '.') = 0;
    snprintf(path, sizeof path, "%s/layertilt/%s.txt", sg_csvdir, stem);
    pthread_mutex_lock(&sl_lock);
    n = *stem && sl_enabled() ? read_small(path, buf, sizeof buf) : -1;   /* SLOWOFF: all lists ignored */
    snprintf(sl.stem, sizeof sl.stem, "%s", n >= 0 ? stem : "");
    if (n < 0) {
        sl.nr = sl.nl = 0;
        sl.err[0] = 0;
    } else {
        sl_parse(buf);
    }
    pthread_mutex_unlock(&sl_lock);
}

static void sgtilt_slow_wait(int layer0)
{
    uint32_t pr = HW(T_PROFPTR);
    if (!tp_ok || !h_ok)
        return;
    sl_read_list();
    pthread_mutex_lock(&sl_lock);
    if (layer0 + 1 <= sl.layer)                   /* a new print */
        sl.beeped = 0;
    sl.layer = layer0 + 1;
    sl.next = sl_next(sl.layer);
    if (sb_enabled()) {                           /* once per slow range, 2 layers before its first layer */
        int a = 0;
        for (int i = 0; i < sl.nr; i++)
            if (sl.r[i][0] > sl.layer && sl.r[i][0] <= sl.layer + 2) {
                a = sl.r[i][0];
                break;
            }
        if (a && a > sl.beeped) {                 /* a = layer + 1 only when the call 2 layers before was missed */
            sgbeep(150, 150, 2);
            sl.beeped = a;
        }
    }
    if (sl_listed(sl.layer) && (pr == T_PROF || pr == T_PROF + 0x38)) {
        if (!sl.active || sl.prof != pr) {
            sl_restore();
            for (int i = 0; i < 12; i++)
                sl.base[i] = HF(pr + 4u * i);
            sl.prof = pr;
        }
        sl_write(pr);
        sl.active = 1;
        sl.nslow++;
    } else {
        sl_restore();
    }
    pthread_mutex_unlock(&sl_lock);
}

/* "slowbeep 0|1|test": the option file / one test beep (idle printer only) */
static void sgtilt_beep_cmd(const char *c, char *res, size_t len)
{
    char path[160];
    int fd;
    if (!tp_ok) {
        snprintf(res, len, "error: beep not available (unknown app version or disabled)");
        return;
    }
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, B_FILE);
    if (c[0] == '1') {                 /* the command text may end with a newline */
        if ((fd = real_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644)) >= 0)
            real_close(fd);
    } else if (c[0] == '0') {
        unlink(path);
    } else if (strncmp(c, "test", 4) == 0) {
        if (HW(H_PSTATE) == 10 || s_active)
            snprintf(res, len, "error: not while a print job is running");
        else if (!sgbeep(150, 150, 2))
            snprintf(res, len, "error: the buzzer is not available (volume or frequency 0, or no beeper queue)");
    } else {
        snprintf(res, len, "error: bad slowbeep command");
    }
}

/* "tiltdump": read-only dump of the tilt driver's registers into <csvdir>/tiltregs_<time>.txt ("name reg value"),
 * idle printer only. Registers as in the TMC5160 map the chip's moves already use. NOT read: GSTAT (0x01), RAMP_STAT
 * (0x35), ENC_STATUS (0x3b) - their event bits clear on read. The app's read op reads twice and retries until two
 * reads agree (except XACTUAL / VACTUAL / RAMP_STAT / DRV_STATUS), so one call per register is enough.
 * Write-only registers (IHOLD_IRUN, PWMCONF, ...) may read as 0 on this clone. */
static const struct { uint8_t reg; const char *name; } td_regs[] = {
    {0x00, "GCONF"}, {0x02, "IFCNT"}, {0x04, "IOIN"}, {0x08, "FACTORY_CONF"}, {0x09, "SHORT_CONF"},
    {0x0a, "DRV_CONF"}, {0x0b, "GLOBAL_SCALER"}, {0x10, "IHOLD_IRUN"}, {0x11, "TPOWERDOWN"}, {0x12, "TSTEP"},
    {0x13, "TPWMTHRS"}, {0x14, "TCOOLTHRS"}, {0x15, "THIGH"}, {0x20, "RAMPMODE"}, {0x21, "XACTUAL"},
    {0x22, "VACTUAL"}, {0x23, "VSTART"}, {0x24, "A1"}, {0x25, "V1"}, {0x26, "AMAX"}, {0x27, "VMAX"},
    {0x28, "DMAX"}, {0x2a, "D1"}, {0x2b, "VSTOP"}, {0x2c, "TZEROWAIT"}, {0x2d, "XTARGET"}, {0x33, "VDCMIN"},
    {0x34, "SW_MODE"}, {0x68, "MSLUTSEL"}, {0x6a, "MSCNT"}, {0x6b, "MSCURACT"}, {0x6c, "CHOPCONF"},
    {0x6d, "COOLCONF"}, {0x6e, "DCCTRL"}, {0x6f, "DRV_STATUS"}, {0x70, "PWMCONF"}, {0x71, "PWM_SCALE"},
    {0x72, "PWM_AUTO"}, {0x73, "LOST_STEPS"},
};

static void sgtilt_dump_cmd(char *res, size_t len)
{
    char path[192], ts[32], buf[2048];
    time_t now = time(NULL);
    struct tm tm;
    int n, fd;
    if (!t_ok) {
        snprintf(res, len, "error: tilt driver code not verified (unknown app version or disabled)");
        return;
    }
    if (HW(H_PSTATE) == 10 || s_active) {
        snprintf(res, len, "error: not while a print job is running");
        return;
    }
    n = snprintf(buf, sizeof buf, "# tilt driver registers (idx %d), up %.1f s\n", T_IDX,
                 raw_us() / 1e6);
    t_lock_spi();
    for (size_t i = 0; i < sizeof td_regs / sizeof td_regs[0] && n < (int)sizeof buf - 40; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s 0x%02x 0x%08x\n", td_regs[i].name, td_regs[i].reg,
                      (uint32_t)t_read(td_regs[i].reg));
    t_unlock_spi();
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tm);
    snprintf(path, sizeof path, "%s/tiltregs_%s.txt", sg_csvdir, ts);
    mkdir(sg_csvdir, 0755);
    fd = real_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        snprintf(res, len, "error: cannot write %s", path);
        return;
    }
    real_write(fd, buf, (size_t)n);
    real_close(fd);
    snprintf(res, len, "ok: %s", path);
}

/* "tiltreg ...": live experiments with the tilt driver's settings (user: the motor is loud, the slower the louder).
 * Plain register writes under the app's SPI mutex, effective at once, lost at the next power cycle (or `tiltreg stock`).
 *   tiltreg status                      CS_ACTUAL / DRV_STATUS, the app's current ramp (now / target), ramp flag
 *   tiltreg stock                       GCONF 4, CHOPCONF 0x100c3, PWMCONF 0x504c8, IHOLD_IRUN as the app has it now
 *   tiltreg stealth 0|1                 GCONF en_pwm_mode: 1 = StealthChop (stock), 0 = SpreadCycle at all speeds
 *   tiltreg pwm AMPL GRAD FREQ SYMM     PWMCONF (TMC5130 layout: bit 19 = pwm_symmetric): AMPL 64-255, GRAD 1-255, FREQ 0-3, SYMM 0|1
 *                                       (autoscale always on: stock 0xc8 4 1 0; autoscale off = open loop, no current regulation: refused)
 *   tiltreg chop TOFF TBL HSTRT HEND    CHOPCONF: TOFF 1-15 (0 = driver off, refused; 1 needs TBL >= 2), TBL 0-3, HSTRT 0-7, HEND 0-15 with
 *                                       (HSTRT+1)+(HEND-3) <= 16 i.e. HSTRT+HEND <= 18 (datasheet: refuse); stock 3 2 4 1;
 *                                       microsteps (256) and vsense are never written
 *   tiltreg irun N                      IRUN 1..the run current the machine settings give (user: never higher than set), IHOLD as now
 *   tiltreg wave P                      microstep wave table f = sin t + a sin 3t, a = P / 100 %, P in -9 -6 -3 0 3 6 9 12 (0 = the chip's default sine);
 *                                       peak current unchanged (table peak 247), average torque unchanged, a 4th-harmonic torque ripple of +-a
 *                                       (can cancel the motor's full-step cogging; tools/mslut.py makes the tables). MSLUT0-7 0x60-0x67,
 *                                       MSLUTSEL 0x68, MSLUTSTART 0x69, never written by the app (chip defaults), `stock` writes them back
 *   tiltreg lut L0 .. L7 SEL START      raw table words in hex (made on the PC with tools/mslut.py, no rebuild): decoded first and refused unless
 *                                       ofs(0) = 0, 0 <= ofs <= 248 (the stock peak: the peak current never rises) and ofs(256) within 2 of START_SIN90
 *   tiltreg cur                         MSCNT and MSCURACT (phase A / B current values) at standstill: checks the table on the chip
 * The app ramps IRUN up (+step every interval ms, 0x817d8 / thread 0x8186c) after start-up until its target; once the ramp flag is
 * 0 it writes no more, so our values stay. Registers other than GCONF / CHOPCONF cannot be read back: watch CS_ACTUAL (status).
 * Every write waits for standstill (DRV_STATUS stst and XACTUAL = XTARGET, checked under the SPI mutex = no move can start in between; the datasheet wants
 * en_pwm_mode switched at standstill): while a move runs the reply is 'moving: retry in the rest phase'. */
#define TR_CUR     0x2fdd00u   /* the app's current ramp: byte 0 = IHOLD now, byte 1 = IRUN now */
#define TR_FLAG    0x2e1681u   /* ramp active flag */
#define TR_TGT     0x39fa30u   /* machine: +0x101 IHOLD target, +0x102 IRUN target (run current setting) */
#define R_GCONF    0x00
#define R_IHOLDRUN 0x10
#define R_CHOPCONF 0x6c
#define R_PWMCONF  0x70

static unsigned tr_byte(uint32_t a)
{
    return *(volatile uint8_t *)(uintptr_t)a;
}

static const struct {
    int pct;
    uint32_t lut[8], sel, start;
} tr_wave[] = {
    {-9, {0x00800000, 0x20100200, 0x42108410, 0x44444444, 0x04108844, 0xefff0201, 0x55b6ddef, 0x008424a5}, 0xb0b0105a, 0x00f70000},
    {-6, {0x42104104, 0x10884210, 0x44444222, 0x42110888, 0x01008208, 0xf7efffff, 0x5556b6ee, 0x00821249}, 0xa0a0105a, 0x00f70000},
    {-3, {0x92492492, 0x24924924, 0x22244892, 0x02084222, 0xffff0201, 0x76f7bfbf, 0x4a956b5b, 0x00408889}, 0x9090105a, 0x00f70000},
    {0, {0xaaaab554, 0x4a9554aa, 0x24492929, 0x10104222, 0xfbffffff, 0xb5bb777d, 0x49295556, 0x00404222}, 0xffff8056, 0x00f70000},
    {3, {0xb6dbb6dd, 0xd556b5b5, 0x4494a554, 0x00020844, 0xbbdf7fff, 0x5556b6db, 0x11229295, 0x00100821}, 0x8080105a, 0x00f70000},
    {6, {0xdfbfbfdf, 0x6db76f7b, 0x1294aab5, 0xffff8111, 0xdb6ef7ef, 0x92a5556a, 0x82111124, 0x00020040}, 0x7070105a, 0x00f70000},
    {9, {0x40002010, 0x6f7dfff7, 0x925556d7, 0xdfff2084, 0x556b6eef, 0x11124a55, 0x08020421, 0x00000200}, 0x7070205b, 0x00f70000},
    {12, {0x08912492, 0x7fff0041, 0x92aad76f, 0xdfef8084, 0x952ad6db, 0x04082224, 0x00000020, 0x00000000}, 0x7070305b, 0x00f70000},
};

static int tr_sext9(uint32_t v)
{
    v &= 0x1ff;
    return v & 0x100 ? (int)v - 0x200 : (int)v;
}

/* under the SPI mutex: standstill (stst) AND position = target (a move just commanded still shows stst for ~120 ms: TZEROWAIT) */
static int tr_at_rest(void)
{
    return (uint32_t)t_read(R_DRVSTAT) >> 31 && t_read(R_XACTUAL) == t_read(R_XTARGET);
}

/* up to max hex numbers (optional 0x) separated by spaces, then the end of the line: count, -1 = junk */
static int tr_hex(const char *p, uint32_t *v, int max)
{
    int n = 0;
    for (; n < max; n++) {
        uint32_t x = 0;
        int d = 0;
        while (*p == ' ')
            p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        for (;; p++, d++) {
            int h = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
            if (h < 0 || d >= 8)
                break;
            x = x << 4 | (uint32_t)h;
        }
        if (d == 0)
            break;
        v[n] = x;
    }
    while (*p == ' ' || *p == '\n' || *p == '\r')
        p++;
    return *p ? -1 : n;
}

/* decode the quarter wave like the chip (tools/mslut.py): 1 = acceptable */
static int tr_lut_ok(const uint32_t *l, uint32_t sel, uint32_t start)
{
    static const int base[4] = {-1, 0, 1, 2};
    int ofs = (int)(start & 255);
    if (ofs != 0)
        return 0;
    for (int i = 0; i < 256; i++) {
        int seg = i < (int)((sel >> 8) & 255) ? 0 : i < (int)((sel >> 16) & 255) ? 1 : i < (int)((sel >> 24) & 255) ? 2 : 3;
        ofs += base[(sel >> (2 * seg)) & 3] + (int)(l[i / 32] >> (i % 32) & 1);
        if (ofs < 0 || ofs > 248)
            return 0;
    }
    ofs -= (int)((start >> 16) & 255);
    return ofs >= -2 && ofs <= 2;
}

static void sgtilt_reg_cmd(const char *c, char *res, size_t len)
{
    double v[4] = {0};
    int n, run_max = (int)tr_byte(TR_TGT + 0x102), hold = (int)tr_byte(TR_CUR);
    int32_t val = 0;
    int reg = -1;
    if (!t_ok || !tr_ok) {
        snprintf(res, len, "error: tilt driver code not verified (unknown app version or disabled)");
        return;
    }
    if (strncmp(c, "status", 6) == 0) {
        uint32_t ds;
        int32_t gc, ch;
        t_lock_spi();
        ds = (uint32_t)t_read(R_DRVSTAT);
        gc = t_read(R_GCONF);
        ch = t_read(R_CHOPCONF);
        t_unlock_spi();
        snprintf(res, len, "ok: CS %u drv %08x irun %u/%d ihold %u ramp %u gc %x ch %x", (ds >> 16) & 31, ds, tr_byte(TR_CUR + 1),
                 run_max, tr_byte(TR_CUR), tr_byte(TR_FLAG), (uint32_t)gc, (uint32_t)ch);
        return;
    }
    if (strncmp(c, "stock", 5) == 0) {
        t_lock_spi();
        if (!tr_at_rest()) {
            t_unlock_spi();
            snprintf(res, len, "moving: retry in the rest phase");
            return;
        }
        t_write(R_GCONF, 4);
        t_write(R_CHOPCONF, 0x100c3);
        t_write(R_PWMCONF, 0x504c8);
        t_write(R_IHOLDRUN, 0x70000 | (int32_t)tr_byte(TR_CUR + 1) << 8 | hold);
        for (unsigned k = 0; k < sizeof tr_wave / sizeof tr_wave[0]; k++)
            if (tr_wave[k].pct == 0) {
                for (int j = 0; j < 8; j++)
                    t_write(0x60 + j, (int32_t)tr_wave[k].lut[j]);
                t_write(0x68, (int32_t)tr_wave[k].sel);
                t_write(0x69, (int32_t)tr_wave[k].start);
            }
        t_unlock_spi();
        snprintf(res, len, "ok: stock values written");
        return;
    }
    if (strncmp(c, "cur", 3) == 0) {
        uint32_t ms, cnt;
        t_lock_spi();
        if (!tr_at_rest()) {
            t_unlock_spi();
            snprintf(res, len, "moving: retry in the rest phase");
            return;
        }
        cnt = (uint32_t)t_read(0x6a);
        ms = (uint32_t)t_read(0x6b);
        t_unlock_spi();
        snprintf(res, len, "ok: mscnt %u A %d B %d", cnt, tr_sext9(ms), tr_sext9(ms >> 16));
        return;
    }
    if (strncmp(c, "lut ", 4) == 0) {
        uint32_t w[10];
        if (tr_hex(c + 4, w, 10) != 10) {
            snprintf(res, len, "error: tiltreg lut L0 L1 L2 L3 L4 L5 L6 L7 SEL START (hex)");
            return;
        }
        if (!tr_lut_ok(w, w[8], w[9])) {
            snprintf(res, len, "error: table refused (ofs(0) must be 0, 0 <= ofs <= 248, ofs(256) = START_SIN90 +-2)");
            return;
        }
        t_lock_spi();
        if (!tr_at_rest()) {
            t_unlock_spi();
            snprintf(res, len, "moving: retry in the rest phase");
            return;
        }
        for (int j = 0; j < 8; j++)
            t_write(0x60 + j, (int32_t)w[j]);
        t_write(0x68, (int32_t)w[8]);
        t_write(0x69, (int32_t)w[9]);
        t_unlock_spi();
        snprintf(res, len, "ok: table written");
        return;
    }
    if (strncmp(c, "wave ", 5) == 0) {
        double pv[1] = {0};
        int k = -1;
        if (parse_nums(c + 5, pv, 1) != 1 && !(c[5] == '-' && parse_nums(c + 6, pv, 1) == 1)) {
            snprintf(res, len, "error: tiltreg wave P (-9 -6 -3 0 3 6 9 12)");
            return;
        }
        if (c[5] == '-')
            pv[0] = -pv[0];
        for (unsigned j = 0; j < sizeof tr_wave / sizeof tr_wave[0]; j++)
            if (tr_wave[j].pct == (int)pv[0] && pv[0] == (int)pv[0])
                k = (int)j;
        if (k < 0) {
            snprintf(res, len, "error: tiltreg wave P (-9 -6 -3 0 3 6 9 12)");
            return;
        }
        t_lock_spi();
        if (!tr_at_rest()) {
            t_unlock_spi();
            snprintf(res, len, "moving: retry in the rest phase");
            return;
        }
        for (int j = 0; j < 8; j++)
            t_write(0x60 + j, (int32_t)tr_wave[k].lut[j]);
        t_write(0x68, (int32_t)tr_wave[k].sel);
        t_write(0x69, (int32_t)tr_wave[k].start);
        t_unlock_spi();
        snprintf(res, len, "ok: wave %d %% written", tr_wave[k].pct);
        return;
    }
    if (strncmp(c, "stealth ", 8) == 0) {
        if ((c[8] != '0' && c[8] != '1') || (c[9] != 0 && c[9] != '\n')) {
            snprintf(res, len, "error: tiltreg stealth 0|1");
            return;
        }
        reg = R_GCONF;
        val = c[8] == '1' ? 4 : 0;
    } else if (strncmp(c, "pwm ", 4) == 0) {
        n = parse_nums(c + 4, v, 4);
        if (n != 4 || v[0] != (int)v[0] || v[1] != (int)v[1] || v[2] != (int)v[2] || v[3] != (int)v[3] || v[0] > 255 || v[1] > 255 ||
            v[2] > 3 || !is01(v[3]) || v[0] < 0x40 || v[1] < 1) {
            snprintf(res, len, "error: tiltreg pwm AMPL(64-255) GRAD(1-255) FREQ(0-3) SYMM(0|1)");
            return;
        }
        reg = R_PWMCONF;
        val = (int32_t)v[0] | (int32_t)v[1] << 8 | (int32_t)v[2] << 16 | 1 << 18 | (int32_t)v[3] << 19;
    } else if (strncmp(c, "chop ", 5) == 0) {
        n = parse_nums(c + 5, v, 4);
        if (n != 4 || v[0] != (int)v[0] || v[1] != (int)v[1] || v[2] != (int)v[2] || v[3] != (int)v[3] || v[0] < 1 || v[0] > 15 ||
            v[1] > 3 || v[2] > 7 || v[3] > 15 || (v[0] == 1 && v[1] < 2) || v[2] + v[3] > 18) {
            snprintf(res, len, "error: tiltreg chop TOFF(1-15) TBL(0-3) HSTRT(0-7) HEND(0-15); TOFF 1 needs TBL>=2, HSTRT+HEND<=18");
            return;
        }
        reg = R_CHOPCONF;
        val = (int32_t)v[0] | (int32_t)v[2] << 4 | (int32_t)v[3] << 7 | (int32_t)v[1] << 15;
    } else if (strncmp(c, "irun ", 5) == 0) {
        n = parse_nums(c + 5, v, 1);
        if (n != 1 || v[0] != (int)v[0] || v[0] < 1 || v[0] > run_max) {
            snprintf(res, len, "error: tiltreg irun N (1-%d: not above the run current set now)", run_max);
            return;
        }
        reg = R_IHOLDRUN;
        val = 0x70000 | (int32_t)v[0] << 8 | hold;
    } else {
        snprintf(res, len, "error: tiltreg status|stock|stealth|pwm|chop|irun|wave|lut|cur");
        return;
    }
    t_lock_spi();
    if (!tr_at_rest()) {
        t_unlock_spi();
        snprintf(res, len, "moving: retry in the rest phase");
        return;
    }
    t_write(reg, val);
    t_unlock_spi();
    snprintf(res, len, "ok: reg 0x%02x = 0x%x%s", reg, (uint32_t)val, reg == R_IHOLDRUN && tr_byte(TR_FLAG) ? " (app ramp active: overwritten)" : "");
}

static int sgtilt_slow_json(char *buf, size_t len)
{
    int n;
    pthread_mutex_lock(&sl_lock);
    n = snprintf(buf, len, ",\"slow\":{\"set\":[%d,%d,%d,%d,%d,%d],\"file\":\"", (int)sl.set[0],
                 (int)sl.set[1], (int)sl.set[2], (int)sl.set[3], (int)sl.set[4], (int)sl.set[5]);
    if (n < (int)len) {
        js(buf + n, len - (size_t)n, sl.stem);
        n += (int)strlen(buf + n);
    }
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "\",\"nl\":%d,\"nr\":%d,\"err\":\"", sl.nl, sl.nr);
    if (n < (int)len) {
        js(buf + n, len - (size_t)n, sl.err);
        n += (int)strlen(buf + n);
    }
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "\",\"on\":%d,\"layer\":%d,\"next\":%d,\"n\":%d,\"beep\":%d,\"en\":%d}",
                      sl.active, sl.layer, sl.next, sl.nslow, sb_enabled(), sl_enabled());
    pthread_mutex_unlock(&sl_lock);
    return n;
}

/* writer thread: a queued profile goes in when no tilt move can be running */
static void tp_apply_due(void)
{
    double v[13];
    int layer = -1, printing;
    uintptr_t P;
    pthread_mutex_lock(&t_lock);
    if (!tp_req.pending) {
        pthread_mutex_unlock(&t_lock);
        return;
    }
    memcpy(v, tp_req.v, sizeof v);
    pthread_mutex_unlock(&t_lock);
    printing = HW(H_PSTATE) == 10;
    P = HW(W_PPTR);
    if (printing) {
        if (!P || (P & 3) || HB(P + W_STATE) != 2)
            return;                    /* not in the wait: try again on the next tick */
        layer = (int)HW(P + W_LAYER);
    }
    pthread_mutex_lock(&sl_lock);      /* slow layer values must not be saved */
    if (sl.active)
        for (int i = 0; i < 12; i++)
            HF(sl.prof + 4u * i) = sl.base[i];
    for (int i = 0; i < 12; i++)
        HF(T_PROF + 4u * i) = (float)v[1 + i];   /* aligned word stores */
    HW(T_PROF + 0x34) = (uint32_t)v[0];
    ((int (*)(void))(uintptr_t)H_SAVEMACH)();    /* -> machine_param_ref.bin */
    if (sl.active) {                   /* new own values, slow set on top again */
        if (sl.prof == T_PROF)
            for (int i = 0; i < 12; i++)
                sl.base[i] = (float)v[1 + i];
        sl_write(sl.prof);
    }
    pthread_mutex_unlock(&sl_lock);
    pthread_mutex_lock(&t_lock);
    tp_req.pending = 0;
    tp_req.layer = layer;
    if (printing)
        snprintf(tp_req.res, sizeof tp_req.res, "applied in the wait, layer %d", layer);
    else
        snprintf(tp_req.res, sizeof tp_req.res, "applied");
    pthread_mutex_unlock(&t_lock);
}

/* writer thread, every 0.5 s: switches, hook install (once the app has set up the tilt
 * stepper), the tilt thread (once per process) */
static void sgtilt_tick(int reload)
{
    pid_t me = getpid();
    uintptr_t obj;
    if (!t_ok)
        return;
    xm_tick();
    if (tp_ok && h_ok && w_ok)
        tp_apply_due();
    if (tp_ok && (HW(H_PSTATE) == 10 || s_active)) {   /* a job: its layer list, refreshed every 2 s */
        static unsigned rl;
        if (rl++ % 4 == 0)
            sl_read_list();
    } else if (sl.stem[0] || sl.nr) {                  /* no job: no list shown */
        pthread_mutex_lock(&sl_lock);
        sl.stem[0] = sl.err[0] = 0;
        sl.nr = sl.nl = 0;
        pthread_mutex_unlock(&sl_lock);
    }
    if (sl.active && HW(H_PSTATE) != 10) {     /* print over: own values back */
        pthread_mutex_lock(&sl_lock);
        sl_restore();
        sl.layer = sl.next = 0;
        pthread_mutex_unlock(&sl_lock);
    }
    if (tp_ok && h_ok && HW(H_PSTATE) != 10)
        dl_idle = 1;
    gs_tick();
    if (reload) {
        gs_load();
        dl_load();
        tc_load();
        sl_load_set();
        char path[160], buf[32];
        double v[1] = {0};
        snprintf(path, sizeof path, "%s/TILT2", sg_csvdir);      /* only the lead angle is read from it now */
        t_lead_deg = read_small(path, buf, sizeof buf) >= 0 && parse_nums(buf, v, 1) == 1 && v[0] <= 30 ? v[0] : 0;
        t_on = !t_fail;                /* the two-stage fix is always on */
        snprintf(path, sizeof path, "%s/TILTTRACE", sg_csvdir);
        t_tracing = access(path, F_OK) == 0;
        snprintf(path, sizeof path, "%s/TILTDRV", sg_csvdir);
        t_drv = t_tracing && access(path, F_OK) == 0;
    }
    tc_reconcile();                    /* after the files are loaded (first tick: reload = 1) */
    obj = HW(T_OBJS + 4);
    if (!t_installed && t_on && obj && !(obj & 3) && HW(obj + 0x18) == T_OPS_ROM) {
        for (int i = 0; i < T_OPS_N; i++)
            t_ops[i] = HW(T_OPS_ROM + 4u * i);
        t_ops[3] = (uint32_t)(uintptr_t)t_move;
        __sync_synchronize();
        HW(obj + 0x18) = (uint32_t)(uintptr_t)t_ops;   /* one aligned word store */
        t_installed = 1;
    }
    if (t_pid != me && t_installed) {
        pthread_t th;
        pthread_attr_t at;
        t_pid = me;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 64 * 1024);
        if (pthread_create(&th, &at, tilt_thread, NULL) != 0)
            t_fail = 1, t_on = 0;      /* no switcher: the wrapper passes through */
        pthread_attr_destroy(&at);
    }
}

/* "tilt":{...} or "tilt":null for live.json */
static int sgtilt_json(char *buf, size_t len)
{
    int n;
    if (!t_ok)
        return snprintf(buf, len, ",\"tilt\":null");
    pthread_mutex_lock(&t_lock);
    n = snprintf(buf, len, ",\"tilt\":{\"on\":%d,\"inst\":%d,\"trace\":%d,\"lead\":%.1f,\"n2\":%d,"
                 "\"nsw\":%d,\"ngive\":%d,\"ncancel\":%d,\"sw_deg\":%.2f,\"late_ms\":%.1f,"
                 "\"pend\":%d,\"res\":\"%s\"",
                 t_on, t_installed, t_tracing, t_lead_deg, t_stat.n2, t_stat.nsw, t_stat.ngive,
                 t_stat.ncancel, t_stat.last_sw_deg, t_stat.last_late_ms, tp_req.pending, tp_req.res);
    pthread_mutex_unlock(&t_lock);
    if (n < (int)len) {
        double v[13];
        if (sgtilt_read(v) && h_ok && w_ok) {
            /* "s2": the print in progress uses the fast-mode profile (these values not used) */
            n += snprintf(buf + n, len - (size_t)n, ",\"s2\":%d,\"n\":%d,\"prof\":[",
                          HW(T_PROFPTR) != 0 && HW(T_PROFPTR) != T_PROF, (int)v[0]);
            for (int i = 1; i < 13 && n < (int)len; i++)
                n += snprintf(buf + n, len - (size_t)n, "%g%s", v[i], i < 12 ? "," : "]");
        } else {
            n += snprintf(buf + n, len - (size_t)n, ",\"prof\":null");
        }
    }
    if (n < (int)len && tp_ok) {       /* the custom profile of the Tilt speed card: cust = switch, eff = used now (switch or Dynamic lift) */
        pthread_mutex_lock(&t_lock);
        n += snprintf(buf + n, len - (size_t)n, ",\"cust\":%d,\"eff\":%d,\"cv\":[", tc_on, tc_on || dl_cfg.on);
        for (int i = 0; i < 13 && n < (int)len; i++)
            n += snprintf(buf + n, len - (size_t)n, "%s%d", i ? "," : "", (int)tc_v[i]);
        if (n < (int)len)
            n += snprintf(buf + n, len - (size_t)n, "]%s", tc_have ? "" : ",\"cnone\":1");
        pthread_mutex_unlock(&t_lock);
    }
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "}");
    if (n < (int)len && tp_ok)
        n += sgtilt_slow_json(buf + n, len - (size_t)n);
    if (n < (int)len)
        n += sgtilt_gsync_json(buf + n, len - (size_t)n);
    if (n < (int)len && tp_ok && h_ok)
        n += sgtilt_dl_json(buf + n, len - (size_t)n);
    if (n < (int)len && h_ok)
        n += sgtilt_xm_json(buf + n, len - (size_t)n);
    return n;
}
