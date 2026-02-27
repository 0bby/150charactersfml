#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

#define SERIAL_PORT "/dev/ttyACM0"
#define BAUD_RATE B115200
#define BUF_SIZE 256

int main(void)
{
    int fd = open(SERIAL_PORT, O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", SERIAL_PORT, strerror(errno));
        return 1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "tcgetattr failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    cfsetispeed(&tty, BAUD_RATE);
    cfsetospeed(&tty, BAUD_RATE);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; /* 8-bit chars */
    tty.c_cflag &= ~(PARENB | CSTOPB);           /* no parity, 1 stop bit */
    tty.c_cflag |= CLOCAL | CREAD;               /* ignore modem, enable rx */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);      /* no sw flow control */
    tty.c_iflag &= ~(ICRNL | INLCR);             /* no CR/LF translation */
    tty.c_lflag = 0;                              /* raw input */
    tty.c_oflag = 0;                              /* raw output */
    tty.c_cc[VMIN]  = 0;                          /* non-blocking */
    tty.c_cc[VTIME] = 10;                         /* 1s timeout (tenths of sec) */

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "tcsetattr failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("Waiting for NFC cards...\n");

    char buf[BUF_SIZE];
    int pos = 0;

    while (1) {
        char c;
        int n = read(fd, &c, 1);
        if (n < 0) {
            fprintf(stderr, "read error: %s\n", strerror(errno));
            break;
        }
        if (n == 0)
            continue; /* timeout, no data */

        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                buf[pos] = '\0';
                printf("%s\n", buf);
                fflush(stdout);
                pos = 0;
            }
        } else if (pos < BUF_SIZE - 1) {
            buf[pos++] = c;
        }
    }

    close(fd);
    return 0;
}
