/* SPDX-License-Identifier: MIT */
/*
 * marble_nfc_hw_probe.c - Clean-room SM7475 (Xiaomi marble) NXP SN110/SN220
 * hardware NFC NCI 2.0 probe over /dev/nq-nci.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NFC_MAGIC           0xE9
#define NFC_SET_PWR         _IOW(NFC_MAGIC, 0x01, unsigned int)
#define NFC_GET_IRQ_STATE   _IO(NFC_MAGIC, 0x06)
#define NFCC_GET_INFO       _IOW(NFC_MAGIC, 0x09, unsigned int)

#define NFC_POWER_OFF       0u
#define NFC_POWER_ON        1u

static void print_hex(const char *tag, const uint8_t *buf, size_t n) {
    printf("%s len=%zu hex=", tag, n);
    for (size_t i = 0; i < n; ++i)
        printf("%02x", buf[i]);
    printf("\n");
}

static int nci_write_pkt(int fd, const uint8_t *pkt, size_t len) {
    for (int retry = 0; retry < 4; ++retry) {
        ssize_t w = write(fd, pkt, len);
        if (w == (ssize_t)len)
            return 0;
        usleep(10000);
    }
    return -EIO;
}

static int nci_read_pkt(int fd, uint8_t *buf, size_t cap, int timeout_ms) {
    int waited = 0;
    while (timeout_ms > 0) {
        int irq = ioctl(fd, NFC_GET_IRQ_STATE, 0);
        if (irq > 0)
            break;
        usleep(5000);
        waited += 5;
        if (waited >= timeout_ms)
            return -ETIMEDOUT;
    }
    ssize_t h = read(fd, buf, 3);
    if (h != 3)
        return (h < 0) ? -errno : -EIO;
    uint8_t plen = buf[2];
    if ((size_t)(3 + plen) > cap)
        return -EMSGSIZE;
    if (plen > 0) {
        ssize_t p = read(fd, buf + 3, plen);
        if (p != plen)
            return (p < 0) ? -errno : -EIO;
    }
    return 3 + plen;
}

static int run_self_test(void) {
    const uint8_t reset_cmd[] = { 0x20, 0x00, 0x01, 0x00 };
    const uint8_t init_cmd[]  = { 0x20, 0x01, 0x02, 0x00, 0x00 };
    if (reset_cmd[2] + 3 != sizeof(reset_cmd) || init_cmd[2] + 3 != sizeof(init_cmd))
        return 1;
    printf("MARBLE_NFC_HW_PROBE_SELF_TEST_OK=1 nfc_set_pwr=0x%08lx nfcc_get_info=0x%08lx\n",
           (unsigned long)NFC_SET_PWR, (unsigned long)NFCC_GET_INFO);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *dev = "/dev/nq-nci";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0)
            return run_self_test();
        if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc)
            dev = argv[++i];
    }

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        printf("NFC_OPEN_ERR dev=%s errno=%d\n", dev, errno);
        return 1;
    }

    unsigned int info = (unsigned int)ioctl(fd, NFCC_GET_INFO, 0);
    uint8_t chip_type = (uint8_t)(info & 0xFFu);
    uint8_t rom_ver   = (uint8_t)((info >> 8) & 0xFFu);
    uint8_t fw_maj    = (uint8_t)((info >> 16) & 0xFFu);
    uint8_t fw_min    = (uint8_t)((info >> 24) & 0xFFu);
    printf("NFC_DEV_INFO dev=%s raw=0x%08x chip=0x%02x rom=0x%02x fw=%u.%u\n",
           dev, info, chip_type, rom_ver, fw_maj, fw_min);

    int prc = ioctl(fd, NFC_SET_PWR, NFC_POWER_ON);
    printf("NFC_PWR_ON rc=%d\n", prc);
    usleep(30000);

    uint8_t buf[300];
    while (ioctl(fd, NFC_GET_IRQ_STATE, 0) > 0) {
        int n = nci_read_pkt(fd, buf, sizeof(buf), 100);
        if (n > 0)
            print_hex("NFC_BOOT_NTF", buf, (size_t)n);
        else
            break;
    }

    /* 1. NCI 2.0 CORE_RESET_CMD (Keep Config = 0x00) */
    const uint8_t core_reset[] = { 0x20, 0x00, 0x01, 0x00 };
    int rc_reset = 0, rc_init = 0, rc_cfg = 0;
    uint8_t nci_ver = 0, mfg_id = 0;

    if (nci_write_pkt(fd, core_reset, sizeof(core_reset)) == 0) {
        int n1 = nci_read_pkt(fd, buf, sizeof(buf), 1000);
        if (n1 > 0) {
            print_hex("NCI_CORE_RESET_RSP", buf, (size_t)n1);
            if (n1 >= 4 && buf[0] == 0x40 && buf[1] == 0x00 && buf[3] == 0x00) {
                rc_reset = 1;
                if (buf[2] == 1) {
                    int n2 = nci_read_pkt(fd, buf, sizeof(buf), 1000);
                    if (n2 > 0) {
                        print_hex("NCI_CORE_RESET_NTF", buf, (size_t)n2);
                        if (n2 >= 10 && buf[0] == 0x60 && buf[1] == 0x00) {
                            nci_ver = buf[5];
                            mfg_id  = buf[6];
                        }
                    }
                } else if (n1 >= 5) {
                    nci_ver = buf[4];
                }
            }
        }
    }

    /* 2. NCI 2.0 CORE_INIT_CMD (Feature Enable = 0x00, 0x00) */
    const uint8_t core_init[] = { 0x20, 0x01, 0x02, 0x00, 0x00 };
    if (rc_reset && nci_write_pkt(fd, core_init, sizeof(core_init)) == 0) {
        int n = nci_read_pkt(fd, buf, sizeof(buf), 1000);
        if (n > 0) {
            print_hex("NCI_CORE_INIT_RSP", buf, (size_t)n);
            if (n >= 16 && buf[0] == 0x40 && buf[1] == 0x01 && buf[3] == 0x00)
                rc_init = 1;
        }
    }

    /* 3. NCI 2.0 CORE_GET_CONFIG_CMD (TOTAL_DURATION = 0x00) */
    const uint8_t get_cfg[] = { 0x20, 0x03, 0x02, 0x01, 0x00 };
    if (rc_init && nci_write_pkt(fd, get_cfg, sizeof(get_cfg)) == 0) {
        int n = nci_read_pkt(fd, buf, sizeof(buf), 1000);
        if (n > 0) {
            print_hex("NCI_CORE_GET_CONFIG_RSP", buf, (size_t)n);
            if (n >= 5 && buf[0] == 0x40 && buf[1] == 0x03 && buf[3] == 0x00)
                rc_cfg = 1;
        }
    }

    int poff = ioctl(fd, NFC_SET_PWR, NFC_POWER_OFF);
    close(fd);

    printf("MARBLE_NFC_HW_PROBE_SUMMARY open=1 pwr_on=%d reset=%d init=%d get_cfg=%d "
           "nci_ver=0x%02x mfg_id=0x%02x chip=0x%02x fw=%u.%u pwr_off=%d\n",
           prc == 0 ? 1 : 0, rc_reset, rc_init, rc_cfg,
           nci_ver, mfg_id, chip_type, fw_maj, fw_min, poff == 0 ? 1 : 0);
    return (rc_reset && rc_init && rc_cfg) ? 0 : 2;
}
