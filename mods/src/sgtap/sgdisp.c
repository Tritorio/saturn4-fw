/*
 * sgdisp.c - show an image on the exposure LCD with the UV OFF (or very dim), from the dashboard helper (step 8.26),
 * #included by sgtap.c after sgspi.c. Commands (sgwebd /api/cmd -> cmd file -> writer thread):
 *   "lcdshow NAME UV SECS"     NAME = text | grid | full (the printer's own LCD test images, resources/exposure_test_image0-2.bin)
 *                              or sglcd/<name>.bin (made by tools/lcd_stream.py); UV = LED in percent of the app's own scale 0..100
 *                              (0 = UV OFF, the default use); SECS 1..30 = how long, then the display is
 *                              cleared, the UV switched off and the fan stopped. The cooling fan runs at 100 % for the whole display
 *                              (it stalls at lower PWM test ) and for 10 s after a UV run.
   "lcdswap NAME RESET"       while a lcdshow runs: replace the image (UV, fan, end time untouched); RESET 1 = 28 2a 2c first, 0 = new stream + 4c + 29 only.
 *   "lcdseq UV NAME1 S1 [NAME2 S2 ... up to 8]" MULTI EXPOSURE sequence: per stage the UV goes OFF, the image is swapped
 *                              (28 2a 2c + stream + 4c x8 + 29, ~0.6 s dark) and the UV goes ON for S seconds (decimals ok, no upper limit by the
 *                              user's choice, > 0 only). Own thread, absolute deadlines in 20 ms slices; the measured UV-on time of every stage
 *                              is in live.json lcd.seq.got and in sglcd/seq_last.txt. Needs sglcd/ALLOW_UV for UV > 0 like lcdshow; lcdoff aborts.
 *                              Made with tools/lcd_stream.py quads (cumulative quadrant masks: each quadrant its own exposure time).
 *   "lcdoff"                   clear the display and UV off at once (the fan runs on 10 s after a UV run).
 * CHANNELS (docs/exposure_lcd_pipeline.txt): the UV LED is the SoC's PWM channel 0 (sysfs pwm0), set with the app's own
 * 0x883a8(0, percent); the FAN is app channel 3 = link channel 1 = the frame fe 04 01 <period 0x07a1> <duty> on the SPI device. Order: fan on
 * first, UV after the image is shown; stop: UV off first, then the fan.
 * What it sends is what the app itself sends for its LCD test (docs/exposure_lcd_pipeline.txt of ), on the app's own
 * SPI fd, under the app's own transport lock (0x3a1590), one sequence in one go:
 *   fe 04 01 07 a1 <fan> (3-byte answer) | 28 00 00 00, 2a 00 00 00, 2c 00 00 00 | 0x3c + 512 bytes ... (the stream from byte
 *   48 of the file, last packet zero padded) | 8 x (0x4c + 2047 zeros) | 29 00 00 00 | then, outside the lock, only if UV > 0: 0x883a8(0, UV).
 * Stop = UV 0, 28 2a 2c, like the app; the fan goes off at once, or 10 s later when the UV was on.
 * SAFETY: only while the printer is idle (no job, printer state not 10) and the tray is not the app's business: the command is
 * refused otherwise; a job starting stops the display at once (writer tick); DUTY > 0 needs the file <csvdir>/../sglcd/ALLOW_UV
 * (created by hand, with the resin tank removed); UV 0..100 % and time 1..30 s (limits chosen by the user ), the fan is fixed at 100 %; the file must have the
 * stream header (1d 01 fd 12 + its own size) and 1000..8000000 bytes. Code words of chitu V1.5.9 (ramp 0x817d8 like sgmach) are
 * checked, the app's SPI fd must be known (sgspi.c). Kill switch: /customer/mod/sglcd_disable.
 * live.json "lcd":{"on":0|1,"name":"..","uv":n,"fan":n,"left":s,"pwm0":duty,"pwm3":duty} (pwm0 = UV, pwm3 = sysfs duty_cycle, -1 unknown).
 */
#define DSP_DIR       "/media/mmcblk0p2/sglcd"
#define DSP_MUTEX     0x3a1590u         /* the app's transport mutex (pthread_mutex_t) */
#define DSP_CHAN      0x830acu          /* the app's channel select (0 = the display board), idempotent */
#define DSP_PERIOD    0x07a1u           /* UV PWM period used by the app (1953) */
#define DSP_PWM       0x883a8u          /* the app's pwm set(channel, percent): ch0 = UV LED (SoC pwm0), ch3 = fan (link ch1) */
#define DSP_PWM_CODE  0xe3510000u       /* first word of it: cmp r1, #0 */
#define DSP_MAXPCT    100
#define DSP_FAN       100
#define DSP_MAXSECS   30
#define DSP_RUNON_US  10000000u         /* the fan runs on after a UV run */

static volatile int dsp_on;
static char dsp_name[40];
static int dsp_duty;                    /* UV percent */
static int dsp_fan;                     /* fan percent */
static uint64_t dsp_end_us;
static uint64_t dsp_fan_off_us;         /* fan run-on: switch it off at this time (0 = none) */
static int dsp_pwm0 = -1, dsp_pwm3 = -1;
static int dsp_swaps;                   /* lcdswap count of the running display */
static int dsp_test;                    /* tests: no chitu checks, no app mutex */

static int dsp_frame(const void *p, size_t n)
{
    return write(spi_fd, p, n) == (ssize_t)n ? 0 : -1;
}

static int dsp_cmd4(unsigned char c)
{
    unsigned char f[4] = {c, 0, 0, 0};
    return dsp_frame(f, 4);
}

/* the app's channel select (gpio 0x48): 0 = the display board (28/29/2a/2c, 0x3c, 0x4c frames), 1 = the second device that takes the
 * 0xfe frames (UV). Every app function selects its channel before it writes; the call is idempotent. */
static void dsp_chan(unsigned c)
{
    if (!dsp_test)
        ((void (*)(unsigned))(uintptr_t)DSP_CHAN)(c);
}

/* FAN frame on channel 1 (fe 04 01 period duty, duty in counts of the period) + the device's 3-byte answer "fe 00 00", then back to channel 0 */
static int dsp_fanframe(unsigned duty)
{
    unsigned char f[7] = {0xfe, 0x04, 0x01, DSP_PERIOD >> 8, DSP_PERIOD & 255, (unsigned char)(duty >> 8), (unsigned char)(duty & 255)};
    unsigned char a[3];
    int r = 0;
    dsp_chan(1);
    if (dsp_frame(f, sizeof f) != 0)
        r = -1;
    else if (!dsp_test && (read(spi_fd, a, sizeof a) != (ssize_t)sizeof a || a[0] != 0xfe))
        r = -2;                         /* a fake file does not answer */
    dsp_chan(0);
    return r;
}

/* UV LED = the SoC's PWM channel 0, through the app's own function (writes sysfs pwm0 duty_cycle); not under the transport lock */
static int dsp_uvpwm(int pct)
{
    if (dsp_test) {
        FILE *f = fopen(getenv("SGDISP_PWM") ? getenv("SGDISP_PWM") : "/tmp/fakepwm0", "a");
        if (f) {
            fprintf(f, "%d\n", pct);
            fclose(f);
        }
        return 0;
    }
    if (*(volatile uint32_t *)(uintptr_t)DSP_PWM != DSP_PWM_CODE)
        return -1;
    ((int (*)(int, int))(uintptr_t)DSP_PWM)(0, pct);
    return 0;
}

static int dsp_fancount(int pct)
{
    return (int)((unsigned)DSP_PERIOD * (unsigned)pct / 100u);
}

static void dsp_lock(int on)
{
    if (dsp_test)
        return;
    if (on) {
        pthread_mutex_lock((pthread_mutex_t *)(uintptr_t)DSP_MUTEX);
        dsp_chan(0);
    } else {
        pthread_mutex_unlock((pthread_mutex_t *)(uintptr_t)DSP_MUTEX);
    }
}

static int dsp_idle(void);

/* fan off now (idle printer only: during a job the app owns the fan) */
static void dsp_fanoff(void)
{
    dsp_fan_off_us = 0;
    dsp_fan = 0;
    if (spi_fd < 0 || !dsp_idle())      /* a job started: the app owns the fan, never write it then */
        return;
    dsp_lock(1);
    dsp_fanframe(0);
    dsp_lock(0);
}

/* UV off first, display off, then the fan: at once, or after the run-on when the UV was on (runon = 1) */
static void dsp_stop(int runon)
{
    int uv = dsp_duty;
    if (spi_fd < 0)
        return;
    dsp_uvpwm(0);
    dsp_lock(1);
    dsp_cmd4(0x28);
    dsp_cmd4(0x2a);
    dsp_cmd4(0x2c);
    dsp_lock(0);
    dsp_on = 0;
    dsp_duty = 0;
    if (runon && uv > 0 && dsp_fan > 0)
        dsp_fan_off_us = now_us() + DSP_RUNON_US;
    else
        dsp_fanoff();
}

static int dsp_idle(void)
{
    if (dsp_test)
        return 1;
    return in_chitu && ms_check() && h_ok && HW(H_PSTATE) != 10 && !s_active;
}

static const char *dsp_path(const char *name, char *buf, size_t len)
{
    static const char *const bi[3] = {"text", "grid", "full"};
    for (int i = 0; i < 3; i++)
        if (strcmp(name, bi[i]) == 0) {
            snprintf(buf, len, "%s/exposure_test_image%d.bin", dsp_test && getenv("SGDISP_RES") ? getenv("SGDISP_RES") : "/customer/resources", i);
            return buf;
        }
    snprintf(buf, len, "%s/%s.bin", dsp_test && getenv("SGDISP_DIR") ? getenv("SGDISP_DIR") : DSP_DIR, name);
    return buf;
}

/* read + check the stream file; returns malloc'd buffer (caller frees) or NULL with res set */
static unsigned char *dsp_load(const char *path, size_t *n, char *res, size_t rl)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long sz;
    if (!f) {
        snprintf(res, rl, "error: image file not found (%s)", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 1000 || sz > 8000000 || !(b = malloc((size_t)sz))) {
        fclose(f);
        snprintf(res, rl, "error: image file size %ld not in 1000..8000000", sz);
        return NULL;
    }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        snprintf(res, rl, "error: reading the image file failed");
        return NULL;
    }
    fclose(f);
    if (b[0] != 0x1d || b[1] != 0x01 || b[2] != 0xfd || b[3] != 0x12 ||
        (uint32_t)(b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24) != (uint32_t)sz) {
        free(b);
        snprintf(res, rl, "error: not a stream file (header magic / size)");
        return NULL;
    }
    *n = (size_t)sz;
    return b;
}

/* one image to the board under the transport lock: [fan on] [28 2a 2c reset] 0x3c packets, 8 x 0x4c, 0x29 */
static int dsp_stream(const unsigned char *img, size_t sz, int fan_first, int reset)
{
    unsigned char pk[513], z[2048];
    int err = 0;
    dsp_lock(1);
    if (fan_first)
        err |= dsp_fanframe((unsigned)dsp_fancount(DSP_FAN));   /* fan on before anything is shown */
    if (reset)
        err |= dsp_cmd4(0x28) | dsp_cmd4(0x2a) | dsp_cmd4(0x2c);
    pk[0] = 0x3c;
    for (size_t off = 48; off < sz && !err; off += 512) {
        size_t m = sz - off < 512 ? sz - off : 512;
        memcpy(pk + 1, img + off, m);
        if (m < 512)
            memset(pk + 1 + m, 0, 512 - m);
        err |= dsp_frame(pk, sizeof pk);
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

static int dsp_namecheck(const char *name)
{
    if (!name[0])
        return 0;
    for (size_t i = 0; name[i]; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '_' || name[i] == '-'))
            return 0;
    return 1;
}

#define DSP_SEQMAX 8
static char dsp_sqname[DSP_SEQMAX][40];
static double dsp_sqsecs[DSP_SEQMAX], dsp_sqgot[DSP_SEQMAX];
static int dsp_sqn, dsp_sqi, dsp_squv;
static volatile int dsp_sqrun, dsp_sqabort;

static void dsp_sq_note(void)
{
    char p[96];
    FILE *f;
    snprintf(p, sizeof p, "%s/seq_last.txt", dsp_test && getenv("SGDISP_DIR") ? getenv("SGDISP_DIR") : DSP_DIR);
    if (!(f = fopen(p, "w")))
        return;
    fprintf(f, "UV %d %%, %d stages\n", dsp_squv, dsp_sqn);
    for (int i = 0; i < dsp_sqn; i++)
        fprintf(f, "stage %d %s want %.3f s, UV on %.3f s\n", i + 1, dsp_sqname[i], dsp_sqsecs[i], dsp_sqgot[i]);
    fclose(f);
}

/* the sequence: UV off -> swap -> UV on -> wait to an absolute deadline; the UV-on time is measured around the two pwm calls */
static void *dsp_sq_thread(void *arg)
{
    char path[200], res[120];
    (void)arg;
    for (int i = 0; i < dsp_sqn && !dsp_sqabort; i++) {
        size_t sz;
        unsigned char *img;
        uint64_t t_on, dead;
        int err;
        dsp_sqi = i;
        if (!(img = dsp_load(dsp_path(dsp_sqname[i], path, sizeof path), &sz, res, sizeof res)))
            break;
        dsp_uvpwm(0);
        err = dsp_stream(img, sz, i == 0, 1);
        free(img);
        if (err || dsp_sqabort)
            break;
        snprintf(dsp_name, sizeof dsp_name, "%s", dsp_sqname[i]);
        dsp_swaps = i;
        if (dsp_squv > 0)
            dsp_uvpwm(dsp_squv);
        t_on = now_us();
        dead = t_on + (uint64_t)(dsp_sqsecs[i] * 1000000.0);
        while (now_us() < dead && !dsp_sqabort) {
            struct timespec ts = {0, 20000000};     /* nanosleep: our own usleep wrapper belongs to the UI loop patch */
            nanosleep(&ts, NULL);
        }
        dsp_uvpwm(0);
        dsp_sqgot[i] = (double)(now_us() - t_on) / 1e6;
    }
    dsp_stop(1);
    dsp_sq_note();
    dsp_sqrun = 0;
    return NULL;
}

static void sgdisp_seq(const char *c, char *res, size_t len)
{
    char name[DSP_SEQMAX][40], path[200];
    double secs[DSP_SEQMAX], uv;
    int n = 0, uvi;
    const char *p = c;
    pthread_t th;
    pthread_attr_t at;
    dsp_test = !in_chitu && getenv("SGTAP_FORCE") != NULL;
    if (dsp_sqrun || dsp_on) {
        snprintf(res, len, "error: a display is running (lcdoff first)");
        return;
    }
    while (*p == ' ')
        p++;
    for (uvi = 0; *p >= '0' && *p <= '9'; p++)
        uvi = uvi * 10 + (*p - '0');
    uv = uvi;
    if (uv > DSP_MAXPCT || (*p != ' ' && *p)) {
        snprintf(res, len, "error: bad lcdseq command (UV 0..%d, then NAME SECS pairs)", DSP_MAXPCT);
        return;
    }
    for (;;) {
        size_t k = 0;
        int dig = 0, dot = 0;
        while (*p == ' ')
            p++;
        if (!*p || *p == '\n' || *p == '\r')
            break;
        if (n >= DSP_SEQMAX) {
            snprintf(res, len, "error: at most %d stages", DSP_SEQMAX);
            return;
        }
        while (*p && *p != ' ' && k < sizeof name[0] - 1)
            name[n][k++] = *p++;
        name[n][k] = 0;
        while (*p == ' ')
            p++;
        for (const char *q = p; (*q >= '0' && *q <= '9') || *q == '.'; q++) {
            if (*q == '.')
                dot++;
            else
                dig++;
        }
        if (!dsp_namecheck(name[n]) || !dig || dot > 1 || (secs[n] = parse_k(p)) <= 0) {
            snprintf(res, len, "error: bad lcdseq stage %d (NAME SECS, SECS > 0)", n + 1);
            return;
        }
        while ((*p >= '0' && *p <= '9') || *p == '.')
            p++;
        if (*p && *p != ' ' && *p != '\n' && *p != '\r') {
            snprintf(res, len, "error: bad lcdseq stage %d (NAME SECS, SECS > 0)", n + 1);
            return;
        }
        n++;
    }
    if (!n) {
        snprintf(res, len, "error: lcdseq needs at least one NAME SECS pair");
        return;
    }
    if (access("/customer/mod/sglcd_disable", F_OK) == 0 || spi_fd < 0) {
        snprintf(res, len, "error: LCD display not available (no board link or switched off)");
        return;
    }
    if (!dsp_idle()) {
        snprintf(res, len, "error: only while the printer is idle (no job, not printing)");
        return;
    }
    if (uv > 0 && !dsp_test && access(DSP_DIR "/ALLOW_UV", F_OK) != 0) {
        snprintf(res, len, "error: UV above 0 needs the file " DSP_DIR "/ALLOW_UV (create it by hand, resin tank removed)");
        return;
    }
    if (uv > 0 && dsp_test && !getenv("SGDISP_ALLOW")) {
        snprintf(res, len, "error: test: UV needs SGDISP_ALLOW");
        return;
    }
    if (!dsp_test && *(volatile uint32_t *)(uintptr_t)DSP_PWM != DSP_PWM_CODE) {
        snprintf(res, len, "error: the app's PWM function is not where expected");
        return;
    }
    for (int i = 0; i < n; i++) {       /* every file checked BEFORE the first UV on */
        size_t sz;
        unsigned char *img = dsp_load(dsp_path(name[i], path, sizeof path), &sz, res, len);
        if (!img)
            return;
        free(img);
    }
    for (int i = 0; i < n; i++) {
        snprintf(dsp_sqname[i], sizeof dsp_sqname[i], "%s", name[i]);
        dsp_sqsecs[i] = secs[i];
        dsp_sqgot[i] = 0;
    }
    dsp_sqn = n;
    dsp_sqi = 0;
    dsp_squv = (int)uv;
    dsp_sqabort = 0;
    dsp_fan_off_us = 0;
    dsp_fan = DSP_FAN;
    dsp_duty = (int)uv;
    dsp_swaps = 0;
    dsp_end_us = 0;
    dsp_on = 1;
    dsp_sqrun = 1;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);
    if (pthread_create(&th, &at, dsp_sq_thread, NULL) != 0) {
        dsp_sqrun = 0;
        dsp_on = 0;
        dsp_duty = 0;
        dsp_fan = 0;
        snprintf(res, len, "error: cannot start the sequence thread");
    }
    pthread_attr_destroy(&at);
}

/* "lcdswap NAME RESET": replace the image while the display is on (UV and fan untouched); RESET 1 = send 28 2a 2c first, 0 = only the new
 * stream + 4c + 29. Experiment for the temporal anti-aliasing idea (what the screen does during a swap). */
static void sgdisp_swap(const char *c, char *res, size_t len)
{
    char name[40], path[200];
    double v[1];
    size_t k = 0, sz;
    unsigned char *img;
    dsp_test = !in_chitu && getenv("SGTAP_FORCE") != NULL;
    while (c[k] && c[k] != ' ' && k < sizeof name - 1) {
        name[k] = c[k];
        k++;
    }
    name[k] = 0;
    if (!k || c[k] != ' ' || parse_nums(c + k, v, 1) != 1 || (v[0] != 0 && v[0] != 1) || !dsp_namecheck(name)) {
        snprintf(res, len, "error: bad lcdswap command (NAME RESET, RESET 0 or 1)");
        return;
    }
    if (access("/customer/mod/sglcd_disable", F_OK) == 0 || spi_fd < 0 || !dsp_on || dsp_sqrun) {
        snprintf(res, len, "error: lcdswap needs a running lcdshow");
        return;
    }
    if (!dsp_idle()) {
        snprintf(res, len, "error: only while the printer is idle (no job, not printing)");
        return;
    }
    if (!(img = dsp_load(dsp_path(name, path, sizeof path), &sz, res, len)))
        return;
    if (dsp_stream(img, sz, 0, (int)v[0])) {
        free(img);
        dsp_stop(1);
        snprintf(res, len, "error: sending to the display board failed (stopped, UV off)");
        return;
    }
    free(img);
    snprintf(dsp_name, sizeof dsp_name, "%s", name);
    dsp_swaps++;
}

static void sgdisp_cmd(const char *c, char *res, size_t len)
{
    char name[40], path[200];
    double v[2];
    size_t k = 0, sz;
    unsigned char *img;
    int duty, secs, err = 0;
    dsp_test = !in_chitu && getenv("SGTAP_FORCE") != NULL;
    if (strcmp(c, "off") == 0) {
        if (dsp_sqrun) {                /* the sequence thread switches everything off itself */
            dsp_sqabort = 1;
            return;
        }
        if (spi_fd < 0 || !dsp_idle()) {
            snprintf(res, len, "error: not available now");
            return;
        }
        dsp_stop(1);
        return;
    }
    while (c[k] && c[k] != ' ' && k < sizeof name - 1) {
        name[k] = c[k];
        k++;
    }
    name[k] = 0;
    if (!k || c[k] != ' ' || parse_nums(c + k, v, 2) != 2 || v[0] != (int)v[0] || v[1] != (int)v[1]) {
        snprintf(res, len, "error: bad lcdshow command");
        return;
    }
    if (!dsp_namecheck(name)) {
        snprintf(res, len, "error: bad image name");
        return;
    }
    duty = (int)v[0];
    secs = (int)v[1];
    if (duty < 0 || duty > DSP_MAXPCT || secs < 1 || secs > DSP_MAXSECS) {
        snprintf(res, len, "error: UV 0..%d %%, seconds 1..%d", DSP_MAXPCT, DSP_MAXSECS);
        return;
    }
    if (access("/customer/mod/sglcd_disable", F_OK) == 0 || spi_fd < 0) {
        snprintf(res, len, "error: LCD display not available (no board link or switched off)");
        return;
    }
    if (!dsp_idle() || dsp_sqrun) {
        snprintf(res, len, "error: only while the printer is idle (no job, not printing) and no sequence runs");
        return;
    }
    if (duty > 0 && !dsp_test && access(DSP_DIR "/ALLOW_UV", F_OK) != 0) {
        snprintf(res, len, "error: UV above 0 needs the file " DSP_DIR "/ALLOW_UV (create it by hand, resin tank removed)");
        return;
    }
    if (duty > 0 && dsp_test && !getenv("SGDISP_ALLOW")) {
        snprintf(res, len, "error: test: UV needs SGDISP_ALLOW");
        return;
    }
    if (!dsp_test && *(volatile uint32_t *)(uintptr_t)DSP_PWM != DSP_PWM_CODE) {
        snprintf(res, len, "error: the app's PWM function is not where expected");
        return;
    }
    if (!(img = dsp_load(dsp_path(name, path, sizeof path), &sz, res, len)))
        return;
    dsp_uvpwm(0);                       /* UV off first, whatever it was */
    dsp_fan_off_us = 0;
    err |= dsp_stream(img, sz, 1, 1);
    free(img);
    dsp_fan = DSP_FAN;
    if (!err && duty > 0)
        err |= dsp_uvpwm(duty);
    if (err) {
        dsp_duty = 0;
        dsp_stop(0);
        snprintf(res, len, "error: sending to the display board failed (stopped, UV off)");
        return;
    }
    snprintf(dsp_name, sizeof dsp_name, "%s", name);
    dsp_swaps = 0;
    dsp_duty = duty;
    dsp_end_us = now_us() + (uint64_t)secs * 1000000u;
    dsp_on = 1;
}

/* writer thread, every 0.5 s */
static int dsp_rdint(const char *path)
{
    char b[24];
    ssize_t n;
    int fd = real_open(path, O_RDONLY | O_CLOEXEC), v = 0;
    if (fd < 0)
        return -1;
    n = real_read(fd, b, sizeof b - 1);
    real_close(fd);
    if (n <= 0)
        return -1;
    for (ssize_t i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++)
        v = v * 10 + (b[i] - '0');
    return v;
}

static void sgdisp_tick(void)
{
    if (!dsp_test && in_chitu) {
        dsp_pwm0 = dsp_rdint("/sys/class/pwm/pwmchip0/pwm0/duty_cycle");
        dsp_pwm3 = dsp_rdint("/sys/class/pwm/pwmchip0/pwm3/duty_cycle");
    }
    if (dsp_fan_off_us && !dsp_on && now_us() >= dsp_fan_off_us) {
        if (dsp_idle())
            dsp_fanoff();
        else {
            dsp_fan_off_us = 0;         /* a job started: the app owns the fan now */
            dsp_fan = 0;
        }
    }
    if (dsp_sqrun) {                    /* the sequence thread owns the display; the tick only watches for a job */
        if (!dsp_idle())
            dsp_sqabort = 1;
        return;
    }
    if (!dsp_on)
        return;
    if (now_us() >= dsp_end_us || !dsp_idle())
        dsp_stop(dsp_idle());
}

static int sgdisp_json(char *buf, size_t len)
{
    uint64_t t = now_us();
    char got[160] = "";
    size_t g = 0;
    for (int i = 0; i < dsp_sqn && g < sizeof got - 24; i++)
        g += (size_t)snprintf(got + g, sizeof got - g, "%s%.2f", i ? "," : "", dsp_sqgot[i]);
    return snprintf(buf, len, ",\"lcd\":{\"on\":%d,\"name\":\"%s\",\"uv\":%d,\"fan\":%d,\"left\":%d,\"pwm0\":%d,\"pwm3\":%d,\"swaps\":%d,\"seq\":{\"run\":%d,\"step\":%d,\"n\":%d,\"got\":[%s]}}",
                    dsp_on ? 1 : 0, dsp_on ? dsp_name : "", dsp_duty, dsp_fan, dsp_on && dsp_end_us > t ? (int)((dsp_end_us - t) / 1000000u) : 0,
                    dsp_pwm0, dsp_pwm3, dsp_swaps, dsp_sqrun ? 1 : 0, dsp_sqi + 1, dsp_sqn, got);
}
