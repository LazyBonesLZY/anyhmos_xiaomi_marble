/*
 * marble_camera_probe.c - Clean-room SM7475 (Xiaomi marble) Camera HW Probe
 *
 * Exercises Qualcomm Spectra / Titan Camera kernel UAPI directly:
 *   - Media controller topology (/dev/media0)
 *   - Camera Request Manager (/dev/video0) & Camera Sync (/dev/video1)
 *   - Subdev capability queries (cam-cpas, cam-isp, cam-icp/LX7 FW, cam-jpeg,
 *     cam-csiphy, cam-tpg, cam-sensor, cam-eeprom, cam-actuator, cam-flash)
 *   - Physical CCI I2C sensor probing via CAM_SENSOR_PROBE_CMD with clean
 *     power-up / chip-ID verify / power-down sequences.
 *
 * Strictly zero audio/modem/persist side effects. Leaves all sensors powered
 * down and closes all device nodes cleanly before exit.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>
#include <media/cam_cpas.h>
#include <media/cam_defs.h>
#include <media/cam_icp.h>
#include <media/cam_isp.h>
#include <media/cam_jpeg.h>
#include <media/cam_req_mgr.h>
#include <media/cam_sensor.h>
#include <media/cam_sync.h>

/* Kernel sensor power sequence enum (cam_sensor_cmn_header.h) */
enum marble_pwr_seq_type {
	MARBLE_SENSOR_MCLK = 0,
	MARBLE_SENSOR_VANA = 1,
	MARBLE_SENSOR_VDIG = 2,
	MARBLE_SENSOR_VIO  = 3,
	MARBLE_SENSOR_VAF  = 4,
	MARBLE_SENSOR_RESET = 8,
};

/* Kernel I2C type enum (cam_sensor_cmn_header.h) */
enum marble_i2c_type {
	MARBLE_I2C_BYTE  = 1,
	MARBLE_I2C_WORD  = 2,
	MARBLE_I2C_3B    = 3,
	MARBLE_I2C_DWORD = 4,
};

enum marble_i2c_freq_mode {
	MARBLE_I2C_STD_MODE  = 0,
	MARBLE_I2C_FAST_MODE = 1,
};

struct marble_pwr_step {
	uint16_t seq_type;
	uint32_t config_val;
	int16_t  delay_ms;
};

struct marble_sensor_candidate {
	uint32_t slot;
	const char *name;
	uint32_t slave_addr_8bit;
	uint32_t id_reg;
	uint8_t  addr_type;
	uint8_t  data_type;
	uint32_t expected_id;
	uint32_t id_mask;
	bool     has_vdig;
};

static const struct marble_sensor_candidate k_marble_candidates[] = {
	/* Slot 0: Rear Wide 64MP (OmniVision OV64B40, verified on marble @ 0x20 id=0x5664) */
	{ 0, "ov64b40_wide",   0x20, 0x300a, MARBLE_I2C_WORD, MARBLE_I2C_WORD, 0x5664, 0xffff, true },
	{ 0, "ov64b40_w_6c",   0x6c, 0x300a, MARBLE_I2C_WORD, MARBLE_I2C_WORD, 0x5664, 0xffff, true },
	/* Slot 1: Front 16MP (Samsung S5K3P9, verified on marble @ 0x20 id=0x3109; OV16A1Q fallback) */
	{ 1, "s5k3p9_front",   0x20, 0x0000, MARBLE_I2C_WORD, MARBLE_I2C_WORD, 0x3109, 0xffff, true },
	{ 1, "s5k3p9_f_5a",    0x5a, 0x0000, MARBLE_I2C_WORD, MARBLE_I2C_WORD, 0x3109, 0xffff, true },
	{ 1, "ov16a1q_front",  0x6c, 0x300b, MARBLE_I2C_WORD, MARBLE_I2C_WORD, 0x1641, 0xffff, true },
	/* Slot 2: Rear Ultra-Wide 8MP (Sony IMX355, verified on marble @ 0x34 id=0x0355) */
	{ 2, "imx355_ultra",   0x34, 0x0016, MARBLE_I2C_WORD, MARBLE_I2C_WORD, 0x0355, 0xffff, true },
	{ 2, "imx355_u_20",    0x20, 0x0016, MARBLE_I2C_WORD, MARBLE_I2C_WORD, 0x0355, 0xffff, true },
	/* Slot 3: Rear Macro 2MP (GalaxyCore GC02M1, verified on marble @ 0x6e id=0x02e0; OV02B10 fallback) */
	{ 3, "gc02m1_macro",   0x6e, 0x00f0, MARBLE_I2C_BYTE, MARBLE_I2C_WORD, 0x02e0, 0xffff, false },
	{ 3, "gc02m1_m_20",    0x20, 0x00f0, MARBLE_I2C_BYTE, MARBLE_I2C_WORD, 0x02e0, 0xffff, false },
	{ 3, "ov02b10_macro",  0x78, 0x0002, MARBLE_I2C_BYTE, MARBLE_I2C_WORD, 0x002b, 0xffff, false },
};

static size_t append_pwr_step(uint8_t *buf, size_t off, size_t cap,
			      uint8_t cmd_type, const struct marble_pwr_step *st)
{
	struct cam_cmd_power pwr;
	struct cam_cmd_unconditional_wait wait;

	if (off + sizeof(pwr) > cap)
		return 0;
	memset(&pwr, 0, sizeof(pwr));
	pwr.count = 1;
	pwr.cmd_type = cmd_type;
	pwr.power_settings[0].power_seq_type = st->seq_type;
	pwr.power_settings[0].config_val_low = st->config_val;
	pwr.power_settings[0].config_val_high = 0;
	memcpy(buf + off, &pwr, sizeof(pwr));
	off += sizeof(pwr);

	if (st->delay_ms > 0) {
		if (off + sizeof(wait) > cap)
			return 0;
		memset(&wait, 0, sizeof(wait));
		wait.delay = st->delay_ms;
		wait.op_code = CAMERA_SENSOR_WAIT_OP_SW_UCND;
		wait.cmd_type = CAMERA_SENSOR_CMD_TYPE_WAIT;
		memcpy(buf + off, &wait, sizeof(wait));
		off += sizeof(wait);
	}
	return off;
}

static size_t build_power_cmd_buf(uint8_t *buf, size_t cap, bool has_vdig)
{
	struct marble_pwr_step up[8];
	struct marble_pwr_step dn[8];
	size_t n_up = 0, n_dn = 0, off = 0;

	/* Power UP: RESET=0 -> VIO -> VANA -> [VDIG] -> MCLK(19.2MHz) -> RESET=1 */
	up[n_up++] = (struct marble_pwr_step){ MARBLE_SENSOR_RESET, 0, 1 };
	up[n_up++] = (struct marble_pwr_step){ MARBLE_SENSOR_VIO,   0, 1 };
	up[n_up++] = (struct marble_pwr_step){ MARBLE_SENSOR_VANA,  0, 1 };
	if (has_vdig)
		up[n_up++] = (struct marble_pwr_step){ MARBLE_SENSOR_VDIG, 0, 1 };
	up[n_up++] = (struct marble_pwr_step){ MARBLE_SENSOR_MCLK, 19200000, 2 };
	up[n_up++] = (struct marble_pwr_step){ MARBLE_SENSOR_RESET, 1, 10 };

	/* Power DOWN: reverse order -> RESET=0 -> MCLK=0 -> [VDIG] -> VANA -> VIO */
	dn[n_dn++] = (struct marble_pwr_step){ MARBLE_SENSOR_RESET, 0, 2 };
	dn[n_dn++] = (struct marble_pwr_step){ MARBLE_SENSOR_MCLK,  0, 1 };
	if (has_vdig)
		dn[n_dn++] = (struct marble_pwr_step){ MARBLE_SENSOR_VDIG, 0, 1 };
	dn[n_dn++] = (struct marble_pwr_step){ MARBLE_SENSOR_VANA,  0, 1 };
	dn[n_dn++] = (struct marble_pwr_step){ MARBLE_SENSOR_VIO,   0, 1 };

	for (size_t i = 0; i < n_up; i++) {
		off = append_pwr_step(buf, off, cap, CAMERA_SENSOR_CMD_TYPE_PWR_UP, &up[i]);
		if (off == 0)
			return 0;
	}
	for (size_t i = 0; i < n_dn; i++) {
		off = append_pwr_step(buf, off, cap, CAMERA_SENSOR_CMD_TYPE_PWR_DOWN, &dn[i]);
		if (off == 0)
			return 0;
	}
	return off;
}

static size_t build_probe_i2c_buf(uint8_t *buf, size_t cap,
				  const struct marble_sensor_candidate *c)
{
	struct cam_cmd_i2c_info i2c;
	struct cam_cmd_probe_v2 prb;
	size_t need = sizeof(i2c) + sizeof(prb);

	if (cap < need)
		return 0;
	memset(&i2c, 0, sizeof(i2c));
	i2c.slave_addr = c->slave_addr_8bit;
	i2c.i2c_freq_mode = MARBLE_I2C_FAST_MODE;
	i2c.cmd_type = CAMERA_SENSOR_CMD_TYPE_I2C_INFO;

	memset(&prb, 0, sizeof(prb));
	prb.data_type = c->data_type;
	prb.addr_type = c->addr_type;
	prb.op_code = 0;
	prb.cmd_type = CAMERA_SENSOR_CMD_TYPE_PROBE;
	prb.reg_addr = c->id_reg;
	prb.expected_data = c->expected_id;
	prb.data_mask = c->id_mask;
	prb.camera_id = (uint16_t)c->slot;
	prb.pipeline_delay = 2;
	prb.logical_camera_id = c->slot;
	snprintf(prb.sensor_name, sizeof(prb.sensor_name), "%s", c->name);

	memcpy(buf, &i2c, sizeof(i2c));
	memcpy(buf + sizeof(i2c), &prb, sizeof(prb));
	return need;
}

static size_t build_probe_packet(uint8_t *pkt_buf, size_t cap,
				 int32_t i2c_mem_hdl, uint32_t i2c_len,
				 int32_t pwr_mem_hdl, uint32_t pwr_len)
{
	size_t hdr_payload_off = offsetof(struct cam_packet, payload);
	size_t total = hdr_payload_off + 2 * sizeof(struct cam_cmd_buf_desc);
	struct cam_packet *pkt;
	struct cam_cmd_buf_desc *descs;

	if (cap < total)
		return 0;
	memset(pkt_buf, 0, total);
	pkt = (struct cam_packet *)pkt_buf;
	pkt->header.op_code = CAM_SENSOR_PACKET_OPCODE_SENSOR_PROBE_V2;
	pkt->header.size = (uint32_t)total;
	pkt->header.request_id = 0;
	pkt->header.flags = 0;
	pkt->cmd_buf_offset = 0;
	pkt->num_cmd_buf = 2;
	pkt->io_configs_offset = 0;
	pkt->num_io_configs = 0;
	pkt->patch_offset = 0;
	pkt->num_patches = 0;
	pkt->kmd_cmd_buf_index = 0xffffffffU;
	pkt->kmd_cmd_buf_offset = 0;

	descs = (struct cam_cmd_buf_desc *)(pkt_buf + hdr_payload_off);
	descs[0].mem_handle = i2c_mem_hdl;
	descs[0].offset = 0;
	descs[0].size = i2c_len;
	descs[0].length = i2c_len;
	descs[0].type = CAM_CMD_BUF_I2C;
	descs[0].meta_data = 0;

	descs[1].mem_handle = pwr_mem_hdl;
	descs[1].offset = 0;
	descs[1].size = pwr_len;
	descs[1].length = pwr_len;
	descs[1].type = CAM_CMD_BUF_I2C;
	descs[1].meta_data = 0;

	return total;
}

static int run_self_test(void)
{
	uint8_t i2c_buf[256];
	uint8_t pwr_buf[512];
	uint8_t pkt_buf[256];

	if (sizeof(struct cam_control) != 24) {
		fprintf(stderr, "FAIL sizeof(cam_control)=%zu != 24\n", sizeof(struct cam_control));
		return 1;
	}
	if (sizeof(struct cam_cmd_i2c_info) != 8) {
		fprintf(stderr, "FAIL sizeof(cam_cmd_i2c_info)=%zu != 8\n", sizeof(struct cam_cmd_i2c_info));
		return 1;
	}
	if (sizeof(struct cam_cmd_probe_v2) != 72) {
		fprintf(stderr, "FAIL sizeof(cam_cmd_probe_v2)=%zu != 72\n", sizeof(struct cam_cmd_probe_v2));
		return 1;
	}
	if (sizeof(struct cam_cmd_power) != 20) {
		fprintf(stderr, "FAIL sizeof(cam_cmd_power)=%zu != 20\n", sizeof(struct cam_cmd_power));
		return 1;
	}
	if (sizeof(struct cam_cmd_unconditional_wait) != 8) {
		fprintf(stderr, "FAIL sizeof(cam_cmd_unconditional_wait)=%zu != 8\n",
			sizeof(struct cam_cmd_unconditional_wait));
		return 1;
	}
	if (sizeof(struct cam_cmd_buf_desc) != 24) {
		fprintf(stderr, "FAIL sizeof(cam_cmd_buf_desc)=%zu != 24\n", sizeof(struct cam_cmd_buf_desc));
		return 1;
	}
	if (offsetof(struct cam_packet, payload) != 56) {
		fprintf(stderr, "FAIL offsetof(cam_packet, payload)=%zu != 56\n",
			offsetof(struct cam_packet, payload));
		return 1;
	}

	size_t i2c_len = build_probe_i2c_buf(i2c_buf, sizeof(i2c_buf), &k_marble_candidates[0]);
	size_t pwr_vdig = build_power_cmd_buf(pwr_buf, sizeof(pwr_buf), true);
	size_t pwr_novdig = build_power_cmd_buf(pwr_buf, sizeof(pwr_buf), false);
	size_t pkt_len = build_probe_packet(pkt_buf, sizeof(pkt_buf), 11, (uint32_t)i2c_len,
					    12, (uint32_t)pwr_vdig);

	if (i2c_len != 80 || pwr_vdig != 308 || pwr_novdig != 252 || pkt_len != 104) {
		fprintf(stderr, "FAIL sizes i2c=%zu pwr_vdig=%zu pwr_novdig=%zu pkt=%zu\n",
			i2c_len, pwr_vdig, pwr_novdig, pkt_len);
		return 1;
	}

	printf("MARBLE_CAMERA_PROBE_SELF_TEST_OK=1 cam_control=%zu probe_v2=%zu i2c=%zu pwr_vdig=%zu pwr_novdig=%zu pkt=%zu candidates=%zu\n",
	       sizeof(struct cam_control), sizeof(struct cam_cmd_probe_v2),
	       i2c_len, pwr_vdig, pwr_novdig, pkt_len,
	       sizeof(k_marble_candidates) / sizeof(k_marble_candidates[0]));
	return 0;
}

static int cam_ioctl_user(int fd, uint32_t op_code, void *ptr, uint32_t size)
{
	struct cam_control ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.op_code = op_code;
	ctrl.size = size;
	ctrl.handle_type = CAM_HANDLE_USER_POINTER;
	ctrl.handle = (uint64_t)(uintptr_t)ptr;
	if (ioctl(fd, VIDIOC_CAM_CONTROL, &ctrl) < 0)
		return -errno;
	return 0;
}

static int cam_query_cap_wrapped(int fd, void *cap_buf, uint32_t cap_size)
{
	struct cam_query_cap_cmd q;

	memset(&q, 0, sizeof(q));
	q.size = cap_size;
	q.handle_type = CAM_HANDLE_USER_POINTER;
	q.caps_handle = (uint64_t)(uintptr_t)cap_buf;
	return cam_ioctl_user(fd, CAM_QUERY_CAP, &q, sizeof(q));
}

static int cam_query_cap_direct(int fd, void *cap_buf, uint32_t cap_size)
{
	return cam_ioctl_user(fd, CAM_QUERY_CAP, cap_buf, cap_size);
}

struct cam_dmabuf {
	int32_t  handle;
	int      fd;
	void    *vaddr;
	size_t   len;
};

static int crm_alloc_cmd_buf(int crm_fd, size_t len, struct cam_dmabuf *out)
{
	struct cam_mem_mgr_alloc_cmd cmd;
	int rc;

	memset(out, 0, sizeof(*out));
	out->fd = -1;
	memset(&cmd, 0, sizeof(cmd));
	cmd.len = len;
	cmd.align = 4096;
	cmd.num_hdl = 0;
	cmd.flags = CAM_MEM_FLAG_KMD_ACCESS | CAM_MEM_FLAG_UMD_ACCESS | CAM_MEM_FLAG_CMD_BUF_TYPE;
	rc = cam_ioctl_user(crm_fd, CAM_REQ_MGR_ALLOC_BUF, &cmd, sizeof(cmd));
	if (rc < 0)
		return rc;
	out->handle = (int32_t)cmd.out.buf_handle;
	out->fd = cmd.out.fd;
	out->len = len;
	out->vaddr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, 0);
	if (out->vaddr == MAP_FAILED) {
		int err = -errno;
		out->vaddr = NULL;
		struct cam_mem_mgr_release_cmd rel = { .buf_handle = out->handle };
		cam_ioctl_user(crm_fd, CAM_REQ_MGR_RELEASE_BUF, &rel, sizeof(rel));
		close(out->fd);
		out->fd = -1;
		return err;
	}
	return 0;
}

static void crm_free_cmd_buf(int crm_fd, struct cam_dmabuf *b)
{
	if (b->vaddr) {
		munmap(b->vaddr, b->len);
		b->vaddr = NULL;
	}
	if (b->fd >= 0) {
		close(b->fd);
		b->fd = -1;
	}
	if (b->handle) {
		struct cam_mem_mgr_release_cmd rel = { .buf_handle = b->handle };
		cam_ioctl_user(crm_fd, CAM_REQ_MGR_RELEASE_BUF, &rel, sizeof(rel));
		b->handle = 0;
	}
}

static int read_sysfs_name(const char *subdev_base, char *out, size_t cap)
{
	char path[128];
	snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", subdev_base);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, out, cap - 1);
	close(fd);
	if (n <= 0)
		return -1;
	out[n] = '\0';
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return 0;
}

struct probe_stats {
	int media_entities;
	int cpas_ok;
	int isp_ok;
	int icp_ok;
	int jpeg_ok;
	int sensor_subdevs;
	int eeprom_subdevs;
	int tpg_subdevs;
	int csiphy_subdevs;
	int sensors_probed;
	int sensor_slot_fd_idx[8];
	char sensor_probed_names[4][64];
};

static void inspect_topology(struct probe_stats *st)
{
	/* Keep /dev/video0 (cam-req-mgr) open during subdev inspection so cam_mem_mgr
	 * is initialized before /dev/v4l-subdev22 (cam-icp) opens and boots LX7 FW. */
	int crm_keep_fd = open("/dev/video0", O_RDWR);

	for (int i = 0; i < 8; i++)
		st->sensor_slot_fd_idx[i] = -1;

	/* Media controller */
	int mfd = open("/dev/media0", O_RDWR);
	if (mfd >= 0) {
		struct media_device_info minfo;
		memset(&minfo, 0, sizeof(minfo));
		if (ioctl(mfd, MEDIA_IOC_DEVICE_INFO, &minfo) == 0) {
			printf("MEDIA0 driver=%s model=%s hw_rev=0x%x\n",
			       minfo.driver, minfo.model, minfo.hw_revision);
		}
		struct media_entity_desc ent;
		uint32_t id = 0;
		for (;;) {
			memset(&ent, 0, sizeof(ent));
			ent.id = id | MEDIA_ENT_ID_FLAG_NEXT;
			if (ioctl(mfd, MEDIA_IOC_ENUM_ENTITIES, &ent) < 0)
				break;
			st->media_entities++;
			id = ent.id;
		}
		printf("MEDIA0_ENTITIES=%d\n", st->media_entities);
		close(mfd);
	} else {
		printf("MEDIA0_OPEN_ERR=%d (%s)\n", errno, strerror(errno));
	}

	/* Scan v4l-subdev0..31 */
	for (int i = 0; i < 32; i++) {
		char base[32], dev[64], name[64];
		snprintf(base, sizeof(base), "v4l-subdev%d", i);
		if (read_sysfs_name(base, name, sizeof(name)) != 0)
			continue;
		snprintf(dev, sizeof(dev), "/dev/%s", base);
		int fd = open(dev, O_RDWR);
		if (fd < 0) {
			printf("SUBDEV %s (%s) open_err=%d\n", base, name, errno);
			continue;
		}

		if (strcmp(name, "cam-cpas") == 0) {
			struct cam_cpas_query_cap cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_direct(fd, &cap, sizeof(cap));
			printf("CPAS %s rc=%d family=%u cam_ver=%u.%u.%u cpas_ver=%u.%u.%u\n",
			       base, rc, cap.camera_family,
			       cap.camera_version.major, cap.camera_version.minor, cap.camera_version.incr,
			       cap.cpas_version.major, cap.cpas_version.minor, cap.cpas_version.incr);
			if (rc == 0)
				st->cpas_ok = 1;
		} else if (strcmp(name, "cam-isp") == 0) {
			struct cam_isp_query_cap_cmd cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_wrapped(fd, &cap, sizeof(cap));
			printf("ISP %s rc=%d num_dev=%d iommu_ns=0x%x cdm_ns=0x%x",
			       base, rc, cap.num_dev, cap.device_iommu.non_secure, cap.cdm_iommu.non_secure);
			if (rc == 0) {
				st->isp_ok = 1;
				for (int j = 0; j < cap.num_dev && j < CAM_ISP_HW_MAX; j++) {
					printf(" [hw%u:n=%u,v=%u.%u.%u]",
					       cap.dev_caps[j].hw_type, cap.dev_caps[j].num_hw,
					       cap.dev_caps[j].hw_version.major,
					       cap.dev_caps[j].hw_version.minor,
					       cap.dev_caps[j].hw_version.incr);
				}
			}
			printf("\n");
		} else if (strcmp(name, "cam-icp") == 0) {
			struct cam_icp_query_cap_cmd cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_wrapped(fd, &cap, sizeof(cap));
			printf("ICP %s rc=%d fw=%u.%u.%u api=%u.%u.%u num_ipe=%u num_bps=%u iommu_ns=0x%x\n",
			       base, rc,
			       cap.fw_version.major, cap.fw_version.minor, cap.fw_version.revision,
			       cap.api_version.major, cap.api_version.minor, cap.api_version.revision,
			       cap.num_ipe, cap.num_bps, cap.dev_iommu_handle.non_secure);
			if (rc == 0)
				st->icp_ok = 1;
		} else if (strcmp(name, "cam-jpeg") == 0) {
			struct cam_jpeg_query_cap_cmd cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_wrapped(fd, &cap, sizeof(cap));
			printf("JPEG %s rc=%d num_enc=%u num_dma=%u iommu_ns=0x%x\n",
			       base, rc, cap.num_enc, cap.num_dma, cap.dev_iommu_handle.non_secure);
			if (rc == 0)
				st->jpeg_ok = 1;
		} else if (strncmp(name, "cam-csiphy-driver", 17) == 0) {
			struct cam_csiphy_query_cap cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_direct(fd, &cap, sizeof(cap));
			printf("CSIPHY %s rc=%d slot=%u ver=0x%x clk_lane=%u\n",
			       base, rc, cap.slot_info, cap.version, cap.clk_lane);
			if (rc == 0)
				st->csiphy_subdevs++;
		} else if (strcmp(name, "cam-sensor-driver") == 0) {
			struct cam_sensor_query_cap cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_direct(fd, &cap, sizeof(cap));
			printf("SENSOR %s rc=%d slot=%u yaw=%u roll=%u pitch=%u csiphy=%u eeprom=%u act=%u flash=%u\n",
			       base, rc, cap.slot_info, cap.pos_yaw, cap.pos_roll, cap.pos_pitch,
			       cap.csiphy_slot_id, cap.eeprom_slot_id, cap.actuator_slot_id, cap.flash_slot_id);
			if (rc == 0) {
				st->sensor_subdevs++;
				if (cap.slot_info < 8)
					st->sensor_slot_fd_idx[cap.slot_info] = i;
			}
		} else if (strcmp(name, "cam-eeprom") == 0) {
			struct cam_eeprom_query_cap_t cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_direct(fd, &cap, sizeof(cap));
			printf("EEPROM %s rc=%d slot=%u kprobe=%u multi=%u\n",
			       base, rc, cap.slot_info, cap.eeprom_kernel_probe, cap.is_multimodule_mode);
			if (rc == 0)
				st->eeprom_subdevs++;
		} else if (strcmp(name, "cam-tpg-driver") == 0) {
			struct cam_tpg_query_cap cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_direct(fd, &cap, sizeof(cap));
			printf("TPG %s rc=%d slot=%u ver=0x%x csiphy=%u\n",
			       base, rc, cap.slot_info, cap.version, cap.csiphy_slot_id);
			if (rc == 0)
				st->tpg_subdevs++;
		} else if (strcmp(name, "cam-actuator-driver") == 0) {
			struct cam_actuator_query_cap cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_direct(fd, &cap, sizeof(cap));
			printf("ACTUATOR %s rc=%d slot=%u\n", base, rc, cap.slot_info);
		} else if (strcmp(name, "cam-flash-dev") == 0) {
			struct cam_flash_query_cap_info cap;
			memset(&cap, 0, sizeof(cap));
			int rc = cam_query_cap_direct(fd, &cap, sizeof(cap));
			printf("FLASH %s rc=%d slot=%u max_flash=%u,%u\n",
			       base, rc, cap.slot_info, cap.max_current_flash[0], cap.max_current_flash[1]);
		}
		close(fd);
	}
	if (crm_keep_fd >= 0)
		close(crm_keep_fd);
}

static int try_probe_one(int crm_fd, int subdev_idx,
			 const struct marble_sensor_candidate *c)
{
	char dev[64];
	struct cam_dmabuf pkt_mem, i2c_mem, pwr_mem;
	int sfd = -1;
	int rc;

	snprintf(dev, sizeof(dev), "/dev/v4l-subdev%d", subdev_idx);
	sfd = open(dev, O_RDWR);
	if (sfd < 0)
		return -errno;

	if ((rc = crm_alloc_cmd_buf(crm_fd, 4096, &i2c_mem)) < 0) {
		close(sfd);
		return rc;
	}
	if ((rc = crm_alloc_cmd_buf(crm_fd, 4096, &pwr_mem)) < 0) {
		crm_free_cmd_buf(crm_fd, &i2c_mem);
		close(sfd);
		return rc;
	}
	if ((rc = crm_alloc_cmd_buf(crm_fd, 4096, &pkt_mem)) < 0) {
		crm_free_cmd_buf(crm_fd, &pwr_mem);
		crm_free_cmd_buf(crm_fd, &i2c_mem);
		close(sfd);
		return rc;
	}

	size_t i2c_len = build_probe_i2c_buf((uint8_t *)i2c_mem.vaddr, i2c_mem.len, c);
	size_t pwr_len = build_power_cmd_buf((uint8_t *)pwr_mem.vaddr, pwr_mem.len, c->has_vdig);
	size_t pkt_len = build_probe_packet((uint8_t *)pkt_mem.vaddr, pkt_mem.len,
					    i2c_mem.handle, (uint32_t)i2c_len,
					    pwr_mem.handle, (uint32_t)pwr_len);
	if (!i2c_len || !pwr_len || !pkt_len) {
		rc = -EINVAL;
	} else {
		struct cam_control ctrl;
		memset(&ctrl, 0, sizeof(ctrl));
		ctrl.op_code = CAM_SENSOR_PROBE_CMD;
		ctrl.size = 0;
		ctrl.handle_type = CAM_HANDLE_MEM_HANDLE;
		ctrl.handle = (uint64_t)(uint32_t)pkt_mem.handle;
		if (ioctl(sfd, VIDIOC_CAM_CONTROL, &ctrl) < 0)
			rc = -errno;
		else
			rc = 0;
	}

	crm_free_cmd_buf(crm_fd, &pkt_mem);
	crm_free_cmd_buf(crm_fd, &pwr_mem);
	crm_free_cmd_buf(crm_fd, &i2c_mem);
	close(sfd);
	return rc;
}

static void probe_sensors(struct probe_stats *st, const struct marble_sensor_candidate *custom)
{
	int crm_fd = open("/dev/video0", O_RDWR);
	if (crm_fd < 0) {
		printf("CRM_OPEN_ERR=%d (%s)\n", errno, strerror(errno));
		return;
	}

	struct cam_req_mgr_session_info ses;
	memset(&ses, 0, sizeof(ses));
	int ses_rc = cam_ioctl_user(crm_fd, CAM_REQ_MGR_CREATE_SESSION, &ses, sizeof(ses));
	printf("CRM_SESSION create_rc=%d hdl=0x%x\n", ses_rc, (unsigned)ses.session_hdl);

	if (custom) {
		int idx = (custom->slot < 8) ? st->sensor_slot_fd_idx[custom->slot] : -1;
		if (idx < 0) {
			printf("PROBE_CUSTOM slot=%u ERR=no_subdev\n", custom->slot);
		} else {
			int rc = try_probe_one(crm_fd, idx, custom);
			printf("PROBE_CUSTOM slot=%u dev=v4l-subdev%d name=%s addr=0x%x reg=0x%x exp=0x%x rc=%d\n",
			       custom->slot, idx, custom->name, custom->slave_addr_8bit,
			       custom->id_reg, custom->expected_id, rc);
		}
	} else {
		bool slot_done[4] = { false, false, false, false };
		size_t count = sizeof(k_marble_candidates) / sizeof(k_marble_candidates[0]);
		for (size_t i = 0; i < count; i++) {
			const struct marble_sensor_candidate *c = &k_marble_candidates[i];
			if (c->slot >= 4 || slot_done[c->slot])
				continue;
			int idx = st->sensor_slot_fd_idx[c->slot];
			if (idx < 0)
				continue;
			int rc = try_probe_one(crm_fd, idx, c);
			printf("PROBE slot=%u dev=v4l-subdev%d cand=%s addr=0x%02x reg=0x%04x exp=0x%04x rc=%d\n",
			       c->slot, idx, c->name, c->slave_addr_8bit, c->id_reg, c->expected_id, rc);
			if (rc == 0) {
				slot_done[c->slot] = true;
				st->sensors_probed++;
				snprintf(st->sensor_probed_names[c->slot],
					 sizeof(st->sensor_probed_names[c->slot]),
					 "%s(0x%02x:0x%04x)", c->name, c->slave_addr_8bit, c->expected_id);
			}
		}
	}

	if (ses_rc == 0) {
		int drc = cam_ioctl_user(crm_fd, CAM_REQ_MGR_DESTROY_SESSION, &ses, sizeof(ses));
		printf("CRM_SESSION destroy_rc=%d\n", drc);
	}
	close(crm_fd);
}

int main(int argc, char **argv)
{
	bool do_topo = true;
	bool do_probe = false;
	bool has_custom = false;
	struct marble_sensor_candidate custom;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--self-test") == 0) {
			return run_self_test();
		} else if (strcmp(argv[i], "--topology") == 0) {
			do_topo = true;
			do_probe = false;
		} else if (strcmp(argv[i], "--probe-sensors") == 0) {
			do_topo = true;
			do_probe = true;
		} else if (strcmp(argv[i], "--all") == 0) {
			do_topo = true;
			do_probe = true;
		} else if (strcmp(argv[i], "--probe-custom") == 0 && i + 8 < argc) {
			memset(&custom, 0, sizeof(custom));
			custom.slot = (uint32_t)strtoul(argv[++i], NULL, 0);
			custom.name = "custom_probe";
			custom.slave_addr_8bit = (uint32_t)strtoul(argv[++i], NULL, 0);
			custom.id_reg = (uint32_t)strtoul(argv[++i], NULL, 0);
			custom.addr_type = (uint8_t)strtoul(argv[++i], NULL, 0);
			custom.data_type = (uint8_t)strtoul(argv[++i], NULL, 0);
			custom.expected_id = (uint32_t)strtoul(argv[++i], NULL, 0);
			custom.id_mask = (uint32_t)strtoul(argv[++i], NULL, 0);
			custom.has_vdig = (strtoul(argv[++i], NULL, 0) != 0);
			has_custom = true;
			do_topo = true;
			do_probe = true;
		}
	}

	struct probe_stats st;
	memset(&st, 0, sizeof(st));
	if (do_topo)
		inspect_topology(&st);
	if (do_probe)
		probe_sensors(&st, has_custom ? &custom : NULL);

	printf("MARBLE_CAMERA_PROBE_SUMMARY media_entities=%d cpas=%d isp=%d icp=%d jpeg=%d csiphy=%d tpg=%d sensors=%d eeproms=%d probed=%d s0=%s s1=%s s2=%s s3=%s\n",
	       st.media_entities, st.cpas_ok, st.isp_ok, st.icp_ok, st.jpeg_ok,
	       st.csiphy_subdevs, st.tpg_subdevs, st.sensor_subdevs, st.eeprom_subdevs,
	       st.sensors_probed,
	       st.sensor_probed_names[0][0] ? st.sensor_probed_names[0] : "none",
	       st.sensor_probed_names[1][0] ? st.sensor_probed_names[1] : "none",
	       st.sensor_probed_names[2][0] ? st.sensor_probed_names[2] : "none",
	       st.sensor_probed_names[3][0] ? st.sensor_probed_names[3] : "none");
	return 0;
}
