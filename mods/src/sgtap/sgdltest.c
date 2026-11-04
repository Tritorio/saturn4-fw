/* host test of sgdl.c: gcc -o /tmp/sgdltest sgdltest.c && /tmp/sgdltest */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sgdl.c"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main(void)
{
    struct dl_det d;
    /* 1: a peel: force builds to -1800 g, then recovers: fires at the 2nd rising reading */
    double g1[] = {0, 3, -50, -400, -1500, -1800, -1200, -600, -100, 0};
    int fired = -1;
    dl_det_start(&d, 1000000, 100.0);
    for (int i = 0; i < 10; i++)
        if (dl_det_feed(&d, 1000000 + g1[i] * 100.0, 10.0 + 5 * i) && fired < 0)
            fired = i;
    CHECK(fired == 7, "fired at %d (want 7)", fired);
    CHECK(fabs(d.peak + 1800) < 0.5, "peak %.1f", d.peak);
    CHECK(d.sep && !d.nopeel, "sep %d nopeel %d", d.sep, d.nopeel);
    CHECK(!dl_det_angle(&d, 30), "no-peel after a separation");
    /* 2: a flat reading after the minimum resets the count; noise below 2 g does not count as rising */
    double g2[] = {0, -300, -302, -301, -300.5, -299.9, -200, -100};
    fired = -1;
    dl_det_start(&d, 5000, 100.0);
    for (int i = 0; i < 8; i++)
        if (dl_det_feed(&d, 5000 + g2[i] * 100.0, 20.0) && fired < 0)
            fired = i;
    CHECK(fired == 7, "noise case fired at %d (want 7)", fired);
    /* 3: no peel: small force, the angle rule fires once at 20 deg */
    dl_det_start(&d, 0, 100.0);
    dl_det_feed(&d, 2000, 5);         /* +20 g */
    dl_det_feed(&d, -3000, 12);       /* -30 g */
    CHECK(!dl_det_angle(&d, 19.9), "early");
    CHECK(dl_det_angle(&d, 20.0), "no-peel at 20");
    CHECK(!dl_det_angle(&d, 25.0), "only once");
    CHECK(d.nopeel, "flag");
    /* 4: a peak of 99 g is no peak, 101 g is */
    dl_det_start(&d, 0, 100.0);
    dl_det_feed(&d, -9900, 5); dl_det_feed(&d, -9000, 6); dl_det_feed(&d, -8000, 7);
    CHECK(!d.sep, "99 g must not count");
    dl_det_start(&d, 0, 100.0);
    dl_det_feed(&d, -10100, 5); dl_det_feed(&d, -9000, 6);
    CHECK(dl_det_feed(&d, -8000, 7), "101 g counts");
    /* 5: the controller, a reference sequence (set 36, min 10, limit 2000, lower 1500, 20 %, 5 %) */
    struct dl_cfg cfg = {1, 2000, 1500, 20, 5};
    struct dl_ctl c;
    double peaks[] = {1100, 1350, 1620, 1900, 2250, 2100, 1780, 1450, 1380, 1520, 1750, 1950, 2080, 1700};
    double want[] = {36, 36, 36, 36, 36, 28.8, 23.0, 18.4, 19.3, 20.3, 21.3, 22.4, 23.5, 18.8};
    dl_ctl_reset(&c);
    for (int i = 0; i < 14; i++) {
        double s = dl_start_ds(&c, 36.0);
        CHECK(fabs(s - want[i]) < 0.06, "layer %d speed %.2f want %.1f", i + 1, s, want[i]);
        dl_ctl_update(&c, &cfg, 36.0, peaks[i]);
    }
    CHECK(fabs(dl_start_ds(&c, 36.0) - 15.0) < 0.06, "next speed %.2f want 15.0", dl_start_ds(&c, 36.0));
    /* 6: the lower clamp: never under 10 deg/s, and a recovery starts from the clamp */
    dl_ctl_reset(&c);
    for (int i = 0; i < 30; i++)
        dl_ctl_update(&c, &cfg, 36.0, 3000);
    CHECK(fabs(dl_start_ds(&c, 36.0) - 10.0) < 1e-9, "clamp %.3f", dl_start_ds(&c, 36.0));
    dl_ctl_update(&c, &cfg, 36.0, 500);       /* below the lower level: raise from the clamp */
    CHECK(fabs(dl_start_ds(&c, 36.0) - 10.5) < 0.01, "after clamp %.3f", dl_start_ds(&c, 36.0));
    /* 7: a base lower than 10 is used as it is */
    dl_ctl_reset(&c);
    CHECK(fabs(dl_start_ds(&c, 8.0) - 8.0) < 1e-9, "base 8");
    dl_ctl_update(&c, &cfg, 8.0, 3000);
    CHECK(fabs(dl_start_ds(&c, 8.0) - 8.0) < 1e-9, "base 8 stays");
    printf(fails ? "%d FAILED\n" : "all sgdl tests passed\n", fails);
    return fails != 0;
}
