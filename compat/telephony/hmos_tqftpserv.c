/* SPDX-License-Identifier: MIT */
/*
 * Clean-room RO->RAM Copy-on-Write Qualcomm TFTP/RFS server (QRTR service 0x1000)
 * for Xiaomi marble (SM7475 / ukee).
 *
 * Security & Flash Safety:
 * - Never opens /dev/block directly and never mounts partitions.
 * - Reads files from --root (RAM tmpfs seeded from transient ro,noload copy)
 *   and optional --ro-dir search paths.
 * - All WRQ writes land strictly inside --root (RAM overlay only).
 * - Any write targeting /readonly/... or traversal with '..' is rejected with
 *   TFTP ERROR code 2 (Access violation).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/qrtr.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#ifndef QRTR_PORT_CTRL
#define QRTR_PORT_CTRL 0xfffffffeu
#endif

#ifndef QRTR_TYPE_NEW_SERVER
#define QRTR_TYPE_NEW_SERVER 4u
#define QRTR_TYPE_DEL_SERVER 5u
#endif

#define TFTP_QMI_SERVICE 0x1000u
#define TFTP_DEFAULT_BLKSIZE 512u
#define TFTP_MAX_BLKSIZE 32768u
#define TFTP_DEFAULT_TIMEOUT 2u
#define TFTP_MAX_TIMEOUT 60u
#define TFTP_DEFAULT_TIMEOUT_MS 2000u
#define TFTP_MIN_TIMEOUT_MS 100u
#define TFTP_MAX_TIMEOUT_MS 60000u
#define TFTP_MAX_WSIZE 16u
#define TFTP_MAX_RO_DIRS 8
#define TFTP_MAX_WORKERS 32
#define TFTP_MAX_RETRIES 5

enum tftp_opcode {
	TFTP_OP_RRQ = 1,
	TFTP_OP_WRQ = 2,
	TFTP_OP_DATA = 3,
	TFTP_OP_ACK = 4,
	TFTP_OP_ERROR = 5,
	TFTP_OP_OACK = 6,
};

enum tftp_errcode {
	TFTP_ERR_UNDEF = 0,
	TFTP_ERR_ENOTFOUND = 1,
	TFTP_ERR_ACCESS = 2,
	TFTP_ERR_ENOSPC = 3,
	TFTP_ERR_BADOP = 4,
	TFTP_ERR_BADTID = 5,
	TFTP_ERR_EXISTS = 6,
	TFTP_ERR_OPTNEG = 8,
};

struct tqftp_opts {
	uint32_t blksize;
	uint32_t timeout_ms;
	uint32_t wsize;
	uint64_t tsize;
	uint64_t rsize;
	uint64_t seek;
	bool has_blksize;
	bool has_timeout;
	bool has_timeoutms;
	bool has_wsize;
	bool has_tsize;
	bool has_rsize;
	bool has_seek;
	bool has_append;
	bool has_unlink;
	bool has_any;
	uint64_t unlink_val;
	char opt_summary[256];
};

struct tqftp_config {
	char root_dir[PATH_MAX];
	char root_real[PATH_MAX];
	char ro_dirs[TFTP_MAX_RO_DIRS][PATH_MAX];
	char ro_real[TFTP_MAX_RO_DIRS][PATH_MAX];
	size_t num_ro_dirs;
};

struct tqftp_session_req {
	const struct tqftp_config *cfg;
	struct sockaddr_storage peer;
	socklen_t peer_len;
	int sock_family;
	uint8_t req_pkt[4096];
	size_t req_len;
};

static volatile sig_atomic_t g_stop = 0;
static pthread_mutex_t g_worker_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_active_workers = 0;

static void on_sig(int sig)
{
	(void)sig;
	g_stop = 1;
}

static uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xffu);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return -1;
	return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000LL);
}

static bool has_prefix_dir(const char *path, const char *prefix)
{
	size_t plen = strlen(prefix);
	if (plen == 0)
		return false;
	if (strncmp(path, prefix, plen) != 0)
		return false;
	return path[plen] == '\0' || path[plen] == '/';
}

static bool path_is_readonly_namespace(const char *rel)
{
	return strcmp(rel, "readonly") == 0 || strncmp(rel, "readonly/", 9) == 0;
}

/*
 * Normalize a client TFTP filename to a clean relative path without leading '/',
 * '.' or empty segments, and strictly reject any '..' traversal or control bytes.
 */
static int normalize_tftp_rel(const char *raw, char *out, size_t out_cap)
{
	if (!raw || !out || out_cap == 0)
		return -EINVAL;

	char copy[PATH_MAX];
	size_t raw_len = strlen(raw);
	if (raw_len == 0 || raw_len >= sizeof(copy))
		return -EINVAL;

	for (size_t i = 0; i < raw_len; i++) {
		unsigned char c = (unsigned char)raw[i];
		if (c < 0x20 || c == 0x7f || c == '\\')
			return -EACCES;
	}
	memcpy(copy, raw, raw_len + 1);

	size_t used = 0;
	out[0] = '\0';
	char *p = copy;
	while (*p != '\0') {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		char *seg = p;
		while (*p != '\0' && *p != '/')
			p++;
		if (*p == '/')
			*p++ = '\0';

		if (strcmp(seg, ".") == 0)
			continue;
		if (strcmp(seg, "..") == 0)
			return -EACCES;

		size_t slen = strlen(seg);
		if (used > 0) {
			if (used + 1 + slen >= out_cap)
				return -ENAMETOOLONG;
			out[used++] = '/';
		} else if (slen >= out_cap) {
			return -ENAMETOOLONG;
		}
		memcpy(out + used, seg, slen);
		used += slen;
		out[used] = '\0';
	}

	if (used == 0)
		return -EACCES;
	return 0;
}

static bool check_real_confined(const char *cand_path, const char *allowed_real)
{
	char rp[PATH_MAX];
	if (!realpath(cand_path, rp))
		return false;
	return has_prefix_dir(rp, allowed_real);
}

static int mkdir_p_under_root(const char *root_dir, const char *root_real, const char *rel_file)
{
	char work[PATH_MAX];
	if (snprintf(work, sizeof(work), "%s", rel_file) >= (int)sizeof(work))
		return -ENAMETOOLONG;

	char cur[PATH_MAX];
	if (snprintf(cur, sizeof(cur), "%s", root_dir) >= (int)sizeof(cur))
		return -ENAMETOOLONG;

	char *slash = strrchr(work, '/');
	if (!slash)
		return 0;
	*slash = '\0';

	char *p = work;
	while (*p != '\0') {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		char *seg = p;
		while (*p != '\0' && *p != '/')
			p++;
		if (*p == '/')
			*p++ = '\0';

		size_t clen = strlen(cur);
		size_t slen = strlen(seg);
		if (clen + 1 + slen >= sizeof(cur))
			return -ENAMETOOLONG;
		cur[clen] = '/';
		memcpy(cur + clen + 1, seg, slen + 1);

		struct stat st;
		if (lstat(cur, &st) == 0) {
			if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
				return -EACCES;
		} else {
			if (mkdir(cur, 0755) != 0 && errno != EEXIST)
				return -errno;
		}
		if (!check_real_confined(cur, root_real))
			return -EACCES;
	}
	return 0;
}

static int try_read_candidate(const char *base_dir, const char *base_real, const char *rel,
			      char *resolved, size_t cap)
{
	if (!base_dir[0] || !base_real[0] || !rel[0])
		return -ENOENT;
	char cand[PATH_MAX];
	if (snprintf(cand, sizeof(cand), "%s/%s", base_dir, rel) >= (int)sizeof(cand))
		return -ENAMETOOLONG;

	char rp[PATH_MAX];
	if (!realpath(cand, rp))
		return -errno;
	if (!has_prefix_dir(rp, base_real))
		return -EACCES;
	struct stat st;
	if (stat(rp, &st) != 0)
		return -errno;
	if (!S_ISREG(st.st_mode))
		return -EACCES;
	if (snprintf(resolved, cap, "%s", rp) >= (int)cap)
		return -ENAMETOOLONG;
	return 0;
}

static void compact_ro_dirs(struct tqftp_config *cfg)
{
	size_t valid_ro = 0;
	for (size_t i = 0; i < cfg->num_ro_dirs && i < TFTP_MAX_RO_DIRS; i++) {
		char rp[PATH_MAX];
		if (!realpath(cfg->ro_dirs[i], rp))
			continue;
		if (valid_ro != i) {
			size_t slen = strlen(cfg->ro_dirs[i]);
			memmove(cfg->ro_dirs[valid_ro], cfg->ro_dirs[i], slen + 1);
		}
		snprintf(cfg->ro_real[valid_ro], sizeof(cfg->ro_real[0]), "%s", rp);
		valid_ro++;
	}
	for (size_t i = valid_ro; i < TFTP_MAX_RO_DIRS; i++) {
		cfg->ro_dirs[i][0] = '\0';
		cfg->ro_real[i][0] = '\0';
	}
	cfg->num_ro_dirs = valid_ro;
}

static int resolve_tftp_path(const struct tqftp_config *cfg, const char *raw_path, bool is_write,
			     char *resolved, size_t cap)
{
	char rel[PATH_MAX];
	int rc = normalize_tftp_rel(raw_path, rel, sizeof(rel));
	if (rc < 0)
		return rc;

	bool is_ro_ns = path_is_readonly_namespace(rel);
	if (is_write && is_ro_ns)
		return -EACCES;

	char mapped_rel[PATH_MAX];
	bool has_mapped = false;
	if (strcmp(rel, "readwrite") == 0 || strcmp(rel, "shared") == 0 || strcmp(rel, "hlos") == 0) {
		if (is_write)
			return -EACCES;
	} else if (strncmp(rel, "readwrite/", 10) == 0 && rel[10] != '\0') {
		if (snprintf(mapped_rel, sizeof(mapped_rel), "rfs/msm/mpss/%s", rel + 10) >= (int)sizeof(mapped_rel))
			return -ENAMETOOLONG;
		has_mapped = true;
	} else if (strncmp(rel, "shared/", 7) == 0 && rel[7] != '\0') {
		if (snprintf(mapped_rel, sizeof(mapped_rel), "rfs/shared/%s", rel + 7) >= (int)sizeof(mapped_rel))
			return -ENAMETOOLONG;
		has_mapped = true;
	} else if (strncmp(rel, "hlos/", 5) == 0 && rel[5] != '\0') {
		if (snprintf(mapped_rel, sizeof(mapped_rel), "hlos_rfs/%s", rel + 5) >= (int)sizeof(mapped_rel))
			return -ENAMETOOLONG;
		has_mapped = true;
	}

	if (is_write) {
		char target_rel[PATH_MAX];
		char probe[PATH_MAX];
		if (has_mapped) {
			snprintf(target_rel, sizeof(target_rel), "%s", mapped_rel);
		} else if (!has_prefix_dir(rel, "rfs") && !has_prefix_dir(rel, "hlos_rfs") &&
			   try_read_candidate(cfg->root_dir, cfg->root_real, rel, probe, sizeof(probe)) != 0) {
			char rfs_rel[PATH_MAX];
			if (snprintf(rfs_rel, sizeof(rfs_rel), "rfs/%s", rel) < (int)sizeof(rfs_rel) &&
			    try_read_candidate(cfg->root_dir, cfg->root_real, rfs_rel, probe, sizeof(probe)) == 0) {
				snprintf(target_rel, sizeof(target_rel), "%s", rfs_rel);
			} else {
				snprintf(target_rel, sizeof(target_rel), "%s", rel);
			}
		} else {
			snprintf(target_rel, sizeof(target_rel), "%s", rel);
		}

		rc = mkdir_p_under_root(cfg->root_dir, cfg->root_real, target_rel);
		if (rc < 0)
			return rc;
		if (snprintf(resolved, cap, "%s/%s", cfg->root_dir, target_rel) >= (int)cap)
			return -ENAMETOOLONG;
		struct stat st;
		if (lstat(resolved, &st) == 0 && S_ISLNK(st.st_mode))
			return -EACCES;
		return 0;
	}

	/* Read resolution order: virtual namespace mapping, root_dir, legacy namespaces, then ro_dirs. */
	if (has_mapped &&
	    try_read_candidate(cfg->root_dir, cfg->root_real, mapped_rel, resolved, cap) == 0)
		return 0;

	if (try_read_candidate(cfg->root_dir, cfg->root_real, rel, resolved, cap) == 0)
		return 0;

	if (strncmp(rel, "hlos_rfs/", 9) == 0) {
		char sub[PATH_MAX];
		if (snprintf(sub, sizeof(sub), "rfs/%s", rel + 9) < (int)sizeof(sub) &&
		    try_read_candidate(cfg->root_dir, cfg->root_real, sub, resolved, cap) == 0)
			return 0;
	} else if (is_ro_ns && rel[8] == '/') {
		static const char *const kRoPrefixes[] = {
			"vendor/firmware_mnt/image/modem_pr/mcfg/",
			"firmware/image/modem_pr/mcfg/",
			"vendor/firmware_mnt/image/",
			"firmware/image/",
			"image/modem_pr/mcfg/",
			"modem_pr/mcfg/",
			"firmware/",
			"",
		};
		const char *after_ro = rel + 9;
		if (try_read_candidate(cfg->root_dir, cfg->root_real, after_ro, resolved, cap) == 0)
			return 0;
		for (size_t d = 0; d < cfg->num_ro_dirs; d++) {
			for (size_t p = 0; p < sizeof(kRoPrefixes) / sizeof(kRoPrefixes[0]); p++) {
				size_t plen = strlen(kRoPrefixes[p]);
				const char *sub = after_ro;
				if (plen > 0) {
					if (strncmp(after_ro, kRoPrefixes[p], plen) != 0)
						continue;
					sub = after_ro + plen;
				}
				if (sub[0] != '\0' &&
				    try_read_candidate(cfg->ro_dirs[d], cfg->ro_real[d], sub, resolved, cap) == 0)
					return 0;
			}
		}
	} else if (!has_mapped && !has_prefix_dir(rel, "rfs")) {
		char alt[PATH_MAX];
		if (snprintf(alt, sizeof(alt), "rfs/%s", rel) < (int)sizeof(alt) &&
		    try_read_candidate(cfg->root_dir, cfg->root_real, alt, resolved, cap) == 0)
			return 0;
		if (snprintf(alt, sizeof(alt), "hlos_rfs/%s", rel) < (int)sizeof(alt) &&
		    try_read_candidate(cfg->root_dir, cfg->root_real, alt, resolved, cap) == 0)
			return 0;
	}

	for (size_t d = 0; d < cfg->num_ro_dirs; d++) {
		if (has_mapped &&
		    try_read_candidate(cfg->ro_dirs[d], cfg->ro_real[d], mapped_rel, resolved, cap) == 0)
			return 0;
		if (try_read_candidate(cfg->ro_dirs[d], cfg->ro_real[d], rel, resolved, cap) == 0)
			return 0;
	}
	return -ENOENT;
}

static bool parse_u64_strict(const char *s, uint64_t *out)
{
	if (!s || !isdigit((unsigned char)s[0]))
		return false;
	errno = 0;
	char *end = NULL;
	unsigned long long v = strtoull(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0')
		return false;
	*out = (uint64_t)v;
	return true;
}

static void append_opt_summary(struct tqftp_opts *opts, const char *key, const char *val)
{
	size_t used = strlen(opts->opt_summary);
	size_t cap = sizeof(opts->opt_summary);
	if (used + 4 >= cap)
		return;
	if (used > 0)
		opts->opt_summary[used++] = ',';
	for (size_t i = 0; key[i] != '\0' && used + 3 < cap; i++) {
		unsigned char c = (unsigned char)key[i];
		opts->opt_summary[used++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
	}
	opts->opt_summary[used++] = '=';
	for (size_t i = 0; val[i] != '\0' && used + 1 < cap; i++) {
		unsigned char c = (unsigned char)val[i];
		opts->opt_summary[used++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
	}
	opts->opt_summary[used] = '\0';
}

static int parse_req_packet(const uint8_t *pkt, size_t len, uint16_t *out_op,
			    const char **out_file, const char **out_mode, struct tqftp_opts *opts)
{
	if (len < 6)
		return -EINVAL;
	uint16_t op = get_be16(pkt);
	if (op != TFTP_OP_RRQ && op != TFTP_OP_WRQ)
		return -EPROTONOSUPPORT;

	memset(opts, 0, sizeof(*opts));
	opts->blksize = TFTP_DEFAULT_BLKSIZE;
	opts->timeout_ms = TFTP_DEFAULT_TIMEOUT_MS;
	opts->wsize = 1;

	size_t pos = 2;
	const char *fname = (const char *)(pkt + pos);
	size_t flen = strnlen(fname, len - pos);
	if (flen == 0 || pos + flen >= len)
		return -EINVAL;
	pos += flen + 1;

	const char *mode = (const char *)(pkt + pos);
	size_t mlen = strnlen(mode, len - pos);
	if (mlen == 0 || pos + mlen >= len)
		return -EINVAL;
	pos += mlen + 1;

	if (strcasecmp(mode, "octet") != 0 && strcasecmp(mode, "netascii") != 0)
		return -EPROTONOSUPPORT;

	while (pos < len) {
		const char *key = (const char *)(pkt + pos);
		size_t klen = strnlen(key, len - pos);
		if (klen == 0 || pos + klen >= len)
			break;
		pos += klen + 1;

		const char *val = (const char *)(pkt + pos);
		size_t vlen = strnlen(val, len - pos);
		if (pos + vlen >= len)
			return -EINVAL;
		pos += vlen + 1;

		append_opt_summary(opts, key, val);

		if (strcasecmp(key, "do_append") == 0) {
			opts->has_append = (val[0] == '\0' || strcmp(val, "0") != 0);
			opts->has_any = true;
			continue;
		}
		if (strcasecmp(key, "unlink") == 0) {
			uint64_t unum = 0;
			if (val[0] != '\0' && !parse_u64_strict(val, &unum))
				continue;
			opts->has_unlink = true;
			opts->unlink_val = unum;
			opts->has_any = true;
			continue;
		}

		uint64_t num = 0;
		if (!parse_u64_strict(val, &num))
			continue;

		opts->has_any = true;
		if (strcasecmp(key, "blksize") == 0) {
			if (num < 8)
				num = 8;
			if (num > TFTP_MAX_BLKSIZE)
				num = TFTP_MAX_BLKSIZE;
			opts->blksize = (uint32_t)num;
			opts->has_blksize = true;
		} else if (strcasecmp(key, "timeout") == 0) {
			if (num < 1)
				num = 1;
			if (num > TFTP_MAX_TIMEOUT)
				num = TFTP_MAX_TIMEOUT;
			opts->timeout_ms = (uint32_t)num * 1000u;
			opts->has_timeout = true;
		} else if (strcasecmp(key, "timeoutms") == 0) {
			if (num < TFTP_MIN_TIMEOUT_MS)
				num = TFTP_MIN_TIMEOUT_MS;
			if (num > TFTP_MAX_TIMEOUT_MS)
				num = TFTP_MAX_TIMEOUT_MS;
			opts->timeout_ms = (uint32_t)num;
			opts->has_timeoutms = true;
		} else if (strcasecmp(key, "wsize") == 0) {
			if (num < 1)
				num = 1;
			if (num > TFTP_MAX_WSIZE)
				num = TFTP_MAX_WSIZE;
			opts->wsize = (uint32_t)num;
			opts->has_wsize = true;
		} else if (strcasecmp(key, "tsize") == 0) {
			opts->tsize = num;
			opts->has_tsize = true;
		} else if (strcasecmp(key, "rsize") == 0) {
			opts->rsize = num;
			opts->has_rsize = true;
		} else if (strcasecmp(key, "seek") == 0) {
			opts->seek = num;
			opts->has_seek = true;
		}
	}

	if (opts->opt_summary[0] == '\0')
		snprintf(opts->opt_summary, sizeof(opts->opt_summary), "none");

	*out_op = op;
	*out_file = fname;
	*out_mode = mode;
	return 0;
}

static size_t append_oack_kv(uint8_t *buf, size_t pos, size_t cap, const char *k, uint64_t v)
{
	int n = snprintf((char *)(buf + pos), cap - pos, "%s", k);
	if (n < 0 || pos + (size_t)n + 1 >= cap)
		return pos;
	pos += (size_t)n + 1;
	n = snprintf((char *)(buf + pos), cap - pos, "%llu", (unsigned long long)v);
	if (n < 0 || pos + (size_t)n + 1 > cap)
		return pos;
	pos += (size_t)n + 1;
	return pos;
}

static size_t build_oack(uint8_t *buf, size_t cap, const struct tqftp_opts *opts,
			 bool is_write, uint64_t xfer_len)
{
	if (cap < 2)
		return 0;
	put_be16(buf, TFTP_OP_OACK);
	size_t pos = 2;
	if (opts->has_blksize)
		pos = append_oack_kv(buf, pos, cap, "blksize", opts->blksize);
	if (opts->has_timeout) {
		uint64_t sec = opts->timeout_ms / 1000u;
		if (sec < 1)
			sec = 1;
		pos = append_oack_kv(buf, pos, cap, "timeout", sec);
	}
	if (opts->has_timeoutms)
		pos = append_oack_kv(buf, pos, cap, "timeoutms", opts->timeout_ms);
	if (opts->has_wsize)
		pos = append_oack_kv(buf, pos, cap, "wsize", opts->wsize);
	if (opts->has_tsize)
		pos = append_oack_kv(buf, pos, cap, "tsize", is_write ? opts->tsize : xfer_len);
	if (opts->has_rsize)
		pos = append_oack_kv(buf, pos, cap, "rsize", is_write ? opts->rsize : xfer_len);
	if (opts->has_seek)
		pos = append_oack_kv(buf, pos, cap, "seek", opts->seek);
	if (opts->has_append)
		pos = append_oack_kv(buf, pos, cap, "do_append", 1);
	if (opts->has_unlink)
		pos = append_oack_kv(buf, pos, cap, "unlink", opts->unlink_val);
	return pos;
}

static ssize_t send_pkt(int fd, const void *buf, size_t len,
			const struct sockaddr *peer, socklen_t peer_len)
{
	const struct sockaddr *dst = peer_len > 0 ? peer : NULL;
	socklen_t dlen = peer_len > 0 ? peer_len : 0;
	for (;;) {
		ssize_t n = sendto(fd, buf, len, 0, dst, dlen);
		if (n < 0 && errno == EINTR)
			continue;
		return n;
	}
}

static void send_tftp_error(int fd, const struct sockaddr *peer, socklen_t peer_len,
			    uint16_t code, const char *msg)
{
	uint8_t buf[256];
	put_be16(buf, TFTP_OP_ERROR);
	put_be16(buf + 2, code);
	size_t mlen = strlen(msg);
	if (mlen > sizeof(buf) - 5)
		mlen = sizeof(buf) - 5;
	memcpy(buf + 4, msg, mlen);
	buf[4 + mlen] = '\0';
	(void)send_pkt(fd, buf, 5 + mlen, peer, peer_len);
}

static void send_tftp_ack(int fd, const struct sockaddr *peer, socklen_t peer_len, uint16_t block)
{
	uint8_t buf[4];
	put_be16(buf, TFTP_OP_ACK);
	put_be16(buf + 2, block);
	(void)send_pkt(fd, buf, sizeof(buf), peer, peer_len);
}

static ssize_t recv_pkt_timeout(int fd, uint8_t *buf, size_t cap, int timeout_ms)
{
	int64_t deadline = monotonic_ms() + timeout_ms;
	for (;;) {
		int64_t now = monotonic_ms();
		if (now >= deadline)
			return -ETIMEDOUT;
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int rc = poll(&pfd, 1, (int)(deadline - now));
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0)
			return -errno;
		if (rc == 0)
			return -ETIMEDOUT;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -EIO;
		if (pfd.revents & POLLIN) {
			ssize_t n = recvfrom(fd, buf, cap, MSG_DONTWAIT, NULL, NULL);
			if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
				continue;
			if (n < 0)
				return -errno;
			return n;
		}
	}
}

static int err_to_tftp(int err_neg)
{
	int e = -err_neg;
	if (e == ENOENT)
		return TFTP_ERR_ENOTFOUND;
	if (e == EACCES || e == EPERM)
		return TFTP_ERR_ACCESS;
	if (e == ENOSPC)
		return TFTP_ERR_ENOSPC;
	if (e == EEXIST)
		return TFTP_ERR_EXISTS;
	return TFTP_ERR_UNDEF;
}

static int serve_rrq(int xfer_fd, const struct sockaddr *peer, socklen_t peer_len,
		     const char *local_path, const char *raw_path, struct tqftp_opts *opts)
{
	int ffd = open(local_path, O_RDONLY | O_NOFOLLOW);
	if (ffd < 0) {
		int rc = -errno;
		send_tftp_error(xfer_fd, peer, peer_len, (uint16_t)err_to_tftp(rc), strerror(-rc));
		return rc;
	}
	struct stat st;
	if (fstat(ffd, &st) != 0) {
		int rc = -errno;
		close(ffd);
		send_tftp_error(xfer_fd, peer, peer_len, TFTP_ERR_UNDEF, "fstat failed");
		return rc;
	}
	uint64_t fsize = (uint64_t)st.st_size;
	uint64_t off = opts->has_seek ? opts->seek : 0;
	uint64_t avail = (off < fsize) ? (fsize - off) : 0;
	if (opts->has_rsize && opts->rsize > 0 && opts->rsize < avail)
		avail = opts->rsize;

	if (opts->has_any) {
		uint8_t oack[512];
		size_t oack_len = build_oack(oack, sizeof(oack), opts, false, avail);
		bool got_ack0 = false;
		for (int attempt = 0; attempt < TFTP_MAX_RETRIES; attempt++) {
			if (send_pkt(xfer_fd, oack, oack_len, peer, peer_len) < 0) {
				close(ffd);
				return -errno;
			}
			uint8_t rx[512];
			ssize_t rlen = recv_pkt_timeout(xfer_fd, rx, sizeof(rx), (int)opts->timeout_ms);
			if (rlen == -ETIMEDOUT)
				continue;
			if (rlen < 4) {
				printf("TQFTP_DONE op=RRQ path=%s bytes=0 rc=-5 stage=oack\n", raw_path);
				close(ffd);
				return -EIO;
			}
			uint16_t op = get_be16(rx);
			uint16_t blk = get_be16(rx + 2);
			if (op == TFTP_OP_ERROR) {
				char emsg[128] = "";
				if (rlen > 4) {
					size_t n = (size_t)rlen - 4u;
					if (n >= sizeof(emsg))
						n = sizeof(emsg) - 1;
					size_t j = 0;
					for (size_t k = 0; k < n && rx[4 + k] != '\0'; k++) {
						unsigned char ch = rx[4 + k];
						emsg[j++] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '.';
					}
					emsg[j] = '\0';
				}
				printf("TQFTP_DONE op=RRQ path=%s bytes=0 rc=-104 peer_err=%u(%s)\n",
				       raw_path, blk, emsg);
				close(ffd);
				return -ECONNRESET;
			}
			if (op == TFTP_OP_ACK && blk == 0) {
				got_ack0 = true;
				break;
			}
		}
		if (!got_ack0) {
			printf("TQFTP_DONE op=RRQ path=%s bytes=0 rc=-110 stage=oack\n", raw_path);
			close(ffd);
			return -ETIMEDOUT;
		}
	}

	uint8_t *pkt = malloc(4u + opts->blksize);
	if (!pkt) {
		close(ffd);
		return -ENOMEM;
	}

	uint16_t base_block = 1;
	uint64_t sent_bytes = 0;
	uint64_t base_off = off;
	bool done = false;
	int rc = 0;

	while (!done && !g_stop) {
		uint32_t sent_win = 0;
		bool win_eof = false;
		uint64_t cursor_off = base_off;

		for (uint32_t w = 0; w < opts->wsize; w++) {
			uint64_t rem = (sent_bytes + (cursor_off - base_off) < avail)
					   ? (avail - (sent_bytes + (cursor_off - base_off)))
					   : 0;
			size_t want = rem < opts->blksize ? (size_t)rem : (size_t)opts->blksize;
			ssize_t nr = 0;
			if (want > 0) {
				nr = pread(ffd, pkt + 4, want, (off_t)cursor_off);
				if (nr < 0) {
					rc = -errno;
					break;
				}
			}
			uint16_t blk = (uint16_t)(base_block + w);
			put_be16(pkt, TFTP_OP_DATA);
			put_be16(pkt + 2, blk);
			if (send_pkt(xfer_fd, pkt, 4u + (size_t)nr, peer, peer_len) < 0) {
				rc = -errno;
				break;
			}
			cursor_off += (uint64_t)nr;
			sent_win++;
			if ((size_t)nr < opts->blksize) {
				win_eof = true;
				break;
			}
		}
		if (rc < 0 || sent_win == 0)
			break;

		bool advanced = false;
		for (int attempt = 0; attempt < TFTP_MAX_RETRIES; attempt++) {
			uint8_t rx[512];
			ssize_t rlen = recv_pkt_timeout(xfer_fd, rx, sizeof(rx), (int)opts->timeout_ms);
			if (rlen == -ETIMEDOUT) {
				/* Retransmit current window */
				uint64_t r_off = base_off;
				for (uint32_t w = 0; w < sent_win; w++) {
					uint64_t rem = (sent_bytes + (r_off - base_off) < avail)
							   ? (avail - (sent_bytes + (r_off - base_off)))
							   : 0;
					size_t want = rem < opts->blksize ? (size_t)rem : (size_t)opts->blksize;
					ssize_t nr = want > 0 ? pread(ffd, pkt + 4, want, (off_t)r_off) : 0;
					if (nr < 0) {
						rc = -errno;
						break;
					}
					put_be16(pkt, TFTP_OP_DATA);
					put_be16(pkt + 2, (uint16_t)(base_block + w));
					(void)send_pkt(xfer_fd, pkt, 4u + (size_t)nr, peer, peer_len);
					r_off += (uint64_t)nr;
				}
				continue;
			}
			if (rlen < 4) {
				rc = -EIO;
				break;
			}
			uint16_t op = get_be16(rx);
			uint16_t ack_blk = get_be16(rx + 2);
			if (op == TFTP_OP_ERROR) {
				rc = -ECONNRESET;
				break;
			}
			if (op != TFTP_OP_ACK)
				continue;

			uint16_t rel = (uint16_t)(ack_blk - base_block);
			if (rel < sent_win) {
				uint32_t acked_cnt = (uint32_t)rel + 1u;
				uint64_t delta = (acked_cnt == sent_win)
							 ? (cursor_off - base_off)
							 : ((uint64_t)acked_cnt * opts->blksize);
				sent_bytes += delta;
				base_off += delta;
				base_block = (uint16_t)(ack_blk + 1u);
				if (win_eof && acked_cnt == sent_win)
					done = true;
				advanced = true;
				break;
			}
		}
		if (rc < 0)
			break;
		if (!advanced) {
			rc = -ETIMEDOUT;
			break;
		}
	}

	printf("TQFTP_DONE op=RRQ path=%s bytes=%llu rc=%d\n",
	       raw_path, (unsigned long long)sent_bytes, rc);
	free(pkt);
	close(ffd);
	return rc;
}

static int serve_wrq(int xfer_fd, const struct sockaddr *peer, socklen_t peer_len,
		     const char *local_path, const char *raw_path, struct tqftp_opts *opts)
{
	if (opts->has_unlink) {
		if (unlink(local_path) != 0) {
			int rc = -errno;
			send_tftp_error(xfer_fd, peer, peer_len,
					(uint16_t)err_to_tftp(rc), strerror(-rc));
			printf("TQFTP_DONE op=WRQ path=%s bytes=0 rc=%d action=unlink\n",
			       raw_path, rc);
			return rc;
		}
		if (opts->has_any) {
			uint8_t init_ack[512];
			size_t init_len = build_oack(init_ack, sizeof(init_ack), opts, true, 0);
			if (send_pkt(xfer_fd, init_ack, init_len, peer, peer_len) < 0)
				return -errno;
		} else {
			send_tftp_ack(xfer_fd, peer, peer_len, 0);
		}
		printf("TQFTP_DONE op=WRQ path=%s bytes=0 rc=0 action=unlink\n", raw_path);
		return 0;
	}

	int flags = O_WRONLY | O_CREAT | O_NOFOLLOW;
	if (!opts->has_seek && !opts->has_append)
		flags |= O_TRUNC;
	int ffd = open(local_path, flags, 0644);
	if (ffd < 0) {
		int rc = -errno;
		send_tftp_error(xfer_fd, peer, peer_len, (uint16_t)err_to_tftp(rc), strerror(-rc));
		return rc;
	}
	if (opts->has_append) {
		if (lseek(ffd, 0, SEEK_END) < 0) {
			int rc = -errno;
			close(ffd);
			send_tftp_error(xfer_fd, peer, peer_len, TFTP_ERR_UNDEF, "seek failed");
			return rc;
		}
	} else if (opts->has_seek) {
		if (lseek(ffd, (off_t)opts->seek, SEEK_SET) < 0) {
			int rc = -errno;
			close(ffd);
			send_tftp_error(xfer_fd, peer, peer_len, TFTP_ERR_UNDEF, "seek failed");
			return rc;
		}
	}

	uint8_t *rx = malloc(4u + TFTP_MAX_BLKSIZE + 64u);
	if (!rx) {
		close(ffd);
		return -ENOMEM;
	}

	uint8_t init_ack[512];
	size_t init_len = 0;
	if (opts->has_any) {
		init_len = build_oack(init_ack, sizeof(init_ack), opts, true, opts->tsize);
	} else {
		put_be16(init_ack, TFTP_OP_ACK);
		put_be16(init_ack + 2, 0);
		init_len = 4;
	}
	if (send_pkt(xfer_fd, init_ack, init_len, peer, peer_len) < 0) {
		int rc = -errno;
		free(rx);
		close(ffd);
		return rc;
	}

	uint16_t expected_blk = 1;
	uint32_t win_got = 0;
	uint64_t total_written = 0;
	int retries = 0;
	int rc = 0;
	bool done = false;

	while (!done && !g_stop) {
		ssize_t rlen = recv_pkt_timeout(xfer_fd, rx, 4u + TFTP_MAX_BLKSIZE + 64u,
						(int)opts->timeout_ms);
		if (rlen == -ETIMEDOUT) {
			if (++retries >= TFTP_MAX_RETRIES) {
				rc = -ETIMEDOUT;
				break;
			}
			if (expected_blk == 1 && win_got == 0)
				(void)send_pkt(xfer_fd, init_ack, init_len, peer, peer_len);
			else
				send_tftp_ack(xfer_fd, peer, peer_len, (uint16_t)(expected_blk - 1u));
			continue;
		}
		if (rlen < 4) {
			rc = -EIO;
			break;
		}
		retries = 0;
		uint16_t op = get_be16(rx);
		uint16_t blk = get_be16(rx + 2);
		if (op == TFTP_OP_ERROR) {
			rc = -ECONNRESET;
			break;
		}
		if (op != TFTP_OP_DATA)
			continue;

		size_t dlen = (size_t)rlen - 4u;
		if (dlen > opts->blksize) {
			send_tftp_error(xfer_fd, peer, peer_len, TFTP_ERR_BADOP, "block too large");
			rc = -EMSGSIZE;
			break;
		}
		if (blk != expected_blk) {
			send_tftp_ack(xfer_fd, peer, peer_len, (uint16_t)(expected_blk - 1u));
			continue;
		}

		size_t off = 0;
		while (off < dlen) {
			ssize_t nw = write(ffd, rx + 4 + off, dlen - off);
			if (nw < 0 && errno == EINTR)
				continue;
			if (nw < 0) {
				rc = -errno;
				send_tftp_error(xfer_fd, peer, peer_len, (uint16_t)err_to_tftp(rc),
						strerror(-rc));
				break;
			}
			off += (size_t)nw;
		}
		if (rc < 0)
			break;

		total_written += dlen;
		expected_blk++;
		win_got++;
		if (dlen < opts->blksize || win_got >= opts->wsize) {
			send_tftp_ack(xfer_fd, peer, peer_len, blk);
			win_got = 0;
		}
		if (dlen < opts->blksize)
			done = true;
	}

	printf("TQFTP_DONE op=WRQ path=%s bytes=%llu rc=%d\n",
	       raw_path, (unsigned long long)total_written, rc);
	free(rx);
	close(ffd);
	return rc;
}

static int run_session_on_fd(int xfer_fd, const struct tqftp_config *cfg,
			     const struct sockaddr *peer, socklen_t peer_len,
			     const uint8_t *req_pkt, size_t req_len)
{
	uint16_t op = 0;
	const char *raw_path = NULL;
	const char *mode = NULL;
	struct tqftp_opts opts;

	int prc = parse_req_packet(req_pkt, req_len, &op, &raw_path, &mode, &opts);
	if (prc < 0) {
		send_tftp_error(xfer_fd, peer, peer_len, TFTP_ERR_BADOP, "malformed TFTP request");
		return prc;
	}

	bool is_write = (op == TFTP_OP_WRQ);
	char local_path[PATH_MAX];
	int rrc = resolve_tftp_path(cfg, raw_path, is_write, local_path, sizeof(local_path));
	if (rrc < 0) {
		uint16_t ecode = (uint16_t)err_to_tftp(rrc);
		printf("TQFTP_REJECT op=%s path=%s err=%u\n",
		       is_write ? "WRQ" : "RRQ", raw_path, ecode);
		send_tftp_error(xfer_fd, peer, peer_len, ecode,
				ecode == TFTP_ERR_ACCESS ? "Access violation" : "File not found");
		return rrc;
	}

	printf("TQFTP_REQ op=%s path=%s local=%s blk=%u wsize=%u opts=[%s]\n",
	       is_write ? "WRQ" : "RRQ", raw_path, local_path,
	       opts.blksize, opts.wsize, opts.opt_summary);

	if (is_write)
		return serve_wrq(xfer_fd, peer, peer_len, local_path, raw_path, &opts);
	return serve_rrq(xfer_fd, peer, peer_len, local_path, raw_path, &opts);
}

static void *session_thread(void *arg)
{
	struct tqftp_session_req *s = (struct tqftp_session_req *)arg;
	int fd = socket(s->sock_family, SOCK_DGRAM, 0);
	if (fd >= 0) {
		struct sockaddr_qrtr local = {0};
		socklen_t sl = sizeof(local);
		if (s->sock_family != AF_QIPCRTR ||
		    getsockname(fd, (struct sockaddr *)&local, &sl) == 0) {
			(void)run_session_on_fd(fd, s->cfg,
						(const struct sockaddr *)&s->peer,
						s->peer_len, s->req_pkt, s->req_len);
		}
		close(fd);
	}
	free(s);
	pthread_mutex_lock(&g_worker_mu);
	if (g_active_workers > 0)
		g_active_workers--;
	pthread_mutex_unlock(&g_worker_mu);
	return NULL;
}

static int qrtr_publish_ctrl(int fd, uint32_t cmd, uint32_t service, uint32_t instance,
			     uint32_t node, uint32_t port)
{
	struct qrtr_ctrl_pkt pkt = {0};
	uint8_t *raw = (uint8_t *)&pkt;
	struct sockaddr_qrtr dst = {
		.sq_family = AF_QIPCRTR,
		.sq_node = node,
		.sq_port = QRTR_PORT_CTRL,
	};
	put_le32(raw + offsetof(struct qrtr_ctrl_pkt, cmd), cmd);
	put_le32(raw + offsetof(struct qrtr_ctrl_pkt, server.service), service);
	put_le32(raw + offsetof(struct qrtr_ctrl_pkt, server.instance), instance);
	put_le32(raw + offsetof(struct qrtr_ctrl_pkt, server.node), node);
	put_le32(raw + offsetof(struct qrtr_ctrl_pkt, server.port), port);
	if (sendto(fd, raw, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
		return -errno;
	return 0;
}

/* ---- Offline unit test suite (socketpair, zero QRTR/hardware dependency) ---- */
static size_t build_test_req(uint8_t *buf, uint16_t op, const char *path,
			     const char *k1, const char *v1, const char *k2, const char *v2)
{
	put_be16(buf, op);
	size_t pos = 2;
	size_t n = strlen(path) + 1;
	memcpy(buf + pos, path, n);
	pos += n;
	memcpy(buf + pos, "octet", 6);
	pos += 6;
	if (k1 && v1) {
		n = strlen(k1) + 1;
		memcpy(buf + pos, k1, n);
		pos += n;
		n = strlen(v1) + 1;
		memcpy(buf + pos, v1, n);
		pos += n;
	}
	if (k2 && v2) {
		n = strlen(k2) + 1;
		memcpy(buf + pos, k2, n);
		pos += n;
		n = strlen(v2) + 1;
		memcpy(buf + pos, v2, n);
		pos += n;
	}
	return pos;
}

static void rm_rf_test(const char *p)
{
	char cmd[PATH_MAX + 32];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", p);
	int rc = system(cmd);
	assert(rc == 0);
}

struct client_test_args {
	int cli_fd;
	uint8_t req[512];
	size_t req_len;
	int mode; /* 0 = RRQ+OACK, 1 = WRQ+OACK, 2 = expect ERROR, 3 = RRQ stat+ERROR */
	uint16_t expected_err;
	uint8_t oack_buf[512];
	size_t oack_len;
	char payload[128];
	size_t payload_len;
};

static bool oack_has_kv(const uint8_t *oack, size_t len, const char *key, const char *val)
{
	if (len < 2 || get_be16(oack) != TFTP_OP_OACK)
		return false;
	size_t pos = 2;
	while (pos < len) {
		const char *k = (const char *)(oack + pos);
		size_t kl = strnlen(k, len - pos);
		if (kl == 0 || pos + kl >= len)
			break;
		pos += kl + 1;
		const char *v = (const char *)(oack + pos);
		size_t vl = strnlen(v, len - pos);
		if (pos + vl >= len)
			break;
		pos += vl + 1;
		if (strcasecmp(k, key) == 0 && strcmp(v, val) == 0)
			return true;
	}
	return false;
}

static size_t build_test_req_kv(uint8_t *buf, uint16_t op, const char *path,
				const char **kvs, size_t num_kvs)
{
	put_be16(buf, op);
	size_t pos = 2;
	size_t n = strlen(path) + 1;
	memcpy(buf + pos, path, n);
	pos += n;
	memcpy(buf + pos, "octet", 6);
	pos += 6;
	for (size_t i = 0; i < num_kvs; i++) {
		n = strlen(kvs[i]) + 1;
		memcpy(buf + pos, kvs[i], n);
		pos += n;
	}
	return pos;
}

static void *test_client_thread(void *arg)
{
	struct client_test_args *a = (struct client_test_args *)arg;
	uint8_t buf[2048];
	ssize_t n;

	if (a->mode == 2) {
		n = recv(a->cli_fd, buf, sizeof(buf), 0);
		assert(n >= 5);
		assert(get_be16(buf) == TFTP_OP_ERROR);
		assert(get_be16(buf + 2) == a->expected_err);
		return NULL;
	}

	if (a->mode == 0 || a->mode == 3) {
		/* Expect OACK -> send ACK(0) (or ERROR for mode 3) -> receive DATA(1) -> send ACK(1) */
		n = recv(a->cli_fd, buf, sizeof(buf), 0);
		assert(n > 4 && get_be16(buf) == TFTP_OP_OACK);
		assert((size_t)n <= sizeof(a->oack_buf));
		memcpy(a->oack_buf, buf, (size_t)n);
		a->oack_len = (size_t)n;

		if (a->mode == 3) {
			uint8_t errp[16];
			put_be16(errp, TFTP_OP_ERROR);
			put_be16(errp + 2, 0);
			memcpy(errp + 4, "stat", 5);
			assert(send(a->cli_fd, errp, 9, 0) == 9);
			return NULL;
		}

		uint8_t ack[4];
		put_be16(ack, TFTP_OP_ACK);
		put_be16(ack + 2, 0);
		assert(send(a->cli_fd, ack, 4, 0) == 4);

		n = recv(a->cli_fd, buf, sizeof(buf), 0);
		assert(n >= 4 && get_be16(buf) == TFTP_OP_DATA);
		assert(get_be16(buf + 2) == 1);
		a->payload_len = (size_t)n - 4u;
		assert(a->payload_len < sizeof(a->payload));
		if (a->payload_len > 0)
			memcpy(a->payload, buf + 4, a->payload_len);
		a->payload[a->payload_len] = '\0';
		put_be16(ack + 2, 1);
		assert(send(a->cli_fd, ack, 4, 0) == 4);
		return NULL;
	}

	if (a->mode == 1) {
		/* Expect OACK -> send DATA(1) -> expect ACK(1) */
		n = recv(a->cli_fd, buf, sizeof(buf), 0);
		assert(n > 4 && get_be16(buf) == TFTP_OP_OACK);
		assert((size_t)n <= sizeof(a->oack_buf));
		memcpy(a->oack_buf, buf, (size_t)n);
		a->oack_len = (size_t)n;

		uint8_t data[256];
		put_be16(data, TFTP_OP_DATA);
		put_be16(data + 2, 1);
		if (a->payload_len > 0)
			memcpy(data + 4, a->payload, a->payload_len);
		assert(send(a->cli_fd, data, 4 + a->payload_len, 0) == (ssize_t)(4 + a->payload_len));

		n = recv(a->cli_fd, buf, sizeof(buf), 0);
		assert(n == 4 && get_be16(buf) == TFTP_OP_ACK && get_be16(buf + 2) == 1);
	}

	if (a->mode == 4) {
		/* Expect OACK only (e.g. WRQ with unlink=0) */
		n = recv(a->cli_fd, buf, sizeof(buf), 0);
		assert(n > 4 && get_be16(buf) == TFTP_OP_OACK);
		assert((size_t)n <= sizeof(a->oack_buf));
		memcpy(a->oack_buf, buf, (size_t)n);
		a->oack_len = (size_t)n;
	}
	return NULL;
}

static int run_self_test(void)
{
	char tmpl[] = "/tmp/tqftp_test_XXXXXX";
	char *tmp = mkdtemp(tmpl);
	assert(tmp != NULL);

	char root_dir[PATH_MAX], ro_dir[PATH_MAX], ro_dir2[PATH_MAX], cmd[PATH_MAX + 64], out[PATH_MAX];
	snprintf(root_dir, sizeof(root_dir), "%s/ram", tmp);
	snprintf(ro_dir, sizeof(ro_dir), "%s/ro", tmp);
	snprintf(ro_dir2, sizeof(ro_dir2), "%s/ro2", tmp);
	assert(mkdir(root_dir, 0755) == 0);
	assert(mkdir(ro_dir, 0755) == 0);
	assert(mkdir(ro_dir2, 0755) == 0);
	assert(mkdir_p_under_root(root_dir, root_dir, "rfs/msm/mpss/server_check.txt") == 0);
	assert(mkdir_p_under_root(root_dir, root_dir, "rfs/shared/server_info.txt") == 0);
	assert(mkdir_p_under_root(ro_dir, ro_dir, "configs/mcfg_hw/mbn_hw.dig") == 0);

	snprintf(cmd, sizeof(cmd), "%s/rfs/msm/mpss/server_check.txt", root_dir);
	int fd = open(cmd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, "hello", 5) == 5);
	close(fd);

	snprintf(cmd, sizeof(cmd), "%s/rfs/shared/server_info.txt", root_dir);
	fd = open(cmd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, "TFTP_SERVER.1.0", 15) == 15);
	close(fd);

	snprintf(cmd, sizeof(cmd), "%s/mcfg_sw.mbn", ro_dir);
	fd = open(cmd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, "MCFGDATA", 8) == 8);
	close(fd);

	snprintf(cmd, sizeof(cmd), "%s/configs/mcfg_hw/mbn_hw.dig", ro_dir);
	fd = open(cmd, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, "DIGEST_HW_32_BYTES_0123456789ABC", 32) == 32);
	close(fd);

	struct tqftp_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.root_dir, sizeof(cfg.root_dir), "%s", root_dir);
	assert(realpath(root_dir, cfg.root_real) != NULL);
	snprintf(cfg.ro_dirs[0], sizeof(cfg.ro_dirs[0]), "%s", ro_dir);
	snprintf(cfg.ro_dirs[1], sizeof(cfg.ro_dirs[1]), "%s", ro_dir2);
	cfg.num_ro_dirs = 2;
	compact_ro_dirs(&cfg);
	assert(cfg.num_ro_dirs == 2);
	assert(strcmp(cfg.ro_dirs[0], ro_dir) == 0);
	assert(strcmp(cfg.ro_dirs[1], ro_dir2) == 0);

	/* Path safety + MCFG + Qualcomm virtual namespace checks */
	assert(resolve_tftp_path(&cfg, "../etc/passwd", false, out, sizeof(out)) == -EACCES);
	assert(resolve_tftp_path(&cfg, "/rfs/../../escape", true, out, sizeof(out)) == -EACCES);
	assert(resolve_tftp_path(&cfg, "/readonly/mcfg_sw.mbn", true, out, sizeof(out)) == -EACCES);
	assert(resolve_tftp_path(&cfg, "/readonly/vendor/firmware_mnt/image/modem_pr/mcfg/mcfg_sw.mbn",
				 false, out, sizeof(out)) == 0);
	assert(resolve_tftp_path(&cfg, "/readonly/firmware/image/modem_pr/mcfg/configs/mcfg_hw/mbn_hw.dig",
				 false, out, sizeof(out)) == 0);
	assert(resolve_tftp_path(&cfg, "/hlos_rfs/msm/mpss/server_check.txt", false, out, sizeof(out)) == 0);
	assert(resolve_tftp_path(&cfg, "/shared/server_info.txt", false, out, sizeof(out)) == 0);
	assert(strstr(out, "/rfs/shared/server_info.txt") != NULL);
	assert(resolve_tftp_path(&cfg, "/readwrite/mcfg.tmp", true, out, sizeof(out)) == 0);
	assert(strstr(out, "/rfs/msm/mpss/mcfg.tmp") != NULL);
	assert(resolve_tftp_path(&cfg, "/readwrite/server_check.txt", false, out, sizeof(out)) == 0);
	assert(strstr(out, "/rfs/msm/mpss/server_check.txt") != NULL);

	/* Protocol roundtrips over UNIX DGRAM socketpair */
	int sp[2];
	assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) == 0);
	pthread_t th;

	/* 1. RRQ + rsize=0 must return actual file size (5), NOT 0 */
	struct client_test_args ca;
	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 0;
	ca.req_len = build_test_req(ca.req, TFTP_OP_RRQ, "/readwrite/server_check.txt",
				    "blksize", "512", "rsize", "0");
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == 0);
	assert(pthread_join(th, NULL) == 0);
	assert(ca.payload_len == 5 && memcmp(ca.payload, "hello", 5) == 0);
	assert(oack_has_kv(ca.oack_buf, ca.oack_len, "rsize", "5"));

	/* 2. 0-byte WRQ then 1-byte WRQ to /readwrite/mcfg.tmp then RRQ with rsize=4096 & timeoutms=1000 */
	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 1;
	ca.payload_len = 0;
	const char *wrq0_kvs[] = { "blksize", "7680", "timeoutms", "1000", "wsize", "10" };
	ca.req_len = build_test_req_kv(ca.req, TFTP_OP_WRQ, "/readwrite/mcfg.tmp", wrq0_kvs, 6);
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == 0);
	assert(pthread_join(th, NULL) == 0);
	assert(oack_has_kv(ca.oack_buf, ca.oack_len, "timeoutms", "1000"));

	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 1;
	ca.payload[0] = 'X';
	ca.payload_len = 1;
	ca.req_len = build_test_req(ca.req, TFTP_OP_WRQ, "/readwrite/mcfg.tmp",
				    "blksize", "7680", "wsize", "10");
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == 0);
	assert(pthread_join(th, NULL) == 0);

	char verify_buf[32] = {0};
	snprintf(cmd, sizeof(cmd), "%s/rfs/msm/mpss/mcfg.tmp", root_dir);
	fd = open(cmd, O_RDONLY);
	assert(fd >= 0);
	assert(read(fd, verify_buf, sizeof(verify_buf)) == 1 && verify_buf[0] == 'X');
	close(fd);

	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 0;
	const char *rrq_kvs[] = { "blksize", "7680", "timeoutms", "1000", "rsize", "4096" };
	ca.req_len = build_test_req_kv(ca.req, TFTP_OP_RRQ, "/readwrite/mcfg.tmp", rrq_kvs, 6);
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == 0);
	assert(pthread_join(th, NULL) == 0);
	assert(ca.payload_len == 1 && ca.payload[0] == 'X');
	assert(oack_has_kv(ca.oack_buf, ca.oack_len, "rsize", "1"));
	assert(oack_has_kv(ca.oack_buf, ca.oack_len, "timeoutms", "1000"));

	/* 3. RRQ rfs_stat pattern where peer sends ERROR after OACK */
	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 3;
	ca.req_len = build_test_req(ca.req, TFTP_OP_RRQ, "/readonly/firmware/image/modem_pr/mcfg/configs/mcfg_hw/mbn_hw.dig",
				    "rsize", "0", "tsize", "0");
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == -ECONNRESET);
	assert(pthread_join(th, NULL) == 0);
	assert(oack_has_kv(ca.oack_buf, ca.oack_len, "rsize", "32"));
	assert(oack_has_kv(ca.oack_buf, ca.oack_len, "tsize", "32"));

	/* 4. WRQ into /rfs/msm/mpss/nv_cache.bin (RAM overlay only) */
	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 1;
	memcpy(ca.payload, "RAM_ONLY_WRITE", 14);
	ca.payload_len = 14;
	ca.req_len = build_test_req(ca.req, TFTP_OP_WRQ, "/rfs/msm/mpss/nv_cache.bin",
				    "blksize", "1024", "wsize", "4");
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == 0);
	assert(pthread_join(th, NULL) == 0);

	snprintf(cmd, sizeof(cmd), "%s/rfs/msm/mpss/nv_cache.bin", root_dir);
	fd = open(cmd, O_RDONLY);
	assert(fd >= 0);
	assert(read(fd, verify_buf, sizeof(verify_buf)) == 14);
	close(fd);
	assert(memcmp(verify_buf, "RAM_ONLY_WRITE", 14) == 0);

	/* 5. WRQ targeting /readonly/ must emit TFTP ERROR 2 */
	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 2;
	ca.expected_err = TFTP_ERR_ACCESS;
	ca.req_len = build_test_req(ca.req, TFTP_OP_WRQ, "/readonly/mcfg_sw.mbn", NULL, NULL, NULL, NULL);
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == -EACCES);
	assert(pthread_join(th, NULL) == 0);

	/* 6. WRQ with unlink=0 removes file in RAM overlay, returns ENOENT if absent, rejects /readonly/ */
	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 1;
	ca.payload[0] = 'U';
	ca.payload_len = 1;
	ca.req_len = build_test_req(ca.req, TFTP_OP_WRQ, "/readwrite/mcfg_unlink.tmp", "blksize", "512", "wsize", "1");
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == 0);
	assert(pthread_join(th, NULL) == 0);

	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 4;
	const char *unlink_kvs[] = { "timeoutms", "500", "unlink", "0" };
	ca.req_len = build_test_req_kv(ca.req, TFTP_OP_WRQ, "/readwrite/mcfg_unlink.tmp", unlink_kvs, 4);
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == 0);
	assert(pthread_join(th, NULL) == 0);
	assert(oack_has_kv(ca.oack_buf, ca.oack_len, "unlink", "0"));
	assert(oack_has_kv(ca.oack_buf, ca.oack_len, "timeoutms", "500"));
	snprintf(cmd, sizeof(cmd), "%s/rfs/msm/mpss/mcfg_unlink.tmp", root_dir);
	struct stat ust;
	assert(stat(cmd, &ust) != 0 && errno == ENOENT);

	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 2;
	ca.expected_err = TFTP_ERR_ENOTFOUND;
	ca.req_len = build_test_req_kv(ca.req, TFTP_OP_WRQ, "/readwrite/mcfg_unlink.tmp", unlink_kvs, 4);
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == -ENOENT);
	assert(pthread_join(th, NULL) == 0);

	memset(&ca, 0, sizeof(ca));
	ca.cli_fd = sp[1];
	ca.mode = 2;
	ca.expected_err = TFTP_ERR_ACCESS;
	ca.req_len = build_test_req_kv(ca.req, TFTP_OP_WRQ, "/readonly/mcfg_sw.mbn", unlink_kvs, 4);
	assert(pthread_create(&th, NULL, test_client_thread, &ca) == 0);
	assert(run_session_on_fd(sp[0], &cfg, NULL, 0, ca.req, ca.req_len) == -EACCES);
	assert(pthread_join(th, NULL) == 0);
	snprintf(cmd, sizeof(cmd), "%s/mcfg_sw.mbn", ro_dir);
	assert(stat(cmd, &ust) == 0);

	close(sp[0]);
	close(sp[1]);
	rm_rf_test(tmp);
	puts("TQFTPSERV_SELF_TEST_OK=1");
	return 0;
}

int main(int argc, char **argv)
{
	struct tqftp_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.root_dir, sizeof(cfg.root_dir), "%s", "/run/marble-rfs-ram");

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--self-test") == 0)
			return run_self_test();
		if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
			snprintf(cfg.root_dir, sizeof(cfg.root_dir), "%s", argv[++i]);
		} else if (strcmp(argv[i], "--ro-dir") == 0 && i + 1 < argc) {
			if (cfg.num_ro_dirs < TFTP_MAX_RO_DIRS) {
				snprintf(cfg.ro_dirs[cfg.num_ro_dirs], sizeof(cfg.ro_dirs[0]),
					 "%s", argv[++i]);
				cfg.num_ro_dirs++;
			} else {
				i++;
			}
		} else {
			fprintf(stderr, "usage: %s [--self-test] [--root DIR] [--ro-dir DIR]\n",
				argv[0]);
			return 2;
		}
	}

	if (!realpath(cfg.root_dir, cfg.root_real)) {
		fprintf(stderr, "TQFTPSERV_ERR invalid root %s: %s\n", cfg.root_dir, strerror(errno));
		return 1;
	}
	compact_ro_dirs(&cfg);

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	static const uint32_t kInstances[3] = { 0x0001u, 0x0101u, 0x0000u };
	int fds[3] = { -1, -1, -1 };
	struct sockaddr_qrtr locals[3] = {0};
	int rcs[3] = { -1, -1, -1 };

	for (size_t i = 0; i < 3; i++) {
		fds[i] = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
		if (fds[i] < 0) {
			fprintf(stderr, "TQFTPSERV_ERR socket[%zu](AF_QIPCRTR): %s\n", i, strerror(errno));
			for (size_t j = 0; j < i; j++)
				close(fds[j]);
			return 1;
		}
		socklen_t sl = sizeof(locals[i]);
		if (getsockname(fds[i], (struct sockaddr *)&locals[i], &sl) < 0 ||
		    locals[i].sq_family != AF_QIPCRTR) {
			fprintf(stderr, "TQFTPSERV_ERR getsockname[%zu]: %s\n", i, strerror(errno));
			for (size_t j = 0; j <= i; j++)
				close(fds[j]);
			return 1;
		}
		rcs[i] = qrtr_publish_ctrl(fds[i], QRTR_TYPE_NEW_SERVER, TFTP_QMI_SERVICE,
					   kInstances[i], locals[i].sq_node, locals[i].sq_port);
		if (rcs[i] == 0) {
			socklen_t sl2 = sizeof(locals[i]);
			(void)getsockname(fds[i], (struct sockaddr *)&locals[i], &sl2);
		}
	}
	if (rcs[0] < 0 && rcs[1] < 0 && rcs[2] < 0) {
		fprintf(stderr, "TQFTPSERV_ERR publish 0x1000 failed: %s\n", strerror(-rcs[0]));
		for (size_t i = 0; i < 3; i++)
			close(fds[i]);
		return 1;
	}

	printf("TQFTPSERV_READY root=%s ro_dirs=%zu node=%u ports=%u,%u,%u inst1=%d inst101=%d inst0=%d\n",
	       cfg.root_dir, cfg.num_ro_dirs, locals[0].sq_node,
	       locals[0].sq_port, locals[1].sq_port, locals[2].sq_port,
	       rcs[0] == 0 ? 1 : 0, rcs[1] == 0 ? 1 : 0, rcs[2] == 0 ? 1 : 0);

	while (!g_stop) {
		struct pollfd pfds[3] = {
			{ .fd = fds[0], .events = POLLIN },
			{ .fd = fds[1], .events = POLLIN },
			{ .fd = fds[2], .events = POLLIN },
		};
		int prc = poll(pfds, 3, 1000);
		if (prc < 0 && errno == EINTR)
			continue;
		if (prc < 0) {
			fprintf(stderr, "TQFTPSERV_ERR poll: %s\n", strerror(errno));
			break;
		}
		if (prc == 0)
			continue;

		for (size_t idx = 0; idx < 3; idx++) {
			if (!(pfds[idx].revents & POLLIN))
				continue;
			struct tqftp_session_req *s = calloc(1, sizeof(*s));
			if (!s)
				continue;
			s->cfg = &cfg;
			s->sock_family = AF_QIPCRTR;
			s->peer_len = sizeof(s->peer);
			ssize_t n = recvfrom(fds[idx], s->req_pkt, sizeof(s->req_pkt), MSG_DONTWAIT,
					     (struct sockaddr *)&s->peer, &s->peer_len);
			if (n <= 0) {
				free(s);
				continue;
			}
			struct sockaddr_qrtr *qpeer = (struct sockaddr_qrtr *)&s->peer;
			if (s->peer_len >= sizeof(struct sockaddr_qrtr) && qpeer->sq_port == QRTR_PORT_CTRL) {
				free(s);
				continue;
			}
			s->req_len = (size_t)n;

			pthread_mutex_lock(&g_worker_mu);
			if (g_active_workers >= TFTP_MAX_WORKERS) {
				pthread_mutex_unlock(&g_worker_mu);
				send_tftp_error(fds[idx], (struct sockaddr *)&s->peer, s->peer_len,
						TFTP_ERR_UNDEF, "server busy");
				free(s);
				continue;
			}
			g_active_workers++;
			pthread_mutex_unlock(&g_worker_mu);

			pthread_attr_t attr;
			pthread_t th;
			pthread_attr_init(&attr);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			pthread_attr_setstacksize(&attr, 256u * 1024u);
			if (pthread_create(&th, &attr, session_thread, s) != 0) {
				pthread_attr_destroy(&attr);
				pthread_mutex_lock(&g_worker_mu);
				if (g_active_workers > 0)
					g_active_workers--;
				pthread_mutex_unlock(&g_worker_mu);
				free(s);
				continue;
			}
			pthread_attr_destroy(&attr);
		}
	}

	for (size_t i = 0; i < 3; i++) {
		(void)qrtr_publish_ctrl(fds[i], QRTR_TYPE_DEL_SERVER, TFTP_QMI_SERVICE,
					kInstances[i], locals[i].sq_node, locals[i].sq_port);
		close(fds[i]);
	}
	puts("TQFTPSERV_STOP=1");
	return 0;
}
