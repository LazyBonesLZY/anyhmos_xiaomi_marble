/* SPDX-License-Identifier: MIT
 * Read-only QMI-over-QRTR modem probe for Xiaomi marble (Batch F1).
 * Exercises only GET_* read-only queries (DMS/UIM/NAS) and QRTR nameserver lookup.
 * Never outputs raw IMEI, never transmits RF frames, never places calls or SMS.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/qrtr.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#define QMI_HDR_LEN 7
#define QMI_MSG_REQ 0x00
#define QMI_MSG_RESP 0x02
#define QMI_MSG_IND 0x04
#define QMI_MAX_BUF 4096

#define SVC_WDS 0x01
#define SVC_DMS 0x02
#define SVC_NAS 0x03
#define SVC_WMS 0x05
#define SVC_VOICE 0x09
#define SVC_UIM 0x0b
#define SVC_RMTFS 0x0e
#define SVC_LOC 0x10
#define SVC_PDC 0x24
#define SVC_WDA 0x1a
#define SVC_DPM 0x2f
#define SVC_MEMSHARE 0x34
#define SVC_SERVREG_LOC 0x40
#define SVC_TFTP 0x1000

#define QMI_DPM_OPEN_PORT 0x0020

#define QMI_WDS_START_NETWORK_INTERFACE 0x0020
#define QMI_WDS_STOP_NETWORK_INTERFACE 0x0021
#define QMI_WDS_GET_PKT_SRVC_STATUS 0x0022
#define QMI_WDS_GET_PROFILE_LIST 0x002a
#define QMI_WDS_GET_PROFILE_SETTINGS 0x002b
#define QMI_WDS_GET_CURRENT_SETTINGS 0x002d
#define QMI_WDS_SET_CLIENT_IP_FAMILY_PREF 0x004d
#define QMI_WDS_BIND_SUBSCRIPTION 0x00af
#define QMI_WDS_BIND_MUX_DATA_PORT 0x00a2

#define QMI_WDA_SET_DATA_FORMAT 0x0020
#define QMI_WDA_GET_DATA_FORMAT 0x0021

#define QMI_DMS_GET_DEVICE_CAP 0x0020
#define QMI_DMS_GET_MODEL_ID 0x0022
#define QMI_DMS_GET_REV_ID 0x0023
#define QMI_DMS_GET_SERIAL_NUMS 0x0025
#define QMI_DMS_GET_OP_MODE 0x002d
#define QMI_DMS_GET_OPERATING_MODE 0x002d
#define QMI_DMS_SET_OP_MODE 0x002e
#define QMI_DMS_GET_ACTIVATION_STATE 0x002f
#define QMI_DMS_GET_BAND_CAPABILITY 0x0045

#define QMI_UIM_READ_TRANSPARENT 0x0020
#define QMI_UIM_GET_FILE_ATTRIBUTES 0x0024
#define QMI_UIM_GET_CARD_STATUS 0x002f
#define QMI_UIM_CHANGE_PROV_SESSION 0x0038
#define QMI_UIM_GET_SLOT_STATUS 0x0047

#define QMI_NAS_GET_SUPPORTED_MESSAGES 0x001e
#define QMI_NAS_GET_SERVING_SYSTEM 0x0024
#define QMI_NAS_GET_HOME_NETWORK 0x0025
#define QMI_NAS_GET_RF_BAND_INFO 0x0031
#define QMI_NAS_SET_SYS_SEL_PREF 0x0033
#define QMI_NAS_GET_SYS_SEL_PREF 0x0034
#define QMI_NAS_BIND_SUBSCRIPTION 0x0045
#define QMI_NAS_SET_DUAL_STANDBY_PREF 0x004b
#define QMI_NAS_GET_SYS_INFO 0x004d
#define QMI_NAS_GET_SIGNAL_INFO 0x004f
#define QMI_NAS_GET_DUAL_STANDBY_PREF 0x005c

#define QMI_PDC_GET_SELECTED_CONFIG 0x0022
#define QMI_PDC_GET_CONFIG_INFO 0x0023
#define QMI_PDC_LIST_CONFIGS 0x0024
#define QMI_PDC_SELECT_CONFIG 0x0025
#define QMI_PDC_ACTIVATE_CONFIG 0x0026

#define QMI_LOC_INFORM_CLIENT_REVISION 0x0020
#define QMI_LOC_REG_EVENTS 0x0021
#define QMI_LOC_START 0x0022
#define QMI_LOC_STOP 0x0023
#define QMI_LOC_POSITION_REPORT_IND 0x0024
#define QMI_LOC_GNSS_SV_INFO_IND 0x0025
#define QMI_LOC_NMEA_IND 0x0026
#define QMI_LOC_ENGINE_STATE_IND 0x002b
#define QMI_LOC_GET_SERVICE_REVISION 0x0032
#define QMI_LOC_SET_NMEA_TYPES 0x003e

#define MAX_EPS 64
#define FIELD_OFF(f) offsetof(struct qrtr_ctrl_pkt, f)

struct endpoint {
	uint32_t node;
	uint32_t port;
	uint32_t instance;
};

struct qrtr_client {
	int fd;
	struct sockaddr_qrtr local;
};

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
	return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static int16_t get_i16(const uint8_t *p)
{
	return (int16_t)get_le16(p);
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	put_le16(p, (uint16_t)(v & 0xffff));
	put_le16(p + 2, (uint16_t)(v >> 16));
}

static int64_t now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return -1;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int qrtr_open_ephemeral(struct qrtr_client *c)
{
	socklen_t sl = sizeof(c->local);
	c->fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (c->fd < 0)
		return -errno;
	if (getsockname(c->fd, (struct sockaddr *)&c->local, &sl) < 0 ||
	    sl != sizeof(c->local) || c->local.sq_family != AF_QIPCRTR) {
		int err = errno ? -errno : -EPROTO;
		close(c->fd);
		c->fd = -1;
		return err;
	}
	c->local.sq_port = 0;
	if (bind(c->fd, (struct sockaddr *)&c->local, sizeof(c->local)) < 0) {
		int err = -errno;
		close(c->fd);
		c->fd = -1;
		return err;
	}
	return 0;
}

static int wait_poll(int fd, int64_t deadline)
{
	for (;;) {
		int64_t t = now_ms();
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int rc;
		if (t < 0)
			return -errno;
		if (t >= deadline)
			return -ETIMEDOUT;
		rc = poll(&pfd, 1, (int)(deadline - t));
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0)
			return -errno;
		if (rc == 0)
			return -ETIMEDOUT;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			return -EIO;
		if (pfd.revents & POLLIN)
			return 0;
	}
}

static int lookup_service(struct qrtr_client *c, uint32_t svc, struct endpoint *eps, size_t max_eps,
			  size_t *out_count)
{
	uint8_t req[sizeof(struct qrtr_ctrl_pkt)] = {0};
	struct sockaddr_qrtr dst = {
		.sq_family = AF_QIPCRTR,
		.sq_node = c->local.sq_node,
		.sq_port = QRTR_PORT_CTRL,
	};
	int64_t deadline = now_ms() + 1000;
	size_t count = 0;
	bool done = false;

	put_le32(req + FIELD_OFF(cmd), QRTR_TYPE_NEW_LOOKUP);
	put_le32(req + FIELD_OFF(server.service), svc);
	if (sendto(c->fd, req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
		return -errno;

	while (!done) {
		uint8_t buf[sizeof(struct qrtr_ctrl_pkt)];
		struct sockaddr_qrtr from = {0};
		socklen_t sl = sizeof(from);
		ssize_t n;
		int rc = wait_poll(c->fd, deadline);
		if (rc < 0)
			return rc;
		n = recvfrom(c->fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &sl);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return -errno;
		}
		if (sl != sizeof(from) || from.sq_node != c->local.sq_node ||
		    from.sq_port != QRTR_PORT_CTRL || (size_t)n != sizeof(struct qrtr_ctrl_pkt))
			continue;
		uint32_t cmd = get_le32(buf + FIELD_OFF(cmd));
		if (cmd != QRTR_TYPE_NEW_SERVER)
			continue;
		uint32_t id = get_le32(buf + FIELD_OFF(server.service));
		uint32_t inst = get_le32(buf + FIELD_OFF(server.instance));
		uint32_t node = get_le32(buf + FIELD_OFF(server.node));
		uint32_t port = get_le32(buf + FIELD_OFF(server.port));
		if (!id && !inst && !node && !port) {
			done = true;
			break;
		}
		if ((svc == 0 || id == svc) && count < max_eps) {
			eps[count].node = node;
			eps[count].port = port;
			eps[count].instance = inst;
			count++;
		}
	}

	put_le32(req + FIELD_OFF(cmd), QRTR_TYPE_DEL_LOOKUP);
	(void)sendto(c->fd, req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst));
	*out_count = count;
	return 0;
}

static bool find_remote_endpoint(struct qrtr_client *c, uint32_t svc, struct endpoint *out)
{
	struct endpoint eps[MAX_EPS];
	size_t n = 0;
	if (lookup_service(c, svc, eps, MAX_EPS, &n) < 0)
		return false;
	for (size_t i = 0; i < n; i++) {
		if (eps[i].node != c->local.sq_node && eps[i].port && eps[i].port != QRTR_PORT_CTRL) {
			*out = eps[i];
			return true;
		}
	}
	return false;
}

static int qmi_find_tlv(const uint8_t *msg, size_t msg_len, uint8_t tlv_id,
			const uint8_t **val, uint16_t *vlen)
{
	size_t pos;
	if (msg_len < QMI_HDR_LEN)
		return -EPROTO;
	if ((size_t)get_le16(msg + 5) != msg_len - QMI_HDR_LEN)
		return -EPROTO;
	pos = QMI_HDR_LEN;
	while (pos < msg_len) {
		uint8_t id;
		uint16_t len;
		if (msg_len - pos < 3)
			return -EPROTO;
		id = msg[pos];
		len = get_le16(msg + pos + 1);
		if (len > msg_len - pos - 3)
			return -EPROTO;
		if (id == tlv_id) {
			*val = msg + pos + 3;
			*vlen = len;
			return 0;
		}
		pos += 3U + len;
	}
	return -ENOENT;
}

static int qmi_check_result_tlv(const uint8_t *msg, size_t msg_len, uint16_t *out_err)
{
	const uint8_t *v = NULL;
	uint16_t vlen = 0;
	int rc = qmi_find_tlv(msg, msg_len, 0x02, &v, &vlen);
	if (rc < 0 || vlen != 4)
		return -EPROTO;
	uint16_t res = get_le16(v);
	uint16_t err = get_le16(v + 2);
	*out_err = err;
	if (res > 1 || (res == 0) != (err == 0))
		return -EPROTO;
	return 0;
}

static int qmi_rpc_tlv_ex(struct qrtr_client *c, const struct endpoint *ep, uint16_t txn,
			  uint16_t msg_id, const uint8_t *req_tlvs, size_t req_tlv_len,
			  uint8_t *resp, size_t resp_cap, size_t *resp_len,
			  uint16_t *qmi_err, uint8_t *ind, size_t ind_cap, size_t *ind_len)
{
	uint8_t req[QMI_MAX_BUF];
	struct sockaddr_qrtr dst = {
		.sq_family = AF_QIPCRTR,
		.sq_node = ep->node,
		.sq_port = ep->port,
	};
	int64_t deadline = now_ms() + 3000;
	bool got_resp = false;

	if (req_tlv_len > sizeof(req) - QMI_HDR_LEN)
		return -EMSGSIZE;
	req[0] = QMI_MSG_REQ;
	put_le16(req + 1, txn);
	put_le16(req + 3, msg_id);
	put_le16(req + 5, (uint16_t)req_tlv_len);
	if (req_tlv_len > 0 && req_tlvs)
		memcpy(req + QMI_HDR_LEN, req_tlvs, req_tlv_len);

	if (sendto(c->fd, req, QMI_HDR_LEN + req_tlv_len, 0,
		   (struct sockaddr *)&dst, sizeof(dst)) < 0)
		return -errno;

	if (ind_len)
		*ind_len = 0;

	for (;;) {
		uint8_t tmp[QMI_MAX_BUF];
		struct sockaddr_qrtr from = {0};
		socklen_t sl = sizeof(from);
		int rc = wait_poll(c->fd, deadline);
		if (rc < 0) {
			if (got_resp && ind)
				return 0;
			return rc;
		}
		ssize_t n = recvfrom(c->fd, tmp, sizeof(tmp), MSG_DONTWAIT,
				     (struct sockaddr *)&from, &sl);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return -errno;
		}
		if (sl != sizeof(from) || from.sq_node != ep->node || from.sq_port != ep->port)
			continue;
		if ((size_t)n < QMI_HDR_LEN)
			continue;
		uint8_t type = tmp[0];
		uint16_t r_txn = get_le16(tmp + 1);
		uint16_t r_msg = get_le16(tmp + 3);
		if (!got_resp && type == QMI_MSG_RESP && r_txn == txn && r_msg == msg_id) {
			if ((size_t)n > resp_cap)
				return -EMSGSIZE;
			memcpy(resp, tmp, (size_t)n);
			if (qmi_check_result_tlv(resp, (size_t)n, qmi_err) < 0)
				return -EPROTO;
			*resp_len = (size_t)n;
			if (!ind || *qmi_err != 0)
				return 0;
			got_resp = true;
			deadline = now_ms() + 1500;
			continue;
		}
		if (ind && got_resp && type == QMI_MSG_IND && r_msg == msg_id) {
			if ((size_t)n > ind_cap)
				return -EMSGSIZE;
			memcpy(ind, tmp, (size_t)n);
			*ind_len = (size_t)n;
			return 0;
		}
	}
}

static int qmi_rpc_tlv(struct qrtr_client *c, const struct endpoint *ep, uint16_t txn,
		       uint16_t msg_id, const uint8_t *req_tlvs, size_t req_tlv_len,
		       uint8_t *resp, size_t resp_cap, size_t *resp_len,
		       uint16_t *qmi_err)
{
	return qmi_rpc_tlv_ex(c, ep, txn, msg_id, req_tlvs, req_tlv_len,
			      resp, resp_cap, resp_len, qmi_err, NULL, 0, NULL);
}

static int qmi_rpc(struct qrtr_client *c, const struct endpoint *ep, uint16_t txn,
		   uint16_t msg_id, uint8_t *resp, size_t resp_cap, size_t *resp_len,
		   uint16_t *qmi_err)
{
	return qmi_rpc_tlv(c, ep, txn, msg_id, NULL, 0, resp, resp_cap, resp_len, qmi_err);
}

static size_t tlv_append(uint8_t *dst, uint8_t id, const uint8_t *val, uint16_t len)
{
	dst[0] = id;
	put_le16(dst + 1, len);
	if (len > 0 && val)
		memcpy(dst + 3, val, len);
	return (size_t)len + 3U;
}

static void format_ascii(const uint8_t *src, size_t len, char *out, size_t cap)
{
	size_t j = 0;
	while (len > 0 && src[len - 1] == '\0')
		len--;
	for (size_t i = 0; i < len && j + 1 < cap; i++) {
		out[j++] = (src[i] >= 0x20 && src[i] <= 0x7e) ? (char)src[i] : '.';
	}
	out[j] = '\0';
}

static void format_masked_str(const char *tmp, char *out, size_t cap)
{
	size_t n = strlen(tmp);
	if (n >= 6)
		snprintf(out, cap, "%c%c***%c%c", tmp[0], tmp[1], tmp[n - 2], tmp[n - 1]);
	else if (n > 0)
		snprintf(out, cap, "***");
	else
		snprintf(out, cap, "none");
}

static void format_masked_imei(const uint8_t *src, size_t len, char *out, size_t cap)
{
	char tmp[64];
	format_ascii(src, len, tmp, sizeof(tmp));
	format_masked_str(tmp, out, cap);
	memset(tmp, 0, sizeof(tmp));
}

static int decode_iccid_bcd(const uint8_t *src, size_t src_len, char *out, size_t cap)
{
	size_t j = 0;
	if (!src || src_len == 0 || cap < 2)
		return -EINVAL;
	for (size_t i = 0; i < src_len && j + 1 < cap; i++) {
		uint8_t lo = src[i] & 0x0f;
		uint8_t hi = (src[i] >> 4) & 0x0f;
		if (lo <= 9)
			out[j++] = (char)('0' + lo);
		else if (lo == 0x0f)
			break;
		else
			return -EPROTO;
		if (j + 1 >= cap)
			break;
		if (hi <= 9)
			out[j++] = (char)('0' + hi);
		else if (hi == 0x0f)
			break;
		else
			return -EPROTO;
	}
	out[j] = '\0';
	return j > 0 ? 0 : -EPROTO;
}

static int decode_imsi_plmn(const uint8_t *src, size_t src_len,
			    size_t *out_len, char *mcc_out, char *mnc_out)
{
	char digits[32];
	size_t j = 0;
	if (!src || src_len < 4)
		return -EINVAL;
	uint8_t ef_bytes = src[0];
	if (ef_bytes < 3 || (size_t)ef_bytes + 1 > src_len)
		return -EPROTO;
	uint8_t first = (src[1] >> 4) & 0x0f;
	if (first > 9)
		return -EPROTO;
	digits[j++] = (char)('0' + first);
	for (size_t i = 2; i <= (size_t)ef_bytes && j + 2 < sizeof(digits); i++) {
		uint8_t lo = src[i] & 0x0f;
		uint8_t hi = (src[i] >> 4) & 0x0f;
		if (lo <= 9)
			digits[j++] = (char)('0' + lo);
		else if (lo != 0x0f)
			return -EPROTO;
		if (hi <= 9)
			digits[j++] = (char)('0' + hi);
		else if (hi != 0x0f)
			return -EPROTO;
	}
	digits[j] = '\0';
	if (j < 6)
		return -EPROTO;
	memcpy(mcc_out, digits, 3);
	mcc_out[3] = '\0';
	/* Standard 2-digit MNC (3-digit for NAM MCC 310..316). */
	int mcc_num = atoi(mcc_out);
	size_t mnc_digits = (mcc_num >= 310 && mcc_num <= 316 && j >= 7) ? 3 : 2;
	memcpy(mnc_out, digits + 3, mnc_digits);
	mnc_out[mnc_digits] = '\0';
	*out_len = j;
	memset(digits, 0, sizeof(digits));
	return 0;
}

static const char *qmi_err_name(uint16_t err)
{
	switch (err) {
	case 0x00: return "none";
	case 0x01: return "malformed_msg";
	case 0x03: return "internal";
	case 0x10: return "not_provisioned";
	case 0x11: return "missing_arg";
	case 0x1a: return "no_effect";
	case 0x2e: return "invalid_op";
	case 0x30: return "invalid_arg";
	case 0x34: return "incompatible_state";
	case 0x4a: return "info_unavailable";
	default: return "other";
	}
}

static const char *op_mode_name(uint8_t m)
{
	switch (m) {
	case 0: return "online";
	case 1: return "low_power";
	case 2: return "factory_test";
	case 3: return "offline";
	case 4: return "resetting";
	case 5: return "shutting_down";
	case 6: return "persistent_low_power";
	default: return "unknown";
	}
}

static const char *card_state_name(uint8_t s)
{
	switch (s) {
	case 0: return "absent";
	case 1: return "present";
	case 2: return "error";
	default: return "unknown";
	}
}

static const char *app_type_name(uint8_t t)
{
	switch (t) {
	case 1: return "SIM";
	case 2: return "USIM";
	case 3: return "RUIM";
	case 4: return "CSIM";
	case 5: return "ISIM";
	default: return "UNKNOWN";
	}
}

static const char *app_state_name(uint8_t s)
{
	switch (s) {
	case 0: return "unknown";
	case 1: return "detected";
	case 2: return "pin1_req";
	case 3: return "puk1_req";
	case 4: return "perso";
	case 5: return "pin1_blocked";
	case 6: return "illegal";
	case 7: return "ready";
	default: return "other";
	}
}

static const char *reg_state_name(uint8_t r)
{
	switch (r) {
	case 0: return "not_registered";
	case 1: return "registered";
	case 2: return "not_registered_searching";
	case 3: return "registration_denied";
	case 4: return "unknown";
	default: return "other";
	}
}

static const char *radio_if_name(uint8_t r)
{
	switch (r) {
	case 0: return "none";
	case 1: return "cdma_1x";
	case 2: return "cdma_hdr";
	case 4: return "gsm";
	case 5: return "umts";
	case 8: return "lte";
	case 9: return "tdscdma";
	case 12: return "nr5g";
	default: return "other";
	}
}

static int parse_and_print_dms(const uint8_t *model_msg, size_t model_len,
			       const uint8_t *rev_msg, size_t rev_len,
			       const uint8_t *sn_msg, size_t sn_len,
			       const uint8_t *mode_msg, size_t mode_len,
			       char *out_buf, size_t out_cap)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	char model[128] = "absent", rev[128] = "absent", masked[32] = "none";
	uint8_t op_mode = 255;
	size_t imei_len = 0;

	if (model_msg && qmi_find_tlv(model_msg, model_len, 0x01, &v, &vl) == 0)
		format_ascii(v, vl, model, sizeof(model));
	if (rev_msg && qmi_find_tlv(rev_msg, rev_len, 0x01, &v, &vl) == 0)
		format_ascii(v, vl, rev, sizeof(rev));
	if (sn_msg && qmi_find_tlv(sn_msg, sn_len, 0x11, &v, &vl) == 0) {
		char tmp[64];
		format_ascii(v, vl, tmp, sizeof(tmp));
		imei_len = strlen(tmp);
		format_masked_imei(v, vl, masked, sizeof(masked));
	}
	if (mode_msg && qmi_find_tlv(mode_msg, mode_len, 0x01, &v, &vl) == 0 && vl >= 1)
		op_mode = v[0];

	int used = snprintf(out_buf, out_cap,
			    "DMS_INFO model=\"%s\" rev=\"%s\" imei_present=%d imei_len=%zu masked=%s op_mode=%u(%s)",
			    model, rev, imei_len > 0 ? 1 : 0, imei_len, masked,
			    op_mode, op_mode == 255 ? "absent" : op_mode_name(op_mode));
	if (used > 0 && (size_t)used < out_cap && mode_msg) {
		if (qmi_find_tlv(mode_msg, mode_len, 0x10, &v, &vl) == 0 && vl >= 2)
			used += snprintf(out_buf + used, out_cap - (size_t)used, " offline_reason=0x%04x", get_le16(v));
		if ((size_t)used < out_cap && qmi_find_tlv(mode_msg, mode_len, 0x11, &v, &vl) == 0 && vl >= 1)
			snprintf(out_buf + used, out_cap - (size_t)used, " hw_restricted=%u", v[0]);
	}
	return 0;
}

static int parse_and_print_uim(const uint8_t *msg, size_t msg_len, char *out_buf, size_t out_cap)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	size_t pos = 0, used = 0;

	if (qmi_find_tlv(msg, msg_len, 0x10, &v, &vl) < 0 || vl < 9)
		return -EPROTO;

	uint16_t gw_pri = get_le16(v + 0);
	uint16_t c1x_pri = get_le16(v + 2);
	uint16_t gw_sec = get_le16(v + 4);
	uint16_t c1x_sec = get_le16(v + 6);
	uint8_t num_cards = v[8];
	pos = 9;

	used += (size_t)snprintf(out_buf + used, out_cap - used,
				 "UIM_STATUS gw_pri=0x%04x 1x_pri=0x%04x gw_sec=0x%04x 1x_sec=0x%04x cards=%u\n",
				 gw_pri, c1x_pri, gw_sec, c1x_sec, num_cards);

	for (uint8_t slot = 0; slot < num_cards; slot++) {
		if (vl - pos < 6)
			return -EPROTO;
		uint8_t cstate = v[pos + 0];
		uint8_t err = v[pos + 4];
		uint8_t napps = v[pos + 5];
		pos += 6;
		if (used < out_cap) {
			used += (size_t)snprintf(out_buf + used, out_cap - used,
						 "UIM_CARD slot=%u state=%u(%s) err=%u apps=%u\n",
						 slot, cstate, card_state_name(cstate), err, napps);
		}
		for (uint8_t ai = 0; ai < napps; ai++) {
			if (vl - pos < 7)
				return -EPROTO;
			uint8_t atype = v[pos + 0];
			uint8_t astate = v[pos + 1];
			uint8_t aid_len = v[pos + 6];
			pos += 7;
			if (vl - pos < (size_t)aid_len + 7)
				return -EPROTO;
			pos += aid_len;
			uint8_t pin1_state = v[pos + 1];
			pos += 7;
			if (used < out_cap) {
				used += (size_t)snprintf(out_buf + used, out_cap - used,
							 "UIM_APP slot=%u idx=%u type=%u(%s) state=%u(%s) pin1=%u\n",
							 slot, ai, atype, app_type_name(atype),
							 astate, app_state_name(astate), pin1_state);
			}
		}
	}
	return 0;
}

static int parse_and_print_nas(const uint8_t *ss_msg, size_t ss_len,
			       const uint8_t *sig_msg, size_t sig_len,
			       char *out_buf, size_t out_cap)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	size_t used = 0;

	if (ss_msg && qmi_find_tlv(ss_msg, ss_len, 0x01, &v, &vl) == 0 && vl >= 5) {
		uint8_t reg = v[0], cs = v[1], ps = v[2], net = v[3], nrif = v[4];
		if (vl < 5 + (size_t)nrif)
			return -EPROTO;
		used += (size_t)snprintf(out_buf + used, out_cap - used,
					 "NAS_SERVING reg=%u(%s) cs=%u ps=%u sel=%u ifs=",
					 reg, reg_state_name(reg), cs, ps, net);
		if (nrif == 0 && used < out_cap)
			used += (size_t)snprintf(out_buf + used, out_cap - used, "none");
		for (uint8_t i = 0; i < nrif && used < out_cap; i++) {
			used += (size_t)snprintf(out_buf + used, out_cap - used, "%s%u(%s)",
						 i ? "," : "", v[5 + i], radio_if_name(v[5 + i]));
		}
		if (used < out_cap)
			used += (size_t)snprintf(out_buf + used, out_cap - used, "\n");
	}
	if (ss_msg && qmi_find_tlv(ss_msg, ss_len, 0x12, &v, &vl) == 0 && vl >= 5) {
		uint16_t mcc = get_le16(v + 0);
		uint16_t mnc = get_le16(v + 2);
		uint8_t dlen = v[4];
		char desc[96] = "";
		if (vl >= 5 + (size_t)dlen)
			format_ascii(v + 5, dlen, desc, sizeof(desc));
		if (used < out_cap)
			used += (size_t)snprintf(out_buf + used, out_cap - used,
						 "NAS_PLMN mcc=%u mnc=%u desc=\"%s\"\n", mcc, mnc, desc);
	}
	if (sig_msg && qmi_find_tlv(sig_msg, sig_len, 0x14, &v, &vl) == 0 && vl >= 6 && used < out_cap) {
		used += (size_t)snprintf(out_buf + used, out_cap - used,
					 "NAS_SIG_LTE rssi=%d rsrq=%d rsrp=%d snr=%d\n",
					 (int8_t)v[0], (int8_t)v[1], get_i16(v + 2), get_i16(v + 4));
	}
	if (sig_msg && qmi_find_tlv(sig_msg, sig_len, 0x17, &v, &vl) == 0 && vl >= 4 && used < out_cap) {
		used += (size_t)snprintf(out_buf + used, out_cap - used,
					 "NAS_SIG_NR5G rsrp=%d snr=%d\n",
					 get_i16(v + 0), get_i16(v + 2));
	}
	if (sig_msg && qmi_find_tlv(sig_msg, sig_len, 0x12, &v, &vl) == 0 && vl >= 1 && used < out_cap) {
		used += (size_t)snprintf(out_buf + used, out_cap - used,
					 "NAS_SIG_GSM rssi=%d\n", (int8_t)v[0]);
	}
	if (sig_msg && qmi_find_tlv(sig_msg, sig_len, 0x13, &v, &vl) == 0 && vl >= 3 && used < out_cap) {
		used += (size_t)snprintf(out_buf + used, out_cap - used,
					 "NAS_SIG_WCDMA rssi=%d ecio=%d\n", (int8_t)v[0], get_i16(v + 1));
	}
	return 0;
}

static void parse_plmn_ascii(const uint8_t *p6, char *out, size_t cap)
{
	size_t n = 0;
	for (size_t i = 0; i < 6 && n + 1 < cap; i++) {
		if (p6[i] >= '0' && p6[i] <= '9')
			out[n++] = (char)p6[i];
	}
	out[n] = '\0';
}

static int parse_and_print_nas_sys_info(const uint8_t *msg, size_t len, char *out_buf, size_t out_cap)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	uint8_t lte_srv = 0, true_srv = 0, domain = 0, roam = 0, ims_voice = 0, nr5g_srv = 0;
	uint32_t cell_id = 0, tac = 0;
	char plmn[16] = "";
	bool has_any = false;

	if (!msg || !out_buf || out_cap == 0)
		return -EINVAL;
	if (qmi_find_tlv(msg, len, 0x14, &v, &vl) == 0 && vl >= 3) {
		lte_srv = v[0];
		true_srv = v[1];
		has_any = true;
	}
	if (qmi_find_tlv(msg, len, 0x4a, &v, &vl) == 0 && vl >= 3) {
		nr5g_srv = v[0];
		has_any = true;
	}
	if (qmi_find_tlv(msg, len, 0x29, &v, &vl) == 0 && vl >= 1) {
		ims_voice = v[0];
		has_any = true;
	}
	if (qmi_find_tlv(msg, len, 0x19, &v, &vl) == 0 && vl >= 26) {
		has_any = true;
		if (v[0])
			domain = v[1];
		if (v[4])
			roam = v[5];
		if (v[11])
			cell_id = get_le32(v + 12);
		if (vl >= 29 && v[26])
			tac = get_le16(v + 27);
		else if (v[8])
			tac = get_le16(v + 9);
		if (v[19])
			parse_plmn_ascii(v + 20, plmn, sizeof(plmn));
	} else if (qmi_find_tlv(msg, len, 0x4b, &v, &vl) == 0 && vl >= 26) {
		has_any = true;
		if (v[0])
			domain = v[1];
		if (v[4])
			roam = v[5];
		if (v[11])
			cell_id = get_le32(v + 12);
		if (vl >= 29 && v[26])
			tac = get_le16(v + 27);
		if (v[19])
			parse_plmn_ascii(v + 20, plmn, sizeof(plmn));
	}
	if (!has_any)
		return -ENOENT;
	snprintf(out_buf, out_cap,
		 "NAS_SYS_INFO_DEC lte_srv=%u true_srv=%u domain=%u roam=%u cell_id=0x%x tac=0x%x plmn=%s ims_voice=%u nr5g_srv=%u\n",
		 lte_srv, true_srv, domain, roam, cell_id, tac, plmn[0] ? plmn : "none", ims_voice, nr5g_srv);
	return 0;
}

static int parse_and_print_nas_home(const uint8_t *hn_msg, size_t hn_len, char *out_buf, size_t out_cap)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (!hn_msg)
		return -ENOENT;
	if (qmi_find_tlv(hn_msg, hn_len, 0x01, &v, &vl) < 0 &&
	    qmi_find_tlv(hn_msg, hn_len, 0x10, &v, &vl) < 0)
		return -ENOENT;
	if (vl < 5)
		return -ENOENT;
	uint16_t mcc = get_le16(v + 0);
	uint16_t mnc = get_le16(v + 2);
	uint8_t dlen = v[4];
	char desc[96] = "";
	if (vl >= 5 + (size_t)dlen)
		format_ascii(v + 5, dlen, desc, sizeof(desc));
	snprintf(out_buf, out_cap, "NAS_HOME mcc=%u mnc=%u desc=\"%s\"", mcc, mnc, desc);
	return 0;
}

static int run_services(struct qrtr_client *c)
{
	static const struct {
		uint32_t id;
		const char *name;
	} svcs[] = {
		{SVC_WDS, "wds"},
		{SVC_DMS, "dms"},
		{SVC_NAS, "nas"},
		{SVC_WMS, "wms"},
		{SVC_VOICE, "voice"},
		{SVC_UIM, "uim"},
		{SVC_RMTFS, "rmtfs"},
		{SVC_LOC, "loc"},
		{SVC_PDC, "pdc"},
		{SVC_WDA, "wda"},
		{SVC_MEMSHARE, "memshare"},
		{SVC_SERVREG_LOC, "servreg_loc"},
		{SVC_TFTP, "tftp"},
	};
	size_t remote_cnt = 0;

	printf("QRTR_CLIENT local_node=%u\n", c->local.sq_node);
	for (size_t i = 0; i < sizeof(svcs) / sizeof(svcs[0]); i++) {
		struct endpoint eps[MAX_EPS];
		size_t n = 0;
		int rc = lookup_service(c, svcs[i].id, eps, MAX_EPS, &n);
		if (rc < 0) {
			printf("QRTR_SVC name=%s id=0x%x status=err(%d)\n", svcs[i].name, svcs[i].id, rc);
			continue;
		}
		if (n == 0) {
			printf("QRTR_SVC name=%s id=0x%x status=not_listed count=0\n", svcs[i].name, svcs[i].id);
			continue;
		}
		for (size_t k = 0; k < n; k++) {
			bool is_remote = (eps[k].node != c->local.sq_node);
			if (is_remote)
				remote_cnt++;
			printf("QRTR_SVC name=%s id=0x%x loc=%s node=%u port=0x%x inst=0x%x idx=%zu/%zu\n",
			       svcs[i].name, svcs[i].id, is_remote ? "remote" : "local",
			       eps[k].node, eps[k].port, eps[k].instance, k + 1, n);
		}
	}
	printf("QRTR_REMOTE_COUNT=%zu\n", remote_cnt);
	return remote_cnt > 0 ? 0 : 1;
}

static int run_services_all(struct qrtr_client *c)
{
	uint8_t req[sizeof(struct qrtr_ctrl_pkt)] = {0};
	struct sockaddr_qrtr dst = {
		.sq_family = AF_QIPCRTR,
		.sq_node = c->local.sq_node,
		.sq_port = QRTR_PORT_CTRL,
	};
	int64_t deadline = now_ms() + 1200;
	size_t count = 0;

	put_le32(req + FIELD_OFF(cmd), QRTR_TYPE_NEW_LOOKUP);
	put_le32(req + FIELD_OFF(server.service), 0u);
	if (sendto(c->fd, req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
		return 1;

	for (;;) {
		uint8_t buf[sizeof(struct qrtr_ctrl_pkt)];
		struct sockaddr_qrtr from = {0};
		socklen_t sl = sizeof(from);
		if (wait_poll(c->fd, deadline) < 0)
			break;
		ssize_t n = recvfrom(c->fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &sl);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			break;
		}
		if (sl != sizeof(from) || from.sq_node != c->local.sq_node ||
		    from.sq_port != QRTR_PORT_CTRL || (size_t)n != sizeof(struct qrtr_ctrl_pkt))
			continue;
		if (get_le32(buf + FIELD_OFF(cmd)) != QRTR_TYPE_NEW_SERVER)
			continue;
		uint32_t id = get_le32(buf + FIELD_OFF(server.service));
		uint32_t inst = get_le32(buf + FIELD_OFF(server.instance));
		uint32_t node = get_le32(buf + FIELD_OFF(server.node));
		uint32_t port = get_le32(buf + FIELD_OFF(server.port));
		if (!id && !inst && !node && !port)
			break;
		printf("QRTR_ALL svc=0x%x inst=0x%x node=%u port=0x%x\n", id, inst, node, port);
		count++;
	}
	put_le32(req + FIELD_OFF(cmd), QRTR_TYPE_DEL_LOOKUP);
	(void)sendto(c->fd, req, sizeof(req), 0, (struct sockaddr *)&dst, sizeof(dst));
	printf("QRTR_ALL_COUNT=%zu\n", count);
	return count > 0 ? 0 : 1;
}

static int run_dms(struct qrtr_client *c)
{
	struct endpoint ep = {0};
	uint8_t m[QMI_MAX_BUF], r[QMI_MAX_BUF], s[QMI_MAX_BUF], o[QMI_MAX_BUF];
	size_t ml = 0, rl = 0, sl = 0, ol = 0;
	uint16_t err = 0;
	char out[512];

	if (!find_remote_endpoint(c, SVC_DMS, &ep)) {
		printf("DMS_ERR status=no_remote_service\n");
		return 1;
	}
	bool hm = qmi_rpc(c, &ep, 1, QMI_DMS_GET_MODEL_ID, m, sizeof(m), &ml, &err) == 0 && err == 0;
	bool hr = qmi_rpc(c, &ep, 2, QMI_DMS_GET_REV_ID, r, sizeof(r), &rl, &err) == 0 && err == 0;
	bool hs = qmi_rpc(c, &ep, 3, QMI_DMS_GET_SERIAL_NUMS, s, sizeof(s), &sl, &err) == 0 && err == 0;
	bool ho = qmi_rpc(c, &ep, 4, QMI_DMS_GET_OP_MODE, o, sizeof(o), &ol, &err) == 0 && err == 0;
	parse_and_print_dms(hm ? m : NULL, ml, hr ? r : NULL, rl, hs ? s : NULL, sl, ho ? o : NULL, ol,
			    out, sizeof(out));
	printf("%s\n", out);
	return (hm || hr || hs || ho) ? 0 : 1;
}

static size_t build_dms_set_mode_tlvs(uint8_t *tlvs, uint8_t mode)
{
	return tlv_append(tlvs, 0x01, &mode, 1);
}

static int parse_mode_arg(const char *mode_str, uint8_t *out_mode)
{
	if (strcmp(mode_str, "online") == 0)
		*out_mode = 0;
	else if (strcmp(mode_str, "lpm") == 0)
		*out_mode = 1;
	else if (strcmp(mode_str, "offline") == 0)
		*out_mode = 3;
	else if (strcmp(mode_str, "reset") == 0)
		*out_mode = 4;
	else
		return -EINVAL;
	return 0;
}

static int run_set_mode(struct qrtr_client *c, const char *mode_str)
{
	struct endpoint ep = {0};
	uint8_t mode = 0;
	if (parse_mode_arg(mode_str, &mode) < 0) {
		printf("DMS_SET_MODE_ERR invalid=%s\n", mode_str);
		return 2;
	}
	if (!find_remote_endpoint(c, SVC_DMS, &ep)) {
		printf("DMS_SET_MODE_ERR status=no_remote_service\n");
		return 1;
	}
	uint8_t req_tlvs[16], resp[QMI_MAX_BUF];
	size_t req_len = build_dms_set_mode_tlvs(req_tlvs, mode);
	size_t rl = 0;
	uint16_t err = 0;
	int rc = qmi_rpc_tlv(c, &ep, 30, QMI_DMS_SET_OP_MODE, req_tlvs, req_len,
			     resp, sizeof(resp), &rl, &err);

	uint8_t op_resp[QMI_MAX_BUF];
	size_t ol = 0;
	uint16_t oerr = 0;
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	uint8_t post_mode = 255;
	if (qmi_rpc(c, &ep, 31, QMI_DMS_GET_OP_MODE, op_resp, sizeof(op_resp), &ol, &oerr) == 0 &&
	    oerr == 0 && qmi_find_tlv(op_resp, ol, 0x01, &v, &vl) == 0 && vl >= 1) {
		post_mode = v[0];
	}

	printf("DMS_SET_MODE req=%s rc=%d err=0x%x(%s) post_mode=%u(%s)\n",
	       mode_str, rc, err, qmi_err_name(err), post_mode,
	       post_mode == 255 ? "absent" : op_mode_name(post_mode));

	if (post_mode != 255) {
		printf("DMS_OP_MODE mode=%u(%s)", post_mode, op_mode_name(post_mode));
		if (qmi_find_tlv(op_resp, ol, 0x10, &v, &vl) == 0 && vl >= 2)
			printf(" offline_reason=0x%04x", get_le16(v));
		if (qmi_find_tlv(op_resp, ol, 0x11, &v, &vl) == 0 && vl >= 1)
			printf(" hw_restricted=%u", v[0]);
		printf("\n");
	} else {
		printf("DMS_OP_MODE mode=unknown err=0x%x\n", oerr);
	}
	return (rc == 0 && err == 0) ? 0 : 1;
}

static int run_uim(struct qrtr_client *c)
{
	struct endpoint ep = {0};
	uint8_t buf[QMI_MAX_BUF];
	size_t bl = 0;
	uint16_t err = 0;
	char out[1024];

	if (!find_remote_endpoint(c, SVC_UIM, &ep)) {
		printf("UIM_ERR status=no_remote_service\n");
		return 1;
	}
	int rc = qmi_rpc(c, &ep, 10, QMI_UIM_GET_CARD_STATUS, buf, sizeof(buf), &bl, &err);
	if (rc < 0) {
		printf("UIM_ERR status=rpc_failed rc=%d\n", rc);
		return 1;
	}
	if (err != 0) {
		printf("UIM_ERR status=qmi_error err=0x%04x\n", err);
		return 3;
	}
	if (parse_and_print_uim(buf, bl, out, sizeof(out)) < 0) {
		printf("UIM_ERR status=decode_error\n");
		return 1;
	}
	printf("%s", out);
	return 0;
}

static size_t build_uim_read_tlvs(uint8_t *tlvs, uint8_t sess_type, const uint8_t *aid, uint8_t aid_len,
				  uint16_t file_id, const uint8_t *path, uint8_t path_len,
				  uint16_t offset, uint16_t length)
{
	uint8_t sbuf[64], fbuf[32], rbuf[4];
	size_t pos = 0;
	if (aid_len > sizeof(sbuf) - 2)
		aid_len = sizeof(sbuf) - 2;
	sbuf[0] = sess_type;
	sbuf[1] = aid_len;
	if (aid_len > 0 && aid)
		memcpy(sbuf + 2, aid, aid_len);
	pos += tlv_append(tlvs + pos, 0x01, sbuf, (uint16_t)(2 + aid_len));

	put_le16(fbuf, file_id);
	fbuf[2] = path_len;
	if (path_len > 0 && path)
		memcpy(fbuf + 3, path, path_len);
	pos += tlv_append(tlvs + pos, 0x02, fbuf, (uint16_t)(3 + path_len));

	put_le16(rbuf, offset);
	put_le16(rbuf + 2, length);
	pos += tlv_append(tlvs + pos, 0x03, rbuf, 4);
	return pos;
}

static int parse_uim_read_result(const uint8_t *resp, size_t resp_len, const uint8_t **out_data, size_t *out_len)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (qmi_find_tlv(resp, resp_len, 0x11, &v, &vl) < 0 || vl < 2)
		return -ENOENT;
	uint16_t content_len = get_le16(v);
	if ((size_t)content_len + 2 > vl)
		return -EPROTO;
	*out_data = v + 2;
	*out_len = content_len;
	return 0;
}

static int run_sim_ef(struct qrtr_client *c)
{
	struct endpoint ep = {0};
	uint8_t card_resp[QMI_MAX_BUF];
	size_t cl = 0;
	uint16_t cerr = 0;

	if (!find_remote_endpoint(c, SVC_UIM, &ep)) {
		printf("UIM_EF_ERR status=no_remote_service\n");
		return 1;
	}
	int active_slot = 0;
	uint8_t prov_sess = 0;
	uint8_t aid[32] = {0};
	uint8_t aid_len = 0;
	if (qmi_rpc(c, &ep, 40, QMI_UIM_GET_CARD_STATUS, card_resp, sizeof(card_resp), &cl, &cerr) == 0 && cerr == 0) {
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		if (qmi_find_tlv(card_resp, cl, 0x10, &v, &vl) == 0 && vl >= 9) {
			uint16_t gw_pri = get_le16(v + 0);
			uint16_t gw_sec = get_le16(v + 4);
			uint8_t num_cards = v[8];
			size_t pos = 9;
			for (uint8_t slot = 0; slot < num_cards && vl - pos >= 6; slot++) {
				uint8_t cstate = v[pos + 0];
				uint8_t napps = v[pos + 5];
				pos += 6;
				if (cstate == 1)
					active_slot = slot;
				for (uint8_t ai = 0; ai < napps && vl - pos >= 7; ai++) {
					uint8_t atype = v[pos + 0];
					uint8_t alen = v[pos + 6];
					pos += 7;
					if (vl - pos < (size_t)alen + 7)
						break;
					if (cstate == 1 && (atype == 2 || atype == 1) && aid_len == 0 && alen <= sizeof(aid)) {
						memcpy(aid, v + pos, alen);
						aid_len = alen;
					}
					pos += alen + 7;
				}
			}
			if (gw_pri != 0xffff && (gw_pri & 0xff) == active_slot)
				prov_sess = 0;
			else if (gw_sec != 0xffff && (gw_sec & 0xff) == active_slot)
				prov_sess = 2;
			else
				prov_sess = (uint8_t)(4 + active_slot);
		}
	}

	uint8_t card_sess = (uint8_t)(6 + active_slot); /* QMI_UIM_SESSION_TYPE_CARD_SLOT_1/2 */
	static const uint8_t path_mf[2] = { 0x00, 0x3f };
	static const uint8_t path_adf[4] = { 0x00, 0x3f, 0xff, 0x7f };
	uint8_t tlvs[128], resp[QMI_MAX_BUF];
	size_t tlen, rl = 0;
	uint16_t err = 0;
	int rc_total = 0;

	/* EF_ICCID (0x2FE2 under MF 3F00) */
	tlen = build_uim_read_tlvs(tlvs, card_sess, NULL, 0, 0x2fe2, path_mf, sizeof(path_mf), 0, 10);
	int rc1 = qmi_rpc_tlv(c, &ep, 41, QMI_UIM_READ_TRANSPARENT, tlvs, tlen, resp, sizeof(resp), &rl, &err);
	if ((rc1 != 0 || err != 0)) {
		tlen = build_uim_read_tlvs(tlvs, prov_sess, aid_len ? aid : NULL, prov_sess >= 4 ? aid_len : 0,
					   0x2fe2, path_mf, sizeof(path_mf), 0, 10);
		rc1 = qmi_rpc_tlv(c, &ep, 42, QMI_UIM_READ_TRANSPARENT, tlvs, tlen, resp, sizeof(resp), &rl, &err);
	}
	const uint8_t *ef_bytes = NULL;
	size_t ef_len = 0;
	if (rc1 == 0 && err == 0 && parse_uim_read_result(resp, rl, &ef_bytes, &ef_len) == 0) {
		char iccid[32], masked[16];
		if (decode_iccid_bcd(ef_bytes, ef_len, iccid, sizeof(iccid)) == 0) {
			format_masked_str(iccid, masked, sizeof(masked));
			printf("UIM_EF_ICCID rc=0 err=0x0 len=%zu masked=%s\n", strlen(iccid), masked);
		} else {
			printf("UIM_EF_ICCID rc=-71 err=0x0 len=%zu masked=invalid\n", ef_len);
			rc_total |= 1;
		}
		memset(iccid, 0, sizeof(iccid));
	} else {
		printf("UIM_EF_ICCID rc=%d err=0x%x len=0 masked=none\n", rc1, err);
		rc_total |= 1;
	}

	/* EF_IMSI (0x6F07 under ADF 3F00/7FFF) */
	tlen = build_uim_read_tlvs(tlvs, prov_sess, prov_sess >= 4 ? aid : NULL, prov_sess >= 4 ? aid_len : 0,
				   0x6f07, path_adf, sizeof(path_adf), 0, 9);
	int rc2 = qmi_rpc_tlv(c, &ep, 43, QMI_UIM_READ_TRANSPARENT, tlvs, tlen, resp, sizeof(resp), &rl, &err);
	if ((rc2 != 0 || err != 0) && aid_len > 0) {
		uint8_t np_sess = (uint8_t)(4 + active_slot);
		tlen = build_uim_read_tlvs(tlvs, np_sess, aid, aid_len, 0x6f07, path_adf, sizeof(path_adf), 0, 9);
		rc2 = qmi_rpc_tlv(c, &ep, 44, QMI_UIM_READ_TRANSPARENT, tlvs, tlen, resp, sizeof(resp), &rl, &err);
	}
	if (rc2 == 0 && err == 0 && parse_uim_read_result(resp, rl, &ef_bytes, &ef_len) == 0) {
		size_t imsi_len = 0;
		char mcc[8] = "000", mnc[8] = "00";
		if (decode_imsi_plmn(ef_bytes, ef_len, &imsi_len, mcc, mnc) == 0) {
			printf("UIM_EF_IMSI rc=0 err=0x0 len=%zu mcc=%s mnc=%s\n", imsi_len, mcc, mnc);
		} else {
			printf("UIM_EF_IMSI rc=-71 err=0x0 len=%zu mcc=000 mnc=00\n", ef_len);
			rc_total |= 1;
		}
	} else {
		printf("UIM_EF_IMSI rc=%d err=0x%x len=0 mcc=000 mnc=00\n", rc2, err);
		rc_total |= 1;
	}
	return rc_total ? 1 : 0;
}

static void dump_tlv_hex(const char *tag, uint16_t msg_id, const uint8_t *msg, size_t len);

static size_t build_nas_bind_sub_tlvs(uint8_t *req, uint8_t sub_id)
{
	return tlv_append(req, 0x01, &sub_id, 1);
}

static size_t build_nas_set_sys_sel_tlvs(uint8_t *req, uint16_t mode_pref, uint8_t net_sel, uint8_t change_dur)
{
	(void)net_sel;
	uint8_t mpb[2];
	put_le16(mpb, mode_pref);
	size_t qlen = tlv_append(req, 0x11, mpb, sizeof(mpb));
	qlen += tlv_append(req + qlen, 0x17, &change_dur, 1);
	return qlen;
}

static int parse_nas_mode_arg(const char *arg, uint16_t *out_mode_pref)
{
	if (!arg || !out_mode_pref)
		return -EINVAL;
	if (strcmp(arg, "lte") == 0)
		*out_mode_pref = 0x001cu; /* GSM | UMTS | LTE */
	else if (strcmp(arg, "lte-only") == 0)
		*out_mode_pref = 0x0010u; /* LTE only */
	else if (strcmp(arg, "nr5g") == 0 || strcmp(arg, "auto") == 0)
		*out_mode_pref = 0x005cu; /* GSM | UMTS | LTE | NR5G */
	else
		return -EINVAL;
	return 0;
}

static int nas_bind_sub(struct qrtr_client *c, const struct endpoint *ep, uint16_t txn, int sub_id)
{
	if (sub_id < 0)
		return 0;
	uint8_t req[8], resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t err = 0;
	size_t qlen = build_nas_bind_sub_tlvs(req, (uint8_t)sub_id);
	int rc = qmi_rpc_tlv(c, ep, txn, QMI_NAS_BIND_SUBSCRIPTION, req, qlen, resp, sizeof(resp), &rl, &err);
	if (rc == 0 && err == 0x01) {
		uint8_t sbuf[4];
		put_le32(sbuf, (uint32_t)sub_id);
		qlen = tlv_append(req, 0x01, sbuf, 4);
		rc = qmi_rpc_tlv(c, ep, (uint16_t)(txn + 1u), QMI_NAS_BIND_SUBSCRIPTION,
				 req, qlen, resp, sizeof(resp), &rl, &err);
		printf("NAS_BIND_SUB_U32 sub=%d rc=%d err=0x%x(%s)\n", sub_id, rc, err, qmi_err_name(err));
		return (rc == 0 && err == 0) ? 0 : 1;
	}
	printf("NAS_BIND_SUB sub=%d rc=%d err=0x%x(%s)\n", sub_id, rc, err, qmi_err_name(err));
	return (rc == 0 && err == 0) ? 0 : 1;
}

static int run_nas_impl(struct qrtr_client *c, bool with_home, int sub_id)
{
	struct endpoint ep = {0};
	uint8_t ss[QMI_MAX_BUF], sig[QMI_MAX_BUF], hn[QMI_MAX_BUF], si[QMI_MAX_BUF];
	size_t ssl = 0, sigl = 0, hnl = 0, sil = 0;
	uint16_t err1 = 0, err2 = 0, err3 = 0, err4 = 0;
	char out[1024] = "";

	if (!find_remote_endpoint(c, SVC_NAS, &ep)) {
		printf("NAS_ERR status=no_remote_service\n");
		return 1;
	}
	if (sub_id >= 0)
		(void)nas_bind_sub(c, &ep, 19, sub_id);

	int rc1 = qmi_rpc(c, &ep, 20, QMI_NAS_GET_SERVING_SYSTEM, ss, sizeof(ss), &ssl, &err1);
	int rc2 = qmi_rpc(c, &ep, 21, QMI_NAS_GET_SIGNAL_INFO, sig, sizeof(sig), &sigl, &err2);
	printf("NAS_RPC serving_rc=%d(err=0x%x) signal_rc=%d(err=0x%x)\n", rc1, err1, rc2, err2);
	if (parse_and_print_nas((rc1 == 0 && err1 == 0) ? ss : NULL, ssl,
				(rc2 == 0 && err2 == 0) ? sig : NULL, sigl,
				out, sizeof(out)) < 0) {
		printf("NAS_ERR status=decode_error\n");
		return 1;
	}
	printf("%s", out);
	if (with_home) {
		int rc3 = qmi_rpc(c, &ep, 22, QMI_NAS_GET_HOME_NETWORK, hn, sizeof(hn), &hnl, &err3);
		char hout[256] = "";
		if (rc3 == 0 && err3 == 0 && parse_and_print_nas_home(hn, hnl, hout, sizeof(hout)) == 0)
			printf("%s\n", hout);
		else
			printf("NAS_HOME rc=%d err=0x%x mcc=0 mnc=0 desc=\"\"\n", rc3, err3);

		int rc4 = qmi_rpc(c, &ep, 23, QMI_NAS_GET_SYS_INFO, si, sizeof(si), &sil, &err4);
		printf("NAS_SYS_INFO_RPC sub=%d rc=%d err=0x%x(%s)\n", sub_id, rc4, err4, qmi_err_name(err4));
		if (rc4 == 0 && err4 == 0) {
			dump_tlv_hex("NAS_SYS_INFO_EXT", QMI_NAS_GET_SYS_INFO, si, sil);
			char sout[256] = "";
			if (parse_and_print_nas_sys_info(si, sil, sout, sizeof(sout)) == 0)
				printf("%s", sout);
		}
	}
	return (rc1 == 0 || rc2 == 0) ? 0 : 1;
}


static int run_nas_mode(struct qrtr_client *c, const char *mode_arg, int sub_id)
{
	struct endpoint ep = {0};
	uint16_t mode_pref = 0;
	int use_sub = (sub_id >= 0) ? sub_id : 1;
	if (parse_nas_mode_arg(mode_arg, &mode_pref) < 0) {
		printf("NAS_SET_MODE_ERR invalid=%s\n", mode_arg ? mode_arg : "null");
		return 2;
	}
	if (!find_remote_endpoint(c, SVC_NAS, &ep)) {
		printf("NAS_SET_MODE_ERR status=no_nas_endpoint\n");
		return 1;
	}
	(void)nas_bind_sub(c, &ep, 24, use_sub);

	uint8_t req[32], resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t err = 0;
	size_t qlen = build_nas_set_sys_sel_tlvs(req, mode_pref, 0u, 0u);
	int rc = qmi_rpc_tlv(c, &ep, 25, QMI_NAS_SET_SYS_SEL_PREF, req, qlen, resp, sizeof(resp), &rl, &err);
	printf("NAS_SET_MODE sub=%d req=%s pref=0x%04x rc=%d err=0x%x(%s)\n",
	       use_sub, mode_arg, mode_pref, rc, err, qmi_err_name(err));

	uint8_t rb[QMI_MAX_BUF];
	size_t rbl = 0;
	uint16_t rerr = 0;
	if (qmi_rpc(c, &ep, 26, QMI_NAS_GET_SYS_SEL_PREF, rb, sizeof(rb), &rbl, &rerr) == 0 && rerr == 0) {
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		uint16_t got_pref = 0;
		if (qmi_find_tlv(rb, rbl, 0x11, &v, &vl) == 0 && vl >= 2)
			got_pref = get_le16(v);
		printf("NAS_GET_MODE sub=%d pref=0x%04x\n", use_sub, got_pref);
	}
	return (rc == 0 && err == 0) ? 0 : 1;
}

static int parse_loc_rev(const uint8_t *resp, size_t resp_len, const uint8_t *ind, size_t ind_len, uint32_t *out_rev)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (ind && ind_len >= QMI_HDR_LEN) {
		if (qmi_find_tlv(ind, ind_len, 0x02, &v, &vl) == 0 && vl == 4) {
			*out_rev = get_le32(v);
			return 0;
		}
		if (qmi_find_tlv(ind, ind_len, 0x10, &v, &vl) == 0 && vl == 4) {
			*out_rev = get_le32(v);
			return 0;
		}
	}
	if (resp && resp_len >= QMI_HDR_LEN) {
		if (qmi_find_tlv(resp, resp_len, 0x10, &v, &vl) == 0 && vl == 4) {
			*out_rev = get_le32(v);
			return 0;
		}
		if (qmi_find_tlv(resp, resp_len, 0x01, &v, &vl) == 0 && vl == 4) {
			*out_rev = get_le32(v);
			return 0;
		}
	}
	return -ENOENT;
}

static int run_loc_rev(struct qrtr_client *c)
{
	struct endpoint ep = {0};
	uint8_t resp[QMI_MAX_BUF], ind[QMI_MAX_BUF];
	size_t rl = 0, il = 0;
	uint16_t err = 0;
	uint32_t rev = 0;

	if (!find_remote_endpoint(c, SVC_LOC, &ep)) {
		printf("LOC_REV rc=-2 err=0x0 rev=0\n");
		return 1;
	}
	int rc = qmi_rpc_tlv_ex(c, &ep, 50, QMI_LOC_GET_SERVICE_REVISION, NULL, 0,
				resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	if (rc == 0 && err == 0)
		(void)parse_loc_rev(resp, rl, ind, il, &rev);
	printf("LOC_REV rc=%d err=0x%x rev=%u\n", rc, err, rev);
	return (rc == 0 && err == 0) ? 0 : 1;
}

static void probe_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void probe_put_le64(uint8_t *p, uint64_t v)
{
	probe_put_le32(p, (uint32_t)(v & 0xffffffffu));
	probe_put_le32(p + 4, (uint32_t)(v >> 32));
}

static void dump_tlv_hex(const char *tag, uint16_t msg_id, const uint8_t *msg, size_t len)
{
	if (!msg || len <= QMI_HDR_LEN) {
		printf("%s msg=0x%04x tlvs=none\n", tag, msg_id);
		return;
	}
	size_t off = QMI_HDR_LEN;
	printf("%s msg=0x%04x len=%zu", tag, msg_id, len);
	while (off + 3 <= len) {
		uint8_t id = msg[off];
		uint16_t tl = get_le16(msg + off + 1);
		if (off + 3 + (size_t)tl > len) {
			printf(" TRUNC@%zu", off);
			break;
		}
		printf(" [0x%02x:%u:", id, tl);
		size_t show = tl > 24 ? 24 : tl;
		for (size_t i = 0; i < show; i++)
			printf("%02x", msg[off + 3 + i]);
		if (tl > show)
			printf("..");
		printf("]");
		off += 3 + (size_t)tl;
	}
	printf("\n");
}

static int parse_dms_cap(const uint8_t *tlvs, size_t len, uint32_t *max_tx,
			 uint32_t *max_rx, uint8_t *data_cap, uint8_t *sim_cap, uint8_t *num_ifs)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (qmi_find_tlv(tlvs, len, 0x01, &v, &vl) != 0 || vl < 11)
		return -ENOENT;
	*max_tx = get_le32(v + 0);
	*max_rx = get_le32(v + 4);
	*data_cap = v[8];
	*sim_cap = v[9];
	*num_ifs = v[10];
	if (vl < 11 + (size_t)*num_ifs)
		return -EPROTO;
	return 0;
}

static int parse_dms_op_mode_raw(const uint8_t *tlvs, size_t len, uint8_t *mode,
				 uint16_t *offline_reason, bool *has_offline,
				 uint8_t *hw_restricted, bool *has_hw)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	*has_offline = false;
	*has_hw = false;
	if (qmi_find_tlv(tlvs, len, 0x01, &v, &vl) != 0 || vl < 1)
		return -ENOENT;
	*mode = v[0];
	if (qmi_find_tlv(tlvs, len, 0x10, &v, &vl) == 0 && vl >= 2) {
		*offline_reason = get_le16(v);
		*has_offline = true;
	}
	if (qmi_find_tlv(tlvs, len, 0x11, &v, &vl) == 0 && vl >= 1) {
		*hw_restricted = v[0];
		*has_hw = true;
	}
	return 0;
}

static int parse_pdc_list_configs(const uint8_t *resp, size_t rl, const uint8_t *ind, size_t il,
				  uint32_t *out_count)
{
	const uint8_t *sources[2] = { ind, resp };
	size_t lens[2] = { il, rl };
	*out_count = 0;
	for (int s = 0; s < 2; s++) {
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		if (!sources[s] || lens[s] == 0)
			continue;
		if (qmi_find_tlv(sources[s], lens[s], 0x11, &v, &vl) == 0 && vl >= 1) {
			*out_count = v[0];
			return 0;
		}
	}
	return -ENOENT;
}

static size_t build_pdc_list_tlvs(uint8_t *req, uint32_t ind_token, uint32_t ctype)
{
	uint8_t tok_buf[4], cbuf[4];
	probe_put_le32(tok_buf, ind_token);
	probe_put_le32(cbuf, ctype);
	size_t qlen = tlv_append(req, 0x10, tok_buf, 4);
	qlen += tlv_append(req + qlen, 0x11, cbuf, 4);
	return qlen;
}

static size_t build_pdc_selected_tlvs(uint8_t *req, uint32_t ctype, uint32_t ind_token)
{
	uint8_t cbuf[4], tok_buf[4];
	probe_put_le32(cbuf, ctype);
	probe_put_le32(tok_buf, ind_token);
	size_t qlen = tlv_append(req, 0x01, cbuf, 4);
	qlen += tlv_append(req + qlen, 0x10, tok_buf, 4);
	return qlen;
}

static size_t build_pdc_selected_sub_tlvs(uint8_t *req, uint32_t ctype, uint32_t ind_token,
					bool has_sub, uint32_t sub_id)
{
	size_t qlen = build_pdc_selected_tlvs(req, ctype, ind_token);
	if (has_sub) {
		uint8_t sbuf[4];
		probe_put_le32(sbuf, sub_id);
		qlen += tlv_append(req + qlen, 0x11, sbuf, 4);
	}
	return qlen;
}

static size_t build_pdc_config_info_tlvs(uint8_t *req, uint32_t ctype, const uint8_t *id,
					   uint8_t id_len, uint32_t ind_token)
{
	uint8_t cid[5 + 64];
	uint8_t tok_buf[4];
	if (id_len > 64)
		id_len = 64;
	probe_put_le32(cid, ctype);
	cid[4] = id_len;
	if (id_len > 0 && id)
		memcpy(cid + 5, id, id_len);
	size_t qlen = tlv_append(req, 0x01, cid, (uint16_t)(5 + id_len));
	probe_put_le32(tok_buf, ind_token);
	qlen += tlv_append(req + qlen, 0x10, tok_buf, 4);
	return qlen;
}

static size_t build_pdc_select_tlvs(uint8_t *req, uint32_t ctype, const uint8_t *id,
				      uint8_t id_len, uint32_t ind_token,
				      bool has_sub, uint32_t sub_id)
{
	size_t qlen = build_pdc_config_info_tlvs(req, ctype, id, id_len, ind_token);
	if (has_sub) {
		uint8_t sbuf[4];
		probe_put_le32(sbuf, sub_id);
		qlen += tlv_append(req + qlen, 0x11, sbuf, 4);
	}
	return qlen;
}

static size_t build_pdc_activate_tlvs(uint8_t *req, uint32_t ctype, uint32_t ind_token,
					bool has_sub, uint32_t sub_id)
{
	uint8_t cbuf[4], tok_buf[4];
	probe_put_le32(cbuf, ctype);
	probe_put_le32(tok_buf, ind_token);
	size_t qlen = tlv_append(req, 0x01, cbuf, 4);
	qlen += tlv_append(req + qlen, 0x10, tok_buf, 4);
	if (has_sub) {
		uint8_t sbuf[4];
		probe_put_le32(sbuf, sub_id);
		qlen += tlv_append(req + qlen, 0x11, sbuf, 4);
	}
	return qlen;
}

struct pdc_list_entry {
	uint32_t type;
	uint8_t id_len;
	uint8_t id[64];
};

static int parse_pdc_selected_id(const uint8_t *resp, size_t rl, const uint8_t *ind, size_t il,
				 uint8_t *id_out, uint8_t *id_len_out)
{
	const uint8_t *sources[2] = { ind, resp };
	size_t lens[2] = { il, rl };
	*id_len_out = 0;
	for (int s = 0; s < 2; s++) {
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		if (!sources[s] || lens[s] == 0)
			continue;
		if (qmi_find_tlv(sources[s], lens[s], 0x11, &v, &vl) == 0 && vl >= 2) {
			uint8_t ilen = v[0];
			if (ilen > 0 && ilen <= 64 && (size_t)(1 + ilen) <= vl) {
				memcpy(id_out, v + 1, ilen);
				*id_len_out = ilen;
				return 0;
			}
		}
		if (qmi_find_tlv(sources[s], lens[s], 0x12, &v, &vl) == 0 && vl >= 2) {
			uint8_t ilen = v[0];
			if (ilen > 0 && ilen <= 64 && (size_t)(1 + ilen) <= vl) {
				memcpy(id_out, v + 1, ilen);
				*id_len_out = ilen;
				return 0;
			}
		}
	}
	return -ENOENT;
}

static int parse_pdc_list_entries(const uint8_t *resp, size_t rl, const uint8_t *ind, size_t il,
				  struct pdc_list_entry *out, size_t cap, size_t *out_cnt)
{
	const uint8_t *sources[2] = { ind, resp };
	size_t lens[2] = { il, rl };
	*out_cnt = 0;
	for (int s = 0; s < 2; s++) {
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		if (!sources[s] || lens[s] == 0)
			continue;
		if (qmi_find_tlv(sources[s], lens[s], 0x11, &v, &vl) == 0 && vl >= 1) {
			uint8_t total = v[0];
			size_t pos = 1;
			size_t cnt = 0;
			for (uint8_t i = 0; i < total && pos + 5 <= vl && cnt < cap; i++) {
				uint32_t ctype = get_le32(v + pos);
				uint8_t ilen = v[pos + 4];
				pos += 5;
				if (pos + ilen > vl)
					break;
				out[cnt].type = ctype;
				out[cnt].id_len = ilen > 64 ? 64 : ilen;
				memcpy(out[cnt].id, v + pos, out[cnt].id_len);
				cnt++;
				pos += ilen;
			}
			*out_cnt = cnt;
			return 0;
		}
	}
	return -ENOENT;
}

static size_t build_wds_bind_sub_tlvs(uint8_t *req, uint32_t sub_id)
{
	uint8_t sbuf[4];
	probe_put_le32(sbuf, sub_id);
	return tlv_append(req, 0x01, sbuf, 4);
}

static size_t build_wds_bind_mux_tlvs(uint8_t *req, uint32_t ep_type, uint32_t iface_id,
				      uint8_t mux_id, int client_type)
{
	uint8_t ep_buf[8];
	probe_put_le32(ep_buf, ep_type);
	probe_put_le32(ep_buf + 4, iface_id);
	size_t qlen = tlv_append(req, 0x10, ep_buf, sizeof(ep_buf));
	qlen += tlv_append(req + qlen, 0x11, &mux_id, 1);
	if (client_type >= 0) {
		uint8_t cbuf[4];
		probe_put_le32(cbuf, (uint32_t)client_type);
		qlen += tlv_append(req + qlen, 0x13, cbuf, 4);
	}
	return qlen;
}

static size_t build_dpm_open_port_tlvs(uint8_t *req, uint32_t ep_type, uint32_t iface_id,
				       uint32_t rx_ep, uint32_t tx_ep)
{
	uint8_t p[17];
	p[0] = 1u;
	probe_put_le32(p + 1, ep_type);
	probe_put_le32(p + 5, iface_id);
	probe_put_le32(p + 9, rx_ep);
	probe_put_le32(p + 13, tx_ep);
	return tlv_append(req, 0x11, p, sizeof(p));
}

static size_t build_wda_set_data_fmt_tlvs(uint8_t *req, uint32_t link_prot,
					  uint32_t ul_prot, uint32_t dl_prot,
					  uint32_t ep_type, uint32_t iface_id)
{
	uint8_t b4[4], ep_buf[8];
	size_t qlen = 0;
	probe_put_le32(b4, link_prot);
	qlen += tlv_append(req + qlen, 0x11, b4, 4);
	probe_put_le32(b4, ul_prot);
	qlen += tlv_append(req + qlen, 0x12, b4, 4);
	probe_put_le32(b4, dl_prot);
	qlen += tlv_append(req + qlen, 0x13, b4, 4);
	probe_put_le32(b4, 16u);
	qlen += tlv_append(req + qlen, 0x15, b4, 4);
	probe_put_le32(b4, 8192u);
	qlen += tlv_append(req + qlen, 0x16, b4, 4);
	probe_put_le32(ep_buf, ep_type);
	probe_put_le32(ep_buf + 4, iface_id);
	qlen += tlv_append(req + qlen, 0x17, ep_buf, sizeof(ep_buf));
	return qlen;
}

static size_t build_wds_ip_family_tlvs(uint8_t *req, uint8_t ip_pref)
{
	return tlv_append(req, 0x01, &ip_pref, 1);
}

static size_t build_wds_profile_list_tlvs(uint8_t *req, uint8_t profile_type)
{
	return tlv_append(req, 0x10, &profile_type, 1);
}

static size_t build_wds_profile_settings_tlvs(uint8_t *req, uint8_t profile_type, uint8_t profile_idx)
{
	uint8_t pbuf[2] = { profile_type, profile_idx };
	return tlv_append(req, 0x01, pbuf, sizeof(pbuf));
}

static size_t build_wds_start_tlvs(uint8_t *req, const char *apn, uint8_t ip_pref)
{
	size_t qlen = 0;
	if (apn && apn[0] != '\0') {
		size_t alen = strlen(apn);
		if (alen > 100)
			alen = 100;
		qlen += tlv_append(req + qlen, 0x14, (const uint8_t *)apn, (uint16_t)alen);
	}
	qlen += tlv_append(req + qlen, 0x19, &ip_pref, 1);
	return qlen;
}

static size_t build_wds_stop_tlvs(uint8_t *req, uint32_t handle)
{
	uint8_t hbuf[4];
	probe_put_le32(hbuf, handle);
	return tlv_append(req, 0x01, hbuf, 4);
}

static size_t build_wds_curr_settings_tlvs(uint8_t *req, uint32_t mask)
{
	uint8_t mbuf[4];
	probe_put_le32(mbuf, mask);
	return tlv_append(req, 0x10, mbuf, 4);
}

static void format_ipv4_u32(uint32_t ip, char *buf, size_t cap)
{
	snprintf(buf, cap, "%u.%u.%u.%u",
		 (ip >> 24) & 0xffu, (ip >> 16) & 0xffu,
		 (ip >> 8) & 0xffu, ip & 0xffu);
}

static int ipv4_mask_to_prefix(uint32_t mask)
{
	if (mask == 0u)
		return 32;
	int pfx = 0;
	while ((mask & 0x80000000u) != 0u && pfx < 32) {
		pfx++;
		mask <<= 1;
	}
	return pfx > 0 ? pfx : 32;
}

#define MARBLE_RMNET_IOCTL_SET_LLP_IP       0x000089F2u
#define MARBLE_RMNET_IOCTL_EXTENDED         0x000089FDu
#define MARBLE_RMNET_IOCTL_EXTENDED_V2      0x000089FEu
#define MARBLE_RMNET_IOCTL_GET_FEATURES     0x0000u
#define MARBLE_RMNET_IOCTL_GET_EPID         0x0003u
#define MARBLE_RMNET_IOCTL_ADD_MUX_CHANNEL  0x0005u
#define MARBLE_RMNET_IOCTL_SET_EGRESS_FMT   0x0006u
#define MARBLE_RMNET_IOCTL_SET_INGRESS_FMT  0x0007u
#define MARBLE_RMNET_IOCTL_GET_EP_PAIR      0x0010u
#define MARBLE_RMNET_IOCTL_SET_EGRESS_V2    0x0000u
#define MARBLE_RMNET_IOCTL_SET_INGRESS_V2   0x0001u
#define MARBLE_RMNET_EGRESS_DEFAULT         0x0000u
#define MARBLE_RMNET_INGRESS_DEFAULT        0x0001u
#define MARBLE_RMNET_DATA_FMT_MAP_MUX       0x0000000au
#define MARBLE_RMNET_INGRESS_FMT_AGG_DATA   (1u << 5)

#ifndef IFLA_RMNET_MUX_ID
#define IFLA_RMNET_MUX_ID 1
#endif
#ifndef IFLA_RMNET_FLAGS
#define IFLA_RMNET_FLAGS 2
#endif
#ifndef RMNET_FLAGS_INGRESS_DEAGGREGATION
#define RMNET_FLAGS_INGRESS_DEAGGREGATION (1U << 0)
#endif
#ifndef RMNET_FLAGS_INGRESS_MAP_COMMANDS
#define RMNET_FLAGS_INGRESS_MAP_COMMANDS (1U << 1)
#endif
#ifndef RMNET_PRIV_FLAGS_INGRESS_MAP_CKSUMV5
#define RMNET_PRIV_FLAGS_INGRESS_MAP_CKSUMV5 (1U << 5)
#endif

struct marble_rmnet_ioctl_data {
	uint32_t operation_mode;
};
_Static_assert(sizeof(struct marble_rmnet_ioctl_data) == 4, "rmnet_ioctl_data must be 4 bytes");

struct marble_rmnet_egress_param {
	uint32_t egress_ep_type;
	uint32_t pipe_setup_status;
	uint32_t cs_offload_en;
	uint32_t aggr_en;
	uint32_t ulso_en;
	uint32_t ipid_min_max_idx;
	uint32_t int_modt;
	uint32_t int_modc;
};
_Static_assert(sizeof(struct marble_rmnet_egress_param) == 32, "rmnet_egress_param must be 32 bytes");

struct marble_rmnet_ingress_param {
	uint32_t ingress_ep_type;
	uint32_t pipe_setup_status;
	uint32_t cs_offload_en;
	uint32_t buff_size;
	uint32_t agg_byte_limit;
	uint32_t agg_time_limit;
	uint32_t agg_pkt_limit;
	uint32_t int_modt;
	uint32_t int_modc;
	uint32_t padding;
};
_Static_assert(sizeof(struct marble_rmnet_ingress_param) == 40, "rmnet_ingress_param must be 40 bytes");

struct marble_egress_format_v2 {
	uint64_t egress_param_ptr;
	uint32_t egress_param_size;
	uint32_t number_of_eps;
};
_Static_assert(sizeof(struct marble_egress_format_v2) == 16, "egress_format_v2 must be 16 bytes");

struct marble_ingress_format_v2 {
	uint64_t ingress_param_ptr;
	uint32_t ingress_param_size;
	uint32_t number_of_eps;
};
_Static_assert(sizeof(struct marble_ingress_format_v2) == 16, "ingress_format_v2 must be 16 bytes");

struct marble_rmnet_ioctl_ext_v2 {
	uint64_t ioctl_ptr;
	uint32_t ioctl_data_size;
	uint32_t extended_v2_ioctl_type;
};
_Static_assert(sizeof(struct marble_rmnet_ioctl_ext_v2) == 16, "rmnet_ioctl_ext_v2 must be 16 bytes");

struct marble_rmnet_ioctl_ext {
	uint32_t extended_ioctl;
	union {
		uint32_t data;
		int8_t if_name[16];
		struct {
			uint32_t mux_id;
			int8_t vchannel_name[16];
		} rmnet_mux_val;
		struct {
			uint32_t consumer_pipe_num;
			uint32_t producer_pipe_num;
		} ipa_ep_pair;
		struct {
			uint32_t data;
			uint32_t agg_size;
			uint32_t agg_count;
		} ingress_format;
		struct {
			uint32_t mux_id;
			int8_t vchannel_name[16];
			uint8_t mac[6];
		} rmnet_mux_val_v2;
	} u;
};
_Static_assert(sizeof(struct marble_rmnet_ioctl_ext) == 32, "rmnet_ioctl_ext must be 32 bytes");
_Static_assert(offsetof(struct marble_rmnet_ioctl_ext, u) == 4, "rmnet_ioctl_ext.u offset");

struct marble_ifla_rmnet_flags {
	uint32_t flags;
	uint32_t mask;
};

static struct rtattr *nl_add_attr(struct nlmsghdr *nlh, size_t cap, uint16_t type,
				  const void *data, size_t len)
{
	size_t rtalen = RTA_LENGTH(len);
	if (NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rtalen) > cap)
		return NULL;
	struct rtattr *rta = (struct rtattr *)(((uint8_t *)nlh) + NLMSG_ALIGN(nlh->nlmsg_len));
	rta->rta_type = type;
	rta->rta_len = (uint16_t)rtalen;
	if (len > 0 && data != NULL)
		memcpy(RTA_DATA(rta), data, len);
	if (RTA_ALIGN(rtalen) > rtalen)
		memset(((uint8_t *)rta) + rtalen, 0, RTA_ALIGN(rtalen) - rtalen);
	nlh->nlmsg_len = (uint32_t)(NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rtalen));
	return rta;
}

static size_t build_rmnet_newlink_msg(uint8_t *buf, size_t cap, uint32_t seq,
				      int parent_ifindex, const char *devname,
				      uint16_t mux_id, uint32_t rmnet_flags, uint32_t rmnet_mask)
{
	if (!buf || cap < NLMSG_SPACE(sizeof(struct ifinfomsg)) || !devname || parent_ifindex <= 0)
		return 0;
	memset(buf, 0, cap);
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	nlh->nlmsg_len = (uint32_t)NLMSG_LENGTH(sizeof(struct ifinfomsg));
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
	nlh->nlmsg_seq = seq;

	struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
	ifi->ifi_family = AF_UNSPEC;

	uint32_t link_idx = (uint32_t)parent_ifindex;
	if (!nl_add_attr(nlh, cap, IFLA_LINK, &link_idx, sizeof(link_idx)))
		return 0;
	if (!nl_add_attr(nlh, cap, IFLA_IFNAME, devname, strlen(devname) + 1))
		return 0;

	struct rtattr *linkinfo = nl_add_attr(nlh, cap, IFLA_LINKINFO, NULL, 0);
	if (!linkinfo)
		return 0;
	if (!nl_add_attr(nlh, cap, IFLA_INFO_KIND, "rmnet", sizeof("rmnet")))
		return 0;

	struct rtattr *infodata = nl_add_attr(nlh, cap, IFLA_INFO_DATA, NULL, 0);
	if (!infodata)
		return 0;
	if (!nl_add_attr(nlh, cap, IFLA_RMNET_MUX_ID, &mux_id, sizeof(mux_id)))
		return 0;
	struct marble_ifla_rmnet_flags fl = { .flags = rmnet_flags, .mask = rmnet_mask };
	if (!nl_add_attr(nlh, cap, IFLA_RMNET_FLAGS, &fl, sizeof(fl)))
		return 0;

	infodata->rta_len = (uint16_t)(((uint8_t *)nlh + nlh->nlmsg_len) - (uint8_t *)infodata);
	linkinfo->rta_len = (uint16_t)(((uint8_t *)nlh + nlh->nlmsg_len) - (uint8_t *)linkinfo);
	return nlh->nlmsg_len;
}

static int netlink_exchange_ack(const uint8_t *msg, size_t msg_len, int *out_nl_err)
{
	int nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (nl_fd < 0)
		return -errno;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int e = -errno;
		close(nl_fd);
		return e;
	}
	struct sockaddr_nl dst = { .nl_family = AF_NETLINK };
	if (sendto(nl_fd, msg, msg_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
		int e = -errno;
		close(nl_fd);
		return e;
	}
	uint8_t rbuf[4096];
	ssize_t n = recv(nl_fd, rbuf, sizeof(rbuf), 0);
	int rc = 0;
	if (n < 0) {
		rc = -errno;
	} else {
		const struct nlmsghdr *nlh = (const struct nlmsghdr *)rbuf;
		if (NLMSG_OK(nlh, (size_t)n) && nlh->nlmsg_type == NLMSG_ERROR &&
		    nlh->nlmsg_len >= NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
			const struct nlmsgerr *ne = (const struct nlmsgerr *)NLMSG_DATA(nlh);
			if (out_nl_err)
				*out_nl_err = ne->error;
			rc = ne->error;
		}
	}
	close(nl_fd);
	return rc;
}

static int netdev_get_ifindex(int sock, const char *ifname)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0)
		return -errno;
	return ifr.ifr_ifindex;
}

static int netdev_set_up(int sock, const char *ifname)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0)
		return -errno;
	ifr.ifr_flags |= IFF_UP;
	if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0)
		return -errno;
	return 0;
}

static int netdev_set_mtu(int sock, const char *ifname, int mtu)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	ifr.ifr_mtu = mtu;
	if (ioctl(sock, SIOCSIFMTU, &ifr) < 0)
		return -errno;
	return 0;
}

static int netdev_set_ipv4_addr(int sock, const char *ifname, uint32_t ip_host,
				uint32_t mask_host, uint32_t gw_host, uint32_t host_route_dst)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(ip_host);
	if (ioctl(sock, SIOCSIFADDR, &ifr) < 0)
		return -errno;

	if (ip_host != 0u && mask_host != 0u) {
		memset(&ifr, 0, sizeof(ifr));
		snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
		sin = (struct sockaddr_in *)&ifr.ifr_netmask;
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = htonl(mask_host);
		(void)ioctl(sock, SIOCSIFNETMASK, &ifr);
	}

	if (ip_host != 0u && host_route_dst != 0u) {
		struct rtentry rt;
		memset(&rt, 0, sizeof(rt));
		rt.rt_dev = (char *)ifname;
		rt.rt_flags = RTF_UP | RTF_HOST;
		struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
		dst->sin_family = AF_INET;
		dst->sin_addr.s_addr = htonl(host_route_dst);
		struct sockaddr_in *msk = (struct sockaddr_in *)&rt.rt_genmask;
		msk->sin_family = AF_INET;
		msk->sin_addr.s_addr = htonl(0xffffffffu);
		if (gw_host != 0u) {
			struct sockaddr_in *gw = (struct sockaddr_in *)&rt.rt_gateway;
			gw->sin_family = AF_INET;
			gw->sin_addr.s_addr = htonl(gw_host);
			rt.rt_flags |= RTF_GATEWAY;
		}
		(void)ioctl(sock, SIOCADDRT, &rt);
	}
	return 0;
}

static void netdev_clear_ipv4_addr(int sock, const char *ifname, uint32_t gw_host, uint32_t host_route_dst)
{
	if (host_route_dst != 0u) {
		struct rtentry rt;
		memset(&rt, 0, sizeof(rt));
		rt.rt_dev = (char *)ifname;
		rt.rt_flags = RTF_UP | RTF_HOST;
		struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
		dst->sin_family = AF_INET;
		dst->sin_addr.s_addr = htonl(host_route_dst);
		struct sockaddr_in *msk = (struct sockaddr_in *)&rt.rt_genmask;
		msk->sin_family = AF_INET;
		msk->sin_addr.s_addr = htonl(0xffffffffu);
		if (gw_host != 0u) {
			struct sockaddr_in *gw = (struct sockaddr_in *)&rt.rt_gateway;
			gw->sin_family = AF_INET;
			gw->sin_addr.s_addr = htonl(gw_host);
			rt.rt_flags |= RTF_GATEWAY;
		}
		(void)ioctl(sock, SIOCDELRT, &rt);
	}
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = 0;
	(void)ioctl(sock, SIOCSIFADDR, &ifr);
}

static int ipa_ext_ioctl(int sock, const char *ifname, struct marble_rmnet_ioctl_ext *ext)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	ifr.ifr_data = (void *)ext;
	if (ioctl(sock, (unsigned long)MARBLE_RMNET_IOCTL_EXTENDED, &ifr) < 0)
		return -errno;
	return 0;
}

static int ipa_ext_v2_ioctl(int sock, const char *ifname, struct marble_rmnet_ioctl_ext_v2 *v2)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	ifr.ifr_data = (void *)v2;
	if (ioctl(sock, (unsigned long)MARBLE_RMNET_IOCTL_EXTENDED_V2, &ifr) < 0)
		return -errno;
	return 0;
}

static void netdev_disable_ipv6(const char *ifname)
{
	char path[128];
	snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/disable_ipv6", ifname);
	FILE *fp = fopen(path, "w");
	if (fp != NULL) {
		(void)fputs("1\n", fp);
		fclose(fp);
	}
}

static int run_ipa_setup(const char *parent_if, const char *devname, int mux_id, int mtu)
{
	int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		printf("IPA_SETUP_ERR socket_rc=%d\n", -errno);
		return 1;
	}
	int parent_idx = netdev_get_ifindex(sock, parent_if);
	if (parent_idx <= 0) {
		printf("IPA_SETUP_ERR parent=%s missing rc=%d\n", parent_if, parent_idx);
		close(sock);
		return 1;
	}

	netdev_disable_ipv6(parent_if);

	struct marble_rmnet_ioctl_ext ext;
	uint32_t features = 0, epid = 0, cons_pipe = 0, prod_pipe = 0;

	memset(&ext, 0, sizeof(ext));
	ext.extended_ioctl = MARBLE_RMNET_IOCTL_GET_FEATURES;
	if (ipa_ext_ioctl(sock, parent_if, &ext) == 0)
		features = ext.u.data;

	memset(&ext, 0, sizeof(ext));
	ext.extended_ioctl = MARBLE_RMNET_IOCTL_GET_EPID;
	if (ipa_ext_ioctl(sock, parent_if, &ext) == 0)
		epid = ext.u.data;

	memset(&ext, 0, sizeof(ext));
	ext.extended_ioctl = MARBLE_RMNET_IOCTL_GET_EP_PAIR;
	if (ipa_ext_ioctl(sock, parent_if, &ext) == 0) {
		cons_pipe = ext.u.ipa_ep_pair.consumer_pipe_num;
		prod_pipe = ext.u.ipa_ep_pair.producer_pipe_num;
	}
	printf("IPA_SETUP_EP epid=%u cons=%u prod=%u features=0x%x\n",
	       epid, cons_pipe, prod_pipe, features);

	struct marble_rmnet_ioctl_data llp_d = { .operation_mode = 1u };
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", parent_if);
	ifr.ifr_data = (void *)&llp_d;
	int llp_rc = (ioctl(sock, (unsigned long)MARBLE_RMNET_IOCTL_SET_LLP_IP, &ifr) < 0)
		   ? -errno : 0;

	/* Prefer IPA v5.0 EXTENDED_V2 with QMAPv5 DL csum (8B header) matching IPA_ENABLE_CS_DL_QMAP. */
	struct marble_rmnet_ingress_param ing_p;
	memset(&ing_p, 0, sizeof(ing_p));
	ing_p.ingress_ep_type = MARBLE_RMNET_INGRESS_DEFAULT;
	ing_p.cs_offload_en = 1u;
	ing_p.buff_size = 8192u;
	ing_p.agg_byte_limit = 7500u;
	ing_p.agg_time_limit = 500u;
	ing_p.agg_pkt_limit = 16u;
	ing_p.int_modt = 32u;
	ing_p.int_modc = 1u;

	struct marble_ingress_format_v2 ing_v2;
	ing_v2.ingress_param_ptr = (uint64_t)(uintptr_t)&ing_p;
	ing_v2.ingress_param_size = (uint32_t)sizeof(ing_p);
	ing_v2.number_of_eps = 1u;

	struct marble_rmnet_ioctl_ext_v2 v2;
	memset(&v2, 0, sizeof(v2));
	v2.ioctl_ptr = (uint64_t)(uintptr_t)&ing_v2;
	v2.ioctl_data_size = (uint32_t)sizeof(ing_v2);
	v2.extended_v2_ioctl_type = MARBLE_RMNET_IOCTL_SET_INGRESS_V2;
	int ing_v2_used = 1;
	int ing_rc = ipa_ext_v2_ioctl(sock, parent_if, &v2);
	if (ing_rc == -EPERM && ing_p.pipe_setup_status == 2u) {
		/* WAN RT table already set up on repeated --ipa-setup invocation */
		ing_rc = 0;
	} else if (ing_rc == -EINVAL || ing_rc == -ENOTTY) {
		ing_v2_used = 0;
		memset(&ext, 0, sizeof(ext));
		ext.extended_ioctl = MARBLE_RMNET_IOCTL_SET_INGRESS_FMT;
		ext.u.ingress_format.data = MARBLE_RMNET_DATA_FMT_MAP_MUX | MARBLE_RMNET_INGRESS_FMT_AGG_DATA;
		ext.u.ingress_format.agg_size = 7500u;
		ext.u.ingress_format.agg_count = 16u;
		ing_rc = ipa_ext_ioctl(sock, parent_if, &ext);
	}

	struct marble_rmnet_egress_param egr_p;
	memset(&egr_p, 0, sizeof(egr_p));
	egr_p.egress_ep_type = MARBLE_RMNET_EGRESS_DEFAULT;
	egr_p.cs_offload_en = 0u;
	egr_p.aggr_en = 0u;
	egr_p.ulso_en = 0u;
	egr_p.int_modt = 32u;
	egr_p.int_modc = 1u;

	struct marble_egress_format_v2 egr_v2;
	egr_v2.egress_param_ptr = (uint64_t)(uintptr_t)&egr_p;
	egr_v2.egress_param_size = (uint32_t)sizeof(egr_p);
	egr_v2.number_of_eps = 1u;

	memset(&v2, 0, sizeof(v2));
	v2.ioctl_ptr = (uint64_t)(uintptr_t)&egr_v2;
	v2.ioctl_data_size = (uint32_t)sizeof(egr_v2);
	v2.extended_v2_ioctl_type = MARBLE_RMNET_IOCTL_SET_EGRESS_V2;
	int egr_v2_used = 1;
	int egr_rc = ipa_ext_v2_ioctl(sock, parent_if, &v2);
	if (egr_rc == -EINVAL || egr_rc == -ENOTTY) {
		egr_v2_used = 0;
		memset(&ext, 0, sizeof(ext));
		ext.extended_ioctl = MARBLE_RMNET_IOCTL_SET_EGRESS_FMT;
		ext.u.data = MARBLE_RMNET_DATA_FMT_MAP_MUX;
		egr_rc = ipa_ext_ioctl(sock, parent_if, &ext);
	}

	memset(&ext, 0, sizeof(ext));
	ext.extended_ioctl = MARBLE_RMNET_IOCTL_ADD_MUX_CHANNEL;
	ext.u.rmnet_mux_val.mux_id = (uint32_t)mux_id;
	snprintf((char *)ext.u.rmnet_mux_val.vchannel_name,
		 sizeof(ext.u.rmnet_mux_val.vchannel_name), "%s", devname);
	int mux_rc = ipa_ext_ioctl(sock, parent_if, &ext);
	if (mux_rc == -EEXIST)
		mux_rc = 0;

	printf("IPA_SETUP_IOCTL if=%s llp=%d ing=%d(v2=%d,st=%u) egr=%d(v2=%d,st=%u) mux=%d\n",
	       parent_if, llp_rc, ing_rc, ing_v2_used, ing_p.pipe_setup_status,
	       egr_rc, egr_v2_used, egr_p.pipe_setup_status, mux_rc);

	int up_parent = netdev_set_up(sock, parent_if);
	int nl_rc = 0;
	if (netdev_get_ifindex(sock, devname) <= 0) {
		uint32_t rmnet_flags = RMNET_FLAGS_INGRESS_DEAGGREGATION |
				       RMNET_FLAGS_INGRESS_MAP_COMMANDS |
				       RMNET_PRIV_FLAGS_INGRESS_MAP_CKSUMV5;
		uint8_t nbuf[512];
		size_t nlen = build_rmnet_newlink_msg(
			nbuf, sizeof(nbuf), 1u, parent_idx, devname, (uint16_t)mux_id,
			rmnet_flags, rmnet_flags);
		int nl_err = 0;
		nl_rc = netlink_exchange_ack(nbuf, nlen, &nl_err);
		if (nl_rc == -EEXIST)
			nl_rc = 0;
	}

	int child_idx = netdev_get_ifindex(sock, devname);
	if (child_idx > 0)
		netdev_disable_ipv6(devname);
	int mtu_rc = (child_idx > 0 && mtu > 0) ? netdev_set_mtu(sock, devname, mtu) : -ENODEV;
	int up_child = (child_idx > 0) ? netdev_set_up(sock, devname) : -ENODEV;
	int final_rc = (up_parent == 0 && nl_rc == 0 && child_idx > 0 && up_child == 0) ? 0 : 1;
	printf("IPA_SETUP_NETDEV parent=%s dev=%s ifindex=%d mux_id=%d mtu=%d up_p=%d nl=%d mtu_rc=%d up_c=%d rc=%d\n",
	       parent_if, devname, child_idx, mux_id, mtu, up_parent, nl_rc, mtu_rc, up_child, final_rc);
	close(sock);
	return final_rc;
}

static size_t build_nas_set_dsb_tlvs(uint8_t *req, uint8_t standby_pref,
				     uint8_t pri_sub, uint8_t data_sub,
				     uint8_t voice_sub, uint64_t active_mask)
{
	uint8_t mbuf[8];
	probe_put_le64(mbuf, active_mask);
	size_t qlen = tlv_append(req, 0x10, &standby_pref, 1);
	qlen += tlv_append(req + qlen, 0x11, &pri_sub, 1);
	qlen += tlv_append(req + qlen, 0x12, &data_sub, 1);
	qlen += tlv_append(req + qlen, 0x13, &voice_sub, 1);
	qlen += tlv_append(req + qlen, 0x14, mbuf, 8);
	return qlen;
}

struct nas_dsb_pref {
	uint8_t standby;
	uint8_t pri;
	uint8_t active;
	uint8_t data;
	uint8_t voice;
	uint64_t mask;
	bool has_any;
};

static void parse_nas_dsb_pref(const uint8_t *msg, size_t len, struct nas_dsb_pref *out)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	memset(out, 0, sizeof(*out));
	out->standby = out->pri = out->active = out->data = out->voice = 255;
	if (!msg || len < QMI_HDR_LEN)
		return;
	if (qmi_find_tlv(msg, len, 0x10, &v, &vl) == 0 && vl >= 1) {
		out->standby = v[0];
		out->has_any = true;
	}
	if (qmi_find_tlv(msg, len, 0x11, &v, &vl) == 0 && vl >= 1) {
		out->pri = v[0];
		out->has_any = true;
	}
	if (qmi_find_tlv(msg, len, 0x12, &v, &vl) == 0 && vl >= 1) {
		out->active = v[0];
		out->has_any = true;
	}
	if (qmi_find_tlv(msg, len, 0x13, &v, &vl) == 0 && vl >= 1) {
		out->data = v[0];
		out->has_any = true;
	}
	if (qmi_find_tlv(msg, len, 0x14, &v, &vl) == 0 && vl >= 1) {
		out->voice = v[0];
		out->has_any = true;
	}
	if (qmi_find_tlv(msg, len, 0x15, &v, &vl) == 0 && vl >= 8) {
		out->mask = get_le64(v);
		out->has_any = true;
	}
}

static size_t build_uim_change_prov_tlvs(uint8_t *req, uint8_t sess_type, uint8_t activate,
					 uint8_t slot_1based, const uint8_t *aid, uint8_t aid_len)
{
	uint8_t sc[2] = { sess_type, activate };
	size_t qlen = tlv_append(req, 0x01, sc, sizeof(sc));
	if (aid_len > 32)
		aid_len = 32;
	uint8_t app[34];
	app[0] = slot_1based;
	app[1] = aid_len;
	if (aid_len > 0 && aid)
		memcpy(app + 2, aid, aid_len);
	qlen += tlv_append(req + qlen, 0x10, app, (uint16_t)(2 + aid_len));
	return qlen;
}

static int uim_detect_active_apps(struct qrtr_client *c, int *active_slot,
				  uint8_t usim_aid[32], uint8_t *usim_alen,
				  uint8_t csim_aid[32], uint8_t *csim_alen)
{
	struct endpoint ep = {0};
	uint8_t card_resp[QMI_MAX_BUF];
	size_t cl = 0;
	uint16_t cerr = 0;
	*active_slot = 0;
	*usim_alen = 0;
	*csim_alen = 0;
	if (!find_remote_endpoint(c, SVC_UIM, &ep))
		return -ENODEV;
	int rc = qmi_rpc(c, &ep, 45, QMI_UIM_GET_CARD_STATUS, card_resp, sizeof(card_resp), &cl, &cerr);
	if (rc != 0 || cerr != 0)
		return -EIO;
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (qmi_find_tlv(card_resp, cl, 0x10, &v, &vl) != 0 || vl < 9)
		return -EPROTO;
	uint8_t num_cards = v[8];
	size_t pos = 9;
	bool found_present = false;
	for (uint8_t slot = 0; slot < num_cards && vl - pos >= 6; slot++) {
		uint8_t cstate = v[pos + 0];
		uint8_t napps = v[pos + 5];
		pos += 6;
		if (cstate == 1 && !found_present) {
			*active_slot = slot;
			found_present = true;
		}
		for (uint8_t ai = 0; ai < napps && vl - pos >= 7; ai++) {
			uint8_t atype = v[pos + 0];
			uint8_t alen = v[pos + 6];
			pos += 7;
			if (vl - pos < (size_t)alen + 7)
				return 0;
			if (cstate == 1 && slot == *active_slot && alen <= 32) {
				if (atype == 2 && *usim_alen == 0) {
					memcpy(usim_aid, v + pos, alen);
					*usim_alen = alen;
				} else if (atype == 4 && *csim_alen == 0) {
					memcpy(csim_aid, v + pos, alen);
					*csim_alen = alen;
				}
			}
			pos += alen + 7;
		}
	}
	return found_present ? 0 : -ENOENT;
}

static size_t build_loc_inform_rev_tlvs(uint8_t *req, uint32_t rev)
{
	uint8_t rbuf[4];
	probe_put_le32(rbuf, rev);
	return tlv_append(req, 0x01, rbuf, 4);
}

static size_t build_loc_reg_events_tlvs(uint8_t *req, uint64_t mask, const char *client_str,
					uint32_t client_type, uint8_t enable_notif)
{
	uint8_t mbuf[8], cbuf[4];
	probe_put_le64(mbuf, mask);
	probe_put_le32(cbuf, client_type);
	size_t qlen = tlv_append(req, 0x01, mbuf, 8);
	if (client_str && client_str[0] != '\0') {
		size_t slen = strlen(client_str);
		if (slen > 4)
			slen = 4;
		qlen += tlv_append(req + qlen, 0x10, (const uint8_t *)client_str, (uint16_t)slen);
	}
	qlen += tlv_append(req + qlen, 0x11, cbuf, 4);
	qlen += tlv_append(req + qlen, 0x12, &enable_notif, 1);
	return qlen;
}

static size_t build_loc_start_tlvs(uint8_t *req, uint8_t sess_id, uint32_t fix_rec,
				   uint32_t intermed, uint32_t min_intv)
{
	uint8_t rec_buf[4], int_buf[4], iv_buf[4];
	probe_put_le32(rec_buf, fix_rec);
	probe_put_le32(int_buf, intermed);
	probe_put_le32(iv_buf, min_intv);
	size_t qlen = tlv_append(req, 0x01, &sess_id, 1);
	qlen += tlv_append(req + qlen, 0x10, rec_buf, 4);
	qlen += tlv_append(req + qlen, 0x12, int_buf, 4);
	qlen += tlv_append(req + qlen, 0x13, iv_buf, 4);
	return qlen;
}

static int run_dms_raw(struct qrtr_client *c, int sub_id)
{
	struct endpoint ep = {0};
	uint8_t resp[2048], ind[2048], req[32];
	size_t rl = 0, il = 0;
	uint16_t err = 0;
	int fail = 0;

	if (!find_remote_endpoint(c, SVC_DMS, &ep)) {
		printf("DMS_RAW err=no_dms_endpoint\n");
		return 1;
	}

	/* 1. QMI_DMS_GET_DEVICE_CAP (0x0020) */
	int rc = qmi_rpc_tlv_ex(c, &ep, 51, QMI_DMS_GET_DEVICE_CAP, NULL, 0,
				resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	printf("DMS_CAP_RPC rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
	if (rc == 0)
		dump_tlv_hex("DMS_RAW", QMI_DMS_GET_DEVICE_CAP, resp, rl);
	if (rc == 0 && err == 0) {
		uint32_t tx = 0, rx = 0;
		uint8_t dcap = 0, scap = 0, nifs = 0;
		if (parse_dms_cap(resp, rl, &tx, &rx, &dcap, &scap, &nifs) == 0) {
			const uint8_t *v = NULL;
			uint16_t vl = 0;
			(void)qmi_find_tlv(resp, rl, 0x01, &v, &vl);
			printf("DMS_CAP tx=%u rx=%u data=%u sim=%u ifs=%u:[", tx, rx, dcap, scap, nifs);
			for (uint8_t i = 0; v && i < nifs && 11 + i < vl; i++)
				printf("%s%u", i ? "," : "", v[11 + i]);
			printf("]\n");
		}
	} else {
		fail = 1;
	}

	/* 2. QMI_DMS_GET_OPERATING_MODE (0x002d) + offline/hw-restricted TLVs */
	rc = qmi_rpc_tlv_ex(c, &ep, 52, QMI_DMS_GET_OPERATING_MODE, NULL, 0,
			    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	printf("DMS_OP_RPC rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
	if (rc == 0)
		dump_tlv_hex("DMS_RAW", QMI_DMS_GET_OPERATING_MODE, resp, rl);
	if (rc == 0 && err == 0) {
		uint8_t mode = 0xff, hw = 0;
		uint16_t off = 0;
		bool hoff = false, hhw = false;
		if (parse_dms_op_mode_raw(resp, rl, &mode, &off, &hoff, &hw, &hhw) == 0) {
			printf("DMS_OP_DETAIL mode=%u(%s) offline_valid=%d offline=0x%04x hw_valid=%d hw=%u\n",
			       mode, op_mode_name(mode), hoff ? 1 : 0, off, hhw ? 1 : 0, hw);
		}
	}

	/* 3. QMI_DMS_GET_REV_ID (0x0023) */
	rc = qmi_rpc_tlv_ex(c, &ep, 53, QMI_DMS_GET_REV_ID, NULL, 0,
			    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	if (rc == 0 && err == 0) {
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		char rev[96] = "";
		if (qmi_find_tlv(resp, rl, 0x01, &v, &vl) == 0)
			format_ascii(v, vl, rev, sizeof(rev));
		printf("DMS_REV_ID rc=0 rev=\"%s\"\n", rev);
	}

	/* 3b. QMI_DMS_GET_BAND_CAPABILITY (0x0045) */
	rc = qmi_rpc_tlv_ex(c, &ep, 54, QMI_DMS_GET_BAND_CAPABILITY, NULL, 0,
			    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	printf("DMS_BAND_CAP_RPC rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
	if (rc == 0)
		dump_tlv_hex("DMS_BAND_CAP", QMI_DMS_GET_BAND_CAPABILITY, resp, rl);

	/* 3c. QMI_DMS_GET_ACTIVATION_STATE (0x002f) */
	rc = qmi_rpc_tlv_ex(c, &ep, 55, QMI_DMS_GET_ACTIVATION_STATE, NULL, 0,
			    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	printf("DMS_ACT_RPC rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
	if (rc == 0)
		dump_tlv_hex("DMS_ACT_STATE", QMI_DMS_GET_ACTIVATION_STATE, resp, rl);

	/* 4. QMI_NAS_GET_RF_BAND_INFO (0x0031), GET_SYS_INFO (0x004d), GET_SYS_SEL_PREF (0x0034), GET_DUAL_STANDBY_PREF (0x005c) */
	struct endpoint nas_ep = {0};
	if (find_remote_endpoint(c, SVC_NAS, &nas_ep)) {
		if (sub_id >= 0)
			(void)nas_bind_sub(c, &nas_ep, 55, sub_id);
		rc = qmi_rpc_tlv_ex(c, &nas_ep, 56, QMI_NAS_GET_RF_BAND_INFO, NULL, 0,
				    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
		printf("NAS_RF_BAND_RPC rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
		if (rc == 0)
			dump_tlv_hex("NAS_RF_BAND", QMI_NAS_GET_RF_BAND_INFO, resp, rl);

		rc = qmi_rpc_tlv_ex(c, &nas_ep, 57, QMI_NAS_GET_SYS_INFO, NULL, 0,
				    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
		printf("NAS_SYS_INFO_RPC rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
		if (rc == 0)
			dump_tlv_hex("NAS_SYS_INFO", QMI_NAS_GET_SYS_INFO, resp, rl);

		rc = qmi_rpc_tlv_ex(c, &nas_ep, 58, QMI_NAS_GET_DUAL_STANDBY_PREF, NULL, 0,
				    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
		printf("NAS_DSB_PREF_RPC rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
		if (rc == 0) {
			struct nas_dsb_pref dsb;
			parse_nas_dsb_pref(resp, rl, &dsb);
			if (dsb.has_any) {
				printf("NAS_DSB_PREF standby=%u pri=%u active=%u data=%u voice=%u active_mask=0x%llx\n",
				       dsb.standby, dsb.pri, dsb.active, dsb.data, dsb.voice,
				       (unsigned long long)dsb.mask);
			}
			dump_tlv_hex("NAS_DSB_PREF_RAW", QMI_NAS_GET_DUAL_STANDBY_PREF, resp, rl);
		}

		rc = qmi_rpc_tlv_ex(c, &nas_ep, 59, QMI_NAS_GET_SYS_SEL_PREF, NULL, 0,
				    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
		printf("NAS_SYS_SEL_PREF_RPC rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
		if (rc == 0)
			dump_tlv_hex("NAS_SYS_SEL_PREF", QMI_NAS_GET_SYS_SEL_PREF, resp, rl);
	} else {
		printf("NAS_RAW err=no_nas_endpoint\n");
	}

	/* 4b. QMI_UIM_GET_SLOT_STATUS (0x0047) */
	struct endpoint uim_ep = {0};
	if (find_remote_endpoint(c, SVC_UIM, &uim_ep)) {
		rc = qmi_rpc_tlv_ex(c, &uim_ep, 60, QMI_UIM_GET_SLOT_STATUS, NULL, 0,
				    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
		printf("UIM_SLOT_STATUS_RPC rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
		if (rc == 0)
			dump_tlv_hex("UIM_SLOT_STATUS", QMI_UIM_GET_SLOT_STATUS, resp, rl);
	}

	/* 5. QMI_PDC_LIST_CONFIGS (0x0024) and QMI_PDC_GET_SELECTED_CONFIG (0x0022) for HW (0) & SW (1) */
	struct endpoint pdc_ep = {0};
	if (find_remote_endpoint(c, SVC_PDC, &pdc_ep)) {
		uint32_t ctypes[2] = { 0u, 1u };
		for (int i = 0; i < 2; i++) {
			size_t qlen = build_pdc_list_tlvs(req, (uint32_t)(1u + i), ctypes[i]);
			rc = qmi_rpc_tlv_ex(c, &pdc_ep, (uint16_t)(61 + i), QMI_PDC_LIST_CONFIGS,
					    req, qlen, resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
			uint32_t cnt = 0;
			int prc = parse_pdc_list_configs(resp, rl, ind, il, &cnt);
			printf("PDC_LIST type=%u rc=%d err=0x%x(%s) parse=%d count=%u rl=%zu il=%zu\n",
			       ctypes[i], rc, err, qmi_err_name(err), prc, cnt, rl, il);
			if (rc == 0 && rl > QMI_HDR_LEN)
				dump_tlv_hex("PDC_LIST_RESP", QMI_PDC_LIST_CONFIGS, resp, rl);
			if (rc == 0 && il > QMI_HDR_LEN)
				dump_tlv_hex("PDC_IND", QMI_PDC_LIST_CONFIGS, ind, il);
		}
		for (int i = 0; i < 2; i++) {
			size_t qlen = build_pdc_selected_sub_tlvs(req, ctypes[i], (uint32_t)(3u + i),
								  sub_id >= 0, sub_id >= 0 ? (uint32_t)sub_id : 0u);
			rc = qmi_rpc_tlv_ex(c, &pdc_ep, (uint16_t)(63 + i), QMI_PDC_GET_SELECTED_CONFIG,
					    req, qlen, resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
			printf("PDC_SELECTED type=%u sub=%d rc=%d err=0x%x(%s) rl=%zu il=%zu\n",
			       ctypes[i], sub_id, rc, err, qmi_err_name(err), rl, il);
			if (rc == 0 && rl > QMI_HDR_LEN)
				dump_tlv_hex("PDC_SEL_RESP", QMI_PDC_GET_SELECTED_CONFIG, resp, rl);
			if (rc == 0 && il > QMI_HDR_LEN)
				dump_tlv_hex("PDC_SEL_IND", QMI_PDC_GET_SELECTED_CONFIG, ind, il);
		}
	} else {
		printf("PDC_LIST err=no_pdc_endpoint\n");
	}
	return fail;
}

static void print_hex_id(const uint8_t *id, uint8_t len)
{
	for (uint8_t i = 0; i < len; i++)
		printf("%02x", id[i]);
}

static int parse_hex_bytes(const char *hex, uint8_t *out, uint8_t cap, uint8_t *out_len)
{
	size_t n = hex ? strlen(hex) : 0;
	if (n == 0 || (n % 2) != 0 || n / 2 > cap)
		return -EINVAL;
	for (size_t i = 0; i < n / 2; i++) {
		unsigned int b = 0;
		if (sscanf(hex + 2 * i, "%02x", &b) != 1)
			return -EINVAL;
		out[i] = (uint8_t)b;
	}
	*out_len = (uint8_t)(n / 2);
	return 0;
}

static void pdc_query_and_print_info(struct qrtr_client *c, const struct endpoint *ep, uint16_t txn,
				     uint32_t ctype, const uint8_t *id, uint8_t id_len,
				     const char *prefix)
{
	uint8_t req[128], resp[1024], ind[2048];
	size_t rl = 0, il = 0;
	uint16_t err = 0;
	size_t qlen = build_pdc_config_info_tlvs(req, ctype, id, id_len, (uint32_t)(100u + txn));
	int rc = qmi_rpc_tlv_ex(c, ep, txn, QMI_PDC_GET_CONFIG_INFO, req, qlen,
				resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	const uint8_t *src = (il > QMI_HDR_LEN) ? ind : resp;
	size_t sl = (il > QMI_HDR_LEN) ? il : rl;
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	uint32_t ver = 0, sz = 0;
	char desc[128] = "";
	if (rc == 0 && err == 0 && src && sl > QMI_HDR_LEN) {
		if (qmi_find_tlv(src, sl, 0x12, &v, &vl) == 0 && vl == 4)
			ver = get_le32(v);
		if (qmi_find_tlv(src, sl, 0x13, &v, &vl) == 0 && vl == 4)
			sz = get_le32(v);
		if (qmi_find_tlv(src, sl, 0x14, &v, &vl) == 0 && vl >= 2) {
			uint8_t dlen = v[0];
			if ((size_t)(1 + dlen) <= vl)
				format_ascii(v + 1, dlen, desc, sizeof(desc));
			else
				format_ascii(v, vl, desc, sizeof(desc));
		}
	}
	printf("%s type=%u id=", prefix, ctype);
	print_hex_id(id, id_len);
	printf(" rc=%d err=0x%x(%s) ver=0x%x size=%u desc=\"%s\"\n",
	       rc, err, qmi_err_name(err), ver, sz, desc);
	if (rc == 0 && err == 0 && il > QMI_HDR_LEN && desc[0] == '\0')
		dump_tlv_hex("PDC_INFO_IND", QMI_PDC_GET_CONFIG_INFO, ind, il);
}

static int run_pdc_info(struct qrtr_client *c, int sub_id)
{
	struct endpoint pdc_ep = {0};
	uint8_t req[64], resp[2048], ind[4096];
	size_t rl = 0, il = 0;
	uint16_t err = 0;

	if (!find_remote_endpoint(c, SVC_PDC, &pdc_ep)) {
		printf("PDC_INFO err=no_pdc_endpoint\n");
		return 1;
	}

	uint16_t txn = 120;
	int subs[2] = { 0, 1 };
	int nsubs = (sub_id >= 0) ? 1 : 2;
	if (sub_id >= 0)
		subs[0] = sub_id;

	for (int si = 0; si < nsubs; si++) {
		for (uint32_t ctype = 0; ctype <= 1; ctype++) {
			size_t qlen = build_pdc_selected_sub_tlvs(req, ctype, (uint32_t)(10u + txn),
								  true, (uint32_t)subs[si]);
			int rc = qmi_rpc_tlv_ex(c, &pdc_ep, txn++, QMI_PDC_GET_SELECTED_CONFIG,
						req, qlen, resp, sizeof(resp), &rl, &err,
						ind, sizeof(ind), &il);
			uint8_t id[64] = {0};
			uint8_t id_len = 0;
			if (rc == 0 && err == 0 &&
			    parse_pdc_selected_id(resp, rl, ind, il, id, &id_len) == 0) {
				char pfx[32];
				snprintf(pfx, sizeof(pfx), "PDC_SEL_SUB%d", subs[si]);
				pdc_query_and_print_info(c, &pdc_ep, txn++, ctype, id, id_len, pfx);
			} else {
				printf("PDC_SEL_SUB%d type=%u rc=%d err=0x%x(%s) id=none\n",
				       subs[si], ctype, rc, err, qmi_err_name(err));
			}
		}
	}

	size_t qlen = build_pdc_list_tlvs(req, 99u, 1u);
	int rc = qmi_rpc_tlv_ex(c, &pdc_ep, txn++, QMI_PDC_LIST_CONFIGS,
				req, qlen, resp, sizeof(resp), &rl, &err,
				ind, sizeof(ind), &il);
	struct pdc_list_entry entries[32];
	size_t cnt = 0;
	if (rc != 0 || err != 0 || parse_pdc_list_entries(resp, rl, ind, il, entries, 32, &cnt) < 0) {
		printf("PDC_LIST_INFO rc=%d err=0x%x(%s) cnt=0\n", rc, err, qmi_err_name(err));
		return 1;
	}
	printf("PDC_LIST_INFO type=1 count=%zu\n", cnt);
	for (size_t i = 0; i < cnt; i++) {
		char pfx[24];
		snprintf(pfx, sizeof(pfx), "PDC_SW[%zu]", i);
		pdc_query_and_print_info(c, &pdc_ep, txn++, entries[i].type,
					 entries[i].id, entries[i].id_len, pfx);
	}
	return 0;
}

static int run_pdc_select_sw(struct qrtr_client *c, const char *hex_id, int sub_id)
{
	struct endpoint pdc_ep = {0};
	uint8_t id[64];
	uint8_t id_len = 0;
	int use_sub = (sub_id >= 0) ? sub_id : 1;
	if (parse_hex_bytes(hex_id, id, sizeof(id), &id_len) < 0) {
		printf("PDC_SELECT_ERR invalid_hex=%s\n", hex_id ? hex_id : "null");
		return 2;
	}
	if (!find_remote_endpoint(c, SVC_PDC, &pdc_ep)) {
		printf("PDC_SELECT_ERR no_pdc_endpoint\n");
		return 1;
	}
	uint8_t req[128], resp[1024], ind[1024];
	size_t rl = 0, il = 0;
	uint16_t err = 0;
	size_t qlen = build_pdc_select_tlvs(req, 1u, id, id_len, 201u, true, (uint32_t)use_sub);
	int rc1 = qmi_rpc_tlv_ex(c, &pdc_ep, 201u, QMI_PDC_SELECT_CONFIG,
				 req, qlen, resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	printf("PDC_SELECT sub=%d rc=%d err=0x%x(%s) il=%zu\n", use_sub, rc1, err, qmi_err_name(err), il);
	if (rc1 != 0 || err != 0)
		return 1;

	qlen = build_pdc_activate_tlvs(req, 1u, 202u, true, (uint32_t)use_sub);
	int rc2 = qmi_rpc_tlv_ex(c, &pdc_ep, 202u, QMI_PDC_ACTIVATE_CONFIG,
				 req, qlen, resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	printf("PDC_ACTIVATE sub=%d rc=%d err=0x%x(%s) il=%zu\n", use_sub, rc2, err, qmi_err_name(err), il);
	return (rc2 == 0 && err == 0) ? 0 : 1;
}

static int run_wds_profiles(struct qrtr_client *c, int sub_id)
{
	struct endpoint wds_ep = {0};
	uint8_t req[64], resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t err = 0;
	int use_sub = (sub_id >= 0) ? sub_id : 1;

	if (!find_remote_endpoint(c, SVC_WDS, &wds_ep)) {
		printf("WDS_PROFILES err=no_wds_endpoint\n");
		return 1;
	}
	size_t qlen = build_wds_bind_sub_tlvs(req, (uint32_t)use_sub);
	int brc = qmi_rpc_tlv(c, &wds_ep, 210, QMI_WDS_BIND_SUBSCRIPTION, req, qlen, resp, sizeof(resp), &rl, &err);
	printf("WDS_BIND_SUB sub=%d rc=%d err=0x%x(%s)\n", use_sub, brc, err, qmi_err_name(err));

	qlen = build_wds_profile_list_tlvs(req, 0u);
	int rc = qmi_rpc_tlv(c, &wds_ep, 211, QMI_WDS_GET_PROFILE_LIST, req, qlen, resp, sizeof(resp), &rl, &err);
	printf("WDS_PROFILE_LIST rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));
	if (rc != 0 || err != 0)
		return 1;

	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if ((qmi_find_tlv(resp, rl, 0x01, &v, &vl) != 0 &&
	     qmi_find_tlv(resp, rl, 0x10, &v, &vl) != 0) || vl < 1) {
		dump_tlv_hex("WDS_PROFILE_LIST_RAW", QMI_WDS_GET_PROFILE_LIST, resp, rl);
		return 0;
	}
	uint8_t cnt = v[0];
	size_t pos = 1;
	uint16_t txn = 212;
	for (uint8_t i = 0; i < cnt && pos + 3 <= vl; i++) {
		uint8_t ptype = v[pos];
		uint8_t pidx = v[pos + 1];
		uint8_t nlen = v[pos + 2];
		pos += 3;
		if (pos + nlen > vl)
			break;
		char pname[64] = "";
		format_ascii(v + pos, nlen, pname, sizeof(pname));
		pos += nlen;

		uint8_t sresp[QMI_MAX_BUF];
		size_t srl = 0;
		uint16_t serr = 0;
		size_t slen = build_wds_profile_settings_tlvs(req, ptype, pidx);
		char apn[96] = "";
		uint8_t pdp = 255;
		if (qmi_rpc_tlv(c, &wds_ep, txn++, QMI_WDS_GET_PROFILE_SETTINGS,
				req, slen, sresp, sizeof(sresp), &srl, &serr) == 0 && serr == 0) {
			const uint8_t *sv = NULL;
			uint16_t svl = 0;
			if (qmi_find_tlv(sresp, srl, 0x14, &sv, &svl) == 0)
				format_ascii(sv, svl, apn, sizeof(apn));
			if (qmi_find_tlv(sresp, srl, 0x11, &sv, &svl) == 0 && svl >= 1)
				pdp = sv[0];
		}
		printf("WDS_PROFILE ptype=%u idx=%u pdp=%u name=\"%s\" apn=\"%s\"\n",
		       ptype, pidx, pdp, pname, apn);
	}
	return 0;
}

static int run_dpm_open(struct qrtr_client *c, int ep_type, int iface_id, uint32_t rx_ep, uint32_t tx_ep)
{
	struct endpoint dpm_ep = {0};
	uint8_t req[64], resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t err = 0;
	uint32_t use_iface = (iface_id >= 0) ? (uint32_t)iface_id : 1u;

	if (rx_ep == 0u || tx_ep == 0u) {
		int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (sock >= 0) {
			struct marble_rmnet_ioctl_ext ext;
			if (iface_id < 0) {
				memset(&ext, 0, sizeof(ext));
				ext.extended_ioctl = MARBLE_RMNET_IOCTL_GET_EPID;
				if (ipa_ext_ioctl(sock, "rmnet_ipa0", &ext) == 0 && ext.u.data > 0)
					use_iface = ext.u.data;
			}
			memset(&ext, 0, sizeof(ext));
			ext.extended_ioctl = MARBLE_RMNET_IOCTL_GET_EP_PAIR;
			if (ipa_ext_ioctl(sock, "rmnet_ipa0", &ext) == 0) {
				if (rx_ep == 0u)
					rx_ep = ext.u.ipa_ep_pair.consumer_pipe_num;
				if (tx_ep == 0u)
					tx_ep = ext.u.ipa_ep_pair.producer_pipe_num;
			}
			close(sock);
		}
		if (rx_ep == 0u)
			rx_ep = 2u;
		if (tx_ep == 0u)
			tx_ep = 23u;
	}

	if (!find_remote_endpoint(c, SVC_DPM, &dpm_ep)) {
		printf("DPM_OPEN_PORT err=no_dpm_endpoint\n");
		return 1;
	}
	size_t qlen = build_dpm_open_port_tlvs(req, (uint32_t)ep_type, use_iface, rx_ep, tx_ep);
	int rc = qmi_rpc_tlv(c, &dpm_ep, 238, QMI_DPM_OPEN_PORT, req, qlen, resp, sizeof(resp), &rl, &err);
	printf("DPM_OPEN_PORT ep=%d iface=%u rx=%u tx=%u rc=%d err=0x%x(%s)\n",
	       ep_type, use_iface, rx_ep, tx_ep, rc, err, qmi_err_name(err));
	int dpm_ok = (rc == 0 && (err == 0 || err == 0x1a)) ? 0 : 1;
	if (dpm_ok == 0) {
		/* Lock QMAPv5 format (llp=2, ul=5, dl=9) right after DPM_OPEN_PORT, BEFORE any WDS
		 * session (IMS PDN or internet) binds ep/iface; otherwise MPSS latches ul=0,dl=0. */
		struct endpoint wda_ep = {0};
		if (find_remote_endpoint(c, SVC_WDA, &wda_ep)) {
			uint8_t wreq[96];
			size_t wlen = build_wda_set_data_fmt_tlvs(wreq, 2u, 5u, 9u,
								  (uint32_t)ep_type, use_iface);
			uint16_t werr = 0;
			size_t wrl = 0;
			int wrc = qmi_rpc_tlv(c, &wda_ep, 239, QMI_WDA_SET_DATA_FORMAT,
					      wreq, wlen, resp, sizeof(resp), &wrl, &werr);
			printf("WDA_SET_FMT ep=%d iface=%u rc=%d err=0x%x(%s)\n",
			       ep_type, use_iface, wrc, werr, qmi_err_name(werr));
			uint8_t ep_buf[8];
			probe_put_le32(ep_buf, (uint32_t)ep_type);
			probe_put_le32(ep_buf + 4, use_iface);
			wlen = tlv_append(wreq, 0x10, ep_buf, sizeof(ep_buf));
			uint16_t gerr = 0;
			size_t grl = 0;
			int grc = qmi_rpc_tlv(c, &wda_ep, 247, QMI_WDA_GET_DATA_FORMAT,
					      wreq, wlen, resp, sizeof(resp), &grl, &gerr);
			const uint8_t *gv = NULL;
			uint16_t gl = 0;
			uint32_t llp = 0, ulp = 0, dlp = 0;
			if (grc == 0 && gerr == 0) {
				if (qmi_find_tlv(resp, grl, 0x11, &gv, &gl) == 0 && gl >= 4)
					llp = get_le32(gv);
				if (qmi_find_tlv(resp, grl, 0x12, &gv, &gl) == 0 && gl >= 4)
					ulp = get_le32(gv);
				if (qmi_find_tlv(resp, grl, 0x13, &gv, &gl) == 0 && gl >= 4)
					dlp = get_le32(gv);
			}
			printf("WDA_GET_FMT ep=%d iface=%u rc=%d err=0x%x(%s) llp=%u ul=%u dl=%u\n",
			       ep_type, use_iface, grc, gerr, qmi_err_name(gerr), llp, ulp, dlp);
		}
	}
	return dpm_ok;
}

static int run_wds_smoke(struct qrtr_client *c, const char *apn, int sub_id,
			 int mux_id, int ep_type, int iface_id, int client_type,
			 const char *configure_net, int hold_sec)
{
	struct endpoint wds_ep = {0};
	uint8_t req[256], resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t err = 0;
	int use_sub = (sub_id >= 0) ? sub_id : 1;

	if (!find_remote_endpoint(c, SVC_WDS, &wds_ep)) {
		printf("WDS_SMOKE err=no_wds_endpoint\n");
		return 1;
	}
	size_t qlen = build_wds_bind_sub_tlvs(req, (uint32_t)use_sub);
	int rc = qmi_rpc_tlv(c, &wds_ep, 240, QMI_WDS_BIND_SUBSCRIPTION, req, qlen, resp, sizeof(resp), &rl, &err);
	printf("WDS_BIND_SUB sub=%d rc=%d err=0x%x(%s)\n", use_sub, rc, err, qmi_err_name(err));

	if (mux_id >= 0) {
		(void)run_dpm_open(c, ep_type, iface_id, 0u, 0u);
		struct endpoint wda_ep = {0};
		if (find_remote_endpoint(c, SVC_WDA, &wda_ep)) {
			/* UL=5 (4B QMAP matching egress cs_offload_en=0), DL=9 (8B QMAPv5 with next_hdr=1 matching IPA v5.0 V2 ingress cs_offload_en=1) */
			qlen = build_wda_set_data_fmt_tlvs(req, 2u, 5u, 9u,
							   (uint32_t)ep_type, (uint32_t)iface_id);
			rc = qmi_rpc_tlv(c, &wda_ep, 239, QMI_WDA_SET_DATA_FORMAT,
					 req, qlen, resp, sizeof(resp), &rl, &err);
			printf("WDA_SET_FMT ep=%d iface=%d rc=%d err=0x%x(%s)\n",
			       ep_type, iface_id, rc, err, qmi_err_name(err));
			uint8_t ep_buf[8];
			probe_put_le32(ep_buf, (uint32_t)ep_type);
			probe_put_le32(ep_buf + 4, (uint32_t)iface_id);
			qlen = tlv_append(req, 0x10, ep_buf, sizeof(ep_buf));
			int grc = qmi_rpc_tlv(c, &wda_ep, 247, QMI_WDA_GET_DATA_FORMAT,
					      req, qlen, resp, sizeof(resp), &rl, &err);
			const uint8_t *gv = NULL;
			uint16_t gl = 0;
			uint32_t llp = 0, ulp = 0, dlp = 0;
			if (grc == 0 && err == 0) {
				if (qmi_find_tlv(resp, rl, 0x11, &gv, &gl) == 0 && gl >= 4)
					llp = get_le32(gv);
				if (qmi_find_tlv(resp, rl, 0x12, &gv, &gl) == 0 && gl >= 4)
					ulp = get_le32(gv);
				if (qmi_find_tlv(resp, rl, 0x13, &gv, &gl) == 0 && gl >= 4)
					dlp = get_le32(gv);
			}
			printf("WDA_GET_FMT ep=%d iface=%d rc=%d err=0x%x(%s) llp=%u ul=%u dl=%u\n",
			       ep_type, iface_id, grc, err, qmi_err_name(err), llp, ulp, dlp);
		}
		qlen = build_wds_bind_mux_tlvs(req, (uint32_t)ep_type, (uint32_t)iface_id,
					       (uint8_t)mux_id, client_type);
		rc = qmi_rpc_tlv(c, &wds_ep, 241, QMI_WDS_BIND_MUX_DATA_PORT,
				 req, qlen, resp, sizeof(resp), &rl, &err);
		printf("WDS_BIND_MUX ep=%d iface=%d mux=%d client=%d rc=%d err=0x%x(%s)\n",
		       ep_type, iface_id, mux_id, client_type, rc, err, qmi_err_name(err));
		if (rc != 0 || err != 0) {
			int alt_client = (client_type < 0) ? 1 : -1;
			qlen = build_wds_bind_mux_tlvs(req, (uint32_t)ep_type, (uint32_t)iface_id,
						       (uint8_t)mux_id, alt_client);
			rc = qmi_rpc_tlv(c, &wds_ep, 246, QMI_WDS_BIND_MUX_DATA_PORT,
					 req, qlen, resp, sizeof(resp), &rl, &err);
			printf("WDS_BIND_MUX_FALLBACK ep=%d iface=%d mux=%d client=%d rc=%d err=0x%x(%s)\n",
			       ep_type, iface_id, mux_id, alt_client, rc, err, qmi_err_name(err));
		}
	}

	qlen = build_wds_ip_family_tlvs(req, 4u);
	rc = qmi_rpc_tlv(c, &wds_ep, 242, QMI_WDS_SET_CLIENT_IP_FAMILY_PREF,
			 req, qlen, resp, sizeof(resp), &rl, &err);
	printf("WDS_IP_PREF fam=4 rc=%d err=0x%x(%s)\n", rc, err, qmi_err_name(err));

	qlen = build_wds_start_tlvs(req, apn, 4u);
	rc = qmi_rpc_tlv(c, &wds_ep, 243, QMI_WDS_START_NETWORK_INTERFACE,
			 req, qlen, resp, sizeof(resp), &rl, &err);
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	uint32_t handle = 0;
	if (rc == 0 && err == 0 && qmi_find_tlv(resp, rl, 0x01, &v, &vl) == 0 && vl >= 4)
		handle = get_le32(v);
	printf("WDS_START apn=\"%s\" rc=%d err=0x%x(%s) handle=0x%x\n",
	       apn ? apn : "", rc, err, qmi_err_name(err), handle);
	if (rc == 0 && err != 0)
		dump_tlv_hex("WDS_START_ERR", QMI_WDS_START_NETWORK_INTERFACE, resp, rl);
	if (rc != 0 || err != 0 || handle == 0)
		return 1;

	qlen = build_wds_curr_settings_tlvs(req, 0x00003ff0u);
	int crc = qmi_rpc_tlv(c, &wds_ep, 244, QMI_WDS_GET_CURRENT_SETTINGS,
			      req, qlen, resp, sizeof(resp), &rl, &err);
	char ip[32] = "0.0.0.0", gw[32] = "0.0.0.0", mask[32] = "0.0.0.0";
	char dns1[32] = "0.0.0.0", dns2[32] = "0.0.0.0";
	uint32_t ip_u32 = 0, gw_u32 = 0, mask_u32 = 0, dns1_u32 = 0;
	uint32_t mtu = 0;
	int pfx = 32;
	if (crc == 0 && err == 0) {
		if (qmi_find_tlv(resp, rl, 0x1e, &v, &vl) == 0 && vl >= 4) {
			ip_u32 = get_le32(v);
			format_ipv4_u32(ip_u32, ip, sizeof(ip));
		}
		if (qmi_find_tlv(resp, rl, 0x20, &v, &vl) == 0 && vl >= 4) {
			gw_u32 = get_le32(v);
			format_ipv4_u32(gw_u32, gw, sizeof(gw));
		}
		if (qmi_find_tlv(resp, rl, 0x21, &v, &vl) == 0 && vl >= 4) {
			mask_u32 = get_le32(v);
			format_ipv4_u32(mask_u32, mask, sizeof(mask));
			pfx = ipv4_mask_to_prefix(mask_u32);
		}
		if (qmi_find_tlv(resp, rl, 0x15, &v, &vl) == 0 && vl >= 4) {
			dns1_u32 = get_le32(v);
			format_ipv4_u32(dns1_u32, dns1, sizeof(dns1));
		}
		if (qmi_find_tlv(resp, rl, 0x16, &v, &vl) == 0 && vl >= 4)
			format_ipv4_u32(get_le32(v), dns2, sizeof(dns2));
		if (qmi_find_tlv(resp, rl, 0x29, &v, &vl) == 0 && vl >= 4)
			mtu = get_le32(v);
	}
	printf("WDS_SETTINGS rc=%d err=0x%x ip=%s pfx=%d gw=%s mask=%s dns1=%s dns2=%s mtu=%u\n",
	       crc, err, ip, pfx, gw, mask, dns1, dns2, mtu);
	fflush(stdout);

	int net_sock = -1;
	bool configured = false;
	if (configure_net != NULL && configure_net[0] != '\0' && ip_u32 != 0u) {
		net_sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (net_sock >= 0) {
			if (mtu >= 1280u && mtu <= 9216u)
				(void)netdev_set_mtu(net_sock, configure_net, (int)mtu);
			int nrc = netdev_set_ipv4_addr(net_sock, configure_net, ip_u32,
						       mask_u32 ? mask_u32 : 0xffffff00u,
						       gw_u32, dns1_u32);
			configured = (nrc == 0);
			printf("WDS_NETDEV dev=%s addr=%s/%d dns1=%s rc=%d\n",
			       configure_net, ip, pfx, dns1, nrc);
			fflush(stdout);
		}
	}

	if (hold_sec > 0)
		sleep((unsigned int)hold_sec);

	if (configured && net_sock >= 0)
		netdev_clear_ipv4_addr(net_sock, configure_net, gw_u32, dns1_u32);
	if (net_sock >= 0)
		close(net_sock);

	qlen = build_wds_stop_tlvs(req, handle);
	int src = qmi_rpc_tlv(c, &wds_ep, 245, QMI_WDS_STOP_NETWORK_INTERFACE,
			      req, qlen, resp, sizeof(resp), &rl, &err);
	printf("WDS_STOP handle=0x%x rc=%d err=0x%x(%s)\n", handle, src, err, qmi_err_name(err));
	return (crc == 0 && src == 0) ? 0 : 1;
}

static int run_qmi_caps(struct qrtr_client *c)
{
	static const struct {
		uint32_t id;
		const char *name;
	} svcs[] = {
		{ SVC_WDS, "wds" },
		{ SVC_DMS, "dms" },
		{ SVC_NAS, "nas" },
		{ SVC_UIM, "uim" },
		{ SVC_PDC, "pdc" },
	};
	uint8_t resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t err = 0;
	for (size_t s = 0; s < sizeof(svcs) / sizeof(svcs[0]); s++) {
		struct endpoint ep = {0};
		if (!find_remote_endpoint(c, svcs[s].id, &ep)) {
			printf("QMI_CAPS svc=%s err=no_ep\n", svcs[s].name);
			continue;
		}
		int rc = qmi_rpc(c, &ep, (uint16_t)(300 + s), 0x001e, resp, sizeof(resp), &rl, &err);
		printf("QMI_CAPS svc=%s rc=%d err=0x%x msgs=", svcs[s].name, rc, err);
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		if (rc == 0 && err == 0 && qmi_find_tlv(resp, rl, 0x10, &v, &vl) == 0 && vl >= 1) {
			uint8_t nbytes = v[0];
			if ((size_t)(1 + nbytes) <= vl) {
				bool first = true;
				for (uint16_t bi = 0; bi < nbytes; bi++) {
					uint8_t byte = v[1 + bi];
					for (uint8_t bit = 0; bit < 8; bit++) {
						if (byte & (1u << bit)) {
							uint16_t mid = (uint16_t)(bi * 8u + bit);
							printf("%s0x%04x", first ? "" : ",", mid);
							first = false;
						}
					}
				}
			}
		} else {
			printf("none");
		}
		printf("\n");
	}
	return 0;
}

static int run_nas_set_dsb(struct qrtr_client *c, const char *arg, int pri_override, int standby_override)
{
	struct endpoint nas_ep = {0};
	uint64_t mask = 0;
	if (strcmp(arg, "primary") == 0)
		mask = 1ULL;
	else if (strcmp(arg, "secondary") == 0)
		mask = 2ULL;
	else if (strcmp(arg, "both") == 0)
		mask = 3ULL;
	else {
		char *end = NULL;
		errno = 0;
		unsigned long long parsed = strtoull(arg, &end, 0);
		if (errno != 0 || end == arg || *end != '\0') {
			printf("NAS_SET_DSB_ERR invalid=%s\n", arg);
			return 2;
		}
		mask = (uint64_t)parsed;
	}

	uint8_t pri = 0;
	if (pri_override == 0 || pri_override == 1) {
		pri = (uint8_t)pri_override;
	} else if (mask == 2ULL) {
		pri = 1u;
	} else if (mask == 1ULL) {
		pri = 0u;
	} else {
		int active_slot = 0;
		uint8_t ua[32], ca[32], ul = 0, cl = 0;
		(void)uim_detect_active_apps(c, &active_slot, ua, &ul, ca, &cl);
		pri = (active_slot == 1) ? 1u : 0u;
	}

	if (!find_remote_endpoint(c, SVC_NAS, &nas_ep)) {
		printf("NAS_SET_DSB_ERR status=no_nas_endpoint\n");
		return 1;
	}
	uint8_t standby = (standby_override >= 0) ? (uint8_t)standby_override : 1u;
	uint8_t req[64], resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t err = 0;
	size_t qlen = build_nas_set_dsb_tlvs(req, standby, pri, pri, pri, mask);
	int rc = qmi_rpc_tlv(c, &nas_ep, 65, QMI_NAS_SET_DUAL_STANDBY_PREF,
			     req, qlen, resp, sizeof(resp), &rl, &err);
	printf("NAS_SET_DSB standby=%u req_mask=0x%llx pri=%u rc=%d err=0x%x(%s)\n",
	       standby, (unsigned long long)mask, pri, rc, err, qmi_err_name(err));

	uint8_t rb[QMI_MAX_BUF];
	size_t rbl = 0;
	uint16_t rberr = 0;
	if (qmi_rpc(c, &nas_ep, 66, QMI_NAS_GET_DUAL_STANDBY_PREF, rb, sizeof(rb), &rbl, &rberr) == 0 && rberr == 0) {
		struct nas_dsb_pref dsb;
		parse_nas_dsb_pref(rb, rbl, &dsb);
		if (dsb.has_any) {
			printf("NAS_DSB_PREF standby=%u pri=%u active=%u data=%u voice=%u active_mask=0x%llx\n",
			       dsb.standby, dsb.pri, dsb.active, dsb.data, dsb.voice,
			       (unsigned long long)dsb.mask);
		}
	}
	return (rc == 0 && err == 0) ? 0 : 1;
}

static int run_uim_prov_primary(struct qrtr_client *c)
{
	struct endpoint uim_ep = {0};
	int active_slot = 0;
	uint8_t usim_aid[32] = {0}, csim_aid[32] = {0};
	uint8_t usim_alen = 0, csim_alen = 0;
	int drc = uim_detect_active_apps(c, &active_slot, usim_aid, &usim_alen, csim_aid, &csim_alen);
	if (drc < 0 || !find_remote_endpoint(c, SVC_UIM, &uim_ep)) {
		printf("UIM_PROV_PRIMARY err=no_active_app rc=%d\n", drc);
		return 1;
	}
	uint8_t req[64], resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t gw_err = 0xffffu, c1x_err = 0xffffu;
	int gw_rc = -1, c1x_rc = -1;
	uint8_t slot_1based = (uint8_t)(active_slot + 1);

	if (usim_alen > 0) {
		size_t qlen = build_uim_change_prov_tlvs(req, 0u, 1u, slot_1based, usim_aid, usim_alen);
		gw_rc = qmi_rpc_tlv(c, &uim_ep, 67, QMI_UIM_CHANGE_PROV_SESSION,
				    req, qlen, resp, sizeof(resp), &rl, &gw_err);
	}
	if (csim_alen > 0) {
		size_t qlen = build_uim_change_prov_tlvs(req, 1u, 1u, slot_1based, csim_aid, csim_alen);
		c1x_rc = qmi_rpc_tlv(c, &uim_ep, 68, QMI_UIM_CHANGE_PROV_SESSION,
				     req, qlen, resp, sizeof(resp), &rl, &c1x_err);
	}
	printf("UIM_PROV_PRIMARY slot=%u gw_rc=%d gw_err=0x%x(%s) 1x_rc=%d 1x_err=0x%x(%s)\n",
	       slot_1based, gw_rc, gw_err, gw_err == 0xffffu ? "skip" : qmi_err_name(gw_err),
	       c1x_rc, c1x_err, c1x_err == 0xffffu ? "skip" : qmi_err_name(c1x_err));
	return run_uim(c);
}

static int parse_loc_sv_count(const uint8_t *tlvs, size_t len, uint32_t *sv_count)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	*sv_count = 0;
	if (qmi_find_tlv(tlvs, len, 0x10, &v, &vl) == 0 && vl >= 1) {
		*sv_count = v[0];
		return 0;
	}
	return -ENOENT;
}

static int parse_loc_engine_state(const uint8_t *tlvs, size_t len, uint32_t *state)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (qmi_find_tlv(tlvs, len, 0x01, &v, &vl) != 0 || vl < 4)
		return -ENOENT;
	*state = get_le32(v);
	return 0;
}

static int run_loc_probe(struct qrtr_client *c)
{
	struct endpoint ep = {0};
	uint8_t resp[2048], ind[2048], req[64];
	size_t rl = 0, il = 0;
	uint16_t err = 0;
	uint32_t rev = 0;

	if (!find_remote_endpoint(c, SVC_LOC, &ep)) {
		printf("LOC_PROBE err=no_loc_endpoint\n");
		return 1;
	}

	int rc_rev = qmi_rpc_tlv_ex(c, &ep, 70, QMI_LOC_GET_SERVICE_REVISION, NULL, 0,
				    resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	if (rc_rev == 0 && err == 0)
		(void)parse_loc_rev(resp, rl, ind, il, &rev);
	printf("LOC_REV rc=%d err=0x%x(%s) rev=%u\n", rc_rev, err, qmi_err_name(err), rev);

	size_t req_len = build_loc_inform_rev_tlvs(req, 2u);
	int rc_irev = qmi_rpc_tlv_ex(c, &ep, 71, QMI_LOC_INFORM_CLIENT_REVISION, req, req_len,
				     resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	printf("LOC_INFORM_REV rc=%d err=0x%x(%s)\n", rc_irev, err, qmi_err_name(err));

	static const uint64_t kMasks[3] = { 0x0187ULL, 0x0087ULL, 0x0007ULL };
	int rc_reg = -1;
	uint16_t err_reg = 0;
	for (size_t mi = 0; mi < 3; mi++) {
		req_len = build_loc_reg_events_tlvs(req, kMasks[mi], "MHAL", 1u, 0u);
		rc_reg = qmi_rpc_tlv_ex(c, &ep, (uint16_t)(72 + mi), QMI_LOC_REG_EVENTS, req, req_len,
					resp, sizeof(resp), &rl, &err_reg, ind, sizeof(ind), &il);
		printf("LOC_REG_EVENTS mask=0x%llx rc=%d err=0x%x(%s) il=%zu\n",
		       (unsigned long long)kMasks[mi], rc_reg, err_reg, qmi_err_name(err_reg), il);
		if (rc_reg == 0 && err_reg == 0)
			break;
		if (rc_reg == 0 && rl > QMI_HDR_LEN)
			dump_tlv_hex("LOC_REG_RESP", QMI_LOC_REG_EVENTS, resp, rl);
	}

	if (rc_reg == 0 && err_reg == 0) {
		uint8_t nbuf[4];
		probe_put_le32(nbuf, 0x0000000fu);
		req_len = tlv_append(req, 0x01, nbuf, 4);
		int rc_nmea = qmi_rpc_tlv_ex(c, &ep, 75, QMI_LOC_SET_NMEA_TYPES, req, req_len,
					     resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
		printf("LOC_SET_NMEA rc=%d err=0x%x(%s)\n", rc_nmea, err, qmi_err_name(err));
	}

	req_len = build_loc_start_tlvs(req, 1u, 1u, 1u, 1000u);
	int rc_start = qmi_rpc_tlv_ex(c, &ep, 76, QMI_LOC_START, req, req_len,
				      resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	printf("LOC_START rc=%d err=0x%x(%s)\n", rc_start, err, qmi_err_name(err));
	if (rc_start == 0 && err != 0 && rl > QMI_HDR_LEN)
		dump_tlv_hex("LOC_START_RESP", QMI_LOC_START, resp, rl);

	uint32_t eng_cnt = 0, sv_cnt = 0, nmea_cnt = 0, pos_cnt = 0, last_eng = 0, max_sv = 0;
	uint32_t ind_logs = 0;
	int64_t deadline = now_ms() + 5000;
	while (now_ms() < deadline) {
		if (wait_poll(c->fd, deadline) < 0)
			break;
		uint8_t buf[2048];
		struct sockaddr_qrtr from = {0};
		socklen_t sl = sizeof(from);
		ssize_t n = recvfrom(c->fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &sl);
		if (n < 7 || from.sq_node != ep.node || from.sq_port != ep.port)
			continue;
		if (buf[0] != 0x04)
			continue;
		uint16_t mid = get_le16(buf + 3);
		uint16_t tlen = get_le16(buf + 5);
		if (7u + (size_t)tlen > (size_t)n)
			continue;
		const uint8_t *tlvs = buf + 7;
		if (mid == QMI_LOC_ENGINE_STATE_IND) {
			uint32_t es = 0;
			if (parse_loc_engine_state(buf, (size_t)n, &es) == 0) {
				last_eng = es;
				if (ind_logs < 12) {
					printf("LOC_IND_ENG state=%u\n", es);
					ind_logs++;
				}
			}
			eng_cnt++;
		} else if (mid == QMI_LOC_GNSS_SV_INFO_IND) {
			uint32_t sc = 0;
			if (parse_loc_sv_count(buf, (size_t)n, &sc) == 0) {
				if (sc > max_sv)
					max_sv = sc;
				if (ind_logs < 12) {
					printf("LOC_IND_SV count=%u\n", sc);
					ind_logs++;
				}
			}
			sv_cnt++;
		} else if (mid == QMI_LOC_NMEA_IND) {
			nmea_cnt++;
			const uint8_t *nv = NULL;
			uint16_t nvl = 0;
			if (ind_logs < 12 && qmi_find_tlv(buf, (size_t)n, 0x01, &nv, &nvl) == 0 && nvl > 0) {
				char hdr[12] = "";
				size_t hi = 0;
				for (size_t k = 0; k < nvl && hi + 1 < sizeof(hdr); k++) {
					unsigned char ch = nv[k];
					if (ch == ',' || ch == '\r' || ch == '\n' || ch == '\0')
						break;
					hdr[hi++] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '.';
				}
				hdr[hi] = '\0';
				printf("LOC_IND_NMEA hdr=%s len=%u\n", hdr, nvl);
				ind_logs++;
			}
		} else if (mid == QMI_LOC_POSITION_REPORT_IND) {
			pos_cnt++;
			if (ind_logs < 12) {
				const uint8_t *v = NULL;
				uint16_t vl = 0;
				uint32_t ss = 255, tech = 0, fcnt = 0;
				uint8_t sid = 255;
				int has_lat = (qmi_find_tlv(buf, (size_t)n, 0x10, &v, &vl) == 0 && vl >= 8) ? 1 : 0;
				int has_lon = (qmi_find_tlv(buf, (size_t)n, 0x11, &v, &vl) == 0 && vl >= 8) ? 1 : 0;
				if (qmi_find_tlv(buf, (size_t)n, 0x01, &v, &vl) == 0 && vl >= 4)
					ss = get_le32(v);
				if (qmi_find_tlv(buf, (size_t)n, 0x02, &v, &vl) == 0 && vl >= 1)
					sid = v[0];
				if (qmi_find_tlv(buf, (size_t)n, 0x23, &v, &vl) == 0 && vl >= 4)
					tech = get_le32(v);
				if (qmi_find_tlv(buf, (size_t)n, 0x2b, &v, &vl) == 0 && vl >= 4)
					fcnt = get_le32(v);
				printf("LOC_IND_POS sess_status=%u sess_id=%u has_lat=%d has_lon=%d tech=0x%x fix_cnt=%u tlvs=[",
				       ss, sid, has_lat, has_lon, tech, fcnt);
				size_t off = 0;
				bool first = true;
				while (off + 3 <= tlen) {
					uint8_t tid = tlvs[off];
					uint16_t ttl = get_le16(tlvs + off + 1);
					if (off + 3 + (size_t)ttl > tlen)
						break;
					printf("%s0x%02x:%u", first ? "" : ",", tid, ttl);
					first = false;
					off += 3 + (size_t)ttl;
				}
				printf("]\n");
				ind_logs++;
			}
		}
	}

	uint8_t sess_id = 1;
	req_len = tlv_append(req, 0x01, &sess_id, 1);
	int rc_stop = qmi_rpc_tlv_ex(c, &ep, 77, QMI_LOC_STOP, req, req_len,
				     resp, sizeof(resp), &rl, &err, ind, sizeof(ind), &il);
	printf("LOC_STOP rc=%d err=0x%x(%s)\n", rc_stop, err, qmi_err_name(err));
	printf("LOC_PROBE rev=%u irev_rc=%d reg_rc=%d start_rc=%d stop_rc=%d eng=%u(last=%u) sv=%u(max=%u) nmea=%u pos=%u\n",
	       rev, rc_irev, rc_reg, rc_start, rc_stop, eng_cnt, last_eng, sv_cnt, max_sv, nmea_cnt, pos_cnt);
	return (rc_rev == 0 && rc_reg == 0 && rc_start == 0 && rc_stop == 0) ? 0 : 1;
}

static size_t build_msg_type(uint8_t type, uint8_t *dst, uint16_t txn, uint16_t msg_id,
			     const uint8_t *tlvs, size_t tlv_len)
{
	dst[0] = type;
	put_le16(dst + 1, txn);
	put_le16(dst + 3, msg_id);
	put_le16(dst + 5, (uint16_t)tlv_len);
	memcpy(dst + 7, tlvs, tlv_len);
	return 7 + tlv_len;
}

static size_t build_msg(uint8_t *dst, uint16_t txn, uint16_t msg_id, const uint8_t *tlvs, size_t tlv_len)
{
	return build_msg_type(QMI_MSG_RESP, dst, txn, msg_id, tlvs, tlv_len);
}

static int run_self_test(void)
{
	char out[1024];

	/* 1. Synthetic DMS responses including IMEI masking check */
	const uint8_t mod_tlvs[] = { 0x02, 0x04, 0x00, 0,0,0,0,  0x01, 0x06, 0x00, 'M','A','R','B','L','E' };
	const uint8_t imei_tlvs[] = { 0x02, 0x04, 0x00, 0,0,0,0, 0x11, 15, 0, '8','6','1','2','3','4','5','6','7','8','9','0','1','2','3' };
	const uint8_t op_tlvs[] = { 0x02, 0x04, 0x00, 0,0,0,0,  0x01, 0x01, 0x00, 0x00 };
	uint8_t m1[64], m2[64], m3[64];
	size_t l1 = build_msg(m1, 1, QMI_DMS_GET_MODEL_ID, mod_tlvs, sizeof(mod_tlvs));
	size_t l2 = build_msg(m2, 2, QMI_DMS_GET_SERIAL_NUMS, imei_tlvs, sizeof(imei_tlvs));
	size_t l3 = build_msg(m3, 3, QMI_DMS_GET_OP_MODE, op_tlvs, sizeof(op_tlvs));
	assert(parse_and_print_dms(m1, l1, NULL, 0, m2, l2, m3, l3, out, sizeof(out)) == 0);
	assert(strstr(out, "model=\"MARBLE\"") != NULL);
	assert(strstr(out, "imei_present=1 imei_len=15 masked=86***23") != NULL);
	assert(strstr(out, "861234567890123") == NULL); /* Never leak full IMEI */
	assert(strstr(out, "op_mode=0(online)") != NULL);

	/* 2. Synthetic UIM 2-slot response: slot 0 absent, slot 1 present USIM ready with 4-byte AID */
	const uint8_t uim_tlvs[] = {
		0x02, 0x04, 0x00, 0,0,0,0,
		0x10, 39, 0x00,
		0xff,0xff, 0xff,0xff, 0x01,0x00, 0xff,0xff, /* idx */
		2, /* cards = 2 */
		0, 0, 0, 0, 0, 0, /* slot 0 absent, 0 apps */
		1, 0, 3, 2, 0, 1, /* slot 1 present, 1 app */
		2, 7, 0, 0, 3, 2, /* type=2(USIM), state=7(ready), perso... */
		4, 0xa0, 0x00, 0x00, 0x87, /* aid_len=4 + AID */
		0, 3, 3, 10, 0, 3, 10 /* upin_rep=0, pin1=3(disabled) ... */
	};
	uint8_t um[128];
	size_t ul = build_msg(um, 4, QMI_UIM_GET_CARD_STATUS, uim_tlvs, sizeof(uim_tlvs));
	assert(parse_and_print_uim(um, ul, out, sizeof(out)) == 0);
	assert(strstr(out, "UIM_CARD slot=0 state=0(absent) err=0 apps=0") != NULL);
	assert(strstr(out, "UIM_CARD slot=1 state=1(present) err=0 apps=1") != NULL);
	assert(strstr(out, "UIM_APP slot=1 idx=0 type=2(USIM) state=7(ready) pin1=3") != NULL);

	/* 3. Synthetic NAS serving system + LTE & 5G signal + Home Network */
	const uint8_t ss_tlvs[] = {
		0x02, 0x04, 0x00, 0,0,0,0,
		0x01, 7, 0,  1, 1, 1, 2, 2, 8, 12,
		0x12, 9, 0,  0xcc, 0x01, 0x00, 0x00, 4, 'C','M','C','C'
	};
	const uint8_t sig_tlvs[] = {
		0x02, 0x04, 0x00, 0,0,0,0,
		0x14, 6, 0, (uint8_t)-68, (uint8_t)-11, 0xa0, 0xfc, 0x64, 0x00,
		0x17, 4, 0, 0xc8, 0xfc, 0x50, 0x00
	};
	const uint8_t hn_tlvs[] = {
		0x02, 0x04, 0x00, 0,0,0,0,
		0x10, 9, 0, 0xcc, 0x01, 0x02, 0x00, 4, 'C','M','C','C'
	};
	uint8_t nm1[128], nm2[128], nm3[128];
	size_t nl1 = build_msg(nm1, 5, QMI_NAS_GET_SERVING_SYSTEM, ss_tlvs, sizeof(ss_tlvs));
	size_t nl2 = build_msg(nm2, 6, QMI_NAS_GET_SIGNAL_INFO, sig_tlvs, sizeof(sig_tlvs));
	size_t nl3 = build_msg(nm3, 7, QMI_NAS_GET_HOME_NETWORK, hn_tlvs, sizeof(hn_tlvs));
	assert(parse_and_print_nas(nm1, nl1, nm2, nl2, out, sizeof(out)) == 0);
	assert(strstr(out, "NAS_SERVING reg=1(registered) cs=1 ps=1 sel=2 ifs=8(lte),12(nr5g)") != NULL);
	assert(strstr(out, "NAS_PLMN mcc=460 mnc=0 desc=\"CMCC\"") != NULL);
	assert(strstr(out, "NAS_SIG_LTE rssi=-68 rsrq=-11 rsrp=-864 snr=100") != NULL);
	assert(strstr(out, "NAS_SIG_NR5G rsrp=-824 snr=80") != NULL);
	assert(parse_and_print_nas_home(nm3, nl3, out, sizeof(out)) == 0);
	assert(strcmp(out, "NAS_HOME mcc=460 mnc=2 desc=\"CMCC\"") == 0);

	/* 4. F2 extensions: DMS SET_OP_MODE encode, EF_ICCID/EF_IMSI BCD decode & masking, UIM read TLV, LOC rev IND */
	uint8_t mode_tlvs[16];
	assert(build_dms_set_mode_tlvs(mode_tlvs, 0) == 4);
	assert(memcmp(mode_tlvs, (const uint8_t[]){0x01, 0x01, 0x00, 0x00}, 4) == 0);

	const uint8_t ef_iccid_raw[10] = {0x98, 0x68, 0x00, 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34};
	char iccid[32], masked[16];
	assert(decode_iccid_bcd(ef_iccid_raw, sizeof(ef_iccid_raw), iccid, sizeof(iccid)) == 0);
	assert(strcmp(iccid, "89860021436587092143") == 0);
	format_masked_str(iccid, masked, sizeof(masked));
	assert(strcmp(masked, "89***43") == 0);

	/* Synthetic 3GPP EF_IMSI: 460021234567890 -> len=8, parity=odd(1)|first=4 -> 0x49, 0x06, 0x20, ... */
	const uint8_t ef_imsi_raw[9] = {0x08, 0x49, 0x06, 0x20, 0x21, 0x43, 0x65, 0x87, 0x09};
	size_t imsi_len = 0;
	char mcc[8], mnc[8];
	assert(decode_imsi_plmn(ef_imsi_raw, sizeof(ef_imsi_raw), &imsi_len, mcc, mnc) == 0);
	assert(imsi_len == 15 && strcmp(mcc, "460") == 0 && strcmp(mnc, "02") == 0);

	uint8_t uim_read_req[64];
	const uint8_t mf[2] = {0x00, 0x3f};
	assert(build_uim_read_tlvs(uim_read_req, 7, NULL, 0, 0x2fe2, mf, 2, 0, 10) == 20);

	const uint8_t loc_ind_tlvs[] = { 0x01, 4, 0, 0,0,0,0, 0x02, 4, 0, 0x34, 0x12, 0x00, 0x00 };
	uint8_t loc_ind[64];
	uint32_t loc_rev = 0;
	size_t lil = build_msg_type(QMI_MSG_IND, loc_ind, 0, QMI_LOC_GET_SERVICE_REVISION, loc_ind_tlvs, sizeof(loc_ind_tlvs));
	assert(parse_loc_rev(NULL, 0, loc_ind, lil, &loc_rev) == 0 && loc_rev == 0x1234);

	/* Synthetic DMS cap, op_mode offline reason, PDC list, and LOC indications tests */
	uint8_t qpkt[64];
	size_t qlen;
	uint8_t dcap_tlvs[16] = { 0x01, 13, 0 };
	dcap_tlvs[3] = 0x40; dcap_tlvs[7] = 0x20; /* max_tx, max_rx */
	dcap_tlvs[11] = 2; dcap_tlvs[12] = 2; dcap_tlvs[13] = 2; dcap_tlvs[14] = 4; dcap_tlvs[15] = 8;
	qlen = build_msg_type(QMI_MSG_RESP, qpkt, 1, QMI_DMS_GET_DEVICE_CAP, dcap_tlvs, sizeof(dcap_tlvs));
	uint32_t mtx = 0, mrx = 0;
	uint8_t dcap = 0, scap = 0, nifs = 0;
	assert(parse_dms_cap(qpkt, qlen, &mtx, &mrx, &dcap, &scap, &nifs) == 0);
	assert(mtx == 0x40 && mrx == 0x20 && dcap == 2 && scap == 2 && nifs == 2);

	uint8_t dop_tlvs[] = { 0x01, 1, 0, 1, 0x10, 2, 0, 0x05, 0x00, 0x11, 1, 0, 1 };
	qlen = build_msg_type(QMI_MSG_RESP, qpkt, 2, QMI_DMS_GET_OPERATING_MODE, dop_tlvs, sizeof(dop_tlvs));
	uint8_t dm = 0, dhw = 0;
	uint16_t doff = 0;
	bool dhoff = false, dhhw = false;
	assert(parse_dms_op_mode_raw(qpkt, qlen, &dm, &doff, &dhoff, &dhw, &dhhw) == 0);
	assert(dm == 1 && dhoff && doff == 5 && dhhw && dhw == 1);

	uint8_t pdc_tlvs[] = { 0x11, 2, 0, 3, 0 };
	qlen = build_msg_type(QMI_MSG_IND, qpkt, 3, QMI_PDC_LIST_CONFIGS, pdc_tlvs, sizeof(pdc_tlvs));
	uint32_t pdc_cnt = 0;
	assert(parse_pdc_list_configs(NULL, 0, qpkt, qlen, &pdc_cnt) == 0 && pdc_cnt == 3);

	uint8_t sv_tlvs[] = { 0x10, 2, 0, 7, 0 };
	qlen = build_msg_type(QMI_MSG_IND, qpkt, 4, QMI_LOC_GNSS_SV_INFO_IND, sv_tlvs, sizeof(sv_tlvs));
	uint32_t svc_cnt = 0;
	assert(parse_loc_sv_count(qpkt, qlen, &svc_cnt) == 0 && svc_cnt == 7);

	uint8_t eng_tlvs[] = { 0x01, 4, 0, 1, 0, 0, 0 };
	qlen = build_msg_type(QMI_MSG_IND, qpkt, 5, QMI_LOC_ENGINE_STATE_IND, eng_tlvs, sizeof(eng_tlvs));
	uint32_t eng_st = 0;
	assert(parse_loc_engine_state(qpkt, qlen, &eng_st) == 0 && eng_st == 1);

	/* F4/F5 unit assertions: error names, set-mode parsing, PDC & LOC_INFORM_REV & AFW TLVs */
	assert(strcmp(qmi_err_name(0x34), "incompatible_state") == 0);
	assert(strcmp(qmi_err_name(0x11), "missing_arg") == 0);
	assert(strcmp(qmi_err_name(0x30), "invalid_arg") == 0);
	assert(strcmp(qmi_err_name(0x2e), "invalid_op") == 0);
	uint8_t parsed_mode = 255;
	assert(parse_mode_arg("online", &parsed_mode) == 0 && parsed_mode == 0);
	assert(parse_mode_arg("lpm", &parsed_mode) == 0 && parsed_mode == 1);
	assert(parse_mode_arg("offline", &parsed_mode) == 0 && parsed_mode == 3);
	assert(parse_mode_arg("reset", &parsed_mode) == 0 && parsed_mode == 4);
	assert(parse_mode_arg("bad", &parsed_mode) < 0);

	uint8_t pdc_req[32];
	assert(build_pdc_list_tlvs(pdc_req, 1u, 1u) == 14);
	assert(pdc_req[0] == 0x10 && pdc_req[3] == 1 && pdc_req[7] == 0x11 && pdc_req[10] == 1);
	assert(build_pdc_selected_tlvs(pdc_req, 1u, 2u) == 14);
	assert(pdc_req[0] == 0x01 && pdc_req[3] == 1 && pdc_req[7] == 0x10 && pdc_req[10] == 2);

	/* F5 NAS Home TLV 0x01, Dual Standby Preference, UIM Change Prov, LOC AFW + Start TLVs */
	const uint8_t hn01_tlvs[] = {
		0x02, 0x04, 0x00, 0,0,0,0,
		0x01, 7, 0, 0xcc, 0x01, 0x0b, 0x00, 2, 'C','T'
	};
	uint8_t nh01[64];
	size_t nh01l = build_msg(nh01, 8, QMI_NAS_GET_HOME_NETWORK, hn01_tlvs, sizeof(hn01_tlvs));
	assert(parse_and_print_nas_home(nh01, nh01l, out, sizeof(out)) == 0);
	assert(strcmp(out, "NAS_HOME mcc=460 mnc=11 desc=\"CT\"") == 0);

	uint8_t dsb_req[32];
	assert(build_nas_set_dsb_tlvs(dsb_req, 0x05u, 1u, 1u, 1u, 0x03ULL) == 27);
	assert(dsb_req[0] == 0x10 && dsb_req[3] == 0x05 && dsb_req[4] == 0x11 && dsb_req[7] == 1);
	assert(dsb_req[16] == 0x14 && dsb_req[19] == 0x03);

	const uint8_t dsb_resp_tlvs[] = {
		0x02, 4, 0, 0,0,0,0,
		0x10, 1, 0, 0x02,
		0x11, 1, 0, 0x01,
		0x12, 1, 0, 0x01,
		0x13, 1, 0, 0x01,
		0x14, 1, 0, 0x01,
		0x15, 8, 0, 0x03,0,0,0,0,0,0,0
	};
	uint8_t dsb_msg[64];
	size_t dsb_len = build_msg(dsb_msg, 9, QMI_NAS_GET_DUAL_STANDBY_PREF, dsb_resp_tlvs, sizeof(dsb_resp_tlvs));
	struct nas_dsb_pref dsb;
	parse_nas_dsb_pref(dsb_msg, dsb_len, &dsb);
	assert(dsb.has_any && dsb.standby == 2 && dsb.pri == 1 && dsb.active == 1 && dsb.mask == 0x03ULL);

	uint8_t prov_req[32];
	const uint8_t fake_aid[4] = { 0xa0, 0x00, 0x00, 0x87 };
	assert(build_uim_change_prov_tlvs(prov_req, 0u, 1u, 2u, fake_aid, 4u) == 14);
	assert(prov_req[0] == 0x01 && prov_req[3] == 0 && prov_req[4] == 1 &&
	       prov_req[5] == 0x10 && prov_req[8] == 2 && prov_req[9] == 4);

	uint8_t loc_rev_req[16];
	assert(build_loc_inform_rev_tlvs(loc_rev_req, 2u) == 7);
	assert(loc_rev_req[0] == 0x01 && loc_rev_req[1] == 4 && loc_rev_req[3] == 2);

	uint8_t loc_reg_req[32];
	assert(build_loc_reg_events_tlvs(loc_reg_req, 0x0187ULL, "MHAL", 1u, 0u) == 29);
	assert(loc_reg_req[0] == 0x01 && loc_reg_req[11] == 0x10 && memcmp(loc_reg_req + 14, "MHAL", 4) == 0);
	assert(loc_reg_req[18] == 0x11 && loc_reg_req[21] == 1 && loc_reg_req[25] == 0x12 && loc_reg_req[28] == 0);

	uint8_t loc_st_req[32];
	assert(build_loc_start_tlvs(loc_st_req, 1u, 1u, 1u, 1000u) == 25);
	assert(loc_st_req[0] == 0x01 && loc_st_req[4] == 0x10 && loc_st_req[11] == 0x12 && loc_st_req[18] == 0x13);

	/* F6 unit assertions: NAS bind/mode, PDC list/selected/info/select, WDS bind/start/settings */
	uint8_t f6_buf[128];
	assert(build_nas_bind_sub_tlvs(f6_buf, 1u) == 4);
	assert(f6_buf[0] == 0x01 && f6_buf[1] == 1 && f6_buf[3] == 1);

	uint16_t mp = 0;
	assert(parse_nas_mode_arg("lte", &mp) == 0 && mp == 0x001cu);
	assert(parse_nas_mode_arg("lte-only", &mp) == 0 && mp == 0x0010u);
	assert(parse_nas_mode_arg("nr5g", &mp) == 0 && mp == 0x005cu);
	assert(parse_nas_mode_arg("auto", &mp) == 0 && mp == 0x005cu);
	assert(parse_nas_mode_arg("invalid", &mp) < 0);
	assert(build_nas_set_sys_sel_tlvs(f6_buf, 0x001cu, 0u, 0u) == 9);
	assert(f6_buf[0] == 0x11 && f6_buf[3] == 0x1c && f6_buf[4] == 0x00);
	assert(f6_buf[5] == 0x17 && f6_buf[8] == 0x00);

	assert(build_pdc_selected_sub_tlvs(f6_buf, 1u, 3u, true, 1u) == 21);
	assert(f6_buf[14] == 0x11 && f6_buf[17] == 1u);
	const uint8_t fake_pdc_id[4] = { 0x8e, 0xfd, 0x24, 0xa9 };
	assert(build_pdc_config_info_tlvs(f6_buf, 1u, fake_pdc_id, 4u, 7u) == 19);
	assert(f6_buf[0] == 0x01 && f6_buf[7] == 4u && memcmp(f6_buf + 8, fake_pdc_id, 4) == 0);
	assert(build_pdc_select_tlvs(f6_buf, 1u, fake_pdc_id, 4u, 7u, true, 1u) == 26);
	assert(build_pdc_activate_tlvs(f6_buf, 1u, 8u, true, 1u) == 21);

	const uint8_t pdc_sel_ind_tlvs[] = { 0x01, 2, 0, 0, 0, 0x11, 5, 0, 4, 0xaa, 0xbb, 0xcc, 0xdd };
	uint8_t pdc_sel_ind[64], got_id[64], got_id_len = 0;
	qlen = build_msg_type(QMI_MSG_IND, pdc_sel_ind, 10, QMI_PDC_GET_SELECTED_CONFIG,
			      pdc_sel_ind_tlvs, sizeof(pdc_sel_ind_tlvs));
	assert(parse_pdc_selected_id(NULL, 0, pdc_sel_ind, qlen, got_id, &got_id_len) == 0);
	assert(got_id_len == 4 && got_id[0] == 0xaa && got_id[3] == 0xdd);

	const uint8_t pdc_list_ind_tlvs[] = {
		0x11, 19, 0, 2,
		1, 0, 0, 0, 4, 0x11, 0x22, 0x33, 0x44,
		1, 0, 0, 0, 4, 0x55, 0x66, 0x77, 0x88
	};
	uint8_t pdc_list_ind[64];
	struct pdc_list_entry plist[4];
	size_t pcnt = 0;
	qlen = build_msg_type(QMI_MSG_IND, pdc_list_ind, 11, QMI_PDC_LIST_CONFIGS,
			      pdc_list_ind_tlvs, sizeof(pdc_list_ind_tlvs));
	assert(parse_pdc_list_entries(NULL, 0, pdc_list_ind, qlen, plist, 4, &pcnt) == 0);
	assert(pcnt == 2 && plist[0].id_len == 4 && plist[1].id[0] == 0x55);

	uint8_t hex_out[8], hex_len = 0;
	assert(parse_hex_bytes("8efd24a9", hex_out, sizeof(hex_out), &hex_len) == 0);
	assert(hex_len == 4 && hex_out[0] == 0x8e && hex_out[3] == 0xa9);
	assert(parse_hex_bytes("xyz", hex_out, sizeof(hex_out), &hex_len) < 0);

	assert(build_wds_bind_sub_tlvs(f6_buf, 1u) == 7);
	assert(build_wds_bind_mux_tlvs(f6_buf, 4u, 1u, 1u, 1) == 22);
	assert(f6_buf[0] == 0x10 && f6_buf[3] == 4u && f6_buf[7] == 1u && f6_buf[11] == 0x11 && f6_buf[14] == 1u);
	assert(build_wds_ip_family_tlvs(f6_buf, 4u) == 4);
	assert(build_wds_profile_list_tlvs(f6_buf, 0u) == 4);
	assert(build_wds_profile_settings_tlvs(f6_buf, 0u, 1u) == 5);
	assert(build_wds_start_tlvs(f6_buf, "ctnet", 4u) == 12);
	assert(f6_buf[0] == 0x14 && f6_buf[1] == 5 && memcmp(f6_buf + 3, "ctnet", 5) == 0);
	assert(build_wds_stop_tlvs(f6_buf, 0x12345678u) == 7);
	assert(build_wds_curr_settings_tlvs(f6_buf, 0x00003ff0u) == 7);

	char ip_str[32];
	format_ipv4_u32(0x0a151ecbu, ip_str, sizeof(ip_str));
	assert(strcmp(ip_str, "10.21.30.203") == 0);

	assert(build_dpm_open_port_tlvs(f6_buf, 4u, 1u, 2u, 23u) == 20);
	assert(f6_buf[0] == 0x11 && f6_buf[1] == 17 && f6_buf[3] == 1u && f6_buf[4] == 4u && f6_buf[12] == 2u && f6_buf[16] == 23u);
	assert(build_wda_set_data_fmt_tlvs(f6_buf, 2u, 5u, 9u, 4u, 1u) == 46);
	assert(f6_buf[0] == 0x11 && f6_buf[3] == 2u && f6_buf[7] == 0x12 && f6_buf[10] == 5u &&
	       f6_buf[14] == 0x13 && f6_buf[17] == 9u && f6_buf[21] == 0x15 && f6_buf[28] == 0x16 && f6_buf[35] == 0x17);

	const uint8_t si_tlvs[] = {
		0x02, 4, 0, 0,0,0,0,
		0x14, 3, 0, 2, 0, 0,
		0x19, 29, 0,
		  1, 2, 1, 2, 1, 0, 1, 0, 0, 0xff, 0xff, 1,
		  0x65, 0x63, 0x56, 0x04, 0, 0, 0,
		  1, '4', '6', '0', '1', '1', 0xff,
		  1, 0x34, 0x12,
		0x29, 1, 0, 1,
		0x4a, 3, 0, 1, 3, 0
	};
	uint8_t si_msg[128];
	size_t sil_t = build_msg(si_msg, 12, QMI_NAS_GET_SYS_INFO, si_tlvs, sizeof(si_tlvs));
	assert(parse_and_print_nas_sys_info(si_msg, sil_t, out, sizeof(out)) == 0);
	assert(strstr(out, "NAS_SYS_INFO_DEC lte_srv=2 true_srv=0 domain=2 roam=0 cell_id=0x4566365 tac=0x1234 plmn=46011 ims_voice=1 nr5g_srv=1") != NULL);

	assert(ipv4_mask_to_prefix(0xffffff00u) == 24);
	assert(ipv4_mask_to_prefix(0xfffffffcu) == 30);
	assert(ipv4_mask_to_prefix(0u) == 32);

	uint8_t nl_msg[256];
	size_t nll = build_rmnet_newlink_msg(
		nl_msg, sizeof(nl_msg), 1u, 17, "rmnet_data0", 1u,
		RMNET_FLAGS_INGRESS_DEAGGREGATION | RMNET_FLAGS_INGRESS_MAP_COMMANDS,
		RMNET_FLAGS_INGRESS_DEAGGREGATION | RMNET_FLAGS_INGRESS_MAP_COMMANDS);
	assert(nll > NLMSG_LENGTH(sizeof(struct ifinfomsg)) && nll <= sizeof(nl_msg));
	const struct nlmsghdr *nlh = (const struct nlmsghdr *)nl_msg;
	assert(nlh->nlmsg_type == RTM_NEWLINK && nlh->nlmsg_len == nll);

	puts("MODEM_PROBE_SELF_TEST_OK=1");
	return 0;
}

int main(int argc, char **argv)
{
	bool do_svc = false, do_svc_all = false, do_dms = false, do_uim = false, do_nas = false;
	bool do_sim_ef = false, do_nas_ext = false, do_loc_rev = false;
	bool do_dms_raw = false, do_loc_probe = false, do_uim_prov = false;
	bool do_pdc_info = false, do_wds_profiles = false, do_qmi_caps = false;
	bool do_ipa_setup = false, do_dpm_open = false;
	const char *set_mode = NULL;
	const char *nas_set_dsb = NULL;
	const char *nas_mode = NULL;
	const char *pdc_select_sw = NULL;
	const char *wds_smoke_apn = NULL;
	const char *ipa_devname = "rmnet_data0";
	const char *configure_net = NULL;
	int nas_dsb_pri = -1;
	int nas_dsb_standby = -1;
	int sub_id = -1;
	int mux_id = -1;
	int ep_type = 4;
	int iface_id = 1;
	int client_type = -1;
	int hold_sec = 0;
	int ipa_mtu = 1500;
	struct qrtr_client c = { .fd = -1 };
	int rc = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2)
		do_svc = true;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--self-test") == 0)
			return run_self_test();
		if (strcmp(argv[i], "--services") == 0)
			do_svc = true;
		else if (strcmp(argv[i], "--services-all") == 0)
			do_svc_all = true;
		else if (strcmp(argv[i], "--dms") == 0)
			do_dms = true;
		else if (strcmp(argv[i], "--uim") == 0)
			do_uim = true;
		else if (strcmp(argv[i], "--uim-prov-primary") == 0)
			do_uim_prov = true;
		else if (strcmp(argv[i], "--nas") == 0)
			do_nas = true;
		else if (strcmp(argv[i], "--sim-ef") == 0)
			do_sim_ef = true;
		else if (strcmp(argv[i], "--nas-ext") == 0)
			do_nas_ext = true;
		else if (strcmp(argv[i], "--loc-rev") == 0)
			do_loc_rev = true;
		else if (strcmp(argv[i], "--dms-raw") == 0)
			do_dms_raw = true;
		else if (strcmp(argv[i], "--loc-probe") == 0)
			do_loc_probe = true;
		else if (strcmp(argv[i], "--pdc-info") == 0)
			do_pdc_info = true;
		else if (strcmp(argv[i], "--wds-profiles") == 0)
			do_wds_profiles = true;
		else if (strcmp(argv[i], "--qmi-caps") == 0)
			do_qmi_caps = true;
		else if (strcmp(argv[i], "--ipa-setup") == 0)
			do_ipa_setup = true;
		else if (strcmp(argv[i], "--dpm-open") == 0)
			do_dpm_open = true;
		else if (strcmp(argv[i], "--ifname") == 0 && i + 1 < argc)
			ipa_devname = argv[++i];
		else if (strcmp(argv[i], "--mtu") == 0 && i + 1 < argc)
			ipa_mtu = atoi(argv[++i]);
		else if (strcmp(argv[i], "--configure-net") == 0 && i + 1 < argc)
			configure_net = argv[++i];
		else if (strcmp(argv[i], "--sub") == 0 && i + 1 < argc)
			sub_id = atoi(argv[++i]);
		else if (strcmp(argv[i], "--nas-mode") == 0 && i + 1 < argc)
			nas_mode = argv[++i];
		else if (strcmp(argv[i], "--pdc-select-sw") == 0 && i + 1 < argc)
			pdc_select_sw = argv[++i];
		else if (strcmp(argv[i], "--wds-smoke") == 0 && i + 1 < argc)
			wds_smoke_apn = argv[++i];
		else if (strcmp(argv[i], "--mux-id") == 0 && i + 1 < argc)
			mux_id = atoi(argv[++i]);
		else if (strcmp(argv[i], "--ep-type") == 0 && i + 1 < argc)
			ep_type = atoi(argv[++i]);
		else if (strcmp(argv[i], "--iface-id") == 0 && i + 1 < argc)
			iface_id = atoi(argv[++i]);
		else if (strcmp(argv[i], "--client-type") == 0 && i + 1 < argc)
			client_type = atoi(argv[++i]);
		else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc)
			hold_sec = atoi(argv[++i]);
		else if (strcmp(argv[i], "--set-mode") == 0 && i + 1 < argc)
			set_mode = argv[++i];
		else if (strcmp(argv[i], "--nas-set-dsb") == 0 && i + 1 < argc)
			nas_set_dsb = argv[++i];
		else if (strcmp(argv[i], "--nas-dsb-pri") == 0 && i + 1 < argc)
			nas_dsb_pri = atoi(argv[++i]);
		else if (strcmp(argv[i], "--standby") == 0 && i + 1 < argc)
			nas_dsb_standby = atoi(argv[++i]);
		else if (strcmp(argv[i], "--all") == 0)
			do_svc = do_dms = do_uim = do_nas = true;
		else {
			fprintf(stderr,
				"usage: %s [--self-test|--ipa-setup [--ifname rmnet_data0 --mux-id 1 --mtu 1500]|--dpm-open [--ep-type N --iface-id N]|--sub 0|1|--services|--services-all|--dms|--dms-raw|--uim|--uim-prov-primary|--sim-ef|--nas|--nas-ext|--nas-mode lte|lte-only|nr5g|auto|--nas-set-dsb primary|secondary|both|0xN [--nas-dsb-pri 0|1] [--standby N]|--pdc-info|--pdc-select-sw HEX|--wds-profiles|--wds-smoke APN [--mux-id N --ep-type N --iface-id N --client-type N --configure-net DEV --hold S]|--set-mode online|lpm|offline|reset|--loc-rev|--loc-probe|--all]\n",
				argv[0]);
			return 2;
		}
	}

	if (do_ipa_setup) {
		int use_mux = (mux_id >= 0) ? mux_id : 1;
		rc |= run_ipa_setup("rmnet_ipa0", ipa_devname, use_mux, ipa_mtu);
	}
	bool need_qrtr = do_svc || do_svc_all || do_dms || do_dms_raw || do_uim || do_uim_prov ||
			 do_nas || do_sim_ef || do_nas_ext || do_loc_rev || do_loc_probe ||
			 do_pdc_info || do_wds_profiles || do_qmi_caps || do_dpm_open ||
			 (set_mode != NULL) || (nas_set_dsb != NULL) || (nas_mode != NULL) ||
			 (pdc_select_sw != NULL) || (wds_smoke_apn != NULL);
	if (!need_qrtr)
		return rc ? 1 : 0;

	if (qrtr_open_ephemeral(&c) < 0) {
		fprintf(stderr, "MODEM_PROBE_ERR socket(AF_QIPCRTR) failed\n");
		return 2;
	}
	if (set_mode)
		rc |= run_set_mode(&c, set_mode);
	if (do_dpm_open)
		rc |= run_dpm_open(&c, ep_type, iface_id, 0u, 0u);
	if (nas_set_dsb)
		rc |= run_nas_set_dsb(&c, nas_set_dsb, nas_dsb_pri, nas_dsb_standby);
	if (nas_mode)
		rc |= run_nas_mode(&c, nas_mode, sub_id);
	if (pdc_select_sw)
		rc |= run_pdc_select_sw(&c, pdc_select_sw, sub_id);
	if (do_uim_prov)
		rc |= run_uim_prov_primary(&c);
	if (do_svc)
		rc |= run_services(&c);
	if (do_svc_all)
		rc |= run_services_all(&c);
	if (do_dms)
		rc |= run_dms(&c);
	if (do_dms_raw)
		rc |= run_dms_raw(&c, sub_id);
	if (do_pdc_info)
		rc |= run_pdc_info(&c, sub_id);
	if (do_uim)
		rc |= run_uim(&c);
	if (do_sim_ef)
		rc |= run_sim_ef(&c);
	if (do_nas_ext)
		rc |= run_nas_impl(&c, true, sub_id);
	else if (do_nas)
		rc |= run_nas_impl(&c, false, sub_id);
	if (do_wds_profiles)
		rc |= run_wds_profiles(&c, sub_id);
	if (wds_smoke_apn)
		rc |= run_wds_smoke(&c, wds_smoke_apn, sub_id, mux_id, ep_type, iface_id,
				    client_type, configure_net, hold_sec);
	if (do_qmi_caps)
		rc |= run_qmi_caps(&c);
	if (do_loc_rev)
		rc |= run_loc_rev(&c);
	if (do_loc_probe)
		rc |= run_loc_probe(&c);
	close(c.fd);
	return rc ? 1 : 0;
}
