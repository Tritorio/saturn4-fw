/*
 * sgdl.c - Dynamic lift logic: the release starts at an adaptive speed and goes fast once
 * the layer has separated. Pure functions, no app memory: #included by sgtap.c before sgtilt.c (which does the tilt moves
 * and the gauge access) and unit-tested on the PC (sgdltest.c).
 *
 * Separation detector (one release): force d = (raw - base) / k in grams, negative = peel. PEAK = the most negative d.
 *   - separation seen = a peak of at least DL_PEAKMIN g followed by two readings that each rise (by more than DL_RISE g) over
 *     the one before ("two readings after the peak"): the speed switch happens then.
 *   - no peel = after DL_NOPEEL_DEG of tilt the force never reached DL_PEAKMIN g: the speed switch happens at once.
 *   The whole release is watched (also after the switch): the peak of the layer is what the controller uses.
 *
 * Controller (per layer, from the peak of that layer's release; reset at every job start): start speed = base x f, clamped
 * to DL_MIN_DS .. base (base = the release stage-1 speed set in the Tilt card).
 *   mode raising: peak > LIMIT -> mode reducing, f x (1 - SLOWER); else f x (1 + FASTER) (f never above 1);
 *   mode reducing: peak < LOWER -> mode raising, f x (1 + FASTER); else f x (1 - SLOWER).
 */
#define DL_PEAKMIN     100.0   /* g: smaller = no peak, no peel */
#define DL_RISE        2.0     /* g: a reading counts as rising when it is more than this above the previous one */
#define DL_NOPEEL_DEG  20.0
#define DL_MIN_DS      10.0    /* deg/s: lowest start speed */
#define DL_NRISE       2

struct dl_cfg {
    int on, limit, lower, slower, faster;     /* switch, g, g, %, % */
    int start;                                /* first layer (1-based) where it works; <= 1 = from the first layer */
};

struct dl_det {
    double base, k;           /* raw counts at the start of the release, counts per gram */
    double peak;              /* g, most negative reading so far (<= 0) */
    double prev;              /* the previous reading (g) */
    int rising, sep, nopeel, n;
    double sep_deg;           /* tilt angle at the switch */
};

struct dl_ctl {
    double f;                 /* start speed factor, 0 < f <= 1 */
    int mode;                 /* 0 raising, 1 reducing */
};

static void dl_det_start(struct dl_det *d, double base_raw, double k)
{
    memset(d, 0, sizeof *d);
    d->base = base_raw;
    d->k = k > 0 ? k : 100.33;
}

/* one gauge reading while the tilt is at angle deg: 1 = switch to the fast speed now (once) */
static int dl_det_feed(struct dl_det *d, double raw, double deg)
{
    double g = (raw - d->base) / d->k;
    int fire = 0;
    if (d->n == 0 || g < d->peak) {
        d->peak = d->n == 0 || g < d->peak ? g : d->peak;
        d->rising = 0;
    } else if (g > d->prev + DL_RISE) {
        d->rising++;
    } else {
        d->rising = 0;
    }
    d->prev = g;
    d->n++;
    if (!d->sep && !d->nopeel && -d->peak >= DL_PEAKMIN && d->rising >= DL_NRISE) {
        d->sep = 1;
        d->sep_deg = deg;
        fire = 1;
    }
    return fire;
}

/* the angle check without a reading: 1 = no peel, switch now (once) */
static int dl_det_angle(struct dl_det *d, double deg)
{
    if (!d->sep && !d->nopeel && deg >= DL_NOPEEL_DEG && -d->peak < DL_PEAKMIN) {
        d->nopeel = 1;
        d->sep_deg = deg;
        return 1;
    }
    return 0;
}

static void dl_ctl_reset(struct dl_ctl *c)
{
    c->f = 1.0;
    c->mode = 0;
}

/* the start speed (deg/s) for a layer whose release stage-1 speed is base_ds */
static double dl_start_ds(const struct dl_ctl *c, double base_ds)
{
    double v = base_ds * c->f;
    if (v < DL_MIN_DS)
        v = DL_MIN_DS;
    if (v > base_ds)
        v = base_ds;
    return v;
}

/* after a release: peak_g = the peak of that release (g, positive number = size of the negative peak) */
static void dl_ctl_update(struct dl_ctl *c, const struct dl_cfg *cfg, double base_ds, double peak_g)
{
    double cur = dl_start_ds(c, base_ds);
    double dn = 1.0 - cfg->slower / 100.0, up = 1.0 + cfg->faster / 100.0;
    int reduce;
    c->f = base_ds > 0 ? cur / base_ds : 1.0;    /* keeps the clamps in f */
    if (c->mode == 0) {
        reduce = peak_g > cfg->limit;
        if (reduce)
            c->mode = 1;
    } else {
        reduce = peak_g >= cfg->lower;
        if (!reduce)
            c->mode = 0;
    }
    c->f *= reduce ? dn : up;
    if (c->f > 1.0)
        c->f = 1.0;
    if (c->f < 0.001)
        c->f = 0.001;
}
