/*
 * sgmxmask.h - MULTI EXPOSURE stream masker (pure functions, no printer access; host test: sgmxtest.c + tools/analysis/mx_mask_check.py).
 * #included by sgmx.c. The layer image the app sends to the display board is a compressed stream of 12460 HALF ROWS of exactly 7560 px
 * (a 15120 px row = left half, right half; 6230 rows): per half row one value byte (bit 7 set, = ONE pixel) followed by run bytes
 * (1..72, bit 7 clear, each adds that many pixels of the same value); a new value byte starts when the gray value changes. The
 * quadrants of the screen are whole half rows (split x = 7560, y = row 3115), so a quadrant is blanked by replacing its half rows by a
 * black half row: 0x80, 104 x 72, 71 (= 1 + 7488 + 71 = 7560 px, 106 bytes). The minimum size of any half row is 106 bytes, so the
 * masked stream is never longer than the original. Quadrant numbers as the stream reaches the board: 0 TL, 1 TR, 2 BL, 3 BR (the
 * mirror switches of the card map them to the slicer view).
 */
#ifndef SGMXMASK_H
#define SGMXMASK_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MXM_HALFROWS 12460u
#define MXM_HALFPX   7560u
#define MXM_BLACKLEN 106u
#define MXM_MAXRUN   72u

/* half row h -> quadrant 0..3 (0 TL, 1 TR, 2 BL, 3 BR) */
static inline unsigned mxm_quad(unsigned h)
{
    return ((h >> 1) >= MXM_HALFROWS / 4u ? 2u : 0u) + (h & 1u);
}

/* Strict parse: off[0..12460] = start of every half row, off[12460] = end of the last one. Returns 0 if the stream is exactly 12460
 * half rows of 7560 px, each starting with a value byte, with run bytes 1..127 (the app's streams only use <= 72; a longer run is accepted
 * here because the masker copies it unchanged); -1 otherwise. The tail after the last half row (zero padding) is left to the caller. */
static int mxm_parse(const uint8_t *s, size_t n, uint32_t *off)
{
    size_t i = 0;
    for (unsigned h = 0; h < MXM_HALFROWS; h++) {
        unsigned px = 1;
        if (i >= n || s[i] < 0x80)
            return -1;
        off[h] = (uint32_t)i;
        i++;
        while (px < MXM_HALFPX) {
            if (i >= n)
                return -1;
            if (s[i] >= 0x80)
                px += 1;
            else if (s[i] == 0)
                return -1;
            else
                px += s[i];
            i++;
        }
        if (px != MXM_HALFPX)
            return -1;
        if (i < n && s[i] < 0x80 && s[i] != 0)     /* a run byte straight after a complete half row = misparse */
            return -1;
    }
    off[MXM_HALFROWS] = (uint32_t)i;
    return 0;
}

static size_t mxm_black(uint8_t *o)
{
    o[0] = 0x80;
    memset(o + 1, (int)MXM_MAXRUN, 104);
    o[105] = 71;
    return MXM_BLACKLEN;
}

/* Build the stage stream: litmask bit q (q = 0..3) = quadrant q keeps its pixels, the others become black. out needs n bytes.
 * Returns the output length (<= n). The zero tail of the input is copied unchanged. off = result of mxm_parse. */
static size_t mxm_build(const uint8_t *s, size_t n, const uint32_t *off, unsigned litmask, uint8_t *out)
{
    size_t o = 0;
    for (unsigned h = 0; h < MXM_HALFROWS; h++) {
        size_t len = off[h + 1] - off[h];
        if (litmask & (1u << mxm_quad(h))) {
            memcpy(out + o, s + off[h], len);
            o += len;
        } else {
            o += mxm_black(out + o);
        }
    }
    memcpy(out + o, s + off[MXM_HALFROWS], n - off[MXM_HALFROWS]);
    o += n - off[MXM_HALFROWS];
    return o;
}

/* Which quadrants have any non-black pixel in this layer (bit q): a quadrant that is black everywhere in the layer gets no stage. A half row
 * is black when it is one value byte 0x80 followed only by run bytes (all pixels = 0x80). */
static unsigned mxm_content(const uint8_t *s, const uint32_t *off)
{
    unsigned m = 0;
    for (unsigned h = 0; h < MXM_HALFROWS; h++) {
        unsigned q = mxm_quad(h);
        if (m & (1u << q))
            continue;
        for (uint32_t i = off[h]; i < off[h + 1]; i++)
            if (s[i] >= 0x80 && s[i] != 0x80) {
                m |= 1u << q;
                break;
            }
    }
    return m;
}

/* stages of the quadrants in `content` with the per-stream-quadrant times tq (s): dt[k] = UV-on of stage k, lit[k] = stream quadrants shown */
static inline int mxm_plan(const double tq[4], unsigned content, double dt[4], unsigned lit[4])
{
    int ord[4], na = 0, n = 0;
    double prev = 0;
    for (int q = 0; q < 4; q++)
        if (content >> q & 1)
            ord[na++] = q;
    for (int i = 1; i < na; i++)
        for (int j = i; j > 0 && tq[ord[j]] < tq[ord[j - 1]]; j--) {
            int t = ord[j];
            ord[j] = ord[j - 1];
            ord[j - 1] = t;
        }
    for (int i = 0; i < na; i++) {
        double t = tq[ord[i]];
        if (t > prev + 1e-9) {
            unsigned m = 0;
            for (int j = i; j < na; j++)
                m |= 1u << ord[j];
            dt[n] = t - prev;
            lit[n] = m;
            n++;
            prev = t;
        }
    }
    return n;
}
#endif
