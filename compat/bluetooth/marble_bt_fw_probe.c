/* SPDX-License-Identifier: MIT */
/* Marble QCA6490 staged Bluetooth bring-up verifier (Round 46).
 * Verifies real hardware baud-rate switch, stock version-table firmware selection
 * (hpbtfw21.tlv + Board-ID/default hpnv21g.*), NVM SIBS-disable patching,
 * TLV streaming, HCI Reset, Read Local Version, and Read BD_ADDR, then cleanly
 * releases UART clock vote and powers off. Never modifies read-only firmware. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Kernel ABI constants: include/linux/btpower.h, include/uapi/linux/msm_geni_serial.h */
enum {
    BT_POWER = 0xbfad,
    BT_CHIPSET_VERS = 0xbfae,
    BT_CHIP_ID = 0xbfaf,
    BT_SET_IPA_TCS = 0xbfc0,
    UART_PM_GET = 0x54ed,
    UART_PM_PUT = 0x54ee,
    UART_PM_ACTIVE = 0x54ef,
};

/* Linux asm-generic/termbits.h struct termios2 (44 bytes on aarch64 & x86_64) */
struct qca_termios2 {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};
_Static_assert(sizeof(struct qca_termios2) == 44, "termios2 ABI mismatch");

enum {
    QCA_TCGETS2 = 0x802c542aU,
    QCA_TCSETSW2 = 0x402c542cU,
    QCA_CBAUD_MASK = 0x100fU,
    QCA_BOTHER = 0x1000U,
};

enum {
    QCA_EDL_OPCODE = 0xfc00,
    QCA_BAUD_OPCODE = 0xfc48,
    HCI_OP_RESET = 0x0c03,
    HCI_OP_READ_LOCAL_VER = 0x1001,
    HCI_OP_READ_BD_ADDR = 0x1009,
    QCA_EDL_SUB_VER = 0x19,
    QCA_EDL_SUB_TLV = 0x1e,
    QCA_EDL_SUB_BUILD = 0x20,
    QCA_EDL_SUB_BOARD_ID = 0x23,
    QCA_TLV_MAX_SEG = 243,
    QCA_TLV_TYPE_PATCH = 1,
    QCA_TLV_TYPE_NVM = 2,
    QCA_SKIP_EVT_NONE = 0,
    QCA_SKIP_EVT_VSE_CC = 3,
};

struct rom_version {
    uint32_t product, soc;
    uint16_t patch, rom;
    bool unified;
};

struct nvm_patch_info {
    bool saw_tag17;
    bool saw_tag27;
    bool saw_tag35;
    bool saw_tag2;
    bool ipa_supported;
    uint8_t tag17_old[2];
    uint8_t tag17_new[2];
    uint8_t tag27_old[2];
    uint8_t tag27_new[2];
    uint8_t tag2_bdaddr[6];
};

struct local_version {
    uint8_t hci_ver;
    uint16_t hci_rev;
    uint8_t lmp_ver;
    uint16_t manufacturer;
    uint16_t lmp_subver;
};

typedef int (*baud_switch_fn)(int fd, uint32_t baud, void *ctx);

static volatile sig_atomic_t cancelled;

static void cancel(int sig)
{
    (void)sig;
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

static uint64_t chipset_key(const struct rom_version *v)
{
    return ((uint64_t)v->soc << 32) | ((uint64_t)(v->product & 0xffffU) << 16) | (uint64_t)v->rom;
}

static int qca_baud_code(uint32_t baud, uint8_t *code)
{
    switch (baud) {
    case 115200: *code = 0x00; return 0;
    case 921600: *code = 0x0a; return 0;
    case 1000000: *code = 0x0b; return 0;
    case 2000000: *code = 0x0d; return 0;
    case 3000000: *code = 0x0e; return 0;
    case 4000000: *code = 0x0f; return 0;
    case 3200000: *code = 0x11; return 0;
    default: errno = EINVAL; return -1;
    }
}

static int fail(const char *stage)
{
    int error = errno;
    fprintf(stderr, "BT_FW_FAIL stage=%s errno=%d (%s)\n", stage, error, strerror(error));
    return 1;
}

static int io_exact(int fd, uint8_t *data, size_t length, int64_t deadline, bool tx)
{
    while (length) {
        if (cancelled) { errno = EINTR; return -1; }
        int64_t t = now_ms();
        if (t < 0) return -1;
        if (t >= deadline) { errno = ETIMEDOUT; return -1; }
        struct pollfd pfd = { .fd = fd, .events = tx ? POLLOUT : POLLIN };
        int ready = poll(&pfd, 1, (int)(deadline - t));
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

static int read_h4_event(int fd, uint8_t *code, uint8_t *params, size_t *plen, int64_t deadline)
{
    uint8_t hdr[3];
    if (io_exact(fd, hdr, 1, deadline, false) < 0) return -1;
    if (hdr[0] != 0x04) {
        printf("BT_FW_UNEXPECTED_H4_TYPE=0x%02x\n", (unsigned)hdr[0]);
        errno = EPROTO;
        return -1;
    }
    if (io_exact(fd, hdr + 1, 2, deadline, false) < 0) return -1;
    size_t size = hdr[2];
    if (io_exact(fd, params, size, deadline, false) < 0) return -1;
    *code = hdr[1];
    *plen = size;
    return 0;
}

static int send_hci_cmd(int fd, uint16_t opcode, const uint8_t *params, uint8_t plen, int64_t deadline)
{
    uint8_t pkt[4 + 255];
    pkt[0] = 0x01;
    pkt[1] = (uint8_t)(opcode & 0xffU);
    pkt[2] = (uint8_t)(opcode >> 8);
    pkt[3] = plen;
    if (plen && params) memcpy(pkt + 4, params, plen);
    return io_exact(fd, pkt, (size_t)4 + plen, deadline, true);
}

static void print_hex(const char *prefix, const uint8_t *buf, size_t len)
{
    printf("%s len=%zu hex=", prefix, len);
    size_t shown = len < 32 ? len : 32;
    for (size_t i = 0; i < shown; i++) printf("%02x", (unsigned)buf[i]);
    if (len > shown) printf("...");
    putchar('\n');
}

static int parse_version(struct rom_version *v, const uint8_t *data, bool unified)
{
    *v = (struct rom_version){
        .product = get32(data),
        .patch = get16(data + 4),
        .rom = get16(data + 6),
        .soc = get32(data + 8),
        .unified = unified,
    };
    if (v->soc && v->soc != UINT32_MAX && v->product != UINT32_MAX && v->rom != UINT16_MAX) return 0;
    errno = EPROTO;
    return -1;
}

static int query_version(int fd, int timeout_ms, struct rom_version *version)
{
    int64_t start = now_ms();
    if (start < 0) return -1;
    int64_t deadline = start + timeout_ms;
    uint8_t sub = QCA_EDL_SUB_VER;
    if (send_hci_cmd(fd, QCA_EDL_OPCODE, &sub, 1, deadline) < 0) return -1;
    bool legacy = false;
    for (;;) {
        uint8_t code = 0, ev[255];
        size_t size = 0;
        if (read_h4_event(fd, &code, ev, &size, deadline) < 0) return -1;
        if (code == 0xff && size >= 2 && ev[0] == 0x00 && ev[1] == 0x02) {
            if (size != 14 || legacy) { errno = EPROTO; return -1; }
            if (parse_version(version, ev + 2, false) < 0) return -1;
            legacy = true;
        } else if (code == 0x0e && size >= 3 && get16(ev + 1) == QCA_EDL_OPCODE) {
            if (size < 4 || ev[3] != 0) { errno = EPROTO; return -1; }
            if (size == 18 && ev[4] == QCA_EDL_SUB_VER && !legacy) {
                return parse_version(version, ev + 6, true);
            }
            if (size != 4) { errno = EPROTO; return -1; }
            if (legacy) return 0;
        } else if (code == 0x0f && size == 4 && get16(ev + 2) == QCA_EDL_OPCODE && ev[0] != 0) {
            errno = EPROTO;
            return -1;
        }
    }
}

/* Wait for Command Complete matching opcode with status == 0. */
static int wait_cmd_complete(int fd, uint16_t opcode, uint8_t *out_ev, size_t *out_len, int timeout_ms)
{
    int64_t start = now_ms();
    if (start < 0) return -1;
    int64_t deadline = start + timeout_ms;
    for (;;) {
        uint8_t code = 0, ev[255];
        size_t size = 0;
        if (read_h4_event(fd, &code, ev, &size, deadline) < 0) return -1;
        if (code == 0x0f && size == 4 && get16(ev + 2) == opcode && ev[0] != 0) {
            printf("BT_FW_CMD_STATUS_ERR opcode=0x%04x status=0x%02x\n", (unsigned)opcode, (unsigned)ev[0]);
            errno = EPROTO;
            return -1;
        }
        if (code == 0x0e && size >= 4 && get16(ev + 1) == opcode) {
            if (ev[3] != 0) {
                printf("BT_FW_CMD_COMPLETE_ERR opcode=0x%04x status=0x%02x\n", (unsigned)opcode, (unsigned)ev[3]);
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

static int set_baud_rate_proto(int fd, uint32_t target_baud, bool unified, int timeout_ms,
                               baud_switch_fn switch_host, void *ctx)
{
    uint8_t code = 0;
    if (qca_baud_code(target_baud, &code) < 0) return -1;
    int64_t start = now_ms();
    if (start < 0) return -1;
    int64_t deadline = start + timeout_ms;
    if (send_hci_cmd(fd, QCA_BAUD_OPCODE, &code, 1, deadline) < 0) return -1;
    if (switch_host && switch_host(fd, target_baud, ctx) < 0) return -1;

    start = now_ms();
    if (start < 0) return -1;
    deadline = start + timeout_ms;
    bool saw_vse_92 = false;
    for (;;) {
        uint8_t ev_code = 0, ev[255];
        size_t size = 0;
        if (read_h4_event(fd, &ev_code, ev, &size, deadline) < 0) return -1;
        printf("BT_FW_BAUD_EV code=0x%02x ", (unsigned)ev_code);
        print_hex("payload", ev, size);
        if (ev_code == 0xff && size >= 1 && ev[0] == 0x92) {
            saw_vse_92 = true;
            continue;
        }
        if (ev_code == 0x0f && size == 4 && get16(ev + 2) == QCA_BAUD_OPCODE && ev[0] != 0) {
            errno = EPROTO;
            return -1;
        }
        if (ev_code == 0x0e && size >= 4 && get16(ev + 1) == QCA_BAUD_OPCODE) {
            /* Stock GetVsHciEvent@0x55c2c: unified/legacy 0xfc48 success status is 0x01 (BAUDRATE_CHANGE_SUCCESS). */
            if (ev[3] != 0x01) { errno = EPROTO; return -1; }
            if (!unified && !saw_vse_92) { errno = EPROTO; return -1; }
            print_hex("BT_FW_BAUD_CC", ev, size);
            return 0;
        }
    }
}

static int parse_and_patch_nvm(uint8_t *buf, size_t len, uint8_t baud_code,
                               bool disable_sibs, struct nvm_patch_info *info)
{
    memset(info, 0, sizeof(*info));
    if (len < 4 || buf[0] != QCA_TLV_TYPE_NVM) { errno = EPROTO; return -1; }
    uint32_t tlv_len = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16);
    if ((size_t)tlv_len + 4 != len) { errno = EPROTO; return -1; }
    size_t pos = 4;
    while (pos < len) {
        if (len - pos < 12) { errno = EPROTO; return -1; }
        uint16_t tag_id = get16(buf + pos);
        uint16_t tag_len = get16(buf + pos + 2);
        if (len - (pos + 12) < tag_len) { errno = EPROTO; return -1; }
        uint8_t *data = buf + pos + 12;
        if (tag_id == 2 && tag_len == 6) {
            info->saw_tag2 = true;
            memcpy(info->tag2_bdaddr, data, 6);
        } else if (tag_id == 17 && tag_len >= 2) {
            info->saw_tag17 = true;
            info->tag17_old[0] = data[0];
            info->tag17_old[1] = data[1];
            if (disable_sibs) data[0] &= (uint8_t)~0x80U;
            data[1] = baud_code;
            info->tag17_new[0] = data[0];
            info->tag17_new[1] = data[1];
        } else if (tag_id == 27 && tag_len >= 2) {
            /* Stock UpdateNewNvmFormat@0x60034..0x600a8: offset +0xd (data[1]) bit 0 is SIBS. */
            info->saw_tag27 = true;
            info->tag27_old[0] = data[0];
            info->tag27_old[1] = data[1];
            if (disable_sibs) data[1] &= (uint8_t)~0x01U;
            info->tag27_new[0] = data[0];
            info->tag27_new[1] = data[1];
        } else if (tag_id == 35 && tag_len >= 0x77) {
            /* Stock UpdateNewNvmFormat@0x60178: hdr+0x82 == data[0x76] bit 2 -> iPA TCS. */
            info->saw_tag35 = true;
            info->ipa_supported = (data[0x76] & 0x04U) != 0;
        }
        pos += 12 + tag_len;
    }
    return 0;
}

static int validate_rampatch(const uint8_t *buf, size_t len, const struct rom_version *v,
                             uint8_t *download_mode, uint16_t *patch_ver)
{
    if (len < 40 || buf[0] != QCA_TLV_TYPE_PATCH) { errno = EPROTO; return -1; }
    uint32_t tlv_len = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16);
    if ((size_t)tlv_len + 4 != len) { errno = EPROTO; return -1; }
    uint16_t prod = get16(buf + 16);
    uint16_t rom = get16(buf + 18);
    uint16_t pver = get16(buf + 20);
    if (prod != (uint16_t)(v->product & 0xffffU) || rom != v->rom) {
        errno = EPROTO;
        return -1;
    }
    *download_mode = buf[14];
    *patch_ver = pver;
    return 0;
}

static int download_tlv_stream(int fd, const uint8_t *buf, size_t len, uint8_t tlv_type,
                               uint8_t download_mode, bool unified, int timeout_ms,
                               unsigned *segments_out)
{
    size_t offset = 0;
    unsigned segs = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > QCA_TLV_MAX_SEG) chunk = QCA_TLV_MAX_SEG;
        bool is_last = (offset + chunk == len);
        bool wait_evt = true;
        if (tlv_type == QCA_TLV_TYPE_PATCH && download_mode == QCA_SKIP_EVT_VSE_CC) {
            wait_evt = is_last;
        }
        uint8_t params[2 + QCA_TLV_MAX_SEG];
        params[0] = QCA_EDL_SUB_TLV;
        params[1] = (uint8_t)chunk;
        memcpy(params + 2, buf + offset, chunk);
        int64_t start = now_ms();
        if (start < 0) return -1;
        if (send_hci_cmd(fd, QCA_EDL_OPCODE, params, (uint8_t)(2 + chunk), start + timeout_ms) < 0) {
            return -1;
        }
        if (wait_evt) {
            uint8_t ev[255];
            size_t ev_len = 0;
            if (unified) {
                if (wait_cmd_complete(fd, QCA_EDL_OPCODE, ev, &ev_len, timeout_ms) < 0) return -1;
                if (ev_len >= 5 && ev[4] != QCA_EDL_SUB_TLV) { errno = EPROTO; return -1; }
                if (ev_len >= 6 && ev[5] != 0) { errno = EPROTO; return -1; }
                if (is_last) print_hex("BT_FW_TLV_LAST_CC", ev, ev_len);
            } else {
                int64_t dl = now_ms() + timeout_ms;
                bool saw_vse = false;
                for (;;) {
                    uint8_t code = 0;
                    if (read_h4_event(fd, &code, ev, &ev_len, dl) < 0) return -1;
                    if (code == 0xff && ev_len >= 3 && ev[0] == 0x00 && ev[1] == 0x04) {
                        if (ev[2] != 0) { errno = EPROTO; return -1; }
                        saw_vse = true;
                    } else if (code == 0x0e && ev_len == 4 && get16(ev + 1) == QCA_EDL_OPCODE) {
                        if (ev[3] != 0 || !saw_vse) { errno = EPROTO; return -1; }
                        break;
                    }
                }
            }
        }
        offset += chunk;
        segs++;
    }
    if (segments_out) *segments_out = segs;
    return 0;
}

static int query_board_id(int fd, bool unified, int timeout_ms, uint8_t *b0, uint8_t *b1, bool *valid)
{
    *b0 = 0;
    *b1 = 0;
    *valid = false;
    int64_t start = now_ms();
    if (start < 0) return -1;
    uint8_t sub = QCA_EDL_SUB_BOARD_ID;
    if (send_hci_cmd(fd, QCA_EDL_OPCODE, &sub, 1, start + timeout_ms) < 0) return -1;
    uint8_t ev[255];
    size_t ev_len = 0;
    if (unified) {
        if (wait_cmd_complete(fd, QCA_EDL_OPCODE, ev, &ev_len, timeout_ms) < 0) return -1;
        print_hex("BT_FW_BOARD_ID_CC", ev, ev_len);
        /* Stock HandleEdlCmdResEvt@0x5626c..0x5669c: plen>=8, ev[4]=0x23, ev[5]=0x02, ev[6]=b0, ev[7]=b1 */
        if (ev_len >= 8 && ev[4] == QCA_EDL_SUB_BOARD_ID && ev[5] == 0x02) {
            *b0 = ev[6];
            *b1 = ev[7];
            *valid = true;
        }
        return 0;
    }
    if (wait_cmd_complete(fd, QCA_EDL_OPCODE, ev, &ev_len, timeout_ms) < 0) return -1;
    print_hex("BT_FW_BOARD_ID_CC", ev, ev_len);
    return 0;
}

static int query_build_info(int fd, int timeout_ms)
{
    int64_t start = now_ms();
    if (start < 0) return -1;
    uint8_t sub = QCA_EDL_SUB_BUILD;
    if (send_hci_cmd(fd, QCA_EDL_OPCODE, &sub, 1, start + timeout_ms) < 0) return -1;
    uint8_t ev[255];
    size_t ev_len = 0;
    if (wait_cmd_complete(fd, QCA_EDL_OPCODE, ev, &ev_len, timeout_ms) < 0) return -1;
    print_hex("BT_FW_BUILD_INFO_CC", ev, ev_len);
    if (ev_len > 6) {
        char text[256];
        size_t n = 0;
        for (size_t i = 5; i < ev_len && n < sizeof(text) - 1; i++) {
            uint8_t c = ev[i];
            if (c >= 32 && c < 127) text[n++] = (char)c;
        }
        text[n] = '\0';
        if (n > 0) printf("BT_FW_BUILD_INFO_TEXT=%s\n", text);
    }
    return 0;
}

static int hci_reset_strict(int fd, int timeout_ms)
{
    int64_t start = now_ms();
    if (start < 0) return -1;
    if (send_hci_cmd(fd, HCI_OP_RESET, NULL, 0, start + timeout_ms) < 0) return -1;
    uint8_t ev[255];
    size_t ev_len = 0;
    if (wait_cmd_complete(fd, HCI_OP_RESET, ev, &ev_len, timeout_ms) < 0) return -1;
    if (ev_len != 4) { errno = EPROTO; return -1; }
    puts("BT_FW_HCI_RESET_OK=1");
    return 0;
}

static int hci_read_local_version(int fd, int timeout_ms, struct local_version *lv)
{
    int64_t start = now_ms();
    if (start < 0) return -1;
    if (send_hci_cmd(fd, HCI_OP_READ_LOCAL_VER, NULL, 0, start + timeout_ms) < 0) return -1;
    uint8_t ev[255];
    size_t ev_len = 0;
    if (wait_cmd_complete(fd, HCI_OP_READ_LOCAL_VER, ev, &ev_len, timeout_ms) < 0) return -1;
    if (ev_len != 12) { errno = EPROTO; return -1; }
    *lv = (struct local_version){
        .hci_ver = ev[4],
        .hci_rev = get16(ev + 5),
        .lmp_ver = ev[7],
        .manufacturer = get16(ev + 8),
        .lmp_subver = get16(ev + 10),
    };
    return 0;
}

static int hci_read_bd_addr(int fd, int timeout_ms, uint8_t bd[6])
{
    int64_t start = now_ms();
    if (start < 0) return -1;
    if (send_hci_cmd(fd, HCI_OP_READ_BD_ADDR, NULL, 0, start + timeout_ms) < 0) return -1;
    uint8_t ev[255];
    size_t ev_len = 0;
    if (wait_cmd_complete(fd, HCI_OP_READ_BD_ADDR, ev, &ev_len, timeout_ms) < 0) return -1;
    if (ev_len != 10) { errno = EPROTO; return -1; }
    memcpy(bd, ev + 4, 6);
    return 0;
}

static int read_file_exact(const char *path, uint8_t **out_buf, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 2 * 1024 * 1024) {
        int e = errno ? errno : EINVAL;
        close(fd);
        errno = e;
        return -1;
    }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = malloc(sz);
    if (!buf) { close(fd); errno = ENOMEM; return -1; }
    size_t done = 0;
    while (done < sz) {
        ssize_t n = read(fd, buf + done, sz - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { int e = n < 0 ? errno : EIO; free(buf); close(fd); errno = e; return -1; }
        done += (size_t)n;
    }
    close(fd);
    *out_buf = buf;
    *out_len = sz;
    return 0;
}

static int select_nvm_filename(const char *fw_dir, const char *base_nvm, bool bid_valid,
                               uint8_t b0, uint8_t b1, char *chosen, size_t chosen_sz)
{
    char cand[4][64];
    size_t n_cand = 0;
    if (bid_valid) {
        if (b0 == 0) {
            snprintf(cand[n_cand++], sizeof(cand[0]), "hpnv21g.b%02x", (unsigned)b1);
        } else {
            snprintf(cand[n_cand++], sizeof(cand[0]), "hpnv21g.b%02x%02x", (unsigned)b0, (unsigned)b1);
            snprintf(cand[n_cand++], sizeof(cand[0]), "hpnv21g.b%x%02x", (unsigned)b0, (unsigned)b1);
            snprintf(cand[n_cand++], sizeof(cand[0]), "hpnv21g.%x%02x", (unsigned)b0, (unsigned)b1);
        }
    }
    snprintf(cand[n_cand++], sizeof(cand[0]), "%s", base_nvm);
    for (size_t i = 0; i < n_cand; i++) {
        char full[512];
        if (snprintf(full, sizeof(full), "%s/%s", fw_dir, cand[i]) < (int)sizeof(full) &&
            access(full, R_OK) == 0) {
            snprintf(chosen, chosen_sz, "%s", cand[i]);
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

/* ---------- Hardware UART / btpower helpers ---------- */

static int char_open(const char *path, int flags)
{
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | flags);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
        int e = errno ? errno : ENODEV;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static int check_chipset(int power_fd)
{
    static const char expected[] = "qcom,qca6490";
    char value[32] = {0};
    if (ioctl(power_fd, BT_CHIP_ID, value) != 0) return -1;
    if (memcmp(value, expected, sizeof(expected)) != 0) { errno = ENODEV; return -1; }
    puts("BT_FW_CHIPSET=qcom,qca6490");
    return 0;
}

static int set_rts(int tty, bool high)
{
    int modem = 0;
    if (ioctl(tty, TIOCMGET, &modem) != 0) return -1;
    if (high) modem |= TIOCM_RTS;
    else modem &= ~TIOCM_RTS;
    if (ioctl(tty, TIOCMSET, &modem) != 0) return -1;
    return 0;
}

static int host_set_baud_termios2(int tty, uint32_t baud)
{
    struct qca_termios2 t2;
    memset(&t2, 0, sizeof(t2));
    if (ioctl(tty, QCA_TCGETS2, &t2) != 0) return -1;
    t2.c_cflag &= ~QCA_CBAUD_MASK;
    t2.c_cflag |= QCA_BOTHER;
    t2.c_ispeed = baud;
    t2.c_ospeed = baud;
    if (ioctl(tty, QCA_TCSETSW2, &t2) != 0) return -1;
    struct qca_termios2 rb;
    memset(&rb, 0, sizeof(rb));
    if (ioctl(tty, QCA_TCGETS2, &rb) != 0) return -1;
    printf("BT_FW_HOST_BAUD_READBACK ispeed=%u ospeed=%u cflag=0x%x\n",
           rb.c_ispeed, rb.c_ospeed, rb.c_cflag);
    if (rb.c_ispeed != baud || rb.c_ospeed != baud) { errno = EIO; return -1; }
    return 0;
}

struct uart_baud_ctx {
    bool flow_toggled;
};

static int uart_switch_baud_cb(int tty, uint32_t baud, void *vctx)
{
    struct uart_baud_ctx *ctx = vctx;
    /* Stock SetBaudRateReq@0x590b4..0x5919c: usleep(20000), tcdrain, SetBaudRate, Flow ON */
    const struct timespec wait20 = { .tv_nsec = 20000000 };
    if (nanosleep(&wait20, NULL) != 0 || cancelled) return -1;
    if (tcdrain(tty) != 0) return -1;
    if (host_set_baud_termios2(tty, baud) != 0) return -1;
    if (set_rts(tty, true) != 0) return -1;
    ctx->flow_toggled = true;
    return 0;
}

static int setup_uart_initial(int fd, struct qca_termios2 *old_t2, bool *restore)
{
    int line = -1;
    if (ioctl(fd, TIOCEXCL, 0) != 0 || ioctl(fd, TIOCGETD, &line) != 0) return -1;
    if (line != 0) { errno = EBUSY; return -1; }
    if (ioctl(fd, QCA_TCGETS2, old_t2) != 0) return -1;
    struct termios raw;
    if (tcgetattr(fd, &raw) != 0) return -1;
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

static int run_hardware(int power, const char *uart_path, const char *fw_dir,
                        uint32_t target_baud, bool baud_only)
{
    int tty = -1, rc = 1;
    bool powered = false, voted = false, restore = false, restore_lines = false;
    int old_modem = 0;
    struct qca_termios2 old_t2;
    memset(&old_t2, 0, sizeof(old_t2));

    if (cancelled) return 1;
    if (ioctl(power, BT_POWER, 0) != 0) return fail("power_baseline");
    powered = true;
    if (ioctl(power, BT_POWER, 1) != 0) { fail("power_on"); goto out; }
    puts("BT_FW_POWER_ON=1");
    const struct timespec settle = { .tv_nsec = 250000000 };
    if (nanosleep(&settle, NULL) != 0 || cancelled) goto out;

    tty = char_open(uart_path, O_NOCTTY);
    if (tty < 0 || setup_uart_initial(tty, &old_t2, &restore) != 0) {
        fail("uart_open_or_setup"); goto out;
    }
    if (ioctl(tty, UART_PM_GET, 0) != 0) { fail("uart_pm_get"); goto out; }
    voted = true;
    int active = ioctl(tty, UART_PM_ACTIVE, 0);
    if (active != 1) { if (active == 0) errno = EIO; fail("uart_pm_active"); goto out; }
    if (ioctl(tty, TIOCMGET, &old_modem) != 0) { fail("uart_get_lines"); goto out; }
    restore_lines = true;
    if (set_rts(tty, true) != 0 || tcflush(tty, TCIOFLUSH) != 0) { fail("uart_set_lines"); goto out; }

    struct rom_version ver = {0};
    if (query_version(tty, 2400, &ver) != 0) { fail("rom_version"); goto out; }
    uint64_t key = chipset_key(&ver);
    printf("BT_FW_ROM_OK unified=%d product=0x%08" PRIx32 " soc=0x%08" PRIx32
           " rom=0x%04x patch=0x%04x key=0x%016" PRIx64 "\n",
           ver.unified ? 1 : 0, ver.product, ver.soc, (unsigned)ver.rom, (unsigned)ver.patch, key);

    /* Notify btpower of SoC version just like stock HandleEdlCmdResEvt@0x5642c */
    if (ioctl(power, BT_CHIPSET_VERS, (unsigned long)ver.soc) != 0) {
        printf("BT_FW_WARN_CHIPSET_VERS errno=%d\n", errno);
    }

    /* Baud switch: Flow OFF before sending 0xfc48 (stock SetBaudRateReq@0x58ff8) */
    if (set_rts(tty, false) != 0) { fail("flow_off_baud"); goto out; }
    struct uart_baud_ctx bctx = {0};
    if (set_baud_rate_proto(tty, target_baud, ver.unified, 2000, uart_switch_baud_cb, &bctx) != 0) {
        fail("baud_switch"); goto out;
    }
    printf("BT_FW_BAUD_OK target=%u\n", target_baud);

    if (baud_only) {
        rc = 0;
        goto out;
    }

    if (key != 0x400c121100130201ULL) {
        errno = ENOTSUP;
        fail("unsupported_chipset_key");
        goto out;
    }

    /* 1. Rampatch download (hpbtfw21.tlv) */
    char path[512];
    snprintf(path, sizeof(path), "%s/hpbtfw21.tlv", fw_dir);
    uint8_t *patch_buf = NULL;
    size_t patch_len = 0;
    if (read_file_exact(path, &patch_buf, &patch_len) != 0) { fail("load_rampatch"); goto out; }
    uint8_t dnld_mode = 0;
    uint16_t patch_ver = 0;
    if (validate_rampatch(patch_buf, patch_len, &ver, &dnld_mode, &patch_ver) != 0) {
        free(patch_buf);
        fail("validate_rampatch");
        goto out;
    }
    printf("BT_FW_RAMPATCH_META file=hpbtfw21.tlv bytes=%zu dnld_mode=0x%02x patch_ver=0x%04x\n",
           patch_len, (unsigned)dnld_mode, (unsigned)patch_ver);
    unsigned patch_segs = 0;
    int dl_rc = download_tlv_stream(tty, patch_buf, patch_len, QCA_TLV_TYPE_PATCH, dnld_mode,
                                    ver.unified, 4000, &patch_segs);
    free(patch_buf);
    if (dl_rc != 0) { fail("download_rampatch"); goto out; }
    printf("BT_FW_RAMPATCH_OK bytes=%zu segments=%u\n", patch_len, patch_segs);

    /* 2. Query real hardware Board ID after rampatch */
    uint8_t b0 = 0, b1 = 0;
    bool bid_valid = false;
    if (query_board_id(tty, ver.unified, 2000, &b0, &b1, &bid_valid) != 0) {
        printf("BT_FW_BOARD_ID_WARN errno=%d (falling back to default NVM)\n", errno);
    } else {
        printf("BT_FW_BOARD_ID valid=%d b0=0x%02x b1=0x%02x\n", bid_valid ? 1 : 0, (unsigned)b0, (unsigned)b1);
    }

    /* 3. Select, patch (SIBS disable + baud match + iPA check), and download NVM */
    char nvm_name[64];
    if (select_nvm_filename(fw_dir, "hpnv21g.bin", bid_valid, b0, b1, nvm_name, sizeof(nvm_name)) != 0) {
        fail("select_nvm"); goto out;
    }
    snprintf(path, sizeof(path), "%s/%s", fw_dir, nvm_name);
    uint8_t *nvm_buf = NULL;
    size_t nvm_len = 0;
    if (read_file_exact(path, &nvm_buf, &nvm_len) != 0) { fail("load_nvm"); goto out; }
    uint8_t bcode = 0;
    (void)qca_baud_code(target_baud, &bcode);
    struct nvm_patch_info ninfo;
    if (parse_and_patch_nvm(nvm_buf, nvm_len, bcode, true, &ninfo) != 0) {
        free(nvm_buf);
        fail("patch_nvm");
        goto out;
    }
    printf("BT_FW_NVM_META file=%s bytes=%zu tag17=%d(%02x%02x->%02x%02x) tag27=%d(%02x%02x->%02x%02x) tag35=%d ipa=%d\n",
           nvm_name, nvm_len,
           ninfo.saw_tag17 ? 1 : 0, ninfo.tag17_old[0], ninfo.tag17_old[1], ninfo.tag17_new[0], ninfo.tag17_new[1],
           ninfo.saw_tag27 ? 1 : 0, ninfo.tag27_old[0], ninfo.tag27_old[1], ninfo.tag27_new[0], ninfo.tag27_new[1],
           ninfo.saw_tag35 ? 1 : 0, ninfo.ipa_supported ? 1 : 0);
    if (ninfo.ipa_supported) {
        int tcs_rc = ioctl(power, BT_SET_IPA_TCS, 0);
        printf("BT_FW_IPA_TCS rc=%d errno=%d\n", tcs_rc, tcs_rc ? errno : 0);
    }
    unsigned nvm_segs = 0;
    dl_rc = download_tlv_stream(tty, nvm_buf, nvm_len, QCA_TLV_TYPE_NVM, QCA_SKIP_EVT_NONE,
                                ver.unified, 3000, &nvm_segs);
    free(nvm_buf);
    if (dl_rc != 0) { fail("download_nvm"); goto out; }
    printf("BT_FW_NVM_OK file=%s bytes=%zu segments=%u\n", nvm_name, nvm_len, nvm_segs);

    /* 4. Post-download Build Info, HCI Reset, Read Local Version, Read BD_ADDR */
    if (query_build_info(tty, 2000) != 0) {
        printf("BT_FW_BUILD_INFO_WARN errno=%d\n", errno);
    }
    if (hci_reset_strict(tty, 2500) != 0) { fail("hci_reset"); goto out; }

    struct local_version lv = {0};
    if (hci_read_local_version(tty, 2000, &lv) != 0) { fail("read_local_version"); goto out; }
    printf("BT_FW_LOCAL_VER hci_ver=0x%02x hci_rev=0x%04x lmp_ver=0x%02x mfg=0x%04x lmp_subver=0x%04x\n",
           (unsigned)lv.hci_ver, (unsigned)lv.hci_rev, (unsigned)lv.lmp_ver,
           (unsigned)lv.manufacturer, (unsigned)lv.lmp_subver);

    uint8_t bd[6] = {0};
    if (hci_read_bd_addr(tty, 2000, bd) != 0) { fail("read_bd_addr"); goto out; }
    printf("BT_FW_BD_ADDR=%02x:%02x:%02x:%02x:%02x:%02x\n",
           bd[5], bd[4], bd[3], bd[2], bd[1], bd[0]);

    rc = 0;
out:
    if (voted && rc != 0 && tcflush(tty, TCIOFLUSH) != 0) rc = fail("flush_on_exit");
    if (restore && ioctl(tty, QCA_TCSETSW2, &old_t2) != 0) rc = fail("restore_termios2");
    if (restore_lines && ioctl(tty, TIOCMSET, &old_modem) != 0) rc = fail("restore_uart_lines");
    if (voted) {
        int put = ioctl(tty, UART_PM_PUT, 0);
        if (put != 0 && errno == EAGAIN) {
            const struct timespec delay = { .tv_nsec = 150000000 };
            (void)nanosleep(&delay, NULL);
            put = ioctl(tty, UART_PM_PUT, 0);
        }
        if (put != 0) rc = fail("uart_pm_put");
        else puts("BT_FW_UART_VOTE_RELEASED=1");
    }
    if (tty >= 0 && close(tty) != 0) rc = fail("uart_close");
    if (powered) {
        if (ioctl(power, BT_POWER, 0) != 0) rc = fail("power_off");
        else puts("BT_FW_POWER_OFF=1");
    }
    return rc;
}

int main(int argc, char **argv)
{
    bool baud_only = argc == 5 && !strcmp(argv[1], "--baud-only");
    bool bringup = (argc == 5 || argc == 6) && !strcmp(argv[1], "--bringup");
    if (!baud_only && !bringup) {
        fprintf(stderr, "usage: %s --baud-only POWER UART BAUD | --bringup POWER UART FW_DIR [BAUD]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction handler = { .sa_handler = cancel };
    if (sigemptyset(&handler.sa_mask) != 0 || sigaction(SIGTERM, &handler, NULL) != 0 ||
        sigaction(SIGINT, &handler, NULL) != 0) return fail("signal_handler");

    uint32_t baud = 3200000;
    const char *fw_dir = NULL;
    if (baud_only) {
        baud = (uint32_t)strtoul(argv[4], NULL, 10);
    } else {
        fw_dir = argv[4];
        if (argc == 6) baud = (uint32_t)strtoul(argv[5], NULL, 10);
    }
    uint8_t dummy = 0;
    if (qca_baud_code(baud, &dummy) != 0) return fail("unsupported_baud");

    int power = char_open(argv[2], 0);
    if (power < 0) return fail("power_open");
    int rc = check_chipset(power) != 0 ? fail("chip_id")
                                       : run_hardware(power, argv[3], fw_dir, baud, baud_only);
    if (close(power) != 0) return fail("power_close");
    return rc;
}
