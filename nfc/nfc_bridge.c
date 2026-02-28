#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

#define BAUD_RATE B115200
#define BUF_SIZE 256
#define PREFIX "PAYLOAD:"
#define PREFIX_LEN 8
#define MAX_ACM 10

int main(void)
{
    int fd = -1;
    char port[32];
    for (int i = 0; i < MAX_ACM; i++) {
        snprintf(port, sizeof(port), "/dev/ttyACM%d", i);
        fd = open(port, O_RDONLY | O_NOCTTY);
        if (fd >= 0) {
            fprintf(stderr, "Opened %s\n", port);
            break;
        }
    }
    if (fd < 0) {
        fprintf(stderr, "No /dev/ttyACM* port found\n");
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

    /* Wait for Arduino startup and check for PN532 detection.
       The Arduino prints "Found chip PN5..." on success or
       "Didn't find PN53x board" on failure, then halts. */
    char buf[BUF_SIZE];
    int pos = 0;
    int startup_timeout = 50; /* ~5 seconds (50 * 100ms VTIME reads) */
    int pn532_found = 0;

    while (startup_timeout > 0) {
        char c;
        int n = read(fd, &c, 1);
        if (n < 0) {
            fprintf(stderr, "read error: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        if (n == 0) {
            startup_timeout--;
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                buf[pos] = '\0';
                fprintf(stderr, "[Arduino] %s\n", buf);
                if (strstr(buf, "Didn't find PN53x")) {
                    fprintf(stderr, "ERROR: PN532 not detected on %s\n", port);
                    close(fd);
                    return 1;
                }
                if (strstr(buf, "Found chip PN5")) {
                    pn532_found = 1;
                }
                if (strstr(buf, "Waiting for")) {
                    break; /* Arduino is ready */
                }
                pos = 0;
            }
        } else if (pos < BUF_SIZE - 1) {
            buf[pos++] = c;
        }
    }

    if (!pn532_found) {
        fprintf(stderr, "WARNING: Did not see PN532 confirmation from Arduino\n");
    }

    fprintf(stderr, "Waiting for NFC cards...\n");
    pos = 0;

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
                if (strncmp(buf, PREFIX, PREFIX_LEN) == 0) {
                    printf("%s\n", buf + PREFIX_LEN);
                    fflush(stdout);
                }
                fprintf(stderr, "[Arduino] %s\n", buf);
                pos = 0;
            }
        } else if (pos < BUF_SIZE - 1) {
            buf[pos++] = c;
        }
    }

    close(fd);
    return 0;
}
