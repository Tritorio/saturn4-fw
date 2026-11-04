/* Test for the board link tracer (sgspi.c) without touching the real board: a plain file stands in for
 * /dev/spidev0.0. Run with
 *   SGTAP_FORCE=1 SGTAP_MATCH=/tmp/fakeser SGTAP_SPIMATCH=/tmp/fakespi SGTAP_DIR=/tmp/sgt LD_PRELOAD=./libsgtap.so ./sgspitest
 * (the tracked fake "serial port" /tmp/fakeser only makes the tap start its writer thread, as chitu's ttys do)
 * after "mkdir -p /tmp/sgt; touch /tmp/sgt/FPGATRACE". It daemonizes first, like chitu. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    unsigned char img[0x801], cmd[9] = {0x1a, 1, 2, 3, 4, 5, 6, 7, 8}, on = 0x29, off = 0x28, rd[16];
    int fd, i, ser;
    if (fork() > 0)
        return 0;
    setsid();
    ser = open("/tmp/fakeser", O_RDWR | O_CREAT | O_TRUNC, 0644);   /* starts the tap's writer thread */
    if (ser < 0)
        return 2;
    fd = open("/tmp/fakespi", O_RDWR | O_CREAT | O_TRUNC, 0644);   /* "board" */
    if (fd < 0)
        return 1;
    memset(img, 0x55, sizeof img);
    img[0] = 0x3c;
    sleep(2);                         /* let the tap thread see the switch and open its file */
    write(fd, cmd, sizeof cmd);
    for (i = 0; i < 12; i++)
        write(fd, img, sizeof img);   /* one image burst */
    usleep(300000);
    write(fd, &on, 1);
    usleep(200000);
    for (i = 0; i < 5; i++)
        write(fd, img, sizeof img);   /* second, shorter burst */
    write(fd, &off, 1);
    lseek(fd, 0, SEEK_SET);
    read(fd, rd, sizeof rd);          /* a read */
    sleep(3);                         /* let the tap thread drain */
    close(fd);
    return 0;
}
