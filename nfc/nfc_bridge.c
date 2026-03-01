#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <dirent.h>

#define BAUD_RATE B115200
#define BUF_SIZE 256
#define PREFIX "UID:"
#define PREFIX_LEN 4
#define MAX_ACM 10

static int try_open(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOCTTY);
    if (fd >= 0)
        fprintf(stderr, "Opened %s\n", path);
    return fd;
}

int main(void)
{
    int fd = -1;
    char port[64];

    /* Try Linux-style /dev/ttyACM* first (Arduino) */
    for (int i = 0; i < MAX_ACM && fd < 0; i++) {
        snprintf(port, sizeof(port), "/dev/ttyACM%d", i);
        fd = try_open(port);
    }

    /* Try macOS-style /dev/cu.usbmodem* (Pico / CircuitPython) */
    if (fd < 0) {
        DIR *dev = opendir("/dev");
        if (dev) {
            struct dirent *ent;
            while ((ent = readdir(dev)) != NULL) {
                if (strncmp(ent->d_name, "cu.usbmodem", 11) == 0) {
                    snprintf(port, sizeof(port), "/dev/%s", ent->d_name);
                    fd = try_open(port);
                    if (fd >= 0) break;
                }
            }
            closedir(dev);
        }
    }

    if (fd < 0) {
        fprintf(stderr, "No serial port found (tried /dev/ttyACM* and /dev/cu.usbmodem*)\n");
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

    /* Read lines from the device and forward PAYLOAD: lines to stdout.
       Works with both Arduino (which prints startup messages first) and
       Pico/CircuitPython (which may already be running when we connect). */
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
                /* Strip OSC escape sequences (ESC ] ... ESC \) from CircuitPython */
                char *line = buf;
                for (char *p = buf; *p; p++) {
                    if (p[0] == '\x1b' && p[1] == ']') {
                        /* Skip until ESC \ (ST) */
                        p += 2;
                        while (*p && !(p[0] == '\x1b' && p[1] == '\\')) p++;
                        if (*p) p++; /* skip past the backslash */
                        line = p + 1;
                    }
                }
                /* Strip leading and trailing whitespace */
                while (*line == ' ' || *line == '\t' || *line == '\0' || *line == '\n' || *line == '\r') line++;
                char *end = line + strlen(line) - 1;
                while (end > line && (*end == ' ' || *end == '\t' || *end == '\0' || *end == '\n' || *end == '\r')) end--;
                *(end + 1) = '\0';
                if (strncmp(line, PREFIX, PREFIX_LEN) == 0) {
                    printf("%s\n", line + PREFIX_LEN);
                    fflush(stdout);
                }
                if (strstr(line, "Didn't find PN53x")) {
                    fprintf(stderr, "WARNING: %s\n", line);
                    /* Don't exit — with multiple readers, the other may still work */
                }
                int lineLen = (int)strlen(line);
                fprintf(stderr, "[NFC] (%d) %s\n", lineLen, line);
                pos = 0;
            }
        } else if (pos < BUF_SIZE - 1) {
            buf[pos++] = c;
        }
    }

    close(fd);
    return 0;
}
