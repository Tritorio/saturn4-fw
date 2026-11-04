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
 *   - plate move with the dashboard's pre-heat: "heat P TPRE H TPRINT Z" (Z = 0|1, optional)
 *     sets the option <csvdir>/PREZ_ON and, when pre-heat is switched on / off by this command with Z = 1, the build
 *     plate is moved with the app's own axis_move (so the foreign matter guard acts on the way down): on = 5 mm/s
 *     down to 40 mm, then 2 mm/s down to 6 mm; off = up to 50 mm at 5 mm/s. Never from the touch screen's pre-heat.
 *     changing Z while pre-heat stays on moves the plate too (tick = down, untick = up); a settings restore never does.
 *     unticking (or pre-heat off) while the plate is still going down stops it (axis_stop(0)) and sends it back to where it was.
 *     One stage at a time, the next one when the app logs "axis 0 is idle"; idle printer, Z homed, guard armed only.
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
#define Z_FAST     5.0f        /* mm/s */
#define Z_SLOW     2.0f
#define Z_SLOW_AT  40.0f       /* mm: fast down to here, slow below */
#define Z_PRE_FLEX 6.0f        /* mm: plate position while pre-heating (flex plate / not factory values) */
#define Z_PRE_STD  3.0f        /* mm: the same with the factory Z values (Flex Plate switch off, sgmach.c) */
static float z_pre_mm(void);   /* sgmach.c */
#define Z_OFF      50.0f       /* mm: plate position after pre-heat is switched off */
#define Z_OPTFILE  "PREZ_ON"

static int z_opt;              /* option: move the plate with the dashboard's pre-heat */
static int z_quiet;            /* set by the settings restore: changing the option must not move the plate */
static int z_st;               /* 0 idle, 1 down to 40, 2 down to 6, 3 up to z_tgt (50 mm, or back to z_origin after a stop) */
static float z_origin;         /* mm: where the plate was when the last downward move started */
static float z_tgt;            /* mm: target of the move in progress */
static uint64_t z_dead;        /* us: give up on the stage */
static unsigned z_idle0, z_trig0;
static char z_msg[72] = "";

static void z_load(void)
{
    char path[160];
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, Z_OPTFILE);
    z_opt = access(path, F_OK) == 0;
}

static void z_set_opt(int on)
{
    char path[160];
    int fd;
    z_opt = on;
    snprintf(path, sizeof path, "%s/%s", sg_csvdir, Z_OPTFILE);
    if (!on)
        unlink(path);
    else if ((fd = real_open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644)) >= 0)
        real_close(fd);
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
    snprintf(z_msg, sizeof z_msg, "%s %.0f mm", st == 3 ? "going up to" : st == 1 ? "going down to" : "going down to", (double)mm);
    move(0, 0, mm, speed);
}

/* "raise the plate" while it is still going down (option unticked / pre-heat off by mistake): stop it and go back to where it started */
static const char *z_abort_back(void)
{
    double pos;
    const char *why;
    struct timespec ts = {0, 300000000};
    z_st = 0;
    ((void (*)(int))(uintptr_t)Z_STOP)(0);
    nanosleep(&ts, NULL);                      /* the axis thread's idle line of the stopped move must not count for the next one */
    if ((why = z_why_not(0, &pos))) {
        snprintf(z_msg, sizeof z_msg, "stopped: %s", why);
        return why;
    }
    if (pos < z_origin - 0.1)
        z_go(z_origin, Z_FAST, pos, 3);
    else
        snprintf(z_msg, sizeof z_msg, "stopped at %.0f mm", pos);
    return NULL;
}

/* start the plate move for pre-heat on (down = 1) / off (0); returns NULL or why not */
static const char *z_start(int down)
{
    double pos;
    const char *why = z_why_not(down, &pos);
    if (why)
        return why;
    if (z_st) {
        if (!down && (z_st == 1 || z_st == 2))
            return z_abort_back();
        return "the plate is still moving";
    }
    if (down) {
        z_origin = (float)pos;
        if (pos > Z_SLOW_AT + 0.5)
            z_go(Z_SLOW_AT, Z_FAST, pos, 1);
        else if (pos > z_pre_mm() + 0.1 || pos < z_pre_mm() - 0.1)
            z_go(z_pre_mm(), Z_SLOW, pos, 2);
        else
            snprintf(z_msg, sizeof z_msg, "plate already at %.0f mm", (double)z_pre_mm());
    } else if (pos < Z_OFF - 0.1 && pos > -1) {
        z_go(Z_OFF, Z_FAST, pos, 3);
    } else {
        snprintf(z_msg, sizeof z_msg, "plate already at or above %.0f mm", (double)Z_OFF);
    }
    return NULL;
}

/* every 0.5 s from the writer thread: next stage when the app says the axis is idle */
static void sgheat_tick(void)
{
    double pos;
    const char *why;
    if (!z_st)
        return;
    if (lg_z_trig != z_trig0) {
        z_st = 0;
        snprintf(z_msg, sizeof z_msg, "stopped: the foreign matter guard tripped");
    } else if (HW(H_PSTATE) == 10 || s_active) {
        z_st = 0;
        snprintf(z_msg, sizeof z_msg, "stopped: a print job started");
    } else if (lg_z_idle != z_idle0) {
        if (z_st == 1) {
            if ((why = z_why_not(1, &pos))) {
                z_st = 0;
                snprintf(z_msg, sizeof z_msg, "stopped: %s", why);
            } else {
                z_go(z_pre_mm(), Z_SLOW, pos, 2);
            }
        } else {
            snprintf(z_msg, sizeof z_msg, "plate at %.0f mm", (double)z_tgt);
            z_st = 0;
        }
    } else if (now_us() > z_dead) {
        z_st = 0;
        snprintf(z_msg, sizeof z_msg, "stopped: the move did not finish in time");
    }
}

/* "plateup": before a print started from the dashboard: with pre-heat on and "Plate into the resin" on the plate rises to
 * Z_OFF (50 mm) first; pre-heat and the option stay as they are. res "ok" (also when nothing is needed) or "error: ..." */
static void sgheat_plateup_cmd(char *res, size_t len)
{
    const char *why;
    if (!h_ok) {
        snprintf(res, len, "error: plate control not available");
        return;
    }
    if (!z_opt || !HB(H_UI + 0x15) || z_st == 3)
        return;
    if ((why = z_start(0)))
        snprintf(res, len, "error: the plate could not be raised: %s", why);
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
    return snprintf(buf, len, ",\"heat\":{\"t\":%s,\"sensor\":%u,\"ready\":%u,\"state\":%u,"
                    "\"printing\":%d,\"pre\":%u,\"hp\":%u,\"tpre\":%d,\"tprint\":%d,\"max\":%d,"
                    "\"zopt\":%d,\"zmove\":%d,\"zmsg\":\"%s\"}",
                    ts, (unsigned)(HW(H_DEV) != 0), (unsigned)HB(H_DEV + 0x118),
                    (unsigned)HW(H_DEV + 0x114), HW(H_PSTATE) == 10, (unsigned)HB(H_UI + 0x15),
                    (unsigned)HB(H_UI + 0x14), (int)HI(H_MACH + 0x1a0), (int)HI(H_MACH + 0x198),
                    (int)HI(H_MACH + 0x194), z_opt, z_st, z_msg);
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
    const char *zwhy = NULL;
    double t;
    void (*set_state)(int) = (void (*)(int))(uintptr_t)H_SETSTATE;
    int (*save_mach)(void) = (int (*)(void))(uintptr_t)H_SAVEMACH;
    int (*save_ui)(void) = (int (*)(void))(uintptr_t)H_SAVEUI;
    int save_u = 0, zchg;

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
    zchg = zopt >= 0 && zopt != z_opt;
    if (zchg)
        z_set_opt(zopt);
    if (zopt == 1 && pre != cur_pre && (zwhy = z_start(pre)))
        snprintf(res, len, "error: pre-heat %s, but the plate was not moved: %s", pre ? "is on" : "is off", zwhy);
    else if (zchg && pre && cur_pre && !z_quiet && (zwhy = z_start(zopt)))   /* option changed while pre-heat stays on: tick = down, untick = up */
        snprintf(res, len, "error: plate option %s, but the plate was not moved: %s", zopt ? "on" : "off", zwhy);
    if (save_u && save_ui() != 0)
        snprintf(res, len, "error: set, but saving ui_param_ref.bin failed");
}
