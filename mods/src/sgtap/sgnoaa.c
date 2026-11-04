/*
 * sgnoaa.c - stand-ins for sgspi.c (board link tracer) and sgdisp.c (LCD image / fan / UV control), #included by sgtap.c in the
 * "normal" build variant (no -DSGTAP_AA). The AA variant ("anti-aliasing", display / fan / UV experiments) compiles the real modules.
 * The normal build never remembers the board's SPI fd, never traces it and answers the lcd commands with an error.
 */
static char spi_match[64] = "";
static volatile int spi_fd = -1;
#define spi_on 0

static void sgspi_log(int is_write, const void *buf, size_t n)
{
    (void)is_write;
    (void)buf;
    (void)n;
}

static void sgspi_tick(void)
{
}

static int sgspi_json(char *buf, size_t len)
{
    (void)buf;
    (void)len;
    return 0;
}

static void sgdisp_cmd(const char *c, char *res, size_t len)
{
    (void)c;
    snprintf(res, len, "error: not in this build (display / fan / UV control is in the AA variant)");
}

static void sgdisp_swap(const char *c, char *res, size_t len)
{
    sgdisp_cmd(c, res, len);
}

static void sgdisp_seq(const char *c, char *res, size_t len)
{
    sgdisp_cmd(c, res, len);
}

static void sgdisp_tick(void)
{
}

static int sgdisp_json(char *buf, size_t len)
{
    (void)buf;
    (void)len;
    return 0;
}

/* Multi Exposure (sgmx.c) is part of the AA variant only */
#define mx_cap 0
static void sgmx_capture(const void *buf, size_t n)
{
    (void)buf;
    (void)n;
}

static int sgmx_active(void)
{
    return 0;
}

static void sgmx_init(void)
{
}

static void sgmx_cmd(const char *c, char *res, size_t len)
{
    (void)c;
    snprintf(res, len, "error: not in this build (Multi Exposure is in the AA variant)");
}

static void sgmx_tick(void)
{
}

static int sgmx_json(char *buf, size_t len)
{
    (void)buf;
    (void)len;
    return 0;
}
