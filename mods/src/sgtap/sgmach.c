/*
 * sgmach.c - live machine settings from the dashboard (step 8.24), #included by sgtap.c after sgsys.c.
 * Command "mset NAME OLD NEW" (writer thread, from sgwebd's cmd file): writes ONE machine setting into
 * the live settings struct of the running app (0x39fa30 + offset, the struct the app reads at run time),
 * only if the live value is OLD right now. A wrong app version or a value changed meanwhile therefore
 * writes nothing. Whitelist and ranges below = sgwebd.tcl (/api/machine) and the page (machine settings
 * panel). Not saved here: the saved file machine_param_ref.bin is written by sgwebd.tcl with its CRC,
 * and the app's own saver (Tilt card Apply) writes the live struct back, so live and file agree.
 * Code check (V1.5.9): the words of the X motor current ramp 0x817d8 that reads these bytes.
 * Kill switch /customer/mod/sgmach_disable. live.json "mach":{"ok":0|1,"v":{name:value,...}}.
 * Offsets verified by a live dump against machine_param_ref.bin (hold 12, run 29, step 1,
 * interval 1000 ms at +0x101..; level 4000 / foreign 5000 at +0x224 / +0x230; I212 at +0x26c..).
 */
#define MS_BASE   0x39fa30u
#define MS_RAMP   0x817d8u          /* push {r4, r5, r6, lr}; movw r4, #0xdd00; movt r4, #0x2f ... */

static volatile int ms_ok;           /* 1 = code words found, -1 = not found / switched off */

static const struct {
    const char *name;
    unsigned off;
    int size;                        /* 1 = u8, 4 = i32 */
    int lo, hi;
} ms_keys[] = {
    {"i_hold",                           0x101, 1, 0, 31},       /* I6 A  X motor hold current */
    {"i_run",                            0x102, 1, 0, 31},       /* I6 B  X motor run current */
    {"i_change_interval",                0x103, 1, 1, 31},       /* I6 P  current step */
    {"i_change_time",                    0x104, 4, 100, 10000},  /* I6 R  ms per step */
    {"z_i_hold",                         0x248, 1, 0, 31},       /* I6 F  Z motor hold current */
    {"z_i_run",                          0x249, 1, 0, 31},       /* I6 S  Z motor run current */
    {"sg_leveing_ths",                   0x224, 4, 0, 20000},    /* I205 E  levelling force */
    {"sg_foregin_triger_ths",            0x230, 4, 0, 20000},    /* I206 C  foreign matter limit */
    {"before_n_layer_x_time_slow_mode",  0x26c, 4, 0, 60000},    /* I212 A */
    {"after_n_layer_x_time_slow_mode",   0x270, 4, 0, 60000},    /* I212 B */
    {"before_n_layer_x_time_fast_mode",  0x274, 4, 0, 60000},    /* I212 C */
    {"after_n_layer_x_time_fast_mode",   0x278, 4, 0, 60000},    /* I212 D */
};
#define MS_N (int)(sizeof ms_keys / sizeof ms_keys[0])

static int ms_check(void)
{
    if (ms_ok == 0) {
        ms_ok = in_chitu && access("/customer/mod/sgmach_disable", F_OK) != 0 &&
                HW(MS_RAMP) == 0xe92d4070u && HW(MS_RAMP + 4) == 0xe30d4d00u &&
                HW(MS_RAMP + 8) == 0xe340402fu && HW(MS_RAMP + 12) == 0xe30f5a30u ? 1 : -1;
    }
    return ms_ok > 0;
}

static int ms_get(int i)
{
    uintptr_t a = (uintptr_t)(MS_BASE + ms_keys[i].off);
    return ms_keys[i].size == 1 ? (int)*(volatile uint8_t *)a : (int)*(volatile int32_t *)a;
}

static void ms_put(int i, int v)
{
    uintptr_t a = (uintptr_t)(MS_BASE + ms_keys[i].off);
    if (ms_keys[i].size == 1)
        *(volatile uint8_t *)a = (uint8_t)v;
    else
        *(volatile int32_t *)a = v;
}

/* "NAME OLD NEW" */
static void sgmach_cmd(const char *c, char *res, size_t len)
{
    char name[48];
    double v[2];
    size_t n = 0;
    int i;
    while (c[n] && c[n] != ' ' && n < sizeof name - 1) {
        name[n] = c[n];
        n++;
    }
    name[n] = 0;
    for (i = 0; i < MS_N && strcmp(name, ms_keys[i].name) != 0; i++)
        ;
    if (c[n] != ' ' || i == MS_N || parse_nums(c + n, v, 2) != 2 || v[0] != (int)v[0] || v[1] != (int)v[1]) {
        snprintf(res, len, "error: bad mset command");
        return;
    }
    if (v[1] < ms_keys[i].lo || v[1] > ms_keys[i].hi) {
        snprintf(res, len, "error: %s outside %d..%d", name, ms_keys[i].lo, ms_keys[i].hi);
        return;
    }
    if (!ms_check()) {
        snprintf(res, len, "error: live settings not available (other app version or switched off)");
        return;
    }
    if (ms_get(i) != (int)v[0]) {
        snprintf(res, len, "error: %s is %d now, not %d (nothing written)", name, ms_get(i), (int)v[0]);
        return;
    }
    ms_put(i, (int)v[1]);
}

static int sgmach_json(char *buf, size_t len)
{
    int n = snprintf(buf, len, ",\"mach\":{\"ok\":%d", ms_check() ? 1 : 0);
    if (ms_ok > 0) {
        n += snprintf(buf + n, len - (size_t)n, ",\"v\":{");
        for (int i = 0; i < MS_N && n < (int)len; i++)
            n += snprintf(buf + n, len - (size_t)n, "%s\"%s\":%d", i ? "," : "", ms_keys[i].name, ms_get(i));
        if (n < (int)len)
            n += snprintf(buf + n, len - (size_t)n, "}");
    }
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "}");
    return n;
}

/* ---- Flex Plate thickness compensation ----
 * "flex on" writes the factory Z values + 3.0 mm, "flex off" the factory values, into the live machine struct (floats; the vat heating
 * positions are doubles), then the app's own saver writes machine_param_ref.bin. The Z axis struct keeps its own copy of the zero
 * position (mm at +0x38, steps at +0x40 = (int)(mm / mm_per_step), the soft lower limit, filled by the axis init 0x5336c): written too,
 * the same way. Only when the printer is idle and the Z plate is not moving. State = what the live struct holds: 0 = all factory,
 * 1 = all +3.0, 2 = anything else (custom). Live offsets: dump of against the saved file (zero float +0x40; I4 X / Y floats
 * +0x1c4 / +0x1c8; I204 A / B floats +0x1f4 / +0x1f8, R +0x210; I205 A / B floats +0x214 / +0x218; I215 E / Z doubles +0x2a0 / +0x2a8).
 * Factory = the default table of the app (identical in V1.5.9 and the repair firmware); I4 X / Y have no factory value there: the
 * printer's 32 / 8 are taken as factory. The volume compensation R is SUBTRACTED from the app's volume estimate (func 0x78e80:
 * volume = w * pos * h / 1000, ok when volume - R >= file volume), so +3.0 mm = +3 * 143 * 244 / 1000 = +104.7 ml. */
#define FX_N 10
static const struct {
    const char *name;
    unsigned off;
    int dbl;                         /* 1 = double, else float */
    double std, flex, tol;
} fx_keys[FX_N] = {
    {"zero",     0x40,  0, 0.0,   3.0,   0.02},
    {"lev_a",    0x214, 0, 3.0,   6.0,   0.02},
    {"lev_b",    0x218, 0, -3.0,  0.0,   0.02},
    {"det_a",    0x1f4, 0, 45.0,  48.0,  0.02},
    {"det_b",    0x1f8, 0, 3.0,   6.0,   0.02},
    {"line_max", 0x1c4, 0, 32.0,  35.0,  0.02},
    {"line_min", 0x1c8, 0, 8.0,   11.0,  0.02},
    {"heat_e",   0x2a0, 1, 18.0,  21.0,  0.02},
    {"heat_z",   0x2a8, 1, 5.0,   8.0,   0.02},
    {"reserved", 0x210, 0, 100.0, 204.7, 0.1},
};

static int fx_ok(void)
{
    float w, h;
    if (!ms_check() || !h_ok)
        return 0;
    w = HF(MS_BASE + 0x208);
    h = HF(MS_BASE + 0x20c);
    return w > 50.0f && w < 500.0f && h > 50.0f && h < 500.0f;     /* the probe's vat width / height: the struct is where we think */
}

static double fx_get(int i)
{
    uintptr_t a = (uintptr_t)(MS_BASE + fx_keys[i].off);
    return fx_keys[i].dbl ? *(volatile double *)a : (double)*(volatile float *)a;
}

static void fx_put(int i, double v)
{
    uintptr_t a = (uintptr_t)(MS_BASE + fx_keys[i].off);
    if (fx_keys[i].dbl)
        *(volatile double *)a = v;
    else
        *(volatile float *)a = (float)v;
}

static int fx_state(void)
{
    int is_std = 1, is_flex = 1;
    if (!fx_ok())
        return -1;
    for (int i = 1; i < FX_N; i++) {          /* not i = 0 (zero): the printer rewrites the Z zero after each levelling (measured floor contact, e.g. 3.3 for 3.0) */
        double v = fx_get(i), t = fx_keys[i].tol;
        if (v < fx_keys[i].std - t || v > fx_keys[i].std + t)
            is_std = 0;
        if (v < fx_keys[i].flex - t || v > fx_keys[i].flex + t)
            is_flex = 0;
    }
    return is_std ? 0 : is_flex ? 1 : 2;
}

static float z_pre_mm(void)          /* pre-heat plate position (sgheat.c): lower with the factory Z values */
{
    return fx_state() == 0 ? Z_PRE_STD : Z_PRE_FLEX;
}

/* "on" | "off" */
static void sgflex_cmd(const char *c, char *res, size_t len)
{
    int on = strncmp(c, "on", 2) == 0, off = strncmp(c, "off", 3) == 0;
    double d;
    uint32_t p;
    if (!on && !off) {
        snprintf(res, len, "error: bad flex command");
        return;
    }
    if (!fx_ok()) {
        snprintf(res, len, "error: flex plate values not available (other app version or switched off)");
        return;
    }
    if (HW(H_PSTATE) == 10 || s_active) {
        snprintf(res, len, "error: not while a print job is running");
        return;
    }
    if (z_st) {
        snprintf(res, len, "error: the plate is moving");
        return;
    }
    if (!(p = z_axis(&d))) {
        snprintf(res, len, "error: Z axis not found");
        return;
    }
    for (int i = 0; i < FX_N; i++)
        fx_put(i, on ? fx_keys[i].flex : fx_keys[i].std);
    {
        float zf = (float)(on ? fx_keys[0].flex : fx_keys[0].std);
        *(volatile float *)(uintptr_t)(p + 0x38) = zf;                                    /* axis: zero position mm */
        *(volatile int32_t *)(uintptr_t)(p + 0x40) = (int32_t)((double)zf / d);          /* axis: soft lower limit, steps (as the axis init) */
    }
    if (((int (*)(void))(uintptr_t)H_SAVEMACH)() != 0)
        snprintf(res, len, "error: set, but saving machine_param_ref.bin failed");
}

static int sgflex_json(char *buf, size_t len)
{
    double d;
    uint32_t p;
    int n, st = fx_state();
    if (st < 0)
        return snprintf(buf, len, ",\"flex\":null");
    n = snprintf(buf, len, ",\"flex\":{\"st\":%d,\"v\":[", st);
    for (int i = 0; i < FX_N && n < (int)len; i++)
        n += snprintf(buf + n, len - (size_t)n, "%s%.3f", i ? "," : "", fx_get(i));
    p = z_axis(&d);
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "],\"axz\":%.4f,\"axs\":%d}", p ? (double)*(volatile float *)(uintptr_t)(p + 0x38) : -1.0,
                      p ? (int)*(volatile int32_t *)(uintptr_t)(p + 0x40) : -1);
    return n;
}
