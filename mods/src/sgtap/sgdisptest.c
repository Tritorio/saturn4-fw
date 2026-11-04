/* Test for the LCD display command (sgdisp.c) without the real board: plain files stand in for the SPI device and the serial port.
 * Run (after mkdir -p /tmp/sgt /tmp/sgtl /tmp/sglcd) with
 *   SGTAP_FORCE=1 SGTAP_MATCH=/tmp/fakeser SGTAP_SPIMATCH=/tmp/fakespi SGTAP_DIR=/tmp/sgt SGTAP_LIVEDIR=/tmp/sgtl SGTAP_CSVDIR=/tmp/sgtcsv
 *   SGTAP_GAUGE=/dev/null SGTAP_LOG=/dev/null SGDISP_DIR=/tmp/sglcd LD_PRELOAD=./libsgtap.so ./sgdisptest
 * It writes cmd files like sgwebd does and prints the result of each command from live.json. The "board" file /tmp/fakespi gets the frames. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put_cmd(int n, const char *line)
{
    char p[96], t[96];
    FILE *f;
    snprintf(t, sizeof t, "/tmp/sgtl/.tmp%d", n);
    snprintf(p, sizeof p, "/tmp/sgtl/cmd.%013d", 1000 + n);
    f = fopen(t, "w");
    fprintf(f, "%s\n", line);
    fclose(f);
    rename(t, p);
}

static void show_res(const char *what)
{
    char b[16384], *q;
    FILE *f = fopen("/tmp/sgtl/live.json", "r");
    size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0;
    if (f)
        fclose(f);
    b[n] = 0;
    q = strstr(b, "\"cmd\":");
    printf("%-28s -> %.90s\n", what, q ? q : "(no live.json)");
    q = strstr(b, "\"lcd\":");
    printf("%-28s    %.230s\n", "", q ? q : "");
}

int main(void)
{
    unsigned char img[2048 + 48];
    int fd, ser, i;
    FILE *f;
    if (fork() > 0)
        return 0;
    setsid();
    ser = open("/tmp/fakeser", O_RDWR | O_CREAT | O_TRUNC, 0644);
    fd = open("/tmp/fakespi", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (ser < 0 || fd < 0)
        return 1;
    memset(img, 0, sizeof img);
    img[0] = 0x1d; img[1] = 0x01; img[2] = 0xfd; img[3] = 0x12;
    img[4] = sizeof img & 255; img[5] = sizeof img >> 8;
    for (i = 48; i < (int)sizeof img; i++)
        img[i] = (unsigned char)(i % 7 ? 0x48 : 0xfc);
    f = fopen("/tmp/sglcd/tiny.bin", "wb");
    fwrite(img, 1, sizeof img, f);
    fclose(f);
    sleep(3);
    put_cmd(1, "lcdshow tiny 0 3");  sleep(2); show_res("lcdshow tiny 0 3");
    sleep(4);                         show_res("(after the 3 s)");
    put_cmd(2, "lcdshow tiny 2 3");  sleep(2); show_res("lcdshow tiny 2 3");
    put_cmd(3, "lcdshow nosuch 0 3"); sleep(2); show_res("lcdshow nosuch 0 3");
    put_cmd(4, "lcdshow bad!name 0 3"); sleep(2); show_res("lcdshow bad!name 0 3");
    put_cmd(5, "lcdshow tiny 101 3");  sleep(2); show_res("lcdshow tiny 101 3");
    put_cmd(6, "lcdshow tiny 0 99"); sleep(2); show_res("lcdshow tiny 0 99");
    put_cmd(7, "lcdshow tiny 0 30"); sleep(2); show_res("lcdshow tiny 0 30");
    put_cmd(10, "lcdshow tiny 100 3"); sleep(2); show_res("lcdshow tiny 100 3 (100 % ok in test)");
    put_cmd(11, "lcdshow tiny 0 31"); sleep(2); show_res("lcdshow tiny 0 31 (too long)");
    put_cmd(12, "lcdswap tiny 0");   sleep(2); show_res("lcdswap tiny 0 (no display running: refused)");
    put_cmd(13, "lcdshow tiny 0 20"); sleep(2); show_res("lcdshow tiny 0 20");
    put_cmd(14, "lcdswap tiny 0");   sleep(2); show_res("lcdswap tiny 0");
    put_cmd(15, "lcdswap tiny 1");   sleep(2); show_res("lcdswap tiny 1");
    put_cmd(16, "lcdswap tiny 2");   sleep(2); show_res("lcdswap tiny 2 (bad)");
    put_cmd(17, "lcdswap nosuch 0"); sleep(2); show_res("lcdswap nosuch 0 (bad)");
    put_cmd(8, "lcdoff");            sleep(2); show_res("lcdoff");
    if (getenv("SGDISP_ALLOW")) {
        put_cmd(9, "lcdshow tiny 80 3"); sleep(2); show_res("lcdshow tiny 80 3 (allowed)");
        sleep(2);                       show_res("(after the 3 s: UV off, fan runs on)");
        sleep(12);                      show_res("(after the run-on)");
    }
    put_cmd(20, "lcdseq 0 tiny 0");      sleep(2); show_res("lcdseq 0 tiny 0 (bad secs)");
    put_cmd(21, "lcdseq 0 nosuch 1");    sleep(2); show_res("lcdseq nosuch (no file)");
    put_cmd(22, "lcdseq 0");             sleep(2); show_res("lcdseq 0 (no stage)");
    put_cmd(23, "lcdseq 101 tiny 1");    sleep(2); show_res("lcdseq 101 (bad UV)");
    put_cmd(24, "lcdseq 0 tiny 1 tiny 1.5 tiny 1"); sleep(2); show_res("lcdseq 0 tiny 1 1.5 1 (running)");
    put_cmd(25, "lcdshow tiny 0 3");     sleep(2); show_res("lcdshow during seq (refused)");
    sleep(3);                            show_res("(after the sequence)");
    put_cmd(26, "lcdseq 0 tiny 5 tiny 5"); sleep(3); put_cmd(27, "lcdoff"); sleep(2); show_res("lcdseq aborted by lcdoff");
    if (getenv("SGDISP_ALLOW")) {
        put_cmd(28, "lcdseq 5 tiny 1 tiny 1"); sleep(5); show_res("lcdseq 5 % (allowed)");
    } else {
        put_cmd(28, "lcdseq 5 tiny 1");  sleep(2); show_res("lcdseq 5 % without allow (refused)");
    }
    close(fd);
    return 0;
}
