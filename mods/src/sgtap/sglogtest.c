/* Logger test for libsgtap.so (sglog.c), on the printer, no real tty touched:
 * a pty pair plays the gauge board (5 Hz polling, replies split "01" + 8 bytes,
 * one corrupted reply), a plain file plays the app log (ticks = CLOCK_MONOTONIC ms).
 *   mkdir -p /tmp/lt && SGTAP_FORCE=1 SGTAP_MATCH=/dev/pts/ SGTAP_GAUGE=/dev/pts/ \
 *     SGTAP_LOG=/tmp/lt/log SGTAP_CSVDIR=/tmp/lt/csv SGTAP_DIR=/tmp/lt \
 *     LD_PRELOAD=./libsgtap.so ./sglogtest
 * Expect /tmp/lt/csv/<date>_My_Part__v2_.csv: zero = -3794, probe ~ +1950, layers 0..2;
 * grams: ref_calibrate = -379350 in the .txt, probe g ~ +1947, peel g_min ~ -1196
 * (k = 100.33, or the number in /tmp/lt/csv/K).
 * sgweb.c: add SGTAP_LIVEDIR=/tmp/lt/live SGLT_IDLE=250 -> /tmp/lt/live/live.json; 10 s
 * after the print auto-tare sets the zero, after 30 s idle a +10 g drift is followed. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int logfd;

static unsigned long long ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000ull + t.tv_nsec / 1000000;
}

static void applog(const char *msg)
{
    char l[256];
    int n = snprintf(l, sizeof l, "[printer][Info][1][%llu]:%s\n", ms(), msg);
    write(logfd, l, n);
}

static uint16_t crc(const unsigned char *p, int n)
{
    uint16_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    return c;
}

/* one poll: app writes the request, "board" answers raw value, app reads it */
static void poll(int app, int board, int32_t raw, int corrupt)
{
    static const unsigned char req[4] = {0x01, 0x03, 0x43, 0x2b};
    unsigned char r[9] = {0x01, 0x03, 0x04}, b[64];
    uint16_t c;
    memcpy(r + 3, &raw, 4);
    c = crc(r, 7);
    r[7] = c >> 8;
    r[8] = c & 0xff;
    if (corrupt)
        r[5] ^= 0x40;
    write(app, req, 4);
    read(board, b, sizeof b);
    write(board, r, 1);                 /* the board sends "01" first, then the rest */
    usleep(2000);
    read(app, b, 1);
    write(board, r + 1, 8);
    usleep(2000);
    read(app, b, sizeof b);
    usleep(196000);                     /* ~5 Hz */
}

static int idle_polls(void)          /* no atoi: see build_sgtap.sh */
{
    const char *p = getenv("SGLT_IDLE");
    int n = 0;
    if (!p)
        return 10;
    while (*p >= '0' && *p <= '9')
        n = n * 10 + *p++ - '0';
    return n;
}

int main(void)
{
    int m, s, i, f;
    FILE *pf;
    struct termios t;
    if (!getenv("NOFORK") && fork() > 0) /* daemonize like chitu */
        return 0;
    setsid();
    logfd = open("/tmp/lt/log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (logfd < 0 || m < 0 || grantpt(m) || unlockpt(m))
        return 1;
    s = open(ptsname(m), O_RDWR | O_NOCTTY);
    tcgetattr(s, &t);
    cfmakeraw(&t);
    tcsetattr(s, TCSANOW, &t);
    for (i = 0; i < 15; i++)            /* 3 s idle baseline */
        poll(s, m, -379400 - (i & 1) * 50, i == 7);
    applog("printer_main_routine enter");
    /* like chitu: fopen the print file and keep it open during the print */
    f = open("/tmp/lt/My Part (v2).ctb", O_RDONLY | O_CREAT, 0644);
    close(f);
    pf = fopen("/tmp/lt/My Part (v2).ctb", "rb");
    for (i = 0; i < 5; i++)
        poll(s, m, -379400, 0);
    applog("start calibrate!!!");       /* grams reference = mean raw of these rows */
    for (i = 0; i < 10; i++)
        poll(s, m, -379350, 0);
    applog("start probe!!!");
    for (i = 0; i < 10; i++)
        poll(s, m, -379400 + 195400, 0);    /* +1954 in app units */
    for (int layer = 0; layer < 3; layer++) {
        char l[32];
        snprintf(l, sizeof l, "layer %d:", layer);
        applog(l);
        if (layer == 1 && getenv("SGLT_ALERT")) {   /* alert lines (sglog.c alert_scan): one repeated, one kind of each */
            applog("sg_trig 1");
            applog("sg_trig 2");
            applog("constant->sg_trigered = 0");    /* must NOT be an alert */
            applog("the resin pos is abnormal high: 12.500000 ,max line value: 11.000000");
            applog("the resin pos is abnormal low: 1.500000 ,min line value: 3.000000");
            applog("probe1 failed zero_data 5000 ths 3000");
            applog("level fail!!!");
            applog("calibrate error:3!!!");
            applog("motor home failed");
            applog("print error code 7");
        }
        applog("printer start exposure");
        for (i = 0; i < 5; i++)
            poll(s, m, -379400, 0);
        applog("lifting = 1 sync_mode 1");
        for (i = 0; i < 5; i++)
            poll(s, m, -379400 - 30000 * i, 0); /* peel: down to -1200 */
        applog("lifting = 2 sync_mode 1");
        for (i = 0; i < 3; i++)
            poll(s, m, -379400, 0);
    }
    applog("printer_main_routine exit");
    if (getenv("SGLT_ALERT"))
        applog("sg_trig 2");            /* after the job: not an alert */
    if (pf)
        fclose(pf);
    /* idle after the print: 2 s, or SGLT_IDLE polls (auto-tare test of sgweb.c) */
    for (i = 0; i < idle_polls(); i++)
        poll(s, m, -379400 + (i > 150 ? 1000 : 0), 0);   /* +10 g drift after 30 s */
    printf("done\n");
    return 0;
}
