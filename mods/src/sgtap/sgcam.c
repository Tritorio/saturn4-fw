/* sgcam: camera relay of the printer's own RTSP server -> fragmented MP4 over HTTP, one viewer, printer idle only.
 *
 *   sgcam [-p HTTPPORT] [-u UDPPORT] [-r RTSPHOST] [-l LIVEDIR]
 *
 * It listens on HTTPPORT (default 8091) and does NOTHING with the camera until a viewer asks:
 *   GET /cam[?print=1]  the stream; without print=1 only while the printer is idle, with it also during a print (user: "let's try")
 *   (GET /cam         the stream (video/mp4, chunked): init segment, then one fragment per frame, from the first key frame on.
 *                    The answer headers carry X-Codec (avc1.PPCCLL) for MediaSource.addSourceBuffer.
 *   GET /status      {"idle":0|1,"busy":0}
 *   GET /            a small test page (MediaSource player)
 * Rules: without ?print=1 live only while the printer is idle (live.json job.active = 0, checked every second: a print
 * that starts ends the stream); with ?print=1 the stream also runs during a print; ONE viewer (a second one gets 409), the RTSP session is always closed with TEARDOWN
 * (viewer gone, print started, error, SIGTERM / SIGINT), nothing runs without a viewer.
 * The app's RTSP server (chitu, 127.0.0.1:554) only accepts "Transport: RTP/AVP/UDP;unicast;client_port=A-B" and sends UDP RTP
 * to that port; the packets are an Annex-B byte stream cut into fake FU-A packets (CAMERA): the bytes are
 * rebuilt, a frame ends when the RTP timestamp changes. Build: tools/build_sgcam.sh (printer glibc 2.28). */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static const char *g_live = "/tmp/sgweb";
static const char *g_rtsp = NULL;        /* default: the printer's own address as the viewer reached it (the loopback does not work on this printer) */
static int g_allow_print;               /* set per viewer from the request */
static int g_http = 8091, g_udp = 40100, g_ls = -1;

static int to_int(const char *s)       /* no atoi / strtol: GCC 15 maps them to __isoc23_* (missing in glibc 2.28) */
{
    int v = 0;
    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void on_sig(int s)
{
    (void)s;
    g_stop = 1;
}

/* 1 = a print job is active, 0 = idle, -1 = unknown (live.json missing / unreadable) */
static int printing(void)
{
    char path[200], buf[16384];
    FILE *f;
    size_t n;
    char *p;
    snprintf(path, sizeof path, "%s/live.json", g_live);
    if (!(f = fopen(path, "r")))
        return -1;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (!(p = strstr(buf, "\"job\":{\"active\":")))
        return -1;
    return p[16] == '1';
}

/* ---- growing buffer + MP4 boxes ---- */
typedef struct {
    uint8_t *p;
    size_t n, cap;
} buf_t;

static void bgrow(buf_t *b, size_t add)
{
    if (b->n + add > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + add)
            c *= 2;
        b->p = realloc(b->p, c);
        if (!b->p)
            abort();
        b->cap = c;
    }
}
static void b8(buf_t *b, unsigned v)
{
    bgrow(b, 1);
    b->p[b->n++] = (uint8_t)v;
}
static void b16(buf_t *b, unsigned v)
{
    b8(b, v >> 8);
    b8(b, v);
}
static void b32(buf_t *b, uint32_t v)
{
    b16(b, v >> 16);
    b16(b, v & 0xffff);
}
static void bcopy_(buf_t *b, const void *d, size_t n)
{
    bgrow(b, n);
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void bzero_n(buf_t *b, size_t n)
{
    while (n--)
        b8(b, 0);
}
static size_t box_begin(buf_t *b, const char *type)
{
    size_t pos = b->n;
    b32(b, 0);
    bcopy_(b, type, 4);
    return pos;
}
static void box_end(buf_t *b, size_t pos)
{
    uint32_t sz = (uint32_t)(b->n - pos);
    b->p[pos] = (uint8_t)(sz >> 24);
    b->p[pos + 1] = (uint8_t)(sz >> 16);
    b->p[pos + 2] = (uint8_t)(sz >> 8);
    b->p[pos + 3] = (uint8_t)sz;
}
static size_t full_box(buf_t *b, const char *type, unsigned ver, unsigned flags)
{
    size_t pos = box_begin(b, type);
    b8(b, ver);
    b8(b, flags >> 16);
    b8(b, flags >> 8);
    b8(b, flags);
    return pos;
}
static void matrix(buf_t *b)
{
    static const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (int i = 0; i < 9; i++)
        b32(b, m[i]);
}

/* ---- SPS parsing (picture size) ---- */
typedef struct {
    const uint8_t *d;
    size_t n, pos;
} bits_t;
static unsigned rbit(bits_t *b)
{
    unsigned v = 0;
    if (b->pos / 8 < b->n)
        v = (b->d[b->pos / 8] >> (7 - b->pos % 8)) & 1;
    b->pos++;
    return v;
}
static unsigned rbits(bits_t *b, int n)
{
    unsigned v = 0;
    while (n--)
        v = (v << 1) | rbit(b);
    return v;
}
static unsigned rue(bits_t *b)
{
    int z = 0;
    while (!rbit(b) && z < 32 && b->pos / 8 < b->n)
        z++;
    return z ? (1u << z) - 1 + rbits(b, z) : 0;
}

/* nal = SPS including its header byte; 1 = size found */
static int sps_size(const uint8_t *nal, size_t len, int *w, int *h)
{
    uint8_t rb[256];
    size_t n = 0;
    if (len < 5 || len > 255)
        return 0;
    for (size_t i = 1; i < len; i++) {                    /* remove emulation prevention bytes */
        if (n >= 2 && rb[n - 1] == 0 && rb[n - 2] == 0 && nal[i] == 3)
            continue;
        rb[n++] = nal[i];
    }
    bits_t b = {rb, n, 0};
    unsigned prof = rbits(&b, 8);
    rbits(&b, 8);
    rbits(&b, 8);
    rue(&b);
    unsigned chroma = 1;
    if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 || prof == 86 || prof == 118 || prof == 128) {
        chroma = rue(&b);
        if (chroma == 3)
            rbit(&b);
        rue(&b);
        rue(&b);
        rbit(&b);
        if (rbit(&b))
            return 0;                                      /* scaling matrices: not parsed */
    }
    rue(&b);
    unsigned poc = rue(&b);
    if (poc == 0)
        rue(&b);
    else if (poc == 1)
        return 0;
    rue(&b);
    rbit(&b);
    unsigned wm = rue(&b) + 1, hm = rue(&b) + 1, fmo = rbit(&b);
    if (!fmo)
        rbit(&b);
    rbit(&b);
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (rbit(&b)) {
        cl = rue(&b);
        cr = rue(&b);
        ct = rue(&b);
        cb = rue(&b);
    }
    unsigned cux = chroma == 0 || chroma == 3 ? 1 : 2, cuy = (chroma == 1 ? 2 : 1) * (2 - fmo);
    *w = (int)(wm * 16 - cux * (cl + cr));
    *h = (int)((2 - fmo) * hm * 16 - cuy * (ct + cb));
    return *w > 15 && *h > 15 && *w < 8192 && *h < 8192;
}

/* ---- the stream state ---- */
typedef struct {
    uint8_t sps[256], pps[256];
    int sps_n, pps_n, w, h;
    int init_sent;
    uint64_t dts;                                          /* decode time of the pending sample */
    uint32_t last_ts;
    int pend_n, pend_sync;                                 /* pending sample (waits for the next timestamp for its duration) */
    uint8_t *pend;
    size_t pend_cap;
    uint32_t seq;
    unsigned long frames, keys;
    int client;
    char codec[16];
    int failed;
} cam_t;

static int send_all(int fd, const void *d, size_t n)
{
    const uint8_t *p = d;
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static int send_chunk(int fd, const void *d, size_t n)
{
    char h[24];
    int hl = snprintf(h, sizeof h, "%zx\r\n", n);
    return send_all(fd, h, (size_t)hl) && send_all(fd, d, n) && send_all(fd, "\r\n", 2);
}

static void build_init(cam_t *c, buf_t *o)
{
    size_t ftyp = box_begin(o, "ftyp"), moov, t, tk, md, mi, st, sd, av, ac;
    bcopy_(o, "isom", 4);
    b32(o, 512);
    bcopy_(o, "isomiso5avc1mp41", 16);
    box_end(o, ftyp);
    moov = box_begin(o, "moov");
    t = full_box(o, "mvhd", 0, 0);
    b32(o, 0); b32(o, 0); b32(o, 90000); b32(o, 0); b32(o, 0x00010000); b16(o, 0x0100); bzero_n(o, 10);
    matrix(o);
    bzero_n(o, 24);
    b32(o, 2);
    box_end(o, t);
    tk = box_begin(o, "trak");
    t = full_box(o, "tkhd", 0, 3);
    b32(o, 0); b32(o, 0); b32(o, 1); b32(o, 0); b32(o, 0); bzero_n(o, 8); b16(o, 0); b16(o, 0); b16(o, 0); b16(o, 0);
    matrix(o);
    b32(o, (uint32_t)c->w << 16); b32(o, (uint32_t)c->h << 16);
    box_end(o, t);
    md = box_begin(o, "mdia");
    t = full_box(o, "mdhd", 0, 0);
    b32(o, 0); b32(o, 0); b32(o, 90000); b32(o, 0); b16(o, 0x55c4); b16(o, 0);
    box_end(o, t);
    t = full_box(o, "hdlr", 0, 0);
    b32(o, 0); bcopy_(o, "vide", 4); bzero_n(o, 12); bcopy_(o, "Video\0", 6);
    box_end(o, t);
    mi = box_begin(o, "minf");
    t = full_box(o, "vmhd", 0, 1);
    b16(o, 0); bzero_n(o, 6);
    box_end(o, t);
    t = box_begin(o, "dinf");
    {
        size_t dr = full_box(o, "dref", 0, 0), u;
        b32(o, 1);
        u = full_box(o, "url ", 0, 1);
        box_end(o, u);
        box_end(o, dr);
    }
    box_end(o, t);
    st = box_begin(o, "stbl");
    sd = full_box(o, "stsd", 0, 0);
    b32(o, 1);
    av = box_begin(o, "avc1");
    bzero_n(o, 6); b16(o, 1); bzero_n(o, 16);
    b16(o, (unsigned)c->w); b16(o, (unsigned)c->h);
    b32(o, 0x00480000); b32(o, 0x00480000); b32(o, 0); b16(o, 1);
    bzero_n(o, 32);
    b16(o, 0x0018); b16(o, 0xffff);
    ac = box_begin(o, "avcC");
    b8(o, 1); b8(o, c->sps[1]); b8(o, c->sps[2]); b8(o, c->sps[3]); b8(o, 0xff);
    b8(o, 0xe1); b16(o, (unsigned)c->sps_n); bcopy_(o, c->sps, (size_t)c->sps_n);
    b8(o, 1); b16(o, (unsigned)c->pps_n); bcopy_(o, c->pps, (size_t)c->pps_n);
    box_end(o, ac);
    box_end(o, av);
    box_end(o, sd);
    {
        static const char *const empty[] = {"stts", "stsc", "stco"};
        for (int i = 0; i < 3; i++) {
            t = full_box(o, empty[i], 0, 0);
            b32(o, 0);
            box_end(o, t);
        }
    }
    t = full_box(o, "stsz", 0, 0);
    b32(o, 0); b32(o, 0);
    box_end(o, t);
    box_end(o, st);
    box_end(o, mi);
    box_end(o, md);
    box_end(o, tk);
    t = box_begin(o, "mvex");
    {
        size_t tr = full_box(o, "trex", 0, 0);
        b32(o, 1); b32(o, 1); b32(o, 0); b32(o, 0); b32(o, 0);
        box_end(o, tr);
    }
    box_end(o, t);
    box_end(o, moov);
}

/* one fragment for the pending sample with the given duration */
static int emit_fragment(cam_t *c, uint32_t dur)
{
    buf_t o = {0};
    size_t moof = box_begin(&o, "moof"), t, traf, off_pos, md;
    t = full_box(&o, "mfhd", 0, 0);
    b32(&o, ++c->seq);
    box_end(&o, t);
    traf = box_begin(&o, "traf");
    t = full_box(&o, "tfhd", 0, 0x020000);
    b32(&o, 1);
    box_end(&o, t);
    t = full_box(&o, "tfdt", 1, 0);
    b32(&o, (uint32_t)(c->dts >> 32)); b32(&o, (uint32_t)c->dts);
    box_end(&o, t);
    t = full_box(&o, "trun", 0, 0x000701);
    b32(&o, 1);
    off_pos = o.n;
    b32(&o, 0);
    b32(&o, dur);
    b32(&o, (uint32_t)c->pend_n);
    b32(&o, c->pend_sync ? 0x02000000u : 0x01010000u);
    box_end(&o, t);
    box_end(&o, traf);
    box_end(&o, moof);
    uint32_t doff = (uint32_t)(o.n + 8);
    o.p[off_pos] = (uint8_t)(doff >> 24); o.p[off_pos + 1] = (uint8_t)(doff >> 16);
    o.p[off_pos + 2] = (uint8_t)(doff >> 8); o.p[off_pos + 3] = (uint8_t)doff;
    md = box_begin(&o, "mdat");
    bcopy_(&o, c->pend, (size_t)c->pend_n);
    box_end(&o, md);
    int ok = send_chunk(c->client, o.p, o.n);
    free(o.p);
    c->dts += dur;
    c->frames++;
    return ok;
}

/* a complete access unit (Annex-B bytes): SPS / PPS are kept, the slices become one AVCC sample */
static void on_frame(cam_t *c, const uint8_t *d, size_t n, uint32_t ts, int corrupt)
{
    uint8_t *sample = malloc(n + 16);
    size_t sn = 0, i = 0;
    int idr = 0, vcl = 0;
    if (!sample)
        return;
    while (i + 3 < n) {
        size_t sc = 0;
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
            sc = 3;
        else if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1)
            sc = 4;
        if (!sc) {
            i++;
            continue;
        }
        size_t s = i + sc, e = s;
        while (e + 2 < n && !(d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (d[e + 2] == 0 && e + 3 < n && d[e + 3] == 1))))
            e++;
        if (e + 2 >= n)
            e = n;
        if (e > s) {
            int type = d[s] & 0x1f;
            size_t len = e - s;
            if (type == 7 && len < sizeof c->sps) {
                if (!c->init_sent) { memcpy(c->sps, d + s, len); c->sps_n = (int)len; }
            } else if (type == 8 && len < sizeof c->pps) {
                if (!c->init_sent) { memcpy(c->pps, d + s, len); c->pps_n = (int)len; }
            } else if (type == 5 || type == 1) {
                sample[sn++] = (uint8_t)(len >> 24); sample[sn++] = (uint8_t)(len >> 16);
                sample[sn++] = (uint8_t)(len >> 8); sample[sn++] = (uint8_t)len;
                memcpy(sample + sn, d + s, len);
                sn += len;
                vcl = 1;
                if (type == 5)
                    idr = 1;
            }
        }
        i = e > i ? e : i + 1;
    }
    if (!vcl || (corrupt && idr)) {
        free(sample);
        return;
    }
    if (!c->init_sent) {                                   /* wait for the first key frame with SPS + PPS known */
        if (!idr || !c->sps_n || !c->pps_n) {
            free(sample);
            return;
        }
        if (!sps_size(c->sps, (size_t)c->sps_n, &c->w, &c->h)) {
            c->w = 1280;
            c->h = 720;
        }
        snprintf(c->codec, sizeof c->codec, "avc1.%02x%02x%02x", c->sps[1], c->sps[2], c->sps[3]);
        c->init_sent = 1;                                  /* the HTTP headers + init segment go out from the caller */
    }
    if (c->pend_n) {                                       /* the pending sample's duration = this timestamp - its own */
        uint32_t dur = ts - c->last_ts;
        if (dur == 0 || dur > 90000)
            dur = 3000;
        if (!emit_fragment(c, dur))
            c->failed = 1;
    }
    if (sn > c->pend_cap) {
        c->pend = realloc(c->pend, sn);
        c->pend_cap = sn;
    }
    memcpy(c->pend, sample, sn);
    c->pend_n = (int)sn;
    c->pend_sync = idr;
    c->last_ts = ts;
    if (idr)
        c->keys++;
    free(sample);
}

/* ---- RTSP client ---- */
static int rtsp_cmd(int fd, const char *method, const char *url, int cseq, const char *sess, const char *hdr, char *resp, size_t rn)
{
    char req[600];
    int n;
    if (sess && sess[0])
        n = snprintf(req, sizeof req, "%s %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n%s\r\n", method, url, cseq, sess, hdr);
    else
        n = snprintf(req, sizeof req, "%s %s RTSP/1.0\r\nCSeq: %d\r\n%s\r\n", method, url, cseq, hdr);
    if (!send_all(fd, req, (size_t)n))
        return 0;
    fprintf(stderr, "sgcam: -> %s\n", method);
    size_t got = 0;
    double t0 = now_s();
    resp[0] = 0;
    while (now_s() - t0 < 3.0 && got < rn - 1) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 200) <= 0)
            continue;
        ssize_t r = recv(fd, resp + got, rn - 1 - got, 0);
        if (r <= 0)
            return 0;
        got += (size_t)r;
        resp[got] = 0;
        char *he = strstr(resp, "\r\n\r\n");
        if (he) {
            char *cl = strcasestr(resp, "Content-Length:");
            size_t body = cl && cl < he ? (size_t)to_int(cl + 15) : 0;
            if (got >= (size_t)(he - resp) + 4 + body) {
                char *eol = strchr(resp, '\r');
                fprintf(stderr, "sgcam: <- %.*s\n", eol ? (int)(eol - resp) : 40, resp);
                return 1;
            }
        }
    }
    return strstr(resp, "\r\n\r\n") != NULL;
}

static void fail(int cli, const char *line, const char *msg)
{
    char h[300];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n%s", line, strlen(msg), msg);
    send_all(cli, h, (size_t)n);
}

static int read_request(int fd, char *req, size_t n, double maxs);
static void http_reply(int fd, int code, const char *type, const char *body);

/* a second viewer while one is streaming: 409 at once (checked from the stream loop); /status still answers (busy = 1) */
static void reject_others(void)
{
    struct pollfd pf = {g_ls, POLLIN, 0};
    while (poll(&pf, 1, 0) > 0) {
        int x = accept(g_ls, NULL, NULL);
        char rq[512] = "";
        if (x < 0)
            break;
        struct timeval tv = {1, 0};
        setsockopt(x, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (read_request(x, rq, sizeof rq, 0.25) && strncmp(rq, "GET /status ", 12) == 0)
            http_reply(x, 200, "application/json", "{\"idle\":0,\"busy\":1}");
        else if (strncmp(rq, "OPTIONS ", 8) == 0)
            http_reply(x, 204, "text/plain", "");
        else
            fail(x, "409 Conflict", "another viewer is already watching");
        close(x);
    }
}

static void serve_stream(int cli)
{
    cam_t c;
    char url[80], turl[100], resp[2048], sess[80] = "", hdr[160];
    int rt = -1, us = -1, cseq = 1, playing = 0;
    uint8_t *frame = NULL;
    size_t fn = 0, fcap = 0;
    memset(&c, 0, sizeof c);
    c.client = cli;
    char host[48];
    if (g_rtsp) {
        snprintf(host, sizeof host, "%s", g_rtsp);
    } else {
        struct sockaddr_in la;
        socklen_t ll = sizeof la;
        if (getsockname(cli, (struct sockaddr *)&la, &ll) != 0 || !inet_ntop(AF_INET, &la.sin_addr, host, sizeof host))
            snprintf(host, sizeof host, "127.0.0.1");
    }
    snprintf(url, sizeof url, "rtsp://%s:554/video", host);
    us = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int up = g_udp, bound = 0;
    for (int k = 0; k < 20 && !bound; k++, up += 2) {
        a.sin_port = htons((uint16_t)up);
        bound = bind(us, (struct sockaddr *)&a, sizeof a) == 0;
    }
    up -= 2;
    int rcvbuf = 1 << 20;
    setsockopt(us, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    if (!bound) {
        fail(cli, "500 Internal Server Error", "no free UDP port");
        goto out;
    }
    fprintf(stderr, "sgcam: viewer: udp port %d\n", up);
    rt = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ra = {0};
    ra.sin_family = AF_INET;
    ra.sin_port = htons(554);
    inet_pton(AF_INET, host, &ra.sin_addr);
    int cr = connect(rt, (struct sockaddr *)&ra, sizeof ra);
    fprintf(stderr, "sgcam: connect %d\n", cr);
    if (cr != 0 || !rtsp_cmd(rt, "OPTIONS", url, cseq++, NULL, "", resp, sizeof resp) ||
        !rtsp_cmd(rt, "DESCRIBE", url, cseq++, NULL, "Accept: application/sdp\r\n", resp, sizeof resp) || !strstr(resp, "H264")) {
        fail(cli, "502 Bad Gateway", "camera server did not answer");
        goto out;
    }
    snprintf(hdr, sizeof hdr, "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d\r\n", up, up + 1);
    snprintf(turl, sizeof turl, "%s/track0", url);
    if (!rtsp_cmd(rt, "SETUP", turl, cseq++, NULL, hdr, resp, sizeof resp) || !strstr(resp, " 200")) {
        fail(cli, "502 Bad Gateway", "SETUP failed");
        goto out;
    }
    char *s = strcasestr(resp, "Session:");
    if (s) {
        s += 8;
        while (*s == ' ')
            s++;
        size_t k = 0;
        while (s[k] && s[k] != '\r' && s[k] != ';' && k < sizeof sess - 1) {
            sess[k] = s[k];
            k++;
        }
        sess[k] = 0;
    }
    playing = 1;                                           /* from here a TEARDOWN is owed, whatever PLAY answers */
    if (!rtsp_cmd(rt, "PLAY", url, cseq++, sess, "", resp, sizeof resp)) {
        fail(cli, "502 Bad Gateway", "PLAY failed");
        goto out;
    }
    double t_play = now_s(), t_chk = t_play, t_last_rx = t_play;
    int hdr_sent = 0, lastseq = -1, corrupt = 0, haveTs = 0;
    uint32_t lastTs = 0;
    unsigned long lost = 0, pk = 0;
    uint8_t dg[2048];
    while (!g_stop && !c.failed) {
        double t = now_s();
        reject_others();
        if (t - t_chk >= 1.0) {                            /* idle only; a viewer that closed the page is seen by the send failing (30 fps) */
            t_chk = t;
            if (!g_allow_print && printing() != 0) {
                fprintf(stderr, "sgcam: printer not idle: stream ends\n");
                break;
            }
        }
        if (!hdr_sent && t - t_play > 8.0) {
            fail(cli, "504 Gateway Timeout", "no key frame from the camera in 8 s");
            break;
        }
        if (t - t_last_rx > 5.0) {
            fprintf(stderr, "sgcam: no packets for 5 s: stream ends\n");
            if (!hdr_sent)
                fail(cli, "504 Gateway Timeout", "the camera sends nothing");
            break;
        }
        struct pollfd pf = {us, POLLIN, 0};
        if (poll(&pf, 1, 200) <= 0)
            continue;
        ssize_t r = recv(us, dg, sizeof dg, 0);
        if (r < 14)
            continue;
        t_last_rx = t;
        pk++;
        int seq = (dg[2] << 8) | dg[3];
        if (lastseq >= 0 && seq != ((lastseq + 1) & 0xffff)) {
            lost += (unsigned long)((seq - lastseq - 1) & 0xffff);
            corrupt = 1;
        }
        lastseq = seq;
        uint32_t ts = ((uint32_t)dg[4] << 24) | ((uint32_t)dg[5] << 16) | ((uint32_t)dg[6] << 8) | dg[7];
        if (haveTs && ts != lastTs) {
            on_frame(&c, frame, fn, lastTs, corrupt);
            fn = 0;
            corrupt = 0;
            if (c.init_sent && !hdr_sent) {                /* first key frame seen: headers + init segment */
                char h[400];
                int hl = snprintf(h, sizeof h,
                                  "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nTransfer-Encoding: chunked\r\nCache-Control: no-store\r\n"
                                  "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Codec, X-Size\r\nX-Codec: %s\r\nX-Size: %dx%d\r\n"
                                  "Connection: close\r\n\r\n",
                                  c.codec, c.w, c.h);
                buf_t init = {0};
                build_init(&c, &init);
                int ok = send_all(cli, h, (size_t)hl) && send_chunk(cli, init.p, init.n);
                free(init.p);
                if (!ok)
                    break;
                hdr_sent = 1;
                fprintf(stderr, "sgcam: stream started (%s %dx%d) %.2f s after PLAY\n", c.codec, c.w, c.h, now_s() - t_play);
            }
        }
        lastTs = ts;
        haveTs = 1;
        int off = 12 + 4 * (dg[0] & 15);
        const uint8_t *src = dg + off;
        size_t add = (size_t)r - (size_t)off;
        int first = 0;
        if (dg[off] == 0x1c && r > off + 2) {              /* the app's fake FU-A: rebuild the original byte stream */
            first = (dg[off + 1] & 0x80) != 0;
            src = dg + off + 2;
            add -= 2;
        }
        if (fn + add + 1 > fcap) {
            if (fn + add + 1 > ((size_t)1 << 21)) {        /* a frame above 2 MB cannot be right */
                fn = 0;
                continue;
            }
            fcap = (fn + add + 1) * 2;
            frame = realloc(frame, fcap);
            if (!frame)
                abort();
        }
        if (first)
            frame[fn++] = (uint8_t)((dg[off] & 0xe0) | (dg[off + 1] & 0x1f));
        memcpy(frame + fn, src, add);
        fn += add;
    }
    fprintf(stderr, "sgcam: viewer done: %lu frames, %lu key frames, %lu packets, %lu lost\n", c.frames, c.keys, pk, lost);
out:
    if (rt >= 0) {
        if (playing) {                                     /* ALWAYS close the camera session */
            rtsp_cmd(rt, "TEARDOWN", url, cseq++, sess, "", resp, sizeof resp);
            fprintf(stderr, "sgcam: TEARDOWN sent\n");
        }
        close(rt);
    }
    if (us >= 0)
        close(us);
    free(frame);
    free(c.pend);
}

/* ---- HTTP front ---- */
static const char PAGE[] =
    "<!doctype html><meta charset=utf-8><title>sgcam</title><body style=\"background:#000;color:#ccc;font:14px sans-serif\">"
    "<video id=v autoplay muted playsinline controls style=\"max-width:100%\"></video><div id=m>connecting...</div><script>"
    "const v=document.getElementById('v'),m=document.getElementById('m');"
    "(async()=>{const r=await fetch('/cam');if(!r.ok){m.textContent='error '+r.status+' '+await r.text();return}"
    "const codec=r.headers.get('X-Codec'),ms=new MediaSource();v.src=URL.createObjectURL(ms);await new Promise(o=>ms.addEventListener('sourceopen',o,{once:true}));"
    "const sb=ms.addSourceBuffer('video/mp4; codecs=\"'+codec+'\"'),q=[];let busy=false;"
    "const pump=()=>{if(busy||!q.length||sb.updating)return;busy=true;try{sb.appendBuffer(q.shift())}catch(e){busy=false;m.textContent='append: '+e}};"
    "sb.addEventListener('updateend',()=>{busy=false;if(v.buffered.length){const e=v.buffered.end(v.buffered.length-1),s=v.buffered.start(0);"
    "if(e-v.currentTime>1.5)v.currentTime=e-0.3;if(e-s>20&&!sb.updating){try{sb.remove(s,e-10)}catch(x){}}}pump()});"
    "const rd=r.body.getReader();m.textContent=codec;for(;;){const{done,value}=await rd.read();if(done)break;q.push(value);pump()}m.textContent='ended'})();"
    "</script>";

static void http_reply(int fd, int code, const char *type, const char *body)
{
    char h[300];
    int hl = snprintf(h, sizeof h, "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", code, type,
                      strlen(body));
    send_all(fd, h, (size_t)hl);
    send_all(fd, body, strlen(body));
}

static int read_request(int fd, char *req, size_t n, double maxs)
{
    size_t got = 0;
    double t0 = now_s();
    while (now_s() - t0 < maxs && got < n - 1) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 200) <= 0)
            continue;
        ssize_t r = recv(fd, req + got, n - 1 - got, 0);
        if (r <= 0)
            return 0;
        got += (size_t)r;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n"))
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int ch;
    while ((ch = getopt(argc, argv, "p:u:r:l:")) != -1) {
        if (ch == 'p')
            g_http = to_int(optarg);
        else if (ch == 'u')
            g_udp = to_int(optarg);
        else if (ch == 'r')
            g_rtsp = optarg;
        else if (ch == 'l')
            g_live = optarg;
        else
            return 2;
    }
    struct sigaction sa = {0};
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    g_ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)g_http);
    if (bind(g_ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(g_ls, 4) != 0) {
        perror("sgcam: listen");
        return 1;
    }
    fprintf(stderr, "sgcam: listening on %d\n", g_http);
    while (!g_stop) {
        struct pollfd pf = {g_ls, POLLIN, 0};
        if (poll(&pf, 1, 500) <= 0)
            continue;
        int cli = accept(g_ls, NULL, NULL);
        if (cli < 0)
            continue;
        struct timeval tv = {3, 0};
        setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        char req[1024];
        if (read_request(cli, req, sizeof req, 2.0)) {
            if (strncmp(req, "OPTIONS ", 8) == 0) {
                http_reply(cli, 204, "text/plain", "");
            } else if (strncmp(req, "GET /cam ", 9) == 0 || strncmp(req, "GET /cam?", 9) == 0) {
                int pr = printing();
                g_allow_print = strncmp(req, "GET /cam?print=1 ", 17) == 0;
                if (!g_allow_print && pr != 0)
                    http_reply(cli, 503, "text/plain", pr < 0 ? "printer state unknown" : "printer is printing: live view only when idle");
                else
                    serve_stream(cli);
            } else if (strncmp(req, "GET /status ", 12) == 0) {
                char j[60];
                snprintf(j, sizeof j, "{\"idle\":%d,\"busy\":0}", printing() == 0);
                http_reply(cli, 200, "application/json", j);
            } else if (strncmp(req, "GET / ", 6) == 0) {
                http_reply(cli, 200, "text/html", PAGE);
            } else {
                http_reply(cli, 404, "text/plain", "not found");
            }
        }
        shutdown(cli, SHUT_RDWR);
        close(cli);
    }
    return 0;
}
