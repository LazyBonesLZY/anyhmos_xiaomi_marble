/* SPDX-License-Identifier: MIT
 * Clean-room RAM-backed Copy-on-Write QMI RMTFS server (service 14) for Xiaomi marble.
 * Opens modemst1/modemst2/fsg/fsc strictly O_RDONLY into RAM buffers, closes the block
 * descriptors immediately, and absorbs all modem writes in RAM only (zero flash writes).
 */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libqrtr.h"
#include "qmi_rmtfs.h"

#define MAX_CALLERS 16
#define MIB (1024ULL * 1024ULL)

static struct qmi_elem_info rmtfs_qmi_result_ei[] = {
	{
		.data_type = QMI_UNSIGNED_2_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint16_t),
		.offset = offsetof(struct rmtfs_qmi_result, result),
	},
	{
		.data_type = QMI_UNSIGNED_2_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint16_t),
		.offset = offsetof(struct rmtfs_qmi_result, error),
	},
	{}
};

static struct qmi_elem_info rmtfs_iovec_entry_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.offset = offsetof(struct rmtfs_iovec_entry, sector_addr),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.offset = offsetof(struct rmtfs_iovec_entry, phys_offset),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.offset = offsetof(struct rmtfs_iovec_entry, num_sector),
	},
	{}
};

struct qmi_elem_info rmtfs_open_req_ei[] = {
	{
		.data_type = QMI_STRING,
		.elem_len = 256,
		.elem_size = sizeof(char),
		.array_type = NO_ARRAY,
		.tlv_type = 1,
		.offset = offsetof(struct rmtfs_open_req, path),
	},
	{}
};

struct qmi_elem_info rmtfs_open_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct rmtfs_qmi_result),
		.tlv_type = 2,
		.offset = offsetof(struct rmtfs_open_resp, result),
		.ei_array = rmtfs_qmi_result_ei,
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(bool),
		.tlv_type = 16,
		.offset = offsetof(struct rmtfs_open_resp, caller_id_valid),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.tlv_type = 16,
		.offset = offsetof(struct rmtfs_open_resp, caller_id),
	},
	{}
};

struct qmi_elem_info rmtfs_close_req_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.tlv_type = 1,
		.offset = offsetof(struct rmtfs_close_req, caller_id),
	},
	{}
};

struct qmi_elem_info rmtfs_close_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct rmtfs_qmi_result),
		.tlv_type = 2,
		.offset = offsetof(struct rmtfs_close_resp, result),
		.ei_array = rmtfs_qmi_result_ei,
	},
	{}
};

struct qmi_elem_info rmtfs_iovec_req_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.tlv_type = 1,
		.offset = offsetof(struct rmtfs_iovec_req, caller_id),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 2,
		.offset = offsetof(struct rmtfs_iovec_req, direction),
	},
	{
		.data_type = QMI_DATA_LEN,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 3,
		.offset = offsetof(struct rmtfs_iovec_req, iovec_len),
	},
	{
		.data_type = QMI_STRUCT,
		.elem_len = RMTFS_MAX_IOVEC,
		.elem_size = sizeof(struct rmtfs_iovec_entry),
		.array_type = VAR_LEN_ARRAY,
		.tlv_type = 3,
		.offset = offsetof(struct rmtfs_iovec_req, iovec),
		.ei_array = rmtfs_iovec_entry_ei,
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 4,
		.offset = offsetof(struct rmtfs_iovec_req, is_force_sync),
	},
	{}
};

struct qmi_elem_info rmtfs_iovec_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct rmtfs_qmi_result),
		.tlv_type = 2,
		.offset = offsetof(struct rmtfs_iovec_resp, result),
		.ei_array = rmtfs_qmi_result_ei,
	},
	{}
};

struct qmi_elem_info rmtfs_alloc_buf_req_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.tlv_type = 1,
		.offset = offsetof(struct rmtfs_alloc_buf_req, caller_id),
	},
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.tlv_type = 2,
		.offset = offsetof(struct rmtfs_alloc_buf_req, buff_size),
	},
	{}
};

struct qmi_elem_info rmtfs_alloc_buf_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct rmtfs_qmi_result),
		.tlv_type = 2,
		.offset = offsetof(struct rmtfs_alloc_buf_resp, result),
		.ei_array = rmtfs_qmi_result_ei,
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(bool),
		.tlv_type = 16,
		.offset = offsetof(struct rmtfs_alloc_buf_resp, buff_address_valid),
	},
	{
		.data_type = QMI_UNSIGNED_8_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint64_t),
		.tlv_type = 16,
		.offset = offsetof(struct rmtfs_alloc_buf_resp, buff_address),
	},
	{}
};

struct qmi_elem_info rmtfs_dev_error_req_ei[] = {
	{
		.data_type = QMI_UNSIGNED_4_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint32_t),
		.tlv_type = 1,
		.offset = offsetof(struct rmtfs_dev_error_req, caller_id),
	},
	{}
};

struct qmi_elem_info rmtfs_dev_error_resp_ei[] = {
	{
		.data_type = QMI_STRUCT,
		.elem_len = 1,
		.elem_size = sizeof(struct rmtfs_qmi_result),
		.tlv_type = 2,
		.offset = offsetof(struct rmtfs_dev_error_resp, result),
		.ei_array = rmtfs_qmi_result_ei,
	},
	{
		.data_type = QMI_OPT_FLAG,
		.elem_len = 1,
		.elem_size = sizeof(bool),
		.tlv_type = 16,
		.offset = offsetof(struct rmtfs_dev_error_resp, status_valid),
	},
	{
		.data_type = QMI_UNSIGNED_1_BYTE,
		.elem_len = 1,
		.elem_size = sizeof(uint8_t),
		.tlv_type = 16,
		.offset = offsetof(struct rmtfs_dev_error_resp, status),
	},
	{}
};

struct shadow_part {
	const char *part_name;
	size_t max_bytes;
	uint8_t *buf;
	size_t size;
	uint64_t read_sectors;
	uint64_t write_sectors;
};

enum part_idx {
	PART_FS1 = 0,
	PART_FS2,
	PART_FSG,
	PART_FSC,
	PART_COUNT,
};

static struct shadow_part g_parts[PART_COUNT] = {
	[PART_FS1] = { .part_name = "modemst1", .max_bytes = 16 * MIB },
	[PART_FS2] = { .part_name = "modemst2", .max_bytes = 16 * MIB },
	[PART_FSG] = { .part_name = "fsg",      .max_bytes = 16 * MIB },
	[PART_FSC] = { .part_name = "fsc",      .max_bytes = 4 * MIB  },
};

struct caller_slot {
	bool in_use;
	uint32_t id;
	uint32_t node;
	enum part_idx part;
};

static struct caller_slot g_callers[MAX_CALLERS];
static uint32_t g_next_caller_id = 1;

struct uio_shm {
	int fd;
	int uio_idx;
	uint64_t phys_addr;
	uint64_t size;
	uint8_t *base;
};

static struct uio_shm g_shm = { .fd = -1, .uio_idx = -1 };
static volatile sig_atomic_t g_stop;

static void on_sig(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void set_result(struct rmtfs_qmi_result *res, uint16_t err)
{
	if (err == QMI_RMTFS_ERR_NONE) {
		res->result = QMI_RMTFS_RESULT_SUCCESS;
		res->error = QMI_RMTFS_ERR_NONE;
	} else {
		res->result = QMI_RMTFS_RESULT_FAILURE;
		res->error = err;
	}
}

static int read_sysfs_str(const char *path, char *out, size_t cap)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;
	size_t len;

	if (fd < 0)
		return -errno;
	n = read(fd, out, cap - 1);
	close(fd);
	if (n < 0)
		return -errno;
	out[n] = '\0';
	len = (size_t)n;
	while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' '))
		out[--len] = '\0';
	return 0;
}

static int read_sysfs_u64(const char *path, uint64_t *val)
{
	char buf[64];
	char *end = NULL;
	unsigned long long v;
	int rc = read_sysfs_str(path, buf, sizeof(buf));

	if (rc < 0)
		return rc;
	errno = 0;
	v = strtoull(buf, &end, 0);
	if (errno != 0 || end == buf || *end != '\0')
		return -EINVAL;
	*val = (uint64_t)v;
	return 0;
}

static int uio_shm_init(const char *sys_uio_dir)
{
	DIR *dir = opendir(sys_uio_dir);
	struct dirent *de;
	char path[PATH_MAX];
	char name[64];
	int found = -1;

	if (!dir) {
		fprintf(stderr, "RMTFS_ERR opendir(%s): %s\n", sys_uio_dir, strerror(errno));
		return -errno;
	}

	while ((de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "uio", 3) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/name", sys_uio_dir, de->d_name);
		if (read_sysfs_str(path, name, sizeof(name)) == 0 && strcmp(name, "rmtfs") == 0) {
			found = atoi(de->d_name + 3);
			break;
		}
	}
	closedir(dir);

	if (found < 0) {
		fprintf(stderr, "RMTFS_ERR no UIO device named 'rmtfs' under %s\n", sys_uio_dir);
		return -ENOENT;
	}

	g_shm.uio_idx = found;
	snprintf(path, sizeof(path), "%s/uio%d/maps/map0/addr", sys_uio_dir, found);
	if (read_sysfs_u64(path, &g_shm.phys_addr) < 0) {
		fprintf(stderr, "RMTFS_ERR failed to read %s\n", path);
		return -EINVAL;
	}
	snprintf(path, sizeof(path), "%s/uio%d/maps/map0/size", sys_uio_dir, found);
	if (read_sysfs_u64(path, &g_shm.size) < 0 || g_shm.size == 0) {
		fprintf(stderr, "RMTFS_ERR failed to read %s\n", path);
		return -EINVAL;
	}

	snprintf(path, sizeof(path), "/dev/uio%d", found);
	g_shm.fd = open(path, O_RDWR | O_SYNC);
	if (g_shm.fd < 0) {
		fprintf(stderr, "RMTFS_ERR open(%s): %s\n", path, strerror(errno));
		return -errno;
	}

	g_shm.base = mmap(NULL, (size_t)g_shm.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm.fd, 0);
	if (g_shm.base == MAP_FAILED) {
		g_shm.base = NULL;
		fprintf(stderr, "RMTFS_ERR mmap(%s, 0x%llx): %s\n", path,
			(unsigned long long)g_shm.size, strerror(errno));
		close(g_shm.fd);
		g_shm.fd = -1;
		return -errno;
	}

	printf("RMTFS_UIO dev=/dev/uio%d addr=0x%llx size=0x%llx\n",
	       found, (unsigned long long)g_shm.phys_addr, (unsigned long long)g_shm.size);
	return 0;
}

static int load_ro_shadow_parts(const char *part_dir)
{
	for (size_t i = 0; i < PART_COUNT; i++) {
		char path[PATH_MAX];
		int fd;
		off_t sz;
		struct stat st;
		size_t total = 0;

		snprintf(path, sizeof(path), "%s/%s", part_dir, g_parts[i].part_name);
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "RMTFS_ERR open(%s, O_RDONLY): %s\n", path, strerror(errno));
			return -errno;
		}
		if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
			sz = st.st_size;
		} else {
			sz = lseek(fd, 0, SEEK_END);
			if (sz < 0 || lseek(fd, 0, SEEK_SET) < 0) {
				int err = errno;
				close(fd);
				fprintf(stderr, "RMTFS_ERR seek(%s): %s\n", path, strerror(err));
				return -err;
			}
		}
		if (sz <= 0 || (size_t)sz > g_parts[i].max_bytes) {
			close(fd);
			fprintf(stderr, "RMTFS_ERR unexpected size %lld for %s (max %zu)\n",
				(long long)sz, path, g_parts[i].max_bytes);
			return -EINVAL;
		}
		g_parts[i].buf = calloc(1, (size_t)sz);
		if (!g_parts[i].buf) {
			close(fd);
			return -ENOMEM;
		}
		while (total < (size_t)sz) {
			ssize_t n = read(fd, g_parts[i].buf + total, (size_t)sz - total);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				int err = n < 0 ? errno : EIO;
				close(fd);
				fprintf(stderr, "RMTFS_ERR read(%s) at %zu/%lld: %s\n",
					path, total, (long long)sz, strerror(err));
				return -err;
			}
			total += (size_t)n;
		}
		close(fd); /* Strictly O_RDONLY and closed immediately: zero flash write path. */
		g_parts[i].size = total;
		printf("RMTFS_SHADOW part=%s bytes=%zu mode=ro_ram_cow\n",
		       g_parts[i].part_name, g_parts[i].size);
	}
	return 0;
}

static int resolve_path(const char *raw_path, enum part_idx *out_idx)
{
	const char *p = raw_path;

	if (!p || !*p)
		return -EINVAL;
	while (*p == '/')
		p++;
	if (strcmp(p, "boot/modem_fs1") == 0 || strcmp(p, "modem_fs1") == 0) {
		*out_idx = PART_FS1;
		return 0;
	}
	if (strcmp(p, "boot/modem_fs2") == 0 || strcmp(p, "modem_fs2") == 0) {
		*out_idx = PART_FS2;
		return 0;
	}
	if (strcmp(p, "boot/modem_fsg") == 0 || strcmp(p, "modem_fsg") == 0) {
		*out_idx = PART_FSG;
		return 0;
	}
	if (strcmp(p, "boot/modem_fsc") == 0 || strcmp(p, "modem_fsc") == 0) {
		*out_idx = PART_FSC;
		return 0;
	}
	return -ENOENT;
}

static uint8_t *shm_ptr(uint64_t addr_or_off, size_t bytes)
{
	if (!g_shm.base || bytes == 0)
		return NULL;
	if (addr_or_off >= g_shm.phys_addr) {
		uint64_t rel = addr_or_off - g_shm.phys_addr;
		if (rel <= g_shm.size && (uint64_t)bytes <= g_shm.size - rel)
			return g_shm.base + rel;
		return NULL;
	}
	if (addr_or_off <= g_shm.size && (uint64_t)bytes <= g_shm.size - addr_or_off)
		return g_shm.base + addr_or_off;
	return NULL;
}

static struct caller_slot *find_caller(uint32_t node, uint32_t caller_id)
{
	for (size_t i = 0; i < MAX_CALLERS; i++) {
		if (g_callers[i].in_use && g_callers[i].id == caller_id && g_callers[i].node == node)
			return &g_callers[i];
	}
	return NULL;
}

static void handle_open(int sock, const struct qrtr_packet *pkt)
{
	struct rmtfs_open_req req = {0};
	struct rmtfs_open_resp resp = {0};
	DEFINE_QRTR_PACKET(out, 256);
	unsigned int txn = 0;
	enum part_idx pidx = PART_FS1;
	struct caller_slot *slot = NULL;
	ssize_t len;

	if (qmi_decode_message(&req, &txn, pkt, QMI_REQUEST, QMI_RMTFS_OPEN, rmtfs_open_req_ei) < 0) {
		set_result(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		printf("RMTFS_OPEN path=<malformed> rc=err_malformed\n");
		goto send;
	}
	req.path[sizeof(req.path) - 1] = '\0';

	if (resolve_path(req.path, &pidx) < 0 || !g_parts[pidx].buf) {
		set_result(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		printf("RMTFS_OPEN path=%s mapped=<none> caller=0 rc=err_internal\n", req.path);
		goto send;
	}

	for (size_t i = 0; i < MAX_CALLERS; i++) {
		if (!g_callers[i].in_use) {
			slot = &g_callers[i];
			break;
		}
	}
	if (!slot) {
		set_result(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		printf("RMTFS_OPEN path=%s mapped=%s caller=0 rc=err_full\n",
		       req.path, g_parts[pidx].part_name);
		goto send;
	}

	slot->in_use = true;
	slot->id = g_next_caller_id++;
	if (g_next_caller_id == 0)
		g_next_caller_id = 1;
	slot->node = pkt->node;
	slot->part = pidx;

	set_result(&resp.result, QMI_RMTFS_ERR_NONE);
	resp.caller_id_valid = true;
	resp.caller_id = slot->id;
	printf("RMTFS_OPEN path=%s mapped=%s caller=%u node=%u rc=0\n",
	       req.path, g_parts[pidx].part_name, slot->id, pkt->node);

send:
	len = qmi_encode_message(&out, QMI_RESPONSE, QMI_RMTFS_OPEN, txn, &resp, rmtfs_open_resp_ei);
	if (len > 0 && sock >= 0)
		(void)qrtr_sendto(sock, pkt->node, pkt->port, out.data, (unsigned int)out.data_len);
}

static void handle_close(int sock, const struct qrtr_packet *pkt)
{
	struct rmtfs_close_req req = {0};
	struct rmtfs_close_resp resp = {0};
	DEFINE_QRTR_PACKET(out, 128);
	unsigned int txn = 0;
	struct caller_slot *slot;
	ssize_t len;

	if (qmi_decode_message(&req, &txn, pkt, QMI_REQUEST, QMI_RMTFS_CLOSE, rmtfs_close_req_ei) < 0) {
		set_result(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		goto send;
	}
	slot = find_caller(pkt->node, req.caller_id);
	if (!slot) {
		set_result(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		printf("RMTFS_CLOSE caller=%u rc=err_not_found\n", req.caller_id);
		goto send;
	}
	printf("RMTFS_CLOSE caller=%u part=%s rc=0\n",
	       slot->id, g_parts[slot->part].part_name);
	memset(slot, 0, sizeof(*slot));
	set_result(&resp.result, QMI_RMTFS_ERR_NONE);

send:
	len = qmi_encode_message(&out, QMI_RESPONSE, QMI_RMTFS_CLOSE, txn, &resp, rmtfs_close_resp_ei);
	if (len > 0 && sock >= 0)
		(void)qrtr_sendto(sock, pkt->node, pkt->port, out.data, (unsigned int)out.data_len);
}

static int apply_iovec(struct caller_slot *slot, const struct rmtfs_iovec_req *req, uint64_t *total_sectors)
{
	struct shadow_part *part = &g_parts[slot->part];
	bool is_write = req->direction != 0;
	uint64_t sectors_done = 0;

	if (req->iovec_len > RMTFS_MAX_IOVEC)
		return -EINVAL;

	for (uint32_t i = 0; i < req->iovec_len; i++) {
		const struct rmtfs_iovec_entry *e = &req->iovec[i];
		uint64_t sec_off = (uint64_t)e->sector_addr * RMTFS_SECTOR_SIZE;
		uint64_t byte_len = (uint64_t)e->num_sector * RMTFS_SECTOR_SIZE;
		uint8_t *shm = shm_ptr(e->phys_offset, (size_t)byte_len);

		if (e->num_sector == 0)
			continue;
		if (!shm)
			return -EFAULT;
		if (sec_off > part->size || byte_len > part->size - sec_off)
			return -ERANGE;

		if (is_write) {
			memcpy(part->buf + sec_off, shm, (size_t)byte_len);
			part->write_sectors += e->num_sector;
		} else {
			memcpy(shm, part->buf + sec_off, (size_t)byte_len);
			part->read_sectors += e->num_sector;
		}
		sectors_done += e->num_sector;
	}
	*total_sectors = sectors_done;
	return 0;
}

static void handle_iovec(int sock, const struct qrtr_packet *pkt)
{
	struct rmtfs_iovec_req req = {0};
	struct rmtfs_iovec_resp resp = {0};
	DEFINE_QRTR_PACKET(out, 128);
	unsigned int txn = 0;
	struct caller_slot *slot;
	uint64_t sectors = 0;
	int rc;
	ssize_t len;

	if (qmi_decode_message(&req, &txn, pkt, QMI_REQUEST, QMI_RMTFS_RW_IOVEC, rmtfs_iovec_req_ei) < 0) {
		set_result(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		printf("RMTFS_RW_IOVEC rc=err_malformed\n");
		goto send;
	}
	slot = find_caller(pkt->node, req.caller_id);
	if (!slot) {
		set_result(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		printf("RMTFS_RW_IOVEC caller=%u rc=err_not_found\n", req.caller_id);
		goto send;
	}
	rc = apply_iovec(slot, &req, &sectors);
	if (rc < 0) {
		set_result(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		printf("RMTFS_RW_IOVEC caller=%u part=%s dir=%s entries=%u rc=%d\n",
		       slot->id, g_parts[slot->part].part_name,
		       req.direction ? "write_ram" : "read",
		       req.iovec_len, rc);
		goto send;
	}
	set_result(&resp.result, QMI_RMTFS_ERR_NONE);
	printf("RMTFS_RW_IOVEC caller=%u part=%s dir=%s entries=%u sectors=%llu force=%u rc=0\n",
	       slot->id, g_parts[slot->part].part_name,
	       req.direction ? "write_ram" : "read",
	       req.iovec_len, (unsigned long long)sectors, req.is_force_sync);

send:
	len = qmi_encode_message(&out, QMI_RESPONSE, QMI_RMTFS_RW_IOVEC, txn, &resp, rmtfs_iovec_resp_ei);
	if (len > 0 && sock >= 0)
		(void)qrtr_sendto(sock, pkt->node, pkt->port, out.data, (unsigned int)out.data_len);
}

static void handle_alloc_buff(int sock, const struct qrtr_packet *pkt)
{
	struct rmtfs_alloc_buf_req req = {0};
	struct rmtfs_alloc_buf_resp resp = {0};
	DEFINE_QRTR_PACKET(out, 128);
	unsigned int txn = 0;
	ssize_t len;

	if (qmi_decode_message(&req, &txn, pkt, QMI_REQUEST, QMI_RMTFS_ALLOC_BUFF, rmtfs_alloc_buf_req_ei) < 0) {
		set_result(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		printf("RMTFS_ALLOC_BUFF rc=err_malformed\n");
		goto send;
	}
	if (!g_shm.base || req.buff_size > g_shm.size) {
		set_result(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		printf("RMTFS_ALLOC_BUFF caller=%u size=%u avail=0x%llx rc=err_size\n",
		       req.caller_id, req.buff_size, (unsigned long long)g_shm.size);
		goto send;
	}
	set_result(&resp.result, QMI_RMTFS_ERR_NONE);
	resp.buff_address_valid = true;
	resp.buff_address = g_shm.phys_addr;
	printf("RMTFS_ALLOC_BUFF caller=%u size=%u addr=0x%llx rc=0\n",
	       req.caller_id, req.buff_size, (unsigned long long)resp.buff_address);

send:
	len = qmi_encode_message(&out, QMI_RESPONSE, QMI_RMTFS_ALLOC_BUFF, txn, &resp, rmtfs_alloc_buf_resp_ei);
	if (len > 0 && sock >= 0)
		(void)qrtr_sendto(sock, pkt->node, pkt->port, out.data, (unsigned int)out.data_len);
}

static void handle_get_dev_error(int sock, const struct qrtr_packet *pkt)
{
	struct rmtfs_dev_error_req req = {0};
	struct rmtfs_dev_error_resp resp = {0};
	DEFINE_QRTR_PACKET(out, 128);
	unsigned int txn = 0;
	ssize_t len;

	if (qmi_decode_message(&req, &txn, pkt, QMI_REQUEST, QMI_RMTFS_GET_DEV_ERROR, rmtfs_dev_error_req_ei) < 0) {
		set_result(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		goto send;
	}
	set_result(&resp.result, QMI_RMTFS_ERR_NONE);
	resp.status_valid = true;
	resp.status = 0;
	printf("RMTFS_GET_DEV_ERROR caller=%u status=0 rc=0\n", req.caller_id);

send:
	len = qmi_encode_message(&out, QMI_RESPONSE, QMI_RMTFS_GET_DEV_ERROR, txn, &resp, rmtfs_dev_error_resp_ei);
	if (len > 0 && sock >= 0)
		(void)qrtr_sendto(sock, pkt->node, pkt->port, out.data, (unsigned int)out.data_len);
}

static int run_self_test(void)
{
	uint8_t fake_shm[4096];
	uint8_t fake_fs1[4096];
	uint8_t req_buf[512];
	struct qrtr_packet req_pkt = { .type = QRTR_TYPE_DATA, .node = 1, .port = 100,
				       .data = req_buf, .data_len = sizeof(req_buf) };
	struct qrtr_packet dec_pkt;

	/* Synthetic UIO shared buffer and RO shadow EFS image */
	memset(fake_shm, 0, sizeof(fake_shm));
	for (size_t i = 0; i < sizeof(fake_fs1); i++)
		fake_fs1[i] = (uint8_t)((i * 29 + 7) & 0xff);
	uint8_t orig_copy[4096];
	memcpy(orig_copy, fake_fs1, sizeof(orig_copy));

	g_shm.phys_addr = 0x8bc00000ULL;
	g_shm.size = sizeof(fake_shm);
	g_shm.base = fake_shm;
	g_parts[PART_FS1].buf = malloc(sizeof(fake_fs1));
	assert(g_parts[PART_FS1].buf != NULL);
	memcpy(g_parts[PART_FS1].buf, fake_fs1, sizeof(fake_fs1));
	g_parts[PART_FS1].size = sizeof(fake_fs1);

	/* 1. OPEN round-trip */
	struct rmtfs_open_req oreq = {0};
	struct rmtfs_open_req oreq_dec = {0};
	snprintf(oreq.path, sizeof(oreq.path), "/boot/modem_fs1");
	req_pkt.data_len = sizeof(req_buf);
	assert(qmi_encode_message(&req_pkt, QMI_REQUEST, QMI_RMTFS_OPEN, 1, &oreq, rmtfs_open_req_ei) > 0);
	unsigned int txn = 0;
	assert(qmi_decode_message(&oreq_dec, &txn, &req_pkt, QMI_REQUEST, QMI_RMTFS_OPEN, rmtfs_open_req_ei) > 0);
	assert(txn == 1 && strcmp(oreq_dec.path, "/boot/modem_fs1") == 0);
	enum part_idx pidx = PART_FSC;
	assert(resolve_path(oreq_dec.path, &pidx) == 0 && pidx == PART_FS1);

	/* Simulate caller allocation */
	memset(g_callers, 0, sizeof(g_callers));
	g_callers[0].in_use = true;
	g_callers[0].id = 1;
	g_callers[0].node = 1;
	g_callers[0].part = PART_FS1;

	/* 2. ALLOC_BUFF response round-trip */
	struct rmtfs_alloc_buf_resp aresp = {0}, aresp_dec = {0};
	set_result(&aresp.result, QMI_RMTFS_ERR_NONE);
	aresp.buff_address_valid = true;
	aresp.buff_address = g_shm.phys_addr;
	dec_pkt.data = req_buf;
	dec_pkt.data_len = sizeof(req_buf);
	assert(qmi_encode_message(&dec_pkt, QMI_RESPONSE, QMI_RMTFS_ALLOC_BUFF, 2, &aresp, rmtfs_alloc_buf_resp_ei) > 0);
	assert(qmi_decode_message(&aresp_dec, &txn, &dec_pkt, QMI_RESPONSE, QMI_RMTFS_ALLOC_BUFF, rmtfs_alloc_buf_resp_ei) > 0);
	assert(txn == 2 && aresp_dec.buff_address_valid && aresp_dec.buff_address == 0x8bc00000ULL);

	/* 3. RW_IOVEC encode/decode + READ then RAM COW WRITE (original image untouched) */
	struct rmtfs_iovec_req rreq = {0}, rreq_dec = {0};
	rreq.caller_id = 1;
	rreq.direction = 0;
	rreq.iovec_len = 1;
	rreq.iovec[0].sector_addr = 2;
	rreq.iovec[0].phys_offset = 0x8bc00200U;
	rreq.iovec[0].num_sector = 2;
	rreq.is_force_sync = 0;
	req_pkt.data_len = sizeof(req_buf);
	assert(qmi_encode_message(&req_pkt, QMI_REQUEST, QMI_RMTFS_RW_IOVEC, 3, &rreq, rmtfs_iovec_req_ei) > 0);
	assert(qmi_decode_message(&rreq_dec, &txn, &req_pkt, QMI_REQUEST, QMI_RMTFS_RW_IOVEC, rmtfs_iovec_req_ei) > 0);
	assert(rreq_dec.caller_id == 1 && rreq_dec.direction == 0 && rreq_dec.iovec_len == 1);
	uint64_t secs = 0;
	assert(apply_iovec(&g_callers[0], &rreq_dec, &secs) == 0 && secs == 2);
	assert(memcmp(fake_shm + 512, orig_copy + 1024, 1024) == 0);

	/* RAM write into shadow buf at sector 1 */
	memset(fake_shm + 2048, 0x5A, 512);
	rreq.direction = 1;
	rreq.iovec[0].sector_addr = 1;
	rreq.iovec[0].phys_offset = 0x8bc00800U;
	rreq.iovec[0].num_sector = 1;
	assert(apply_iovec(&g_callers[0], &rreq, &secs) == 0 && secs == 1);
	assert(memcmp(g_parts[PART_FS1].buf + 512, fake_shm + 2048, 512) == 0);
	assert(memcmp(fake_fs1, orig_copy, sizeof(fake_fs1)) == 0);

	free(g_parts[PART_FS1].buf);
	g_parts[PART_FS1].buf = NULL;
	g_shm.base = NULL;
	puts("RMTFS_SELF_TEST_OK=1");
	return 0;
}

int main(int argc, char **argv)
{
	const char *part_dir = "/dev/block/by-name";
	const char *uio_dir = "/sys/class/uio";
	int fd;
	char buf[8192];

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--self-test") == 0)
			return run_self_test();
		if (strcmp(argv[i], "--part-dir") == 0 && i + 1 < argc)
			part_dir = argv[++i];
		else if (strcmp(argv[i], "--uio-dir") == 0 && i + 1 < argc)
			uio_dir = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--self-test] [--part-dir DIR] [--uio-dir DIR]\n", argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	if (uio_shm_init(uio_dir) < 0)
		return 1;
	if (load_ro_shadow_parts(part_dir) < 0)
		return 1;

	fd = qrtr_open(RMTFS_QMI_SERVICE);
	if (fd < 0) {
		fprintf(stderr, "RMTFS_WARN qrtr_open(%d) failed, trying ephemeral port 0\n",
			RMTFS_QMI_SERVICE);
		fd = qrtr_open(0);
	}
	if (fd < 0) {
		fprintf(stderr, "RMTFS_ERR failed to open QRTR socket\n");
		return 1;
	}
	if (qrtr_publish(fd, RMTFS_QMI_SERVICE, RMTFS_QMI_VERSION, RMTFS_QMI_INSTANCE) < 0) {
		fprintf(stderr, "RMTFS_ERR qrtr_publish(%d,%d,%d) failed\n",
			RMTFS_QMI_SERVICE, RMTFS_QMI_VERSION, RMTFS_QMI_INSTANCE);
		qrtr_close(fd);
		return 1;
	}

	printf("RMTFS_READY service=%d version=%d instance=%d\n",
	       RMTFS_QMI_SERVICE, RMTFS_QMI_VERSION, RMTFS_QMI_INSTANCE);

	while (!g_stop) {
		int prc = qrtr_poll(fd, 1000);
		struct sockaddr_qrtr sq;
		socklen_t sl = sizeof(sq);
		struct qrtr_packet pkt;
		unsigned int msg_id = 0;
		ssize_t n;

		if (prc < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "RMTFS_ERR qrtr_poll: %s\n", strerror(errno));
			break;
		}
		if (prc == 0)
			continue;

		n = recvfrom(fd, buf, sizeof(buf), 0, (void *)&sq, &sl);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			fprintf(stderr, "RMTFS_ERR recvfrom: %s\n", strerror(errno));
			break;
		}
		if (qrtr_decode(&pkt, buf, (size_t)n, &sq) < 0)
			continue;
		if (pkt.type != QRTR_TYPE_DATA)
			continue;
		if (qmi_decode_header(&pkt, &msg_id) < 0)
			continue;

		switch (msg_id) {
		case QMI_RMTFS_OPEN:
			handle_open(fd, &pkt);
			break;
		case QMI_RMTFS_CLOSE:
			handle_close(fd, &pkt);
			break;
		case QMI_RMTFS_RW_IOVEC:
			handle_iovec(fd, &pkt);
			break;
		case QMI_RMTFS_ALLOC_BUFF:
			handle_alloc_buff(fd, &pkt);
			break;
		case QMI_RMTFS_GET_DEV_ERROR:
			handle_get_dev_error(fd, &pkt);
			break;
		default:
			printf("RMTFS_UNHANDLED msg_id=0x%x node=%u port=0x%x\n",
			       msg_id, pkt.node, pkt.port);
			break;
		}
	}

	(void)qrtr_bye(fd, RMTFS_QMI_SERVICE, RMTFS_QMI_VERSION, RMTFS_QMI_INSTANCE);
	qrtr_close(fd);
	if (g_shm.base)
		munmap(g_shm.base, (size_t)g_shm.size);
	if (g_shm.fd >= 0)
		close(g_shm.fd);
	for (size_t i = 0; i < PART_COUNT; i++)
		free(g_parts[i].buf);
	return 0;
}
