/* host test of sgmxmask.h: gcc -O2 -o /tmp/sgmxtest sgmxtest.c && /tmp/sgmxtest DUMP.bin OUTPREFIX
 * writes OUTPREFIX_<litmask>.bin for every litmask 0..15; tools/analysis/mx_mask_check.py decodes and checks them. */
#include <stdio.h>
#include <stdlib.h>
#include "sgmxmask.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static void plan_tests(void)
{
    double dt[4], tq1[4] = {5, 10, 15, 20}, tq2[4] = {3, 3, 8, 8}, tq3[4] = {20, 5, 5, 12};
    unsigned lit[4];
    int n = mxm_plan(tq1, 15, dt, lit);
    CHECK(n == 4 && dt[0] == 5 && dt[1] == 5 && dt[2] == 5 && dt[3] == 5, "plan 1: n %d", n);
    CHECK(lit[0] == 15 && lit[1] == 14 && lit[2] == 12 && lit[3] == 8, "plan 1 lit %x %x %x %x", lit[0], lit[1], lit[2], lit[3]);
    n = mxm_plan(tq2, 15, dt, lit);                       /* ties share a stage */
    CHECK(n == 2 && dt[0] == 3 && dt[1] == 5 && lit[0] == 15 && lit[1] == 12, "plan 2: n %d lit %x %x", n, lit[0], lit[1]);
    n = mxm_plan(tq3, 15, dt, lit);
    CHECK(n == 3 && dt[0] == 5 && dt[1] == 7 && dt[2] == 8 && lit[0] == 15 && lit[1] == 9 && lit[2] == 1, "plan 3: n %d lit %x %x %x", n, lit[0], lit[1], lit[2]);
    n = mxm_plan(tq1, 13, dt, lit);                       /* quadrant 1 (10 s) empty: no stage for it */
    CHECK(n == 3 && dt[0] == 5 && dt[1] == 10 && dt[2] == 5 && lit[0] == 13 && lit[1] == 12 && lit[2] == 8, "plan 4: n %d lit %x %x %x", n, lit[0], lit[1], lit[2]);
    n = mxm_plan(tq1, 0, dt, lit);
    CHECK(n == 0, "plan 5: n %d", n);
    n = mxm_plan(tq1, 8, dt, lit);                        /* only the 20 s quadrant: one stage, 20 s */
    CHECK(n == 1 && dt[0] == 20 && lit[0] == 8, "plan 6: n %d", n);
    CHECK(mxm_quad(0) == 0 && mxm_quad(1) == 1 && mxm_quad(6229) == 1 && mxm_quad(6230) == 2 && mxm_quad(12459) == 3 && mxm_quad(6231) == 3, "quad map");
    printf("plan tests: %s\n", fails ? "FAILED" : "ok");
}

int main(int argc, char **argv)
{
    plan_tests();
    if (fails)
        return 1;
    static uint32_t off[MXM_HALFROWS + 1];
    FILE *f;
    long n;
    uint8_t *s, *o;
    if (argc < 3) {
        puts("usage: sgmxtest DUMP.bin OUTPREFIX");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    s = malloc((size_t)n); o = malloc((size_t)n);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    if (mxm_parse(s, (size_t)n, off)) {
        puts("parse: REJECTED");
        return 1;
    }
    printf("parse: ok, tail %ld B, content mask %x\n", n - (long)off[MXM_HALFROWS], mxm_content(s, off));
    for (unsigned m = 0; m < 16; m++) {
        char fn[300];
        size_t k = mxm_build(s, (size_t)n, off, m, o);
        if (k > (size_t)n) { printf("mask %u: output longer (%zu > %ld)\n", m, k, n); return 1; }
        snprintf(fn, sizeof fn, "%s_%u.bin", argv[2], m);
        f = fopen(fn, "wb"); fwrite(o, 1, k, f); fclose(f);
        printf("mask %2u: %zu bytes\n", m, k);
    }
    return 0;
}
