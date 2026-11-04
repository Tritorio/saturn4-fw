/* Test for libsgtap.so on the printer, without touching real serial ports:
 * a pty pair stands in for the UART; it daemonizes first, like chitu. Run with
 *   SGTAP_FORCE=1 SGTAP_MATCH=/dev/pts/ SGTAP_DIR=/tmp/sgtest LD_PRELOAD=./libsgtap.so ./sgtest */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    char buf[64];
    struct termios t;
    int i, m, s;
    /* daemonize like chitu (fork + setsid, parent exits): threads do not survive this */
    if (fork() > 0)
        return 0;
    setsid();
    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m))
        return 1;
    /* open + configure first, before the capture starts (chitu does the same) */
    s = open(ptsname(m), O_RDWR | O_NOCTTY);
    if (s < 0)
        return 2;
    tcgetattr(s, &t);
    cfmakeraw(&t);
    cfsetospeed(&t, B115200);
    cfsetispeed(&t, B115200);
    tcsetattr(s, TCSANOW, &t);
    sleep(2);                       /* let the tap thread see ENABLE and open its file */
    /* vfork child that closes the tty and execs, like chitu's command runner: the
     * parent's capture must survive this */
    if (vfork() == 0) {
        close(s);
        execl("/bin/true", "true", (char *)NULL);
        _exit(127);
    }
    wait(NULL);
    for (i = 0; i < 5; i++) {
        int n = snprintf(buf, sizeof buf, "TX%d", i);
        write(s, buf, n);                      /* app -> "board": captured as WRITE */
        read(m, buf, sizeof buf);
        n = snprintf(buf, sizeof buf, "RX%d", i);
        write(m, buf, n);                      /* "board" -> app: captured as READ */
        n = read(s, buf, sizeof buf);
        printf("got %.*s\n", n, buf);
        usleep(100000);
    }
    close(s);
    sleep(3);                       /* let the tap thread drain */
    printf("errno-check: read on bad fd -> %zd\n", read(-1, buf, 1));
    return 0;
}
