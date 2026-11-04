/*
 * sgmx.c - MULTI EXPOSURE for real prints: each of the four screen quadrants gets its own UV time on the NORMAL
 * layers of a normally sliced file. #included by sgtap.c after sgdisp.c, AA variant only (it needs the board link code: spi_fd, dsp_*).
 * Method: the layer is exposed in stages. Stage 1 is the app's own exposure (its image, its UV); when the shortest time is over, the UV goes
 * off, the image is replaced by the same stream with the half rows of the finished quadrants blacked out (sgmxmask.h; 28 2a 2c + stream + 4c x8
 * + 29, about 0.6 s dark), the UV comes back on at the app's own level, and so on until the longest time is over. The app's end of exposure
 * (P+0x24c, ms since P+0x338) is preset in the wait to the sum of the stages plus a generous swap allowance (so the app never ends a stage
 * sequence), and rewritten to the exact end when the last stage starts; the last stage's UV is switched off by this module at its deadline.
 * Facts: the image of layer N+1 is sent right after exposure N ends; during an
 * exposure the app writes only fan frames; real streams are 12460 half rows of exactly 7560 px, runs <= 72. The app ends the exposure when
 * now_ms - P+0x338 > P+0x24c (0x7d17c..0x7d1a4), then clears P+0x30c, UV off (0x81be4(1,0)), 28.
 * Pieces:
 *   sgmx_capture: called from the write wrapper for every write to the board link while armed; collects each image burst (0x3c packets of
 *       >= 0x400 bytes) into one of two 4 MB buffers (allocated by the writer tick when the switch goes ON, freed when it is OFF). Its own writes
 *       (this module's thread) are skipped by thread id. A burst is "ready" when a non-image frame follows (or 100 ms of silence).
 *   sgmx_latch: from sgexpo.c e_latch, at the start of a NORMAL layer's wait (the block is loaded, the image has been sent): parse the newest
 *       burst (every image is used once), find the quadrants with content, build the stage plan from the times, preset P+0x24c.
 *   mx_thread: 10 ms poll of P+0x30c; at the start of an exposure of a planned layer it runs the stages (UV off / swap / UV on at absolute
 *       deadlines on CLOCK_MONOTONIC_RAW, measured UV-on time per stage).
 * Safety rules: the dose can come out short, never long. Any error (write failure, flag dropped, switch off, image changed, UV duty differs after
 * a restore) = UV off, remaining stages dropped, P+0x24c = 0 so the app ends the exposure now. A quadrant with no content on a layer gets no stage.
 * Bottom and transition layers, and every layer whose image was not captured cleanly, run with the file's time. Exclusive with Manual Exposure
 * (sgexpo.c) in both directions. The switch (command "mx on") ends with the job; the four times and the two mirror flags persist in
 * <csvdir>/MULTIEXPO. Commands: "mx on", "mx off", "mx set T1 T2 T3 T4 MIRX MIRY" (T in s, box order TL TR BL BR as the dashboard card shows them;
 * MIRX / MIRY 1 = the stream's quadrant numbering is mirrored left-right / top-bottom against the slicer view). Kill switch:
 * /customer/mod/sgmx_disable. live.json "mx":{...}, absent in the normal variant.
 */
#include "sgmxmask.h"

#define MX_BUF       (4u << 20)
#define MX_SWAP_ALLOW_MS 3000.0       /* P+0x24c preset: allowance per swap (a swap takes ~0.6 s) */
#define MX_MARGIN_MS     60.0         /* P+0x24c at the last stage = our deadline + this; the UV is off by us at the deadline */
#define MX_LAST       8
#define MX_UVON       0x81be4u        /* the app's UV function (mode, pwm): (2, byte of float P+0x248) = exposure level, (1, 0) = off */
#define MX_UVON_CODE  0xe3500001u     /* cmp r0, #1 */
#define MX_FILE       "MULTIEXPO"

static pthread_mutex_t mx_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int mx_cap;          /* capture armed, buffers allocated */
static volatile pid_t mx_tid;        /* the engine thread: its own writes are not captured */
static volatile int mx_on;           /* the switch */
static volatile int mx_avail;        /* code verified, not disabled */
static uint8_t *mx_buf[2];
static uint8_t *mx_out;
static uint32_t *mx_off;
static int mx_inburst, mx_idx, mx_over;
static uint32_t mx_rl;
static uint64_t mx_last_us;
static int mx_ready = -1, mx_busy = -1;
static uint32_t mx_ready_len;
static unsigned mx_ready_seq, mx_overs;
static double mx_t[4] = {5, 10, 15, 20};
static volatile int mx_from = 1;      /* first layer (1-based, as in the slicer) that gets the quadrant times; earlier normal layers keep the file time. File MXFROM, "mx from N" */
static int mx_mirx, mx_miry;
static volatile int mx_uv_on;        /* we switched the UV on and have not switched it off */
static volatile int mx_st;           /* 0 idle, 1 planned (waiting for the exposure), 2 running */
static int mx_k, mx_n;
static char mx_err[96];

static struct {                      /* the plan of the current layer (latch: 20 Hz thread; engine: mx_thread; handed over by mx_lock) */
    int valid, prep, layer, n, nfull;
    float orig, preset;
    unsigned seq, used_seq, content;
    double dt[4];
    unsigned lit[4];
    uint8_t pwm;
    uint32_t len;
    int buf;
} mxp;

static struct {                      /* last layers for the card */
    int n;
    struct { int layer, nst, skipped; float got[4]; float total; } r[MX_LAST];
} mxl;

static uint64_t mx_raw_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void mx_seterr(const char *m)
{
    pthread_mutex_lock(&mx_lock);
    snprintf(mx_err, sizeof mx_err, "%s", m);
    pthread_mutex_unlock(&mx_lock);
}

/* ---- capture (the app's send thread, every write to the board link while armed) ---- */
static void mx_finish_burst(void)                     /* mx_lock held */
{
    mx_inburst = 0;
    if (mx_over) {
        mx_overs++;
    } else if (mx_rl >= 200000u) {
        mx_ready = mx_idx;
        mx_ready_len = mx_rl;
        mx_ready_seq++;
    }
}

static void sgmx_capture(const void *buf, size_t n)
{
    const uint8_t *b = buf;
    if (!mx_cap || n == 0 || (pid_t)syscall(SYS_gettid) == mx_tid)
        return;
    pthread_mutex_lock(&mx_lock);
    if (mx_cap && n >= SPI_BURST_MIN && b[0] == SPI_IMG_CMD) {
        if (!mx_inburst) {
            mx_inburst = 1;
            mx_idx = mx_ready == 0 ? 1 : 0;
            mx_rl = 0;
            mx_over = mx_idx == mx_busy;              /* the engine is reading it: this image is dropped */
        }
        if (!mx_over) {
            if (mx_rl + n - 1 <= MX_BUF) {
                memcpy(mx_buf[mx_idx] + mx_rl, b + 1, n - 1);
                mx_rl += (uint32_t)n - 1;
            } else {
                mx_over = 1;
            }
        }
        mx_last_us = now_us();
    } else if (mx_inburst) {
        mx_finish_burst();
    }
    pthread_mutex_unlock(&mx_lock);
}

/* ---- the plan ---- */
static unsigned mx_stream_quad(int box)               /* card box 0 TL 1 TR 2 BL 3 BR -> stream quadrant */
{
    unsigned qx = ((unsigned)box & 1u) ^ (unsigned)mx_mirx, qy = ((unsigned)box >> 1) ^ (unsigned)mx_miry;
    return qy * 2u + qx;
}

static int mx_uvcode_ok(void)
{
    return HW(MX_UVON) == MX_UVON_CODE && HW(DSP_PWM) == DSP_PWM_CODE;
}

static void mx_uv_off(void)
{
    dsp_uvpwm(0);                    /* the app's own 0x883a8(0, 0) = what its 0x81be4(1, 0) ends in */
    mx_uv_on = 0;
}

static int sgmx_active(void)         /* sgexpo.c: Manual Exposure is refused while this is on */
{
    return mx_on;
}

static uint64_t mx_fresh_start(uintptr_t P)           /* P+0x338 of the exposure that has just started, ms; 0 = not fresh */
{
    for (int i = 0; i < 12; i++) {
        uint64_t t = read_u64(P + E_START), now = mono_raw_ms();
        if (t && t <= now && now - t < 1500)
            return t;
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* Pre-parse of the newest captured layer image (mx_thread, between exposures): its stream is checked and the quadrants with content found, so
 * that the plan at the start of the exposure costs nothing. Every image is used once. 1 = a prepared image is waiting. */
static int mx_prep(void)
{
    int idx;
    pthread_mutex_lock(&mx_lock);
    if (mxp.prep && mxp.seq == mx_ready_seq) {
        pthread_mutex_unlock(&mx_lock);
        return 1;
    }
    mxp.prep = 0;
    if (!mx_cap || mx_inburst || mx_ready < 0 || mx_ready_seq == mxp.used_seq) {
        pthread_mutex_unlock(&mx_lock);
        return 0;
    }
    idx = mx_ready;
    mx_busy = idx;
    mxp.seq = mx_ready_seq;
    mxp.len = mx_ready_len;
    mxp.buf = idx;
    pthread_mutex_unlock(&mx_lock);
    if (mxm_parse(mx_buf[idx], mxp.len, mx_off) != 0) {
        mx_busy = -1;
        mxp.used_seq = mxp.seq;
        mx_seterr("layer image is not a valid stream (file time)");
        return 0;
    }
    mxp.content = mxm_content(mx_buf[idx], mx_off);
    mxp.prep = 1;
    return 1;
}

/* The exposure of a layer has just started (P+0x30c = 1, the app's own image and UV are on): plan this NORMAL layer from the four times and
 * take over the end of the exposure (P+0x24c) within the first few ms, long before the app's own end (the file's time). 1 = planned. */
static int mx_plan_layer(uintptr_t P)
{
    double tq[4], dtf[4];
    unsigned litf[4];
    int layer = HI(P + W_LAYER), bot = HI(P + E_BOT), trans = (int)(HW(P + E_TRANS) & 0xffff);
    float orig = HF(P + E_EXPO);
    uint64_t t0, now = mono_raw_ms();
    mxp.valid = 0;
    if (bot < 0 || bot > 1000 || trans < 0 || trans > 1000 || layer < bot + trans)
        return 0;                                            /* bottom / transition layer: the file's time, no message */
    if (layer + 1 < mx_from)
        return 0;                                            /* before the start layer: the file's time, no message */
    if (HW(E_MODE) != 0xffffffffu || !mx_uvcode_ok()) {
        mx_seterr("app in a different exposure mode (file time)");
        return 0;
    }
    if (!(orig >= 0 && orig < 3.6e6f)) {
        mx_seterr("exposure time not readable (file time)");
        return 0;
    }
    if (!mx_prep()) {
        if (!mx_err[0])
            mx_seterr("no new layer image captured (file time)");
        return 0;
    }
    for (int i = 0; i < 4; i++)
        tq[mx_stream_quad(i)] = mx_t[i];
    mxp.n = mxm_plan(tq, mxp.content, mxp.dt, mxp.lit);
    mxp.nfull = mxm_plan(tq, 15, dtf, litf);
    mxp.used_seq = mxp.seq;
    mxp.prep = 0;
    if (mxp.n == 0) {
        mx_busy = -1;
        mx_seterr("layer is empty (file time)");
        return 0;
    }
    t0 = mx_fresh_start(P);                                  /* P+0x338 of THIS exposure (the flag can be set a few ms before it) */
    now = mono_raw_ms();
    if (!t0 || now - t0 > (uint64_t)(orig * 0.5f < 400.0f ? orig * 0.5f : 400.0f)) {
        mx_busy = -1;
        mx_seterr("too late to plan this layer (file time)");
        return 0;
    }
    mxp.orig = orig;
    mxp.layer = layer;
    {                                                        /* P+0x248 is a float; the app passes the low byte of vcvt.u32.f32 (0x7d700..0x7d714) */
        float pf = HF(P + 0x248);
        mxp.pwm = (uint8_t)(pf > 0 ? (pf >= 4294967040.0f ? 0xffu : (uint32_t)pf & 0xffu) : 0u);
    }
    {
        double ms = 0;
        for (int k = 0; k < mxp.n; k++)
            ms += mxp.dt[k] * 1000.0;
        ms += (mxp.n - 1) * MX_SWAP_ALLOW_MS + (mxp.n > 1 ? 1000.0 : 0);
        mxp.preset = (float)ms;
    }
    HF(P + E_EXPO) = mxp.preset;
    __sync_synchronize();
    mxp.valid = 1;
    mx_st = 1;
    mx_seterr("");
    return 1;
}

/* ---- the exposure ---- */
static void mx_sleep_until(uint64_t dead_us, uintptr_t P)
{
    for (;;) {
        uint64_t now = mx_raw_us();
        struct timespec ts = {0, 0};
        uint64_t rem;
        if (now >= dead_us || HB(P + E_FLAG) != 1 || !mx_on)
            return;
        rem = dead_us - now;
        if (rem > 5000)
            rem = 5000;
        ts.tv_nsec = (long)(rem * 1000u);
        nanosleep(&ts, NULL);
    }
}

/* 28 2a 2c + stream in 0x3c packets of 2048 bytes + 8 x 4c + 29 under the app's transport lock */
static int mx_swap(const uint8_t *s, size_t n)
{
    static uint8_t pk[2049], z[2048];
    int err = 0;
    dsp_lock(1);
    err |= dsp_cmd4(0x28) | dsp_cmd4(0x2a) | dsp_cmd4(0x2c);
    pk[0] = 0x3c;
    for (size_t off = 0; off < n && !err; off += 2048) {
        size_t m = n - off < 2048 ? n - off : 2048;
        memcpy(pk + 1, s + off, m);
        err |= dsp_frame(pk, m + 1);
    }
    memset(z, 0, sizeof z);
    z[0] = 0x4c;
    for (int i = 0; i < 8 && !err; i++)
        err |= dsp_frame(z, sizeof z);
    if (!err)
        err |= dsp_cmd4(0x29);
    dsp_lock(0);
    return err;
}

static void mx_record(int layer, int nst, const double got[4], uint64_t ref_us, int ok)
{
    unsigned q;
    pthread_mutex_lock(&mx_lock);
    if (mxl.n == MX_LAST) {
        memmove(mxl.r, mxl.r + 1, sizeof mxl.r[0] * (MX_LAST - 1));
        mxl.n--;
    }
    mxl.r[mxl.n].layer = layer;
    mxl.r[mxl.n].nst = ok ? nst : 0;
    mxl.r[mxl.n].skipped = ok ? mxp.nfull - mxp.n : 0;
    for (int i = 0; i < 4; i++) {
        q = mx_stream_quad(i);
        mxl.r[mxl.n].got[i] = !ok ? -2.0f : (mxp.content >> q & 1) ? (float)got[q] : -1.0f;   /* -1 = empty quadrant, -2 = layer not done */
    }
    mxl.r[mxl.n].total = (float)((mx_raw_us() - ref_us) / 1e6);
    mxl.n++;
    pthread_mutex_unlock(&mx_lock);
}

static void mx_abort(uintptr_t P, const char *why)
{
    mx_uv_off();
    if (HB(P + E_FLAG) == 1)
        HF(P + E_EXPO) = 0.0f;       /* the app ends the exposure (and sends 28) at its next pass */
    mx_seterr(why);
}

static void mx_plan_done(void)             /* the plan of this layer is used up (or dropped): the image buffer is free again */
{
    mxp.valid = 0;
    mx_busy = -1;
    mx_st = 0;
}

static void mx_stopped(uintptr_t P, int layer, int nst, const double got[4], uint64_t ref_us, const char *why)
{
    mx_abort(P, why);
    mx_record(layer, nst, got, ref_us, 0);
    mx_plan_done();
}

/* an exposure of a planned layer has started (P+0x30c = 1) */
static void mx_run(uintptr_t P)
{
    int layer = HI(P + W_LAYER), n = mxp.n, k, duty1 = -1;
    double got[4] = {0, 0, 0, 0};
    uint64_t ref_ms, ref_us, t_on, dead;
    if (mxp.layer != layer || HF(P + E_EXPO) != mxp.preset) {     /* cannot happen right after mx_plan_layer */
        mx_seterr("the exposure time was changed under us (file time)");
        HF(P + E_EXPO) = mxp.orig;
        mx_plan_done();
        return;
    }
    mx_st = 2;
    mx_k = 0;
    mx_n = n;
    ref_ms = mx_fresh_start(P);
    if (!ref_ms) {
        mx_stopped(P, layer, 0, got, mx_raw_us(), "exposure start time not fresh (UV off)");
        return;
    }
    ref_us = ref_ms * 1000u;
    t_on = ref_us;                    /* the app switched the UV on just before it set P+0x338 */
    dead = t_on + (uint64_t)(mxp.dt[0] * 1e6);
    if (n == 1) {                     /* one stage = the app's own exposure with the common time (preset = that time) */
        for (int q = 0; q < 4; q++)
            got[q] = mxp.dt[0];
        goto done;
    }
    mx_uv_on = 1;
    duty1 = dsp_rdint("/sys/class/pwm/pwmchip0/pwm0/duty_cycle");
    for (k = 0; k < n; k++) {
        size_t olen = 0;
        double s;
        if (k + 1 < n)
            olen = mxm_build(mx_buf[mxp.buf], mxp.len, mx_off, mxp.lit[k + 1], mx_out);
        mx_k = k + 1;
        mx_sleep_until(dead, P);
        if (HB(P + E_FLAG) != 1 || !mx_on) {
            mx_stopped(P, layer, k + 1, got, ref_us, "stopped (print paused / stopped or switch off, UV off)");
            return;
        }
        mx_uv_off();
        s = (double)(mx_raw_us() - t_on) / 1e6;
        for (int q = 0; q < 4; q++)
            if (mxp.lit[k] >> q & 1)
                got[q] += s;
        if (k + 1 == n)
            break;
        if (mx_swap(mx_out, olen)) {
            mx_stopped(P, layer, k + 1, got, ref_us, "writing to the display board failed (UV off)");
            return;
        }
        if (HB(P + E_FLAG) != 1 || !mx_on) {
            mx_stopped(P, layer, k + 1, got, ref_us, "stopped during a swap (UV off)");
            return;
        }
        ((void (*)(int, int))(uintptr_t)MX_UVON)(2, mxp.pwm);
        t_on = mx_raw_us();
        mx_uv_on = 1;
        dead = t_on + (uint64_t)(mxp.dt[k + 1] * 1e6);
        if (duty1 > 0 && dsp_rdint("/sys/class/pwm/pwmchip0/pwm0/duty_cycle") != duty1) {
            mx_stopped(P, layer, k + 2, got, ref_us, "UV level differs after a swap (UV off)");
            return;
        }
        if (k + 2 == n)               /* last stage: the app's end of exposure = our deadline + margin; we switch the UV off at the deadline */
            HF(P + E_EXPO) = (float)((double)(dead - ref_us) / 1000.0 + MX_MARGIN_MS);
    }
done:
    for (int i = 0; i < 300 && HB(P + E_FLAG) == 1; i++) {      /* the app ends the exposure itself */
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
        if (i == 250)
            HF(P + E_EXPO) = 0.0f;
    }
    mx_record(layer, n, got, ref_us, 1);
    mx_uv_off();
    mx_plan_done();
}

static volatile unsigned mx_hb, mx_exps, mx_planned;     /* diagnostics: thread loops, exposures seen, layers planned */

static void *mx_thread(void *arg)
{
    int seen = 0, idle = 0, in = 0, act_seen = 0, done_n = 0;
    (void)arg;
    mx_tid = (pid_t)syscall(SYS_gettid);
    for (;;) {
        uintptr_t P = HW(W_PPTR);
        struct timespec ts = {0, mx_on ? 10000000 : 100000000};
        mx_hb++;
        if (!P || (P & 3)) {
            if (mx_uv_on)
                mx_uv_off();
            if (seen && ++idle >= 300) {      /* 3 s without a job: the switch ends with the job */
                mx_on = 0;
                seen = act_seen = 0;
                pthread_mutex_lock(&mx_lock);
                mxp.valid = mxp.prep = 0;
                mx_busy = -1;
                mxp.used_seq = 0;
                pthread_mutex_unlock(&mx_lock);
                mx_st = 0;
            }
            in = 0;
        } else {
            int flag = HB(P + E_FLAG);
            idle = 0;
            if (mx_on || mx_uv_on)
                seen = 1;
            /* the app keeps its print structure after a job until the finished screen is dismissed: the job also ends when the logger
             * session has ended (s_active 0, about 20 s after the app is idle) for 3 s after it had been active in this job */
            if (s_active)
                act_seen = 1;
            if (mx_on && act_seen && !s_active && !flag) {
                if (++done_n >= 300) {
                    mx_on = 0;
                    seen = act_seen = done_n = 0;
                    pthread_mutex_lock(&mx_lock);
                    mxp.valid = mxp.prep = 0;
                    mx_busy = -1;
                    mxp.used_seq = 0;
                    pthread_mutex_unlock(&mx_lock);
                    mx_st = 0;
                }
            } else {
                done_n = 0;
            }
            if (flag == 1 && !in) {
                in = 1;
                mx_exps++;
                if (mx_on && mx_avail && mx_plan_layer(P)) {
                    mx_planned++;
                    mx_run(P);
                }
            } else if (flag != 1) {
                in = 0;
                if (mx_uv_on)
                    mx_uv_off();
                if (mx_on && mx_cap)
                    mx_prep();                /* the image of the next layer is checked while the printer lifts */
            }
        }
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- settings, commands, writer tick, json ---- */
static void mx_save(void)
{
    char path[160];
    FILE *f;
    snprintf(path, sizeof path, "%s/" MX_FILE, sg_csvdir);
    if ((f = fopen(path, "w"))) {
        fprintf(f, "%.3f %.3f %.3f %.3f %d %d\n", mx_t[0], mx_t[1], mx_t[2], mx_t[3], mx_mirx, mx_miry);
        fclose(f);
    }
}

static void mx_load(void)
{
    char path[160], buf[96];
    double v[6];
    snprintf(path, sizeof path, "%s/" MX_FILE, sg_csvdir);
    if (read_small(path, buf, sizeof buf) > 0 && parse_nums(buf, v, 6) == 6) {
        int good = 1;
        for (int i = 0; i < 4; i++)
            good &= v[i] >= 0.1;
        if (good) {
            for (int i = 0; i < 4; i++)
                mx_t[i] = v[i];
            mx_mirx = v[4] != 0;
            mx_miry = v[5] != 0;
        }
    }
}

static void mx_from_save(void)
{
    char path[160];
    FILE *f;
    snprintf(path, sizeof path, "%s/MXFROM", sg_csvdir);
    if ((f = fopen(path, "w"))) {
        fprintf(f, "%d\n", mx_from);
        fclose(f);
    }
}

static void mx_from_load(void)
{
    char path[160], buf[32];
    double v[1];
    snprintf(path, sizeof path, "%s/MXFROM", sg_csvdir);
    if (read_small(path, buf, sizeof buf) > 0 && parse_nums(buf, v, 1) == 1 && v[0] >= 1 && v[0] <= 99999 && v[0] == (double)(int)v[0])
        mx_from = (int)v[0];
}

static void sgmx_init(void)
{
    if (!in_chitu || !w_ok || access("/customer/mod/sgmx_disable", F_OK) == 0 || !e_ok || !mx_uvcode_ok())
        return;
    mx_avail = 1;                    /* the thread and the settings come with sgmx_tick, once per process (the app daemonizes: a thread made here would not survive the fork) */
}

static void sgmx_cmd(const char *c, char *res, size_t len)
{
    if (!mx_avail) {
        snprintf(res, len, "error: Multi Exposure is not available (not the AA variant, disabled, or the library is not loaded yet)");
        return;
    }
    if (strncmp(c, "off", 3) == 0) {
        mx_on = 0;
        return;
    }
    if (strncmp(c, "on", 2) == 0) {
        if (eset.bon || eset.non) {
            snprintf(res, len, "error: Manual Exposure is on: switch it off first");
            return;
        }
        if (spi_fd < 0 || dsp_sqrun || dsp_on) {
            snprintf(res, len, "error: no board link (or a display test is running)");
            return;
        }
        mx_seterr("");
        mx_on = 1;
        return;
    }
    if (strncmp(c, "from ", 5) == 0) {
        double v[1];
        if (parse_nums(c + 5, v, 1) != 1 || !(v[0] >= 1 && v[0] <= 99999) || v[0] != (double)(int)v[0]) {
            snprintf(res, len, "error: bad mx from command (layer 1-99999)");
            return;
        }
        mx_from = (int)v[0];
        mx_from_save();
        return;
    }
    if (strncmp(c, "set ", 4) == 0) {
        double v[6];
        if (parse_nums(c + 4, v, 6) != 6 || (v[4] != 0 && v[4] != 1) || (v[5] != 0 && v[5] != 1)) {
            snprintf(res, len, "error: bad mx set command (T1 T2 T3 T4 MIRX MIRY)");
            return;
        }
        for (int i = 0; i < 4; i++)
            if (!(v[i] >= 0.1)) {
                snprintf(res, len, "error: each quadrant time must be at least 0.1 s");
                return;
            }
        for (int i = 0; i < 4; i++)
            mx_t[i] = v[i];
        mx_mirx = (int)v[4];
        mx_miry = (int)v[5];
        mx_save();
        return;
    }
    snprintf(res, len, "error: bad mx command");
}

/* writer thread, every 0.5 s: buffers follow the switch; a burst that ended without a following frame is finished here */
static void sgmx_tick(void)
{
    static volatile pid_t mx_pid;
    pid_t me = getpid();
    if (!mx_avail)
        return;
    if (mx_pid != me) {
        pthread_t th;
        mx_load();
        mx_from_load();
        pthread_attr_t at;
        mx_pid = me;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 128 * 1024);
        if (pthread_create(&th, &at, mx_thread, NULL) != 0)
            mx_avail = 0;
        pthread_attr_destroy(&at);
    }
    if (mx_on && !mx_cap) {
        uint8_t *a = malloc(MX_BUF), *b = malloc(MX_BUF), *o = malloc(MX_BUF);
        uint32_t *off = malloc(sizeof(uint32_t) * (MXM_HALFROWS + 1));
        if (a && b && o && off) {
            pthread_mutex_lock(&mx_lock);
            mx_buf[0] = a;
            mx_buf[1] = b;
            mx_out = o;
            mx_off = off;
            mx_inburst = 0;
            mx_ready = mx_busy = -1;
            mxp.valid = 0;
            mxp.used_seq = 0;
            mx_ready_seq = 0;
            mx_cap = 1;
            pthread_mutex_unlock(&mx_lock);
        } else {
            free(a);
            free(b);
            free(o);
            free(off);
            mx_on = 0;
            mx_seterr("not enough memory for the image buffers");
        }
    } else if (!mx_on && mx_cap && mx_st == 0) {
        pthread_mutex_lock(&mx_lock);
        mx_cap = 0;
        mx_inburst = 0;
        mxp.valid = 0;
        free(mx_buf[0]);
        free(mx_buf[1]);
        free(mx_out);
        free(mx_off);
        mx_buf[0] = mx_buf[1] = mx_out = NULL;
        mx_off = NULL;
        pthread_mutex_unlock(&mx_lock);
    }
    pthread_mutex_lock(&mx_lock);
    if (mx_cap && mx_inburst && now_us() - mx_last_us > 100000u)
        mx_finish_burst();
    pthread_mutex_unlock(&mx_lock);
}

static int sgmx_json(char *buf, size_t len)
{
    int n;
    if (!mx_avail)
        return 0;
    pthread_mutex_lock(&mx_lock);
    n = snprintf(buf, len, ",\"mx\":{\"on\":%d,\"t\":[%.3f,%.3f,%.3f,%.3f],\"mirx\":%d,\"miry\":%d,\"from\":%d,\"st\":%d,\"k\":%d,\"n\":%d,\"cap\":%u,\"over\":%u,"
                 "\"err\":\"%s\",\"pl\":[%d,%d,%d,%.0f,%u],\"hb\":[%u,%u,%u],\"layers\":[",
                 mx_on ? 1 : 0, mx_t[0], mx_t[1], mx_t[2], mx_t[3], mx_mirx, mx_miry, mx_from, mx_st, mx_k, mx_n, mx_ready_seq, mx_overs, mx_err,
                 mxp.valid, mxp.layer, mxp.n, mxp.preset, mxp.content, mx_hb, mx_exps, mx_planned);
    for (int i = mxl.n - 1; i >= 0 && n < (int)len; i--)
        n += snprintf(buf + n, len - (size_t)n, "%s[%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f]", i == mxl.n - 1 ? "" : ",", mxl.r[i].layer, mxl.r[i].nst,
                      mxl.r[i].skipped, mxl.r[i].got[0], mxl.r[i].got[1], mxl.r[i].got[2], mxl.r[i].got[3], mxl.r[i].total);
    pthread_mutex_unlock(&mx_lock);
    if (n < (int)len)
        n += snprintf(buf + n, len - (size_t)n, "]}");
    return n;
}
