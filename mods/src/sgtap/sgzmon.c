/*
 * sgzmon.c - Z monitor: the Z position at the end of the retract, per layer.
 * #included by sgtap.c after sgexpo.c; run from sgwait.c's 20 Hz wait thread (sgzmon_step / sgzmon_idle).
 *
 * Sample moment = the first tick of each exposure (P+0x30c == 1, new P+0x338 = exposure start). The retract is finished by then and the plate
 * does not move during the exposure, so it is the Z after the retract. NOT the start of the wait: a file with a wait of a few ms (the
 * dry-run / 50 um test files) is missed by the 20 Hz thread (seen live: zm.n stayed 0 over 5 layers).
 * Position = the app's own step counter of the Z axis (axis struct *(0x2fa124): +0x50 steps x +0x18 mm/step, read as in sgheat.c z_axis):
 * what the printer believes, not a measurement. Step = Z minus the Z of the layer before (um) when that layer was the one before.
 * The expected step of a layer comes from the app's own per-layer block of the print struct: P+0x220 (float mm) = the target Z of the current layer
 * (checked live: 6.2 / 6.1 against the measured 6.199 / 6.099 mm). Expected step = target Z minus the target Z of the layer before; first
 * layer = target Z minus the Z zero. (P+0x228 = 0.05 is only the file's general layer height: the Z targets of that file have a 0.1 mm step between
 * layers 1 and 2, so it is only the fallback.) The job maximum of |step - expected step| (um) is kept; "lh" in live.json = the expected step.
 * The first layer of a job has no layer before: its "step" is the height above the Z zero of the machine settings (floor contact), flagged fl = 1 as a 4th element
 * (compared with its own layer height too, not written to the CSV).
 * What is shown / logged as "Z" is the PRINT HEIGHT = plate position minus that zero (about 0.1 mm after the first layer of a 0.1 mm file), not the machine
 * coordinate (the zero is e.g. 5.85 mm, so the machine value is higher than the height of the print); "now" (idle) stays the machine position.
 * Data: live.json "zm" {now, n, max, h:[[layer, z_mm, step_um|null, fl, layer_height_um|0] x14]}, CSV columns z_ret_mm,z_step_um on the first row after
 * each retract. The record ends with the job (3 s without a print struct). Kill switch /customer/mod/sgzmon_disable.
 */
#define ZM_HIST 14

static int zm_ok;
static pthread_mutex_t zm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int layer; double z, step, lh; int has, fl; } zm_h[ZM_HIST];   /* lh = the file's layer height in um (0 = unknown) */   /* fl = the step is the height above the Z zero (first layer of a job) */
static int zm_n;                         /* entries in zm_h */
static int zm_prev_layer = -1;
static double zm_prev_z, zm_prev_zt;     /* measured Z and target Z (P+0x220) of the layer before */
static uint64_t zm_t0;
static double zm_max;                    /* job maximum of |step - median|, um */
static int zm_idle_n;
static struct { int pending; double z, step; int has; } zm_csvs;

static double zm_now_mm(int *ok)
{
    double d;
    uint32_t p;
    *ok = 0;
    if (!in_chitu || !(p = z_axis(&d)))
        return 0;
    if (HB(p + 0x49) != 3 || !HB(p + 0x4a))
        return 0;
    double mm = HI(p + 0x50) * d;
    if (mm < -1 || mm > HI(p + 0x3c) * d + 1)
        return 0;
    *ok = 1;
    return mm;
}

/* the Z zero of the MACHINE SETTINGS (I0 A0 Y, float at 0x39fa30 + 0x40, as sgmach.c): the app builds the Z targets of a print from it (file heights on top,
 * checked live: zero 5.85, targets 5.95 / 6.05 / 6.10 for a file with 0.1 / 0.1 / 0.05 mm layers) and the levelling rewrites it. NOT the Z axis
 * struct's own +0x38 copy: that one is only refreshed at the next start (5.80 while the setting was 5.85), it gave a first layer of 150 um instead of 100. */
static int zm_zero_mm(double *zero)
{
    if (!in_chitu)
        return 0;
    *zero = HF(0x39fa30u + 0x40);
    return *zero > -1 && *zero < 30;
}

/* <csvdir>/LEVELS.csv: one line per job, written at its first layer = the result of the levelling of that job (is Z consistent among print
 * jobs? could the levelling be skipped / a custom start Z be used?). Never deleted by the job history, never needs the job log. Columns: time_utc, print_file,
 * zero_machine (I0 A0 Y after the levelling), zero_axis (the Z axis' own copy, only refreshed at a restart), z_first (plate position at layer 1, machine coordinate),
 * z_target (the app's target Z of layer 1 = zero_machine + the file's first layer height). */
static void zm_levellog(double zero, int zok, double z, double zt, int ztok)
{
    char path[192], line[400], ts[32], file[200];
    double d, za = -1;
    uint32_t p;
    struct timespec rt;
    struct tm tm;
    int fd, fresh;
    if (!in_chitu || !sg_csvdir[0])
        return;
    if ((p = z_axis(&d)))
        za = (double)HF(p + 0x38);
    clock_gettime(CLOCK_REALTIME, &rt);
    gmtime_r(&rt.tv_sec, &tm);
    if (tm.tm_year + 1900 >= 2020)
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    else
        snprintf(ts, sizeof ts, "clock_unset");
    pthread_mutex_lock(&glock);
    snprintf(file, sizeof file, "%s", print_file);
    pthread_mutex_unlock(&glock);
    for (char *c = file; *c; c++)
        if (*c == ',' || *c == '\n' || *c == '\r')
            *c = '_';
    snprintf(path, sizeof path, "%s/LEVELS.csv", sg_csvdir);
    fresh = access(path, F_OK) != 0;
    fd = real_open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (fresh)
        real_write(fd, "time_utc,print_file,zero_machine,zero_axis,z_first,z_target\n", 61);
    int n = snprintf(line, sizeof line, "%s,%s,%s%.3f,%.3f,%.3f,%s%.3f\n", ts, file, zok ? "" : "?", zok ? zero : 0.0, za, z, ztok ? "" : "?", ztok ? zt : 0.0);
    real_write(fd, line, (size_t)n);
    real_close(fd);
}

static void zm_reset_job(void)            /* caller holds zm_lock */
{
    zm_n = 0;
    zm_prev_layer = -1;
    zm_max = 0;
}

/* every 50 ms from the wait thread while P is valid */
static void sgzmon_step(uintptr_t P, uint64_t now_ms)
{
    int flag, layer, ok;
    uint64_t t0;
    (void)now_ms;
    if (!zm_ok)
        return;
    zm_idle_n = 0;
    flag = HB(P + E_FLAG);
    t0 = read_u64(P + E_START);
    layer = HI(P + W_LAYER);
    if (flag != 1 || t0 == zm_t0)
        return;
    zm_t0 = t0;
    double z = zm_now_mm(&ok);
    if (!ok)
        return;
    double zt = HF(P + 0x220), lh = 0;                         /* this layer's target Z (mm) from the app's layer block */
    int ztok = zt > 0 && zt - z < 0.5 && z - zt < 0.5;
    float lhf = HF(P + 0x228);                                /* fallback: the file's general layer height (mm) */
    if (lhf > 0.004f && lhf < 1.0f)
        lh = lhf * 1000.0;
    pthread_mutex_lock(&zm_lock);
    if (zm_prev_layer >= 0 && layer < zm_prev_layer - 1)     /* a new job without P going to 0 */
        zm_reset_job();
    int has = zm_prev_layer >= 0 && layer == zm_prev_layer + 1;
    int first = zm_prev_layer < 0;                           /* the first sample of a job (not after a missed layer) */
    double step = has ? (z - zm_prev_z) * 1000.0 : 0, zero = 0;
    int fl = 0, zok = zm_zero_mm(&zero);
    if (!has && zok && z >= zero - 0.01 && z - zero < 1.0) {   /* the first layer: its height above the floor contact */
        step = (z - zero) * 1000.0;
        fl = 1;
        if (ztok && zt >= zero - 0.01 && zt - zero < 1.0)
            lh = (zt - zero) * 1000.0;
    } else if (has && ztok && zm_prev_zt > 0 && zt - zm_prev_zt > 0.004 && zt - zm_prev_zt < 1.0) {
        lh = (zt - zm_prev_zt) * 1000.0;
    }
    lh = (double)(long)(lh + 0.5);                            /* whole um: float noise of the targets */
    if (zm_n && zm_h[zm_n - 1].layer == layer + 1)               /* the same layer again: replace */
        zm_n--;
    if (zm_n == ZM_HIST) {
        memmove(&zm_h[0], &zm_h[1], sizeof zm_h[0] * (ZM_HIST - 1));
        zm_n--;
    }
    zm_h[zm_n].layer = layer + 1;                            /* shown 1-based like the Dynamic lift list */
    zm_h[zm_n].z = zok ? z - zero : z;                       /* shown = the PRINT HEIGHT: the plate position minus the Z zero of the machine settings */
    zm_h[zm_n].step = step;
    zm_h[zm_n].lh = lh;
    zm_h[zm_n].has = has || fl;
    zm_h[zm_n].fl = fl;
    zm_n++;
    if ((has || fl) && lh > 0) {
        double e = step - lh;
        if (e < 0)
            e = -e;
        if (e > zm_max)
            zm_max = e;
    }
    zm_prev_layer = layer;
    zm_prev_z = z;
    zm_prev_zt = ztok ? zt : 0;
    zm_csvs.pending = 1;
    zm_csvs.z = zok ? z - zero : z;
    zm_csvs.step = step;
    zm_csvs.has = has;                                       /* the CSV step is only ever a step from the layer before */
    pthread_mutex_unlock(&zm_lock);
    if (first)                                               /* the first layer of a job: its levelling result */
        zm_levellog(zero, zok, z, zt, ztok);
}

/* every 50 ms from the wait thread while P is 0 */
static void sgzmon_idle(uint64_t now_ms)
{
    (void)now_ms;
    if (!zm_ok)
        return;
    if (++zm_idle_n == 60) {                                  /* 3 s without a print job: the record ends with the job */
        pthread_mutex_lock(&zm_lock);
        zm_reset_job();
        pthread_mutex_unlock(&zm_lock);
        zm_t0 = 0;
    }
}

static void sgzmon_init(void)
{
    if (!w_ok || access("/customer/mod/sgzmon_disable", F_OK) == 0)
        return;
    zm_ok = 1;
}

/* ",z_ret_mm,z_step_um" for the CSV row: on the first row after the start of each exposure */
static void sgzmon_csv(char *buf, size_t len)
{
    pthread_mutex_lock(&zm_lock);
    if (zm_csvs.pending) {
        if (zm_csvs.has)
            snprintf(buf, len, ",%.3f,%.1f", zm_csvs.z, zm_csvs.step);
        else
            snprintf(buf, len, ",%.3f,", zm_csvs.z);
    } else {
        snprintf(buf, len, ",,");
    }
    zm_csvs.pending = 0;
    pthread_mutex_unlock(&zm_lock);
}

static int sgzmon_json(char *buf, size_t len)
{
    int n, ok;
    double now;
    if (!zm_ok)
        return snprintf(buf, len, ",\"zm\":null");
    now = zm_now_mm(&ok);
    double zero = 0;
    int zk = zm_zero_mm(&zero);
    char zs[24];
    if (zk)
        snprintf(zs, sizeof zs, "%.3f", zero);
    else
        snprintf(zs, sizeof zs, "null");
    pthread_mutex_lock(&zm_lock);
    if (ok)
        n = snprintf(buf, len, ",\"zm\":{\"now\":%.3f,\"zero\":%s,\"n\":%d,\"max\":%.1f,\"h\":[", now, zs, zm_n, zm_max);
    else                                                       /* Z not homed yet (just after the boot): null, never an invalid number */
        n = snprintf(buf, len, ",\"zm\":{\"now\":null,\"zero\":%s,\"n\":%d,\"max\":%.1f,\"h\":[", zs, zm_n, zm_max);
    for (int i = 0; i < zm_n && n < (int)len - 60; i++) {
        n += snprintf(buf + n, len - (size_t)n, "%s[%d,%.3f,", i ? "," : "", zm_h[i].layer, zm_h[i].z);
        n += zm_h[i].has ? snprintf(buf + n, len - (size_t)n, "%.1f,%d,%.0f]", zm_h[i].step, zm_h[i].fl, zm_h[i].lh) : snprintf(buf + n, len - (size_t)n, "null,0,%.0f]", zm_h[i].lh);
    }
    pthread_mutex_unlock(&zm_lock);
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "]}");
    return n;
}
