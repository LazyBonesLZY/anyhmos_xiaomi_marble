// SPDX-License-Identifier: MIT
/*
 * Non-destructive offline ext4 RO_COMPAT_VERITY feature enabler for Xiaomi marble.
 *
 * Equivalent to `tune2fs -O verity <unmounted-ext4-dev>`:
 *   - Refuses to touch any device that is not a clean 4096-byte-block extent-based ext4.
 *   - No-op (returns 0) if EXT4_FEATURE_RO_COMPAT_VERITY (0x8000) is already set.
 *   - If EXT4_FEATURE_RO_COMPAT_METADATA_CSUM (0x0400) is set, verifies the existing
 *     CRC32C superblock checksum BEFORE modifying a single byte, then updates both
 *     s_feature_ro_compat and s_checksum atomically in the 1024-byte primary superblock.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EXT4_SB_OFFSET              1024
#define EXT4_SB_SIZE                1024
#define EXT4_SB_LOG_BLOCK_SIZE_OFF  0x18
#define EXT4_SB_MAGIC_OFF           0x38
#define EXT4_SB_FEATURE_INCOMPAT    0x60
#define EXT4_SB_FEATURE_RO_COMPAT   0x64
#define EXT4_SB_CHECKSUM_TYPE_OFF   0x175
#define EXT4_SB_CHECKSUM_OFF        0x3fc

#define EXT4_SUPER_MAGIC            0xEF53U
#define EXT4_FEATURE_INCOMPAT_RECOVER       0x0004U
#define EXT4_FEATURE_INCOMPAT_EXTENTS       0x0040U
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400U
#define EXT4_FEATURE_RO_COMPAT_VERITY       0x8000U
#define EXT4_CRC32C_CHKSUM          1U

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffU);
    p[1] = (uint8_t)((v >> 8) & 0xffU);
    p[2] = (uint8_t)((v >> 16) & 0xffU);
    p[3] = (uint8_t)((v >> 24) & 0xffU);
}

/* Castagnoli CRC32C (poly 0x82F63B78), matching Linux ext4_chksum() / e2fsprogs ext2fs_crc32c_le(~0, sb, 0x3fc). */
static uint32_t ext4_sb_crc32c(const uint8_t *sb)
{
    uint32_t crc = 0xFFFFFFFFU;
    size_t i;
    int bit;

    for (i = 0; i < EXT4_SB_CHECKSUM_OFF; i++) {
        crc ^= (uint32_t)sb[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1U)
                crc = (crc >> 1) ^ 0x82F63B78U;
            else
                crc >>= 1;
        }
    }
    return crc;
}

static int pread_full(int fd, uint8_t *buf, size_t len, off_t off)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, buf + done, len - done, off + (off_t)done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int pwrite_full(int fd, const uint8_t *buf, size_t len, off_t off)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, buf + done, len - done, off + (off_t)done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t sb[EXT4_SB_SIZE];
    uint8_t verify[EXT4_SB_SIZE];
    uint16_t magic;
    uint32_t log_bsize, incompat, ro_compat;
    int fd;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <unmounted-ext4-device>\n", argv[0]);
        return 2;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "EXT4_VERITY_ERR open %s: %s\n", argv[1], strerror(errno));
        return 3;
    }

    if (pread_full(fd, sb, sizeof(sb), EXT4_SB_OFFSET) != 0) {
        fprintf(stderr, "EXT4_VERITY_ERR read superblock: %s\n", strerror(errno));
        close(fd);
        return 4;
    }

    magic = read_le16(sb + EXT4_SB_MAGIC_OFF);
    if (magic != EXT4_SUPER_MAGIC) {
        fprintf(stderr, "EXT4_VERITY_ERR bad magic 0x%04x\n", magic);
        close(fd);
        return 5;
    }

    log_bsize = read_le32(sb + EXT4_SB_LOG_BLOCK_SIZE_OFF);
    if (log_bsize != 2U) {
        fprintf(stderr, "EXT4_VERITY_ERR unsupported log_block_size=%u (need 2 for 4096B)\n",
                log_bsize);
        close(fd);
        return 6;
    }

    incompat = read_le32(sb + EXT4_SB_FEATURE_INCOMPAT);
    if (!(incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        fprintf(stderr, "EXT4_VERITY_ERR extents feature missing (incompat=0x%08x)\n", incompat);
        close(fd);
        return 7;
    }

    ro_compat = read_le32(sb + EXT4_SB_FEATURE_RO_COMPAT);
    if (ro_compat & EXT4_FEATURE_RO_COMPAT_VERITY) {
        printf("EXT4_VERITY_ALREADY_ENABLED ro_compat=0x%08x\n", ro_compat);
        close(fd);
        return 0;
    }

    if (incompat & EXT4_FEATURE_INCOMPAT_RECOVER) {
        printf("EXT4_VERITY_NEEDS_RECOVERY incompat=0x%08x ro_compat=0x%08x\n",
               incompat, ro_compat);
        close(fd);
        return 20;
    }

    if (ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint8_t ctype = sb[EXT4_SB_CHECKSUM_TYPE_OFF];
        uint32_t stored = read_le32(sb + EXT4_SB_CHECKSUM_OFF);
        uint32_t computed = ext4_sb_crc32c(sb);
        if (ctype != EXT4_CRC32C_CHKSUM) {
            fprintf(stderr, "EXT4_VERITY_ERR unsupported checksum_type=%u\n", ctype);
            close(fd);
            return 8;
        }
        if (stored != computed) {
            fprintf(stderr, "EXT4_VERITY_ERR existing sb checksum mismatch stored=0x%08x computed=0x%08x (refusing write)\n",
                    stored, computed);
            close(fd);
            return 9;
        }
    }

    ro_compat |= EXT4_FEATURE_RO_COMPAT_VERITY;
    write_le32(sb + EXT4_SB_FEATURE_RO_COMPAT, ro_compat);

    if (ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint32_t new_csum = ext4_sb_crc32c(sb);
        write_le32(sb + EXT4_SB_CHECKSUM_OFF, new_csum);
    }

    if (pwrite_full(fd, sb, sizeof(sb), EXT4_SB_OFFSET) != 0 || fsync(fd) != 0) {
        fprintf(stderr, "EXT4_VERITY_ERR write/fsync failed: %s\n", strerror(errno));
        close(fd);
        return 10;
    }

    if (pread_full(fd, verify, sizeof(verify), EXT4_SB_OFFSET) != 0 ||
        memcmp(sb, verify, sizeof(sb)) != 0) {
        fprintf(stderr, "EXT4_VERITY_ERR readback verification mismatch\n");
        close(fd);
        return 11;
    }

    close(fd);
    printf("EXT4_VERITY_ENABLED ro_compat=0x%08x\n", ro_compat);
    return 0;
}
