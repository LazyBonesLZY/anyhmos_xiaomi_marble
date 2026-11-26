/* SPDX-License-Identifier: MIT */
/*
 * marble_isp_capture.c - Clean-room SM7475 real silicon capture engine
 * (TPG v1.3 + Sony IMX355 optical sensor via CSIPHY0 -> CSID -> IFE RDI0)
 * plus MIPI RAW10 Bayer demosaic, auto-gain/AWB, NV21 & 2D 8x8 DCT JPEG encoder.
 */

#define _GNU_SOURCE
#include "marble_isp_capture.h"
#include "marble_sensor_tables.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>
#include <media/cam_cpas.h>
#include <media/cam_defs.h>
#include <media/cam_isp.h>
#include <media/cam_isp_ife.h>
#include <media/cam_req_mgr.h>
#include <media/cam_sensor.h>
#include <media/cam_sync.h>

#define MARBLE_RING_SLOTS 3
#define PACK_8_BIT  8
#define PACK_10_BIT 10

/* ============================================================================
 * Part 1: Sony IMX355 19.2MHz MCLK Register Tables (GPL/open Linux imx355.c)
 * ============================================================================ */

struct imx355_reg_pair {
    uint16_t addr;
    uint8_t  val;
};

static const struct imx355_reg_pair k_imx355_global_regs[] = {
    { 0x0136, 0x13 }, { 0x0137, 0x33 }, { 0x304e, 0x03 }, { 0x4348, 0x16 },
    { 0x4350, 0x19 }, { 0x4408, 0x0a }, { 0x440c, 0x0b }, { 0x4411, 0x5f },
    { 0x4412, 0x2c }, { 0x4623, 0x00 }, { 0x462c, 0x0f }, { 0x462d, 0x00 },
    { 0x462e, 0x00 }, { 0x4684, 0x54 }, { 0x480a, 0x07 }, { 0x4908, 0x07 },
    { 0x4909, 0x07 }, { 0x490d, 0x0a }, { 0x491e, 0x0f }, { 0x4921, 0x06 },
    { 0x4923, 0x28 }, { 0x4924, 0x28 }, { 0x4925, 0x29 }, { 0x4926, 0x29 },
    { 0x4927, 0x1f }, { 0x4928, 0x20 }, { 0x4929, 0x20 }, { 0x492a, 0x20 },
    { 0x492c, 0x05 }, { 0x492d, 0x06 }, { 0x492e, 0x06 }, { 0x492f, 0x06 },
    { 0x4930, 0x03 }, { 0x4931, 0x04 }, { 0x4932, 0x04 }, { 0x4933, 0x05 },
    { 0x595e, 0x01 }, { 0x5963, 0x01 }, { 0x3030, 0x01 }, { 0x3031, 0x01 },
    { 0x3045, 0x01 }, { 0x4010, 0x00 }, { 0x4011, 0x00 }, { 0x4012, 0x00 },
    { 0x4013, 0x01 }, { 0x68a8, 0xfe }, { 0x68a9, 0xff }, { 0x6888, 0x00 },
    { 0x6889, 0x00 }, { 0x68b0, 0x00 }, { 0x3058, 0x00 }, { 0x305a, 0x00 },
};

/* 2x2 binned 1640x1232 @ 19.2MHz MCLK, 4-lane D-PHY 360MHz (720Mbps/lane), RAW10 */
static const struct imx355_reg_pair k_imx355_mode_1640x1232[] = {
    { 0x0112, 0x0a }, { 0x0113, 0x0a }, { 0x0114, 0x03 }, { 0x0342, 0x07 },
    { 0x0343, 0x2c }, { 0x0340, 0x05 }, { 0x0341, 0x1a }, { 0x0344, 0x00 },
    { 0x0345, 0x00 }, { 0x0346, 0x00 }, { 0x0347, 0x00 }, { 0x0348, 0x0c },
    { 0x0349, 0xcf }, { 0x034a, 0x09 }, { 0x034b, 0x9f }, { 0x0220, 0x00 },
    { 0x0222, 0x01 }, { 0x0900, 0x01 }, { 0x0901, 0x22 }, { 0x0902, 0x00 },
    { 0x034c, 0x06 }, { 0x034d, 0x68 }, { 0x034e, 0x04 }, { 0x034f, 0xd0 },
    { 0x0301, 0x05 }, { 0x0303, 0x01 }, { 0x0305, 0x02 }, { 0x0306, 0x00 },
    { 0x0307, 0x78 }, { 0x030b, 0x01 }, { 0x030d, 0x02 }, { 0x030e, 0x00 },
    { 0x030f, 0x4b }, { 0x0310, 0x00 }, { 0x0700, 0x00 }, { 0x0701, 0x10 },
    { 0x0820, 0x0b }, { 0x0821, 0x40 }, { 0x3088, 0x04 }, { 0x6813, 0x02 },
    { 0x6835, 0x07 }, { 0x6836, 0x01 }, { 0x6837, 0x04 }, { 0x684d, 0x07 },
    { 0x684e, 0x01 }, { 0x684f, 0x04 },
};

/* 4x4 binned 820x616 @ 19.2MHz MCLK, 4-lane D-PHY 360MHz (720Mbps/lane), RAW10 */
static const struct imx355_reg_pair k_imx355_mode_820x616[] = {
    { 0x0112, 0x0a }, { 0x0113, 0x0a }, { 0x0114, 0x03 }, { 0x0342, 0x0e },
    { 0x0343, 0x58 }, { 0x0340, 0x02 }, { 0x0341, 0x8c }, { 0x0344, 0x00 },
    { 0x0345, 0x00 }, { 0x0346, 0x00 }, { 0x0347, 0x00 }, { 0x0348, 0x0c },
    { 0x0349, 0xcf }, { 0x034a, 0x09 }, { 0x034b, 0x9f }, { 0x0220, 0x00 },
    { 0x0222, 0x01 }, { 0x0900, 0x01 }, { 0x0901, 0x44 }, { 0x0902, 0x00 },
    { 0x034c, 0x03 }, { 0x034d, 0x34 }, { 0x034e, 0x02 }, { 0x034f, 0x68 },
    { 0x0301, 0x05 }, { 0x0303, 0x01 }, { 0x0305, 0x02 }, { 0x0306, 0x00 },
    { 0x0307, 0x78 }, { 0x030b, 0x01 }, { 0x030d, 0x02 }, { 0x030e, 0x00 },
    { 0x030f, 0x4b }, { 0x0310, 0x00 }, { 0x0700, 0x02 }, { 0x0701, 0x78 },
    { 0x0820, 0x0b }, { 0x0821, 0x40 }, { 0x3088, 0x04 }, { 0x6813, 0x02 },
    { 0x6835, 0x07 }, { 0x6836, 0x01 }, { 0x6837, 0x04 }, { 0x684d, 0x07 },
    { 0x684e, 0x01 }, { 0x684f, 0x04 },
};

/* ============================================================================
 * Part 2: SM7475 Camera-Kernel UAPI Session Management
 * ============================================================================ */

struct cam_buf {
    int32_t handle;
    int fd;
    size_t size;
    void *vaddr;
};

struct marble_hw_session {
    marble_hw_src_t src;
    marble_bayer_order_t bayer_order;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t raw_bytes;
    uint32_t cam_fmt;
    uint32_t bpp;
    int use_raw8;
    int is_optical;
    int is_sensor;

    int crm_fd;
    int sync_fd;
    int isp_fd;
    int tpg_fd;
    int csiphy_fd;
    int sensor_fd;
    int act_fd;

    int32_t session_hdl;
    int32_t isp_hdl;
    int32_t tpg_hdl;
    int32_t csiphy_hdl;
    int32_t sensor_hdl;
    int32_t act_hdl;
    int32_t link_hdl;
    int32_t img_iommu;
    int32_t cdm_iommu;

    bool isp_hw_acq;
    bool isp_started;
    bool tpg_started;
    bool csiphy_started;
    bool sensor_started;
    bool act_configured;

    struct cam_buf src_pkt[5]; /* 0:csiphy, 1:init, 2:streamon, 3:streamoff, 4:live AE update */
    struct cam_buf src_cmd[5];
    struct cam_buf act_pkt;
    struct cam_buf act_cmd[3]; /* 0:i2c_info, 1:pwr, 2:regs */
    struct cam_buf isp_init_pkt;
    struct cam_buf isp_init_kmd;
    struct cam_buf isp_init_blob;

    struct cam_buf upd_pkt[MARBLE_RING_SLOTS];
    struct cam_buf upd_kmd[MARBLE_RING_SLOTS];
    struct cam_buf img_buf[MARBLE_RING_SLOTS];
    int32_t fences[MARBLE_RING_SLOTS];
    uint32_t slot_req_id[MARBLE_RING_SLOTS];
    int ring_head;      /* next slot to wait on */
    int ring_tail;      /* next slot to submit */
    int ring_queued;
    uint32_t next_req_id;

    /* Real-time closed-loop Auto-Exposure (AE) & GT9764 AF state */
    int ae_enabled;
    marble_ae_limits_t ae_lim;
    uint32_t ae_exp;
    uint32_t ae_gain_q8;
    uint32_t ae_updates;
    int ae_cooldown;
    uint16_t focus_dac;

    uint8_t *scratch_raw;
};

const char *marble_hw_src_name(marble_hw_src_t src) {
    switch (src) {
        case MARBLE_HW_SRC_OV64B40: return "ov64b40";
        case MARBLE_HW_SRC_S5K3P9:  return "s5k3p9";
        case MARBLE_HW_SRC_IMX355:  return "imx355";
        case MARBLE_HW_SRC_GC02M1:  return "gc02m1";
        default:                    return "tpg";
    }
}

static void set_err(char *buf, size_t sz, const char *msg, int rc) {
    if (buf && sz > 0)
        snprintf(buf, sz, "%s (rc=%d)", msg, rc);
}

static int cam_ctrl(int fd, uint32_t op_code, void *payload, uint32_t size) {
    struct cam_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = op_code;
    ctrl.size = size;
    ctrl.handle_type = CAM_HANDLE_USER_POINTER;
    ctrl.handle = (uint64_t)(uintptr_t)payload;
    int rc = ioctl(fd, VIDIOC_CAM_CONTROL, &ctrl);
    return (rc < 0) ? -errno : 0;
}

static int cam_sync_ioctl(int sync_fd, uint32_t id, void *payload, uint32_t size) {
    struct cam_private_ioctl_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.id = id;
    arg.size = size;
    arg.ioctl_ptr = (uint64_t)(uintptr_t)payload;
    int rc = ioctl(sync_fd, CAM_PRIVATE_IOCTL_CMD, &arg);
    if (rc < 0)
        return -errno;
    return (int)arg.result;
}

static int entity_to_subdev_path(const struct media_entity_desc *ent, char *out_path, size_t out_sz) {
    if (strncmp(ent->name, "v4l-subdev", 10) == 0) {
        snprintf(out_path, out_sz, "/dev/%s", ent->name);
        return 0;
    }
    for (int i = 0; i < 64; ++i) {
        char sysp[128], buf[32];
        snprintf(sysp, sizeof(sysp), "/sys/class/video4linux/v4l-subdev%d/dev", i);
        int sfd = open(sysp, O_RDONLY);
        if (sfd < 0)
            continue;
        ssize_t n = read(sfd, buf, sizeof(buf) - 1);
        close(sfd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        unsigned maj = 0, min = 0;
        if (sscanf(buf, "%u:%u", &maj, &min) == 2 &&
            maj == ent->v4l.major && min == ent->v4l.minor) {
            snprintf(out_path, out_sz, "/dev/v4l-subdev%d", i);
            return 0;
        }
    }
    return -ENOENT;
}

static int find_subdev_by_type(uint32_t ent_type, int index_want, char *out_path, size_t out_sz) {
    int mfd = open("/dev/media0", O_RDWR);
    if (mfd < 0)
        return -errno;
    struct media_entity_desc ent;
    memset(&ent, 0, sizeof(ent));
    int found = -ENOENT;
    int seen = 0;
    for (;;) {
        ent.id |= MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(mfd, MEDIA_IOC_ENUM_ENTITIES, &ent) < 0)
            break;
        if (ent.type == ent_type) {
            if (seen == index_want) {
                found = entity_to_subdev_path(&ent, out_path, out_sz);
                break;
            }
            seen++;
        }
    }
    close(mfd);
    return found;
}

static int find_subdev_by_slot(uint32_t ent_type, uint32_t want_slot, char *out_path, size_t out_sz) {
    int mfd = open("/dev/media0", O_RDWR);
    if (mfd < 0)
        return -errno;
    struct media_entity_desc ent;
    memset(&ent, 0, sizeof(ent));
    int found = -ENOENT;
    for (;;) {
        ent.id |= MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(mfd, MEDIA_IOC_ENUM_ENTITIES, &ent) < 0)
            break;
        if (ent.type != ent_type)
            continue;
        char cand[64];
        if (entity_to_subdev_path(&ent, cand, sizeof(cand)) != 0)
            continue;
        int fd = open(cand, O_RDWR);
        if (fd < 0)
            continue;
        bool match = false;
        if (ent_type == CAM_SENSOR_DEVICE_TYPE) {
            struct cam_sensor_query_cap cap;
            memset(&cap, 0, sizeof(cap));
            if (cam_ctrl(fd, CAM_QUERY_CAP, &cap, sizeof(cap)) == 0 && cap.slot_info == want_slot)
                match = true;
        } else if (ent_type == CAM_CSIPHY_DEVICE_TYPE) {
            struct cam_csiphy_query_cap cap;
            memset(&cap, 0, sizeof(cap));
            if (cam_ctrl(fd, CAM_QUERY_CAP, &cap, sizeof(cap)) == 0 && cap.slot_info == want_slot)
                match = true;
        } else if (ent_type == CAM_ACTUATOR_DEVICE_TYPE) {
            struct cam_actuator_query_cap cap;
            memset(&cap, 0, sizeof(cap));
            if (cam_ctrl(fd, CAM_QUERY_CAP, &cap, sizeof(cap)) == 0 && cap.slot_info == want_slot)
                match = true;
        }
        close(fd);
        if (match) {
            snprintf(out_path, out_sz, "%s", cand);
            found = 0;
            break;
        }
    }
    close(mfd);
    return found;
}

static int cam_alloc_buf(int crm_fd, size_t len, uint32_t flags, int32_t mmu_hdl, struct cam_buf *out) {
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    struct cam_mem_mgr_alloc_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.len = (len + 4095u) & ~4095u;
    cmd.align = 4096u;
    cmd.flags = flags;
    if (mmu_hdl > 0) {
        cmd.mmu_hdls[0] = mmu_hdl;
        cmd.num_hdl = 1;
    }
    int rc = cam_ctrl(crm_fd, CAM_REQ_MGR_ALLOC_BUF, &cmd, sizeof(cmd));
    if (rc != 0)
        return rc;
    out->handle = (int32_t)cmd.out.buf_handle;
    out->fd = cmd.out.fd;
    out->size = (size_t)cmd.len;
    if (out->fd >= 0) {
        void *p = mmap(NULL, out->size, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, 0);
        if (p != MAP_FAILED) {
            out->vaddr = p;
            memset(p, 0, out->size);
        }
    }
    return 0;
}

static void cam_free_buf(int crm_fd, struct cam_buf *b) {
    if (!b)
        return;
    if (b->vaddr) {
        munmap(b->vaddr, b->size);
        b->vaddr = NULL;
    }
    if (b->handle > 0 && crm_fd >= 0) {
        struct cam_mem_mgr_release_cmd rel;
        memset(&rel, 0, sizeof(rel));
        rel.buf_handle = b->handle;
        (void)cam_ctrl(crm_fd, CAM_REQ_MGR_RELEASE_BUF, &rel, sizeof(rel));
        b->handle = 0;
    }
    if (b->fd >= 0) {
        close(b->fd);
        b->fd = -1;
    }
}

static int cam_cache_op(int crm_fd, int32_t handle, uint32_t op) {
    struct cam_mem_cache_ops_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.buf_handle = handle;
    cmd.mem_cache_ops = op;
    return cam_ctrl(crm_fd, CAM_REQ_MGR_CACHE_OPS, &cmd, sizeof(cmd));
}

static int sync_create(int sync_fd, const char *name) {
    struct cam_sync_info info;
    memset(&info, 0, sizeof(info));
    snprintf(info.name, sizeof(info.name), "%s", name);
    int rc = cam_sync_ioctl(sync_fd, CAM_SYNC_CREATE, &info, sizeof(info));
    return (rc == 0) ? info.sync_obj : rc;
}

static int sync_wait(int sync_fd, int32_t obj, uint64_t timeout_ms) {
    struct cam_sync_wait w;
    memset(&w, 0, sizeof(w));
    w.sync_obj = obj;
    w.timeout_ms = timeout_ms;
    return cam_sync_ioctl(sync_fd, CAM_SYNC_WAIT, &w, sizeof(w));
}

static void sync_destroy(int sync_fd, int32_t obj) {
    if (obj <= 0 || sync_fd < 0)
        return;
    struct cam_sync_info info;
    memset(&info, 0, sizeof(info));
    info.sync_obj = obj;
    (void)cam_sync_ioctl(sync_fd, CAM_SYNC_DESTROY, &info, sizeof(info));
}

static uint32_t append_generic_blob(uint8_t *dst, uint32_t off, uint32_t blob_type,
                                    const void *payload, uint32_t payload_sz) {
    uint32_t hdr = ((payload_sz << CAM_GENERIC_BLOB_CMDBUFFER_SIZE_SHIFT) & CAM_GENERIC_BLOB_CMDBUFFER_SIZE_MASK) |
                   ((blob_type << CAM_GENERIC_BLOB_CMDBUFFER_TYPE_SHIFT) & CAM_GENERIC_BLOB_CMDBUFFER_TYPE_MASK);
    memcpy(dst + off, &hdr, 4);
    memcpy(dst + off + 4, payload, payload_sz);
    uint32_t aligned = (payload_sz + 3u) & ~3u;
    if (aligned > payload_sz)
        memset(dst + off + 4 + payload_sz, 0, aligned - payload_sz);
    return off + 4u + aligned;
}

struct sensor_pwr_step {
    uint8_t cmd;
    uint16_t seq;
    uint32_t val;
    int16_t ms;
};

struct sensor_probe_spec {
    const char *name;
    uint32_t slot;
    uint32_t slave_addr;
    uint8_t i2c_freq_mode;
    uint8_t id_addr_type;
    uint8_t id_data_type;
    uint16_t id_reg;
    uint16_t id_val;
    const struct sensor_pwr_step *steps;
    size_t n_steps;
};

static const struct sensor_pwr_step k_pwr_ov64b40[] = {
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   4,        1, 10 }, /* VAF */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   8,        0,  1 }, /* RESET=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   1,        1,  0 }, /* VANA */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   2,        1,  0 }, /* VDIG */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   3,        1,  1 }, /* VIO */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   0, 19200000, 10 }, /* MCLK 19.2MHz */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   8,        1,  5 }, /* RESET=1 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 8,        0,  2 }, /* RESET=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 0,        0,  1 }, /* MCLK=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 3,        0,  0 }, /* VIO */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 2,        0,  0 }, /* VDIG */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 1,        0,  0 }, /* VANA */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 4,        0,  0 }, /* VAF */
};

static const struct sensor_pwr_step k_pwr_s5k3p9[] = {
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   8,        0,  1 }, /* RESET=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   1,        1,  0 }, /* VANA */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   2,        1,  0 }, /* VDIG */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   3,        1,  1 }, /* VIO */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   8,        1,  2 }, /* RESET=1 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   0, 19200000, 20 }, /* MCLK 19.2MHz */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 0,        0,  2 }, /* MCLK=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 8,        0,  1 }, /* RESET=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 3,        0,  0 }, /* VIO */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 2,        0,  0 }, /* VDIG */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 1,        0,  0 }, /* VANA */
};

static const struct sensor_pwr_step k_pwr_imx355[] = {
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   8,        0,  1 }, /* RESET=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   3,        0,  1 }, /* VIO */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   1,        0,  1 }, /* VANA */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   2,        0,  1 }, /* VDIG */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   0, 19200000,  2 }, /* MCLK 19.2MHz */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   8,        1, 10 }, /* RESET=1 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 8,        0,  2 }, /* RESET=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 0,        0,  1 }, /* MCLK=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 2,        0,  1 }, /* VDIG */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 1,        0,  1 }, /* VANA */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 3,        0,  1 }, /* VIO */
};

static const struct sensor_pwr_step k_pwr_gc02m1[] = {
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   8,        0,  1 }, /* RESET=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   3,        1,  1 }, /* VIO */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   1,        1,  1 }, /* VANA */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   8,        1,  1 }, /* RESET=1 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_UP,   0, 19200000,  5 }, /* MCLK 19.2MHz */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 0,        0,  5 }, /* MCLK=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 8,        0,  1 }, /* RESET=0 */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 1,        0,  1 }, /* VANA */
    { CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, 3,        0,  2 }, /* VIO */
};

static const struct sensor_probe_spec k_probe_specs[] = {
    [MARBLE_HW_SRC_IMX355]  = { "imx355_ultra", 2, 0x34, 1, 2, 2, 0x0016, 0x0355, k_pwr_imx355,  sizeof(k_pwr_imx355)  / sizeof(k_pwr_imx355[0])  },
    [MARBLE_HW_SRC_GC02M1]  = { "gc02m1_macro", 3, 0x6e, 1, 1, 2, 0x00f0, 0x02e0, k_pwr_gc02m1,  sizeof(k_pwr_gc02m1)  / sizeof(k_pwr_gc02m1[0])  },
    [MARBLE_HW_SRC_S5K3P9]  = { "s5k3p9_front", 1, 0x20, 1, 2, 2, 0x0000, 0x3109, k_pwr_s5k3p9,  sizeof(k_pwr_s5k3p9)  / sizeof(k_pwr_s5k3p9[0])  },
    [MARBLE_HW_SRC_OV64B40] = { "ov64b40_wide", 0, 0x20, 1, 2, 2, 0x300a, 0x5664, k_pwr_ov64b40, sizeof(k_pwr_ov64b40) / sizeof(k_pwr_ov64b40[0]) },
};

static uint32_t append_i2c_wait_ms(uint8_t *dst, uint32_t off, uint32_t cap, int16_t ms) {
    if (ms <= 0)
        return off;
    if (off + sizeof(struct cam_cmd_unconditional_wait) > cap)
        return 0;
    struct cam_cmd_unconditional_wait wt;
    memset(&wt, 0, sizeof(wt));
    wt.delay = ms;
    wt.op_code = CAMERA_SENSOR_WAIT_OP_SW_UCND;
    wt.cmd_type = CAMERA_SENSOR_CMD_TYPE_WAIT;
    memcpy(dst + off, &wt, sizeof(wt));
    return off + (uint32_t)sizeof(wt);
}

static uint32_t append_i2c_rndm_wr_u32(uint8_t *dst, uint32_t off, uint32_t cap,
                                       uint8_t addr_type, uint8_t data_type,
                                       const uint32_t *addrs, const uint32_t *vals, size_t n) {
    size_t pos = 0;
    while (pos < n) {
        size_t chunk = (n - pos > 64) ? 64 : (n - pos);
        size_t need = sizeof(struct i2c_rdwr_header) + chunk * sizeof(struct i2c_random_wr_payload);
        if (off + need > cap)
            return 0;
        struct i2c_rdwr_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.count = (uint32_t)chunk;
        hdr.op_code = CAMERA_SENSOR_I2C_OP_RNDM_WR;
        hdr.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_RNDM_WR;
        hdr.data_type = data_type;
        hdr.addr_type = addr_type;
        memcpy(dst + off, &hdr, sizeof(hdr));
        off += (uint32_t)sizeof(hdr);
        for (size_t i = 0; i < chunk; ++i) {
            struct i2c_random_wr_payload pl;
            pl.reg_addr = addrs[pos + i];
            pl.reg_data = vals[pos + i];
            memcpy(dst + off, &pl, sizeof(pl));
            off += (uint32_t)sizeof(pl);
        }
        pos += chunk;
    }
    return off;
}

static uint32_t append_i2c_burst_wr_u16(uint8_t *dst, uint32_t off, uint32_t cap,
                                        uint8_t addr_type, uint8_t data_type,
                                        uint32_t reg_addr, const uint16_t *vals, size_t n) {
    size_t pos = 0;
    while (pos < n) {
        size_t chunk = (n - pos > 64) ? 64 : (n - pos);
        size_t need = sizeof(struct i2c_rdwr_header) + sizeof(uint32_t) + chunk * sizeof(struct cam_cmd_read);
        if (off + need > cap)
            return 0;
        struct i2c_rdwr_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.count = (uint32_t)chunk;
        hdr.op_code = CAMERA_SENSOR_I2C_OP_CONT_WR_BRST;
        hdr.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_CONT_WR;
        hdr.data_type = data_type;
        hdr.addr_type = addr_type;
        memcpy(dst + off, &hdr, sizeof(hdr));
        off += (uint32_t)sizeof(hdr);
        memcpy(dst + off, &reg_addr, sizeof(reg_addr));
        off += (uint32_t)sizeof(reg_addr);
        for (size_t i = 0; i < chunk; ++i) {
            struct cam_cmd_read cr;
            cr.reg_data = vals[pos + i];
            cr.reserved = 0;
            memcpy(dst + off, &cr, sizeof(cr));
            off += (uint32_t)sizeof(cr);
        }
        pos += chunk;
    }
    return off;
}

static uint32_t append_reg8_table(uint8_t *dst, uint32_t off, uint32_t cap,
                                  const struct marble_reg8_pair *regs, size_t n) {
    uint32_t addrs[64], vals[64];
    size_t pos = 0;
    while (pos < n) {
        size_t chunk = (n - pos > 64) ? 64 : (n - pos);
        for (size_t i = 0; i < chunk; ++i) {
            addrs[i] = regs[pos + i].addr;
            vals[i] = regs[pos + i].val;
        }
        off = append_i2c_rndm_wr_u32(dst, off, cap, 1, 1, addrs, vals, chunk);
        if (off == 0)
            return 0;
        pos += chunk;
    }
    return off;
}

static uint32_t append_reg16_8_table(uint8_t *dst, uint32_t off, uint32_t cap,
                                     const struct marble_reg16_8 *regs, size_t n) {
    uint32_t addrs[64], vals[64];
    size_t pos = 0;
    while (pos < n) {
        size_t chunk = (n - pos > 64) ? 64 : (n - pos);
        for (size_t i = 0; i < chunk; ++i) {
            addrs[i] = regs[pos + i].addr;
            vals[i] = regs[pos + i].val;
        }
        off = append_i2c_rndm_wr_u32(dst, off, cap, 2, 1, addrs, vals, chunk);
        if (off == 0)
            return 0;
        pos += chunk;
    }
    return off;
}

static uint32_t append_reg16_16_table(uint8_t *dst, uint32_t off, uint32_t cap,
                                      const struct marble_reg16_16 *regs, size_t n) {
    uint32_t addrs[64], vals[64];
    size_t pos = 0;
    while (pos < n) {
        size_t chunk = 0;
        int16_t wait_after = 0;
        while (pos + chunk < n && chunk < 64) {
            addrs[chunk] = regs[pos + chunk].addr;
            vals[chunk] = regs[pos + chunk].val;
            wait_after = (int16_t)regs[pos + chunk].delay_ms;
            chunk++;
            if (wait_after > 0)
                break;
        }
        off = append_i2c_rndm_wr_u32(dst, off, cap, 2, 2, addrs, vals, chunk);
        if (off == 0)
            return 0;
        if (wait_after > 0) {
            off = append_i2c_wait_ms(dst, off, cap, wait_after);
            if (off == 0)
                return 0;
        }
        pos += chunk;
    }
    return off;
}

/* Sends CAM_SENSOR_PROBE_CMD for any physical marble sensor on an open sensor_fd
 * so power_info & is_probe_succeed are armed before CAM_ACQUIRE_DEV. */
static int probe_sensor_on_fd(int crm_fd, int sensor_fd, const struct sensor_probe_spec *spec) {
    uint32_t cpu_flags = CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_CMD_BUF_TYPE;
    struct cam_buf i2c_b = {0}, pwr_b = {0}, pkt_b = {0};
    int rc = 0;
    if ((rc = cam_alloc_buf(crm_fd, 4096, cpu_flags, 0, &i2c_b)) != 0 ||
        (rc = cam_alloc_buf(crm_fd, 4096, cpu_flags, 0, &pwr_b)) != 0 ||
        (rc = cam_alloc_buf(crm_fd, 4096, cpu_flags, 0, &pkt_b)) != 0) {
        goto out;
    }

    struct cam_cmd_i2c_info i2c;
    memset(&i2c, 0, sizeof(i2c));
    i2c.slave_addr = spec->slave_addr;
    i2c.i2c_freq_mode = spec->i2c_freq_mode;
    i2c.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_INFO;

    struct cam_cmd_probe_v2 prb;
    memset(&prb, 0, sizeof(prb));
    prb.data_type = spec->id_data_type;
    prb.addr_type = spec->id_addr_type;
    prb.cmd_type = CAMERA_SENSOR_CMD_TYPE_PROBE;
    prb.reg_addr = spec->id_reg;
    prb.expected_data = spec->id_val;
    prb.data_mask = 0xFFFF;
    prb.camera_id = spec->slot;
    prb.pipeline_delay = 2;
    prb.logical_camera_id = spec->slot;
    snprintf(prb.sensor_name, sizeof(prb.sensor_name), "%s", spec->name);

    memcpy(i2c_b.vaddr, &i2c, sizeof(i2c));
    memcpy((uint8_t *)i2c_b.vaddr + sizeof(i2c), &prb, sizeof(prb));
    uint32_t i2c_len = (uint32_t)(sizeof(i2c) + sizeof(prb));

    uint32_t pwr_off = 0;
    uint8_t *pb = (uint8_t *)pwr_b.vaddr;
    for (size_t i = 0; i < spec->n_steps; ++i) {
        struct cam_cmd_power pwr;
        memset(&pwr, 0, sizeof(pwr));
        pwr.count = 1;
        pwr.cmd_type = spec->steps[i].cmd;
        pwr.power_settings[0].power_seq_type = spec->steps[i].seq;
        pwr.power_settings[0].config_val_low = spec->steps[i].val;
        memcpy(pb + pwr_off, &pwr, sizeof(pwr));
        pwr_off += (uint32_t)sizeof(pwr);
        if (spec->steps[i].ms > 0) {
            pwr_off = append_i2c_wait_ms(pb, pwr_off, 4096, spec->steps[i].ms);
            if (pwr_off == 0) {
                rc = -ENOSPC;
                goto out;
            }
        }
    }

    struct cam_packet *pkt = (struct cam_packet *)pkt_b.vaddr;
    pkt->header.op_code = CAM_SENSOR_PACKET_OPCODE_SENSOR_PROBE_V2;
    pkt->header.size = (uint32_t)(offsetof(struct cam_packet, payload) + 2u * sizeof(struct cam_cmd_buf_desc));
    pkt->cmd_buf_offset = 0;
    pkt->num_cmd_buf = 2;
    pkt->kmd_cmd_buf_index = 0xFFFFFFFFu;
    struct cam_cmd_buf_desc *descs = (struct cam_cmd_buf_desc *)pkt->payload;
    descs[0].mem_handle = i2c_b.handle;
    descs[0].size = i2c_len;
    descs[0].length = i2c_len;
    descs[0].type = CAM_CMD_BUF_I2C;
    descs[1].mem_handle = pwr_b.handle;
    descs[1].size = pwr_off;
    descs[1].length = pwr_off;
    descs[1].type = CAM_CMD_BUF_I2C;

    struct cam_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_code = CAM_SENSOR_PROBE_CMD;
    ctrl.size = 0;
    ctrl.handle_type = CAM_HANDLE_MEM_HANDLE;
    ctrl.handle = (uint64_t)(uint32_t)pkt_b.handle;
    rc = (ioctl(sensor_fd, VIDIOC_CAM_CONTROL, &ctrl) < 0) ? -errno : 0;

out:
    cam_free_buf(crm_fd, &pkt_b);
    cam_free_buf(crm_fd, &pwr_b);
    cam_free_buf(crm_fd, &i2c_b);
    return rc;
}

static int submit_sensor_cmdbuf(marble_hw_session_t *s, int buf_idx, uint32_t pkt_opcode,
                                uint32_t cmd_len) {
    struct cam_packet *p = (struct cam_packet *)s->src_pkt[buf_idx].vaddr;
    memset(p, 0, offsetof(struct cam_packet, payload) + sizeof(struct cam_cmd_buf_desc));
    p->header.op_code = pkt_opcode;
    p->header.size = (uint32_t)(offsetof(struct cam_packet, payload) + sizeof(struct cam_cmd_buf_desc));
    p->header.request_id = 0;
    p->cmd_buf_offset = 0;
    p->num_cmd_buf = 1;
    struct cam_cmd_buf_desc *cd = (struct cam_cmd_buf_desc *)p->payload;
    cd[0].mem_handle = s->src_cmd[buf_idx].handle;
    cd[0].offset = 0;
    cd[0].size = (uint32_t)s->src_cmd[buf_idx].size;
    cd[0].length = cmd_len;
    cd[0].type = CAM_CMD_BUF_I2C;

    struct cam_config_dev_cmd cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session_handle = s->session_hdl;
    cfg.dev_handle = s->sensor_hdl;
    cfg.offset = 0;
    cfg.packet_handle = (uint64_t)(uint32_t)s->src_pkt[buf_idx].handle;
    return cam_ctrl(s->sensor_fd, CAM_CONFIG_DEV, &cfg, sizeof(cfg));
}

static int alloc_sensor_i2c_pair(marble_hw_session_t *s, int buf_idx, size_t cmd_cap) {
    uint32_t cpu_flags = CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_CMD_BUF_TYPE;
    int rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->src_pkt[buf_idx]);
    if (rc != 0)
        return rc;
    return cam_alloc_buf(s->crm_fd, cmd_cap < 4096 ? 4096 : cmd_cap, cpu_flags, 0, &s->src_cmd[buf_idx]);
}

static int send_sensor_i2c_packet(marble_hw_session_t *s, int buf_idx, uint32_t pkt_opcode,
                                  const struct imx355_reg_pair *regs, size_t n_regs) {
    size_t cmd_bytes = ((n_regs + 63u) / 64u) * sizeof(struct i2c_rdwr_header) +
                       n_regs * sizeof(struct i2c_random_wr_payload) + 256u;
    int rc = alloc_sensor_i2c_pair(s, buf_idx, cmd_bytes);
    if (rc != 0)
        return rc;
    uint32_t addrs[64], vals[64];
    uint32_t off = 0;
    size_t pos = 0;
    while (pos < n_regs) {
        size_t chunk = (n_regs - pos > 64) ? 64 : (n_regs - pos);
        for (size_t i = 0; i < chunk; ++i) {
            addrs[i] = regs[pos + i].addr;
            vals[i] = regs[pos + i].val;
        }
        off = append_i2c_rndm_wr_u32((uint8_t *)s->src_cmd[buf_idx].vaddr, off,
                                     (uint32_t)s->src_cmd[buf_idx].size, 2, 1, addrs, vals, chunk);
        if (off == 0)
            return -ENOSPC;
        pos += chunk;
    }
    return submit_sensor_cmdbuf(s, buf_idx, pkt_opcode, off);
}

/* ---- GT9764 VCM AF Actuator (Rear Wide OV64B40, slot 0, slave 0x18) ---- */

static int gt9764_write_dac_locked(marble_hw_session_t *s, uint16_t dac, int16_t wait_ms) {
    if (!s || s->act_fd < 0 || s->act_hdl <= 0 || !s->act_configured)
        return -ENODEV;
    if (dac < 128u) dac = 128u;
    if (dac > 896u) dac = 896u;
    uint8_t *cb = (uint8_t *)s->act_cmd[2].vaddr;
    uint32_t cap = (uint32_t)s->act_cmd[2].size;
    uint32_t addr = 0x03u, val = (uint32_t)dac;
    uint32_t off = append_i2c_rndm_wr_u32(cb, 0, cap, 1, 2, &addr, &val, 1);
    if (off > 0 && wait_ms > 0)
        off = append_i2c_wait_ms(cb, off, cap, wait_ms);
    if (off == 0)
        return -ENOSPC;

    struct cam_packet *p = (struct cam_packet *)s->act_pkt.vaddr;
    memset(p, 0, offsetof(struct cam_packet, payload) + sizeof(struct cam_cmd_buf_desc));
    p->header.op_code = CAM_ACTUATOR_PACKET_OPCODE_INIT;
    p->header.size = (uint32_t)(offsetof(struct cam_packet, payload) + sizeof(struct cam_cmd_buf_desc));
    p->header.request_id = 0;
    p->cmd_buf_offset = 0;
    p->num_cmd_buf = 1;
    struct cam_cmd_buf_desc *cd = (struct cam_cmd_buf_desc *)p->payload;
    cd[0].mem_handle = s->act_cmd[2].handle;
    cd[0].offset = 0;
    cd[0].size = (uint32_t)s->act_cmd[2].size;
    cd[0].length = off;
    cd[0].type = CAM_CMD_BUF_I2C;

    struct cam_config_dev_cmd cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session_handle = s->session_hdl;
    cfg.dev_handle = s->act_hdl;
    cfg.offset = 0;
    cfg.packet_handle = (uint64_t)(uint32_t)s->act_pkt.handle;
    int rc = cam_ctrl(s->act_fd, CAM_CONFIG_DEV, &cfg, sizeof(cfg));
    if (rc == 0)
        s->focus_dac = dac;
    return rc;
}

static int gt9764_actuator_open(marble_hw_session_t *s, uint16_t init_dac) {
    char act_path[64] = {0};
    if (find_subdev_by_slot(CAM_ACTUATOR_DEVICE_TYPE, 0, act_path, sizeof(act_path)) != 0)
        return -ENOENT;
    s->act_fd = open(act_path, O_RDWR);
    if (s->act_fd < 0)
        return -errno;

    struct cam_sensor_acquire_dev acq;
    memset(&acq, 0, sizeof(acq));
    acq.session_handle = (uint32_t)s->session_hdl;
    acq.handle_type = CAM_HANDLE_USER_POINTER;
    int rc = cam_ctrl(s->act_fd, CAM_ACQUIRE_DEV, &acq, sizeof(acq));
    if (rc != 0) {
        close(s->act_fd);
        s->act_fd = -1;
        return rc;
    }
    s->act_hdl = (int32_t)acq.device_handle;

    uint32_t cpu_flags = CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_CMD_BUF_TYPE;
    if ((rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->act_pkt)) != 0 ||
        (rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->act_cmd[0])) != 0 ||
        (rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->act_cmd[1])) != 0 ||
        (rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->act_cmd[2])) != 0) {
        return rc;
    }

    /* buf 0: I2C slave info (GT9764 8-bit slave 0x18, FAST_PLUS mode 3) */
    struct cam_cmd_i2c_info i2c;
    memset(&i2c, 0, sizeof(i2c));
    i2c.slave_addr = 0x18u;
    i2c.i2c_freq_mode = 3u;
    i2c.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_INFO;
    memcpy(s->act_cmd[0].vaddr, &i2c, sizeof(i2c));

    /* buf 1: Stock Xiaomi marble VAF power-up (10ms) & power-down (1ms) */
    uint8_t *pb = (uint8_t *)s->act_cmd[1].vaddr;
    uint32_t pwr_off = 0;
    struct cam_cmd_power pwr;
    memset(&pwr, 0, sizeof(pwr));
    pwr.count = 1;
    pwr.cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_UP;
    pwr.power_settings[0].power_seq_type = 4; /* SENSOR_VAF */
    pwr.power_settings[0].config_val_low = 1;
    memcpy(pb + pwr_off, &pwr, sizeof(pwr));
    pwr_off += (uint32_t)sizeof(pwr);
    pwr_off = append_i2c_wait_ms(pb, pwr_off, 4096, 10);

    memset(&pwr, 0, sizeof(pwr));
    pwr.count = 1;
    pwr.cmd_type = CAMERA_SENSOR_CMD_TYPE_PWR_DOWN;
    pwr.power_settings[0].power_seq_type = 4; /* SENSOR_VAF */
    pwr.power_settings[0].config_val_low = 0;
    memcpy(pb + pwr_off, &pwr, sizeof(pwr));
    pwr_off += (uint32_t)sizeof(pwr);
    pwr_off = append_i2c_wait_ms(pb, pwr_off, 4096, 1);

    /* buf 2: Stock 5-step GT9764 AAC3 ringing-control init + hyperfocal target DAC */
    uint16_t target_dac = (init_dac >= 128u && init_dac <= 896u) ? init_dac : 370u;
    uint8_t *rb = (uint8_t *)s->act_cmd[2].vaddr;
    uint32_t r_off = 0;
    uint32_t a, v;
    a = 0x02u; v = 0x02u; r_off = append_i2c_rndm_wr_u32(rb, r_off, 4096, 1, 1, &a, &v, 1);
    r_off = append_i2c_wait_ms(rb, r_off, 4096, 1);
    a = 0x06u; v = 0x40u; r_off = append_i2c_rndm_wr_u32(rb, r_off, 4096, 1, 1, &a, &v, 1);
    r_off = append_i2c_wait_ms(rb, r_off, 4096, 1);
    a = 0x07u; v = 0x06u; r_off = append_i2c_rndm_wr_u32(rb, r_off, 4096, 1, 1, &a, &v, 1);
    r_off = append_i2c_wait_ms(rb, r_off, 4096, 1);
    a = 0x03u; v = 430u;  r_off = append_i2c_rndm_wr_u32(rb, r_off, 4096, 1, 2, &a, &v, 1);
    r_off = append_i2c_wait_ms(rb, r_off, 4096, 20);
    a = 0x03u; v = target_dac; r_off = append_i2c_rndm_wr_u32(rb, r_off, 4096, 1, 2, &a, &v, 1);
    if (pwr_off == 0 || r_off == 0)
        return -ENOSPC;

    struct cam_packet *p = (struct cam_packet *)s->act_pkt.vaddr;
    memset(p, 0, offsetof(struct cam_packet, payload) + 3u * sizeof(struct cam_cmd_buf_desc));
    p->header.op_code = CAM_ACTUATOR_PACKET_OPCODE_INIT;
    p->header.size = (uint32_t)(offsetof(struct cam_packet, payload) + 3u * sizeof(struct cam_cmd_buf_desc));
    p->header.request_id = 0;
    p->cmd_buf_offset = 0;
    p->num_cmd_buf = 3;
    struct cam_cmd_buf_desc *cd = (struct cam_cmd_buf_desc *)p->payload;
    cd[0].mem_handle = s->act_cmd[0].handle; cd[0].size = (uint32_t)sizeof(i2c); cd[0].length = (uint32_t)sizeof(i2c); cd[0].type = CAM_CMD_BUF_I2C;
    cd[1].mem_handle = s->act_cmd[1].handle; cd[1].size = pwr_off; cd[1].length = pwr_off; cd[1].type = CAM_CMD_BUF_I2C;
    cd[2].mem_handle = s->act_cmd[2].handle; cd[2].size = r_off;   cd[2].length = r_off;   cd[2].type = CAM_CMD_BUF_I2C;

    struct cam_config_dev_cmd cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session_handle = s->session_hdl;
    cfg.dev_handle = s->act_hdl;
    cfg.offset = 0;
    cfg.packet_handle = (uint64_t)(uint32_t)s->act_pkt.handle;
    rc = cam_ctrl(s->act_fd, CAM_CONFIG_DEV, &cfg, sizeof(cfg));
    if (rc == 0) {
        s->act_configured = true;
        s->focus_dac = target_dac;
    }
    return rc;
}

/* ---- Live CCI I2C Exposure & Analog Gain Writer (all 4 sensors) ---- */

static uint8_t gc02m1_gain_q8_to_idx(uint32_t gain_q8) {
    /* Stock GC02M1 analog gain steps (Q8: 256=1.0x up to 12.0x) */
    static const uint16_t k_gc_tbl[16] = {
        256, 365, 512, 730, 1024, 1460, 2048, 2920,
        3072, 3328, 3584, 3840, 4096, 4096, 4096, 4096
    };
    uint8_t best = 0;
    for (uint8_t i = 0; i < 16; ++i) {
        if (gain_q8 >= k_gc_tbl[i])
            best = i;
        else
            break;
    }
    return best;
}

int marble_hw_session_set_exposure(marble_hw_session_t *s,
                                   uint32_t exp_lines, uint32_t gain_q8) {
    if (!s || !s->is_sensor || s->sensor_fd < 0 || s->sensor_hdl <= 0 || !s->src_cmd[4].vaddr)
        return -EINVAL;
    if (exp_lines < s->ae_lim.min_exp) exp_lines = s->ae_lim.min_exp;
    if (exp_lines > s->ae_lim.max_exp) exp_lines = s->ae_lim.max_exp;
    if (gain_q8 < s->ae_lim.min_gain_q8) gain_q8 = s->ae_lim.min_gain_q8;
    if (gain_q8 > s->ae_lim.max_gain_q8) gain_q8 = s->ae_lim.max_gain_q8;

    uint8_t *cb = (uint8_t *)s->src_cmd[4].vaddr;
    uint32_t cap = (uint32_t)s->src_cmd[4].size;
    uint32_t off = 0;

    if (s->src == MARBLE_HW_SRC_OV64B40) {
        /* OV64B40 group-0 atomic update: 0x3500..0x3502 exp, 0x3508..0x3509 gain (0x100=1x) */
        uint32_t ag = gain_q8;
        if (ag < 0x0100u) ag = 0x0100u;
        if (ag > 0x0F80u) ag = 0x0F80u;
        struct marble_reg16_8 regs[] = {
            { 0x3208u, 0x00u },
            { 0x3500u, (uint8_t)((exp_lines >> 16) & 0x0Fu) },
            { 0x3501u, (uint8_t)((exp_lines >> 8) & 0xFFu) },
            { 0x3502u, (uint8_t)(exp_lines & 0xFFu) },
            { 0x3508u, (uint8_t)((ag >> 8) & 0xFFu) },
            { 0x3509u, (uint8_t)(ag & 0xFFu) },
            { 0x3208u, 0x10u },
            { 0x3208u, 0xA0u },
        };
        off = append_reg16_8_table(cb, 0, cap, regs, sizeof(regs) / sizeof(regs[0]));
    } else if (s->src == MARBLE_HW_SRC_S5K3P9) {
        /* S5K3P9 16-bit addr/data: 0x0202 exp, 0x0204 gain (32=1x => gain_q8 >> 3) */
        uint16_t ag = (uint16_t)(gain_q8 >> 3);
        if (ag < 0x0020u) ag = 0x0020u;
        if (ag > 0x0200u) ag = 0x0200u;
        struct marble_reg16_16 regs[] = {
            { 0x0202u, (uint16_t)exp_lines, 0 },
            { 0x0204u, ag, 0 },
        };
        off = append_reg16_16_table(cb, 0, cap, regs, sizeof(regs) / sizeof(regs[0]));
    } else if (s->src == MARBLE_HW_SRC_IMX355) {
        /* Sony IMX355 grouped parameter hold: gain_reg = 1024 - 1024/(gain_q8/256) */
        uint32_t denom = (gain_q8 < 256u) ? 256u : gain_q8;
        uint32_t greg = (denom >= 4096u) ? 960u : (1024u - (1024u * 256u) / denom);
        if (greg > 960u) greg = 960u;
        struct marble_reg16_8 regs[] = {
            { 0x0104u, 0x01u },
            { 0x0202u, (uint8_t)((exp_lines >> 8) & 0xFFu) },
            { 0x0203u, (uint8_t)(exp_lines & 0xFFu) },
            { 0x0204u, (uint8_t)((greg >> 8) & 0xFFu) },
            { 0x0205u, (uint8_t)(greg & 0xFFu) },
            { 0x0104u, 0x00u },
        };
        off = append_reg16_8_table(cb, 0, cap, regs, sizeof(regs) / sizeof(regs[0]));
    } else if (s->src == MARBLE_HW_SRC_GC02M1) {
        uint8_t ag_idx = gc02m1_gain_q8_to_idx(gain_q8);
        struct marble_reg8_pair regs[] = {
            { 0xfe, 0x00 },
            { 0x03, (uint8_t)((exp_lines >> 8) & 0x3Fu) },
            { 0x04, (uint8_t)(exp_lines & 0xFFu) },
            { 0xb6, ag_idx },
        };
        off = append_reg8_table(cb, 0, cap, regs, sizeof(regs) / sizeof(regs[0]));
    } else {
        return -ENOTSUP;
    }

    if (off == 0)
        return -ENOSPC;
    /* CAM_SENSOR_PACKET_OPCODE_SENSOR_INITIAL_CONFIG applies immediately in cam_sensor_core.c
     * without disturbing s_ctrl->sensor_state == CAM_SENSOR_START. */
    int rc = submit_sensor_cmdbuf(s, 4, CAM_SENSOR_PACKET_OPCODE_SENSOR_INITIAL_CONFIG, off);
    if (rc == 0) {
        s->ae_exp = exp_lines;
        s->ae_gain_q8 = gain_q8;
        s->ae_updates++;
    }
    return rc;
}

int marble_hw_session_set_focus(marble_hw_session_t *s, uint16_t dac) {
    return gt9764_write_dac_locked(s, dac, 2);
}

int marble_hw_session_actuator_ok(const marble_hw_session_t *s) {
    return (s && s->act_configured) ? 1 : 0;
}

static int queue_isp_slot(marble_hw_session_t *s, int slot) {
    char fname[32];
    uint32_t req_id = s->next_req_id++;
    snprintf(fname, sizeof(fname), "marble_rdi_%u", req_id);
    if (s->fences[slot] > 0) {
        sync_destroy(s->sync_fd, s->fences[slot]);
        s->fences[slot] = 0;
    }
    int fence = sync_create(s->sync_fd, fname);
    if (fence <= 0)
        return (fence < 0) ? fence : -EIO;
    s->fences[slot] = fence;
    s->slot_req_id[slot] = req_id;

    if (s->img_buf[slot].vaddr)
        memset(s->img_buf[slot].vaddr, 0xA5, s->raw_bytes);
    (void)cam_cache_op(s->crm_fd, s->img_buf[slot].handle, CAM_MEM_CLEAN_CACHE);

    memset(s->upd_pkt[slot].vaddr, 0, 512);
    uint32_t io_off = (uint32_t)sizeof(struct cam_cmd_buf_desc);
    struct cam_packet *up = (struct cam_packet *)s->upd_pkt[slot].vaddr;
    up->header.op_code = CAM_ISP_PACKET_UPDATE_DEV - 1u; /* kernel adds +1 */
    up->header.size = (uint32_t)(offsetof(struct cam_packet, payload) +
                                 sizeof(struct cam_cmd_buf_desc) + sizeof(struct cam_buf_io_cfg));
    up->header.request_id = (uint64_t)req_id;
    up->cmd_buf_offset = 0;
    up->num_cmd_buf = 1;
    up->io_configs_offset = io_off;
    up->num_io_configs = 1;
    up->kmd_cmd_buf_index = 0;
    up->kmd_cmd_buf_offset = 0;

    struct cam_cmd_buf_desc *ucd = (struct cam_cmd_buf_desc *)up->payload;
    ucd[0].mem_handle = s->upd_kmd[slot].handle;
    ucd[0].offset = 0;
    ucd[0].size = (uint32_t)s->upd_kmd[slot].size;
    ucd[0].length = 0;
    ucd[0].type = CAM_CMD_BUF_DIRECT;
    ucd[0].meta_data = CAM_ISP_PACKET_META_LEFT;

    struct cam_buf_io_cfg *io = (struct cam_buf_io_cfg *)((uint8_t *)up->payload + io_off);
    io->mem_handle[0] = s->img_buf[slot].handle;
    io->offsets[0] = 0;
    io->planes[0].width = s->width;
    io->planes[0].height = s->height;
    io->planes[0].plane_stride = s->stride;
    io->planes[0].slice_height = s->height;
    io->format = s->cam_fmt;
    io->color_space = CAM_COLOR_SPACE_BASE;
    io->color_pattern = CAM_ISP_PATTERN_BAYER_RGRGRG;
    io->bpp = s->bpp;
    io->rotation = CAM_ROTATE_CW_0_DEGREE;
    io->resource_type = CAM_ISP_IFE_OUT_RES_RDI_0;
    io->fence = fence;
    io->early_fence = -1;
    io->direction = CAM_BUF_OUTPUT;
    io->batch_size = 1;

    struct cam_req_mgr_sched_request sched;
    memset(&sched, 0, sizeof(sched));
    sched.session_hdl = s->session_hdl;
    sched.link_hdl = s->link_hdl;
    sched.bubble_enable = 1;
    sched.sync_mode = CAM_REQ_MGR_SYNC_MODE_NO_SYNC;
    sched.req_id = (int64_t)req_id;
    int rc = cam_ctrl(s->crm_fd, CAM_REQ_MGR_SCHED_REQ, &sched, sizeof(sched));
    if (rc != 0)
        return rc;

    struct cam_config_dev_cmd cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.session_handle = s->session_hdl;
    cfg.dev_handle = s->isp_hdl;
    cfg.offset = 0;
    cfg.packet_handle = (uint64_t)(uint32_t)s->upd_pkt[slot].handle;
    rc = cam_ctrl(s->isp_fd, CAM_CONFIG_DEV, &cfg, sizeof(cfg));
    if (rc != 0)
        return rc;

    s->ring_tail = (s->ring_tail + 1) % MARBLE_RING_SLOTS;
    s->ring_queued++;
    return 0;
}

int marble_hw_session_open(const marble_hw_config_t *cfg,
                           marble_hw_session_t **out_sess,
                           char *err_buf, size_t err_sz) {
    if (!cfg || !out_sess)
        return -EINVAL;
    *out_sess = NULL;

    marble_hw_session_t *s = (marble_hw_session_t *)calloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->crm_fd = s->sync_fd = s->isp_fd = s->tpg_fd = s->csiphy_fd = s->sensor_fd = s->act_fd = -1;
    s->session_hdl = s->isp_hdl = s->tpg_hdl = s->csiphy_hdl = s->sensor_hdl = s->act_hdl = s->link_hdl = -1;
    s->src = cfg->src;
    s->next_req_id = 1;

    int rc = 0;
    uint32_t sensor_slot = 0, csiphy_slot = 0, isp_in_res = CAM_ISP_IFE_IN_RES_CPHY_TPG_0;
    uint8_t csiphy_3phase = 0, lane_cnt = 4;
    uint16_t lane_assign = 0x3210;
    uint64_t settle_time = 3000000000ULL, data_rate = 720000000ULL;
    s->is_sensor = (cfg->src != MARBLE_HW_SRC_TPG) ? 1 : 0;
    s->bayer_order = MARBLE_BAYER_RGGB;

    if (cfg->src == MARBLE_HW_SRC_IMX355) {
        bool use_820 = (cfg->width == 820 && cfg->height == 616);
        s->width = use_820 ? 820u : 1640u;
        s->height = use_820 ? 616u : 1232u;
        s->use_raw8 = 0;
        s->is_optical = (cfg->test_pattern == 0) ? 1 : 0;
        s->bayer_order = MARBLE_BAYER_RGGB;
        sensor_slot = 2; csiphy_slot = 0; isp_in_res = CAM_ISP_IFE_IN_RES_PHY_0;
        csiphy_3phase = 0; lane_cnt = 4; lane_assign = 0x3210;
        settle_time = 3000000000ULL; data_rate = 720000000ULL;
    } else if (cfg->src == MARBLE_HW_SRC_GC02M1) {
        s->width = 1600u;
        s->height = 1200u;
        s->use_raw8 = 0;
        s->is_optical = (cfg->test_pattern == 0) ? 1 : 0;
        s->bayer_order = MARBLE_BAYER_GBRG;
        sensor_slot = 3; csiphy_slot = 1; isp_in_res = CAM_ISP_IFE_IN_RES_PHY_1;
        csiphy_3phase = 0; lane_cnt = 1; lane_assign = 0x0000;
        settle_time = 24ULL * 200000000ULL; data_rate = 672000000ULL;
    } else if (cfg->src == MARBLE_HW_SRC_S5K3P9) {
        s->width = 2304u;
        s->height = 1728u;
        s->use_raw8 = 0;
        s->is_optical = (cfg->test_pattern == 0) ? 1 : 0;
        s->bayer_order = MARBLE_BAYER_GRBG;
        sensor_slot = 1; csiphy_slot = 3; isp_in_res = CAM_ISP_IFE_IN_RES_PHY_3;
        csiphy_3phase = 0; lane_cnt = 4; lane_assign = 0x3210;
        settle_time = 28ULL * 200000000ULL; data_rate = 545000000ULL;
    } else if (cfg->src == MARBLE_HW_SRC_OV64B40) {
        bool use_16mp = (cfg->width == 4624 && cfg->height == 3472);
        s->width = use_16mp ? 4624u : 1920u;
        s->height = use_16mp ? 3472u : 1080u;
        s->use_raw8 = 0;
        s->is_optical = (cfg->test_pattern == 0) ? 1 : 0;
        s->bayer_order = MARBLE_BAYER_RGGB;
        sensor_slot = 0; csiphy_slot = 2; isp_in_res = CAM_ISP_IFE_IN_RES_PHY_2;
        csiphy_3phase = 1; lane_cnt = 3; lane_assign = 0x0210;
        settle_time = (use_16mp ? 30ULL : 20ULL) * 200000000ULL;
        data_rate = 3840000000ULL; /* 1152MHz * 10 / 3 (~1684 MSpS * 2.28) */
    } else {
        s->width = cfg->width ? cfg->width : 640u;
        s->height = cfg->height ? cfg->height : 480u;
        s->use_raw8 = cfg->use_raw8 ? 1 : 0;
        s->is_optical = 0;
        s->bayer_order = MARBLE_BAYER_RGGB;
    }
    s->cam_fmt = s->use_raw8 ? CAM_FORMAT_MIPI_RAW_8 : CAM_FORMAT_MIPI_RAW_10;
    s->bpp = s->use_raw8 ? 8u : 10u;
    uint32_t mipi_dt = s->use_raw8 ? 0x2Au : 0x2Bu;
    s->stride = s->use_raw8 ? ((s->width + 15u) & ~15u) : ((((s->width * 5u) / 4u) + 15u) & ~15u);
    s->raw_bytes = s->stride * s->height;

    char isp_path[64] = {0}, src_path[64] = {0}, phy_path[64] = {0};
    if ((rc = find_subdev_by_type(CAM_IFE_DEVICE_TYPE, 0, isp_path, sizeof(isp_path))) != 0) {
        set_err(err_buf, err_sz, "find cam-isp failed", rc);
        goto fail;
    }
    if (s->is_sensor) {
        if ((rc = find_subdev_by_slot(CAM_SENSOR_DEVICE_TYPE, sensor_slot, src_path, sizeof(src_path))) != 0) {
            set_err(err_buf, err_sz, "find sensor subdev failed", rc);
            goto fail;
        }
        if ((rc = find_subdev_by_slot(CAM_CSIPHY_DEVICE_TYPE, csiphy_slot, phy_path, sizeof(phy_path))) != 0) {
            set_err(err_buf, err_sz, "find CSIPHY subdev failed", rc);
            goto fail;
        }
    } else {
        if ((rc = find_subdev_by_type(CAM_TPG_DEVICE_TYPE, 0, src_path, sizeof(src_path))) != 0) {
            set_err(err_buf, err_sz, "find cam-tpg0 failed", rc);
            goto fail;
        }
    }

    s->crm_fd = open("/dev/video0", O_RDWR);
    s->sync_fd = open("/dev/video1", O_RDWR);
    s->isp_fd = open(isp_path, O_RDWR);
    if (s->is_sensor) {
        s->sensor_fd = open(src_path, O_RDWR);
        s->csiphy_fd = open(phy_path, O_RDWR);
    } else {
        s->tpg_fd = open(src_path, O_RDWR);
    }
    if (s->crm_fd < 0 || s->sync_fd < 0 || s->isp_fd < 0 ||
        (s->is_sensor && (s->sensor_fd < 0 || s->csiphy_fd < 0)) ||
        (!s->is_sensor && s->tpg_fd < 0)) {
        rc = -errno;
        set_err(err_buf, err_sz, "open camera device nodes failed", rc);
        goto fail;
    }

    /* 1. Query ISP IOMMU handles */
    struct cam_isp_query_cap_cmd isp_cap;
    memset(&isp_cap, 0, sizeof(isp_cap));
    struct cam_query_cap_cmd qcap;
    memset(&qcap, 0, sizeof(qcap));
    qcap.size = sizeof(isp_cap);
    qcap.handle_type = CAM_HANDLE_USER_POINTER;
    qcap.caps_handle = (uint64_t)(uintptr_t)&isp_cap;
    if ((rc = cam_ctrl(s->isp_fd, CAM_QUERY_CAP, &qcap, sizeof(qcap))) != 0) {
        set_err(err_buf, err_sz, "ISP CAM_QUERY_CAP failed", rc);
        goto fail;
    }
    s->img_iommu = isp_cap.device_iommu.non_secure;
    s->cdm_iommu = isp_cap.cdm_iommu.non_secure;

    /* 2. Create CRM session */
    struct cam_req_mgr_session_info sess;
    memset(&sess, 0, sizeof(sess));
    if ((rc = cam_ctrl(s->crm_fd, CAM_REQ_MGR_CREATE_SESSION, &sess, sizeof(sess))) != 0) {
        set_err(err_buf, err_sz, "CRM CREATE_SESSION failed", rc);
        goto fail;
    }
    s->session_hdl = sess.session_hdl;

    uint32_t cpu_flags = CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_CMD_BUF_TYPE;

    /* 3. Acquire & configure source device (physical sensor + CSIPHY, OR TPG0) */
    if (s->is_sensor) {
        /* 3a. Probe sensor on this fd to arm power_info & is_probe_succeed, then acquire & power up */
        if ((rc = probe_sensor_on_fd(s->crm_fd, s->sensor_fd, &k_probe_specs[s->src])) != 0) {
            set_err(err_buf, err_sz, "sensor CAM_SENSOR_PROBE_CMD failed", rc);
            goto fail;
        }
        struct cam_sensor_acquire_dev sens_acq;
        memset(&sens_acq, 0, sizeof(sens_acq));
        sens_acq.session_handle = (uint32_t)s->session_hdl;
        sens_acq.handle_type = CAM_HANDLE_USER_POINTER;
        if ((rc = cam_ctrl(s->sensor_fd, CAM_ACQUIRE_DEV, &sens_acq, sizeof(sens_acq))) != 0) {
            set_err(err_buf, err_sz, "sensor CAM_ACQUIRE_DEV failed", rc);
            goto fail;
        }
        s->sensor_hdl = (int32_t)sens_acq.device_handle;

        /* 3b. Acquire CSIPHY (2-phase D-PHY or 3-phase C-PHY) */
        struct cam_csiphy_acquire_dev_info phy_params;
        memset(&phy_params, 0, sizeof(phy_params));
        phy_params.combo_mode = 0;
        phy_params.csiphy_3phase = csiphy_3phase;
        struct cam_sensor_acquire_dev phy_acq;
        memset(&phy_acq, 0, sizeof(phy_acq));
        phy_acq.session_handle = (uint32_t)s->session_hdl;
        phy_acq.handle_type = CAM_HANDLE_USER_POINTER;
        phy_acq.info_handle = (uint64_t)(uintptr_t)&phy_params;
        if ((rc = cam_ctrl(s->csiphy_fd, CAM_ACQUIRE_DEV, &phy_acq, sizeof(phy_acq))) != 0) {
            set_err(err_buf, err_sz, "CSIPHY CAM_ACQUIRE_DEV failed", rc);
            goto fail;
        }
        s->csiphy_hdl = (int32_t)phy_acq.device_handle;

        /* 3c. Configure CSIPHY lanes, settle_time, and data_rate */
        if ((rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->src_pkt[0])) != 0 ||
            (rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->src_cmd[0])) != 0) {
            set_err(err_buf, err_sz, "alloc CSIPHY bufs failed", rc);
            goto fail;
        }
        struct cam_csiphy_info pinfo;
        memset(&pinfo, 0, sizeof(pinfo));
        pinfo.lane_assign = lane_assign;
        pinfo.lane_cnt = lane_cnt;
        pinfo.secure_mode = 0;
        pinfo.settle_time = settle_time;
        pinfo.data_rate = data_rate;
        memcpy(s->src_cmd[0].vaddr, &pinfo, sizeof(pinfo));

        struct cam_packet *pp = (struct cam_packet *)s->src_pkt[0].vaddr;
        pp->header.op_code = 0;
        pp->header.size = (uint32_t)(offsetof(struct cam_packet, payload) + sizeof(struct cam_cmd_buf_desc));
        pp->cmd_buf_offset = 0;
        pp->num_cmd_buf = 1;
        struct cam_cmd_buf_desc *pcd = (struct cam_cmd_buf_desc *)pp->payload;
        pcd[0].mem_handle = s->src_cmd[0].handle;
        pcd[0].offset = 0;
        pcd[0].size = sizeof(pinfo);
        pcd[0].length = sizeof(pinfo);
        pcd[0].type = CAM_CMD_BUF_GENERIC;

        struct cam_config_dev_cmd pcfg;
        memset(&pcfg, 0, sizeof(pcfg));
        pcfg.session_handle = s->session_hdl;
        pcfg.dev_handle = s->csiphy_hdl;
        pcfg.packet_handle = (uint64_t)(uint32_t)s->src_pkt[0].handle;
        if ((rc = cam_ctrl(s->csiphy_fd, CAM_CONFIG_DEV, &pcfg, sizeof(pcfg))) != 0) {
            set_err(err_buf, err_sz, "CSIPHY CAM_CONFIG_DEV failed", rc);
            goto fail;
        }

        /* 3d. Program sensor INITIAL_CONFIG, STREAMON, STREAMOFF */
        if (s->src == MARBLE_HW_SRC_IMX355) {
            const struct imx355_reg_pair *mregs = (s->width == 820) ? k_imx355_mode_820x616 : k_imx355_mode_1640x1232;
            size_t n_mregs = (s->width == 820)
                ? (sizeof(k_imx355_mode_820x616) / sizeof(k_imx355_mode_820x616[0]))
                : (sizeof(k_imx355_mode_1640x1232) / sizeof(k_imx355_mode_1640x1232[0]));
            size_t n_gregs = sizeof(k_imx355_global_regs) / sizeof(k_imx355_global_regs[0]);

            uint32_t exp_lines = cfg->exposure_lines ? cfg->exposure_lines : ((s->width == 820) ? 0x0260u : 0x04E0u);
            uint32_t again = cfg->analog_gain ? cfg->analog_gain : 0x0360u;
            if (again > 960u) again = 960u;
            uint16_t tpat = (uint16_t)(cfg->test_pattern >= 0 ? cfg->test_pattern : 0);

            struct imx355_reg_pair extra_regs[] = {
                { 0x0100, 0x00 },
                { 0x0202, (uint8_t)((exp_lines >> 8) & 0xFFu) },
                { 0x0203, (uint8_t)(exp_lines & 0xFFu) },
                { 0x0204, (uint8_t)((again >> 8) & 0xFFu) },
                { 0x0205, (uint8_t)(again & 0xFFu) },
                { 0x3070, 0x01 },
                { 0x020e, 0x01 },
                { 0x020f, 0x80 },
                { 0x0600, (uint8_t)((tpat >> 8) & 0xFFu) },
                { 0x0601, (uint8_t)(tpat & 0xFFu) },
            };
            size_t n_extra = sizeof(extra_regs) / sizeof(extra_regs[0]);
            struct imx355_reg_pair *all_regs =
                (struct imx355_reg_pair *)malloc((n_gregs + n_mregs + n_extra) * sizeof(*all_regs));
            if (!all_regs) {
                rc = -ENOMEM;
                goto fail;
            }
            all_regs[0] = extra_regs[0];
            memcpy(all_regs + 1, k_imx355_global_regs, n_gregs * sizeof(*all_regs));
            memcpy(all_regs + 1 + n_gregs, mregs, n_mregs * sizeof(*all_regs));
            memcpy(all_regs + 1 + n_gregs + n_mregs, extra_regs + 1, (n_extra - 1) * sizeof(*all_regs));
            rc = send_sensor_i2c_packet(s, 1, CAM_SENSOR_PACKET_OPCODE_SENSOR_INITIAL_CONFIG,
                                        all_regs, n_gregs + n_mregs + n_extra);
            free(all_regs);
            if (rc != 0) {
                set_err(err_buf, err_sz, "IMX355 INITIAL_CONFIG failed", rc);
                goto fail;
            }
            const struct imx355_reg_pair reg_on[1] = { { 0x0100, 0x01 } };
            const struct imx355_reg_pair reg_off[1] = { { 0x0100, 0x00 } };
            if ((rc = send_sensor_i2c_packet(s, 2, CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMON, reg_on, 1)) != 0 ||
                (rc = send_sensor_i2c_packet(s, 3, CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMOFF, reg_off, 1)) != 0) {
                set_err(err_buf, err_sz, "IMX355 STREAMON/OFF setup failed", rc);
                goto fail;
            }
        } else if (s->src == MARBLE_HW_SRC_GC02M1) {
            if ((rc = alloc_sensor_i2c_pair(s, 1, 8192)) != 0) {
                set_err(err_buf, err_sz, "alloc GC02M1 init buf failed", rc);
                goto fail;
            }
            uint8_t *cb = (uint8_t *)s->src_cmd[1].vaddr;
            uint32_t cap = (uint32_t)s->src_cmd[1].size;
            uint32_t off = append_reg8_table(cb, 0, cap, k_gc02m1_init_regs,
                                             sizeof(k_gc02m1_init_regs) / sizeof(k_gc02m1_init_regs[0]));
            uint32_t exp = cfg->exposure_lines ? cfg->exposure_lines : 0x04D0u;
            uint32_t ag  = cfg->analog_gain ? cfg->analog_gain : 0x06u;
            if (ag > 15u) ag = 15u;
            struct marble_reg8_pair gc_exp[] = {
                { 0xfe, 0x00 },
                { 0x03, (uint8_t)((exp >> 8) & 0x3Fu) },
                { 0x04, (uint8_t)(exp & 0xFFu) },
                { 0xb6, (uint8_t)ag },
                { 0xb1, 0x01 },
                { 0xb2, 0x80 },
            };
            if (off > 0)
                off = append_reg8_table(cb, off, cap, gc_exp, sizeof(gc_exp) / sizeof(gc_exp[0]));
            if (off > 0 && cfg->test_pattern > 0) {
                static const struct marble_reg8_pair gc_tp[] = {
                    { 0xfe, 0x01 }, { 0x8c, 0x11 }, { 0xfe, 0x00 },
                };
                off = append_reg8_table(cb, off, cap, gc_tp, 3);
            }
            if (off == 0 || (rc = submit_sensor_cmdbuf(s, 1, CAM_SENSOR_PACKET_OPCODE_SENSOR_INITIAL_CONFIG, off)) != 0) {
                set_err(err_buf, err_sz, "GC02M1 INITIAL_CONFIG failed", rc ? rc : -ENOSPC);
                goto fail;
            }
            static const struct marble_reg8_pair gc_on[]  = { { 0xfe, 0x00 }, { 0x17, 0x83 }, { 0x3e, 0x90 } };
            static const struct marble_reg8_pair gc_off[] = { { 0xfe, 0x00 }, { 0x3e, 0x00 } };
            if ((rc = alloc_sensor_i2c_pair(s, 2, 4096)) != 0 ||
                (off = append_reg8_table((uint8_t *)s->src_cmd[2].vaddr, 0, 4096, gc_on, 3)) == 0 ||
                (rc = submit_sensor_cmdbuf(s, 2, CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMON, off)) != 0 ||
                (rc = alloc_sensor_i2c_pair(s, 3, 4096)) != 0 ||
                (off = append_reg8_table((uint8_t *)s->src_cmd[3].vaddr, 0, 4096, gc_off, 2)) == 0 ||
                (rc = submit_sensor_cmdbuf(s, 3, CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMOFF, off)) != 0) {
                set_err(err_buf, err_sz, "GC02M1 STREAMON/OFF failed", rc ? rc : -ENOSPC);
                goto fail;
            }
        } else if (s->src == MARBLE_HW_SRC_S5K3P9) {
            if ((rc = alloc_sensor_i2c_pair(s, 1, 32768)) != 0) {
                set_err(err_buf, err_sz, "alloc S5K3P9 init buf failed", rc);
                goto fail;
            }
            uint8_t *cb = (uint8_t *)s->src_cmd[1].vaddr;
            uint32_t cap = (uint32_t)s->src_cmd[1].size;
            uint32_t off = append_reg16_16_table(cb, 0, cap, k_s5k3p9_init_pre,
                                                 sizeof(k_s5k3p9_init_pre) / sizeof(k_s5k3p9_init_pre[0]));
            if (off > 0)
                off = append_i2c_burst_wr_u16(cb, off, cap, 2, 2, 0x6f12u, k_s5k3p9_burst_6f12,
                                              sizeof(k_s5k3p9_burst_6f12) / sizeof(k_s5k3p9_burst_6f12[0]));
            if (off > 0)
                off = append_reg16_16_table(cb, off, cap, k_s5k3p9_init_post,
                                            sizeof(k_s5k3p9_init_post) / sizeof(k_s5k3p9_init_post[0]));
            if (off > 0)
                off = append_reg16_16_table(cb, off, cap, k_s5k3p9_mode_2304x1728,
                                            sizeof(k_s5k3p9_mode_2304x1728) / sizeof(k_s5k3p9_mode_2304x1728[0]));
            uint16_t exp = (uint16_t)(cfg->exposure_lines ? cfg->exposure_lines : 0x0900u);
            uint16_t ag  = (uint16_t)(cfg->analog_gain ? cfg->analog_gain : 0x00C0u); /* 6x (32=1x) */
            struct marble_reg16_16 s5_exp[] = {
                { 0x0202u, exp, 0 },
                { 0x0204u, ag,  0 },
                { 0x0600u, (uint16_t)(cfg->test_pattern > 0 ? cfg->test_pattern : 0), 0 },
            };
            if (off > 0)
                off = append_reg16_16_table(cb, off, cap, s5_exp, 3);
            if (off == 0 || (rc = submit_sensor_cmdbuf(s, 1, CAM_SENSOR_PACKET_OPCODE_SENSOR_INITIAL_CONFIG, off)) != 0) {
                set_err(err_buf, err_sz, "S5K3P9 INITIAL_CONFIG failed", rc ? rc : -ENOSPC);
                goto fail;
            }
            /* S5K3P9 STREAMON: (0x6028=0x4000, 16-bit), (0x0101=0x03, 8-bit), (0x0100=0x01, 8-bit), wait 5ms */
            if ((rc = alloc_sensor_i2c_pair(s, 2, 4096)) != 0 ||
                (rc = alloc_sensor_i2c_pair(s, 3, 4096)) != 0) {
                set_err(err_buf, err_sz, "alloc S5K3P9 stream bufs failed", rc);
                goto fail;
            }
            uint32_t a16[1] = { 0x6028u }, v16[1] = { 0x4000u };
            uint32_t a8[2]  = { 0x0101u, 0x0100u }, v8[2] = { 0x03u, 0x01u };
            off = append_i2c_rndm_wr_u32((uint8_t *)s->src_cmd[2].vaddr, 0, 4096, 2, 2, a16, v16, 1);
            if (off > 0)
                off = append_i2c_rndm_wr_u32((uint8_t *)s->src_cmd[2].vaddr, off, 4096, 2, 1, a8, v8, 2);
            if (off > 0)
                off = append_i2c_wait_ms((uint8_t *)s->src_cmd[2].vaddr, off, 4096, 5);
            if (off == 0 || (rc = submit_sensor_cmdbuf(s, 2, CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMON, off)) != 0) {
                set_err(err_buf, err_sz, "S5K3P9 STREAMON failed", rc ? rc : -ENOSPC);
                goto fail;
            }
            uint32_t aoff[1] = { 0x0100u }, voff[1] = { 0x00u };
            off = append_i2c_rndm_wr_u32((uint8_t *)s->src_cmd[3].vaddr, 0, 4096, 2, 1, aoff, voff, 1);
            if (off == 0 || (rc = submit_sensor_cmdbuf(s, 3, CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMOFF, off)) != 0) {
                set_err(err_buf, err_sz, "S5K3P9 STREAMOFF failed", rc ? rc : -ENOSPC);
                goto fail;
            }
        } else if (s->src == MARBLE_HW_SRC_OV64B40) {
            if ((rc = alloc_sensor_i2c_pair(s, 1, 32768)) != 0) {
                set_err(err_buf, err_sz, "alloc OV64B40 init buf failed", rc);
                goto fail;
            }
            uint8_t *cb = (uint8_t *)s->src_cmd[1].vaddr;
            uint32_t cap = (uint32_t)s->src_cmd[1].size;
            /* Reg 0 is soft reset {0x0103, 0x01}: write it, wait 5ms, then write rest of init + mode */
            uint32_t off = append_reg16_8_table(cb, 0, cap, k_ov64b40_init_regs, 1);
            if (off > 0)
                off = append_i2c_wait_ms(cb, off, cap, 5);
            if (off > 0)
                off = append_reg16_8_table(cb, off, cap, k_ov64b40_init_regs + 1,
                                           (sizeof(k_ov64b40_init_regs) / sizeof(k_ov64b40_init_regs[0])) - 1);
            bool use_16mp = (s->width == 4624u);
            const struct marble_reg16_8 *omode = use_16mp ? k_ov64b40_mode_4624x3472 : k_ov64b40_mode_1920x1080;
            size_t n_omode = use_16mp
                ? (sizeof(k_ov64b40_mode_4624x3472) / sizeof(k_ov64b40_mode_4624x3472[0]))
                : (sizeof(k_ov64b40_mode_1920x1080) / sizeof(k_ov64b40_mode_1920x1080[0]));
            if (off > 0)
                off = append_reg16_8_table(cb, off, cap, omode, n_omode);
            uint32_t fll = use_16mp ? 0x0E2Cu : 0x2710u; /* 30fps for both modes */
            uint32_t exp = cfg->exposure_lines ? cfg->exposure_lines : (use_16mp ? 0x0DC0u : 0x2580u);
            uint32_t ag  = cfg->analog_gain ? cfg->analog_gain : 0x0600u; /* 6x (0x100=1x) */
            struct marble_reg16_8 ov_exp[] = {
                { 0x380eu, (uint8_t)((fll >> 8) & 0xFFu) },
                { 0x380fu, (uint8_t)(fll & 0xFFu) },
                { 0x3500u, (uint8_t)((exp >> 16) & 0x0Fu) },
                { 0x3501u, (uint8_t)((exp >> 8) & 0xFFu) },
                { 0x3502u, (uint8_t)(exp & 0xFFu) },
                { 0x3508u, (uint8_t)((ag >> 8) & 0xFFu) },
                { 0x3509u, (uint8_t)(ag & 0xFFu) },
            };
            if (off > 0)
                off = append_reg16_8_table(cb, off, cap, ov_exp, sizeof(ov_exp) / sizeof(ov_exp[0]));
            if (off > 0 && cfg->test_pattern > 0) {
                static const struct marble_reg16_8 ov_tp[] = {
                    { 0x3019u, 0xf0u }, { 0x4308u, 0x01u },
                    { 0x4300u, 0x00u }, { 0x4301u, 0x00u }, { 0x4302u, 0x00u }, { 0x4303u, 0x00u },
                    { 0x4304u, 0x00u }, { 0x4305u, 0x00u }, { 0x4306u, 0x00u }, { 0x4307u, 0x00u },
                };
                off = append_reg16_8_table(cb, off, cap, ov_tp, sizeof(ov_tp) / sizeof(ov_tp[0]));
            }
            if (off == 0 || (rc = submit_sensor_cmdbuf(s, 1, CAM_SENSOR_PACKET_OPCODE_SENSOR_INITIAL_CONFIG, off)) != 0) {
                set_err(err_buf, err_sz, "OV64B40 INITIAL_CONFIG failed", rc ? rc : -ENOSPC);
                goto fail;
            }
            const struct imx355_reg_pair reg_on[1] = { { 0x0100, 0x01 } };
            const struct imx355_reg_pair reg_off[1] = { { 0x0100, 0x00 } };
            if ((rc = send_sensor_i2c_packet(s, 2, CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMON, reg_on, 1)) != 0 ||
                (rc = send_sensor_i2c_packet(s, 3, CAM_SENSOR_PACKET_OPCODE_SENSOR_STREAMOFF, reg_off, 1)) != 0) {
                set_err(err_buf, err_sz, "OV64B40 STREAMON/OFF failed", rc);
                goto fail;
            }
        }

        /* 3e. Allocate pre-baked slot-4 I2C command buffer & initialize closed-loop AE limits */
        if ((rc = alloc_sensor_i2c_pair(s, 4, 4096)) != 0) {
            set_err(err_buf, err_sz, "alloc AE update buf failed", rc);
            goto fail;
        }
        s->ae_lim.black_level = 64u;
        s->ae_lim.min_gain_q8 = 256u;
        if (s->src == MARBLE_HW_SRC_OV64B40) {
            bool use_16mp = (s->width == 4624u);
            s->ae_lim.min_exp = 16u;
            s->ae_lim.max_exp = use_16mp ? 0x0E00u : 0x26E0u;
            s->ae_lim.max_gain_q8 = 0x0F80u;
            s->ae_exp = cfg->exposure_lines ? cfg->exposure_lines : (use_16mp ? 0x0DC0u : 0x2580u);
            s->ae_gain_q8 = cfg->analog_gain ? cfg->analog_gain : 0x0600u;
        } else if (s->src == MARBLE_HW_SRC_S5K3P9) {
            s->ae_lim.min_exp = 8u;
            s->ae_lim.max_exp = 0x09F0u;
            s->ae_lim.max_gain_q8 = 4096u;
            s->ae_exp = cfg->exposure_lines ? cfg->exposure_lines : 0x0900u;
            s->ae_gain_q8 = cfg->analog_gain ? (cfg->analog_gain << 3) : 1536u;
        } else if (s->src == MARBLE_HW_SRC_IMX355) {
            s->ae_lim.min_exp = 8u;
            s->ae_lim.max_exp = (s->width == 820u) ? 0x027Cu : 0x0518u;
            s->ae_lim.max_gain_q8 = 4096u;
            s->ae_exp = cfg->exposure_lines ? cfg->exposure_lines : ((s->width == 820u) ? 0x0260u : 0x04E0u);
            s->ae_gain_q8 = 1536u;
        } else if (s->src == MARBLE_HW_SRC_GC02M1) {
            s->ae_lim.min_exp = 4u;
            s->ae_lim.max_exp = 0x04D0u;
            s->ae_lim.max_gain_q8 = 3072u;
            s->ae_exp = cfg->exposure_lines ? cfg->exposure_lines : 0x04D0u;
            s->ae_gain_q8 = 2048u;
        }
        s->ae_enabled = (s->is_optical && !cfg->ae_disable &&
                         cfg->exposure_lines == 0 && cfg->analog_gain == 0) ? 1 : 0;
        s->ae_cooldown = 0;

        /* 3f. For Rear Wide OV64B40 (slot 0), bring up GT9764 VCM AF actuator (non-fatal) */
        if (s->src == MARBLE_HW_SRC_OV64B40) {
            (void)gt9764_actuator_open(s, cfg->focus_dac);
        }
    } else {
        struct cam_tpg_acquire_dev tpg_acq;
        memset(&tpg_acq, 0, sizeof(tpg_acq));
        tpg_acq.session_handle = (uint32_t)s->session_hdl;
        tpg_acq.handle_type = CAM_HANDLE_USER_POINTER;
        if ((rc = cam_ctrl(s->tpg_fd, CAM_ACQUIRE_DEV, &tpg_acq, sizeof(tpg_acq))) != 0) {
            set_err(err_buf, err_sz, "TPG0 CAM_ACQUIRE_DEV failed", rc);
            goto fail;
        }
        s->tpg_hdl = (int32_t)tpg_acq.device_handle;

        if ((rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->src_pkt[0])) != 0 ||
            (rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->src_cmd[0])) != 0) {
            set_err(err_buf, err_sz, "alloc TPG0 bufs failed", rc);
            goto fail;
        }
        struct tpg_global_config_t gcfg;
        memset(&gcfg, 0, sizeof(gcfg));
        gcfg.header.cmd_type = TPG_CMD_TYPE_GLOBAL_CONFIG;
        gcfg.header.size = (ssize_t)sizeof(gcfg);
        gcfg.header.cmd_version = 1;
        gcfg.phy_type = TPG_PHY_TYPE_DPHY;
        gcfg.lane_count = 4;
        gcfg.interleaving_format = TPG_INTERLEAVING_FORMAT_FRAME;
        gcfg.shutter_type = TPG_SHUTTER_TYPE_ROLLING;
        gcfg.hbi = 500;
        gcfg.vbi = 500;
        gcfg.skip_pattern = 0xFFFFFFFFu;
        gcfg.tpg_clock = 400000000ULL;

        struct tpg_stream_config_t scfg;
        memset(&scfg, 0, sizeof(scfg));
        scfg.header.cmd_type = TPG_CMD_TYPE_STREAM_CONFIG;
        scfg.header.size = (ssize_t)sizeof(scfg);
        scfg.header.cmd_version = 1;
        scfg.pattern_type = TPG_PATTERN_COLOR_BAR;
        scfg.cb_mode = TPG_COLOR_BAR_MODE_NORMAL;
        scfg.frame_count = 0;
        scfg.stream_type = TPG_STREAM_TYPE_IMAGE;
        scfg.stream_dimension.width = s->width;
        scfg.stream_dimension.height = s->height;
        scfg.pixel_depth = s->use_raw8 ? PACK_8_BIT : PACK_10_BIT;
        scfg.output_format = TPG_IMAGE_FORMAT_BAYER;
        scfg.hbi = 500;
        scfg.vbi = 500;
        scfg.vc = 0;
        scfg.dt = (uint16_t)mipi_dt;

        uint32_t gcfg_off = 0;
        uint32_t scfg_off = ((uint32_t)sizeof(gcfg) + 15u) & ~15u;
        memcpy((uint8_t *)s->src_cmd[0].vaddr + gcfg_off, &gcfg, sizeof(gcfg));
        memcpy((uint8_t *)s->src_cmd[0].vaddr + scfg_off, &scfg, sizeof(scfg));

        struct cam_packet *tp = (struct cam_packet *)s->src_pkt[0].vaddr;
        tp->header.op_code = CAM_TPG_PACKET_OPCODE_INITIAL_CONFIG;
        tp->header.size = (uint32_t)(offsetof(struct cam_packet, payload) + 2u * sizeof(struct cam_cmd_buf_desc));
        tp->cmd_buf_offset = 0;
        tp->num_cmd_buf = 2;
        struct cam_cmd_buf_desc *tcd = (struct cam_cmd_buf_desc *)tp->payload;
        tcd[0].mem_handle = s->src_cmd[0].handle;
        tcd[0].offset = gcfg_off;
        tcd[0].size = sizeof(gcfg);
        tcd[0].length = sizeof(gcfg);
        tcd[0].type = CAM_CMD_BUF_GENERIC;
        tcd[1].mem_handle = s->src_cmd[0].handle;
        tcd[1].offset = scfg_off;
        tcd[1].size = sizeof(scfg);
        tcd[1].length = sizeof(scfg);
        tcd[1].type = CAM_CMD_BUF_GENERIC;

        struct cam_config_dev_cmd tcfg;
        memset(&tcfg, 0, sizeof(tcfg));
        tcfg.session_handle = s->session_hdl;
        tcfg.dev_handle = s->tpg_hdl;
        tcfg.packet_handle = (uint64_t)(uint32_t)s->src_pkt[0].handle;
        if ((rc = cam_ctrl(s->tpg_fd, CAM_CONFIG_DEV, &tcfg, sizeof(tcfg))) != 0) {
            set_err(err_buf, err_sz, "TPG0 CAM_CONFIG_DEV failed", rc);
            goto fail;
        }
    }

    /* 4. Acquire ISP device + RDI0 HW */
    struct cam_acquire_dev_cmd isp_dev_acq;
    memset(&isp_dev_acq, 0, sizeof(isp_dev_acq));
    isp_dev_acq.session_handle = s->session_hdl;
    isp_dev_acq.handle_type = CAM_HANDLE_USER_POINTER;
    isp_dev_acq.num_resources = CAM_API_COMPAT_CONSTANT;
    if ((rc = cam_ctrl(s->isp_fd, CAM_ACQUIRE_DEV, &isp_dev_acq, sizeof(isp_dev_acq))) != 0) {
        set_err(err_buf, err_sz, "ISP CAM_ACQUIRE_DEV failed", rc);
        goto fail;
    }
    s->isp_hdl = isp_dev_acq.dev_handle;

    uint8_t acq_buf[512];
    memset(acq_buf, 0, sizeof(acq_buf));
    struct cam_isp_acquire_hw_info *acq_info = (struct cam_isp_acquire_hw_info *)acq_buf;
    acq_info->common_info_version = 0x2000u;
    acq_info->num_inputs = 1;
    acq_info->input_info_version = 0x00020000u;
    acq_info->input_info_size = (uint32_t)sizeof(struct cam_isp_in_port_info_v2);

    struct cam_isp_in_port_info_v2 *in_port = (struct cam_isp_in_port_info_v2 *)&acq_info->data;
    in_port->res_type = isp_in_res;
    in_port->lane_type = csiphy_3phase ? CAM_ISP_LANE_TYPE_CPHY : CAM_ISP_LANE_TYPE_DPHY;
    in_port->lane_num = lane_cnt;
    in_port->lane_cfg = lane_assign;
    in_port->vc[0] = 0;
    in_port->dt[0] = mipi_dt;
    in_port->num_valid_vc_dt = 1;
    in_port->format = s->cam_fmt;
    in_port->usage_type = CAM_ISP_RES_USAGE_SINGLE;
    in_port->left_start = 0;
    in_port->left_stop = s->width - 1u;
    in_port->left_width = s->width;
    in_port->line_start = 0;
    in_port->line_stop = s->height - 1u;
    in_port->height = s->height;
    in_port->pixel_clk = csiphy_3phase ? 600000000u : 400000000u;
    in_port->batch_size = 1;
    in_port->dsp_mode = CAM_ISP_DSP_MODE_NONE;
    in_port->hbi_cnt = 500;
    in_port->num_out_res = 1;
    in_port->data[0].res_type = CAM_ISP_IFE_OUT_RES_RDI_0;
    in_port->data[0].format = s->cam_fmt;
    in_port->data[0].width = s->width;
    in_port->data[0].height = s->height;
    in_port->data[0].comp_grp_id = CAM_ISP_RES_COMP_GROUP_NONE;
    in_port->data[0].secure_mode = CAM_SECURE_MODE_NON_SECURE;

    struct cam_acquire_hw_cmd_v1 hw_acq;
    memset(&hw_acq, 0, sizeof(hw_acq));
    hw_acq.struct_version = CAM_ACQUIRE_HW_STRUCT_VERSION_1;
    hw_acq.session_handle = s->session_hdl;
    hw_acq.dev_handle = s->isp_hdl;
    hw_acq.handle_type = CAM_HANDLE_USER_POINTER;
    hw_acq.data_size = (uint32_t)(offsetof(struct cam_isp_acquire_hw_info, data) +
                                  sizeof(struct cam_isp_in_port_info_v2));
    hw_acq.resource_hdl = (uint64_t)(uintptr_t)acq_buf;
    if ((rc = cam_ctrl(s->isp_fd, CAM_ACQUIRE_HW, &hw_acq, sizeof(hw_acq))) != 0) {
        set_err(err_buf, err_sz, "ISP CAM_ACQUIRE_HW failed", rc);
        goto fail;
    }
    s->isp_hw_acq = true;

    /* 5. CRM link (ISP only -> pd=1 immediate apply) + activate */
    struct cam_req_mgr_link_info link;
    memset(&link, 0, sizeof(link));
    link.session_hdl = s->session_hdl;
    link.num_devices = 1;
    link.dev_hdls[0] = s->isp_hdl;
    if ((rc = cam_ctrl(s->crm_fd, CAM_REQ_MGR_LINK, &link, sizeof(link))) != 0) {
        set_err(err_buf, err_sz, "CRM LINK failed", rc);
        goto fail;
    }
    s->link_hdl = link.link_hdl;

    struct cam_req_mgr_link_control lctrl;
    memset(&lctrl, 0, sizeof(lctrl));
    lctrl.ops = CAM_REQ_MGR_LINK_ACTIVATE;
    lctrl.session_hdl = s->session_hdl;
    lctrl.num_links = 1;
    lctrl.link_hdls[0] = s->link_hdl;
    (void)cam_ctrl(s->crm_fd, CAM_REQ_MGR_LINK_CONTROL, &lctrl, sizeof(lctrl));

    /* 6. ISP INIT packet (clock + BW + KMD cmd buf) */
    uint32_t cdm_flags = CAM_MEM_FLAG_HW_READ_WRITE | CAM_MEM_FLAG_KMD_ACCESS |
                         CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_CMD_BUF_TYPE;
    if ((rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->isp_init_pkt)) != 0 ||
        (rc = cam_alloc_buf(s->crm_fd, 8192, cdm_flags, s->cdm_iommu, &s->isp_init_kmd)) != 0 ||
        (rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->isp_init_blob)) != 0) {
        set_err(err_buf, err_sz, "alloc ISP INIT bufs failed", rc);
        goto fail;
    }

    struct cam_isp_clock_config clk_cfg;
    memset(&clk_cfg, 0, sizeof(clk_cfg));
    clk_cfg.usage_type = CAM_ISP_RES_USAGE_SINGLE;
    clk_cfg.num_rdi = 1;
    clk_cfg.left_pix_hz = csiphy_3phase ? 600000000ULL : 400000000ULL;
    clk_cfg.rdi_hz[0]   = csiphy_3phase ? 600000000ULL : 400000000ULL;

    struct cam_isp_bw_config_v2 bw_cfg;
    memset(&bw_cfg, 0, sizeof(bw_cfg));
    bw_cfg.usage_type = CAM_ISP_RES_USAGE_SINGLE;
    bw_cfg.num_paths = 1;
    bw_cfg.axi_path[0].usage_data = CAM_ISP_USAGE_RDI;
    bw_cfg.axi_path[0].transac_type = CAM_AXI_TRANSACTION_WRITE;
    bw_cfg.axi_path[0].path_data_type = CAM_AXI_PATH_DATA_IFE_RDI0;
    bw_cfg.axi_path[0].camnoc_bw  = csiphy_3phase ?  800000000ULL : 400000000ULL;
    bw_cfg.axi_path[0].mnoc_ab_bw = csiphy_3phase ?  800000000ULL : 400000000ULL;
    bw_cfg.axi_path[0].mnoc_ib_bw = csiphy_3phase ? 1200000000ULL : 600000000ULL;
    bw_cfg.axi_path[0].ddr_ab_bw  = csiphy_3phase ?  800000000ULL : 400000000ULL;
    bw_cfg.axi_path[0].ddr_ib_bw  = csiphy_3phase ? 1200000000ULL : 600000000ULL;

    uint32_t blob_len = 0;
    blob_len = append_generic_blob((uint8_t *)s->isp_init_blob.vaddr, blob_len,
                                   CAM_ISP_GENERIC_BLOB_TYPE_CLOCK_CONFIG, &clk_cfg, sizeof(clk_cfg));
    blob_len = append_generic_blob((uint8_t *)s->isp_init_blob.vaddr, blob_len,
                                   CAM_ISP_GENERIC_BLOB_TYPE_BW_CONFIG_V2, &bw_cfg, sizeof(bw_cfg));

    struct cam_packet *ip = (struct cam_packet *)s->isp_init_pkt.vaddr;
    ip->header.op_code = CAM_ISP_PACKET_INIT_DEV - 1u;
    ip->header.size = (uint32_t)(offsetof(struct cam_packet, payload) + 2u * sizeof(struct cam_cmd_buf_desc));
    ip->cmd_buf_offset = 0;
    ip->num_cmd_buf = 2;
    ip->kmd_cmd_buf_index = 0;
    struct cam_cmd_buf_desc *icd = (struct cam_cmd_buf_desc *)ip->payload;
    icd[0].mem_handle = s->isp_init_kmd.handle;
    icd[0].size = (uint32_t)s->isp_init_kmd.size;
    icd[0].length = 0;
    icd[0].type = CAM_CMD_BUF_DIRECT;
    icd[0].meta_data = CAM_ISP_PACKET_META_LEFT;
    icd[1].mem_handle = s->isp_init_blob.handle;
    icd[1].size = (uint32_t)s->isp_init_blob.size;
    icd[1].length = blob_len;
    icd[1].type = CAM_CMD_BUF_GENERIC;
    icd[1].meta_data = CAM_ISP_PACKET_META_GENERIC_BLOB_COMMON;

    struct cam_config_dev_cmd icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.session_handle = s->session_hdl;
    icfg.dev_handle = s->isp_hdl;
    icfg.packet_handle = (uint64_t)(uint32_t)s->isp_init_pkt.handle;
    if ((rc = cam_ctrl(s->isp_fd, CAM_CONFIG_DEV, &icfg, sizeof(icfg))) != 0) {
        set_err(err_buf, err_sz, "ISP INIT CAM_CONFIG_DEV failed", rc);
        goto fail;
    }

    /* 7. Allocate 3-slot DMA-BUF ring & prime all slots for continuous streaming */
    uint32_t img_flags = CAM_MEM_FLAG_HW_READ_WRITE | CAM_MEM_FLAG_UMD_ACCESS |
                         CAM_MEM_FLAG_PIXEL_BUF_TYPE | CAM_MEM_FLAG_CACHE;
    for (int i = 0; i < MARBLE_RING_SLOTS; ++i) {
        if ((rc = cam_alloc_buf(s->crm_fd, 4096, cpu_flags, 0, &s->upd_pkt[i])) != 0 ||
            (rc = cam_alloc_buf(s->crm_fd, 8192, cdm_flags, s->cdm_iommu, &s->upd_kmd[i])) != 0 ||
            (rc = cam_alloc_buf(s->crm_fd, s->raw_bytes, img_flags, s->img_iommu, &s->img_buf[i])) != 0) {
            set_err(err_buf, err_sz, "alloc ring DMA-BUF failed", rc);
            goto fail;
        }
    }
    for (int i = 0; i < MARBLE_RING_SLOTS; ++i) {
        if ((rc = queue_isp_slot(s, i)) != 0) {
            set_err(err_buf, err_sz, "prime ISP UPDATE request failed", rc);
            goto fail;
        }
    }

    /* 8. Start ISP -> CSIPHY -> sensor (or ISP -> TPG0) */
    struct cam_start_stop_dev_cmd ss;
    memset(&ss, 0, sizeof(ss));
    ss.session_handle = s->session_hdl;
    ss.dev_handle = s->isp_hdl;
    if ((rc = cam_ctrl(s->isp_fd, CAM_START_DEV, &ss, sizeof(ss))) != 0) {
        set_err(err_buf, err_sz, "ISP CAM_START_DEV failed", rc);
        goto fail;
    }
    s->isp_started = true;

    if (s->is_sensor) {
        ss.dev_handle = s->csiphy_hdl;
        if ((rc = cam_ctrl(s->csiphy_fd, CAM_START_DEV, &ss, sizeof(ss))) != 0) {
            set_err(err_buf, err_sz, "CSIPHY CAM_START_DEV failed", rc);
            goto fail;
        }
        s->csiphy_started = true;

        ss.dev_handle = s->sensor_hdl;
        if ((rc = cam_ctrl(s->sensor_fd, CAM_START_DEV, &ss, sizeof(ss))) != 0) {
            set_err(err_buf, err_sz, "sensor CAM_START_DEV failed", rc);
            goto fail;
        }
        s->sensor_started = true;
    } else {
        ss.dev_handle = s->tpg_hdl;
        if ((rc = cam_ctrl(s->tpg_fd, CAM_START_DEV, &ss, sizeof(ss))) != 0) {
            set_err(err_buf, err_sz, "TPG0 CAM_START_DEV failed", rc);
            goto fail;
        }
        s->tpg_started = true;
    }

    *out_sess = s;
    return 0;

fail:
    marble_hw_session_close(s);
    return rc ? rc : -EIO;
}

void marble_hw_session_get_geometry(const marble_hw_session_t *sess,
                                    uint32_t *width, uint32_t *height,
                                    uint32_t *stride, uint32_t *raw_bytes) {
    if (!sess)
        return;
    if (width) *width = sess->width;
    if (height) *height = sess->height;
    if (stride) *stride = sess->stride;
    if (raw_bytes) *raw_bytes = sess->raw_bytes;
}

int marble_hw_session_grab_raw(marble_hw_session_t *s,
                               uint8_t *out_raw, size_t out_raw_cap,
                               uint64_t timeout_ms,
                               marble_raw_frame_info_t *out_info) {
    if (!s || s->ring_queued <= 0)
        return -EINVAL;

    int slot = s->ring_head;
    int wrc = sync_wait(s->sync_fd, s->fences[slot], timeout_ms ? timeout_ms : 2500u);
    (void)cam_cache_op(s->crm_fd, s->img_buf[slot].handle, CAM_MEM_INV_CACHE);

    const uint8_t *p = (const uint8_t *)s->img_buf[slot].vaddr;
    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->width = s->width;
        out_info->height = s->height;
        out_info->stride = s->stride;
        out_info->raw_bytes = s->raw_bytes;
        out_info->is_raw8 = (uint32_t)s->use_raw8;
        out_info->req_id = s->slot_req_id[slot];
        if (p) {
            uint8_t seen[256] = {0};
            uint32_t csum = 0, non_sent = 0, non_zero = 0;
            for (uint32_t i = 0; i < s->raw_bytes; ++i) {
                uint8_t v = p[i];
                if (v != 0xA5u) non_sent++;
                if (v != 0x00u) non_zero++;
                csum = (csum * 131u) ^ v;
                seen[v] = 1;
            }
            uint32_t uniq = 0;
            for (int b = 0; b < 256; ++b)
                uniq += seen[b];
            out_info->csum = csum;
            out_info->non_sentinel = non_sent;
            out_info->non_zero = non_zero;
            out_info->uniq_bytes = uniq;

            /* Sample 10-bit pixel min/max/mean, highlight clip permille & center-ROI sharpness */
            uint16_t pmin = 1023u, pmax = 0u;
            uint64_t psum = 0, pcnt = 0, clip_cnt = 0;
            uint64_t sharp_sum = 0, sharp_cnt = 0;
            uint16_t *row_u16 = (uint16_t *)malloc(s->width * sizeof(uint16_t));
            if (row_u16) {
                uint32_t ystep = (s->height >= 32) ? (s->height / 32u) : 1u;
                uint32_t x0 = s->width / 4u, x1 = (s->width * 3u) / 4u;
                uint32_t y0 = s->height / 4u, y1 = (s->height * 3u) / 4u;
                for (uint32_t y = 0; y < s->height; y += ystep) {
                    const uint8_t *rp = p + (size_t)y * s->stride;
                    if (s->use_raw8) {
                        for (uint32_t x = 0; x < s->width; ++x)
                            row_u16[x] = (uint16_t)(rp[x] << 2);
                    } else {
                        marble_unpack_mipi_raw10_row(rp, row_u16, s->width);
                    }
                    for (uint32_t x = 0; x < s->width; ++x) {
                        uint16_t v = row_u16[x];
                        if (v < pmin) pmin = v;
                        if (v > pmax) pmax = v;
                        if (v >= 980u) clip_cnt++;
                        psum += v;
                        pcnt++;
                    }
                    if (y >= y0 && y <= y1 && x1 > x0 + 4u) {
                        /* Same-color horizontal Bayer gradient |P[x+2] - P[x]| in center ROI */
                        for (uint32_t x = x0; x + 2u < x1; x += 2u) {
                            int d = (int)row_u16[x + 2u] - (int)row_u16[x];
                            if (d < 0) d = -d;
                            sharp_sum += (uint64_t)(d * d);
                            sharp_cnt++;
                        }
                    }
                }
                free(row_u16);
            }
            uint32_t mean10 = pcnt ? (uint32_t)(psum / pcnt) : 0u;
            uint32_t clip_pm = pcnt ? (uint32_t)((clip_cnt * 1000u) / pcnt) : 0u;
            out_info->raw10_min = pcnt ? pmin : 0u;
            out_info->raw10_max = pcnt ? pmax : 0u;
            out_info->raw10_mean = mean10;
            out_info->clip_permille = clip_pm;
            out_info->sharpness = sharp_cnt ? (uint32_t)(sharp_sum / sharp_cnt) : 0u;

            /* Real-time closed-loop Auto-Exposure (AE) step over live CCI I2C */
            if (wrc == 0 && s->ae_enabled && pcnt > 0) {
                if (s->ae_cooldown > 0) {
                    s->ae_cooldown--;
                } else {
                    uint32_t next_exp = s->ae_exp;
                    uint32_t next_gain = s->ae_gain_q8;
                    if (marble_ae_compute_next(&s->ae_lim, s->ae_exp, s->ae_gain_q8,
                                               mean10, clip_pm, &next_exp, &next_gain)) {
                        if (marble_hw_session_set_exposure(s, next_exp, next_gain) == 0)
                            s->ae_cooldown = 1; /* skip 1 frame for sensor pipeline latency */
                    }
                }
            }
            out_info->ae_exp_lines = s->ae_exp;
            out_info->ae_gain_q8 = s->ae_gain_q8;
            out_info->ae_updates = s->ae_updates;
            out_info->focus_dac = s->focus_dac;
            out_info->actuator_ok = s->act_configured ? 1u : 0u;
        }
    }

    if (wrc == 0 && out_raw && p) {
        if (out_raw_cap >= s->raw_bytes)
            memcpy(out_raw, p, s->raw_bytes);
    }

    s->ring_head = (s->ring_head + 1) % MARBLE_RING_SLOTS;
    s->ring_queued--;

    /* Keep pipeline continuously fed by re-queuing this slot! */
    if (wrc == 0)
        (void)queue_isp_slot(s, slot);

    return wrc;
}

int marble_hw_session_grab_rgb24(marble_hw_session_t *s,
                                 uint8_t *dst_rgb, uint32_t dst_w, uint32_t dst_h,
                                 uint64_t timeout_ms,
                                 marble_raw_frame_info_t *out_info) {
    if (!s || !dst_rgb || dst_w == 0 || dst_h == 0)
        return -EINVAL;
    if (!s->scratch_raw) {
        s->scratch_raw = (uint8_t *)malloc(s->raw_bytes);
        if (!s->scratch_raw)
            return -ENOMEM;
    }
    int rc = marble_hw_session_grab_raw(s, s->scratch_raw, s->raw_bytes, timeout_ms, out_info);
    if (rc != 0)
        return rc;
    return marble_bayer_to_rgb24_resize_ex(s->scratch_raw, s->width, s->height,
                                           s->stride, s->use_raw8, s->is_optical,
                                           s->bayer_order, dst_rgb, dst_w, dst_h);
}

void marble_hw_session_close(marble_hw_session_t *s) {
    if (!s)
        return;
    if (s->sensor_started) {
        struct cam_start_stop_dev_cmd cmd = { s->session_hdl, s->sensor_hdl };
        (void)cam_ctrl(s->sensor_fd, CAM_STOP_DEV, &cmd, sizeof(cmd));
    }
    if (s->csiphy_started) {
        struct cam_start_stop_dev_cmd cmd = { s->session_hdl, s->csiphy_hdl };
        (void)cam_ctrl(s->csiphy_fd, CAM_STOP_DEV, &cmd, sizeof(cmd));
    }
    if (s->tpg_started) {
        struct cam_start_stop_dev_cmd cmd = { s->session_hdl, s->tpg_hdl };
        (void)cam_ctrl(s->tpg_fd, CAM_STOP_DEV, &cmd, sizeof(cmd));
    }
    if (s->isp_started) {
        struct cam_start_stop_dev_cmd cmd = { s->session_hdl, s->isp_hdl };
        (void)cam_ctrl(s->isp_fd, CAM_STOP_DEV, &cmd, sizeof(cmd));
    }
    if (s->link_hdl > 0 && s->crm_fd >= 0) {
        struct cam_req_mgr_unlink_info u = { s->session_hdl, s->link_hdl };
        (void)cam_ctrl(s->crm_fd, CAM_REQ_MGR_UNLINK, &u, sizeof(u));
    }
    for (int i = 0; i < MARBLE_RING_SLOTS; ++i) {
        sync_destroy(s->sync_fd, s->fences[i]);
        cam_free_buf(s->crm_fd, &s->img_buf[i]);
        cam_free_buf(s->crm_fd, &s->upd_kmd[i]);
        cam_free_buf(s->crm_fd, &s->upd_pkt[i]);
    }
    cam_free_buf(s->crm_fd, &s->isp_init_blob);
    cam_free_buf(s->crm_fd, &s->isp_init_kmd);
    cam_free_buf(s->crm_fd, &s->isp_init_pkt);
    if (s->act_configured) {
        /* Smoothly park GT9764 lens to stock sleep DAC 512 before VAF power-down */
        (void)gt9764_write_dac_locked(s, 512u, 5);
    }
    for (int i = 0; i < 5; ++i) {
        cam_free_buf(s->crm_fd, &s->src_cmd[i]);
        cam_free_buf(s->crm_fd, &s->src_pkt[i]);
    }
    for (int i = 0; i < 3; ++i)
        cam_free_buf(s->crm_fd, &s->act_cmd[i]);
    cam_free_buf(s->crm_fd, &s->act_pkt);
    if (s->act_hdl > 0) {
        struct cam_release_dev_cmd r = { s->session_hdl, s->act_hdl };
        (void)cam_ctrl(s->act_fd, CAM_RELEASE_DEV, &r, sizeof(r));
    }
    if (s->isp_hw_acq) {
        struct cam_release_dev_cmd r = { s->session_hdl, s->isp_hdl };
        (void)cam_ctrl(s->isp_fd, CAM_RELEASE_HW, &r, sizeof(r));
    }
    if (s->isp_hdl > 0) {
        struct cam_release_dev_cmd r = { s->session_hdl, s->isp_hdl };
        (void)cam_ctrl(s->isp_fd, CAM_RELEASE_DEV, &r, sizeof(r));
    }
    if (s->csiphy_hdl > 0) {
        struct cam_release_dev_cmd r = { s->session_hdl, s->csiphy_hdl };
        (void)cam_ctrl(s->csiphy_fd, CAM_RELEASE_DEV, &r, sizeof(r));
    }
    if (s->sensor_hdl > 0) {
        struct cam_release_dev_cmd r = { s->session_hdl, s->sensor_hdl };
        (void)cam_ctrl(s->sensor_fd, CAM_RELEASE_DEV, &r, sizeof(r));
    }
    if (s->tpg_hdl > 0) {
        struct cam_release_dev_cmd r = { s->session_hdl, s->tpg_hdl };
        (void)cam_ctrl(s->tpg_fd, CAM_RELEASE_DEV, &r, sizeof(r));
    }
    if (s->session_hdl > 0 && s->crm_fd >= 0) {
        struct cam_req_mgr_session_info si = { s->session_hdl, 0 };
        (void)cam_ctrl(s->crm_fd, CAM_REQ_MGR_DESTROY_SESSION, &si, sizeof(si));
    }
    if (s->act_fd >= 0) close(s->act_fd);
    if (s->sensor_fd >= 0) close(s->sensor_fd);
    if (s->csiphy_fd >= 0) close(s->csiphy_fd);
    if (s->tpg_fd >= 0) close(s->tpg_fd);
    if (s->isp_fd >= 0) close(s->isp_fd);
    if (s->sync_fd >= 0) close(s->sync_fd);
    if (s->crm_fd >= 0) close(s->crm_fd);
    free(s->scratch_raw);
    free(s);
}

/* ============================================================================
 * Part 2.5: Pure Host-Testable Closed-Loop Auto-Exposure (AE) Controller
 * ============================================================================ */

int marble_ae_compute_next(const marble_ae_limits_t *lim,
                           uint32_t cur_exp, uint32_t cur_gain_q8,
                           uint32_t raw10_mean, uint32_t clip_permille,
                           uint32_t *out_exp, uint32_t *out_gain_q8) {
    if (!lim || !out_exp || !out_gain_q8 || lim->max_exp < lim->min_exp ||
        lim->max_gain_q8 < lim->min_gain_q8 || lim->min_gain_q8 == 0)
        return 0;

    if (cur_exp < lim->min_exp) cur_exp = lim->min_exp;
    if (cur_exp > lim->max_exp) cur_exp = lim->max_exp;
    if (cur_gain_q8 < lim->min_gain_q8) cur_gain_q8 = lim->min_gain_q8;
    if (cur_gain_q8 > lim->max_gain_q8) cur_gain_q8 = lim->max_gain_q8;

    *out_exp = cur_exp;
    *out_gain_q8 = cur_gain_q8;

    const uint32_t target_net = 220u; /* ~22% linear mid-grey above black level */
    const uint32_t dead_lo = 175u;
    const uint32_t dead_hi = 270u;

    uint32_t net = (raw10_mean > lim->black_level) ? (raw10_mean - lim->black_level) : 0u;

    /* Compute desired EV ratio in Q8 (256 = 1.0x) */
    uint32_t ratio_q8 = 256u;
    if (clip_permille >= 150u) {
        ratio_q8 = 115u; /* ~0.45x: severe highlight clipping */
    } else if (clip_permille >= 50u && net > 140u) {
        ratio_q8 = 166u; /* ~0.65x: moderate highlight clipping */
    } else if (net >= dead_lo && net <= dead_hi) {
        return 0; /* settled inside target deadband */
    } else {
        uint32_t meas = (net < 16u) ? 16u : net;
        uint32_t raw_ratio = (target_net * 256u) / meas;
        if (raw_ratio < 90u)  raw_ratio = 90u;   /* min 0.35x per step */
        if (raw_ratio > 716u) raw_ratio = 716u;  /* max 2.80x per step */
        /* Damped step: 0.65 * raw_ratio + 0.35 * 1.0 */
        ratio_q8 = (raw_ratio * 65u + 256u * 35u) / 100u;
    }

    uint64_t cur_ev = (uint64_t)cur_exp * (uint64_t)cur_gain_q8;
    uint64_t min_ev = (uint64_t)lim->min_exp * (uint64_t)lim->min_gain_q8;
    uint64_t max_ev = (uint64_t)lim->max_exp * (uint64_t)lim->max_gain_q8;
    uint64_t new_ev = (cur_ev * (uint64_t)ratio_q8 + 128u) >> 8;
    if (new_ev < min_ev) new_ev = min_ev;
    if (new_ev > max_ev) new_ev = max_ev;

    /* Split EV using exposure-first priority (lowest noise in daylight) */
    uint64_t exp_thresh = (uint64_t)lim->max_exp * (uint64_t)lim->min_gain_q8;
    uint32_t next_exp, next_gain;
    if (new_ev <= exp_thresh) {
        next_gain = lim->min_gain_q8;
        next_exp = (uint32_t)(new_ev / (uint64_t)next_gain);
    } else {
        next_exp = lim->max_exp;
        next_gain = (uint32_t)(new_ev / (uint64_t)next_exp);
    }
    if (next_exp < lim->min_exp) next_exp = lim->min_exp;
    if (next_exp > lim->max_exp) next_exp = lim->max_exp;
    if (next_gain < lim->min_gain_q8) next_gain = lim->min_gain_q8;
    if (next_gain > lim->max_gain_q8) next_gain = lim->max_gain_q8;

    /* Require >= 3% change to avoid unnecessary I2C churn */
    uint32_t de = (next_exp > cur_exp) ? (next_exp - cur_exp) : (cur_exp - next_exp);
    uint32_t dg = (next_gain > cur_gain_q8) ? (next_gain - cur_gain_q8) : (cur_gain_q8 - next_gain);
    if ((uint64_t)de * 33u < cur_exp && (uint64_t)dg * 33u < cur_gain_q8)
        return 0;

    *out_exp = next_exp;
    *out_gain_q8 = next_gain;
    return 1;
}

/* ============================================================================
 * Part 3: Pure DSP — MIPI RAW10 Unpack, RGGB Bayer Demosaic, AWB/Gamma, NV21
 * ============================================================================ */

void marble_unpack_mipi_raw10_row(const uint8_t *src, uint16_t *dst, uint32_t width) {
    uint32_t x = 0;
    const uint8_t *p = src;
    for (; x + 3 < width; x += 4, p += 5) {
        uint8_t lsbs = p[4];
        dst[x + 0] = (uint16_t)(((uint16_t)p[0] << 2) | ((lsbs >> 0) & 0x3u));
        dst[x + 1] = (uint16_t)(((uint16_t)p[1] << 2) | ((lsbs >> 2) & 0x3u));
        dst[x + 2] = (uint16_t)(((uint16_t)p[2] << 2) | ((lsbs >> 4) & 0x3u));
        dst[x + 3] = (uint16_t)(((uint16_t)p[3] << 2) | ((lsbs >> 6) & 0x3u));
    }
    for (; x < width; ++x)
        dst[x] = (uint16_t)((uint16_t)src[(x / 4) * 5 + (x % 4)] << 2);
}

static inline uint8_t clamp_u8_i(int v) {
    return (v < 0) ? 0u : (v > 255 ? 255u : (uint8_t)v);
}

/* sRGB-ish 10-bit -> 8-bit gamma LUT (gamma ~ 1/2.2, precomputed without libm) */
static uint8_t srgb_gamma_10to8(int v10) {
    if (v10 <= 0) return 0;
    if (v10 >= 1023) return 255;
    /* Fast piecewise quadratic/sqrt approximation of 255 * (v/1023)^(1/2.2) */
    uint32_t x = (uint32_t)v10 * 64u; /* 0..65472 */
    /* Integer Newton sqrt(x) -> 0..255 */
    uint32_t r = (x > 255) ? (x >> 4) : 16u;
    if (r == 0) r = 1;
    r = (r + x / r) >> 1;
    r = (r + x / r) >> 1;
    r = (r + x / r) >> 1;
    r = (r + x / r) >> 1;
    return (r > 255u) ? 255u : (uint8_t)r;
}

static inline void extract_bayer_quad(marble_bayer_order_t order,
                                      int p00, int p01, int p10, int p11,
                                      int *r, int *g, int *b) {
    switch (order) {
        case MARBLE_BAYER_GRBG:
            *g = (p00 + p11) >> 1; *r = p01; *b = p10; break;
        case MARBLE_BAYER_BGGR:
            *b = p00; *g = (p01 + p10) >> 1; *r = p11; break;
        case MARBLE_BAYER_GBRG:
            *g = (p00 + p11) >> 1; *b = p01; *r = p10; break;
        case MARBLE_BAYER_RGGB:
        default:
            *r = p00; *g = (p01 + p10) >> 1; *b = p11; break;
    }
}

int marble_bayer_to_rgb24_resize_ex(const uint8_t *raw, uint32_t src_w, uint32_t src_h,
                                    uint32_t src_stride, int is_raw8, int is_optical,
                                    marble_bayer_order_t order,
                                    uint8_t *dst_rgb, uint32_t dst_w, uint32_t dst_h) {
    if (!raw || !dst_rgb || src_w < 4 || src_h < 4 || dst_w == 0 || dst_h == 0)
        return -EINVAL;

    uint16_t *bayer = (uint16_t *)malloc((size_t)src_w * src_h * sizeof(uint16_t));
    if (!bayer)
        return -ENOMEM;

    for (uint32_t y = 0; y < src_h; ++y) {
        const uint8_t *rp = raw + (size_t)y * src_stride;
        uint16_t *dp = bayer + (size_t)y * src_w;
        if (is_raw8) {
            for (uint32_t x = 0; x < src_w; ++x)
                dp[x] = (uint16_t)(rp[x] << 2);
        } else {
            marble_unpack_mipi_raw10_row(rp, dp, src_w);
        }
    }

    int black_level = is_optical ? 64 : 0;
    uint32_t gain_r = 256u, gain_g = 256u, gain_b = 256u;
    if (is_optical) {
        uint64_t sum_r = 0, sum_g = 0, sum_b = 0, cnt = 0;
        uint32_t hist[64] = {0};
        for (uint32_t y = 0; y + 1 < src_h; y += 4) {
            for (uint32_t x = 0; x + 1 < src_w; x += 4) {
                int r, g, b;
                extract_bayer_quad(order,
                                   (int)bayer[y * src_w + x] - black_level,
                                   (int)bayer[y * src_w + x + 1] - black_level,
                                   (int)bayer[(y + 1) * src_w + x] - black_level,
                                   (int)bayer[(y + 1) * src_w + x + 1] - black_level,
                                   &r, &g, &b);
                if (r < 0) r = 0;
                if (g < 0) g = 0;
                if (b < 0) b = 0;
                sum_r += (uint64_t)r;
                sum_g += (uint64_t)g;
                sum_b += (uint64_t)b;
                int mx = r;
                if (g > mx) mx = g;
                if (b > mx) mx = b;
                uint32_t bin = (uint32_t)(mx >> 4);
                if (bin > 63u) bin = 63u;
                hist[bin]++;
                cnt++;
            }
        }
        uint32_t p98_val = 256u;
        if (cnt > 0) {
            uint64_t target = (cnt * 98u) / 100u, acc = 0;
            for (uint32_t k = 0; k < 64u; ++k) {
                acc += hist[k];
                if (acc >= target) {
                    p98_val = (k + 1u) * 16u;
                    break;
                }
            }
            if (p98_val < 32u) p98_val = 32u;
            uint32_t base_gain = (960u * 256u) / p98_val;
            if (base_gain > 2048u) base_gain = 2048u;
            if (base_gain < 256u) base_gain = 256u;

            uint64_t avg_r = (sum_r / cnt) + 1u;
            uint64_t avg_g = (sum_g / cnt) + 1u;
            uint64_t avg_b = (sum_b / cnt) + 1u;
            uint64_t gr = (base_gain * avg_g) / avg_r;
            uint64_t gb = (base_gain * avg_g) / avg_b;
            if (gr > base_gain * 22u / 10u) gr = base_gain * 22u / 10u;
            if (gr < base_gain * 6u / 10u)  gr = base_gain * 6u / 10u;
            if (gb > base_gain * 22u / 10u) gb = base_gain * 22u / 10u;
            if (gb < base_gain * 6u / 10u)  gb = base_gain * 6u / 10u;
            gain_r = (uint32_t)gr;
            gain_g = base_gain;
            gain_b = (uint32_t)gb;
        }
    }

    uint32_t q_w = src_w / 2u;
    uint32_t q_h = src_h / 2u;
    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        uint32_t qy = (uint32_t)(((uint64_t)dy * q_h) / dst_h);
        if (qy >= q_h) qy = q_h - 1u;
        uint32_t sy = qy * 2u;
        const uint16_t *r0 = bayer + (size_t)sy * src_w;
        const uint16_t *r1 = bayer + (size_t)(sy + 1u) * src_w;
        uint8_t *out_row = dst_rgb + (size_t)dy * dst_w * 3u;
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            uint32_t qx = (uint32_t)(((uint64_t)dx * q_w) / dst_w);
            if (qx >= q_w) qx = q_w - 1u;
            uint32_t sx = qx * 2u;

            int r, g, b;
            extract_bayer_quad(order,
                               (int)r0[sx] - black_level,
                               (int)r0[sx + 1u] - black_level,
                               (int)r1[sx] - black_level,
                               (int)r1[sx + 1u] - black_level,
                               &r, &g, &b);
            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;

            if (is_optical) {
                r = (int)(((uint32_t)r * gain_r) >> 8);
                g = (int)(((uint32_t)g * gain_g) >> 8);
                b = (int)(((uint32_t)b * gain_b) >> 8);
                out_row[dx * 3u + 0u] = srgb_gamma_10to8(r);
                out_row[dx * 3u + 1u] = srgb_gamma_10to8(g);
                out_row[dx * 3u + 2u] = srgb_gamma_10to8(b);
            } else {
                out_row[dx * 3u + 0u] = clamp_u8_i(r >> 2);
                out_row[dx * 3u + 1u] = clamp_u8_i(g >> 2);
                out_row[dx * 3u + 2u] = clamp_u8_i(b >> 2);
            }
        }
    }

    free(bayer);
    return 0;
}

int marble_bayer_to_rgb24_resize(const uint8_t *raw, uint32_t src_w, uint32_t src_h,
                                 uint32_t src_stride, int is_raw8, int is_optical,
                                 uint8_t *dst_rgb, uint32_t dst_w, uint32_t dst_h) {
    return marble_bayer_to_rgb24_resize_ex(raw, src_w, src_h, src_stride, is_raw8, is_optical,
                                           MARBLE_BAYER_RGGB, dst_rgb, dst_w, dst_h);
}

void marble_rgb24_to_nv21(const uint8_t *rgb, uint32_t w, uint32_t h,
                          int nv12, uint8_t *dst_yuv, uint32_t dst_stride) {
    if (dst_stride < w) dst_stride = w;
    uint8_t *y_plane = dst_yuv;
    uint8_t *uv_plane = dst_yuv + (size_t)dst_stride * h;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *sr = rgb + (size_t)y * w * 3u;
        uint8_t *yd = y_plane + (size_t)y * dst_stride;
        for (uint32_t x = 0; x < w; ++x) {
            int r = sr[x * 3u + 0u];
            int g = sr[x * 3u + 1u];
            int b = sr[x * 3u + 2u];
            int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yd[x] = clamp_u8_i(Y);
        }
    }
    for (uint32_t y = 0; y + 1 < h; y += 2) {
        const uint8_t *s0 = rgb + (size_t)y * w * 3u;
        const uint8_t *s1 = rgb + (size_t)(y + 1u) * w * 3u;
        uint8_t *uv = uv_plane + (size_t)(y / 2u) * dst_stride;
        for (uint32_t x = 0; x + 1 < w; x += 2) {
            int r = (s0[x*3+0] + s0[(x+1)*3+0] + s1[x*3+0] + s1[(x+1)*3+0] + 2) >> 2;
            int g = (s0[x*3+1] + s0[(x+1)*3+1] + s1[x*3+1] + s1[(x+1)*3+1] + 2) >> 2;
            int b = (s0[x*3+2] + s0[(x+1)*3+2] + s1[x*3+2] + s1[(x+1)*3+2] + 2) >> 2;
            int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            if (nv12) {
                uv[x + 0] = clamp_u8_i(U);
                uv[x + 1] = clamp_u8_i(V);
            } else {
                uv[x + 0] = clamp_u8_i(V);
                uv[x + 1] = clamp_u8_i(U);
            }
        }
    }
}

void marble_rgb24_to_rgba(const uint8_t *rgb, uint32_t w, uint32_t h,
                          uint8_t *dst_rgba, uint32_t dst_stride_bytes) {
    if (dst_stride_bytes < w * 4u) dst_stride_bytes = w * 4u;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *sr = rgb + (size_t)y * w * 3u;
        uint8_t *dr = dst_rgba + (size_t)y * dst_stride_bytes;
        for (uint32_t x = 0; x < w; ++x) {
            dr[x * 4u + 0u] = sr[x * 3u + 0u];
            dr[x * 4u + 1u] = sr[x * 3u + 1u];
            dr[x * 4u + 2u] = sr[x * 3u + 2u];
            dr[x * 4u + 3u] = 0xFFu;
        }
    }
}

/* ============================================================================
 * Part 4: Full 2D 8x8 Baseline DCT + Annex K DC+AC Huffman JFIF JPEG Encoder
 * ============================================================================ */

/* Standard ITU-T T.81 Annex K quantization tables */
static const uint8_t k_qtable_luma[64] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99
};

static const uint8_t k_qtable_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

static const uint8_t k_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Exact orthonormal 8-point DCT-II basis matrix C[u][x] = alpha(u)*cos((2x+1)*u*pi/16),
 * where alpha(0) = 1/sqrt(8) = 0.35355339f and alpha(u>0) = 0.5f. No libm needed! */
static const float k_dct_basis[8][8] = {
    {  0.35355339f,  0.35355339f,  0.35355339f,  0.35355339f,  0.35355339f,  0.35355339f,  0.35355339f,  0.35355339f },
    {  0.49039264f,  0.41573481f,  0.27778512f,  0.09754516f, -0.09754516f, -0.27778512f, -0.41573481f, -0.49039264f },
    {  0.46193977f,  0.19134172f, -0.19134172f, -0.46193977f, -0.46193977f, -0.19134172f,  0.19134172f,  0.46193977f },
    {  0.41573481f, -0.09754516f, -0.49039264f, -0.27778512f,  0.27778512f,  0.49039264f,  0.09754516f, -0.41573481f },
    {  0.35355339f, -0.35355339f, -0.35355339f,  0.35355339f,  0.35355339f, -0.35355339f, -0.35355339f,  0.35355339f },
    {  0.27778512f, -0.49039264f,  0.09754516f,  0.41573481f, -0.41573481f, -0.09754516f,  0.49039264f, -0.27778512f },
    {  0.19134172f, -0.46193977f,  0.46193977f, -0.19134172f, -0.19134172f,  0.46193977f, -0.46193977f,  0.19134172f },
    {  0.09754516f, -0.27778512f,  0.41573481f, -0.49039264f,  0.49039264f, -0.41573481f,  0.27778512f, -0.09754516f },
};

/* Standard Annex K Huffman tables */
static const uint8_t k_dc_luma_bits[16]   = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t k_dc_luma_vals[12]   = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t k_dc_chroma_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t k_dc_chroma_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t k_ac_luma_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125};
static const uint8_t k_ac_luma_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

static const uint8_t k_ac_chroma_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,119};
static const uint8_t k_ac_chroma_vals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

struct huff_lut {
    uint16_t code[256];
    uint8_t  len[256];
};

static void build_huff_lut(const uint8_t bits[16], const uint8_t *vals, struct huff_lut *lut) {
    memset(lut, 0, sizeof(*lut));
    uint32_t code = 0;
    size_t idx = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int i = 0; i < bits[len - 1]; ++i) {
            uint8_t sym = vals[idx++];
            lut->code[sym] = (uint16_t)code;
            lut->len[sym] = (uint8_t)len;
            code++;
        }
        code <<= 1;
    }
}

struct jbw {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint32_t bitbuf;
    int bitcnt;
    bool overflow;
};

static inline void jbw_byte(struct jbw *w, uint8_t b) {
    if (w->len < w->cap) w->buf[w->len++] = b;
    else w->overflow = true;
}

static inline void jbw_be16(struct jbw *w, uint16_t v) {
    jbw_byte(w, (uint8_t)(v >> 8));
    jbw_byte(w, (uint8_t)(v & 0xFFu));
}

static inline void jbw_bits(struct jbw *w, uint32_t val, int nbits) {
    if (nbits <= 0) return;
    w->bitbuf = (w->bitbuf << nbits) | (val & ((1u << nbits) - 1u));
    w->bitcnt += nbits;
    while (w->bitcnt >= 8) {
        w->bitcnt -= 8;
        uint8_t b = (uint8_t)((w->bitbuf >> w->bitcnt) & 0xFFu);
        jbw_byte(w, b);
        if (b == 0xFFu) jbw_byte(w, 0x00u);
    }
}

static void jbw_flush(struct jbw *w) {
    if (w->bitcnt > 0)
        jbw_bits(w, (1u << (8 - w->bitcnt)) - 1u, 8 - w->bitcnt);
}

static void emit_dht(struct jbw *w, uint8_t tc_th, const uint8_t bits[16],
                     const uint8_t *vals, size_t nvals) {
    jbw_be16(w, 0xFFC4u);
    jbw_be16(w, (uint16_t)(2u + 1u + 16u + nvals));
    jbw_byte(w, tc_th);
    for (int i = 0; i < 16; ++i) jbw_byte(w, bits[i]);
    for (size_t i = 0; i < nvals; ++i) jbw_byte(w, vals[i]);
}

static inline int vli_cat_bits(int v, uint32_t *out_bits) {
    if (v == 0) { *out_bits = 0; return 0; }
    int av = (v < 0) ? -v : v;
    int cat = 0;
    while (av > 0) { cat++; av >>= 1; }
    *out_bits = (v < 0) ? (uint32_t)(v + (1 << cat) - 1) : (uint32_t)v;
    return cat;
}

static void encode_block_dct(struct jbw *w, const float block[64],
                             const uint8_t qtbl[64], int *prev_dc,
                             const struct huff_lut *dc_lut,
                             const struct huff_lut *ac_lut) {
    float tmp[64], coeff[64];
    /* T = C * block */
    for (int u = 0; u < 8; ++u) {
        for (int x = 0; x < 8; ++x) {
            float s = 0.0f;
            for (int k = 0; k < 8; ++k)
                s += k_dct_basis[u][k] * block[k * 8 + x];
            tmp[u * 8 + x] = s;
        }
    }
    /* D = T * C^T */
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            float s = 0.0f;
            for (int k = 0; k < 8; ++k)
                s += tmp[u * 8 + k] * k_dct_basis[v][k];
            coeff[u * 8 + v] = s;
        }
    }

    int zz[64];
    for (int i = 0; i < 64; ++i) {
        uint8_t idx = k_zigzag[i];
        float q = (float)qtbl[idx];
        float v = coeff[idx] / q;
        zz[i] = (v >= 0.0f) ? (int)(v + 0.5f) : (int)(v - 0.5f);
    }

    /* DC coefficient */
    int diff = zz[0] - *prev_dc;
    *prev_dc = zz[0];
    uint32_t vbits = 0;
    int cat = vli_cat_bits(diff, &vbits);
    if (cat > 11) cat = 11;
    jbw_bits(w, dc_lut->code[cat], dc_lut->len[cat]);
    jbw_bits(w, vbits, cat);

    /* AC coefficients (1..63) */
    int zeros = 0;
    for (int i = 1; i < 64; ++i) {
        int val = zz[i];
        if (val == 0) {
            zeros++;
            continue;
        }
        while (zeros >= 16) {
            jbw_bits(w, ac_lut->code[0xF0], ac_lut->len[0xF0]); /* ZRL */
            zeros -= 16;
        }
        int ac_cat = vli_cat_bits(val, &vbits);
        if (ac_cat > 10) ac_cat = 10;
        uint8_t sym = (uint8_t)((zeros << 4) | ac_cat);
        jbw_bits(w, ac_lut->code[sym], ac_lut->len[sym]);
        jbw_bits(w, vbits, ac_cat);
        zeros = 0;
    }
    if (zeros > 0)
        jbw_bits(w, ac_lut->code[0x00], ac_lut->len[0x00]); /* EOB */
}

size_t marble_encode_baseline_jpeg_rgb24(const uint8_t *rgb, uint32_t w, uint32_t h,
                                         int quality, uint8_t *out_buf, size_t out_cap) {
    if (!rgb || !out_buf || w == 0 || h == 0 || out_cap < 1024)
        return 0;
    if (quality < 10) quality = 10;
    if (quality > 98) quality = 98;
    int scale = (quality < 50) ? (5000 / quality) : (200 - 2 * quality);

    uint8_t qluma[64], qchroma[64];
    for (int i = 0; i < 64; ++i) {
        int ql = (k_qtable_luma[i] * scale + 50) / 100;
        int qc = (k_qtable_chroma[i] * scale + 50) / 100;
        qluma[i] = (uint8_t)(ql < 1 ? 1 : (ql > 255 ? 255 : ql));
        qchroma[i] = (uint8_t)(qc < 1 ? 1 : (qc > 255 ? 255 : qc));
    }

    struct huff_lut dc_y, dc_c, ac_y, ac_c;
    build_huff_lut(k_dc_luma_bits, k_dc_luma_vals, &dc_y);
    build_huff_lut(k_dc_chroma_bits, k_dc_chroma_vals, &dc_c);
    build_huff_lut(k_ac_luma_bits, k_ac_luma_vals, &ac_y);
    build_huff_lut(k_ac_chroma_bits, k_ac_chroma_vals, &ac_c);

    struct jbw bw;
    memset(&bw, 0, sizeof(bw));
    bw.buf = out_buf;
    bw.cap = out_cap;

    /* SOI + APP0 JFIF 1.01 */
    jbw_be16(&bw, 0xFFD8u);
    static const uint8_t app0[] = {
        0xFF,0xE0, 0x00,0x10, 'J','F','I','F',0x00, 0x01,0x01, 0x00, 0x00,0x01, 0x00,0x01, 0x00,0x00
    };
    for (size_t i = 0; i < sizeof(app0); ++i) jbw_byte(&bw, app0[i]);

    /* DQT luma (0) & chroma (1) in zig-zag order! */
    jbw_be16(&bw, 0xFFDBu); jbw_be16(&bw, 67u); jbw_byte(&bw, 0x00u);
    for (int i = 0; i < 64; ++i) jbw_byte(&bw, qluma[k_zigzag[i]]);
    jbw_be16(&bw, 0xFFDBu); jbw_be16(&bw, 67u); jbw_byte(&bw, 0x01u);
    for (int i = 0; i < 64; ++i) jbw_byte(&bw, qchroma[k_zigzag[i]]);

    /* SOF0 Baseline DCT YCbCr 4:2:0 */
    jbw_be16(&bw, 0xFFC0u);
    jbw_be16(&bw, 17u);
    jbw_byte(&bw, 8u);
    jbw_be16(&bw, (uint16_t)h);
    jbw_be16(&bw, (uint16_t)w);
    jbw_byte(&bw, 3u);
    jbw_byte(&bw, 1u); jbw_byte(&bw, 0x22u); jbw_byte(&bw, 0u); /* Y: 2x2 */
    jbw_byte(&bw, 2u); jbw_byte(&bw, 0x11u); jbw_byte(&bw, 1u); /* Cb: 1x1 */
    jbw_byte(&bw, 3u); jbw_byte(&bw, 0x11u); jbw_byte(&bw, 1u); /* Cr: 1x1 */

    /* DHT: DC0, AC0, DC1, AC1 */
    emit_dht(&bw, 0x00u, k_dc_luma_bits, k_dc_luma_vals, sizeof(k_dc_luma_vals));
    emit_dht(&bw, 0x10u, k_ac_luma_bits, k_ac_luma_vals, sizeof(k_ac_luma_vals));
    emit_dht(&bw, 0x01u, k_dc_chroma_bits, k_dc_chroma_vals, sizeof(k_dc_chroma_vals));
    emit_dht(&bw, 0x11u, k_ac_chroma_bits, k_ac_chroma_vals, sizeof(k_ac_chroma_vals));

    /* SOS */
    jbw_be16(&bw, 0xFFDAu);
    jbw_be16(&bw, 12u);
    jbw_byte(&bw, 3u);
    jbw_byte(&bw, 1u); jbw_byte(&bw, 0x00u);
    jbw_byte(&bw, 2u); jbw_byte(&bw, 0x11u);
    jbw_byte(&bw, 3u); jbw_byte(&bw, 0x11u);
    jbw_byte(&bw, 0u); jbw_byte(&bw, 63u); jbw_byte(&bw, 0u);

    int prev_y = 0, prev_cb = 0, prev_cr = 0;
    for (uint32_t my = 0; my < h; my += 16u) {
        for (uint32_t mx = 0; mx < w; mx += 16u) {
            float y_blk[4][64], cb_blk[64], cr_blk[64];
            memset(cb_blk, 0, sizeof(cb_blk));
            memset(cr_blk, 0, sizeof(cr_blk));
            for (uint32_t py = 0; py < 16u; ++py) {
                uint32_t sy = (my + py < h) ? (my + py) : (h - 1u);
                const uint8_t *row = rgb + (size_t)sy * w * 3u;
                for (uint32_t px = 0; px < 16u; ++px) {
                    uint32_t sx = (mx + px < w) ? (mx + px) : (w - 1u);
                    float r = (float)row[sx * 3u + 0u];
                    float g = (float)row[sx * 3u + 1u];
                    float b = (float)row[sx * 3u + 2u];
                    float Y  =  0.29900f * r + 0.58700f * g + 0.11400f * b - 128.0f;
                    float Cb = -0.16874f * r - 0.33126f * g + 0.50000f * b;
                    float Cr =  0.50000f * r - 0.41869f * g - 0.08131f * b;
                    int bi = ((py >> 3) << 1) | (px >> 3);
                    y_blk[bi][(py & 7u) * 8u + (px & 7u)] = Y;
                    int ci = (int)((py >> 1) * 8u + (px >> 1));
                    cb_blk[ci] += Cb * 0.25f;
                    cr_blk[ci] += Cr * 0.25f;
                }
            }
            encode_block_dct(&bw, y_blk[0], qluma, &prev_y, &dc_y, &ac_y);
            encode_block_dct(&bw, y_blk[1], qluma, &prev_y, &dc_y, &ac_y);
            encode_block_dct(&bw, y_blk[2], qluma, &prev_y, &dc_y, &ac_y);
            encode_block_dct(&bw, y_blk[3], qluma, &prev_y, &dc_y, &ac_y);
            encode_block_dct(&bw, cb_blk, qchroma, &prev_cb, &dc_c, &ac_c);
            encode_block_dct(&bw, cr_blk, qchroma, &prev_cr, &dc_c, &ac_c);
            if (bw.overflow)
                return 0;
        }
    }

    jbw_flush(&bw);
    jbw_be16(&bw, 0xFFD9u);
    return bw.overflow ? 0 : bw.len;
}
