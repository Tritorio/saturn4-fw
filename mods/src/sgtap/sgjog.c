/*
 * sgjog.c - manual build plate control with load cell protection, mixing heights, camera light, Stock Mode.
 * #included by sgtap.c last (after sgmx.c / sgnoaa.c). Everything runs from the writer thread (commands, sgjog_tick every 0.5 s)
 * except the load cell watch sgjog_poll, which runs in the 20 Hz wait thread (sgwait.c).
 *
 * BUILD PLATE (card "Build plate"): commands
 *   "zjog U|D UM"  relative move up / down by UM micrometres (10 .. 100000), the app's own absolute axis_move (Z_MOVE, as the resin mixing):
 *                  1 mm/s up to 1 mm, 5 mm/s above. The target is clamped to 0 .. the travel of the axis. Z must be homed; a move down also
 *                  needs the app's foreign matter guard armed (it acts on the way down, as in the mixing).
 *   "zhome"        Home: what the app's own "motor home" handler does for "Z" (0x19ca8: axis_stop(0), axis_home(0)); the app's axis thread
 *                  runs the sequence (up to the top switch, back off, slow approach). Done = the axis' homing flag +0x4b is 0 again;
 *                  succeeded = homing_state +0x49 == 3 (the app then sets +0x4a). Allowed when Z is not homed (that is what it is for).
 *   "zstop"        axis_stop(0) of a move of ours.
 *   "zlim G"       the protection limit in grams (100 .. 10000, file ZJOG, default 2000).
 * LOAD CELL PROTECTION, both directions and the homing (the app's guard only checks moves DOWN, docs/foreign_matter_guard.txt): before a
 * move the mean of the last 0.6 s is the starting load v0; with the live zero r (tare / auto-tare; none = v0) the allowed band is
 * min(v0, r) - LIMIT .. max(v0, r) + LIMIT ("more than LIMIT beyond both the zero and the load the move started with": a plate that starts
 * pressed or loaded can be backed off without a false stop). 20 times a second the newest readings (5 Hz) are tested: two in a row outside
 * the band, or one outside by more than twice the limit, or no reading for 1.5 s = axis_stop at once. No readings = no move at all.
 * A Home stopped that way leaves Z not homed: Home again.
 * A jog ends on the app's "axis 0 is idle" line or by time (that line can be ~20 s late: the mixing and the wait test, which count those
 * lines, are refused for JG_SETTLE_US after a manual move). Not while printing, mixing or a wait test. live.json "jog".
 *
 * MIXING HEIGHTS: "mixz LO HI" (mm, 0.1 steps; LO 1 .. 150, HI >= LO + 2 and within the travel) / "mixz default"; file MIXZ. The mixing
 * itself is sgheat.c (first descent to Z low slow with the guard active, as before).
 *
 * CAMERA LIGHT: "light 0|1" = the app's own remote "light control" handler (0xb2310): ui param +0x13 (indicate_light_switch), the ui
 * saver, PWM channels 2 and 5 to 100 / 0 %, and its status message 0x7fb10. live.json "light".
 *
 * STOCK MODE: "stock 0|1", file STOCKMODE (survives a restart). While on, a print runs as on the stock firmware: sgwait.c keeps its
 * hands off every wait (dynamic wait, slicer-wait override, wait & heat, tare in the wait), sgexpo.c writes no exposure time (manual
 * override, resin profile), sgtilt.c brings the stock tilt profile back and passes every tilt move through (no two-stage fix, no Dynamic
 * lift, no gauge sync, no slow layers), Multi Exposure is switched off and refused. The settings themselves are kept and act again when
 * the mode is switched off. NOT touched: the dashboard, logging, Z monitor, timelapse, alerts, the heating (pre-heat, heating during
 * print and the pre-heat thermostat). Refused while a print job is running.
 * Only with the V1.5.9 code words below. Kill switch: /customer/mod/sgjog_disable (jog, home, light).
 */

#define JG_HOME     0x53774u   /* void axis_home(int axis) */
#define JG_FILE     "ZJOG"
#define JG_LIM_DEF  2000
#define JG_LIM_MIN  100
#define JG_LIM_MAX  10000
#define JG_FAST     5.0f       /* mm/s */
#define JG_SLOW     1.0f       /* mm/s, moves up to 1 mm */
#define JG_FRESH_US 1500000ull /* a reading older than this is no reading */
#define JG_HOME_S   240        /* s: a Home must be over by then */
#define MZ_FILE     "MIXZ"
#define MZ_LO_MIN   1.0
#define MZ_LO_MAX   150.0
#define MZ_SPAN_MIN 2.0
#define SM_FILE     "STOCKMODE"
#define LT_PWM      0x883a8u   /* int pwm(int channel, int percent) */
#define LT_NOTE     0x7fb10u   /* void light_status(int on) */

static const struct { uint32_t a, w; } jg_sig[] = {
    {0x53774, 0xe92d41f0}, {0x53778, 0xe30a5124}, {0x5377c, 0xe340502f}, {0x53780, 0xed2d8b02}, {0x537c8, 0xe5c3704b},
    {0x537cc, 0xe1c324b8}, {0x538ec, 0xebffff4a}, {0x19ca8, 0xeb00e642}, {0x19cac, 0xe1a0000b}, {0x19cb0, 0xeb00e6af},
    {0x542a4, 0xe5c4604b}, {0x542b8, 0xe5c4104a},
};
static const struct { uint32_t a, w; } lt_sig[] = {
    {0xb2334, 0xe2433001}, {0xb2338, 0xe16f3f13}, {0xb233c, 0xe1a032a3}, {0xb2340, 0xe5c43013}, {0xb2344, 0xebfed8f7},
    {0xb2354, 0xe3a01064}, {0xb2358, 0xe3a00002}, {0xb235c, 0xebff5811}, {0xb2360, 0xe3a01064}, {0xb2364, 0xe3a00005},
    {0xb2368, 0xebff580e}, {0xb236c, 0xe3a00001}, {0xb2370, 0xebff35e6}, {0x7fb10, 0xe3500001}, {0x7fb14, 0x8a000013},
    {0x7fb18, 0xe52de004}, {0x7fb1c, 0xe24ddf43}, {0x7fb20, 0xe3a0c009}, {0x883a8, 0xe3510000}, {0x883b0, 0xe30136b0},
    {0x883b4, 0xe340302e}, {0x883b8, 0xe7930180},
};

static pthread_mutex_t jg_lock = PTHREAD_MUTEX_INITIALIZER;
static int jg_init, jg_ok, lt_ok;
static volatile int jg_st;             /* 0 idle, 1 a jog is moving, 2 homing; changed under jg_lock */
static struct {
    double lo, hi;                     /* raw: the allowed band of the load cell */
    int trip;                          /* 1 = the watch stopped the move (trip_g says with what), 2 = it lost the readings */
    double trip_g;
    uint64_t t0, due, dead;
    unsigned idle0, trig0;
    float tgt;
} jg;
static int jg_lim = JG_LIM_DEF;
static char jg_msg[80] = "";

static double jg_absk(void)
{
    return w_k < 0 ? -w_k : w_k;
}

/* the newest readings: 0 = none fresh, 1 = *a, 2 = *a (newest) and *b */
static int jg_last2(double *a, double *b)
{
    uint64_t now = now_us();
    int n = 0;
    pthread_mutex_lock(&glock);
    if (hpos && hist[(hpos - 1) % HIST].ts + JG_FRESH_US >= now) {
        *a = hist[(hpos - 1) % HIST].raw;
        n = 1;
        if (hpos > 1 && hist[(hpos - 2) % HIST].ts + JG_FRESH_US >= now) {
            *b = hist[(hpos - 2) % HIST].raw;
            n = 2;
        }
    }
    pthread_mutex_unlock(&glock);
    return n;
}

/* the band for the move that starts now: 0 = no live readings (then no move) */
static int jg_arm(void)
{
    double v0, sd, r;
    const char *src;
    if (!(jg_absk() > 0) || recent(600000u, &v0, &sd) < 2)
        return 0;
    if (!cur_ref(&r, &src))
        r = v0;
    jg.lo = (v0 < r ? v0 : r) - jg_lim * jg_absk();
    jg.hi = (v0 > r ? v0 : r) + jg_lim * jg_absk();
    jg.trip = 0;
    jg.trip_g = 0;
    return 1;
}

static double jg_excess(double v)      /* raw counts outside the band, 0 = inside */
{
    return v > jg.hi ? v - jg.hi : v < jg.lo ? jg.lo - v : 0;
}

/* 20 Hz (wait thread): the load cell watch of a move of ours */
static void sgjog_poll(void)
{
    double a = 0, b = 0, ea;
    int n;
    if (!jg_st)
        return;
    pthread_mutex_lock(&jg_lock);
    if (jg_st && !jg.trip) {
        n = jg_last2(&a, &b);
        ea = n ? jg_excess(a) : 0;
        if (!n) {
            if (now_us() > jg.t0 + JG_FRESH_US)
                jg.trip = 2;
        } else if (ea > jg_lim * jg_absk() || (n == 2 && ea > 0 && jg_excess(b) > 0)) {
            jg.trip = 1;
            jg.trip_g = (a > jg.hi ? ea : -ea) / jg_absk();
        }
        if (jg.trip)
            ((void (*)(int))(uintptr_t)Z_STOP)(0);
    }
    pthread_mutex_unlock(&jg_lock);
}

static void jg_end(void)               /* jg_lock held */
{
    jg_st = 0;
    z_st = 0;
    jg_end_us = now_us();
}

/* NULL = a manual move may start; else why not */
static const char *jg_why(void)
{
    double d;
    uint32_t p;
    if (!jg_ok || !h_ok || !w_ok || !in_chitu)
        return "plate control not available";
    if (HW(H_PSTATE) == 10 || s_active)
        return "a print job is running";
    if (wt_busy)
        return "a wait test is running";
    if (jg_st)
        return "the plate is still moving";
    if (mix_ph || z_st)
        return "the resin mixing is running";
    if (!(p = z_axis(&d)))
        return "Z axis not found";
    if (HB(p + 0x4b))
        return "Z is homing";
    return NULL;
}

static void jg_load(void)
{
    char path[160], buf[64];
    double v[2];
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, JG_FILE);
    if (read_small(path, buf, sizeof buf) > 0 && parse_nums(buf, v, 1) == 1 && v[0] >= JG_LIM_MIN && v[0] <= JG_LIM_MAX)
        jg_lim = (int)v[0];
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, MZ_FILE);
    if (!mix_ph) {                     /* never under a running mixing */
        if (read_small(path, buf, sizeof buf) > 0 && parse_nums(buf, v, 2) == 2 && v[0] >= MZ_LO_MIN && v[0] <= MZ_LO_MAX &&
            v[1] >= v[0] + MZ_SPAN_MIN && v[1] <= 400) {
            mz_lo = (float)v[0];
            mz_hi = (float)v[1];
            mz_set = 1;
        } else if (access(path, F_OK) != 0) {
            mz_set = 0;
        }
    }
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, SM_FILE);
    sg_stock = access(path, F_OK) == 0;
}

/* writer thread, every 0.5 s (before the other modules' ticks) */
static void sgjog_tick(int reload)
{
    double d, pos;
    uint32_t p;
    uint64_t now;
    if (!jg_init) {
        jg_init = 1;
        if (in_chitu && access("/customer/mod/sgjog_disable", F_OK) != 0) {
            jg_ok = lt_ok = 1;
            for (unsigned i = 0; i < sizeof jg_sig / sizeof jg_sig[0]; i++)
                if (HW(jg_sig[i].a) != jg_sig[i].w)
                    jg_ok = 0;
            for (unsigned i = 0; i < sizeof lt_sig / sizeof lt_sig[0]; i++)
                if (HW(lt_sig[i].a) != lt_sig[i].w)
                    lt_ok = 0;
        }
    }
    if (reload)
        jg_load();
    if (!jg_st)
        return;
    now = now_us();
    pthread_mutex_lock(&jg_lock);
    p = z_axis(&d);
    pos = p ? HI(p + 0x50) * d : 0;
    if (!jg_st) {
        ;
    } else if (jg.trip) {
        if (jg.trip == 2)
            snprintf(jg_msg, sizeof jg_msg, "stopped: no load cell readings%s", jg_st == 2 ? " (Z not homed: Home again)" : "");
        else
            snprintf(jg_msg, sizeof jg_msg, "stopped by the load cell: %+.0f g over%s", jg.trip_g, jg_st == 2 ? " (Z not homed: Home again)" : "");
        jg_end();
    } else if (HW(H_PSTATE) == 10 || s_active) {
        snprintf(jg_msg, sizeof jg_msg, "ended: a print job started");
        jg_end();
    } else if (lg_z_trig != jg.trig0) {
        snprintf(jg_msg, sizeof jg_msg, "stopped: the foreign matter guard tripped");
        jg_end();
    } else if (jg_st == 1) {
        if (now > jg.due)
            sglog_force_follow();      /* the idle line may sit in the app's log buffer */
        if (lg_z_idle != jg.idle0 || now > jg.dead) {
            snprintf(jg_msg, sizeof jg_msg, "plate at %.2f mm", pos);
            jg_end();
        }
    } else if (!p) {
        ((void (*)(int))(uintptr_t)Z_STOP)(0);
        snprintf(jg_msg, sizeof jg_msg, "stopped: Z axis not found");
        jg_end();
    } else if (!HB(p + 0x4b) && now > jg.t0 + 1000000u) {
        if (HB(p + 0x49) == 3 && HB(p + 0x4a))
            snprintf(jg_msg, sizeof jg_msg, "homed: plate at %.1f mm", pos);
        else
            snprintf(jg_msg, sizeof jg_msg, "homing failed (state %d)", (int)HB(p + 0x49));
        jg_end();
    } else if (now > jg.dead) {
        ((void (*)(int))(uintptr_t)Z_STOP)(0);
        snprintf(jg_msg, sizeof jg_msg, "stopped: the homing did not finish in time (Home again)");
        jg_end();
    }
    pthread_mutex_unlock(&jg_lock);
}

static void jg_start(int st, float tgt, double secs)     /* jg_lock held, jg_arm done */
{
    uint64_t now = now_us();
    jg.t0 = now;
    jg.due = now + (uint64_t)((secs + 1.0) * 1e6);
    jg.dead = now + (uint64_t)((st == 2 ? JG_HOME_S : secs * 1.1 + 3.0) * 1e6);
    jg.idle0 = lg_z_idle;
    jg.trig0 = lg_z_trig;
    jg.tgt = tgt;
    z_st = st == 2 ? 7 : 6;
    jg_st = st;
}

/* the whole command line: "zjog U|D UM" | "zhome" | "zstop" | "zlim G" */
static void sgjog_cmd(const char *c, char *res, size_t len)
{
    const char *why;
    double v[1], d, pos, max, tgt, dist;
    uint32_t p;
    if (strncmp(c, "zstop", 5) == 0) {
        pthread_mutex_lock(&jg_lock);
        if (jg_st) {
            ((void (*)(int))(uintptr_t)Z_STOP)(0);
            snprintf(jg_msg, sizeof jg_msg, "stopped%s", jg_st == 2 ? " (Z not homed: Home again)" : "");
            jg_end();
        }
        pthread_mutex_unlock(&jg_lock);
        return;
    }
    if (strncmp(c, "zlim ", 5) == 0) {
        char path[160], line[24];
        if (parse_nums(c + 5, v, 1) != 1 || v[0] < JG_LIM_MIN || v[0] > JG_LIM_MAX || v[0] != (double)(int)v[0]) {
            snprintf(res, len, "error: zlim G (%d-%d g)", JG_LIM_MIN, JG_LIM_MAX);
            return;
        }
        if (jg_st) {
            snprintf(res, len, "error: the plate is still moving");
            return;
        }
        jg_lim = (int)v[0];
        snprintf(path, sizeof path, "%s/%s", sg_csvdir, JG_FILE);
        if (!write_small(path, line, snprintf(line, sizeof line, "%d\n", jg_lim)))
            snprintf(res, len, "error: set, but saving %s failed", JG_FILE);
        return;
    }
    if ((why = jg_why())) {
        snprintf(res, len, "error: %s", why);
        return;
    }
    if (strncmp(c, "zhome", 5) == 0) {
        pthread_mutex_lock(&jg_lock);
        if (!jg_arm()) {
            snprintf(res, len, "error: no live load cell readings (no protection): not moved");
        } else {
            jg_start(2, 0, 0);
            snprintf(jg_msg, sizeof jg_msg, "homing");
            ((void (*)(int))(uintptr_t)Z_STOP)(0);            /* as the app's own "motor home" does */
            ((void (*)(int))(uintptr_t)JG_HOME)(0);
        }
        pthread_mutex_unlock(&jg_lock);
        return;
    }
    /* zjog U|D UM */
    if ((c[5] != 'U' && c[5] != 'D') || c[6] != ' ' || parse_nums(c + 7, v, 1) != 1 || v[0] < 10 || v[0] > 100000 ||
        v[0] != (double)(int)v[0]) {
        snprintf(res, len, "error: zjog U|D UM (10-100000 um)");
        return;
    }
    if ((why = z_why_not(c[5] == 'D', &pos)) || !(p = z_axis(&d))) {
        snprintf(res, len, "error: %s", why ? why : "Z axis not found");
        return;
    }
    max = HI(p + 0x3c) * d;
    tgt = c[5] == 'U' ? pos + v[0] / 1000 : pos - v[0] / 1000;
    if (tgt > max)
        tgt = max;
    if (tgt < 0)
        tgt = 0;
    dist = tgt > pos ? tgt - pos : pos - tgt;
    if (dist < 0.005) {
        snprintf(res, len, "ok: already at the %s end (%.2f mm)", c[5] == 'U' ? "upper" : "lower", pos);
        return;
    }
    pthread_mutex_lock(&jg_lock);
    if (!jg_arm()) {
        snprintf(res, len, "error: no live load cell readings (no protection): not moved");
    } else {
        float spd = v[0] <= 1000 ? JG_SLOW : JG_FAST;
        jg_start(1, (float)tgt, dist / spd);
        snprintf(jg_msg, sizeof jg_msg, "going %s to %.2f mm", tgt > pos ? "up" : "down", tgt);
        ((void (*)(int, int, float, float))(uintptr_t)Z_MOVE)(0, 0, (float)tgt, spd);
    }
    pthread_mutex_unlock(&jg_lock);
}

/* "LO HI" (mm) | "default" */
static void sgjog_mixz_cmd(const char *c, char *res, size_t len)
{
    char path[160], line[48];
    double v[2], d;
    uint32_t p;
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, MZ_FILE);
    if (!h_ok) {
        snprintf(res, len, "error: plate control not available");
        return;
    }
    if (mix_ph) {
        snprintf(res, len, "error: not while the mixing is running");
        return;
    }
    if (strncmp(c, "default", 7) == 0) {
        unlink(path);
        mz_set = 0;
        return;
    }
    if (parse_nums(c, v, 2) != 2 || v[0] < MZ_LO_MIN || v[0] > MZ_LO_MAX || v[1] < v[0] + MZ_SPAN_MIN) {
        snprintf(res, len, "error: mixz LO HI (LO %.0f-%.0f mm, HI at least LO + %.0f)", MZ_LO_MIN, MZ_LO_MAX, MZ_SPAN_MIN);
        return;
    }
    if ((p = z_axis(&d)) && v[1] > HI(p + 0x3c) * d - 0.5) {
        snprintf(res, len, "error: Z high is beyond the travel of the Z axis (%.0f mm)", HI(p + 0x3c) * d - 0.5);
        return;
    }
    mz_lo = (float)v[0];
    mz_hi = (float)v[1];
    mz_set = 1;
    if (!write_small(path, line, snprintf(line, sizeof line, "%.1f %.1f\n", v[0], v[1])))
        snprintf(res, len, "error: set, but saving %s failed", MZ_FILE);
}

/* "0|1": the app's own remote light control */
static void sglight_cmd(const char *c, char *res, size_t len)
{
    int on = c[0] == '1';
    int (*pwm)(int, int) = (int (*)(int, int))(uintptr_t)LT_PWM;
    if ((c[0] != '0' && c[0] != '1') || (c[1] && c[1] != '\n' && c[1] != ' ')) {
        snprintf(res, len, "error: bad light command");
        return;
    }
    if (!lt_ok || !h_ok) {
        snprintf(res, len, "error: light control not available (unknown app version or disabled)");
        return;
    }
    HB(H_UI + 0x13) = (uint8_t)on;
    if (((int (*)(void))(uintptr_t)H_SAVEUI)() != 0)
        snprintf(res, len, "error: set, but saving ui_param_ref.bin failed");
    pwm(2, on ? 100 : 0);
    pwm(5, on ? 100 : 0);
    ((void (*)(int))(uintptr_t)LT_NOTE)(on);
}

/* "0|1" */
static void sgstock_cmd(const char *c, char *res, size_t len)
{
    char path[160], dummy[96];
    int fd;
    if ((c[0] != '0' && c[0] != '1') || (c[1] && c[1] != '\n' && c[1] != ' ')) {
        snprintf(res, len, "error: bad stock command");
        return;
    }
    if (HW(H_PSTATE) == 10 || s_active) {
        snprintf(res, len, "error: not while a print job is running");
        return;
    }
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, SM_FILE);
    if (c[0] == '1') {
        mkdir(sg_csvdir, 0755);
        if ((fd = real_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644)) < 0) {
            snprintf(res, len, "error: saving %s failed", SM_FILE);
            return;
        }
        real_close(fd);
        sg_stock = 1;
        sgmx_cmd("off", dummy, sizeof dummy);                 /* Multi Exposure ends with Stock Mode on */
    } else {
        unlink(path);
        sg_stock = 0;
    }
}

static int sgjog_json(char *buf, size_t len)
{
    double d = 0, pos = 0, max = 0, a = 0, b = 0, r = 0;
    const char *why = jg_why(), *src;
    uint32_t p = in_chitu && h_ok ? z_axis(&d) : 0;
    int n, homed = 0, have = jg_last2(&a, &b);
    char g[16] = "null", m[80];
    if (p) {
        pos = HI(p + 0x50) * d;
        max = HI(p + 0x3c) * d;
        homed = HB(p + 0x49) == 3 && HB(p + 0x4a);
    }
    if (have && cur_ref(&r, &src) && jg_absk() > 0)
        snprintf(g, sizeof g, "%.0f", (a - r) / w_k);
    js(m, sizeof m, jg_msg);
    n = snprintf(buf, len, ",\"jog\":{\"ok\":%d,\"st\":%d,\"pos\":%.2f,\"max\":%.1f,\"homed\":%d,\"armed\":%d,\"lim\":%d,\"g\":%s,\"why\":\"%s\",\"msg\":\"%s\","
                 "\"mixset\":%d},\"light\":%s%d%s,\"stock\":{\"on\":%d}",
                 jg_ok && h_ok && w_ok, (int)jg_st, pos, max, homed, in_chitu && h_ok ? HW(Z_ARMED) != 0 : 0, jg_lim, g,
                 jg_st ? "" : why ? why : "", m, mz_set,
                 lt_ok && h_ok ? "{\"on\":" : "", lt_ok && h_ok ? (int)HB(H_UI + 0x13) : 0, lt_ok && h_ok ? "}" : "",
                 (int)sg_stock);
    return n;
}
