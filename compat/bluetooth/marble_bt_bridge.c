/*
 * marble Bluetooth H4 bridge (Qualcomm SM7475 / ukee / QCA6490 HSP2_1).
 *
 * Hardware-verified QCA6490 bring-up sequence (Round 45–46):
 *   1. power cycle controller via /dev/btpower (BT_CMD_PWR_CTRL 0xbfad: 0 -> 1),
 *   2. open /dev/ttyHS0 raw 8N1 CRTSCTS, TIOCEXCL, vote GENI UART PM clock
 *      (MSM_GENI_SERIAL_TIOCPMGET 0x54ed), assert TIOCM_RTS,
 *   3. query ROM version (0xfc00/0x19) and notify btpower (BT_CMD_CHIPSET_VERS 0xbfae),
 *      mapping real hardware version key 0x400c121100130201 -> hpbtfw21.tlv + hpnv21g.*,
 *   4. switch controller + host UART baud to 3.2 Mbps (flow off -> 0xfc48/0x11 ->
 *      20ms + tcdrain -> TCSETSW2 BOTHER 3200000 -> flow on -> CC status 0x01),
 *   5. stream rampatch (0xfc00/0x1e, 243-byte chunks; skip intermediate ACKs for
 *      download_mode=3, verify final CC), query Board ID (0xfc00/0x23), patch NVM
 *      RAM copy (Tag 17 baud + SIBS off, Tag 27 data[1] &= ~1 SIBS disable, Tag 35
 *      iPA TCS check), stream NVM with per-segment CC check, query Build Info (0x20),
 *   6. confirm normal HCI Reset (0x0c03 -> 04 0e 04 01 03 0c 00),
 *   7. serve raw H4 byte stream on /dev/unix/socket/hmos.bt.h4 ("SBT1" handshake)
 *      for libbt_vendor.z.so / OHOS Bluetooth HDI.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define HMOS_BT_BRIDGE_VERSION "marble-2"
#define HMOS_BT_EXPORT __attribute__((visibility("default")))

/* kernel: drivers/bluetooth/btpower.c + include/linux/btpower.h */
#define HMOS_BT_POWER_IOCTL 0xbfadU       /* BT_CMD_PWR_CTRL */
#define HMOS_BT_CHIPSET_VERS_IOCTL 0xbfaeU /* BT_CMD_CHIPSET_VERS */
#define HMOS_BT_IPA_TCS_IOCTL 0xbfc0U     /* BT_CMD_SET_IPA_TCS_INFO */
#define HMOS_BT_POWER_ON 1

/* kernel: include/uapi/linux/msm_geni_serial.h */
#define HMOS_BT_UART_PM_GET 0x54edU
#define HMOS_BT_UART_PM_PUT 0x54eeU
#define HMOS_BT_UART_PM_ACTIVE 0x54efU

/* Linux asm-generic/termbits.h struct termios2 (44 bytes on aarch64 & x86_64) */
struct hmos_bt_termios2 {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

#define HMOS_BT_TCGETS2 0x802c542aU
#define HMOS_BT_TCSETSW2 0x402c542cU
#define HMOS_BT_CBAUD_MASK 0x100fU
#define HMOS_BT_BOTHER 0x1000U

/* Qualcomm EDL (emergency download) HCI commands, A15 vendor HAL. */
#define HMOS_BT_EDL_OPCODE 0xfc00U      /* command + sub-command byte */
#define HMOS_BT_EDL_SUB_VER_REQ 0x19U   /* PatchVerReq */
#define HMOS_BT_EDL_SUB_APP_VER 0x06U   /* GetAppVerCmd */
#define HMOS_BT_EDL_SUB_BUILD_INFO 0x20U
#define HMOS_BT_EDL_SUB_BOARD_ID 0x23U
#define HMOS_BT_EDL_SUB_TLV 0x1eU       /* TlvDnldSegment */
#define HMOS_BT_EDL_BAUD_OPCODE 0xfc48U
#define HMOS_BT_EDL_MODE_OPCODE 0xfc09U
#define HMOS_BT_EDL_SEGMENT 243         /* TlvDnldReq(): a2 / 243 */

#define HMOS_BT_TLV_TYPE_PATCH 1U
#define HMOS_BT_TLV_TYPE_NVM 2U
#define HMOS_BT_SKIP_EVT_NONE 0U
#define HMOS_BT_SKIP_EVT_VSE_CC 3U

#define HMOS_BT_H4_COMMAND 0x01U
#define HMOS_BT_H4_EVENT 0x04U
#define HMOS_BT_HCI_RESET 0x0c03U
#define HMOS_BT_HCI_EV_CMD_COMPLETE 0x0eU
#define HMOS_BT_HCI_EV_CMD_STATUS 0x0fU
#define HMOS_BT_HCI_EV_VENDOR 0xffU

#define HMOS_BT_DEFAULT_TTY "/dev/ttyHS0"
#define HMOS_BT_DEFAULT_POWER "/dev/btpower"
#define HMOS_BT_DEFAULT_FW_DIR "/bt_firmware/image"
#define HMOS_BT_DEFAULT_SOCKET "/dev/unix/socket/hmos.bt.h4"
#define HMOS_BT_DEFAULT_RAMPATCH "hpbtfw21.tlv"
#define HMOS_BT_DEFAULT_NVM "hpnv21g.bin"
static const char *const hmos_bt_rampatch_fallbacks[] = {
    "hpbtfw20.tlv", "hpbtfw10.tlv", NULL
};
static const char *const hmos_bt_nvm_fallbacks[] = {
    "hpnv21.bin", "hpnv20.bin", "hpnv20g.bin", "hpnv10.bin", NULL
};

/* Handshake with libbt_vendor.z.so (compat/bluetooth/bluetooth_vendor_v136.cpp). */
static const char hmos_bt_ready_magic[4] = {'S', 'B', 'T', '1'};
static const char hmos_bt_fail_magic[4] = {'S', 'B', 'E', '1'};

#define HMOS_BT_SOCKET_UID 3021 /* blue_host */
#define HMOS_BT_SOCKET_GID 3021 /* bluetooth */

struct hmos_bt_config {
    const char *tty_path;
    const char *power_path;
    const char *firmware_dir;
    const char *rampatch;
    const char *nvm;
    const char *socket_path;
    const char *log_path;
    int baud;            /* initial UART baud, default 115200 */
    int post_baud;       /* operating baud for download + H4, 0 = auto 3200000 on HSP2_1, <0 = stay */
    int seg_wait;        /* force waiting for an event after every TLV segment */
    int timeout_ms;      /* per command/event wait */
    int skip_nvm;
    int no_download;     /* skip EDL firmware download */
    int check_only;      /* validate paths/config, never touch the hardware */
    unsigned max_sessions; /* 0 = serve until signalled */
};

struct hmos_bt_rom_ver {
    uint32_t product;
    uint32_t soc;
    uint16_t patch;
    uint16_t rom;
    int unified;
};

static volatile sig_atomic_t hmos_bt_stop;

static void hmos_bt_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("HMOS_BT ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(args);
}

static void hmos_bt_on_signal(int sig)
{
    (void)sig;
    hmos_bt_stop = 1;
}

static uint16_t hmos_bt_get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t hmos_bt_get32(const uint8_t *p)
{
    return (uint32_t)hmos_bt_get16(p) | ((uint32_t)hmos_bt_get16(p + 2) << 16);
}

static int64_t hmos_bt_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        return -1;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

HMOS_BT_EXPORT const char *hmos_bt_bridge_version(void)
{
    return HMOS_BT_BRIDGE_VERSION;
}

HMOS_BT_EXPORT void hmos_bt_bridge_defaults(struct hmos_bt_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->tty_path = HMOS_BT_DEFAULT_TTY;
    cfg->power_path = HMOS_BT_DEFAULT_POWER;
    cfg->firmware_dir = HMOS_BT_DEFAULT_FW_DIR;
    cfg->rampatch = HMOS_BT_DEFAULT_RAMPATCH;
    cfg->nvm = HMOS_BT_DEFAULT_NVM;
    cfg->socket_path = HMOS_BT_DEFAULT_SOCKET;
    cfg->log_path = NULL;
    cfg->baud = 115200;
    cfg->post_baud = 0;
    cfg->seg_wait = 0;
    cfg->timeout_ms = 2000;
    cfg->skip_nvm = 0;
    cfg->no_download = 0;
    cfg->check_only = 0;
    cfg->max_sessions = 1;
}

/* ---------- power ---------- */

static int hmos_bt_power_ioctl(const char *power_path, unsigned long req, unsigned long arg)
{
    int fd = open(power_path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        hmos_bt_log("POWER_ERROR path=%s op=open errno=%d (%s)", power_path, errno, strerror(errno));
        return -1;
    }
    int rc = ioctl(fd, req, arg);
    int saved = errno;
    close(fd);
    errno = saved;
    return rc;
}

HMOS_BT_EXPORT int hmos_bt_power_set(const char *power_path, int on)
{
    if (power_path == NULL) {
        hmos_bt_log("POWER_ERROR reason=null_path");
        return -1;
    }
    if (hmos_bt_power_ioctl(power_path, HMOS_BT_POWER_IOCTL, (unsigned long)(on ? HMOS_BT_POWER_ON : 0)) < 0) {
        hmos_bt_log("POWER_ERROR path=%s op=ioctl(0x%x,%d) errno=%d (%s)", power_path,
                    HMOS_BT_POWER_IOCTL, on ? HMOS_BT_POWER_ON : 0, errno, strerror(errno));
        return -1;
    }
    hmos_bt_log("POWER=%s path=%s ioctl=0x%x", on ? "on" : "off", power_path, HMOS_BT_POWER_IOCTL);
    return 0;
}

/* ---------- UART ---------- */

static int hmos_bt_qca_baud_code(int baud, uint8_t *code)
{
    switch (baud) {
    case 115200: *code = 0x00; return 0;
    case 230400: *code = 0x05; return 0;
    case 460800: *code = 0x07; return 0;
    case 921600: *code = 0x0a; return 0;
    case 1000000: *code = 0x0b; return 0;
    case 2000000: *code = 0x0d; return 0;
    case 3000000: *code = 0x0e; return 0;
    case 4000000: *code = 0x0f; return 0;
    case 3200000: *code = 0x11; return 0;
    default: return -1;
    }
}

static speed_t hmos_bt_speed(int baud)
{
    switch (baud) {
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 3000000: return B3000000;
    case 4000000: return B4000000;
    default: return 0;
    }
}

static int hmos_bt_set_rts(int fd, int high)
{
    int modem = 0;
    if (ioctl(fd, TIOCMGET, &modem) != 0) {
        return -1;
    }
    if (high) modem |= TIOCM_RTS;
    else modem &= ~TIOCM_RTS;
    return ioctl(fd, TIOCMSET, &modem);
}

HMOS_BT_EXPORT int hmos_bt_uart_set_baud(int fd, int baud)
{
    struct hmos_bt_termios2 t2;
    memset(&t2, 0, sizeof(t2));
    if (baud <= 0) {
        hmos_bt_log("UART_ERROR op=baud baud=%d reason=unsupported", baud);
        return -1;
    }
    if (ioctl(fd, HMOS_BT_TCGETS2, &t2) == 0) {
        t2.c_cflag &= ~HMOS_BT_CBAUD_MASK;
        t2.c_cflag |= HMOS_BT_BOTHER;
        t2.c_ispeed = (uint32_t)baud;
        t2.c_ospeed = (uint32_t)baud;
        if (ioctl(fd, HMOS_BT_TCSETSW2, &t2) != 0) {
            hmos_bt_log("UART_ERROR op=TCSETSW2 baud=%d errno=%d (%s)", baud, errno, strerror(errno));
            return -1;
        }
        struct hmos_bt_termios2 rb;
        memset(&rb, 0, sizeof(rb));
        if (ioctl(fd, HMOS_BT_TCGETS2, &rb) != 0 || rb.c_ispeed != (uint32_t)baud || rb.c_ospeed != (uint32_t)baud) {
            hmos_bt_log("UART_ERROR op=baud_readback want=%d got=%u/%u", baud, rb.c_ispeed, rb.c_ospeed);
            return -1;
        }
        return 0;
    }
    struct termios tio;
    speed_t speed = hmos_bt_speed(baud);
    if (speed == 0) {
        hmos_bt_log("UART_ERROR op=baud baud=%d reason=unsupported", baud);
        return -1;
    }
    if (tcgetattr(fd, &tio) != 0) {
        hmos_bt_log("UART_ERROR op=tcgetattr errno=%d (%s)", errno, strerror(errno));
        return -1;
    }
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    if (tcsetattr(fd, TCSADRAIN, &tio) != 0) {
        hmos_bt_log("UART_ERROR op=tcsetattr baud=%d errno=%d (%s)", baud, errno, strerror(errno));
        return -1;
    }
    return 0;
}

HMOS_BT_EXPORT int hmos_bt_uart_open(const char *tty_path, int baud)
{
    struct termios tio;
    int fd;
    speed_t speed = hmos_bt_speed(baud);

    if (tty_path == NULL) {
        hmos_bt_log("UART_ERROR reason=null_path");
        return -1;
    }
    if (speed == 0 && baud != 3200000) {
        hmos_bt_log("UART_ERROR path=%s reason=unsupported_baud baud=%d", tty_path, baud);
        return -1;
    }
    fd = open(tty_path, O_RDWR | O_NOCTTY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        hmos_bt_log("UART_ERROR path=%s op=open errno=%d (%s)", tty_path, errno, strerror(errno));
        return -1;
    }
    (void)ioctl(fd, TIOCEXCL, 0);
    if (tcgetattr(fd, &tio) != 0) {
        hmos_bt_log("UART_ERROR path=%s op=tcgetattr errno=%d (%s)", tty_path, errno, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= (CS8 | CLOCAL | CREAD | CRTSCTS);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    if (speed != 0) {
        cfsetispeed(&tio, speed);
        cfsetospeed(&tio, speed);
    }
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        hmos_bt_log("UART_ERROR path=%s op=tcsetattr errno=%d (%s)", tty_path, errno, strerror(errno));
        close(fd);
        return -1;
    }
    if (speed == 0 && hmos_bt_uart_set_baud(fd, baud) != 0) {
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    hmos_bt_log("UART_OPEN path=%s fd=%d baud=%d raw=1 crtscts=1", tty_path, fd, baud);
    return fd;
}

static int hmos_bt_write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;

    while (len > 0) {
        ssize_t written = write(fd, p, len);
        if (written > 0) {
            p += written;
            len -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            int ready = poll(&pfd, 1, 5000);
            if (ready > 0 && (pfd.revents & POLLOUT)) {
                continue;
            }
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready == 0) errno = ETIMEDOUT;
            else if (ready > 0) errno = EIO;
        }
        hmos_bt_log("WRITE_ERROR fd=%d errno=%d (%s)", fd, errno, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------- HCI / EDL framing ---------- */

static int hmos_bt_reset_read(int fd, uint8_t *data, size_t len, int64_t deadline)
{
    while (len > 0) {
        if (hmos_bt_stop) {
            errno = EINTR;
            return -1;
        }
        int64_t now = hmos_bt_monotonic_ms();
        if (now < 0) {
            return -1;
        }
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, (int)(deadline - now));
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ready == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (!(pfd.revents & POLLIN)) {
            errno = EIO;
            return -1;
        }
        ssize_t got = read(fd, data, len);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        if (got == 0) {
            errno = EIO;
            return -1;
        }
        data += got;
        len -= (size_t)got;
    }
    return 0;
}

static int hmos_bt_read_event(int fd, uint8_t *ev_code, uint8_t *params, size_t *plen, int64_t deadline)
{
    uint8_t hdr[3];
    if (hmos_bt_reset_read(fd, hdr, 1, deadline) != 0) return -1;
    if (hdr[0] != HMOS_BT_H4_EVENT) {
        hmos_bt_log("HCI_ERROR reason=not_event type=0x%02x", hdr[0]);
        errno = EPROTO;
        return -1;
    }
    if (hmos_bt_reset_read(fd, hdr + 1, 2, deadline) != 0) return -1;
    if (hmos_bt_reset_read(fd, params, hdr[2], deadline) != 0) return -1;
    *ev_code = hdr[1];
    *plen = hdr[2];
    return 0;
}

HMOS_BT_EXPORT int hmos_bt_hci_command(int fd, uint16_t opcode, const uint8_t *params, size_t len,
                                       uint8_t *event, size_t *event_len, int timeout_ms)
{
    uint8_t packet[4 + 255];
    size_t total = 0;

    if (len > sizeof(packet) - 4) {
        hmos_bt_log("HCI_ERROR reason=params_too_large len=%zu", len);
        return -1;
    }
    packet[0] = HMOS_BT_H4_COMMAND;
    packet[1] = (uint8_t)(opcode & 0xff);
    packet[2] = (uint8_t)(opcode >> 8);
    packet[3] = (uint8_t)len;
    if (len > 0 && params != NULL) {
        memcpy(packet + 4, params, len);
    }
    total = 4 + len;
    if (hmos_bt_write_all(fd, packet, total) != 0) {
        return -1;
    }
    if (event == NULL || event_len == NULL || *event_len == 0) {
        return 0;
    }
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready == 0) {
            hmos_bt_log("HCI_TIMEOUT opcode=0x%04x timeout_ms=%d", opcode, timeout_ms);
            return -1;
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            hmos_bt_log("HCI_ERROR op=poll errno=%d (%s)", errno, strerror(errno));
            return -1;
        }
        ssize_t got = read(fd, event, *event_len);
        if (got <= 0) {
            if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            hmos_bt_log("HCI_ERROR op=read errno=%d (%s)", errno, strerror(errno));
            return -1;
        }
        *event_len = (size_t)got;
        return 0;
    }
}

HMOS_BT_EXPORT int hmos_bt_edl_exec(int fd, uint8_t sub, const uint8_t *extra, size_t extra_len,
                                    int timeout_ms, uint8_t *event, size_t *event_len)
{
    uint8_t params[8];

    if (extra_len > sizeof(params) - 1) {
        hmos_bt_log("EDL_ERROR reason=payload_too_large len=%zu", extra_len);
        return -1;
    }
    params[0] = sub;
    if (extra_len > 0 && extra != NULL) {
        memcpy(params + 1, extra, extra_len);
    }
    return hmos_bt_hci_command(fd, HMOS_BT_EDL_OPCODE, params, 1 + extra_len, event, event_len,
                               timeout_ms);
}

static int hmos_bt_wait_cc(int fd, uint16_t opcode, uint8_t expect_status,
                           uint8_t *out_ev, size_t *out_len, int timeout_ms)
{
    int64_t start = hmos_bt_monotonic_ms();
    if (start < 0) return -1;
    int64_t deadline = start + timeout_ms;
    for (;;) {
        uint8_t code = 0, ev[255];
        size_t size = 0;
        if (hmos_bt_read_event(fd, &code, ev, &size, deadline) != 0) return -1;
        if (code == HMOS_BT_HCI_EV_CMD_STATUS && size == 4 &&
            hmos_bt_get16(ev + 2) == opcode && ev[0] != 0) {
            hmos_bt_log("HCI_CMD_STATUS_ERR opcode=0x%04x status=0x%02x", opcode, ev[0]);
            errno = EPROTO;
            return -1;
        }
        if (code == HMOS_BT_HCI_EV_CMD_COMPLETE && size >= 4 && hmos_bt_get16(ev + 1) == opcode) {
            if (ev[3] != expect_status) {
                hmos_bt_log("HCI_CMD_COMPLETE_ERR opcode=0x%04x want=0x%02x got=0x%02x",
                            opcode, expect_status, ev[3]);
                errno = EPROTO;
                return -1;
            }
            if (out_ev && out_len) {
                memcpy(out_ev, ev, size);
                *out_len = size;
            }
            return 0;
        }
    }
}

static int hmos_bt_query_version(int fd, int timeout_ms, struct hmos_bt_rom_ver *v)
{
    uint8_t sub = HMOS_BT_EDL_SUB_VER_REQ;
    if (hmos_bt_hci_command(fd, HMOS_BT_EDL_OPCODE, &sub, 1, NULL, NULL, timeout_ms) != 0) {
        return -1;
    }
    int64_t start = hmos_bt_monotonic_ms();
    if (start < 0) return -1;
    int64_t deadline = start + timeout_ms;
    int legacy = 0;
    for (;;) {
        uint8_t code = 0, ev[255];
        size_t size = 0;
        if (hmos_bt_read_event(fd, &code, ev, &size, deadline) != 0) return -1;
        if (code == HMOS_BT_HCI_EV_VENDOR && size == 14 && ev[0] == 0x00 && ev[1] == 0x02 && !legacy) {
            v->product = hmos_bt_get32(ev + 2);
            v->patch = hmos_bt_get16(ev + 6);
            v->rom = hmos_bt_get16(ev + 8);
            v->soc = hmos_bt_get32(ev + 10);
            v->unified = 0;
            legacy = 1;
        } else if (code == HMOS_BT_HCI_EV_CMD_COMPLETE && size >= 4 &&
                   hmos_bt_get16(ev + 1) == HMOS_BT_EDL_OPCODE) {
            if (ev[3] != 0) { errno = EPROTO; return -1; }
            if (size == 18 && ev[4] == HMOS_BT_EDL_SUB_VER_REQ && !legacy) {
                v->product = hmos_bt_get32(ev + 6);
                v->patch = hmos_bt_get16(ev + 10);
                v->rom = hmos_bt_get16(ev + 12);
                v->soc = hmos_bt_get32(ev + 14);
                v->unified = 1;
                return (v->soc != 0 && v->soc != UINT32_MAX) ? 0 : -1;
            }
            if (size == 4 && legacy) {
                return (v->soc != 0 && v->soc != UINT32_MAX) ? 0 : -1;
            }
            errno = EPROTO;
            return -1;
        } else if (code == HMOS_BT_HCI_EV_CMD_STATUS && size == 4 &&
                   hmos_bt_get16(ev + 2) == HMOS_BT_EDL_OPCODE && ev[0] != 0) {
            errno = EPROTO;
            return -1;
        }
    }
}

static int hmos_bt_switch_baud_qca(int tty_fd, int target_baud, int unified, int timeout_ms)
{
    uint8_t bcode = 0;
    if (hmos_bt_qca_baud_code(target_baud, &bcode) != 0) {
        hmos_bt_log("BAUD_ERROR unsupported=%d", target_baud);
        return -1;
    }
    if (hmos_bt_set_rts(tty_fd, 0) != 0) {
        hmos_bt_log("BAUD_ERROR stage=flow_off errno=%d (%s)", errno, strerror(errno));
        return -1;
    }
    if (hmos_bt_hci_command(tty_fd, HMOS_BT_EDL_BAUD_OPCODE, &bcode, 1, NULL, NULL, timeout_ms) != 0) {
        (void)hmos_bt_set_rts(tty_fd, 1);
        return -1;
    }
    usleep(20000);
    (void)tcdrain(tty_fd);
    if (hmos_bt_uart_set_baud(tty_fd, target_baud) != 0) {
        (void)hmos_bt_set_rts(tty_fd, 1);
        return -1;
    }
    if (hmos_bt_set_rts(tty_fd, 1) != 0) {
        hmos_bt_log("BAUD_ERROR stage=flow_on errno=%d (%s)", errno, strerror(errno));
        return -1;
    }
    /* Stock GetVsHciEvent@0x55c2c: 0xfc48 CC success status is 0x01 (BAUDRATE_CHANGE_SUCCESS). */
    int64_t start = hmos_bt_monotonic_ms();
    if (start < 0) return -1;
    int64_t deadline = start + timeout_ms;
    int saw_vse_92 = 0;
    for (;;) {
        uint8_t code = 0, ev[255];
        size_t size = 0;
        if (hmos_bt_read_event(tty_fd, &code, ev, &size, deadline) != 0) return -1;
        if (code == HMOS_BT_HCI_EV_VENDOR && size >= 1 && ev[0] == 0x92) {
            saw_vse_92 = 1;
            continue;
        }
        if (code == HMOS_BT_HCI_EV_CMD_COMPLETE && size >= 4 &&
            hmos_bt_get16(ev + 1) == HMOS_BT_EDL_BAUD_OPCODE) {
            if (ev[3] != 0x01 || (!unified && !saw_vse_92)) {
                hmos_bt_log("BAUD_ERROR status=0x%02x unified=%d vse=%d", ev[3], unified, saw_vse_92);
                errno = EPROTO;
                return -1;
            }
            hmos_bt_log("UART_BAUD_SWITCHED baud=%d code=0x%02x status=0x01", target_baud, bcode);
            return 0;
        }
    }
}

/* ---------- firmware ---------- */

static const char *hmos_bt_pick(const char *dir, const char *preferred,
                                const char *const *fallbacks, char *out, size_t out_len)
{
    char path[512];

    if (preferred != NULL && preferred[0] != '\0') {
        if (snprintf(path, sizeof(path), "%s/%s", dir, preferred) < (int)sizeof(path) &&
            access(path, R_OK) == 0) {
            snprintf(out, out_len, "%s", preferred);
            return out;
        }
    }
    for (size_t i = 0; fallbacks != NULL && fallbacks[i] != NULL; i++) {
        if (snprintf(path, sizeof(path), "%s/%s", dir, fallbacks[i]) < (int)sizeof(path) &&
            access(path, R_OK) == 0) {
            hmos_bt_log("FIRMWARE_FALLBACK preferred=%s chosen=%s", preferred ? preferred : "-",
                        fallbacks[i]);
            snprintf(out, out_len, "%s", fallbacks[i]);
            return out;
        }
    }
    return NULL;
}

HMOS_BT_EXPORT int hmos_bt_firmware_load(const char *dir, const char *name, uint8_t **data,
                                         size_t *size)
{
    char path[512];
    struct stat st;
    int fd;
    uint8_t *buffer;
    size_t done = 0;

    if (data == NULL || size == NULL || dir == NULL || name == NULL) {
        hmos_bt_log("FIRMWARE_ERROR reason=bad_argument");
        return -1;
    }
    *data = NULL;
    *size = 0;
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
        hmos_bt_log("FIRMWARE_ERROR reason=path_too_long dir=%s name=%s", dir, name);
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        hmos_bt_log("FIRMWARE_ERROR path=%s op=open errno=%d (%s)", path, errno, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        hmos_bt_log("FIRMWARE_ERROR path=%s reason=bad_size errno=%d (%s)", path, errno,
                    strerror(errno));
        close(fd);
        return -1;
    }
    if (st.st_size > (off_t)(8 * 1024 * 1024)) {
        hmos_bt_log("FIRMWARE_ERROR path=%s reason=too_large size=%lld", path,
                    (long long)st.st_size);
        close(fd);
        return -1;
    }
    buffer = malloc((size_t)st.st_size);
    if (buffer == NULL) {
        hmos_bt_log("FIRMWARE_ERROR path=%s reason=out_of_memory size=%lld", path,
                    (long long)st.st_size);
        close(fd);
        return -1;
    }
    while (done < (size_t)st.st_size) {
        ssize_t got = read(fd, buffer + done, (size_t)st.st_size - done);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) continue;
            hmos_bt_log("FIRMWARE_ERROR path=%s op=read done=%zu errno=%d (%s)", path, done, errno,
                        strerror(errno));
            free(buffer);
            close(fd);
            return -1;
        }
        done += (size_t)got;
    }
    close(fd);
    *data = buffer;
    *size = done;
    hmos_bt_log("FIRMWARE_LOADED path=%s bytes=%zu", path, done);
    return 0;
}

static int hmos_bt_patch_nvm_sibs_off(uint8_t *buf, size_t len, uint8_t baud_code, int *ipa_out)
{
    if (ipa_out) *ipa_out = 0;
    if (len < 4 || buf[0] != HMOS_BT_TLV_TYPE_NVM) {
        return -1;
    }
    uint32_t tlv_len = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16);
    if ((size_t)tlv_len + 4 != len) {
        return -1;
    }
    size_t pos = 4;
    while (pos < len) {
        if (len - pos < 12) return -1;
        uint16_t tag_id = hmos_bt_get16(buf + pos);
        uint16_t tag_len = hmos_bt_get16(buf + pos + 2);
        if (len - (pos + 12) < tag_len) return -1;
        uint8_t *data = buf + pos + 12;
        if (tag_id == 17 && tag_len >= 2) {
            uint8_t old0 = data[0], old1 = data[1];
            data[0] &= (uint8_t)~0x80U;
            data[1] = baud_code;
            hmos_bt_log("NVM_TAG17 %02x%02x->%02x%02x", old0, old1, data[0], data[1]);
        } else if (tag_id == 27 && tag_len >= 2) {
            uint8_t old0 = data[0], old1 = data[1];
            data[1] &= (uint8_t)~0x01U; /* SIBS Disable per UpdateNewNvmFormat@0x600a0 */
            hmos_bt_log("NVM_TAG27 %02x%02x->%02x%02x", old0, old1, data[0], data[1]);
        } else if (tag_id == 35 && tag_len >= 0x77) {
            if (ipa_out && (data[0x76] & 0x04U)) *ipa_out = 1;
        }
        pos += 12 + tag_len;
    }
    return 0;
}

/*
 * TlvDnldReq()/TlvDnldSegment(): chop TLV file into <=243-byte segments and send
 * each as 0xfc00 with payload [0x1e, seg_len, data...].
 * For PATCH with download_mode == 3 (SKIP_EVT_VSE_CC), only the last segment is acked;
 * for NVM (type == 2) or seg_wait != 0, every segment waits for a matching CC.
 */
static int hmos_bt_download_tlv_typed(int fd, const uint8_t *data, size_t len,
                                      uint8_t tlv_type, uint8_t dnld_mode,
                                      int unified, int force_seg_wait, int timeout_ms)
{
    size_t offset = 0;
    unsigned segments = 0;

    while (offset < len) {
        size_t chunk = len - offset;
        uint8_t params[2 + HMOS_BT_EDL_SEGMENT];
        if (chunk > HMOS_BT_EDL_SEGMENT) {
            chunk = HMOS_BT_EDL_SEGMENT;
        }
        int is_last = (offset + chunk == len);
        int wait_evt = force_seg_wait || (tlv_type != HMOS_BT_TLV_TYPE_PATCH) ||
                       (dnld_mode != HMOS_BT_SKIP_EVT_VSE_CC) || is_last;

        params[0] = HMOS_BT_EDL_SUB_TLV;
        params[1] = (uint8_t)chunk;
        memcpy(params + 2, data + offset, chunk);
        if (hmos_bt_hci_command(fd, HMOS_BT_EDL_OPCODE, params, 2 + chunk,
                                NULL, NULL, timeout_ms) != 0) {
            hmos_bt_log("TLV_DOWNLOAD_FAILED offset=%zu sent=%zu/%zu", offset, offset, len);
            return -1;
        }
        if (wait_evt) {
            uint8_t ev[255];
            size_t ev_len = 0;
            if (unified) {
                if (hmos_bt_wait_cc(fd, HMOS_BT_EDL_OPCODE, 0x00, ev, &ev_len, timeout_ms) != 0) {
                    hmos_bt_log("TLV_DOWNLOAD_FAILED stage=cc offset=%zu/%zu", offset, len);
                    return -1;
                }
                if (ev_len >= 5 && ev[4] != HMOS_BT_EDL_SUB_TLV) {
                    hmos_bt_log("TLV_DOWNLOAD_FAILED stage=sub offset=%zu got=0x%02x", offset, ev[4]);
                    errno = EPROTO;
                    return -1;
                }
            } else {
                if (hmos_bt_wait_cc(fd, HMOS_BT_EDL_OPCODE, 0x00, ev, &ev_len, timeout_ms) != 0) {
                    hmos_bt_log("TLV_DOWNLOAD_FAILED stage=legacy_cc offset=%zu/%zu", offset, len);
                    return -1;
                }
            }
        }
        offset += chunk;
        segments++;
    }
    hmos_bt_log("TLV_DOWNLOADED type=%u bytes=%zu segments=%u", (unsigned)tlv_type, len, segments);
    return 0;
}

HMOS_BT_EXPORT int hmos_bt_download_stream(int fd, const uint8_t *data, size_t len, int seg_wait,
                                           int timeout_ms)
{
    uint8_t tlv_type = (len >= 1) ? data[0] : HMOS_BT_TLV_TYPE_PATCH;
    uint8_t dnld_mode = (len >= 15 && tlv_type == HMOS_BT_TLV_TYPE_PATCH)
                        ? data[14] : HMOS_BT_SKIP_EVT_NONE;
    return hmos_bt_download_tlv_typed(fd, data, len, tlv_type, dnld_mode, 1, seg_wait, timeout_ms);
}

/* ---------- socket service ---------- */

static int hmos_bt_socket_prepare(const char *path)
{
    char dir[512];
    char *slash;

    if (path == NULL || path[0] != '/') {
        hmos_bt_log("SOCKET_ERROR reason=bad_path path=%s", path ? path : "(null)");
        return -1;
    }
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        hmos_bt_log("SOCKET_ERROR reason=path_too_long path=%s", path);
        return -1;
    }
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash == NULL) {
        hmos_bt_log("SOCKET_ERROR reason=no_parent path=%s", path);
        return -1;
    }
    *slash = '\0';
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        hmos_bt_log("SOCKET_ERROR op=mkdir dir=%s errno=%d (%s)", dir, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int hmos_bt_peer_allowed(int fd)
{
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        hmos_bt_log("SOCKET_ERROR op=SO_PEERCRED errno=%d (%s)", errno, strerror(errno));
        return 0;
    }
    if (cred.uid != 0 && cred.uid != HMOS_BT_SOCKET_UID) {
        hmos_bt_log("SOCKET_REJECT pid=%d uid=%d gid=%d", cred.pid, cred.uid, cred.gid);
        return 0;
    }
    hmos_bt_log("SOCKET_CLIENT pid=%d uid=%d gid=%d", cred.pid, cred.uid, cred.gid);
    return 1;
}

static int hmos_bt_pump(int tty_fd, int client_fd)
{
    struct pollfd pfds[2] = {
        { .fd = tty_fd, .events = POLLIN, .revents = 0 },
        { .fd = client_fd, .events = POLLIN, .revents = 0 },
    };
    uint8_t buffer[4096];

    while (!hmos_bt_stop) {
        int ready = poll(pfds, 2, 500);
        if (ready < 0) {
            if (errno == EINTR) continue;
            hmos_bt_log("PUMP_ERROR op=poll errno=%d (%s)", errno, strerror(errno));
            return -1;
        }
        if (ready == 0) {
            if (hmos_bt_stop) return 0;
            continue;
        }
        for (int i = 0; i < 2; i++) {
            if (pfds[i].revents == 0) continue;
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                hmos_bt_log("PUMP_CLOSED side=%s revents=0x%x", i == 0 ? "tty" : "client",
                            pfds[i].revents);
                return 0;
            }
            ssize_t got = read(pfds[i].fd, buffer, sizeof(buffer));
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                hmos_bt_log("PUMP_ERROR side=%s op=read errno=%d (%s)", i == 0 ? "tty" : "client",
                            errno, strerror(errno));
                return -1;
            }
            if (got == 0) {
                hmos_bt_log("PUMP_EOF side=%s", i == 0 ? "tty" : "client");
                return 0;
            }
            int target = i == 0 ? client_fd : tty_fd;
            if (hmos_bt_write_all(target, buffer, (size_t)got) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int hmos_bt_confirm_reset(int tty_fd, int timeout_ms);

HMOS_BT_EXPORT int hmos_bt_socket_serve(int tty_fd, const char *path, unsigned max_sessions)
{
    int listener;
    struct sockaddr_un addr;
    unsigned sessions = 0;

    if (hmos_bt_socket_prepare(path) != 0) {
        return -1;
    }
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        hmos_bt_log("SOCKET_ERROR op=socket errno=%d (%s)", errno, strerror(errno));
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        hmos_bt_log("SOCKET_ERROR op=bind path=%s errno=%d (%s)", path, errno, strerror(errno));
        close(listener);
        return -1;
    }
    if (chown(path, 0, HMOS_BT_SOCKET_GID) != 0) {
        hmos_bt_log("SOCKET_WARN op=chown path=%s errno=%d (%s)", path, errno, strerror(errno));
    }
    if (chmod(path, 0660) != 0) {
        hmos_bt_log("SOCKET_WARN op=chmod path=%s errno=%d (%s)", path, errno, strerror(errno));
    }
    if (listen(listener, 1) != 0) {
        hmos_bt_log("SOCKET_ERROR op=listen path=%s errno=%d (%s)", path, errno, strerror(errno));
        close(listener);
        unlink(path);
        return -1;
    }
    hmos_bt_log("BRIDGE_LISTENING=1 socket=%s hardware_ready=1", path);

    while (!hmos_bt_stop) {
        struct pollfd lpfd = { .fd = listener, .events = POLLIN, .revents = 0 };
        int ready = poll(&lpfd, 1, 500);
        if (ready == 0) continue;
        if (ready < 0) {
            if (errno == EINTR) continue;
            hmos_bt_log("SOCKET_ERROR op=poll errno=%d (%s)", errno, strerror(errno));
            break;
        }
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            hmos_bt_log("SOCKET_ERROR op=accept errno=%d (%s)", errno, strerror(errno));
            break;
        }
        if (!hmos_bt_peer_allowed(client)) {
            if (hmos_bt_write_all(client, hmos_bt_fail_magic, sizeof(hmos_bt_fail_magic)) != 0) {
                hmos_bt_log("SOCKET_WARN op=send_reject");
            }
            close(client);
            continue;
        }
        if (isatty(tty_fd)) {
            tcflush(tty_fd, TCIFLUSH);
        }
        if (hmos_bt_write_all(client, hmos_bt_ready_magic, sizeof(hmos_bt_ready_magic)) != 0) {
            hmos_bt_log("SOCKET_WARN op=send_ready errno=%d (%s)", errno, strerror(errno));
            close(client);
            break;
        }
        hmos_bt_log("BRIDGE_SESSION_START sessions=%u", sessions + 1);
        (void)hmos_bt_pump(tty_fd, client);
        close(client);
        sessions++;
        hmos_bt_log("BRIDGE_SESSION_CLOSED sessions=%u", sessions);
        if (max_sessions != 0 && sessions >= max_sessions) {
            break;
        }
        if (!hmos_bt_stop && isatty(tty_fd)) {
            tcflush(tty_fd, TCIOFLUSH);
            int rrc = hmos_bt_confirm_reset(tty_fd, 2000);
            hmos_bt_log("BRIDGE_SESSION_RESET sessions=%u rc=%d", sessions, rrc);
        }
    }
    close(listener);
    unlink(path);
    hmos_bt_log("BRIDGE_SOCKET_REMOVED path=%s", path);
    return 0;
}

/* ---------- bring-up ---------- */

static int hmos_bt_confirm_reset(int tty_fd, int timeout_ms)
{
    if (timeout_ms <= 0) {
        hmos_bt_log("BRINGUP_FAILED stage=hci_reset reason=invalid_timeout timeout_ms=%d", timeout_ms);
        return -1;
    }
    if (hmos_bt_hci_command(tty_fd, HMOS_BT_HCI_RESET, NULL, 0, NULL, NULL, timeout_ms) != 0) {
        hmos_bt_log("BRINGUP_FAILED stage=hci_reset reason=command_write");
        return -1;
    }
    int64_t start = hmos_bt_monotonic_ms();
    if (start < 0) {
        hmos_bt_log("BRINGUP_FAILED stage=hci_reset reason=clock errno=%d", errno);
        return -1;
    }
    int64_t deadline = start + timeout_ms;

    while (1) {
        uint8_t header[3];
        uint8_t params[255];
        if (hmos_bt_reset_read(tty_fd, header, 1, deadline) != 0) break;
        if (header[0] != HMOS_BT_H4_EVENT) {
            hmos_bt_log("BRINGUP_FAILED stage=hci_reset reason=not_event type=0x%02x", header[0]);
            return -1;
        }
        if (hmos_bt_reset_read(tty_fd, header + 1, sizeof(header) - 1, deadline) != 0 ||
            hmos_bt_reset_read(tty_fd, params, header[2], deadline) != 0) break;

        if (header[1] != HMOS_BT_HCI_EV_CMD_COMPLETE) {
            continue;
        }
        if (header[2] < 3) {
            hmos_bt_log("BRINGUP_FAILED stage=hci_reset reason=short_command_complete len=%u", header[2]);
            return -1;
        }
        uint16_t opcode = (uint16_t)(params[1] | (params[2] << 8));
        if (opcode != HMOS_BT_HCI_RESET) {
            continue;
        }
        if (header[2] != 4) {
            hmos_bt_log("BRINGUP_FAILED stage=hci_reset reason=bad_completion_length len=%u", header[2]);
            return -1;
        }
        if (params[3] != 0) {
            hmos_bt_log("BRINGUP_FAILED stage=hci_reset reason=controller_status status=0x%02x", params[3]);
            return -1;
        }
        return 0;
    }

    if (errno == ETIMEDOUT) {
        hmos_bt_log("HCI_TIMEOUT opcode=0x%04x timeout_ms=%d", HMOS_BT_HCI_RESET, timeout_ms);
        hmos_bt_log("BRINGUP_FAILED stage=hci_reset reason=no_matching_command_complete");
    } else {
        hmos_bt_log("BRINGUP_FAILED stage=hci_reset reason=event_read errno=%d", errno);
    }
    return -1;
}

static int hmos_bt_reset_then_serve(int tty_fd, const struct hmos_bt_config *c)
{
    if (hmos_bt_confirm_reset(tty_fd, c->timeout_ms) != 0) {
        return -1;
    }
    hmos_bt_log("HCI_RESET_OK");
    return hmos_bt_socket_serve(tty_fd, c->socket_path, c->max_sessions);
}

static void hmos_bt_select_nvm_by_bid(const char *dir, const char *base_nvm,
                                      uint8_t b0, uint8_t b1, int valid,
                                      char *out, size_t out_sz)
{
    char cand[4][128];
    size_t n = 0;
    if (valid && (b0 != 0 || b1 != 0)) {
        if (b0 == 0) {
            snprintf(cand[n++], sizeof(cand[0]), "hpnv21g.b%02x", (unsigned)b1);
        } else {
            snprintf(cand[n++], sizeof(cand[0]), "hpnv21g.b%02x%02x", (unsigned)b0, (unsigned)b1);
            snprintf(cand[n++], sizeof(cand[0]), "hpnv21g.b%x%02x", (unsigned)b0, (unsigned)b1);
            snprintf(cand[n++], sizeof(cand[0]), "hpnv21g.%x%02x", (unsigned)b0, (unsigned)b1);
        }
    }
    snprintf(cand[n++], sizeof(cand[0]), "%.*s", (int)(sizeof(cand[0]) - 1), base_nvm);
    for (size_t i = 0; i < n; i++) {
        char full[512];
        if (snprintf(full, sizeof(full), "%s/%s", dir, cand[i]) < (int)sizeof(full) &&
            access(full, R_OK) == 0) {
            snprintf(out, out_sz, "%.*s", (int)(out_sz - 1), cand[i]);
            return;
        }
    }
    snprintf(out, out_sz, "%.*s", (int)(out_sz - 1), base_nvm);
}

HMOS_BT_EXPORT int hmos_bt_bridge_run(const struct hmos_bt_config *cfg)
{
    struct hmos_bt_config copy;
    const struct hmos_bt_config *c = cfg ? cfg : &copy;
    char rampatch[128];
    char nvm[128];
    const char *rampatch_name;
    const char *nvm_name = NULL;
    uint8_t *firmware = NULL;
    size_t firmware_size = 0;
    int tty_fd = -1;
    int rc = -1;
    int powered = 0;
    int pm_voted = 0;
    int restore_lines = 0;
    int old_modem = 0;
    int restore_t2 = 0;
    struct hmos_bt_termios2 old_t2;
    memset(&old_t2, 0, sizeof(old_t2));

    if (cfg == NULL) {
        hmos_bt_bridge_defaults(&copy);
    }

    hmos_bt_log("BRIDGE_VERSION=%s tty=%s power=%s firmware_dir=%s socket=%s", HMOS_BT_BRIDGE_VERSION,
                c->tty_path, c->power_path, c->firmware_dir, c->socket_path);

    rampatch_name = hmos_bt_pick(c->firmware_dir, c->rampatch, hmos_bt_rampatch_fallbacks,
                                 rampatch, sizeof(rampatch));
    if (!c->skip_nvm) {
        nvm_name = hmos_bt_pick(c->firmware_dir, c->nvm, hmos_bt_nvm_fallbacks, nvm, sizeof(nvm));
    }
    if (rampatch_name == NULL) {
        hmos_bt_log("BRINGUP_FAILED stage=firmware reason=no_rampatch dir=%s wanted=%s",
                    c->firmware_dir, c->rampatch ? c->rampatch : "-");
        return -1;
    }
    if (!c->skip_nvm && nvm_name == NULL) {
        hmos_bt_log("FIRMWARE_NVM_MISSING dir=%s wanted=%s", c->firmware_dir,
                    c->nvm ? c->nvm : "-");
    }
    hmos_bt_log("FIRMWARE_SELECTED rampatch=%s nvm=%s", rampatch_name, nvm_name ? nvm_name : "-");

    if (c->check_only) {
        int tty_ok = access(c->tty_path, R_OK | W_OK) == 0;
        int power_ok = access(c->power_path, R_OK | W_OK) == 0;
        char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
        struct stat parent_st;
        int socket_ok = 0;
        if (c->socket_path && c->socket_path[0] == '/' && strlen(c->socket_path) < sizeof(parent)) {
            strcpy(parent, c->socket_path);
            char *slash = strrchr(parent, '/');
            if (slash == parent) slash[1] = '\0';
            else if (slash) *slash = '\0';
            socket_ok = lstat(parent, &parent_st) == 0 && S_ISDIR(parent_st.st_mode) &&
                        access(parent, W_OK | X_OK) == 0;
        }
        hmos_bt_log("CHECK_ONLY=1 tty_exists=%d power_exists=%d socket_dir_ok=%d", tty_ok,
                    power_ok, socket_ok);
        if (!tty_ok) {
            hmos_bt_log("CHECK_FAILED stage=tty path=%s errno=%d (%s)", c->tty_path, errno,
                        strerror(errno));
        }
        if (!power_ok) {
            hmos_bt_log("CHECK_FAILED stage=power path=%s errno=%d (%s)", c->power_path, errno,
                        strerror(errno));
        }
        if (!socket_ok) {
            hmos_bt_log("CHECK_FAILED stage=socket path=%s", c->socket_path);
        }
        hmos_bt_log("CHECK_RESULT=%s", (tty_ok && power_ok && socket_ok) ? "ok" : "failed");
        return (tty_ok && power_ok && socket_ok) ? 0 : -1;
    }

    signal(SIGTERM, hmos_bt_on_signal);
    signal(SIGINT, hmos_bt_on_signal);
    signal(SIGPIPE, SIG_IGN);

    (void)hmos_bt_power_set(c->power_path, 0);
    if (hmos_bt_power_set(c->power_path, 1) != 0) {
        hmos_bt_log("BRINGUP_FAILED stage=power");
        return -1;
    }
    powered = 1;
    usleep(250000);

    tty_fd = hmos_bt_uart_open(c->tty_path, c->baud);
    if (tty_fd < 0) {
        hmos_bt_log("BRINGUP_FAILED stage=uart");
        goto out;
    }
    if (ioctl(tty_fd, HMOS_BT_TCGETS2, &old_t2) == 0) {
        restore_t2 = 1;
    }
    if (ioctl(tty_fd, HMOS_BT_UART_PM_GET, 0) == 0) {
        pm_voted = 1;
        int active = ioctl(tty_fd, HMOS_BT_UART_PM_ACTIVE, 0);
        hmos_bt_log("UART_PM_VOTED=1 active=%d", active);
        if (active != 1) {
            hmos_bt_log("BRINGUP_FAILED stage=uart_pm_active active=%d", active);
            goto out;
        }
    } else if (errno != ENOTTY && errno != EINVAL) {
        hmos_bt_log("BRINGUP_FAILED stage=uart_pm_get errno=%d (%s)", errno, strerror(errno));
        goto out;
    }
    if (ioctl(tty_fd, TIOCMGET, &old_modem) == 0) {
        restore_lines = 1;
        if (hmos_bt_set_rts(tty_fd, 1) != 0) {
            hmos_bt_log("BRINGUP_FAILED stage=uart_rts errno=%d (%s)", errno, strerror(errno));
            goto out;
        }
    }
    (void)tcflush(tty_fd, TCIOFLUSH);

    struct hmos_bt_rom_ver ver;
    memset(&ver, 0, sizeof(ver));
    int ver_rc = -1;
    for (int attempt = 1; attempt <= 3; attempt++) {
        (void)tcflush(tty_fd, TCIOFLUSH);
        ver_rc = hmos_bt_query_version(tty_fd, c->timeout_ms, &ver);
        if (ver_rc == 0) break;
        hmos_bt_log("EDL_VERSION_RETRY n=%d errno=%d", attempt, errno);
        usleep(200000);
    }
    if (ver_rc != 0) {
        hmos_bt_log("BRINGUP_FAILED stage=edl_version");
        goto out;
    }
    uint64_t key = ((uint64_t)ver.soc << 32) | ((uint64_t)(ver.product & 0xffffU) << 16) | ver.rom;
    hmos_bt_log("EDL_VERSION_OK format=%s product=0x%08" PRIx32 " soc=0x%08" PRIx32
                " rom=0x%04x patch=0x%04x key=0x%016" PRIx64,
                ver.unified ? "unified" : "legacy", ver.product, ver.soc,
                (unsigned)ver.rom, (unsigned)ver.patch, key);
    (void)hmos_bt_power_ioctl(c->power_path, HMOS_BT_CHIPSET_VERS_IOCTL, (unsigned long)ver.soc);

    /* Map real QCA6490 HSP2_1 version key -> hpbtfw21.tlv / hpnv21g.bin per stock LoadPatchMaptable. */
    if (key == 0x400c121100130201ULL) {
        char probed_rp[128], probed_nv[128];
        if (hmos_bt_pick(c->firmware_dir, "hpbtfw21.tlv", NULL, probed_rp, sizeof(probed_rp))) {
            snprintf(rampatch, sizeof(rampatch), "%s", probed_rp);
            rampatch_name = rampatch;
        }
        if (!c->skip_nvm && hmos_bt_pick(c->firmware_dir, "hpnv21g.bin", NULL, probed_nv, sizeof(probed_nv))) {
            snprintf(nvm, sizeof(nvm), "%s", probed_nv);
            nvm_name = nvm;
        }
        hmos_bt_log("FIRMWARE_VERSION_MATCH key=0x%016" PRIx64 " rampatch=%s nvm=%s",
                    key, rampatch_name, nvm_name ? nvm_name : "-");
    }

    int active_baud = c->baud;
    int target_baud = c->post_baud;
    if (target_baud == 0 && key == 0x400c121100130201ULL) {
        target_baud = 3200000;
    }
    if (target_baud > 0 && target_baud != active_baud) {
        if (hmos_bt_switch_baud_qca(tty_fd, target_baud, ver.unified, c->timeout_ms) != 0) {
            hmos_bt_log("BRINGUP_FAILED stage=baud_switch baud=%d", target_baud);
            goto out;
        }
        active_baud = target_baud;
    }

    if (c->no_download) {
        hmos_bt_log("EDL_DOWNLOAD=skipped reason=no_download");
    } else {
        if (hmos_bt_firmware_load(c->firmware_dir, rampatch_name, &firmware, &firmware_size) != 0) {
            hmos_bt_log("BRINGUP_FAILED stage=load_rampatch name=%s", rampatch_name);
            goto out;
        }
        uint8_t dnld_mode = (firmware_size >= 15 && firmware[0] == HMOS_BT_TLV_TYPE_PATCH)
                            ? firmware[14] : HMOS_BT_SKIP_EVT_NONE;
        if (hmos_bt_download_tlv_typed(tty_fd, firmware, firmware_size, HMOS_BT_TLV_TYPE_PATCH,
                                       dnld_mode, ver.unified, c->seg_wait, c->timeout_ms * 2) != 0) {
            free(firmware);
            firmware = NULL;
            hmos_bt_log("BRINGUP_FAILED stage=download_rampatch name=%s bytes=%zu", rampatch_name,
                        firmware_size);
            goto out;
        }
        free(firmware);
        firmware = NULL;

        if (!c->skip_nvm && nvm_name != NULL) {
            uint8_t bid_sub = HMOS_BT_EDL_SUB_BOARD_ID;
            uint8_t ev[255];
            size_t ev_len = 0;
            uint8_t b0 = 0, b1 = 0;
            int bid_valid = 0;
            if (hmos_bt_hci_command(tty_fd, HMOS_BT_EDL_OPCODE, &bid_sub, 1, NULL, NULL, c->timeout_ms) == 0 &&
                hmos_bt_wait_cc(tty_fd, HMOS_BT_EDL_OPCODE, 0x00, ev, &ev_len, c->timeout_ms) == 0 &&
                ev_len >= 8 && ev[4] == HMOS_BT_EDL_SUB_BOARD_ID && ev[5] == 0x02) {
                b0 = ev[6];
                b1 = ev[7];
                bid_valid = 1;
                hmos_bt_log("BOARD_ID b0=0x%02x b1=0x%02x", b0, b1);
            }
            char chosen_nvm[128];
            hmos_bt_select_nvm_by_bid(c->firmware_dir, nvm_name, b0, b1, bid_valid,
                                      chosen_nvm, sizeof(chosen_nvm));
            if (hmos_bt_firmware_load(c->firmware_dir, chosen_nvm, &firmware, &firmware_size) != 0) {
                hmos_bt_log("BRINGUP_FAILED stage=load_nvm name=%s", chosen_nvm);
                goto out;
            }
            uint8_t bcode = 0;
            if (hmos_bt_qca_baud_code(active_baud, &bcode) == 0) {
                int ipa = 0;
                if (hmos_bt_patch_nvm_sibs_off(firmware, firmware_size, bcode, &ipa) == 0 && ipa) {
                    (void)hmos_bt_power_ioctl(c->power_path, HMOS_BT_IPA_TCS_IOCTL, 0);
                    hmos_bt_log("IPA_TCS_CONFIGURED=1");
                }
            }
            if (hmos_bt_download_tlv_typed(tty_fd, firmware, firmware_size, HMOS_BT_TLV_TYPE_NVM,
                                           HMOS_BT_SKIP_EVT_NONE, ver.unified, 1, c->timeout_ms) != 0) {
                hmos_bt_log("BRINGUP_FAILED stage=download_nvm name=%s bytes=%zu", chosen_nvm,
                            firmware_size);
                free(firmware);
                firmware = NULL;
                goto out;
            }
            free(firmware);
            firmware = NULL;
        }

        uint8_t bld_sub = HMOS_BT_EDL_SUB_BUILD_INFO;
        uint8_t ev[255];
        size_t ev_len = 0;
        if (hmos_bt_hci_command(tty_fd, HMOS_BT_EDL_OPCODE, &bld_sub, 1, NULL, NULL, c->timeout_ms) == 0 &&
            hmos_bt_wait_cc(tty_fd, HMOS_BT_EDL_OPCODE, 0x00, ev, &ev_len, c->timeout_ms) == 0 &&
            ev_len > 6) {
            char label[128];
            size_t n = 0;
            for (size_t i = 5; i < ev_len && n < sizeof(label) - 1; i++) {
                if (ev[i] >= 32 && ev[i] < 127) label[n++] = (char)ev[i];
            }
            label[n] = '\0';
            if (n > 0) hmos_bt_log("BUILD_INFO=%s", label);
        }
    }

    rc = hmos_bt_reset_then_serve(tty_fd, c);

out:
    if (tty_fd >= 0) {
        if (restore_t2) (void)ioctl(tty_fd, HMOS_BT_TCSETSW2, &old_t2);
        if (restore_lines) (void)ioctl(tty_fd, TIOCMSET, &old_modem);
        if (pm_voted) {
            if (ioctl(tty_fd, HMOS_BT_UART_PM_PUT, 0) != 0 && errno == EAGAIN) {
                usleep(150000);
                (void)ioctl(tty_fd, HMOS_BT_UART_PM_PUT, 0);
            }
            hmos_bt_log("UART_PM_RELEASED=1");
        }
        close(tty_fd);
    }
    if (powered) {
        (void)hmos_bt_power_set(c->power_path, 0);
    }
    if (rc != 0) {
        hmos_bt_log("BRIDGE_EXIT rc=%d errno=%d (%s)", rc, errno, strerror(errno));
    } else {
        hmos_bt_log("BRIDGE_EXIT rc=0");
    }
    return rc;
}

static void hmos_bt_usage(FILE *out)
{
    fprintf(out,
            "usage: hmos_bt_bridge_marble [options]\n"
            "  --tty PATH           HCI UART (default %s)\n"
            "  --power PATH         btpower node (default %s)\n"
            "  --firmware-dir DIR   read-only bluetooth_a image dir (default %s)\n"
            "  --rampatch NAME      rampatch file (default %s)\n"
            "  --nvm NAME           NVM file (default %s)\n"
            "  --skip-nvm           do not download NVM\n"
            "  --socket PATH        H4 socket (default %s)\n"
            "  --baud N             initial baud (default 115200)\n"
            "  --post-baud N        operating baud (0=auto 3200000 on HSP2_1, -1=keep)\n"
            "  --wait-each-segment  wait for an event after every TLV segment\n"
            "  --timeout-ms N       per command timeout (default 2000)\n"
            "  --max-sessions N     0 = serve forever (default 1)\n"
            "  --no-download        skip EDL firmware download\n"
            "  --check              validate paths, touch no hardware\n"
            "  --log PATH           duplicate log to file\n",
            HMOS_BT_DEFAULT_TTY, HMOS_BT_DEFAULT_POWER, HMOS_BT_DEFAULT_FW_DIR,
            HMOS_BT_DEFAULT_RAMPATCH, HMOS_BT_DEFAULT_NVM, HMOS_BT_DEFAULT_SOCKET);
}

static int hmos_bt_parse_args(int argc, char **argv, struct hmos_bt_config *cfg)
{
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            hmos_bt_usage(stdout);
            return 1;
        } else if (!strcmp(arg, "--check")) {
            cfg->check_only = 1;
        } else if (!strcmp(arg, "--tty") && val) {
            cfg->tty_path = val;
            i++;
        } else if (!strcmp(arg, "--power") && val) {
            cfg->power_path = val;
            i++;
        } else if (!strcmp(arg, "--firmware-dir") && val) {
            cfg->firmware_dir = val;
            i++;
        } else if (!strcmp(arg, "--rampatch") && val) {
            cfg->rampatch = val;
            i++;
        } else if (!strcmp(arg, "--nvm") && val) {
            cfg->nvm = val;
            i++;
        } else if (!strcmp(arg, "--skip-nvm")) {
            cfg->skip_nvm = 1;
        } else if (!strcmp(arg, "--socket") && val) {
            cfg->socket_path = val;
            i++;
        } else if (!strcmp(arg, "--baud") && val) {
            cfg->baud = atoi(val);
            i++;
        } else if (!strcmp(arg, "--post-baud") && val) {
            cfg->post_baud = atoi(val);
            i++;
        } else if (!strcmp(arg, "--wait-each-segment")) {
            cfg->seg_wait = 1;
        } else if (!strcmp(arg, "--timeout-ms") && val) {
            cfg->timeout_ms = atoi(val);
            i++;
        } else if (!strcmp(arg, "--max-sessions") && val) {
            cfg->max_sessions = (unsigned)atoi(val);
            i++;
        } else if (!strcmp(arg, "--no-download")) {
            cfg->no_download = 1;
        } else if (!strcmp(arg, "--log") && val) {
            cfg->log_path = val;
            i++;
        } else {
            fprintf(stderr, "hmos_bt_bridge_marble: unknown or incomplete option '%s'\n", arg);
            hmos_bt_usage(stderr);
            return -1;
        }
    }
    return 0;
}

HMOS_BT_EXPORT int hmos_bt_bridge_main(int argc, char **argv)
{
    struct hmos_bt_config cfg;
    int parsed;
    int rc;

    hmos_bt_bridge_defaults(&cfg);
    parsed = hmos_bt_parse_args(argc, argv, &cfg);
    if (parsed != 0) {
        return parsed > 0 ? 0 : 2;
    }
    if (cfg.log_path != NULL) {
        int fd = open(cfg.log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (fd < 0) {
            hmos_bt_log("LOG_ERROR path=%s errno=%d (%s)", cfg.log_path, errno, strerror(errno));
        } else {
            if (dup2(fd, STDERR_FILENO) < 0) {
                hmos_bt_log("LOG_ERROR path=%s op=dup2 errno=%d (%s)", cfg.log_path, errno,
                            strerror(errno));
            }
            if (fd > STDERR_FILENO) {
                close(fd);
            }
        }
    }
    rc = hmos_bt_bridge_run(&cfg);
    return rc == 0 ? 0 : 1;
}

#ifndef HMOS_BT_BRIDGE_NO_MAIN
int main(int argc, char **argv)
{
    return hmos_bt_bridge_main(argc, argv);
}
#endif
