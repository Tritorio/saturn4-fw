/*
 * sgheat.c - resin vat heating from the dashboard (step 8.9), #included by sgtap.c after
 * sglog.c and before sgweb.c. Does what the touch screen does, from our writer thread
 * (chitu is non-PIE: fixed addresses, see docs/tank_heating.txt):
 *   - reads the vat temperature, the heater state and the settings for live.json and the
 *     CSV (column tank_c);
 *   - command "heat P TPRE H TPRINT" (sgweb.c run_cmd): P = pre-heat now 0|1,
 *     TPRE = pre-heat target, H = heating during print 0|1, TPRINT = print target (C).
 *     All checked first, then applied together: targets -> live machine params + the
 *     app's own saver (machine_param_ref.bin), H -> live ui param + ui saver; pre-heat
 *     on/off like the touch screen (heater state 3 + RAM flag; refused while printing or
 *     without a working sensor, or at/above the vat limit).
 *   - RESIN MIXING (user, replaces "Plate into the resin"): command "mix MIN" (0 = stop): the build plate goes to Z low (the old
 *     pre-heat position: 6 mm, 3 mm with the factory Z values), then up to Z low + 30 mm and down again, at 5 mm/s over the whole travel, for MIN
 *     minutes counted from the first arrival at Z low; it ends by rising to Z = 50 mm (also after Stop). App's own axis_move (the foreign matter guard acts on the way down); one
 *     move at a time, the next when the app logs "axis 0 is idle". Stop / pre-heat off / print job / guard trip: axis_stop (guard trip / print job: stays put), else Stop / pre-heat off: up to 50 mm.
 *     FIRST DESCENT TO Z LOW = THE FOREIGN MATTER CHECK: it runs at 2.5 mm/s (as the resin probe) with the app's guard active (cold, viscous resin
 *     made the 5 mm/s strokes trip the guard on resin drag alone). If it arrives without a trip, the live limit (+0x230) is raised to INT_MAX for the rest of the mixing
 *     (restored on stop / done / pre-heat off / print job / guard-less exit), and the strokes run at 5 mm/s; live.json heat.mixg = 1 while the guard is ignored.
 *     Idle printer, Z homed, guard armed only. The "heat" command's 5th number (old plate option) is accepted and ignored.
 * Only inside /customer/resources/chitu V1.5.9: every code word below must match before
 * anything is read or written, else heating is off (live.json "heat": null).
 * Kill switch: /customer/mod/sgheat_disable (checked at start).
 */

#define H_DEV      0x2fed38u   /* heater: +0 sensor present, +4 vat C (float), +0x114 state,
                                  +0x118 ok (u8). state: 0 off, 1 print warm-up, 2 pulse, 3 pre-heat */
#define H_PSTATE   0x30280cu   /* printer state, 10 = printing */
#define H_UI       0x39fe78u   /* live ui params: +0x14 heat during print, +0x15 pre-heat (RAM) */
#define H_MACH     0x39fa30u   /* live machine params: +0x194 vat limit, +0x198 print target,
                                  +0x1a0 pre-heat target (int32, C) */
#define H_SETSTATE 0x8aae0u    /* void set_heater_state(int) */
#define H_SAVEMACH 0x686f4u    /* int save_machine_params(void), app mutex inside */
#define H_SAVEUI   0x68728u    /* int save_ui_params(void), app mutex inside */
#define H_TMIN     20
#define H_TPREMAX  40         /* pre-heat: the resin is not stirred (page warns above 30) */
#define H_TPRTMAX  50

#define HW(a)  (*(volatile uint32_t *)(uintptr_t)(a))
#define HB(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define HI(a)  (*(volatile int32_t *)(uintptr_t)(a))
#define HF(a)  (*(volatile float *)(uintptr_t)(a))

static int in_chitu;                  /* set by sgtap_init: exe is the real app */
static int h_ok;                      /* 1 = code verified, heating available */

/* code words of V1.5.9 that the addresses above depend on */
static const struct { uint32_t a, w; } h_sig[] = {
    {0x4dcc0, 0xe92d4010}, {0x4dcc4, 0xeb01bf7b}, {0x4dcc8, 0xe30f3a30}, {0x4dccc, 0xe3403039},
    {0x4dcd0, 0xe350000a}, {0x4dcd4, 0x05930198}, {0x4dcd8, 0x159301a0}, {0x4dcdc, 0xe8bd8010},
    {0x686f4, 0xe92d4070}, {0x686f8, 0xe30d464c}, {0x68708, 0xe30b0554}, {0x6870c, 0xe3400034},
    {0x68710, 0xebfff941}, {0x68724, 0xe8bd8070},
    {0x68728, 0xe92d4070}, {0x6872c, 0xe30d464c}, {0x6873c, 0xe30a0d50}, {0x68740, 0xe3400034},
    {0x68744, 0xebfffdb9}, {0x68758, 0xe8bd8070},
    {0x8aad0, 0xe30e3d38}, {0x8aad4, 0xe340302f}, {0x8aad8, 0xe5930114}, {0x8aae0, 0xe30e3d38},
    {0x8aae4, 0xe340302f}, {0x8aae8, 0xe5830114}, {0x8aaec, 0xe3a00000},
    {0xbdab8, 0xe3023808}, {0xbdabc, 0xe3403030}, {0xbdac0, 0xe5930004},
    {0x6516c, 0xe5d51014}, {0x65188, 0xe5c41004}, {0x65558, 0xe2841f65},
    {0x48b0c, 0xe5d46015}, {0x48b28, 0xe5c43015}, {0x48b50, 0xe5d32014}, {0x48b58, 0xe5c32014},
    {0x4caf8, 0xe5d33015}, {0x4cb44, 0xe5d33014},
    {0x5361c, 0xe92d43f0}, {0x53620, 0xe30a4124}, {0x53624, 0xe340402f}, {0x53628, 0xeef70ac0},
};

static void z_load(void);
static int parse_int(const char **p, int *v);

static void sgheat_init(void)
{
    if (!in_chitu || access("/customer/mod/sgheat_disable", F_OK) == 0)
        return;
    for (unsigned i = 0; i < sizeof h_sig / sizeof h_sig[0]; i++)
        if (HW(h_sig[i].a) != h_sig[i].w)
            return;
    h_ok = 1;
    z_load();
}

/* ---- plate move with the dashboard's pre-heat ---- */
#define Z_PTR      0x2fa124u   /* axis structs [axis]; Z = 0 */
#define Z_MOVE     0x5361cu    /* void axis_move(int axis, int flags, float mm, float mm_s); flags 0 = absolute */
#define Z_STOP     0x535b8u    /* void axis_stop(int axis): driver stop + queues flushed + position re-read; the app's own stop path calls it for axis 0 (Z) */
#define Z_ARMED    0x2fdcb4u   /* int (0x2fdc58 + 0x5c): first sg calibration of the session done = foreign matter guard armed */
#define Z_MIX_SPD  5.0f        /* mm/s, the whole travel */
#define Z_CHK_SPD  2.5f        /* mm/s: the first descent to Z low (the foreign matter check), as the resin probe of the print start */
#define Z_LIMIT    (H_MACH + 0x230u) /* int: foreign matter limit (I206 C) the app's guard 0x53ab8 reads at every check */
#define Z_LIM_OFF  0x7fffffff  /* the limit while the mixing ignores the guard */
#define Z_END      50.0f       /* mm: where the plate goes when the mixing ends or is stopped */
#define Z_MIX_SPAN 30.0f       /* mm: Z high = Z low + this */
#define Z_PRE_FLEX 6.0f        /* mm: plate position while pre-heating (flex plate / not factory values) */
#define Z_PRE_STD  3.0f        /* mm: the same with the factory Z values (Flex Plate switch off, sgmach.c) */
static float z_pre_mm(void);   /* sgmach.c */

static int z_opt;              /* the old plate option: always 0 now (kept for the settings snapshot) */
static int z_quiet;            /* set by the settings restore: changing the option must not move the plate */
static int z_st;               /* 0 idle, 4 a mixing stroke is moving, 5 the plate rises to 50 mm at the end or after a stop */
static volatile int wt_busy;      /* 1 = a Wait Times Testing run owns the plate and the tilt (sgwtest.c) */
static int mix_chk;            /* 0 = the first descent (slow, guard active) not done yet, 1 = done: the guard is ignored */
static int g_off;              /* 1 = we hold the foreign matter limit at Z_LIM_OFF */
static int g_saved;            /* the app's limit while g_off */
static int z_lastdown;         /* the move in progress goes down */
static int mix_ph;             /* 0 off, 1 first move to Z low, 2 up, 3 down */
static unsigned mix_n;         /* strokes (up + down = 1) */
static uint64_t mix_end;       /* us: end of the mixing time (0 until the first arrival at Z low) */
static uint64_t mix_dur;       /* us */
static float z_tgt;            /* mm: target of the move in progress */
static uint64_t z_dead;        /* us: give up on the stage */
static unsigned z_idle0, z_trig0;
static char z_msg[72] = "";

static void z_load(void)
{
    z_opt = 0;
}

/* the Z axis struct when it looks right, else 0 */
static uint32_t z_axis(double *mm_step)
{
    uint32_t p = HW(Z_PTR);
    double d;
    if (p < 0x100000u || p > 0x10000000u || (p & 3))
        return 0;
    d = *(volatile double *)(uintptr_t)(p + 0x18);
    if (!(d > 1e-4 && d < 1e-2) || HI(p + 0x3c) < 100000 || HI(p + 0x3c) > 2000000)
        return 0;
    *mm_step = d;
    return p;
}

/* NULL = the plate may move (down = the move goes down); else why not */
static const char *z_why_not(int down, double *pos_mm)
{
    double d;
    uint32_t p;
    if (!h_ok || !in_chitu)
        return "plate control not available";
    if (HW(H_PSTATE) == 10 || s_active)
        return "a print job is running";
    if (!(p = z_axis(&d)))
        return "Z axis not found";
    if (HB(p + 0x49) != 3 || !HB(p + 0x4a))
        return "Z is not homed";
    if (down && !HW(Z_ARMED))
        return "the foreign matter guard is not armed yet (about a minute after boot)";
    *pos_mm = HI(p + 0x50) * d;
    if (*pos_mm < -1 || *pos_mm > HI(p + 0x3c) * d + 1)
        return "Z position unknown";
    return NULL;
}

static void z_go(float mm, float speed, double from, int st)
{
    void (*move)(int, int, float, float) = (void (*)(int, int, float, float))(uintptr_t)Z_MOVE;
    double dist = from > mm ? from - mm : mm - from;
    z_idle0 = lg_z_idle;
    z_trig0 = lg_z_trig;
    z_dead = now_us() + (uint64_t)((dist / speed + 20.0) * 1e6);
    z_st = st;
    z_tgt = mm;
    z_lastdown = mm < from;
    snprintf(z_msg, sizeof z_msg, "%s %.0f mm", mm > from ? "going up to" : "going down to", (double)mm);
    move(0, 0, mm, speed);
}

/* ignore the foreign matter guard (only after a clean slow descent) / give the app's limit back */
static void guard_off(void)
{
    if (g_off)
        return;
    g_saved = (int)HI(Z_LIMIT);
    HI(Z_LIMIT) = Z_LIM_OFF;
    g_off = 1;
}

static void guard_restore(void)
{
    if (!g_off)
        return;
    if (HI(Z_LIMIT) == Z_LIM_OFF)             /* the user may have changed it meanwhile: then leave it */
        HI(Z_LIMIT) = (uint32_t)g_saved;
    g_off = 0;
}

/* stop the mixing (or any plate move of ours) at once and send the plate up to Z_END */
static void mix_stop(const char *why)
{
    double pos;
    const char *w;
    struct timespec ts = {0, 300000000};
    int was = mix_ph || z_st;
    guard_restore();
    mix_ph = 0;
    z_st = 0;
    if (!was)
        return;
    ((void (*)(int))(uintptr_t)Z_STOP)(0);
    nanosleep(&ts, NULL);                      /* the idle line of the stopped move must not count for the next one */
    if ((w = z_why_not(0, &pos))) {
        snprintf(z_msg, sizeof z_msg, "stopped: %s", w);
        return;
    }
    if (pos < Z_END - 0.1) {
        z_go(Z_END, Z_MIX_SPD, pos, 5);
        snprintf(z_msg, sizeof z_msg, "%s: going up to %.0f mm", why, (double)Z_END);
    } else {
        snprintf(z_msg, sizeof z_msg, "%s", why);
    }
}

/* "mix MIN": MIN minutes of mixing, 0 = stop */
static void sgheat_mix_cmd(const char *c, char *res, size_t len)
{
    int min;
    double pos, d;
    uint32_t p;
    const char *why;
    if (!h_ok) {
        snprintf(res, len, "error: plate control not available");
        return;
    }
    if (!parse_int(&c, &min)) {
        snprintf(res, len, "error: bad mix command");
        return;
    }
    if (min == 0) {
        if (mix_ph)
            mix_stop("mixing stopped");
        return;
    }
    if (wt_busy) {
        snprintf(res, len, "error: a wait test is running");
        return;
    }
    if (mix_ph || z_st) {
        snprintf(res, len, "error: the plate is still moving");
        return;
    }
    if ((why = z_why_not(1, &pos)) || !(p = z_axis(&d))) {
        snprintf(res, len, "error: %s", why ? why : "Z axis not found");
        return;
    }
    if (z_pre_mm() + Z_MIX_SPAN > HI(p + 0x3c) * d - 0.5) {
        snprintf(res, len, "error: Z high (%.0f mm) is beyond the travel of the Z axis", (double)(z_pre_mm() + Z_MIX_SPAN));
        return;
    }
    mix_n = 0;
    mix_end = 0;
    mix_chk = 0;
    mix_dur = (uint64_t)min * 60000000ull;
    if (pos > z_pre_mm() + 0.1 || pos < z_pre_mm() - 0.1) {
        mix_ph = 1;
        z_go(z_pre_mm(), pos > z_pre_mm() ? Z_CHK_SPD : Z_MIX_SPD, pos, 4);
    } else {
        mix_ph = 2;
        mix_end = now_us() + mix_dur;
        z_go(z_pre_mm() + Z_MIX_SPAN, Z_MIX_SPD, pos, 4);
    }
}

/* every 0.5 s from the writer thread: next stroke when the app says the axis is idle */
static void sgheat_tick(void)
{
    double pos;
    const char *why;
    if (g_off && (!mix_ph || HW(H_PSTATE) == 10 || s_active))
        guard_restore();
    if (!z_st)
        return;
    if (lg_z_trig != z_trig0) {
        mix_ph = 0;
        z_st = 0;
        snprintf(z_msg, sizeof z_msg, "stopped: the foreign matter guard tripped");
    } else if (HW(H_PSTATE) == 10 || s_active) {
        mix_ph = 0;
        z_st = 0;
        snprintf(z_msg, sizeof z_msg, "stopped: a print job started");
    } else if (lg_z_idle != z_idle0) {
        if (z_st == 5) {
            snprintf(z_msg, sizeof z_msg, "plate at %.0f mm", (double)z_tgt);
            z_st = 0;
        } else if ((why = z_why_not(1, &pos))) {
            mix_ph = 0;
            z_st = 0;
            snprintf(z_msg, sizeof z_msg, "stopped: %s", why);
        } else if (mix_ph == 1 || mix_ph == 3) {             /* at Z low */
            if (!mix_chk && z_lastdown) {                    /* the slow descent ended without a trip: no foreign matter */
                mix_chk = 1;
                guard_off();
            }
            if (mix_ph == 1)
                mix_end = now_us() + mix_dur;
            else
                mix_n++;
            if (now_us() >= mix_end) {
                mix_ph = 0;
                z_st = 0;
                if (pos < Z_END - 0.1) {
                    z_go(Z_END, Z_MIX_SPD, pos, 5);
                    snprintf(z_msg, sizeof z_msg, "mixing done: going up to %.0f mm", (double)Z_END);
                } else {
                    snprintf(z_msg, sizeof z_msg, "mixing done");
                }
            } else {
                mix_ph = 2;
                z_go(z_pre_mm() + Z_MIX_SPAN, Z_MIX_SPD, pos, 4);
            }
        } else {                                              /* at Z high */
            mix_ph = 3;
            z_go(z_pre_mm(), mix_chk ? Z_MIX_SPD : Z_CHK_SPD, pos, 4);
        }
    } else if (now_us() > z_dead) {
        mix_ph = 0;
        z_st = 0;
        snprintf(z_msg, sizeof z_msg, "stopped: the move did not finish in time");
    }
}

/* vat temperature for the CSV: 1 = valid */
static int sgheat_temp(double *c)
{
    float t;
    if (!h_ok || !HW(H_DEV))
        return 0;
    t = HF(H_DEV + 4);
    if (!(t > -40 && t < 150))
        return 0;
    *c = t;
    return 1;
}

/* "heat":{...} or "heat":null for live.json */
static int sgheat_json(char *buf, size_t len)
{
    double t;
    char ts[16] = "null";
    if (!h_ok)
        return snprintf(buf, len, ",\"heat\":null");
    if (sgheat_temp(&t))
        snprintf(ts, sizeof ts, "%.2f", t);
    return snprintf(buf, len, ",\"heat\":{\"t\":%s,\"sensor\":%u,\"ready\":%u,\"state\":%u,\"on\":%d,"
                    "\"printing\":%d,\"pre\":%u,\"hp\":%u,\"tpre\":%d,\"tprint\":%d,\"max\":%d,"
                    "\"mixg\":%d,\"mix\":%d,\"mixleft\":%d,\"mixn\":%u,\"mixlo\":%.1f,\"mixhi\":%.1f,\"zmove\":%d,\"zmsg\":\"%s\"}",
                    ts, (unsigned)(HW(H_DEV) != 0), (unsigned)HB(H_DEV + 0x118),
                    (unsigned)HW(H_DEV + 0x114), lg_heat_on && HW(H_DEV + 0x114) != 0, HW(H_PSTATE) == 10, (unsigned)HB(H_UI + 0x15),
                    (unsigned)HB(H_UI + 0x14), (int)HI(H_MACH + 0x1a0), (int)HI(H_MACH + 0x198),
                    (int)HI(H_MACH + 0x194), g_off, mix_ph, mix_ph == 1 ? -1 : mix_ph ? (int)(mix_end > now_us() ? (mix_end - now_us()) / 1000000 : 0) : 0,
                    mix_n, (double)z_pre_mm(), (double)(z_pre_mm() + Z_MIX_SPAN), z_st, z_msg);
}

static int parse_int(const char **p, int *v)
{
    const char *s = *p;
    while (*s == ' ')
        s++;
    if (*s < '0' || *s > '9')
        return 0;
    *v = (int)parse_u(s);
    while (*s >= '0' && *s <= '9')
        s++;
    *p = s;
    return 1;
}

/* "P TPRE H TPRINT" -> res "ok" or "error: ..." */
static void sgheat_cmd(const char *c, char *res, size_t len)
{
    int pre, tpre, hp, tprint, max, cur_pre, zopt = -1;
    double t;
    void (*set_state)(int) = (void (*)(int))(uintptr_t)H_SETSTATE;
    int (*save_mach)(void) = (int (*)(void))(uintptr_t)H_SAVEMACH;
    int (*save_ui)(void) = (int (*)(void))(uintptr_t)H_SAVEUI;
    int save_u = 0;

    if (!h_ok) {
        snprintf(res, len, "error: heating not available (unknown app version or disabled)");
        return;
    }
    if (!parse_int(&c, &pre) || !parse_int(&c, &tpre) || !parse_int(&c, &hp) ||
        !parse_int(&c, &tprint) || pre > 1 || hp > 1) {
        snprintf(res, len, "error: bad heat command");
        return;
    }
    if (parse_int(&c, &zopt) && zopt > 1) {
        snprintf(res, len, "error: bad heat command");
        return;
    }
    max = HI(H_MACH + 0x194);
    if (tpre < H_TMIN || tpre > H_TPREMAX || tprint < H_TMIN || tprint > H_TPRTMAX ||
        tpre >= max || tprint >= max) {
        snprintf(res, len, "error: temperature out of range (pre-heat %d-%d C, print %d-%d C, "
                 "below the %d C limit)", H_TMIN, H_TPREMAX, H_TMIN, H_TPRTMAX, max);
        return;
    }
    cur_pre = HB(H_UI + 0x15);
    if (pre && !cur_pre) {                        /* the touch screen's checks */
        if (HW(H_PSTATE) == 10) {
            snprintf(res, len, "error: no pre-heat while printing");
            return;
        }
        if (!HW(H_DEV) || !sgheat_temp(&t)) {
            snprintf(res, len, "error: vat temperature sensor not found");
            return;
        }
        if (!HB(H_DEV + 0x118)) {
            snprintf(res, len, "error: vat heater not ready");
            return;
        }
        if (t >= max) {
            snprintf(res, len, "error: vat already at the %d C limit", max);
            return;
        }
    }
    /* all checked: apply */
    if (HI(H_MACH + 0x1a0) != tpre || HI(H_MACH + 0x198) != tprint) {
        HI(H_MACH + 0x1a0) = tpre;
        HI(H_MACH + 0x198) = tprint;
        if (save_mach() != 0)
            snprintf(res, len, "error: targets set, but saving machine_param_ref.bin failed");
    }
    if (HB(H_UI + 0x14) != hp) {
        HB(H_UI + 0x14) = (uint8_t)hp;
        save_u = 1;
    }
    if (pre && !cur_pre) {
        set_state(3);
        HB(H_UI + 0x15) = 1;
        save_u = 1;
    } else if (!pre && cur_pre) {
        if (HW(H_DEV + 0x114) == 3)               /* never stop the print heating */
            set_state(0);
        HB(H_UI + 0x15) = 0;
        save_u = 1;
    }
    if (!pre && mix_ph)                           /* pre-heat off ends the mixing */
        mix_stop("mixing stopped");
    if (save_u && save_ui() != 0)
        snprintf(res, len, "error: set, but saving ui_param_ref.bin failed");
}
