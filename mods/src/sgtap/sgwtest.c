/*
 * sgwtest.c - "Wait Times Testing" (Developer card), #included by sgtap.c after sgtilt.c.
 *
 * A simulated print job without UV, driven by our own sequencer with the app's own axis_move (Z_MOVE) / axis_stop (Z_STOP), as the resin mixing and
 * Tilt Move do. Command "wtest start H N" (layer height mm >= 0.010 with no upper limit (larger Z steps for the UV-heating tests), N layers 1-999; the run ends early when the next step would pass Z 50 mm) / "wtest stop". Idle printer only (not printing, Z and tilt homed,
 * the foreign matter guard armed). Every position, speed and threshold comes from the live machine settings (the struct at 0x39fa30, reference:
 * gcode/S4U_201026.gcode): I204 A / B / D / E (probe start, end, speed, trigger), I205 A / B / C / D / E (level start, end, step, speed, threshold; the
 * positions are ABSOLUTE machine Z), I5 E (Z initial speed, 0.05 mm/s = the Z speed of a layer step in the app's log). The sequence of a print start,
 * then N layers:
 *   prep      tilt to 0, plate up to the probe start height if it is below
 *   calibrate 8 s still, the mean of the last 5 s = the gauge zero in air (app units = (raw - zero) / 100, like the app's zero_data)
 *   probe     (from a higher park position first down to the probe start height at 5 mm/s) down at the probe speed (guard active) until the reading is above the trigger, at most to the probe end position; 0.3 mm back up
 *   level     down to the level start (level speed), 5 s still, then steps of the level step size (level speed), 1 s still after each, until the
 *             reading is above the level threshold or the level end position is reached: the plate position = the contact Z0
 *   layers the plate rises to Z0 + H (the first layer's height, 5 mm/s) and the WAIT of layer 0 follows at once, no tilt cycle before it (
 *             as in the app's log of a print: after the levelling the plate waits, then exposes); then for every layer 1 .. N-1 as in a print (app log of a real print): the tilt
 *             releases (0 -> 140); when it has arrived the plate rises one layer height (lift distance = layer height, user: so the app's
 *             second Z move after the return has no distance and is left out) (Z0 + (n+1) * H, at the Z initial speed: about 1 s for
 *             0.05 mm) and the tilt return (140 -> 0) starts at the same moment; both with the stages of the live tilt profile (T_PROF: layers <= N use
 *             A..F, later ones S..Y, the Tilt card values). No exposure: nothing is cured, so the gap under the plate grows by one layer height per
 *             layer. Then the WAIT: no maximum time; it ends when the average of the last WIN s differs from the average of the WIN s before by less than N g (WIN / N = the Dynamic wait settings; so at least 2 x WIN s pass) (the
 *             minimum wait of the Dynamic wait is NOT used: the test measures the settling time). Wait & heat follows the dashboard settings.
 *   end       the plate rises to 50 mm, the heater is off. Stop / print start / guard trip / any failure: axis_stop, tilt back to 0, plate up to 50 mm.
 * Own datalog <csvdir>/waittest/waittest_<H 3 decimals>mm_<YYYYmmdd_HHMMSS>.csv (row every 0.5 s + one "layer" row per wait, line 2 = column
 * descriptions). live.json "wt". Runs in the 20 Hz wait thread (sgwtest_poll). Kill switch /customer/mod/sgwtest_disable (checked at start).
 */

#define WT_DIR        "waittest"
#define WT_UP_SPD     1.0f
#define WT_BACK_MM    0.3f
#define WT_STEP_HOLD  1000000ull       /* us still after each level step */
#define WT_CAL_S      8.0              /* s at the park height; the mean of the last WT_CAL_AVG s is the zero */
#define WT_CAL_AVG    5.0
#define WT_LSTILL_S   5.0
#define WT_TILT_MAX   140.0f
#define WT_TILT_HOME  36.0f
#define WT_WIN_MAX    380.0              /* s: longest averaging window (two windows must fit in the reading history)  */
#define WT_TILT_SPD   36.0f              /* deg/s of every tilt move of the test */
#define WT_NRES       200              /* settle times kept for the page */
#define WT_ROW_US     500000ull
#define H_Z_INITSPD   (H_MACH + 0x4cu)     /* float, I5 E  Z initial speed, mm/s */
#define H_DET_A       (H_MACH + 0x1f4u)    /* float, I204 A  resin detection start position, mm */
#define H_DET_B       (H_MACH + 0x1f8u)    /* float, I204 B  end position */
#define H_DET_SPD     (H_MACH + 0x200u)    /* float, I204 D  speed, mm/s */
#define H_PROBE_THS   (H_MACH + 0x204u)    /* int,   I204 E  trigger threshold */
#define H_LEV_A       (H_MACH + 0x214u)    /* float, I205 A  auto-levelling start position, mm */
#define H_LEV_B       (H_MACH + 0x218u)    /* float, I205 B  end position */
#define H_LEV_STEP    (H_MACH + 0x21cu)    /* float, I205 C  step size, mm */
#define H_LEV_SPD     (H_MACH + 0x220u)    /* float, I205 D  speed, mm/s */
#define H_LEVEL_THS   (H_MACH + 0x224u)    /* int,   I205 E  threshold */

enum { WT_OFF = 0, WT_PREP_T, WT_PREP_Z, WT_CAL, WT_TOPARK, WT_PROBE, WT_BACK, WT_TOLEV, WT_LSTILL, WT_LPRESS, WT_UP1, WT_REL1, WT_REL2, WT_RET1, WT_RET2, WT_WAIT,
       WT_ENDUP, WT_ABT_T, WT_ABT_Z };
static const char *const wt_phname[] = {"off", "prep", "prep", "calibrate", "probe", "probe", "probe", "level", "level", "level", "level", "release", "release",
                                        "return", "return", "wait", "end", "abort", "abort"};

static pthread_mutex_t wt_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int start, stop; double h; int n; } wt_req;                 /* from the command thread, under wt_lock */
static volatile int wt_ph;                                                  /* WT_*; the poll thread only */
static int wt_state;                                                        /* shown: 0 idle (nothing yet), 1 running, 2 finished, 3 stopped / error; under wt_lock */
static char wt_msg[96], wt_file[96];
static double wt_h;
static int wt_n, wt_layer;
static float wt_res[WT_NRES];
static int wt_nres;
static double wt_el, wt_gnow = -1, wt_zmm, wt_xdeg;                         /* shown values */

/* sequencer state (poll thread only) */
static struct { int on; unsigned i0, need; uint64_t dead, due, recheck, fb; float tgt; } wt_mx[2];       /* per axis (0 = Z, 1 = tilt): a move in progress, idle counter at its start, deadline */
static unsigned wt_mv_t0;                                              /* guard trip counter at the latest move start */
static uint64_t wt_t0, wt_t_ph, wt_t_row, wt_t_start;
static double wt_zero_raw, wt_z0, wt_prev_settle = -1;
/* the foreign matter guard limit (I206 C, live +0x230) is raised during the levelling press only: the level threshold (I205 E, 4000) is above the guard limit
 * (3000), so the last steps of the press pass it for more than the guard's 26 checks and it trips (seen in the first runs). The limit goes to the level
 * threshold itself (4000: "the 3000 limit is for foreign matter detection, levelling shall use the 4000 one"): the press stops at the
 * first reading above it, so the guard only sees the last step above 4000. It is given back when the press ends, on a stop / failure and in wt_finish. */
#define H_GUARD_LIM  (H_MACH + 0x230u)
#define WT_GUARD_MARGIN 0              /* user: the foreign matter limit (3000) is for detection, the levelling uses its own 4000 */
static int wt_graised, wt_gsave;
static void wt_guard_raise(void)
{
    int want = HI(H_LEVEL_THS) + WT_GUARD_MARGIN;
    if (wt_graised || HI(H_GUARD_LIM) >= want)
        return;
    wt_gsave = HI(H_GUARD_LIM);
    HI(H_GUARD_LIM) = want;
    wt_graised = want;
}
static void wt_guard_restore(void)
{
    if (!wt_graised)
        return;
    if (HI(H_GUARD_LIM) == wt_graised)                     /* the user may have changed it meanwhile: then leave it */
        HI(H_GUARD_LIM) = wt_gsave;
    wt_graised = 0;
}
/* user: the foreign matter detection is active for the first layer only (probe, levelling, the wait of layer 0); once that passed, the limit is set
 * to INT_MAX for the layers 1.. (a dry run presses the plate against the film with only 0.2-0.4 mm of resin: a 0.2 mm run tripped it at 4710 on the
 * return stroke of layer 1). Given back by wt_guard_restore (stop, failure, end). */
static void wt_guard_off(void)
{
    if (wt_graised == 0x7fffffff)
        return;
    if (!wt_graised)
        wt_gsave = HI(H_GUARD_LIM);
    HI(H_GUARD_LIM) = 0x7fffffff;
    wt_graised = 0x7fffffff;
}
static int wt_z0_ok;                                                        /* the contact height of this run is known */
static float wt_park, wt_det_b, wt_det_spd, wt_lev_a, wt_lev_b, wt_lev_step, wt_lev_spd, wt_zspd;   /* the machine values of this run */
static int wt_step, wt_step_state, wt_heated, wt_fd = -1;
static unsigned wt_hp0;                                                     /* hist position of the row accumulator */
static double wt_rmin, wt_rmax, wt_rlast;
static int wt_rn;
static char wt_fail[96];
static unsigned wt_nrecheck;                                         /* moves whose idle line was only found by the forced log re-read */

static double wt_k(void) { return w_k < 0 ? -w_k : w_k; }

/* mean raw of the readings with ts >= from (us); *n = how many; newest reading time in *last */
static double wt_mean_raw(uint64_t from, int *n, uint64_t *last)
{
    double s = 0;
    int c = 0;
    uint64_t l = 0;
    pthread_mutex_lock(&glock);
    for (unsigned i = 0; i < HIST && i < hpos; i++) {
        unsigned j = (hpos - 1 - i) % HIST;
        if (hist[j].ts < from)
            break;
        if (!l)
            l = hist[j].ts;
        s += hist[j].raw;
        c++;
    }
    pthread_mutex_unlock(&glock);
    *n = c;
    if (last)
        *last = l;
    return c ? s / c : 0;
}

/* app units (raw / 100 - zero) of the newest ~0.6 s; 1 = valid (fresh readings) */
static int wt_force_u(double *u)
{
    int n;
    uint64_t last;
    double m = wt_mean_raw(now_us() - 600000ull, &n, &last);
    if (n < 2 || now_us() - last > 1500000ull)
        return 0;
    *u = (m - wt_zero_raw) / 100.0;
    return 1;
}

/* settle rule: |mean of the last win s - mean of the win s before| in g (two consecutive averages); -1 = not available yet
 * (fewer than 2 * win s of readings since from_us, or too few readings in a block). *mean_raw = mean raw of the last block. win is capped to what the history holds. */
static double wt_block_diff(uint64_t from_us, double win, double *mean_raw)
{
    uint64_t now = now_us(), b1 = now - (uint64_t)(win * 1e6), b0 = now - (uint64_t)(2 * win * 1e6);
    double s1 = 0, s0 = 0, k = wt_k();
    int n1 = 0, n0 = 0;
    if (b0 < from_us)
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
    if (n1 < (int)(3.0 * win) + 1 || n0 < (int)(3.0 * win) + 1 || n1 < 3 || n0 < 3)   /* about 60 % of the 5.4 readings per second (a fixed 10 refused short windows at times) */
        return -1;
    *mean_raw = s1 / n1;
    {
        double d = (s1 / n1 - s0 / n0) / k;
        return d < 0 ? -d : d;
    }
}

static double wt_zpos(int *ok)
{
    double d = 0;
    uint32_t p = z_axis(&d);
    *ok = p != 0;
    return p ? HI(p + 0x50) * d : 0;
}

static double wt_xpos(int *ok)
{
    double d = 0;
    uint32_t p = xm_axis(&d);
    *ok = p != 0;
    return p ? HI(p + 0x50) * d : 0;
}

/* ---- datalog ---- */
static void wt_put(const char *s)
{
    size_t n = strlen(s);
    if (wt_fd >= 0 && write(wt_fd, s, n) < 0) {
        real_close(wt_fd);
        wt_fd = -1;
    }
}

static int wt_logging;             /* rows are written from the first level phase on; an abort row is always written */
static uint64_t wt_wait0_us;      /* start of the first wait of the run (0 = not yet) */

static void wt_row(const char *kind, double settle, double plateau, double gend)
{
    char b[500], tb[40], t1[16] = "", t2[24] = "", t3[24] = "", t4[24] = "", t5[24] = "", t6[24] = "", t7[16] = "";
    struct timespec rt;
    struct tm tm;
    double g, k = wt_k(), c = 0;
    if (!wt_logging && wt_ph != WT_ABT_T && wt_ph != WT_ABT_Z)
        return;
    int ok, n;
    uint64_t now = now_us(), last;
    double raw = wt_mean_raw(now - 400000ull, &n, &last);
    clock_gettime(CLOCK_REALTIME, &rt);
    gmtime_r(&rt.tv_sec, &tm);
    snprintf(tb, sizeof tb, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)(rt.tv_nsec / 1000000));
    g = n ? (raw - wt_zero_raw) / k : 0;
    if (sgheat_temp(&c))
        snprintf(t1, sizeof t1, "%.1f", c);
    if (wt_gnow >= 0 && wt_ph == WT_WAIT)
        snprintf(t2, sizeof t2, "%.3f", wt_gnow);
    if (settle >= 0) {
        snprintf(t3, sizeof t3, "%.2f", settle);
        snprintf(t4, sizeof t4, "%.2f", plateau);
        snprintf(t5, sizeof t5, "%.3f", gend);
    }
    if (!wt_wait0_us && wt_ph == WT_WAIT)
        wt_wait0_us = now;
    if (wt_wait0_us) {
        unsigned long ws = (unsigned long)((now - wt_wait0_us) / 1000000u);
        snprintf(t7, sizeof t7, "%02lu:%02lu:%02lu", ws / 3600, ws / 60 % 60, ws % 60);
    }
    wt_zmm = wt_zpos(&ok);
    wt_xdeg = wt_xpos(&ok);
    if (wt_z0_ok)
        snprintf(t6, sizeof t6, "%.3f", wt_zmm - wt_z0);
    snprintf(b, sizeof b, "%s,%.3f,%s,%d,%s,%.0f,%.2f,%.2f,%.2f,%d,%s,%s,%.3f,%.1f,%d,%s,%s,%s,%.3f,%s,%s,%d\n", tb, (now - wt_t_start) / 1e6, kind, wt_layer,
             wt_phname[wt_ph], n ? raw : 0.0, g, wt_rn ? (wt_rmin - wt_zero_raw) / k : g, wt_rn ? (wt_rmax - wt_zero_raw) / k : g, wt_rn, t1, t2,
             wt_zmm, wt_xdeg, wt_heated, t3, t4, t5, wt_h, t6, t7, sgheat_out());
    wt_put(b);
    wt_rn = 0;
}

/* accumulate the readings since the last call for the 0.5 s rows (min / max of the window) */
static void wt_accum(void)
{
    pthread_mutex_lock(&glock);
    if (hpos - wt_hp0 > HIST)
        wt_hp0 = hpos - HIST;
    while (wt_hp0 != hpos) {
        double r = hist[wt_hp0 % HIST].raw;
        if (!wt_rn || r < wt_rmin) wt_rmin = r;
        if (!wt_rn || r > wt_rmax) wt_rmax = r;
        wt_rlast = r;
        wt_rn++;
        wt_hp0++;
    }
    pthread_mutex_unlock(&glock);
}

static void wt_header(void)
{
    wt_put("time_utc,t_s,kind,layer,phase,raw_last,g,g_min,g_max,n,tank_c,diff_g,z_mm,tilt_deg,heater,settle_s,plateau_g,diff_end_g,layer_h_mm,above_mm,t_wait_hms,heat_on\n");
    wt_put("\"Wall-clock time of the row (printer clock)\",\"Seconds since the test started\",\"row = every 0.5 s, layer = summary of one finished wait\","
           "\"Layer number, 0 = the first layer (the wait right after the levelling)\",\"prep, calibrate, probe, level, release, return, wait, end, abort\","
           "\"Last gauge reading in the row, raw counts\",\"Force in grams: (raw - zero in air) / k. Pressing down = positive\","
           "\"Lowest g of the readings in the row\",\"Highest g of the readings in the row\",\"Gauge readings in the row\","
           "\"Vat temperature, C (empty if no sensor)\",\"In the wait: absolute difference of the last window average and the average of the window before, g\","
           "\"Plate position, mm (machine Z)\",\"Tilt position, deg (0 = flat, 140 = fully tilted)\",\"1 = resin heated in the wait\","
           "\"layer rows: time from the end of the retract until settled, s\",\"layer rows: mean force of the settling window, g\","
           "\"layer rows: difference of the last two window averages at the settled moment, g\",\"Layer height of this run, mm\","
           "\"Plate height above the levelled contact, mm (empty before the levelling is done): the resin gap under the plate, since nothing is cured\",\"Time since the start of the first wait of the run, hh:mm:ss (empty before it)\",\"Tank heater output: 1 = on, 0 = off (app heat_control lines; the pre-heat and the Wait and heat thermostat both show here)\"\n");
}

/* IDLE LINES THAT ARRIVE LATE (test aborted in layer 3 with "a move did not finish in time"): the app writes its log in buffered blocks, so
 * "axis N is idle" can reach the file 10-20 s after the move ended (the line of the abort's own tilt-back move was found 11 s late). A move is therefore also
 * accepted as finished by TIME: dist / speed * 1.1 + 3 s after its start (the tilt: always; Z: only when the live Z position is at the target). The idle line that
 * arrives later is remembered (wt_late, the counter value at that moment) and the next move of that axis waits for one more line, so a late line is never
 * taken for the end of the next move. Row "note-idle-late" marks every such move. The final limit is 60 s. */
static unsigned wt_late[2], wt_late_c[2];
static int wt_nlate;

/* ---- moves (the Z rise and the tilt return of a layer run at the same time, so each axis has its own state) ---- */
static void wt_move(int axis, float tgt, float spd)
{
    void (*mv)(int, int, float, float) = (void (*)(int, int, float, float))(uintptr_t)Z_MOVE;
    int ok;
    double from = axis ? wt_xpos(&ok) : wt_zpos(&ok), dist = from > tgt ? from - tgt : tgt - from;
    if (wt_late[axis]) {
        sglog_force_follow();                                  /* the late line may have arrived by now */
        if ((axis ? lg_x_idle : lg_z_idle) != wt_late_c[axis])
            wt_late[axis] = 0;
    }
    wt_mx[axis].i0 = axis ? lg_x_idle : lg_z_idle;
    wt_mx[axis].need = 1 + wt_late[axis];
    wt_mx[axis].tgt = tgt;
    wt_mx[axis].fb = now_us() + (uint64_t)((dist / spd * 1.1 + 3.0) * 1e6);
    wt_mv_t0 = lg_z_trig;
    wt_mx[axis].dead = now_us() + (uint64_t)((dist / spd + 60.0) * 1e6);
    wt_mx[axis].due = now_us() + (uint64_t)((dist / spd + 1.5) * 1e6);      /* the move should be over by now: read the app log directly (a missed idle line aborted a test) */
    wt_mx[axis].recheck = 0;
    wt_mx[axis].on = 1;
    mv(axis, 0, tgt, spd);
}

/* one axis: 0 = still moving, 1 = done (or no move), -1 = failed (wt_fail says why) */
static int wt_chk(int axis)
{
    if (!wt_mx[axis].on)
        return 1;
    if (lg_z_trig != wt_mv_t0) {
        snprintf(wt_fail, sizeof wt_fail, "the foreign matter guard tripped");
        return -1;
    }
    if ((axis ? lg_x_idle : lg_z_idle) - wt_mx[axis].i0 >= wt_mx[axis].need) {
        wt_mx[axis].on = 0;
        wt_late[axis] = 0;
        return 1;
    }
    if (now_us() > wt_mx[axis].fb) {                           /* the idle line is late: finished by time (the tilt) / by time and position (Z) */
        int ok = 1;
        if (axis == 0) {
            double z = wt_zpos(&ok);
            ok = ok && z > (double)wt_mx[0].tgt - 0.02 && z < (double)wt_mx[0].tgt + 0.02;
        }
        if (ok) {
            wt_mx[axis].on = 0;
            wt_late[axis]++;
            wt_late_c[axis] = axis ? lg_x_idle : lg_z_idle;
            wt_nlate++;
            wt_row("note-idle-late", -1, 0, 0);
            return 1;
        }
    }
    if (now_us() > wt_mx[axis].due && now_us() >= wt_mx[axis].recheck) {   /* every 0.25 s: the log follower thread may be late or have missed the line */
        wt_mx[axis].recheck = now_us() + 250000u;
        sglog_force_follow();
        if ((axis ? lg_x_idle : lg_z_idle) - wt_mx[axis].i0 >= wt_mx[axis].need) {
            wt_mx[axis].on = 0;
            wt_late[axis] = 0;
            wt_nrecheck++;
            wt_row("note-idle-line-found-by-recheck", -1, 0, 0);
            return 1;
        }
        if (lg_z_trig != wt_mv_t0) {
            snprintf(wt_fail, sizeof wt_fail, "the foreign matter guard tripped");
            return -1;
        }
    }
    if (now_us() > wt_mx[axis].dead) {
        snprintf(wt_fail, sizeof wt_fail, "a move did not finish in time");
        return -1;
    }
    return 0;
}

/* both axes: 0 = a move is still running, 1 = all done, -1 = failed */
static int wt_mv_done(void)
{
    int a = wt_chk(0), b = wt_chk(1);
    return a < 0 || b < 0 ? -1 : a && b;
}

static void wt_stop_axes(void)
{
    struct timespec ts = {0, 300000000};
    ((void (*)(int))(uintptr_t)Z_STOP)(0);
    ((void (*)(int))(uintptr_t)Z_STOP)(1);
    nanosleep(&ts, NULL);                                  /* the idle lines of the stopped moves must not count for the next move */
    wt_mx[0].on = wt_mx[1].on = 0;
    wt_late[0] = wt_late[1] = 0;
}

static uint64_t wt_gov_off;                                         /* us: the wait test's heater was switched off by the thermostat (heat_govern, sgwait.c; 0 = not) */

static void wt_heater_off(void)
{
    wt_gov_off = 0;
    if (wt_heated) {
        heat_release();                                    /* a pre-heat that the thermostat switched off goes back on */
        if (h_ok && HW(H_DEV + 0x114) == 2)
            HW(H_DEV + 0x114) = 0;
    }
    wt_heated = 0;
}

static void wt_set_phase(int ph)
{
    wt_ph = ph;
    if (ph == WT_TOLEV)
        wt_logging = 1;                                     /* the log starts with the first level phase */
    wt_t_ph = now_us();
    wt_step_state = 0;
}

static void wt_finish(int state, const char *msg)
{
    wt_guard_restore();
    wt_heater_off();
    wt_row("row", -1, 0, 0);
    if (wt_fd >= 0) {
        fsync(wt_fd);
        real_close(wt_fd);
        wt_fd = -1;
    }
    pthread_mutex_lock(&wt_lock);
    wt_state = state;
    snprintf(wt_msg, sizeof wt_msg, "%s", msg);
    pthread_mutex_unlock(&wt_lock);
    wt_ph = WT_OFF;
    wt_busy = 0;
}

/* an error / stop: stop both axes, tilt flat, plate up to 50 mm, then wt_finish(3, why) */
static void wt_abort(const char *why)
{
    int ok;
    double x, z;
    if (wt_ph == WT_ABT_T || wt_ph == WT_ABT_Z)
        return;
    if (why != wt_fail)                                    /* wt_abort(wt_fail) passes the buffer itself */
        snprintf(wt_fail, sizeof wt_fail, "%s", why);
    wt_guard_restore();
    wt_heater_off();
    wt_stop_axes();
    x = wt_xpos(&ok);
    if (ok && x > 0.5) {
        wt_move(1, 0.0f, WT_TILT_HOME);
        wt_set_phase(WT_ABT_T);
        return;
    }
    z = wt_zpos(&ok);
    if (ok && z < Z_END - 0.1) {
        wt_move(0, Z_END, Z_MIX_SPD);
        wt_set_phase(WT_ABT_Z);
        return;
    }
    wt_finish(3, wt_fail);
}

/* NULL = a test may start; else why not */
static const char *wt_why(void)
{
    double pos;
    const char *w;
    if (!h_ok || !w_ok || !in_chitu || access("/customer/mod/sgwtest_disable", F_OK) == 0)
        return "not available";
    if (wt_ph != WT_OFF)
        return NULL;
    if (mix_ph || z_st)
        return "the resin mixing is running";
    if (xm_busy && now_us() < xm_until)
        return "the tilt is moving";
    if (HW(H_PSTATE) == 10 || s_active)
        return "a print job is running";
    if ((w = z_why_not(1, &pos)))
        return w;
    if ((w = xm_why(&pos)))
        return w;
    return NULL;
}

/* the machine values of this run, checked; NULL = ok, else why not */
static const char *wt_load_machine(void)
{
    wt_park = HF(H_DET_A); wt_det_b = HF(H_DET_B); wt_det_spd = HF(H_DET_SPD);
    wt_lev_a = HF(H_LEV_A); wt_lev_b = HF(H_LEV_B); wt_lev_step = HF(H_LEV_STEP); wt_lev_spd = HF(H_LEV_SPD); wt_zspd = HF(H_Z_INITSPD);
    if (!(wt_park > 20.0f && wt_park < 200.0f) || !(wt_det_b > -5.0f && wt_det_b < wt_park) || !(wt_det_spd >= 0.2f && wt_det_spd <= 10.0f))
        return "the resin detection settings (I204) look wrong";
    if (!(wt_lev_a > wt_lev_b && wt_lev_a < wt_det_b + 0.01f) || !(wt_lev_b > -5.0f) || !(wt_lev_step >= 0.01f && wt_lev_step <= 0.5f) ||
        !(wt_lev_spd >= 0.2f && wt_lev_spd <= 10.0f))
        return "the auto-levelling settings (I205) look wrong";
    if (!(wt_zspd >= 0.005f && wt_zspd <= 5.0f))
        return "the Z initial speed (I5 E) looks wrong";
    if (HI(H_LEVEL_THS) < 100 || HI(H_LEVEL_THS) > 20000 || HI(H_PROBE_THS) < 5 || HI(H_PROBE_THS) > 1000)
        return "the probe / level thresholds look wrong";
    return NULL;
}

static void wt_start(double h, int n)
{
    char path[200], name[96], ts[24];
    struct timespec rt;
    struct tm tm;
    const char *why = wt_why();
    if (!why)
        why = wt_load_machine();
    if (why) {
        pthread_mutex_lock(&wt_lock);
        snprintf(wt_msg, sizeof wt_msg, "not started: %s", why);
        wt_state = 3;
        pthread_mutex_unlock(&wt_lock);
        return;
    }
    clock_gettime(CLOCK_REALTIME, &rt);
    gmtime_r(&rt.tv_sec, &tm);
    if (tm.tm_year + 1900 >= 2020)
        strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tm);
    else
        snprintf(ts, sizeof ts, "boot%llu", (unsigned long long)(now_us() / 1000000u));
    snprintf(name, sizeof name, "waittest_%.3fmm_%s.csv", h, ts);
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, WT_DIR);
    mkdir(sg_csvdir, 0755);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/%s/%s", sg_csvdir, WT_DIR, name);
    wt_fd = real_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (wt_fd < 0) {
        pthread_mutex_lock(&wt_lock);
        snprintf(wt_msg, sizeof wt_msg, "not started: the datalog could not be created");
        wt_state = 3;
        pthread_mutex_unlock(&wt_lock);
        return;
    }
    wt_header();
    pthread_mutex_lock(&wt_lock);
    wt_state = 1;
    wt_msg[0] = 0;
    snprintf(wt_file, sizeof wt_file, "%s", name);
    wt_h = h;
    wt_n = n;
    wt_layer = 0;
    wt_nres = 0;
    pthread_mutex_unlock(&wt_lock);
    wt_busy = 1;
    wt_t_start = now_us();
    wt_wait0_us = 0;
    wt_logging = 0;
    wt_late[0] = wt_late[1] = 0;
    wt_nlate = 0;
    wt_t_row = wt_t_start;
    wt_hp0 = hpos;
    wt_rn = 0;
    wt_prev_settle = -1;
    wt_z0_ok = 0;
    wt_heated = 0;
    wt_fail[0] = 0;
    {
        int ok;
        double x = wt_xpos(&ok), z = wt_zpos(&ok);
        if (x > 0.5) {
            wt_move(1, 0.0f, WT_TILT_HOME);
            wt_set_phase(WT_PREP_T);
        } else if (z < wt_park - 0.1) {
            wt_move(0, wt_park, Z_MIX_SPD);
            wt_set_phase(WT_PREP_Z);
        } else {
            wt_set_phase(WT_CAL);
        }
    }
}

/* the tilt release or return: stage speeds / angle from the live profile */
static void wt_tilt_stage(int release, int stage)
{
    double n = HW(T_PROF + 0x34);
    int blk2 = wt_layer > (int)n;                     /* layers <= N: A..F, later: S..Y */
    const unsigned o = blk2 ? 6 : 0;                  /* floats: A B C D E F | S P R T X Y */
    double ret1 = HF(T_PROF + 4u * (o + 0)), ret2 = HF(T_PROF + 4u * (o + 1)), rel1 = HF(T_PROF + 4u * (o + 2)), rel2 = HF(T_PROF + 4u * (o + 3));
    double retang = HF(T_PROF + 4u * (o + 4)), relang = HF(T_PROF + 4u * (o + 5));
    float tgt, spd;
    if (release) {
        if (stage == 1) {
            tgt = relang > 0 && relang < WT_TILT_MAX ? (float)relang : WT_TILT_MAX;
            spd = (float)rel1;
        } else {
            tgt = WT_TILT_MAX;
            spd = (float)rel2;
        }
    } else {
        if (stage == 1) {
            tgt = retang > 0 && retang < WT_TILT_MAX ? (float)retang : 0.0f;
            spd = (float)ret1;
        } else {
            tgt = 0.0f;
            spd = (float)ret2;
        }
    }
    (void)rel1; (void)rel2; (void)ret1; (void)ret2;
    spd = WT_TILT_SPD;                                /* user: the test always tilts at the printer's default 36 deg/s (not the profile speeds) */
    wt_move(1, tgt, spd);
}

static int wt_two_stage(int release)
{
    double n = HW(T_PROF + 0x34);
    const unsigned o = wt_layer > (int)n ? 6 : 0;
    double ang = HF(T_PROF + 4u * (o + (release ? 5 : 4)));
    return ang > 0 && ang < WT_TILT_MAX;
}

static void wt_layer_start(void)
{
    pthread_mutex_lock(&wt_lock);
    wt_layer++;
    pthread_mutex_unlock(&wt_lock);
    wt_guard_off();                                      /* the first layer passed: the foreign matter detection is over for this run */
    wt_tilt_stage(1, 1);
    wt_set_phase(WT_REL1);
}

/* the tilt has arrived at the release angle: the Z rise by one layer height and the tilt return start together (as the app does) */
static void wt_release_done(void)
{
    wt_move(0, (float)(wt_z0 + (wt_layer + 1) * wt_h), wt_zspd);
    wt_tilt_stage(0, 1);
    wt_set_phase(WT_RET1);
}

static void wt_wait_begin(void)
{
    wt_set_phase(WT_WAIT);
    wt_t0 = now_us();
    wt_gnow = -1;
    wt_heated = 0;
    if (wset.wh && h_ok && HB(H_UI + 0x14) && HW(H_DEV) && HB(H_DEV + 0x118) && (wt_prev_settle < 0 || wt_prev_settle >= W_HEATMIN))
        wt_heated = heat_start(&wt_gov_off);
}

static void wt_wait_step(void)
{
    double mean = 0, g, el = (now_us() - wt_t0) / 1e6;
    uint64_t last;
    int n;
    (void)wt_mean_raw(now_us() - 5000000ull, &n, &last);
    if (!n || now_us() - last > 6000000ull) {
        wt_abort("the force gauge stopped sending");
        return;
    }
    wt_el = el;
    if (wt_heated)
        heat_govern(&wt_gov_off);
    g = wt_block_diff(wt_t0, wset.win > WT_WIN_MAX ? WT_WIN_MAX : wset.win, &mean);   /* g = |last average - previous average| */
    wt_gnow = g;
    if (g >= 0 && g < wset.grad) {                                   /* two consecutive averages of win s differ by less than N g */
        double plateau = (mean - wt_zero_raw) / wt_k();
        wt_row("layer", el, plateau, g);
        pthread_mutex_lock(&wt_lock);
        if (wt_nres < WT_NRES)
            wt_res[wt_nres++] = (float)el;
        pthread_mutex_unlock(&wt_lock);
        wt_prev_settle = el;
        wt_heater_off();
        if (wt_layer >= wt_n - 1 || wt_z0 + (wt_layer + 2) * wt_h > Z_END - 0.05) {   /* the waits of layers 0 .. N-1 are done, or the next step would pass the end height (large steps) */
            int ok;
            double z = wt_zpos(&ok);
            if (ok && z < Z_END - 0.1) {
                wt_move(0, Z_END, Z_MIX_SPD);
                wt_set_phase(WT_ENDUP);
            } else {
                wt_finish(2, "finished");
            }
        } else {
            wt_layer_start();
        }
    }
}

static void wt_poll_step(void)
{
    double u = 0, pos;
    int ok, r;
    const char *w;
    if (HW(H_PSTATE) == 10 || s_active) {
        wt_abort("a print job started");
        return;
    }
    switch (wt_ph) {
    case WT_PREP_T:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) {
            pos = wt_zpos(&ok);
            if (ok && pos < wt_park - 0.1) { wt_move(0, wt_park, Z_MIX_SPD); wt_set_phase(WT_PREP_Z); }
            else wt_set_phase(WT_CAL);
        }
        break;
    case WT_PREP_Z:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) wt_set_phase(WT_CAL);
        break;
    case WT_CAL:
        if (now_us() - wt_t_ph >= (uint64_t)(WT_CAL_S * 1e6)) {
            int n;
            double m = wt_mean_raw(now_us() - (uint64_t)(WT_CAL_AVG * 1e6), &n, NULL);
            if (n < 10) { wt_abort("not enough force readings for the zero"); return; }
            wt_zero_raw = m;
            if ((w = z_why_not(1, &pos))) { wt_abort(w); return; }
            pos = wt_zpos(&ok);
            if (ok && pos > wt_park + 0.5) {                           /* parked high: down to the probe start height at the travel speed first (in air) */
                wt_move(0, wt_park, Z_MIX_SPD);
                wt_set_phase(WT_TOPARK);
            } else {
                wt_move(0, wt_det_b, wt_det_spd);                      /* down to the probe end position at most */
                wt_set_phase(WT_PROBE);
            }
        }
        break;
    case WT_TOPARK:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) {
            wt_move(0, wt_det_b, wt_det_spd);
            wt_set_phase(WT_PROBE);
        }
        break;
    case WT_PROBE:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) { wt_abort("no resin found by the probe"); return; }
        if (wt_force_u(&u) && u > (double)HI(H_PROBE_THS)) {
            wt_stop_axes();
            pos = wt_zpos(&ok);
            wt_move(0, (float)(pos + WT_BACK_MM), WT_UP_SPD);
            wt_set_phase(WT_BACK);
        }
        break;
    case WT_BACK:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) {
            pos = wt_zpos(&ok);
            if (pos > wt_lev_a + 0.05) { wt_move(0, wt_lev_a, wt_lev_spd); wt_set_phase(WT_TOLEV); }
            else wt_set_phase(WT_LSTILL);
        }
        break;
    case WT_TOLEV:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) wt_set_phase(WT_LSTILL);
        break;
    case WT_LSTILL:
        if (now_us() - wt_t_ph >= (uint64_t)(WT_LSTILL_S * 1e6)) { wt_step = 0; wt_guard_raise(); wt_set_phase(WT_LPRESS); }
        break;
    case WT_LPRESS:
        if (wt_step_state == 0) {                                   /* look at the force first: the start may already be at the target */
            if (wt_force_u(&u) && u > (double)HI(H_LEVEL_THS)) { wt_z0 = wt_zpos(&ok); wt_z0_ok = 1; wt_step_state = 9; }
            else {
                pos = wt_zpos(&ok);
                if (pos - wt_lev_step < wt_lev_b - 0.001f) { wt_abort("the levelling end position was reached without the target force"); return; }
                wt_move(0, (float)(pos - wt_lev_step), wt_lev_spd);
                wt_step++;
                wt_step_state = 1;
            }
        } else if (wt_step_state == 1) {
            if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
            if (r) { wt_t_ph = now_us(); wt_step_state = 2; }
        } else if (wt_step_state == 2) {
            if (now_us() - wt_t_ph >= WT_STEP_HOLD) wt_step_state = 0;
        }
        if (wt_step_state == 9) {
            wt_guard_restore();
            wt_move(0, (float)(wt_z0 + wt_h), Z_MIX_SPD);           /* to the first layer height at 5 mm/s, as the app does after its levelling (log of a real print) */
            wt_set_phase(WT_UP1);
        }
        break;
    case WT_UP1:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) wt_wait_begin();                                      /* layer 0: the wait right after the levelling, no tilt cycle before it (as the print) */
        break;
    case WT_REL1:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) {
            if (wt_two_stage(1)) { wt_tilt_stage(1, 2); wt_set_phase(WT_REL2); }
            else wt_release_done();
        }
        break;
    case WT_REL2:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) wt_release_done();
        break;
    case WT_RET1:                                                   /* the tilt return, stage 1; the Z rise runs alongside */
        if ((r = wt_chk(1)) < 0 || wt_chk(0) < 0) { wt_abort(wt_fail); return; }
        if (r && wt_two_stage(0)) { wt_tilt_stage(0, 2); wt_set_phase(WT_RET2); }
        else if (r && wt_mv_done() == 1) wt_wait_begin();           /* the wait starts when the return AND the Z rise are over */
        break;
    case WT_RET2:
        if (wt_mv_done() < 0) { wt_abort(wt_fail); return; }
        if (wt_mv_done() == 1) wt_wait_begin();
        break;
    case WT_WAIT:
        wt_wait_step();
        break;
    case WT_ENDUP:
        if ((r = wt_mv_done()) < 0) { wt_abort(wt_fail); return; }
        if (r) wt_finish(2, "finished");
        break;
    case WT_ABT_T:
        if (wt_mv_done() != 0) {
            wt_mx[0].on = wt_mx[1].on = 0;
            pos = wt_zpos(&ok);
            if (ok && pos < Z_END - 0.1) { wt_move(0, Z_END, Z_MIX_SPD); wt_set_phase(WT_ABT_Z); }
            else wt_finish(3, wt_fail);
        }
        break;
    case WT_ABT_Z:
        if (wt_mv_done() != 0)
            wt_finish(3, wt_fail);
        break;
    default:
        break;
    }
}

/* 20 Hz from the wait thread */
static void sgwtest_poll(void)
{
    int start, stop;
    double h;
    int n;
    if (!w_ok || !h_ok)
        return;
    pthread_mutex_lock(&wt_lock);
    start = wt_req.start; stop = wt_req.stop; h = wt_req.h; n = wt_req.n;
    wt_req.start = wt_req.stop = 0;
    pthread_mutex_unlock(&wt_lock);
    if (stop && wt_ph != WT_OFF)
        wt_abort("stopped");
    if (start && wt_ph == WT_OFF)
        wt_start(h, n);
    if (wt_ph == WT_OFF)
        return;
    wt_accum();
    if (now_us() - wt_t_row >= WT_ROW_US) {
        wt_t_row = now_us();
        wt_row("row", -1, 0, 0);
    }
    if (wt_ph == WT_OFF)
        return;
    if (wt_ph != WT_WAIT)
        wt_gnow = -1;
    wt_poll_step();
    if (wt_ph != WT_OFF && wt_ph != WT_WAIT)
        wt_el = 0;
}

/* "start H N" | "stop" (command thread) */
static void sgwtest_cmd(const char *c, char *res, size_t len)
{
    pthread_mutex_lock(&wt_lock);
    if (strncmp(c, "stop", 4) == 0) {
        wt_req.stop = 1;
    } else if (strncmp(c, "start ", 6) == 0) {
        double v[2];
        const char *why;
        if (parse_nums(c + 6, v, 2) != 2 || v[0] < 0.0095 || v[1] < 1 || v[1] > 999 || v[1] != (double)(int)v[1]) {
            snprintf(res, len, "error: wtest start HEIGHT_MM(>= 0.010, no upper limit; the test ends when the plate would pass 50 mm) LAYERS(1-999)");
        } else if (wt_ph != WT_OFF || wt_req.start) {
            snprintf(res, len, "error: a test is already running");
        } else if ((why = wt_why())) {
            snprintf(res, len, "error: %s", why);
        } else {
            wt_req.start = 1;
            wt_req.h = (double)(int)(v[0] * 1000 + 0.5) / 1000.0;
            wt_req.n = (int)v[1];
        }
    } else {
        snprintf(res, len, "error: bad wtest command");
    }
    pthread_mutex_unlock(&wt_lock);
}

static int sgwtest_json(char *buf, size_t len)
{
    char file[100], msg[100], why[100];
    const char *w;
    int n, i, st, ly, nn, nr;
    double h, el, g, z, x;
    float res[WT_NRES];
    if (!h_ok || !w_ok)
        return 0;
    w = wt_why();
    pthread_mutex_lock(&wt_lock);
    js(file, sizeof file, wt_file);
    js(msg, sizeof msg, wt_msg);
    st = wt_state; ly = wt_layer; nn = wt_n; h = wt_h; nr = wt_nres;
    memcpy(res, wt_res, sizeof(float) * (size_t)nr);
    pthread_mutex_unlock(&wt_lock);
    js(why, sizeof why, w ? w : "");
    el = wt_el; g = wt_gnow; z = wt_zmm; x = wt_xdeg;
    n = snprintf(buf, len, ",\"wt\":{\"st\":%d,\"ph\":\"%s\",\"layer\":%d,\"n\":%d,\"h\":%.3f,\"el\":%.1f,\"g\":%.3f,\"grad\":%.2f,\"win\":%.1f,\"z\":%.2f,\"x\":%.1f,"
                 "\"file\":\"%s\",\"msg\":\"%s\",\"why\":\"%s\",\"res\":[", st, wt_phname[wt_ph], ly, nn, h, el, g, wset.grad, wset.win, z, x, file, msg, why);
    for (i = 0; i < nr && n < (int)len - 16; i++)
        n += snprintf(buf + n, len - (size_t)n, "%s%.1f", i ? "," : "", (double)res[i]);
    if (n < (int)len - 4)
        n += snprintf(buf + n, len - (size_t)n, "]}");
    return n;
}
