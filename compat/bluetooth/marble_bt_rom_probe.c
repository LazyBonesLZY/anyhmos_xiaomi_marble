/* SPDX-License-Identifier: MIT */
/* Separate Marble/QCA6490 diagnostic. One ROM query at the initial baud;
 * no Reset, controller baud switch, firmware transfer, address writes,
 * H4 socket, scanning, or HDI startup. The caller must exclude other owners. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* marble kernel: include/linux/btpower.h, include/uapi/linux/msm_geni_serial.h */
enum { BT_POWER = 0xbfad, BT_CHIP_ID = 0xbfaf,
       UART_PM_GET = 0x54ed, UART_PM_PUT = 0x54ee, UART_PM_ACTIVE = 0x54ef };
static volatile sig_atomic_t cancelled;

struct rom_version {
    uint32_t product, soc;
    uint16_t patch, rom;
    const char *format;
};

static void cancel(int signal_number)
{
    (void)signal_number;
    cancelled = 1;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static int fail(const char *stage)
{
    int error = errno;
    fprintf(stderr, "BT_ROM_FAIL stage=%s errno=%d (%s)\n", stage, error, strerror(error));
    return 1;
}

static int io_exact(int fd, uint8_t *data, size_t length, int64_t deadline, bool tx)
{
    while (length) {
        if (cancelled) { errno = EINTR; return -1; }
        int64_t time = now_ms();
        if (time < 0) return -1;
        if (time >= deadline) { errno = ETIMEDOUT; return -1; }
        struct pollfd pfd = { .fd = fd, .events = tx ? POLLOUT : POLLIN };
        int ready = poll(&pfd, 1, (int)(deadline - time));
        if (ready == 0) { errno = ETIMEDOUT; return -1; }
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!(pfd.revents & pfd.events)) { errno = EIO; return -1; }
        ssize_t n = tx ? write(fd, data, length) : read(fd, data, length);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -1;
        }
        if (!n) { errno = EIO; return -1; }
        data += n;
        length -= (size_t)n;
    }
    return 0;
}

static int parse_version(struct rom_version *v, const uint8_t *data, const char *format)
{
    *v = (struct rom_version){
        .product = get32(data), .patch = get16(data + 4),
        .rom = get16(data + 6), .soc = get32(data + 8), .format = format,
    };
    if (v->soc && v->soc != UINT32_MAX && v->product != UINT32_MAX && v->rom != UINT16_MAX) return 0;
    errno = EPROTO;
    return -1;
}

/* Stock HAL SHA 39cde925… uses both full legacy VSE followed by CC and
 * unified CC with an extra subtype byte. Neither a plain CC nor a raw
 * readable stream counts as a version. All events share one deadline. */
static int query_version(int fd, int timeout_ms, struct rom_version *version)
{
    int64_t start = now_ms();
    if (start < 0) return -1;
    int64_t deadline = start + timeout_ms;
    uint8_t request[] = {0x01, 0x00, 0xfc, 0x01, 0x19};
    if (io_exact(fd, request, sizeof(request), deadline, true) < 0) return -1;
    puts("BT_ROM_QUERY_SENT=1 opcode=0xfc00 sub=0x19");
    bool legacy = false;
    unsigned frames = 0;
    for (;;) {
        uint8_t header[3], event[255];
        if (io_exact(fd, header, 1, deadline, false) < 0) return -1;
        if (header[0] != 0x04) {
            printf("BT_ROM_UNEXPECTED_H4_TYPE=0x%02x\n", (unsigned)header[0]);
            errno = EPROTO;
            return -1;
        }
        if (io_exact(fd, header + 1, 2, deadline, false) < 0) return -1;
        size_t size = header[2];
        if (io_exact(fd, event, size, deadline, false) < 0) return -1;
        if (frames++ < 6) printf("BT_ROM_EVENT code=0x%02x params=%zu\n", (unsigned)header[1], size);
        if (header[1] == 0xff && size >= 2 && event[0] == 0x00 && event[1] == 0x02) {
            if (size != 14 || legacy) { errno = EPROTO; return -1; }
            if (parse_version(version, event + 2, "legacy") < 0) return -1;
            legacy = true;
        } else if (header[1] == 0x0e && size >= 3 && get16(event + 1) == 0xfc00) {
            if (size < 4 || event[3]) { errno = EPROTO; return -1; }
            if (size == 18 && event[4] == 0x19 && !legacy) {
                return parse_version(version, event + 6, "unified");
            }
            if (size != 4) { errno = EPROTO; return -1; }
            if (legacy) return 0;  /* Vendor reply must precede its completion. */
        } else if (header[1] == 0x0f && size == 4 && get16(event + 2) == 0xfc00 && event[0]) {
            errno = EPROTO;
            return -1;
        }
    }
}

static int char_open(const char *path, int flags)
{
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | flags);
    if (fd < 0) return -1;
    struct stat st;
    int rc = fstat(fd, &st);
    if (rc != 0 || !S_ISCHR(st.st_mode)) {
        int error = rc != 0 ? errno : ENODEV;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int chip_id(int fd)
{
    static const char expected[] = "qcom,qca6490";
    char value[32] = {0};  /* BT_CMD_GET_CHIPSET_ID copies MAX_PROP_SIZE bytes */
    if (ioctl(fd, BT_CHIP_ID, value) != 0) return -1;
    if (memcmp(value, expected, sizeof(expected))) { errno = ENODEV; return -1; }
    puts("BT_ROM_CHIPSET=qcom,qca6490");
    return 0;
}

static int setup_uart(int fd, struct termios *old, bool *restore)
{
    int line = -1;
    if (ioctl(fd, TIOCEXCL, 0) != 0 || ioctl(fd, TIOCGETD, &line) != 0) return -1;
    if (line != 0) { errno = EBUSY; return -1; }  /* Require the existing N_TTY. */
    if (tcgetattr(fd, old) != 0) return -1;
    struct termios raw = *old;
    cfmakeraw(&raw);
    raw.c_iflag &= ~(IXON | IXOFF | IXANY);
    raw.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    raw.c_cflag |= CS8 | CLOCAL | CREAD | CRTSCTS;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (cfsetispeed(&raw, B115200) != 0 || cfsetospeed(&raw, B115200) != 0) return -1;
    *restore = true;
    if (tcsetattr(fd, TCSANOW, &raw) != 0 || tcgetattr(fd, &raw) != 0) return -1;
    if ((raw.c_cflag & (CSIZE | PARENB | CSTOPB | CRTSCTS)) != (CS8 | CRTSCTS) ||
        cfgetispeed(&raw) != B115200 || cfgetospeed(&raw) != B115200) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int rom_probe(int power, const char *path)
{
    int tty = -1, rc = 1;
    bool powered = false, voted = false, restore = false, restore_lines = false;
    int old_modem = 0;
    struct termios old;
    struct rom_version version = {0};
    if (cancelled) return 1;
    if (ioctl(power, BT_POWER, 0) != 0) return fail("power_baseline");
    if (cancelled) return 1;
    powered = true;  /* Attempt matched off even if the driver reports failed power-on. */
    if (ioctl(power, BT_POWER, 1) != 0) { fail("power_on"); goto out; }
    puts("BT_ROM_POWER_ON=1");
    const struct timespec settle = { .tv_nsec = 250000000 };
    if (nanosleep(&settle, NULL) != 0 || cancelled) goto out;
    tty = char_open(path, O_NOCTTY);
    if (tty < 0 || setup_uart(tty, &old, &restore) != 0) { fail("uart_open_or_setup"); goto out; }
    if (ioctl(tty, UART_PM_GET, 0) != 0) { fail("uart_pm_get"); goto out; }
    voted = true;
    int active = ioctl(tty, UART_PM_ACTIVE, 0);
    if (active != 1) {
        if (active == 0) errno = EIO;
        fail("uart_pm_active"); goto out;
    }
    int modem = 0;
    if (ioctl(tty, TIOCMGET, &old_modem) != 0) { fail("uart_get_lines"); goto out; }
    modem = old_modem | TIOCM_RTS;
    restore_lines = true;
    if (ioctl(tty, TIOCMSET, &modem) != 0 || ioctl(tty, TIOCMGET, &modem) != 0 ||
        tcflush(tty, TCIOFLUSH) != 0) { fail("uart_set_lines"); goto out; }
    printf("BT_ROM_UART_ACTIVE=1 baud=115200 crtscts=1 cts=%d rts=%d\n",
           !!(modem & TIOCM_CTS), !!(modem & TIOCM_RTS));
    if (!(modem & TIOCM_RTS)) { errno = EIO; fail("uart_rts_readback"); goto out; }
    if (query_version(tty, 2400, &version) != 0) { fail("rom_reply"); goto out; }
    rc = 0;
out:
    /* Discard a failed query that may still be queued behind CTS. Restore
     * line state before dropping the GENI clock, then close and power off. */
    if (voted && rc != 0 && tcflush(tty, TCIOFLUSH) != 0) rc = fail("flush_on_exit");
    if (restore && tcsetattr(tty, TCSANOW, &old) != 0) rc = fail("restore_termios");
    if (restore_lines && ioctl(tty, TIOCMSET, &old_modem) != 0) rc = fail("restore_uart_lines");
    if (voted) {
        int put = ioctl(tty, UART_PM_PUT, 0);
        if (put != 0 && errno == EAGAIN) {
            const struct timespec delay = { .tv_nsec = 150000000 };
            (void)nanosleep(&delay, NULL);
            put = ioctl(tty, UART_PM_PUT, 0);
        }
        if (put != 0) rc = fail("uart_pm_put");
        else puts("BT_ROM_UART_VOTE_RELEASED=1");
    }
    if (tty >= 0 && close(tty) != 0) rc = fail("uart_close");
    if (powered) {
        if (ioctl(power, BT_POWER, 0) != 0) rc = fail("power_off");
        else puts("BT_ROM_POWER_OFF=1");
    }
    if (rc == 0 && !cancelled) {
        printf("BT_ROM_REPLY_OK format=%s product=0x%08" PRIx32 " soc=0x%08" PRIx32
               " rom=0x%04x patch=0x%04x\n", version.format, version.product, version.soc,
               (unsigned)version.rom, (unsigned)version.patch);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    bool identify = argc == 3 && !strcmp(argv[1], "--identify");
    if (!identify && (argc != 4 || strcmp(argv[1], "--version-only"))) {
        fprintf(stderr, "usage: %s --identify POWER_NODE | --version-only POWER_NODE UART_NODE\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction handler = { .sa_handler = cancel };
    if (sigemptyset(&handler.sa_mask) != 0 || sigaction(SIGTERM, &handler, NULL) != 0 ||
        sigaction(SIGINT, &handler, NULL) != 0) return fail("signal_handler");
    int power = char_open(argv[2], 0);
    if (power < 0) return fail("power_open");
    int rc = chip_id(power) != 0 ? fail("chip_id") : identify ? 0 : rom_probe(power, argv[3]);
    if (close(power) != 0) return fail("power_close");
    return rc;
}
