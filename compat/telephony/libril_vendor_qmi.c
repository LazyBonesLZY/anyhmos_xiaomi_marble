#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hril_vendor_abi.h"
#include "ims_core_wire.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif
#ifndef QRTR_PORT_CTRL
#define QRTR_NODE_BCAST 0xffffffffu
#define QRTR_PORT_CTRL 0xfffffffeu
struct sockaddr_qrtr {
	sa_family_t sq_family;
	uint32_t sq_node;
	uint32_t sq_port;
};
enum qrtr_pkt_type {
	QRTR_TYPE_DATA = 1,
	QRTR_TYPE_HELLO = 2,
	QRTR_TYPE_BYE = 3,
	QRTR_TYPE_NEW_SERVER = 4,
	QRTR_TYPE_DEL_SERVER = 5,
	QRTR_TYPE_DEL_CLIENT = 6,
	QRTR_TYPE_RESUME_TX = 7,
	QRTR_TYPE_EXIT = 8,
	QRTR_TYPE_PING = 9,
	QRTR_TYPE_NEW_LOOKUP = 10,
	QRTR_TYPE_DEL_LOOKUP = 11,
};
struct qrtr_ctrl_pkt {
	uint32_t cmd;
	union {
		struct {
			uint32_t service;
			uint32_t instance;
			uint32_t node;
			uint32_t port;
		} server;
		struct {
			uint32_t node;
			uint32_t port;
		} client;
	};
} __attribute__((__packed__));
#endif

#define SVC_WDS 0x01u
#define SVC_DMS 0x02u
#define SVC_NAS 0x03u
#define SVC_WMS 0x05u
#define SVC_VOICE 0x09u
#define SVC_UIM 0x0bu
#define SVC_IMSS 0x12u
#define SVC_WDA 0x1au
#define SVC_IMSA 0x21u
#define SVC_DPM 0x2fu
#define SVC_IMSDCM 0x302u

#define QMI_DPM_OPEN_PORT 0x0020u

#define QMI_WDS_START_NETWORK_INTERFACE 0x0020u
#define QMI_WDS_STOP_NETWORK_INTERFACE 0x0021u
#define QMI_WDS_GET_PKT_SRVC_STATUS 0x0022u
#define QMI_WDS_GET_CURRENT_SETTINGS 0x002du
#define QMI_WDS_SET_CLIENT_IP_FAMILY_PREF 0x004du
#define QMI_WDS_BIND_MUX_DATA_PORT 0x00a2u
#define QMI_WDS_BIND_SUBSCRIPTION 0x00afu

#define QMI_WDA_SET_DATA_FORMAT 0x0020u

#define QMI_DMS_GET_DEVICE_REV_ID 0x0023u
#define QMI_DMS_GET_DEVICE_SERIAL_NUMS 0x0025u
#define QMI_DMS_GET_OP_MODE 0x002du
#define QMI_DMS_SET_OP_MODE 0x002eu

#define QMI_NAS_GET_SERVING_SYSTEM 0x0024u
#define QMI_NAS_GET_HOME_NETWORK 0x0025u
#define QMI_NAS_GET_RF_BAND_INFO 0x0031u
#define QMI_NAS_SET_SYSTEM_SELECTION_PREF 0x0033u
#define QMI_NAS_GET_SYSTEM_SELECTION_PREF 0x0034u
#define QMI_NAS_GET_CELL_LOCATION_INFO 0x0043u
#define QMI_NAS_BIND_SUBSCRIPTION 0x0045u
#define QMI_NAS_SET_DUAL_STANDBY_PREF 0x004bu
#define QMI_NAS_GET_SYS_INFO 0x004du
#define QMI_NAS_GET_SIGNAL_INFO 0x004fu
#define QMI_NAS_GET_DUAL_STANDBY_PREF 0x005cu

#define QMI_WMS_SET_EVENT_REPORT 0x0001u
#define QMI_WMS_EVENT_REPORT_IND 0x0001u
#define QMI_WMS_RAW_SEND 0x0020u
#define QMI_WMS_RAW_READ 0x0022u
#define QMI_WMS_DELETE 0x0024u
#define QMI_WMS_GET_SMSC_ADDRESS 0x0034u
#define QMI_WMS_SET_SMSC_ADDRESS 0x0035u
#define QMI_WMS_SEND_ACK 0x0037u
#define QMI_WMS_BIND_SUBSCRIPTION 0x004cu

#define QMI_VOICE_INDICATION_REGISTER 0x0003u
#define QMI_VOICE_DIAL_CALL 0x0020u
#define QMI_VOICE_END_CALL 0x0021u
#define QMI_VOICE_ANSWER_CALL 0x0022u
#define QMI_VOICE_BURST_DTMF 0x0028u
#define QMI_VOICE_START_CONT_DTMF 0x0029u
#define QMI_VOICE_STOP_CONT_DTMF 0x002au
#define QMI_VOICE_ALL_CALL_STATUS_IND 0x002eu
#define QMI_VOICE_GET_ALL_CALL_INFO 0x002fu
#define QMI_VOICE_MANAGE_CALLS 0x0031u
#define QMI_VOICE_BIND_SUBSCRIPTION 0x0044u

#define QMI_IMSS_SET_IMS_SERVICES_ENABLED 0x008fu
#define QMI_IMSS_GET_IMS_SERVICES_ENABLED 0x0090u
#define QMI_IMSS_BIND 0x0098u

#define QMI_IMSA_GET_IMS_REG_STATUS 0x0020u
#define QMI_IMSA_GET_IMS_SRV_STATUS 0x0021u
#define QMI_IMSA_BIND 0x0033u

#define QMI_IMSDCM_START_CONN 0x0020u
#define QMI_IMSDCM_STOP_CONN 0x0021u

#define QMI_UIM_READ_TRANSPARENT 0x0020u
#define QMI_UIM_READ_RECORD 0x0021u
#define QMI_UIM_GET_FILE_ATTRIBUTES 0x0024u
#define QMI_UIM_GET_CARD_STATUS 0x002fu

#define SIM_CMD_READ_BINARY 176
#define SIM_CMD_READ_RECORD 178
#define SIM_CMD_GET_RESPONSE 192

#define QMI_MAX_BUF 4096
#define QMI_POLL_TIMEOUT_MS 1500
#define WORK_QUEUE_CAP 64
#define MAX_VOICE_CALLS 8

/* OHOS libnetsys_bpf_utils.z.so::NetsysBpfStats::GetTotalStats() whitelists rmnet0..rmnet3
 * (and skips rmnet_data0), so naming the IPA QMAPv5 netdev "rmnet0" enables live status-bar
 * cellular network speed and NetStats traffic accounting natively without donor binary patches. */
#define MARBLE_DATA_IFNAME "rmnet0"
#define MARBLE_EP_TYPE 4u
#define MARBLE_IFACE_ID 1u
#define MARBLE_MUX_ID 1u
#define MARBLE_IMS_MUX_BASE 2u
#define MARBLE_IPA_RX_EP 2u
#define MARBLE_IPA_TX_EP 23u

struct qmi_ep {
	uint32_t node;
	uint32_t port;
	bool valid;
};

struct qrtr_ctx {
	int sock;
	int nas_sock[2];
	bool nas_bound[2];
	int wms_sock[2];
	bool wms_bound[2];
	int voice_sock[2];
	bool voice_bound[2];
	struct qmi_ep wds;
	struct qmi_ep dms;
	struct qmi_ep nas;
	struct qmi_ep wms;
	struct qmi_ep voice;
	struct qmi_ep uim;
	struct qmi_ep imss;
	struct qmi_ep wda;
	struct qmi_ep imsa;
	struct qmi_ep dpm;
	uint16_t tx_id;
};

enum ril_task_kind {
	TASK_MODEM_SET_RADIO_STATE,
	TASK_MODEM_GET_RADIO_STATE,
	TASK_MODEM_GET_IMEI,
	TASK_MODEM_GET_IMEISV,
	TASK_MODEM_GET_MEID,
	TASK_MODEM_GET_VOICE_TECH,
	TASK_MODEM_GET_BASEBAND,
	TASK_MODEM_SHUTDOWN,

	TASK_SIM_GET_STATUS,
	TASK_SIM_GET_IO,
	TASK_SIM_GET_IMSI,
	TASK_SIM_GET_LOCK_STATUS,
	TASK_SIM_SET_ACTIVE,
	TASK_SIM_STK_READY,
	TASK_SIM_GET_PROTOCOL,
	TASK_SIM_SET_PROTOCOL,
	TASK_SIM_SEND_MATCHED_OP,
	TASK_SIM_GET_PRIMARY,
	TASK_SIM_SET_PRIMARY,

	TASK_NET_GET_SIGNAL,
	TASK_NET_GET_CS_REG,
	TASK_NET_GET_PS_REG,
	TASK_NET_GET_OPERATOR,
	TASK_NET_GET_NEIGHBOR_CELL,
	TASK_NET_GET_CURRENT_CELL,
	TASK_NET_GET_SEARCH,
	TASK_NET_GET_SEL_MODE,
	TASK_NET_SET_SEL_MODE,
	TASK_NET_GET_PREF_NET,
	TASK_NET_SET_PREF_NET,
	TASK_NET_GET_PHY_CHNL,
	TASK_NET_SET_FILTER,
	TASK_NET_SET_DEV_STATE,
	TASK_NET_GET_RRC,
	TASK_NET_SET_NR_OPT,
	TASK_NET_GET_NR_OPT,

	TASK_DATA_SET_APN,
	TASK_DATA_ACTIVATE_PDP,
	TASK_DATA_DEACTIVATE_PDP,
	TASK_DATA_GET_PDP_LIST,
	TASK_DATA_GET_LINK_BW,
	TASK_DATA_SET_LINK_BW,
	TASK_DATA_SET_PROFILE,
	TASK_DATA_SET_PERF_MODE,
	TASK_DATA_SET_SLEEP_MODE,
	TASK_DATA_SET_PERMITTED,
	TASK_DATA_GET_LINK_CAP,
	TASK_DATA_CLEAN_ALL,

	TASK_CALL_GET_LIST,
	TASK_CALL_DIAL,
	TASK_CALL_HANGUP,
	TASK_CALL_REJECT,
	TASK_CALL_ANSWER,
	TASK_CALL_HOLD,
	TASK_CALL_UNHOLD,
	TASK_CALL_SWITCH,
	TASK_CALL_SUPPLEMENT,
	TASK_CALL_GET_FAIL_REASON,
	TASK_CALL_START_DTMF,
	TASK_CALL_SEND_DTMF,
	TASK_CALL_STOP_DTMF,

	TASK_SMS_SEND_GSM,
	TASK_SMS_SEND_ACK,
	TASK_SMS_SEND_CDMA,
	TASK_SMS_SET_SMSC,
	TASK_SMS_GET_SMSC,
};

struct ril_task {
	enum ril_task_kind kind;
	const ReqDataInfo *req;
	int32_t slot_id;
	int32_t arg0;
	int32_t arg1;
	bool has_data_info;
	HRilSimIO sim_io;
	HRilRadioProtocol radio_proto;
	char sim_path[64];
	char apn[104];
	char pdp_type[24];
	char str0[128];
	char str1[640];
};

struct pdp_session {
	bool active;
	int wds_sock;
	uint32_t handle;
	int32_t slot_id;
	int32_t cid;
	char apn[104];
	char pdp_type[24];
	char ip[32];
	char ip_prefix[40];
	char gw[32];
	char dns1[32];
	char dns2[32];
	int32_t mtu;
};

struct cached_call_entry {
	int32_t index;
	int32_t dir;
	int32_t state;
	int32_t mode;
	int32_t mpty;
	int32_t type;
	uint8_t call_type; /* QMI_VOICE call_type: 0x00=CS, 0x08=VOICE_IP, 0x09=VT */
	char number[64];
};

#ifdef HRIL_VENDOR_HOST_TEST
static bool g_host_test_ims_call_bridge = false;
static bool ims_call_bridge_active(void) { return g_host_test_ims_call_bridge; }
#else
static bool ims_call_bridge_active(void)
{
	return access(MARBLE_IMS_CALL_ACTIVE_PATH, F_OK) == 0;
}
#endif

/* Filter parsed QMI_VOICE calls for CSControl: when SA 4014 ImsCallStub is active
 * (/data/service/el1/public/telephony/ims-call-active exists), filter out IP/IMS calls
 * (call_type 0x08 VOICE_IP / 0x09 VT) to prevent duplicate incoming call screens while
 * keeping 100% CS (0x00) and fallback (marker absent) delivery. */
static size_t filter_cs_call_entries(const struct cached_call_entry *in, size_t n_in,
				     bool ims_active,
				     struct cached_call_entry *out, size_t max_out,
				     size_t *ims_cnt_out)
{
	size_t n_out = 0;
	size_t n_ims = 0;
	for (size_t i = 0; i < n_in; i++) {
		bool is_ip = ims_is_ip_call_type(in[i].call_type);
		if (is_ip)
			n_ims++;
		if (!ims_active || !is_ip) {
			if (out && n_out < max_out)
				out[n_out++] = in[i];
		}
	}
	if (ims_cnt_out)
		*ims_cnt_out = n_ims;
	return n_out;
}

static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;

static const HRilReport *g_report_ops = NULL;
static struct qrtr_ctx g_qrtr = {
	.sock = -1,
	.nas_sock = { -1, -1 },
	.nas_bound = { false, false },
	.wms_sock = { -1, -1 },
	.wms_bound = { false, false },
	.voice_sock = { -1, -1 },
	.voice_bound = { false, false },
	.tx_id = 1,
};
static int32_t g_primary_slot = 0;
static int32_t g_active_slot = -1;
static bool g_primary_slot_explicit = false;
static int32_t g_pref_net = 0;
static int32_t g_nr_opt_mode = 3; /* NSA + SA */
static int32_t g_sel_mode = 0;
static int32_t g_notif_filter = 0;
static bool g_dsb_activated = false;
static bool g_worker_started = false;
static bool g_radio_on_notified = false;
static int32_t g_slot_radio_state[2] = { HRIL_RADIO_POWER_UNAVAILABLE, HRIL_RADIO_POWER_UNAVAILABLE };
static bool g_slot_radio_inited = false;
static int32_t g_slot_modem_id[2] = { 0, 1 };
static int32_t g_slot_radio_tech[2] = { 0x1fff, 0x000f };
static bool g_slot_proto_inited = false;
static char g_init_apn[104] = "";
static struct pdp_session g_pdp = {
	.active = false,
	.wds_sock = -1,
	.handle = 0,
	.slot_id = -1,
	.cid = 1,
};
static HRilRegStatusInfo g_last_cs_reg[2];
static HRilRegStatusInfo g_last_ps_reg[2];
static HRilRssi g_last_rssi[2];
static bool g_has_last_cs[2] = { false, false };
static bool g_has_last_ps[2] = { false, false };
static bool g_has_last_rssi[2] = { false, false };
static bool g_slot_present_cached[2] = { false, false };
static bool g_slot_present_known = false;

static bool g_ims_registered = false;
static bool g_ims_voice_avail = false;
static bool g_ims_sms_avail = false;
static char g_cached_smsc_addr[48] = "";
static int32_t g_cached_smsc_tosca = 145;
static uint8_t g_cached_smsc_raw[16];
static size_t g_cached_smsc_raw_len = 0;
static uint32_t g_last_mt_sms_tx_id = 0;
static bool g_last_mt_sms_on_ims = false;
static bool g_has_mt_sms_ack = false;
static struct cached_call_entry g_cached_calls[MAX_VOICE_CALLS];
static size_t g_cached_call_cnt = 0;
static int32_t g_last_call_fail_reason = 16; /* 3GPP NORMAL_CLEARING */
static bool g_hangup_pending = false;
static int32_t g_hangup_slot = -1;
static int g_call_watchdog_ticks = 0;

static struct ril_task g_queue[WORK_QUEUE_CAP];
static size_t g_q_head = 0;
static size_t g_q_tail = 0;
static size_t g_q_count = 0;

/* --- Little-endian / QMI TLV helpers --- */

static inline uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static inline void put_le64(uint8_t *p, uint64_t v)
{
	put_le32(p, (uint32_t)(v & 0xffffffffu));
	put_le32(p + 4, (uint32_t)(v >> 32));
}

static inline uint32_t le32_swap_host(uint32_t v)
{
	uint8_t b[4] = {
		(uint8_t)(v & 0xffu),
		(uint8_t)((v >> 8) & 0xffu),
		(uint8_t)((v >> 16) & 0xffu),
		(uint8_t)((v >> 24) & 0xffu),
	};
	uint32_t out;
	memcpy(&out, b, sizeof(out));
	return out;
}

static size_t tlv_append(uint8_t *buf, uint8_t type, const void *val, uint16_t len)
{
	buf[0] = type;
	put_le16(buf + 1, len);
	if (len > 0 && val != NULL)
		memcpy(buf + 3, val, len);
	return (size_t)3 + len;
}

static int qmi_find_tlv(const uint8_t *tlvs, size_t len, uint8_t type,
			const uint8_t **out_val, uint16_t *out_len)
{
	size_t off = 0;
	while (off + 3 <= len) {
		uint8_t t = tlvs[off];
		uint16_t l = get_le16(tlvs + off + 1);
		if (off + 3 + (size_t)l > len)
			return -EPROTO;
		if (t == type) {
			*out_val = tlvs + off + 3;
			*out_len = l;
			return 0;
		}
		off += 3 + (size_t)l;
	}
	return -ENOENT;
}

static int qmi_parse_result(const uint8_t *tlvs, size_t len, uint16_t *qmi_err)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (qmi_find_tlv(tlvs, len, 0x02, &v, &vl) < 0 || vl < 4)
		return -EPROTO;
	uint16_t res = get_le16(v);
	uint16_t err = get_le16(v + 2);
	*qmi_err = (res == 0) ? 0 : err;
	return 0;
}

/* --- EF / SIM decoding & path parsing helpers --- */

static int decode_iccid_bcd(const uint8_t *raw, size_t raw_len, char *out, size_t out_sz)
{
	if (!raw || raw_len == 0 || out_sz < 4)
		return -EINVAL;
	size_t pos = 0;
	for (size_t i = 0; i < raw_len && pos + 2 < out_sz; i++) {
		uint8_t lo = raw[i] & 0x0fu;
		uint8_t hi = (raw[i] >> 4) & 0x0fu;
		if (lo <= 9u)
			out[pos++] = (char)('0' + lo);
		if (hi <= 9u)
			out[pos++] = (char)('0' + hi);
		else
			break;
	}
	out[pos] = '\0';
	return (pos >= 10) ? 0 : -EPROTO;
}

static int decode_imsi_bcd(const uint8_t *raw, size_t raw_len, char *out, size_t out_sz)
{
	if (!raw || raw_len < 3 || out_sz < 8)
		return -EINVAL;
	size_t bytes = raw[0];
	if (bytes + 1 > raw_len)
		bytes = raw_len - 1;
	if (bytes < 2)
		return -EPROTO;
	size_t pos = 0;
	uint8_t hi0 = (raw[1] >> 4) & 0x0fu;
	if (hi0 <= 9u && pos + 1 < out_sz)
		out[pos++] = (char)('0' + hi0);
	for (size_t i = 2; i <= bytes && pos + 2 < out_sz; i++) {
		uint8_t lo = raw[i] & 0x0fu;
		uint8_t hi = (raw[i] >> 4) & 0x0fu;
		if (lo <= 9u)
			out[pos++] = (char)('0' + lo);
		if (hi <= 9u)
			out[pos++] = (char)('0' + hi);
	}
	out[pos] = '\0';
	return (pos >= 6) ? 0 : -EPROTO;
}

static void encode_hex_upper(const uint8_t *in, size_t len, char *out, size_t out_sz)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t pos = 0;
	for (size_t i = 0; i < len && pos + 2 < out_sz; i++) {
		out[pos++] = hex[(in[i] >> 4) & 0x0fu];
		out[pos++] = hex[in[i] & 0x0fu];
	}
	out[pos] = '\0';
}

static uint8_t hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return (uint8_t)(c - '0');
	if (c >= 'a' && c <= 'f')
		return (uint8_t)(10 + (c - 'a'));
	if (c >= 'A' && c <= 'F')
		return (uint8_t)(10 + (c - 'A'));
	return 0xffu;
}

/* Converts ASCII big-endian EF path (e.g. "3F007FFF") to QMI LE 16-bit pairs ({0x00,0x3f,0xff,0x7f}). */
static void parse_sim_path_le(const char *pathid, uint16_t fileid, uint8_t *out, uint8_t *out_len)
{
	char clean[64];
	size_t clen = 0;
	if (pathid != NULL) {
		for (size_t i = 0; pathid[i] != '\0' && clen + 1 < sizeof(clean); i++) {
			if (isxdigit((unsigned char)pathid[i]))
				clean[clen++] = pathid[i];
		}
	}
	if (clen >= 4 && (clen % 4) == 0 && clen <= 24) {
		uint8_t pos = 0;
		for (size_t i = 0; i + 3 < clen; i += 4) {
			uint8_t b_hi = (uint8_t)((hex_nibble(clean[i]) << 4) | hex_nibble(clean[i + 1]));
			uint8_t b_lo = (uint8_t)((hex_nibble(clean[i + 2]) << 4) | hex_nibble(clean[i + 3]));
			out[pos++] = b_lo;
			out[pos++] = b_hi;
		}
		*out_len = pos;
		return;
	}
	if (fileid == 0x2fe2u || fileid == 0x2f00u || fileid == 0x2f05u) {
		out[0] = 0x00;
		out[1] = 0x3f;
		*out_len = 2;
	} else {
		out[0] = 0x00;
		out[1] = 0x3f;
		out[2] = 0xff;
		out[3] = 0x7f;
		*out_len = 4;
	}
}

/* --- QRTR transport --- */

static void qrtr_close_locked(void)
{
	if (g_pdp.wds_sock >= 0) {
		close(g_pdp.wds_sock);
		g_pdp.wds_sock = -1;
	}
	g_pdp.active = false;
	g_pdp.handle = 0;
	if (g_qrtr.sock >= 0) {
		close(g_qrtr.sock);
		g_qrtr.sock = -1;
	}
	for (int i = 0; i < 2; i++) {
		if (g_qrtr.nas_sock[i] >= 0) {
			close(g_qrtr.nas_sock[i]);
			g_qrtr.nas_sock[i] = -1;
		}
		g_qrtr.nas_bound[i] = false;
		if (g_qrtr.wms_sock[i] >= 0) {
			close(g_qrtr.wms_sock[i]);
			g_qrtr.wms_sock[i] = -1;
		}
		g_qrtr.wms_bound[i] = false;
		if (g_qrtr.voice_sock[i] >= 0) {
			close(g_qrtr.voice_sock[i]);
			g_qrtr.voice_sock[i] = -1;
		}
		g_qrtr.voice_bound[i] = false;
	}
	memset(&g_qrtr.wds, 0, sizeof(g_qrtr.wds));
	memset(&g_qrtr.dms, 0, sizeof(g_qrtr.dms));
	memset(&g_qrtr.nas, 0, sizeof(g_qrtr.nas));
	memset(&g_qrtr.wms, 0, sizeof(g_qrtr.wms));
	memset(&g_qrtr.voice, 0, sizeof(g_qrtr.voice));
	memset(&g_qrtr.uim, 0, sizeof(g_qrtr.uim));
	memset(&g_qrtr.imss, 0, sizeof(g_qrtr.imss));
	memset(&g_qrtr.wda, 0, sizeof(g_qrtr.wda));
	memset(&g_qrtr.imsa, 0, sizeof(g_qrtr.imsa));
	memset(&g_qrtr.dpm, 0, sizeof(g_qrtr.dpm));
	g_dsb_activated = false;
}

static int qrtr_discover_locked(void)
{
	struct qrtr_ctrl_pkt pkt;
	struct sockaddr_qrtr addr;
	socklen_t slen = sizeof(addr);
	struct pollfd pfd;

	if (g_qrtr.sock < 0) {
		g_qrtr.sock = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (g_qrtr.sock < 0)
			return -errno;
		if (getsockname(g_qrtr.sock, (struct sockaddr *)&addr, &slen) < 0) {
			qrtr_close_locked();
			return -EIO;
		}
		struct sockaddr_qrtr ctrl = {
			.sq_family = AF_QIPCRTR,
			.sq_node = addr.sq_node,
			.sq_port = QRTR_PORT_CTRL,
		};
		memset(&pkt, 0, sizeof(pkt));
		pkt.cmd = le32_swap_host(QRTR_TYPE_NEW_LOOKUP);
		if (sendto(g_qrtr.sock, &pkt, sizeof(pkt), 0,
			   (struct sockaddr *)&ctrl, sizeof(ctrl)) < 0) {
			int err = -errno;
			qrtr_close_locked();
			return err;
		}
	}

	if (g_qrtr.wds.valid && g_qrtr.dms.valid && g_qrtr.nas.valid &&
	    g_qrtr.wms.valid && g_qrtr.voice.valid && g_qrtr.uim.valid &&
	    g_qrtr.imss.valid && g_qrtr.wda.valid && g_qrtr.imsa.valid &&
	    g_qrtr.dpm.valid)
		return 0;

	for (int iter = 0; iter < 160; iter++) {
		pfd.fd = g_qrtr.sock;
		pfd.events = POLLIN;
		pfd.revents = 0;
		int pr = poll(&pfd, 1, 120);
		if (pr <= 0)
			break;
		struct sockaddr_qrtr src;
		socklen_t src_len = sizeof(src);
		uint8_t buf[512];
		ssize_t n = recvfrom(g_qrtr.sock, buf, sizeof(buf), 0,
				     (struct sockaddr *)&src, &src_len);
		if (n < (ssize_t)sizeof(struct qrtr_ctrl_pkt))
			continue;
		const struct qrtr_ctrl_pkt *cp = (const struct qrtr_ctrl_pkt *)buf;
		uint32_t cmd = le32_swap_host(cp->cmd);
		if (cmd != QRTR_TYPE_NEW_SERVER)
			continue;
		uint32_t svc = le32_swap_host(cp->server.service);
		uint32_t node = le32_swap_host(cp->server.node);
		uint32_t port = le32_swap_host(cp->server.port);
		if (svc == 0 && node == 0 && port == 0)
			break;
		if (svc == SVC_WDS)
			g_qrtr.wds = (struct qmi_ep){ node, port, true };
		else if (svc == SVC_DMS)
			g_qrtr.dms = (struct qmi_ep){ node, port, true };
		else if (svc == SVC_NAS)
			g_qrtr.nas = (struct qmi_ep){ node, port, true };
		else if (svc == SVC_WMS)
			g_qrtr.wms = (struct qmi_ep){ node, port, true };
		else if (svc == SVC_VOICE)
			g_qrtr.voice = (struct qmi_ep){ node, port, true };
		else if (svc == SVC_UIM)
			g_qrtr.uim = (struct qmi_ep){ node, port, true };
		else if (svc == SVC_IMSS)
			g_qrtr.imss = (struct qmi_ep){ node, port, true };
		else if (svc == SVC_WDA)
			g_qrtr.wda = (struct qmi_ep){ node, port, true };
		else if (svc == SVC_IMSA)
			g_qrtr.imsa = (struct qmi_ep){ node, port, true };
		else if (svc == SVC_DPM)
			g_qrtr.dpm = (struct qmi_ep){ node, port, true };
	}

	return (g_qrtr.dms.valid || g_qrtr.uim.valid || g_qrtr.nas.valid || g_qrtr.wds.valid)
	     ? 0 : -ENODEV;
}

static int qmi_rpc_on_sock_locked(int sock, uint32_t svc, uint16_t msg_id,
				  const uint8_t *req_tlvs, size_t req_len,
				  uint8_t *resp_tlvs, size_t resp_cap, size_t *out_len,
				  uint16_t *out_qmi_err)
{
	if (sock < 0)
		return -EINVAL;
	if (qrtr_discover_locked() < 0)
		return -ENODEV;

	const struct qmi_ep *ep = NULL;
	if (svc == SVC_WDS)
		ep = &g_qrtr.wds;
	else if (svc == SVC_DMS)
		ep = &g_qrtr.dms;
	else if (svc == SVC_NAS)
		ep = &g_qrtr.nas;
	else if (svc == SVC_WMS)
		ep = &g_qrtr.wms;
	else if (svc == SVC_VOICE)
		ep = &g_qrtr.voice;
	else if (svc == SVC_UIM)
		ep = &g_qrtr.uim;
	else if (svc == SVC_IMSS)
		ep = &g_qrtr.imss;
	else if (svc == SVC_WDA)
		ep = &g_qrtr.wda;
	else if (svc == SVC_IMSA)
		ep = &g_qrtr.imsa;
	else if (svc == SVC_DPM)
		ep = &g_qrtr.dpm;

	if (!ep || !ep->valid)
		return -ENODEV;

	if (7 + req_len > QMI_MAX_BUF)
		return -EMSGSIZE;

	uint8_t pkt[QMI_MAX_BUF];
	uint16_t tx = g_qrtr.tx_id++;
	if (g_qrtr.tx_id == 0)
		g_qrtr.tx_id = 1;

	pkt[0] = 0x00; /* QMI request */
	put_le16(pkt + 1, tx);
	put_le16(pkt + 3, msg_id);
	put_le16(pkt + 5, (uint16_t)req_len);
	if (req_len > 0 && req_tlvs != NULL)
		memcpy(pkt + 7, req_tlvs, req_len);

	struct sockaddr_qrtr dst = {
		.sq_family = AF_QIPCRTR,
		.sq_node = ep->node,
		.sq_port = ep->port,
	};
	if (sendto(sock, pkt, 7 + req_len, 0,
		   (struct sockaddr *)&dst, sizeof(dst)) < 0) {
		return -errno;
	}

	for (int attempt = 0; attempt < 24; attempt++) {
		struct pollfd pfd = { .fd = sock, .events = POLLIN };
		int pr = poll(&pfd, 1, QMI_POLL_TIMEOUT_MS);
		if (pr == 0)
			return -ETIMEDOUT;
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		uint8_t rbuf[QMI_MAX_BUF];
		struct sockaddr_qrtr src;
		socklen_t slen = sizeof(src);
		ssize_t n = recvfrom(sock, rbuf, sizeof(rbuf), 0,
				     (struct sockaddr *)&src, &slen);
		if (n < 7)
			continue;
		if (src.sq_port == QRTR_PORT_CTRL || src.sq_node != ep->node || src.sq_port != ep->port)
			continue;
		if (rbuf[0] != 0x02) /* QMI response */
			continue;
		if (get_le16(rbuf + 1) != tx || get_le16(rbuf + 3) != msg_id)
			continue;
		uint16_t payload_len = get_le16(rbuf + 5);
		if (7 + (size_t)payload_len > (size_t)n || payload_len > resp_cap)
			return -EPROTO;
		memcpy(resp_tlvs, rbuf + 7, payload_len);
		*out_len = payload_len;
		*out_qmi_err = 0;
		(void)qmi_parse_result(resp_tlvs, payload_len, out_qmi_err);
		return 0;
	}
	return -ETIMEDOUT;
}

static int qmi_rpc_locked(uint32_t svc, uint16_t msg_id,
			  const uint8_t *req_tlvs, size_t req_len,
			  uint8_t *resp_tlvs, size_t resp_cap, size_t *out_len,
			  uint16_t *out_qmi_err)
{
	if (qrtr_discover_locked() < 0)
		return -ENODEV;
	int rc = qmi_rpc_on_sock_locked(g_qrtr.sock, svc, msg_id, req_tlvs, req_len,
					resp_tlvs, resp_cap, out_len, out_qmi_err);
	if (rc == -EPIPE || rc == -ENOTCONN || rc == -ECONNRESET)
		qrtr_close_locked();
	return rc;
}

static size_t build_nas_bind_sub_tlvs(uint8_t *buf, uint8_t sub_id)
{
	return tlv_append(buf, 0x01, &sub_id, 1);
}

static int qmi_nas_rpc_slot_locked(int32_t slot, uint16_t msg_id,
				   const uint8_t *req_tlvs, size_t req_len,
				   uint8_t *resp_tlvs, size_t resp_cap, size_t *out_len,
				   uint16_t *out_qmi_err)
{
	if (slot < 0 || slot > 1)
		return -EINVAL;
	if (qrtr_discover_locked() < 0)
		return -ENODEV;
	if (!g_qrtr.nas.valid)
		return -ENODEV;

	if (g_qrtr.nas_sock[slot] < 0) {
		g_qrtr.nas_sock[slot] = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (g_qrtr.nas_sock[slot] < 0)
			return -errno;
		g_qrtr.nas_bound[slot] = false;
	}

	if (!g_qrtr.nas_bound[slot]) {
		uint8_t breq[8], bresp[QMI_MAX_BUF];
		size_t bl = build_nas_bind_sub_tlvs(breq, (uint8_t)slot);
		size_t rbl = 0;
		uint16_t berr = 0;
		int brc = qmi_rpc_on_sock_locked(g_qrtr.nas_sock[slot], SVC_NAS,
						 QMI_NAS_BIND_SUBSCRIPTION,
						 breq, bl, bresp, sizeof(bresp), &rbl, &berr);
		if (brc == 0 && berr == 0x01u) {
			uint8_t u32buf[4];
			put_le32(u32buf, (uint32_t)slot);
			bl = tlv_append(breq, 0x01, u32buf, 4);
			brc = qmi_rpc_on_sock_locked(g_qrtr.nas_sock[slot], SVC_NAS,
						     QMI_NAS_BIND_SUBSCRIPTION,
						     breq, bl, bresp, sizeof(bresp), &rbl, &berr);
		}
		if (brc == -EPIPE || brc == -ENOTCONN || brc == -ECONNRESET) {
			close(g_qrtr.nas_sock[slot]);
			g_qrtr.nas_sock[slot] = -1;
			return brc;
		}
		if (brc == 0 && berr == 0)
			g_qrtr.nas_bound[slot] = true;
	}

	int rc = qmi_rpc_on_sock_locked(g_qrtr.nas_sock[slot], SVC_NAS, msg_id,
					req_tlvs, req_len, resp_tlvs, resp_cap,
					out_len, out_qmi_err);
	if (rc == -EPIPE || rc == -ENOTCONN || rc == -ECONNRESET) {
		close(g_qrtr.nas_sock[slot]);
		g_qrtr.nas_sock[slot] = -1;
		g_qrtr.nas_bound[slot] = false;
	}
	return rc;
}

static int qmi_sub_svc_rpc_slot_locked(int32_t slot, uint32_t svc, uint16_t bind_msg_id,
				       int socks[2], bool bound[2], uint16_t msg_id,
				       const uint8_t *req_tlvs, size_t req_len,
				       uint8_t *resp_tlvs, size_t resp_cap, size_t *out_len,
				       uint16_t *out_qmi_err)
{
	if (slot < 0 || slot > 1)
		return -EINVAL;
	if (qrtr_discover_locked() < 0)
		return -ENODEV;
	if (socks[slot] < 0) {
		socks[slot] = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (socks[slot] < 0)
			return -errno;
		bound[slot] = false;
	}
	if (!bound[slot]) {
		uint8_t breq[8], bresp[QMI_MAX_BUF];
		uint8_t sub = (uint8_t)slot;
		size_t bl = tlv_append(breq, 0x01, &sub, 1);
		size_t rbl = 0;
		uint16_t berr = 0;
		int brc = qmi_rpc_on_sock_locked(socks[slot], svc, bind_msg_id,
						 breq, bl, bresp, sizeof(bresp), &rbl, &berr);
		if (brc == -EPIPE || brc == -ENOTCONN || brc == -ECONNRESET) {
			close(socks[slot]);
			socks[slot] = -1;
			return brc;
		}
		if (brc == 0 && berr == 0)
			bound[slot] = true;
	}
	int rc = qmi_rpc_on_sock_locked(socks[slot], svc, msg_id,
					req_tlvs, req_len, resp_tlvs, resp_cap,
					out_len, out_qmi_err);
	if (rc == -EPIPE || rc == -ENOTCONN || rc == -ECONNRESET) {
		close(socks[slot]);
		socks[slot] = -1;
		bound[slot] = false;
	}
	return rc;
}

static int qmi_wms_rpc_slot_locked(int32_t slot, uint16_t msg_id,
				   const uint8_t *req_tlvs, size_t req_len,
				   uint8_t *resp_tlvs, size_t resp_cap, size_t *out_len,
				   uint16_t *out_qmi_err)
{
	int32_t qslot = (slot == 0 || slot == 1) ? slot : 0;
	if (!g_qrtr.wms.valid && qrtr_discover_locked() < 0)
		return -ENODEV;
	if (!g_qrtr.wms.valid)
		return -ENODEV;
	return qmi_sub_svc_rpc_slot_locked(qslot, SVC_WMS, QMI_WMS_BIND_SUBSCRIPTION,
					   g_qrtr.wms_sock, g_qrtr.wms_bound,
					   msg_id, req_tlvs, req_len,
					   resp_tlvs, resp_cap, out_len, out_qmi_err);
}

static int qmi_voice_rpc_slot_locked(int32_t slot, uint16_t msg_id,
				     const uint8_t *req_tlvs, size_t req_len,
				     uint8_t *resp_tlvs, size_t resp_cap, size_t *out_len,
				     uint16_t *out_qmi_err)
{
	int32_t qslot = (slot == 0 || slot == 1) ? slot : 0;
	if (!g_qrtr.voice.valid && qrtr_discover_locked() < 0)
		return -ENODEV;
	if (!g_qrtr.voice.valid)
		return -ENODEV;
	return qmi_sub_svc_rpc_slot_locked(qslot, SVC_VOICE, QMI_VOICE_BIND_SUBSCRIPTION,
					   g_qrtr.voice_sock, g_qrtr.voice_bound,
					   msg_id, req_tlvs, req_len,
					   resp_tlvs, resp_cap, out_len, out_qmi_err);
}

/* --- Mapping & QMI parsing helpers (pure C, shared with host tests) --- */

static int32_t qmi_radio_if_to_hril_tech(uint8_t rif)
{
	switch (rif) {
	case 1: return HRIL_TECH_CDMA1X;
	case 2: return HRIL_TECH_EVDO;
	case 4: return HRIL_TECH_GSM;
	case 5: return HRIL_TECH_WCDMA;
	case 8: return HRIL_TECH_LTE;
	case 9: return HRIL_TECH_TD_SCDMA;
	case 12: return HRIL_TECH_NR;
	default: return HRIL_TECH_UNKNOWN;
	}
}

static const char *hril_tech_name(int32_t tech)
{
	switch (tech) {
	case HRIL_TECH_NR: return "NR";
	case HRIL_TECH_LTE: return "LTE";
	case HRIL_TECH_WCDMA: return "WCDMA";
	case HRIL_TECH_TD_SCDMA: return "TD-SCDMA";
	case HRIL_TECH_GSM: return "GSM";
	case HRIL_TECH_CDMA1X: return "CDMA1X";
	case HRIL_TECH_EVDO: return "EVDO";
	default: return "Unknown";
	}
}

static void init_empty_rssi(HRilRssi *r)
{
	memset(r, 0, sizeof(*r));
}

static size_t build_nas_set_dsb_tlvs(uint8_t *buf, uint8_t standby_pref,
				     uint8_t pri_sub, uint8_t data_sub,
				     uint8_t voice_sub, uint64_t active_mask)
{
	uint8_t mask_buf[8];
	put_le64(mask_buf, active_mask);
	size_t off = 0;
	off += tlv_append(buf + off, 0x10, &standby_pref, 1);
	off += tlv_append(buf + off, 0x11, &pri_sub, 1);
	off += tlv_append(buf + off, 0x12, &data_sub, 1);
	off += tlv_append(buf + off, 0x13, &voice_sub, 1);
	off += tlv_append(buf + off, 0x14, mask_buf, 8);
	return off;
}

static int32_t sanitize_qmi_dbm16(int16_t v, int32_t min_ok, int32_t max_ok)
{
	if (v == (int16_t)-32768 || (int32_t)v < min_ok || (int32_t)v > max_ok)
		return 0;
	return (int32_t)v;
}

static int32_t sanitize_qmi_dbm8(int8_t v, int32_t min_ok, int32_t max_ok)
{
	if (v == (int8_t)-128 || (int32_t)v < min_ok || (int32_t)v > max_ok)
		return 0;
	return (int32_t)v;
}

static void parse_nas_signal_info(const uint8_t *tlvs, size_t len, HRilRssi *out)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;

	init_empty_rssi(out);
	if (qmi_find_tlv(tlvs, len, 0x12, &v, &vl) == 0 && vl >= 1) {
		int8_t rx = (int8_t)v[0];
		out->vals[HRIL_RSSI_GSM_RXLEV] = sanitize_qmi_dbm8(rx, -125, -1);
	}
	if (qmi_find_tlv(tlvs, len, 0x13, &v, &vl) == 0 && vl >= 3) {
		int8_t rx = (int8_t)v[0];
		int16_t ecio = (int16_t)get_le16(v + 1);
		out->vals[HRIL_RSSI_WCDMA_RXLEV] = sanitize_qmi_dbm8(rx, -125, -1);
		out->vals[HRIL_RSSI_WCDMA_RSCP] = out->vals[HRIL_RSSI_WCDMA_RXLEV];
		out->vals[HRIL_RSSI_WCDMA_ECIO] = sanitize_qmi_dbm16(ecio, -50, 0);
	}
	if (qmi_find_tlv(tlvs, len, 0x19, &v, &vl) == 0 && vl >= 2) {
		int16_t rscp = (int16_t)get_le16(v);
		out->vals[HRIL_RSSI_WCDMA_RSCP] = sanitize_qmi_dbm16(rscp, -140, -1);
	}
	if (qmi_find_tlv(tlvs, len, 0x14, &v, &vl) == 0 && vl >= 6) {
		int8_t rssi = (int8_t)v[0];
		int8_t rsrq = (int8_t)v[1];
		int16_t rsrp = (int16_t)get_le16(v + 2);
		int16_t snr = (int16_t)get_le16(v + 4);
		out->vals[HRIL_RSSI_LTE_RXLEV] = sanitize_qmi_dbm8(rssi, -125, -1);
		out->vals[HRIL_RSSI_LTE_RSRQ] = sanitize_qmi_dbm8(rsrq, -40, 0);
		out->vals[HRIL_RSSI_LTE_RSRP] = sanitize_qmi_dbm16(rsrp, -160, -1);
		out->vals[HRIL_RSSI_LTE_SNR] = sanitize_qmi_dbm16(snr, -300, 600);
	}
	if (qmi_find_tlv(tlvs, len, 0x17, &v, &vl) == 0 && vl >= 4) {
		int16_t rsrp = (int16_t)get_le16(v + 0);
		int16_t snr = (int16_t)get_le16(v + 2);
		out->vals[HRIL_RSSI_NR_RSRP] = sanitize_qmi_dbm16(rsrp, -160, -1);
		out->vals[HRIL_RSSI_NR_SINR] = sanitize_qmi_dbm16(snr, -300, 600);
		if (vl >= 6) {
			int16_t rsrq = (int16_t)get_le16(v + 4);
			out->vals[HRIL_RSSI_NR_RSRQ] = sanitize_qmi_dbm16(rsrq, -40, 0);
		}
	}
	if (out->vals[HRIL_RSSI_NR_RSRQ] == 0 &&
	    qmi_find_tlv(tlvs, len, 0x18, &v, &vl) == 0 && vl >= 2) {
		int16_t rsrq = (int16_t)get_le16(v);
		out->vals[HRIL_RSSI_NR_RSRQ] = sanitize_qmi_dbm16(rsrq, -40, 0);
	}
}

static void parse_nas_serving_info(const uint8_t *tlvs, size_t len, bool is_ps,
				   HRilRegStatusInfo *reg, HRilVoiceRadioInfo *voice,
				   char *numeric_out, size_t numeric_sz,
				   char *name_out, size_t name_sz)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	int32_t tech = HRIL_TECH_UNKNOWN;
	int32_t reg_state = HRIL_REG_NOT_REGISTERED;
	int32_t cs_state = 0;
	int32_t ps_state = 0;
	int32_t lac = -1;
	int32_t cell_id = -1;
	int32_t roam = 1; /* 0 = roaming, 1 = home per 3GPP2 */
	bool has_nr = false;

	if (qmi_find_tlv(tlvs, len, 0x01, &v, &vl) == 0 && vl >= 5) {
		uint8_t qreg = v[0];
		cs_state = v[1];
		ps_state = v[2];
		uint8_t nif = v[4];
		for (uint8_t i = 0; i < nif && (size_t)(5 + i) < vl; i++) {
			int32_t t = qmi_radio_if_to_hril_tech(v[5 + i]);
			if (t == HRIL_TECH_NR)
				has_nr = true;
			if (t != HRIL_TECH_UNKNOWN)
				tech = t;
		}
		if (qreg == 1)
			reg_state = HRIL_REG_HOME;
		else if (qreg == 2)
			reg_state = HRIL_REG_SEARCHING;
		else if (qreg == 3)
			reg_state = HRIL_REG_DENIED;
		else if (qreg == 4)
			reg_state = HRIL_REG_UNKNOWN;
		else
			reg_state = HRIL_REG_NOT_REGISTERED;
	}

	if (qmi_find_tlv(tlvs, len, 0x10, &v, &vl) == 0 && vl >= 1) {
		roam = v[0];
		if (reg_state == HRIL_REG_HOME && roam == 0)
			reg_state = HRIL_REG_ROAMING;
	}
	if (qmi_find_tlv(tlvs, len, 0x1c, &v, &vl) == 0 && vl >= 2)
		lac = (int32_t)get_le16(v);
	else if (qmi_find_tlv(tlvs, len, 0x24, &v, &vl) == 0 && vl >= 2)
		lac = (int32_t)get_le16(v);

	if (qmi_find_tlv(tlvs, len, 0x1d, &v, &vl) == 0 && vl >= 4)
		cell_id = (int32_t)get_le32(v);

	if (numeric_out && numeric_sz >= 8)
		numeric_out[0] = '\0';
	if (name_out && name_sz > 0)
		name_out[0] = '\0';
	if (qmi_find_tlv(tlvs, len, 0x12, &v, &vl) == 0 && vl >= 5) {
		uint16_t mcc = get_le16(v);
		uint16_t mnc = get_le16(v + 2);
		uint8_t dlen = v[4];
		if (mcc != 0xffffu && mnc != 0xffffu && numeric_out && numeric_sz >= 8) {
			if (mnc >= 100u)
				snprintf(numeric_out, numeric_sz, "%03u%03u", mcc, mnc);
			else
				snprintf(numeric_out, numeric_sz, "%03u%02u", mcc, mnc);
		}
		if (name_out && name_sz > 1 && dlen > 0 && (size_t)(5 + dlen) <= vl) {
			size_t cp = dlen < name_sz - 1 ? dlen : name_sz - 1;
			memcpy(name_out, v + 5, cp);
			name_out[cp] = '\0';
		}
	}

	if (reg != NULL) {
		memset(reg, 0, sizeof(*reg));
		reg->notifyMode = 2;
		reg->regStatus = is_ps ? (ps_state == 1 ? reg_state : HRIL_REG_NOT_REGISTERED)
				       : (cs_state == 1 ? reg_state : reg_state);
		reg->lac = lac;
		reg->cellId = cell_id;
		reg->actType = tech;
		reg->nrAvailable = has_nr ? 1 : 0;
		reg->enDcAvailable = (tech == HRIL_TECH_LTE || has_nr) ? 1 : 0;
		reg->dcnr = has_nr ? 1 : 0;
	}

	if (voice != NULL) {
		memset(voice, 0, sizeof(*voice));
		voice->srvStatus = (reg_state == HRIL_REG_HOME || reg_state == HRIL_REG_ROAMING) ? 2 : 0;
		voice->srvDomain = (cs_state == 1 && ps_state == 1) ? 3
				 : (ps_state == 1 ? 2 : (cs_state == 1 ? 1 : 0));
		voice->roamStatus = (reg_state == HRIL_REG_ROAMING) ? 1 : 0;
		voice->simStatus = HRIL_SIM_READY;
		voice->lockStatus = 0;
		voice->sysMode = tech;
		voice->sysModeName = hril_tech_name(tech);
		voice->actType = tech;
		voice->actName = hril_tech_name(tech);
	}
}

static bool parse_nas_sys_info(const uint8_t *tlvs, size_t len, bool is_ps,
			       HRilRegStatusInfo *reg_out, HRilVoiceRadioInfo *voice_out,
			       char *num_out, size_t num_sz,
			       char *name_out, size_t name_sz)
{
	const uint8_t *v_lte_st = NULL, *v_nr_st = NULL, *v_w_st = NULL, *v_g_st = NULL;
	const uint8_t *v_lte = NULL, *v_nr = NULL, *v_w = NULL, *v_g = NULL;
	const uint8_t *v_ims = NULL, *v_dcnr = NULL;
	uint16_t l_lte_st = 0, l_nr_st = 0, l_w_st = 0, l_g_st = 0;
	uint16_t l_lte = 0, l_nr = 0, l_w = 0, l_g = 0;
	uint16_t l_ims = 0, l_dcnr = 0;
	bool has_any = false;

	uint8_t nr_srv = 0, lte_srv = 0, wcdma_srv = 0, gsm_srv = 0;
	if (qmi_find_tlv(tlvs, len, 0x4a, &v_nr_st, &l_nr_st) == 0 && l_nr_st >= 3) {
		nr_srv = v_nr_st[0];
		has_any = true;
	}
	if (qmi_find_tlv(tlvs, len, 0x14, &v_lte_st, &l_lte_st) == 0 && l_lte_st >= 3) {
		lte_srv = v_lte_st[0];
		has_any = true;
	}
	if (qmi_find_tlv(tlvs, len, 0x13, &v_w_st, &l_w_st) == 0 && l_w_st >= 3) {
		wcdma_srv = v_w_st[0];
		has_any = true;
	}
	if (qmi_find_tlv(tlvs, len, 0x12, &v_g_st, &l_g_st) == 0 && l_g_st >= 3) {
		gsm_srv = v_g_st[0];
		has_any = true;
	}

	int32_t tech = HRIL_TECH_UNKNOWN;
	uint8_t chosen_srv = 0;
	const uint8_t *info = NULL;
	uint16_t info_len = 0;

	if (qmi_find_tlv(tlvs, len, 0x19, &v_lte, &l_lte) == 0 && l_lte >= 26)
		has_any = true;
	else
		v_lte = NULL;
	if (qmi_find_tlv(tlvs, len, 0x4b, &v_nr, &l_nr) == 0 && l_nr >= 26)
		has_any = true;
	else
		v_nr = NULL;
	if (qmi_find_tlv(tlvs, len, 0x18, &v_w, &l_w) == 0 && l_w >= 26)
		has_any = true;
	else
		v_w = NULL;
	if (qmi_find_tlv(tlvs, len, 0x17, &v_g, &l_g) == 0 && l_g >= 26)
		has_any = true;
	else
		v_g = NULL;

	if (!has_any)
		return false;

	if (nr_srv == 2u) {
		tech = HRIL_TECH_NR;
		chosen_srv = nr_srv;
		info = v_nr ? v_nr : v_lte;
		info_len = v_nr ? l_nr : l_lte;
	} else if (lte_srv > 0u) {
		tech = HRIL_TECH_LTE;
		chosen_srv = lte_srv;
		info = v_lte ? v_lte : v_nr;
		info_len = v_lte ? l_lte : l_nr;
	} else if (wcdma_srv > 0u) {
		tech = HRIL_TECH_WCDMA;
		chosen_srv = wcdma_srv;
		info = v_w;
		info_len = l_w;
	} else if (gsm_srv > 0u) {
		tech = HRIL_TECH_GSM;
		chosen_srv = gsm_srv;
		info = v_g;
		info_len = l_g;
	} else if (nr_srv > 0u) {
		tech = HRIL_TECH_NR;
		chosen_srv = nr_srv;
		info = v_nr;
		info_len = l_nr;
	}

	uint8_t srv_domain = 0;
	uint8_t roam_status = 0;
	int32_t lac = -1;
	int32_t cell_id = -1;
	char plmn[16] = "";
	if (info != NULL && info_len >= 26) {
		if (info[0])
			srv_domain = info[1];
		if (info[4])
			roam_status = info[5];
		if (info_len >= 29 && info[26])
			lac = (int32_t)get_le16(info + 27);
		else if (info[8])
			lac = (int32_t)get_le16(info + 9);
		if (info[11])
			cell_id = (int32_t)get_le32(info + 12);
		if (info[19]) {
			size_t n = 0;
			for (size_t i = 0; i < 6 && n + 1 < sizeof(plmn); i++) {
				uint8_t ch = info[20 + i];
				if (ch >= '0' && ch <= '9')
					plmn[n++] = (char)ch;
			}
			plmn[n] = '\0';
		}
	}

	uint8_t ims_voice = 0;
	if (qmi_find_tlv(tlvs, len, 0x29, &v_ims, &l_ims) == 0 && l_ims >= 1)
		ims_voice = v_ims[0];
	if (ims_voice == 0u && qmi_find_tlv(tlvs, len, 0x56, &v_ims, &l_ims) == 0 && l_ims >= 1)
		ims_voice = v_ims[0]; /* nr5g_ims_voice_avail on 5G NR SA */

	/* QMI NAS_GET_SYS_INFO TLVs:
	 *   0x4e = nr5g_dcnr_restriction (1 = RESTRICTED, 0 = ALLOWED)
	 *   0x4f = nr5g_endc_available   (1 = ENDC AVAILABLE, 0 = NOT AVAILABLE)
	 * CoreService UpdateNrState (0xa5c44..0xa5c64):
	 *   enDcAvailable == 1 && dcnr == 0 => NrState = 2 (NSA available -> ConfigD -> NR/5G icon!)
	 *   dcnr != 0                       => NrState = 1 (NR_STATE_NOT_SUPPORT!)
	 */
	const uint8_t *v_endc = NULL;
	uint16_t l_endc = 0;
	bool dcnr_restricted = false;
	bool endc_avail = false;
	if (qmi_find_tlv(tlvs, len, 0x4e, &v_dcnr, &l_dcnr) == 0 && l_dcnr >= 1)
		dcnr_restricted = (v_dcnr[0] != 0);
	if (qmi_find_tlv(tlvs, len, 0x4f, &v_endc, &l_endc) == 0 && l_endc >= 1)
		endc_avail = (v_endc[0] != 0);
	if (nr_srv > 0u)
		endc_avail = true;

	int32_t reg_state = HRIL_REG_NOT_REGISTERED;
	if (chosen_srv == 2u) {
		int32_t reg_ok = (roam_status != 0u) ? HRIL_REG_ROAMING : HRIL_REG_HOME;
		if (is_ps) {
			reg_state = (srv_domain == 2u || srv_domain == 3u)
				  ? reg_ok : HRIL_REG_SEARCHING;
		} else {
			reg_state = (srv_domain == 1u || srv_domain == 3u ||
				     (srv_domain == 2u && ims_voice == 1u))
				  ? reg_ok : HRIL_REG_SEARCHING;
		}
	} else if (chosen_srv == 1u || chosen_srv == 3u) {
		reg_state = HRIL_REG_SEARCHING;
	} else {
		reg_state = HRIL_REG_NOT_REGISTERED;
	}

	if (num_out && num_sz > 0) {
		size_t cp = strlen(plmn);
		if (cp >= num_sz)
			cp = num_sz - 1;
		memcpy(num_out, plmn, cp);
		num_out[cp] = '\0';
	}
	if (name_out && name_sz > 0) {
		name_out[0] = '\0';
		if (strcmp(plmn, "46011") == 0)
			snprintf(name_out, name_sz, "China Telecom");
	}

	if (reg_out != NULL) {
		memset(reg_out, 0, sizeof(*reg_out));
		reg_out->notifyMode = 2;
		reg_out->regStatus = reg_state;
		reg_out->lac = lac;
		reg_out->cellId = cell_id;
		reg_out->actType = tech;
		reg_out->nrAvailable = (nr_srv > 0u) ? 1 : 0;
		reg_out->enDcAvailable = endc_avail ? 1 : 0;
		reg_out->dcnr = dcnr_restricted ? 1 : 0;
	}

	if (voice_out != NULL) {
		memset(voice_out, 0, sizeof(*voice_out));
		voice_out->srvStatus = (reg_state == HRIL_REG_HOME || reg_state == HRIL_REG_ROAMING) ? 2 : 0;
		voice_out->srvDomain = (int32_t)srv_domain;
		voice_out->roamStatus = (roam_status != 0u) ? 1 : 0;
		voice_out->simStatus = HRIL_SIM_READY;
		voice_out->lockStatus = 0;
		voice_out->sysMode = tech;
		voice_out->sysModeName = hril_tech_name(tech);
		voice_out->actType = tech;
		voice_out->actName = hril_tech_name(tech);
	}
	return true;
}

static bool nas_reg_equal(const HRilRegStatusInfo *a, const HRilRegStatusInfo *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

struct nas_cell_loc {
	bool has_lte;
	int32_t lte_tac;
	int32_t lte_cell_id;
	int32_t lte_earfcn;
	int32_t lte_pci;
	bool has_nr;
	int32_t nr_tac;
	int64_t nr_nci;
	int32_t nr_pci;
	int32_t nr_arfcn;
};

static bool parse_plmn_mcc_mnc(const char *plmn, int32_t *mcc_out, int32_t *mnc_out)
{
	if (!plmn || !mcc_out || !mnc_out)
		return false;
	size_t n = strlen(plmn);
	if (n < 5 || n > 6)
		return false;
	for (size_t i = 0; i < n; i++) {
		if (plmn[i] < '0' || plmn[i] > '9')
			return false;
	}
	*mcc_out = (plmn[0] - '0') * 100 + (plmn[1] - '0') * 10 + (plmn[2] - '0');
	int32_t mnc = 0;
	for (size_t i = 3; i < n; i++)
		mnc = mnc * 10 + (plmn[i] - '0');
	*mnc_out = mnc;
	return true;
}

static void parse_nas_cell_location_info(const uint8_t *tlvs, size_t len, struct nas_cell_loc *out)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	memset(out, 0, sizeof(*out));
	out->lte_tac = -1;
	out->lte_cell_id = -1;
	out->lte_earfcn = -1;
	out->lte_pci = -1;
	out->nr_tac = -1;
	out->nr_pci = -1;
	out->nr_arfcn = -1;

	/* QMI_NAS_GET_CELL_LOCATION_INFO TLV 0x13: intrafreq_lte_info_v2 */
	if (qmi_find_tlv(tlvs, len, 0x13, &v, &vl) == 0 && vl >= 14) {
		out->has_lte = true;
		out->lte_tac = (int32_t)get_le16(v + 4);
		out->lte_cell_id = (int32_t)get_le32(v + 6);
		out->lte_earfcn = (int32_t)get_le16(v + 10);
		out->lte_pci = (int32_t)get_le16(v + 12);
	}
	/* TLV 0x1c: optional 32-bit LTE EARFCN extension */
	if (qmi_find_tlv(tlvs, len, 0x1c, &v, &vl) == 0 && vl >= 4) {
		uint32_t earfcn32 = get_le32(v);
		if (earfcn32 != 0xffffffffu)
			out->lte_earfcn = (int32_t)earfcn32;
	}
	/* TLV 0x22: nr5g_cell_info (plmn[3] + tac[3] + global_cell_id[8] + pci[2]) */
	if (qmi_find_tlv(tlvs, len, 0x22, &v, &vl) == 0 && vl >= 16) {
		out->has_nr = true;
		out->nr_tac = (int32_t)(((uint32_t)v[3]) | ((uint32_t)v[4] << 8) | ((uint32_t)v[5] << 16));
		out->nr_nci = (int64_t)((uint64_t)get_le32(v + 6) | ((uint64_t)get_le32(v + 10) << 32));
		out->nr_pci = (int32_t)get_le16(v + 14);
	}
	/* TLV 0x23: optional 32-bit NR-ARFCN */
	if (qmi_find_tlv(tlvs, len, 0x23, &v, &vl) == 0 && vl >= 4) {
		uint32_t arfcn = get_le32(v);
		if (arfcn != 0xffffffffu)
			out->nr_arfcn = (int32_t)arfcn;
	}
}

static int32_t signal_pos_mag(int32_t dbm, int32_t fallback)
{
	if (dbm < 0)
		return -dbm;
	if (dbm > 0)
		return dbm;
	return fallback;
}

static bool build_current_cell_info(const HRilRegStatusInfo *reg, const char *plmn,
				    const HRilRssi *rssi, const struct nas_cell_loc *loc,
				    HRilCurrentCellInfoVendor *out)
{
	if (!reg || !out)
		return false;
	if (reg->regStatus != HRIL_REG_HOME && reg->regStatus != HRIL_REG_ROAMING)
		return false;
	int32_t mcc = 0, mnc = 0;
	if (!parse_plmn_mcc_mnc(plmn, &mcc, &mnc))
		return false;

	memset(out, 0, sizeof(*out));
	out->mcc = mcc;
	out->mnc = mnc;

	if (reg->actType == HRIL_TECH_LTE || reg->actType == HRIL_TECH_LTE_CA) {
		out->ratType = HRIL_CELL_RAT_LTE;
		out->serviceCellParas.lte.arfcn =
			(loc && loc->has_lte && loc->lte_earfcn >= 0) ? loc->lte_earfcn : 0;
		out->serviceCellParas.lte.cellId =
			(loc && loc->has_lte && loc->lte_cell_id > 0) ? loc->lte_cell_id
			: (reg->cellId >= 0 ? reg->cellId : 0);
		out->serviceCellParas.lte.pci =
			(loc && loc->has_lte && loc->lte_pci >= 0) ? loc->lte_pci : 0;
		out->serviceCellParas.lte.tac =
			(loc && loc->has_lte && loc->lte_tac >= 0) ? loc->lte_tac
			: (reg->lac >= 0 ? reg->lac : 0);
		out->serviceCellParas.lte.rsrp =
			signal_pos_mag(rssi ? rssi->vals[HRIL_RSSI_LTE_RSRP] : 0, 100);
		out->serviceCellParas.lte.rsrq =
			signal_pos_mag(rssi ? rssi->vals[HRIL_RSSI_LTE_RSRQ] : 0, 10);
		out->serviceCellParas.lte.rssi =
			signal_pos_mag(rssi ? rssi->vals[HRIL_RSSI_LTE_RXLEV] : 0, 75);
		return true;
	}
	if (reg->actType == HRIL_TECH_NR) {
		out->ratType = HRIL_CELL_RAT_NR;
		out->serviceCellParas.nr.nrArfcn =
			(loc && loc->has_nr && loc->nr_arfcn >= 0) ? loc->nr_arfcn : 0;
		out->serviceCellParas.nr.pci =
			(loc && loc->has_nr && loc->nr_pci >= 0) ? loc->nr_pci : 0;
		out->serviceCellParas.nr.tac =
			(loc && loc->has_nr && loc->nr_tac >= 0) ? loc->nr_tac
			: (reg->lac >= 0 ? reg->lac : 0);
		out->serviceCellParas.nr.nci =
			(loc && loc->has_nr && loc->nr_nci > 0) ? loc->nr_nci
			: (reg->cellId >= 0 ? (int64_t)(uint32_t)reg->cellId : 0);
		out->serviceCellParas.nr.rsrp =
			signal_pos_mag(rssi ? rssi->vals[HRIL_RSSI_NR_RSRP] : 0, 95);
		out->serviceCellParas.nr.rsrq =
			signal_pos_mag(rssi ? rssi->vals[HRIL_RSSI_NR_RSRQ] : 0, 10);
		return true;
	}
	return false;
}

static __attribute__((unused)) bool rssi_equal(const HRilRssi *a, const HRilRssi *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static bool rssi_changed_significantly(const HRilRssi *a, const HRilRssi *b)
{
	static const int idxs[] = {
		HRIL_RSSI_GSM_RXLEV,
		HRIL_RSSI_WCDMA_RXLEV,
		HRIL_RSSI_WCDMA_RSCP,
		HRIL_RSSI_LTE_RXLEV,
		HRIL_RSSI_LTE_RSRQ,
		HRIL_RSSI_LTE_RSRP,
		HRIL_RSSI_NR_RSRP,
		HRIL_RSSI_NR_RSRQ,
	};
	for (size_t i = 0; i < sizeof(idxs) / sizeof(idxs[0]); i++) {
		int32_t av = a->vals[idxs[i]];
		int32_t bv = b->vals[idxs[i]];
		if ((av == 0) != (bv == 0))
			return true;
		int64_t diff = (int64_t)av - (int64_t)bv;
		if (diff < 0)
			diff = -diff;
		if (diff >= 2)
			return true;
	}
	return false;
}

/* --- F10 pure-C parsers & builders: RF band, SMSP/WMS, VOICE, IMSDCM --- */

static int32_t qmi_dl_bw_to_khz(uint32_t bw)
{
	switch (bw) {
	case 0: return 1400;
	case 1: return 3000;
	case 2: return 5000;
	case 3: return 10000;
	case 4: return 15000;
	case 5: return 20000;
	case 6: return 5000;
	case 7: return 10000;
	case 8: return 15000;
	case 9: return 20000;
	case 10: return 25000;
	case 11: return 30000;
	case 12: return 40000;
	case 13: return 50000;
	case 14: return 60000;
	case 15: return 80000;
	case 16: return 90000;
	case 17: return 100000;
	case 18: return 200000;
	case 19: return 400000;
	case 24: return 70000;
	default: return 10000;
	}
}

static size_t parse_nas_rf_band_info(const uint8_t *tlvs, size_t len,
				     int32_t fallback_pci,
				     HRilPhyChannelConfig *out, size_t max_out)
{
	const uint8_t *v_ext = NULL, *v_bw = NULL;
	uint16_t l_ext = 0, l_bw = 0;
	if (!out || max_out == 0)
		return 0;

	/* Prefer TLV 0x11 (Extended List: u8 count + {u8 rif, u16 band, u32 chan}[]) */
	size_t n = 0;
	if (qmi_find_tlv(tlvs, len, 0x11, &v_ext, &l_ext) == 0 && l_ext >= 1) {
		uint8_t cnt = v_ext[0];
		size_t pos = 1;
		for (uint8_t i = 0; i < cnt && n < max_out && pos + 7 <= l_ext; i++, pos += 7) {
			uint8_t rif = v_ext[pos];
			uint32_t ch = get_le32(v_ext + pos + 3);
			int32_t tech = qmi_radio_if_to_hril_tech(rif);
			if (tech == HRIL_TECH_UNKNOWN)
				continue;
			memset(&out[n], 0, sizeof(out[n]));
			out[n].cellConnStatus = (n == 0) ? 1 : 2;
			out[n].cellBandwidthDownlinkKhz = (tech == HRIL_TECH_NR) ? 100000 : 20000;
			out[n].cellBandwidthUplinkKhz = out[n].cellBandwidthDownlinkKhz;
			out[n].ratType = tech;
			out[n].freqRange = (tech == HRIL_TECH_NR) ? 2 : 1;
			out[n].downlinkChannelNum = (int32_t)ch;
			out[n].uplinkChannelNum = (int32_t)ch;
			out[n].physicalCellId = fallback_pci;
			n++;
		}
	} else if (qmi_find_tlv(tlvs, len, 0x01, &v_ext, &l_ext) == 0 && l_ext >= 1) {
		uint8_t cnt = v_ext[0];
		size_t pos = 1;
		for (uint8_t i = 0; i < cnt && n < max_out && pos + 5 <= l_ext; i++, pos += 5) {
			uint8_t rif = v_ext[pos];
			uint16_t ch = get_le16(v_ext + pos + 3);
			int32_t tech = qmi_radio_if_to_hril_tech(rif);
			if (tech == HRIL_TECH_UNKNOWN)
				continue;
			memset(&out[n], 0, sizeof(out[n]));
			out[n].cellConnStatus = (n == 0) ? 1 : 2;
			out[n].cellBandwidthDownlinkKhz = (tech == HRIL_TECH_NR) ? 100000 : 20000;
			out[n].cellBandwidthUplinkKhz = out[n].cellBandwidthDownlinkKhz;
			out[n].ratType = tech;
			out[n].freqRange = (tech == HRIL_TECH_NR) ? 2 : 1;
			out[n].downlinkChannelNum = (int32_t)ch;
			out[n].uplinkChannelNum = (int32_t)ch;
			out[n].physicalCellId = fallback_pci;
			n++;
		}
	}

	if (n > 0 && qmi_find_tlv(tlvs, len, 0x12, &v_bw, &l_bw) == 0 && l_bw >= 1) {
		uint8_t bcnt = v_bw[0];
		size_t pos = 1;
		for (uint8_t i = 0; i < bcnt && i < n && pos + 5 <= l_bw; i++, pos += 5) {
			int32_t khz = qmi_dl_bw_to_khz(get_le32(v_bw + pos + 1));
			out[i].cellBandwidthDownlinkKhz = khz;
			out[i].cellBandwidthUplinkKhz = khz;
		}
	}
	return n;
}

static size_t decode_hex_bytes(const char *hex, uint8_t *out, size_t cap)
{
	if (!hex || !out || cap == 0)
		return 0;
	size_t n = 0;
	while (hex[0] && hex[1] && n < cap) {
		uint8_t hi = hex_nibble(hex[0]);
		uint8_t lo = hex_nibble(hex[1]);
		if (hi > 15u || lo > 15u)
			break;
		out[n++] = (uint8_t)((hi << 4) | lo);
		hex += 2;
	}
	return n;
}

/* Decode 3GPP TS 31.102 EF_SMSP (0x6F42) record: last 28 bytes start with param_ind,
 * followed by 12B dest_addr, then 12B TS-Service-Centre-Address ([len, ton_npi, bcd...]). */
static bool parse_smsp_record(const uint8_t *rec, size_t rec_len,
			      char *addr_out, size_t addr_cap, int32_t *tosca_out,
			      uint8_t *raw_sc_out, size_t raw_sc_cap, size_t *raw_sc_len)
{
	if (!rec || rec_len < 28 || !addr_out || addr_cap < 8)
		return false;
	const uint8_t *base = rec + (rec_len - 28);
	uint8_t param_ind = base[0];
	if ((param_ind & 0x02u) != 0u) /* bit 1 == 1 means SC address absent */
		return false;
	const uint8_t *sc = base + 13; /* 1B param_ind + 12B dest_addr */
	uint8_t sclen = sc[0];
	if (sclen < 2 || sclen > 11)
		return false;
	uint8_t ton_npi = sc[1];
	size_t pos = 0;
	if ((ton_npi & 0x70u) == 0x10u)
		addr_out[pos++] = '+';
	for (uint8_t i = 2; i <= sclen && pos + 2 < addr_cap; i++) {
		uint8_t lo = sc[i] & 0x0fu;
		uint8_t hi = (sc[i] >> 4) & 0x0fu;
		if (lo <= 9u)
			addr_out[pos++] = (char)('0' + lo);
		else
			break;
		if (hi <= 9u)
			addr_out[pos++] = (char)('0' + hi);
		else
			break;
	}
	addr_out[pos] = '\0';
	if (pos < 4)
		return false;
	if (tosca_out)
		*tosca_out = (int32_t)ton_npi;
	if (raw_sc_out && raw_sc_len && (size_t)(1 + sclen) <= raw_sc_cap) {
		memcpy(raw_sc_out, sc, (size_t)(1 + sclen));
		*raw_sc_len = (size_t)(1 + sclen);
	}
	return true;
}

/* Encode ASCII SMSC address (e.g. "+8613334262200", tosca=145) into 3GPP TS 24.011
 * binary format: [len_octets] [TON/NPI] [swapped-nibble BCD...]. */
static size_t encode_smsc_bcd(const char *addr, int32_t tosca, uint8_t *out, size_t cap)
{
	if (!addr || !out || cap < 3)
		return 0;
	bool intl = (tosca == 145);
	const char *p = addr;
	if (*p == '+') {
		intl = true;
		p++;
	}
	char digits[24];
	size_t dlen = 0;
	while (*p && dlen < sizeof(digits)) {
		if (*p >= '0' && *p <= '9')
			digits[dlen++] = *p;
		p++;
	}
	if (dlen == 0)
		return 0;
	size_t bcd_len = (dlen + 1u) / 2u;
	if (2u + bcd_len > cap || 1u + bcd_len > 15u)
		return 0;
	out[0] = (uint8_t)(1u + bcd_len);
	out[1] = intl ? 0x91u : 0x81u;
	for (size_t i = 0; i < dlen; i += 2) {
		uint8_t lo = (uint8_t)(digits[i] - '0');
		uint8_t hi = (i + 1 < dlen) ? (uint8_t)(digits[i + 1] - '0') : 0x0fu;
		out[2u + (i / 2u)] = (uint8_t)((hi << 4) | (lo & 0x0fu));
	}
	return 2u + bcd_len;
}

/* Strictly zero-PII diagnostic trace for RIL call/SMS operations (op/slot/rc/qerr only). */
static void ril_trace_op(const char *op, int32_t slot, int a0, int a1, int a2, int a3, int rc, uint16_t qerr)
{
#ifndef HRIL_VENDOR_HOST_TEST
	static const char *const paths[] = {
		"/data/service/el1/public/telephony/ril-qmi-trace.log",
		"/data/local/tmp/ril-qmi-trace.log",
	};
	for (size_t i = 0; i < 2; i++) {
		FILE *fp = fopen(paths[i], "a");
		if (!fp)
			continue;
		fprintf(fp, "RIL_QMI_OP op=%s slot=%d a0=%d a1=%d a2=%d a3=%d rc=%d qerr=0x%x\n",
			op, slot, a0, a1, a2, a3, rc, (unsigned)qerr);
		fclose(fp);
		return;
	}
#else
	(void)op; (void)slot; (void)a0; (void)a1; (void)a2; (void)a3; (void)rc; (void)qerr;
#endif
}

static size_t build_wms_raw_send_fmt_tlvs(uint8_t *buf, size_t cap,
					  uint8_t fmt,
					  const uint8_t *pdu, size_t pdu_len,
					  bool sms_on_ims)
{
	if (!buf || !pdu || pdu_len == 0 || pdu_len > 300 || cap < pdu_len + 16)
		return 0;
	uint8_t raw[310];
	raw[0] = fmt; /* 0x06=GSM/WCDMA 3GPP, 0x00=CDMA 3GPP2 */
	put_le16(raw + 1, (uint16_t)pdu_len);
	memcpy(raw + 3, pdu, pdu_len);
	size_t off = tlv_append(buf, 0x01, raw, (uint16_t)(3 + pdu_len));
	if (sms_on_ims) {
		uint8_t ims = 1u;
		off += tlv_append(buf + off, 0x13, &ims, 1);
	}
	return off;
}

static size_t build_wms_raw_send_tlvs(uint8_t *buf, size_t cap,
				      const uint8_t *pdu, size_t pdu_len,
				      bool sms_on_ims)
{
	return build_wms_raw_send_fmt_tlvs(buf, cap, 0x06u, pdu, pdu_len, sms_on_ims);
}

/* --- F12 pure-C helpers: VoLTE dial TLVs, call end reason, 3GPP2->3GPP SMS transcoder --- */

static size_t build_voice_dial_tlvs(uint8_t *buf, size_t cap,
				    const char *number, int call_type)
{
	if (!buf || !number || number[0] == '\0')
		return 0;
	size_t alen = strlen(number);
	if (alen > 80)
		alen = 80;
	if (cap < alen + 12)
		return 0;
	size_t off = tlv_append(buf, 0x01, number, (uint16_t)alen);
	if (call_type >= 0) {
		uint8_t ct = (uint8_t)(call_type & 0xff);
		off += tlv_append(buf + off, 0x10, &ct, 1);
	}
	return off;
}

/* Parse QMI_VOICE_ALL_CALL_STATUS_IND TLV 0x16 (Call End Reason Info):
 *   u8 count, { u8 call_id, u16 end_reason }[]
 * Map common Qualcomm QMI voice end reasons to 3GPP/OHOS disconnect causes. */
static int32_t parse_voice_call_end_reason(const uint8_t *tlvs, size_t len)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (qmi_find_tlv(tlvs, len, 0x16, &v, &vl) != 0 || vl < 4)
		return 16; /* NORMAL_CLEARING */
	uint8_t cnt = v[0];
	if (cnt == 0)
		return 16;
	uint16_t qcause = get_le16(v + 2);
	switch (qcause) {
	case 0x0011u: /* USER_BUSY */
	case 0x0386u: /* SIP_BUSY_HERE (486) */
		return 17;
	case 0x0012u: /* NO_USER_RESPONDING */
	case 0x0013u: /* USER_ALERTING_NO_ANSWER */
		return 19;
	case 0x0015u: /* CALL_REJECTED */
	case 0x0387u: /* SIP_REQUEST_TERMINATED */
	case 0x03d7u: /* SIP_DECLINE (603) */
		return 21;
	case 0x0001u: /* UNASSIGNED_NUMBER */
		return 1;
	case 0x0010u: /* NORMAL_CLEARING */
	case 0x001fu: /* NORMAL_UNSPECIFIED */
	default:
		return 16; /* NORMAL_CLEARING */
	}
}

/* Transcode 3GPP2 C.S0015 Point-to-Point CDMA MO SMS PDU into a 3GPP TS 23.040
 * GSM SMS-SUBMIT PDU (with SMSC prefix) via shared clean-room helper in ims_core_wire.h. */
static bool transcode_cdma_pdu_to_gsm_submit(const uint8_t *cdma, size_t cdma_len,
					     const uint8_t *smsc_raw, size_t smsc_raw_len,
					     uint8_t *out, size_t cap, size_t *out_len)
{
	return ims_transcode_cdma_pdu_to_gsm_submit(cdma, cdma_len, smsc_raw, smsc_raw_len,
						    out, cap, out_len);
}

static size_t build_wms_send_ack_tlvs(uint8_t *buf, uint32_t tx_id, bool success,
				      uint8_t rp_cause, bool sms_on_ims)
{
	uint8_t info[6];
	put_le32(info, tx_id);
	info[4] = 0x01u; /* 3GPP / WCDMA */
	info[5] = success ? 1u : 0u;
	size_t off = tlv_append(buf, 0x01, info, sizeof(info));
	if (!success) {
		uint8_t fail[2] = { rp_cause ? rp_cause : 0x6fu, 0xffu };
		off += tlv_append(buf + off, 0x11, fail, sizeof(fail));
	}
	if (sms_on_ims) {
		uint8_t ims = 1u;
		off += tlv_append(buf + off, 0x12, &ims, 1);
	}
	return off;
}

static bool parse_wms_event_report_ind(const uint8_t *tlvs, size_t len,
				       uint32_t *tx_id_out, bool *sms_on_ims_out,
				       char *hex_pdu_out, size_t hex_cap)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	bool on_ims = false;
	if (qmi_find_tlv(tlvs, len, 0x16, &v, &vl) == 0 && vl >= 1)
		on_ims = (v[0] != 0);
	if (sms_on_ims_out)
		*sms_on_ims_out = on_ims;

	if (qmi_find_tlv(tlvs, len, 0x11, &v, &vl) == 0 && vl >= 8) {
		/* TLV 0x11 Transfer Route MT Message: ack_ind(1), tx_id(4), fmt(1), raw_len(2), raw[] */
		uint32_t tx_id = get_le32(v + 1);
		uint16_t rlen = get_le16(v + 6);
		if ((size_t)(8 + rlen) <= vl && rlen > 0) {
			if (tx_id_out)
				*tx_id_out = tx_id;
			if (hex_pdu_out && hex_cap > (size_t)rlen * 2) {
				encode_hex_upper(v + 8, rlen, hex_pdu_out, hex_cap);
				return true;
			}
		}
	}
	return false;
}

static int32_t qmi_voice_state_to_hril(uint8_t qstate)
{
	switch (qstate) {
	case 0x03: return 0; /* CONVERSATION -> ACTIVE */
	case 0x06: return 1; /* HOLD -> HOLDING */
	case 0x01: /* ORIGINATION */
	case 0x04: /* CC_IN_PROGRESS */
		return 2; /* DIALING */
	case 0x05: /* ALERTING */
	case 0x0b: /* PRE_ALERTING */
		return 3; /* ALERTING */
	case 0x02: /* INCOMING */
	case 0x0a: /* SETUP */
		return 4; /* INCOMING */
	case 0x07: return 5; /* WAITING */
	default: return -1; /* END / DISCONNECTING / UNKNOWN -> omit */
	}
}

/* Parses both QMI_VOICE_GET_ALL_CALL_INFO (0x002f: info=0x10, num=0x11)
 * and QMI_VOICE_ALL_CALL_STATUS_IND (0x002e: info=0x01, num=0x10). */
static size_t parse_voice_call_list(const uint8_t *tlvs, size_t len, bool is_ind,
				    struct cached_call_entry *out, size_t max_out)
{
	const uint8_t *v_info = NULL, *v_num = NULL;
	uint16_t l_info = 0, l_num = 0;
	uint8_t info_tag = is_ind ? 0x01u : 0x10u;
	uint8_t num_tag = is_ind ? 0x10u : 0x11u;

	if (!out || max_out == 0)
		return 0;
	if (qmi_find_tlv(tlvs, len, info_tag, &v_info, &l_info) < 0 || l_info < 1)
		return 0;

	uint8_t cnt = v_info[0];
	size_t n = 0;
	size_t pos = 1;
	for (uint8_t i = 0; i < cnt && n < max_out && pos + 7 <= l_info; i++, pos += 7) {
		uint8_t cid = v_info[pos + 0];
		int32_t hstate = qmi_voice_state_to_hril(v_info[pos + 1]);
		if (hstate < 0)
			continue;
		uint8_t ctype = v_info[pos + 2];
		uint8_t qdir = v_info[pos + 3];
		uint8_t mpty = v_info[pos + 5];
		memset(&out[n], 0, sizeof(out[n]));
		out[n].index = (int32_t)cid;
		out[n].dir = (qdir == 2u) ? 1 : 0; /* 2=MT->1, 1=MO->0 */
		out[n].state = hstate;
		out[n].mode = 0; /* voice */
		out[n].mpty = mpty ? 1 : 0;
		out[n].type = 129;
		out[n].call_type = ctype;
		n++;
	}

	if (n > 0 && qmi_find_tlv(tlvs, len, num_tag, &v_num, &l_num) == 0 && l_num >= 1) {
		uint8_t ncnt = v_num[0];
		size_t np = 1;
		for (uint8_t i = 0; i < ncnt && np + 3 <= l_num; i++) {
			uint8_t cid = v_num[np + 0];
			uint8_t nlen = v_num[np + 2];
			np += 3;
			if (np + nlen > l_num)
				break;
			for (size_t j = 0; j < n; j++) {
				if (out[j].index == (int32_t)cid) {
					size_t cp = nlen < sizeof(out[j].number) - 1 ? nlen : sizeof(out[j].number) - 1;
					memcpy(out[j].number, v_num + np, cp);
					out[j].number[cp] = '\0';
					out[j].type = (out[j].number[0] == '+') ? 145 : 129;
					break;
				}
			}
			np += nlen;
		}
	}
	return n;
}

static size_t build_imsdcm_start_resp(uint8_t *pkt, uint16_t tx, uint8_t local_id,
				      uint32_t seq, uint32_t inst_id)
{
	size_t o = 7;
	uint8_t ok4[4] = { 0, 0, 0, 0 };
	uint8_t b4[4];
	o += tlv_append(pkt + o, 0x02, ok4, 4);
	o += tlv_append(pkt + o, 0x10, &local_id, 1);
	put_le32(b4, seq);
	o += tlv_append(pkt + o, 0x11, b4, 4);
	put_le32(b4, inst_id);
	o += tlv_append(pkt + o, 0x12, b4, 4);
	pkt[0] = 0x02;
	put_le16(pkt + 1, tx);
	put_le16(pkt + 3, QMI_IMSDCM_START_CONN);
	put_le16(pkt + 5, (uint16_t)(o - 7));
	return o;
}

static size_t build_imsdcm_conn_ind(uint8_t *pkt, uint16_t tx, uint8_t local_id,
				    uint32_t seq, uint32_t inst_id,
				    uint32_t family, const char *ip_str)
{
	size_t o = 7;
	uint8_t res4[4] = { 0, 0, 0, 0 };
	uint8_t b4[4];
	bool ok = (ip_str != NULL && ip_str[0] != '\0');
	if (!ok) {
		res4[0] = 0u;
		res4[2] = 13u; /* 81voltd: err_status=0, err_code=13 on disconnect/failure */
	}
	o += tlv_append(pkt + o, 0x02, res4, 4);
	o += tlv_append(pkt + o, 0x01, &local_id, 1);
	put_le32(b4, seq);
	o += tlv_append(pkt + o, 0x10, b4, 4);
	if (ok) {
		uint8_t abuf[128];
		size_t alen = strlen(ip_str);
		if (alen > 120)
			alen = 120;
		put_le32(abuf, family);
		abuf[4] = (uint8_t)alen;
		memcpy(abuf + 5, ip_str, alen);
		o += tlv_append(pkt + o, 0x11, abuf, (uint16_t)(5 + alen));
	}
	put_le32(b4, inst_id);
	o += tlv_append(pkt + o, 0x12, b4, 4);
	pkt[0] = 0x04;
	put_le16(pkt + 1, tx);
	put_le16(pkt + 3, QMI_IMSDCM_START_CONN);
	put_le16(pkt + 5, (uint16_t)(o - 7));
	return o;
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

static void format_ipv4_u32(uint32_t ip, char *buf, size_t cap)
{
	snprintf(buf, cap, "%u.%u.%u.%u",
		 (ip >> 24) & 0xffu, (ip >> 16) & 0xffu,
		 (ip >> 8) & 0xffu, ip & 0xffu);
}

static size_t build_wds_bind_sub_tlvs(uint8_t *buf, uint32_t sub_id)
{
	uint8_t sbuf[4];
	put_le32(sbuf, sub_id);
	return tlv_append(buf, 0x01, sbuf, sizeof(sbuf));
}

static size_t build_dpm_open_port_tlvs(uint8_t *buf, uint32_t ep_type, uint32_t iface_id,
				       uint32_t rx_ep, uint32_t tx_ep)
{
	uint8_t p[17];
	p[0] = 1u;
	put_le32(p + 1, ep_type);
	put_le32(p + 5, iface_id);
	put_le32(p + 9, rx_ep);
	put_le32(p + 13, tx_ep);
	return tlv_append(buf, 0x11, p, sizeof(p));
}

static size_t build_wda_set_data_fmt_tlvs(uint8_t *buf, uint32_t link_prot,
					  uint32_t ul_prot, uint32_t dl_prot,
					  uint32_t ep_type, uint32_t iface_id)
{
	uint8_t b4[4], ep_buf[8];
	size_t off = 0;
	put_le32(b4, link_prot);
	off += tlv_append(buf + off, 0x11, b4, 4);
	put_le32(b4, ul_prot);
	off += tlv_append(buf + off, 0x12, b4, 4);
	put_le32(b4, dl_prot);
	off += tlv_append(buf + off, 0x13, b4, 4);
	put_le32(b4, 16u);
	off += tlv_append(buf + off, 0x15, b4, 4);
	put_le32(b4, 8192u);
	off += tlv_append(buf + off, 0x16, b4, 4);
	put_le32(ep_buf, ep_type);
	put_le32(ep_buf + 4, iface_id);
	off += tlv_append(buf + off, 0x17, ep_buf, sizeof(ep_buf));
	return off;
}

static size_t build_wds_bind_mux_tlvs(uint8_t *buf, uint32_t ep_type, uint32_t iface_id,
				      uint8_t mux_id, int client_type)
{
	uint8_t ep_buf[8];
	put_le32(ep_buf, ep_type);
	put_le32(ep_buf + 4, iface_id);
	size_t off = tlv_append(buf, 0x10, ep_buf, sizeof(ep_buf));
	off += tlv_append(buf + off, 0x11, &mux_id, 1);
	if (client_type >= 0) {
		uint8_t cbuf[4];
		put_le32(cbuf, (uint32_t)client_type);
		off += tlv_append(buf + off, 0x13, cbuf, sizeof(cbuf));
	}
	return off;
}

static size_t build_wds_start_tlvs(uint8_t *buf, const char *apn, uint8_t ip_pref)
{
	size_t off = 0;
	if (apn != NULL && apn[0] != '\0') {
		size_t alen = strlen(apn);
		if (alen > 100)
			alen = 100;
		off += tlv_append(buf + off, 0x14, (const uint8_t *)apn, (uint16_t)alen);
	}
	off += tlv_append(buf + off, 0x19, &ip_pref, 1);
	return off;
}

static size_t build_wds_stop_tlvs(uint8_t *buf, uint32_t handle)
{
	uint8_t hbuf[4];
	put_le32(hbuf, handle);
	return tlv_append(buf, 0x01, hbuf, sizeof(hbuf));
}

static size_t build_wds_curr_settings_tlvs(uint8_t *buf, uint32_t mask)
{
	uint8_t mbuf[4];
	put_le32(mbuf, mask);
	return tlv_append(buf, 0x10, mbuf, sizeof(mbuf));
}

static bool parse_wds_current_settings(const uint8_t *tlvs, size_t len, struct pdp_session *out)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	uint32_t ip_val = 0;
	int pfx = 32;

	out->ip[0] = '\0';
	out->ip_prefix[0] = '\0';
	out->gw[0] = '\0';
	out->dns1[0] = '\0';
	out->dns2[0] = '\0';
	out->mtu = 1500;

	if (qmi_find_tlv(tlvs, len, 0x1e, &v, &vl) == 0 && vl >= 4) {
		ip_val = get_le32(v);
		if (ip_val != 0u)
			format_ipv4_u32(ip_val, out->ip, sizeof(out->ip));
	}
	if (qmi_find_tlv(tlvs, len, 0x20, &v, &vl) == 0 && vl >= 4) {
		uint32_t gw = get_le32(v);
		if (gw != 0u)
			format_ipv4_u32(gw, out->gw, sizeof(out->gw));
	}
	if (qmi_find_tlv(tlvs, len, 0x21, &v, &vl) == 0 && vl >= 4)
		pfx = ipv4_mask_to_prefix(get_le32(v));
	if (qmi_find_tlv(tlvs, len, 0x15, &v, &vl) == 0 && vl >= 4) {
		uint32_t d1 = get_le32(v);
		if (d1 != 0u)
			format_ipv4_u32(d1, out->dns1, sizeof(out->dns1));
	}
	if (qmi_find_tlv(tlvs, len, 0x16, &v, &vl) == 0 && vl >= 4) {
		uint32_t d2 = get_le32(v);
		if (d2 != 0u)
			format_ipv4_u32(d2, out->dns2, sizeof(out->dns2));
	}
	if (qmi_find_tlv(tlvs, len, 0x29, &v, &vl) == 0 && vl >= 4) {
		uint32_t mtu = get_le32(v);
		if (mtu >= 1280u && mtu <= 9216u)
			out->mtu = (int32_t)mtu;
	}
	if (out->ip[0] == '\0')
		return false;
	snprintf(out->ip_prefix, sizeof(out->ip_prefix), "%s/%d", out->ip, pfx);
	return true;
}

static void fill_pdp_response(const struct pdp_session *p, HRilDataCallResponse *out)
{
	memset(out, 0, sizeof(*out));
	out->reason = 0;
	out->retryTime = -1;
	out->cid = p->cid;
	out->active = p->active ? 2 : 0;
	out->type = p->pdp_type[0] ? p->pdp_type : "IP";
	out->netPortName = MARBLE_DATA_IFNAME;
	out->address = p->ip_prefix;
	out->dns = p->dns1;
	out->dnsSec = p->dns2;
	out->gateway = p->gw;
	out->maxTransferUnit = p->mtu > 0 ? p->mtu : 1500;
	out->pCscfPrimAddr = "";
	out->pCscfSecAddr = "";
	out->pduSessionId = 0;
}

static bool ascii_ieq(const char *a, const char *b)
{
	if (!a || !b)
		return false;
	while (*a && *b) {
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (ca >= 'A' && ca <= 'Z')
			ca = (unsigned char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z')
			cb = (unsigned char)(cb - 'A' + 'a');
		if (ca != cb)
			return false;
	}
	return *a == '\0' && *b == '\0';
}

static bool is_secondary_non_internet_apn(const char *apn)
{
	return ascii_ieq(apn, "ims") || ascii_ieq(apn, "xcap") || ascii_ieq(apn, "emergency");
}

struct uim_card_info {
	bool slot_present[2];
	int32_t sim_state[2];
	int32_t sim_type[2];
	int32_t pin1_state[2];
	uint8_t prov_sess[2];
	uint8_t aid[2][32];
	uint8_t aid_len[2];
	int active_slot;
};

static void parse_uim_card_status(const uint8_t *tlvs, size_t len, struct uim_card_info *ci)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;

	memset(ci, 0, sizeof(*ci));
	ci->active_slot = -1;
	ci->prov_sess[0] = 0;
	ci->prov_sess[1] = 2;
	if (qmi_find_tlv(tlvs, len, 0x10, &v, &vl) < 0 || vl < 9)
		return;

	uint16_t gw_pri = get_le16(v + 0);
	uint16_t gw_sec = get_le16(v + 4);
	uint8_t num_cards = v[8];
	size_t pos = 9;

	for (uint8_t slot = 0; slot < num_cards && vl - pos >= 6; slot++) {
		uint8_t cstate = v[pos + 0];
		uint8_t napps = v[pos + 5];
		pos += 6;

		bool present = (cstate == 1);
		int32_t st = present ? HRIL_SIM_NOT_READY : HRIL_SIM_NOT_INSERTED;
		int32_t ty = HRIL_SIM_TYPE_UNKNOWN;
		int32_t pin1 = 0;
		uint8_t found_aid[32] = {0};
		uint8_t found_alen = 0;

		for (uint8_t ai = 0; ai < napps && vl - pos >= 7; ai++) {
			uint8_t atype = v[pos + 0];
			uint8_t astate = v[pos + 1];
			uint8_t alen = v[pos + 6];
			pos += 7;
			if (vl - pos < (size_t)alen + 7) {
				pos = vl;
				break;
			}
			uint8_t p1 = v[pos + alen + 1];
			if ((atype == 1 || atype == 2) && found_alen == 0) {
				ty = (atype == 2) ? HRIL_SIM_TYPE_USIM : HRIL_SIM_TYPE_SIM;
				pin1 = p1;
				if (astate == 7)
					st = HRIL_SIM_READY;
				else if (astate == 3 || p1 == 2)
					st = HRIL_SIM_PIN;
				else if (astate == 4 || p1 == 4)
					st = HRIL_SIM_PUK;
				if (alen <= sizeof(found_aid)) {
					memcpy(found_aid, v + pos, alen);
					found_alen = alen;
				}
			}
			pos += (size_t)alen + 7;
		}

		if (slot < 2) {
			ci->slot_present[slot] = present;
			ci->sim_state[slot] = st;
			ci->sim_type[slot] = ty;
			ci->pin1_state[slot] = pin1;
			if (found_alen > 0) {
				memcpy(ci->aid[slot], found_aid, found_alen);
				ci->aid_len[slot] = found_alen;
			}
			if (gw_pri != 0xffffu && (gw_pri & 0xffu) == slot)
				ci->prov_sess[slot] = 0;
			else if (gw_sec != 0xffffu && (gw_sec & 0xffu) == slot)
				ci->prov_sess[slot] = 2;
			else
				ci->prov_sess[slot] = (uint8_t)(4 + slot);
			if (present && ci->active_slot < 0)
				ci->active_slot = slot;
		}
	}
}

static size_t build_uim_read_tlvs(uint8_t *tlvs, uint8_t sess_type,
				  const uint8_t *aid, uint8_t aid_len,
				  uint16_t file_id, const uint8_t *path, uint8_t path_len,
				  uint16_t p1, uint16_t p2, bool include_read_param)
{
	uint8_t sbuf[64], fbuf[32], rbuf[4];
	size_t pos = 0;
	if (aid_len > sizeof(sbuf) - 2)
		aid_len = sizeof(sbuf) - 2;
	sbuf[0] = sess_type;
	sbuf[1] = aid_len;
	if (aid_len > 0 && aid != NULL)
		memcpy(sbuf + 2, aid, aid_len);
	pos += tlv_append(tlvs + pos, 0x01, sbuf, (uint16_t)(2 + aid_len));

	put_le16(fbuf, file_id);
	if (path_len > 28)
		path_len = 28;
	fbuf[2] = path_len;
	if (path_len > 0 && path != NULL)
		memcpy(fbuf + 3, path, path_len);
	pos += tlv_append(tlvs + pos, 0x02, fbuf, (uint16_t)(3 + path_len));

	if (include_read_param) {
		put_le16(rbuf, p1);
		put_le16(rbuf + 2, p2);
		pos += tlv_append(tlvs + pos, 0x03, rbuf, 4);
	}
	return pos;
}

static void synthesize_gsm_fcp_15(uint16_t fsize, uint16_t fid, uint8_t ftype,
				  uint16_t rec_size, uint8_t out[15])
{
	memset(out, 0, 15);
	out[2] = (uint8_t)((fsize >> 8) & 0xffu);
	out[3] = (uint8_t)(fsize & 0xffu);
	out[4] = (uint8_t)((fid >> 8) & 0xffu);
	out[5] = (uint8_t)(fid & 0xffu);
	out[6] = 0x04; /* EF */
	out[8] = 0x11;
	out[9] = 0xff;
	out[10] = 0x22;
	out[11] = 0x01;
	out[12] = 0x02;
	out[13] = (ftype == 1 || ftype == 2) ? ftype : 0x00; /* 0=transparent, 1=linear fixed, 2=cyclic */
	out[14] = (uint8_t)(rec_size & 0xffu);
}

/* --- Report dispatch helpers --- */

static void emit_report(HRilReportFn fn, int32_t slot_id, const ReqDataInfo *req,
			int32_t notify_id, int32_t type, int32_t err,
			const void *data, size_t data_len)
{
	if (fn == NULL)
		return;
	ReportInfo ri;
	memset(&ri, 0, sizeof(ri));
	ri.requestInfo = req;
	ri.notifyMessage = notify_id;
	ri.type = type;
	ri.error = err;
	ri.flag = 0;
	fn(slot_id, &ri, (const uint8_t *)data, data_len);
}

static int uim_resolve_slot_session_locked(int32_t slot_id, struct uim_card_info *ci,
					   uint8_t *card_sess, uint8_t *prov_sess,
					   const uint8_t **aid, uint8_t *aid_len)
{
	uint8_t resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t qerr = 0;
	memset(ci, 0, sizeof(*ci));
	ci->active_slot = -1;
	if (qmi_rpc_locked(SVC_UIM, QMI_UIM_GET_CARD_STATUS, NULL, 0,
			   resp, sizeof(resp), &rl, &qerr) != 0 || qerr != 0)
		return -ENODEV;
	parse_uim_card_status(resp, rl, ci);
	if (slot_id < 0 || slot_id > 1 || !ci->slot_present[slot_id])
		return -ENOENT;
	*card_sess = (uint8_t)(6 + slot_id);
	*prov_sess = ci->prov_sess[slot_id];
	*aid = ci->aid[slot_id];
	*aid_len = (*prov_sess >= 4) ? ci->aid_len[slot_id] : 0;
	return 0;
}

static int32_t resolve_active_slot_locked(void)
{
	if (g_active_slot >= 0)
		return g_active_slot;
	uint8_t resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t qerr = 0;
	struct uim_card_info ci;
	memset(&ci, 0, sizeof(ci));
	ci.active_slot = -1;
	if (qmi_rpc_locked(SVC_UIM, QMI_UIM_GET_CARD_STATUS, NULL, 0,
			   resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
		parse_uim_card_status(resp, rl, &ci);
		g_slot_present_cached[0] = ci.slot_present[0];
		g_slot_present_cached[1] = ci.slot_present[1];
		g_slot_present_known = true;
		if (ci.active_slot >= 0) {
			g_active_slot = ci.active_slot;
			if (!g_primary_slot_explicit)
				g_primary_slot = ci.active_slot;
			return g_active_slot;
		}
	}
	return 0;
}

static int uim_read_ef_bytes_locked(int32_t slot_id, uint16_t msg_id,
				    uint16_t file_id, const uint8_t *path, uint8_t path_len,
				    uint16_t p1, uint16_t p2,
				    uint8_t *out, size_t out_cap, size_t *out_len)
{
	struct uim_card_info ci;
	uint8_t card_sess = 6, prov_sess = 0, aid_len = 0;
	const uint8_t *aid = NULL;
	if (uim_resolve_slot_session_locked(slot_id, &ci, &card_sess, &prov_sess, &aid, &aid_len) < 0)
		return -ENOENT;

	uint8_t tlvs[128], resp[QMI_MAX_BUF];
	size_t tlen, rl = 0;
	uint16_t qerr = 0;
	uint8_t sess = (file_id == 0x2fe2u) ? card_sess : prov_sess;
	uint8_t use_alen = (file_id == 0x2fe2u) ? 0 : aid_len;
	const uint8_t *use_aid = (use_alen > 0) ? aid : NULL;

	tlen = build_uim_read_tlvs(tlvs, sess, use_aid, use_alen, file_id, path, path_len, p1, p2, true);
	int rc = qmi_rpc_locked(SVC_UIM, msg_id, tlvs, tlen, resp, sizeof(resp), &rl, &qerr);
	if ((rc != 0 || qerr != 0) && ci.aid_len[slot_id] > 0) {
		uint8_t np_sess = (uint8_t)(4 + slot_id);
		tlen = build_uim_read_tlvs(tlvs, np_sess, ci.aid[slot_id], ci.aid_len[slot_id],
					   file_id, path, path_len, p1, p2, true);
		rc = qmi_rpc_locked(SVC_UIM, msg_id, tlvs, tlen, resp, sizeof(resp), &rl, &qerr);
	}
	if (rc != 0 || qerr != 0)
		return -EIO;

	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (qmi_find_tlv(resp, rl, 0x11, &v, &vl) < 0 || vl < 2)
		return -EPROTO;
	uint16_t clen = get_le16(v);
	if ((size_t)clen + 2 > vl || clen > out_cap)
		return -EPROTO;
	memcpy(out, v + 2, clen);
	*out_len = clen;
	return 0;
}

static void nas_ensure_dsb_locked(int32_t active_slot)
{
	if (g_dsb_activated)
		return;
	uint8_t resp[QMI_MAX_BUF];
	size_t rl = 0;
	uint16_t qerr = 0;
	if (qmi_rpc_locked(SVC_NAS, QMI_NAS_GET_DUAL_STANDBY_PREF, NULL, 0,
			   resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
		const uint8_t *v_mask = NULL, *v_data = NULL;
		uint16_t l_mask = 0, l_data = 0;
		uint64_t mask = 0;
		if (qmi_find_tlv(resp, rl, 0x15, &v_mask, &l_mask) == 0 && l_mask >= 8) {
			mask = (uint64_t)get_le32(v_mask) | ((uint64_t)get_le32(v_mask + 4) << 32);
		}
		if (qmi_find_tlv(resp, rl, 0x13, &v_data, &l_data) == 0 && l_data >= 1 &&
		    (mask & (1ULL << active_slot)) != 0 && v_data[0] == (uint8_t)active_slot) {
			g_dsb_activated = true;
			return;
		}
	}
	uint8_t pri = (active_slot == 1) ? 1u : 0u;
	uint8_t tlvs[32];
	size_t tlen = build_nas_set_dsb_tlvs(tlvs, 0x05u, pri, pri, pri, 0x03ULL);
	rl = 0;
	qerr = 0;
	if (qmi_rpc_locked(SVC_NAS, QMI_NAS_SET_DUAL_STANDBY_PREF, tlvs, tlen,
			   resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
		g_dsb_activated = true;
	}
}

static void wds_stop_locked(void)
{
	if (g_pdp.wds_sock >= 0) {
		if (g_pdp.active && g_pdp.handle != 0 && qrtr_discover_locked() == 0 && g_qrtr.wds.valid) {
			uint8_t sreq[16], sresp[QMI_MAX_BUF];
			size_t sl = build_wds_stop_tlvs(sreq, g_pdp.handle);
			size_t rl = 0;
			uint16_t qerr = 0;
			(void)qmi_rpc_on_sock_locked(g_pdp.wds_sock, SVC_WDS,
						     QMI_WDS_STOP_NETWORK_INTERFACE,
						     sreq, sl, sresp, sizeof(sresp), &rl, &qerr);
		}
		close(g_pdp.wds_sock);
		g_pdp.wds_sock = -1;
	}
	g_pdp.active = false;
	g_pdp.handle = 0;
	g_pdp.slot_id = -1;
	g_pdp.ip[0] = '\0';
	g_pdp.ip_prefix[0] = '\0';
	g_pdp.gw[0] = '\0';
	g_pdp.dns1[0] = '\0';
	g_pdp.dns2[0] = '\0';
}

/* --- Worker execution --- */

static void sync_slot_radio_defaults_locked(bool modem_online)
{
	(void)resolve_active_slot_locked();
	if (!g_slot_radio_inited) {
		for (int s = 0; s < 2; s++) {
			bool present = g_slot_present_known && g_slot_present_cached[s];
			g_slot_radio_state[s] = (modem_online && present)
					      ? HRIL_RADIO_POWER_ON : HRIL_RADIO_POWER_OFF;
		}
		g_slot_radio_inited = true;
	}
}

static void handle_task(const struct ril_task *t)
{
	int32_t slot = t->slot_id;
	const ReqDataInfo *req = t->req;
	const HRilReport *rep = g_report_ops;
	if (rep == NULL)
		return;

	pthread_mutex_lock(&g_state_lock);

	switch (t->kind) {
	case TASK_MODEM_SET_RADIO_STATE: {
		int32_t s = (slot == 0 || slot == 1) ? slot : 0;
		int32_t other = 1 - s;
		int32_t req_state = (t->arg0 != 0) ? HRIL_RADIO_POWER_ON : HRIL_RADIO_POWER_OFF;
		if (!g_slot_radio_inited) {
			int32_t act = resolve_active_slot_locked();
			for (int i = 0; i < 2; i++) {
				bool pres = g_slot_present_known ? g_slot_present_cached[i] : (i == act);
				g_slot_radio_state[i] = pres ? HRIL_RADIO_POWER_ON : HRIL_RADIO_POWER_OFF;
			}
			g_slot_radio_inited = true;
		}
		g_slot_radio_state[s] = req_state;

		int32_t err = HRIL_ERR_SUCCESS;
		if (t->arg0 != 0) {
			uint8_t mode = 0; /* 1->online(0) */
			uint8_t tlvs[8], resp[QMI_MAX_BUF];
			size_t tlen = tlv_append(tlvs, 0x01, &mode, 1);
			size_t rl = 0;
			uint16_t qerr = 0;
			int rc = qmi_rpc_locked(SVC_DMS, QMI_DMS_SET_OP_MODE, tlvs, tlen,
						resp, sizeof(resp), &rl, &qerr);
			err = (rc == 0 && qerr == 0) ? HRIL_ERR_SUCCESS : HRIL_ERR_GENERIC_FAILURE;
			if (err == HRIL_ERR_SUCCESS) {
				g_radio_on_notified = true;
				nas_ensure_dsb_locked(resolve_active_slot_locked());
			}
		} else {
			bool other_on = (g_slot_radio_state[other] == HRIL_RADIO_POWER_ON);
			if (!other_on) {
				uint8_t mode = 1; /* 0->lpm(1) only when both slots off */
				uint8_t tlvs[8], resp[QMI_MAX_BUF];
				size_t tlen = tlv_append(tlvs, 0x01, &mode, 1);
				size_t rl = 0;
				uint16_t qerr = 0;
				int rc = qmi_rpc_locked(SVC_DMS, QMI_DMS_SET_OP_MODE, tlvs, tlen,
							resp, sizeof(resp), &rl, &qerr);
				err = (rc == 0 && qerr == 0) ? HRIL_ERR_SUCCESS : HRIL_ERR_GENERIC_FAILURE;
				if (err == HRIL_ERR_SUCCESS)
					g_radio_on_notified = false;
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnModemReport, slot, req, 0, HRIL_REPORT_RESPONSE, err, NULL, 0);
		if (err == HRIL_ERR_SUCCESS) {
			emit_report(rep->OnModemReport, slot, NULL, HNOTI_MODEM_RADIO_STATE_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, &req_state, sizeof(req_state));
		}
		return;
	}
	case TASK_MODEM_GET_RADIO_STATE: {
		uint8_t resp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		int32_t rstate = HRIL_RADIO_POWER_OFF;
		int rc = qmi_rpc_locked(SVC_DMS, QMI_DMS_GET_OP_MODE, NULL, 0, resp, sizeof(resp), &rl, &qerr);
		if (rc == 0 && qerr == 0 && qmi_find_tlv(resp, rl, 0x01, &v, &vl) == 0 && vl >= 1) {
			if (v[0] == 0) {
				sync_slot_radio_defaults_locked(true);
				rstate = (slot == 0 || slot == 1) ? g_slot_radio_state[slot] : HRIL_RADIO_POWER_ON;
			} else {
				rstate = HRIL_RADIO_POWER_OFF;
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnModemReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    (rc == 0 && qerr == 0) ? HRIL_ERR_SUCCESS : HRIL_ERR_MODEM_DEVICE_CLOSE,
			    &rstate, sizeof(rstate));
		return;
	}
	case TASK_MODEM_GET_IMEI:
	case TASK_MODEM_GET_IMEISV:
	case TASK_MODEM_GET_MEID: {
		uint8_t resp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		char buf[32] = {0};
		uint8_t tlv_id = (t->kind == TASK_MODEM_GET_IMEI) ? 0x11u
			       : (t->kind == TASK_MODEM_GET_IMEISV) ? 0x14u : 0x12u;
		int rc = qmi_rpc_locked(SVC_DMS, QMI_DMS_GET_DEVICE_SERIAL_NUMS, NULL, 0,
					resp, sizeof(resp), &rl, &qerr);
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		int32_t err = HRIL_ERR_GENERIC_FAILURE;
		if (rc == 0 && qerr == 0 && qmi_find_tlv(resp, rl, tlv_id, &v, &vl) == 0 && vl > 0) {
			size_t cp = vl < sizeof(buf) - 1 ? vl : sizeof(buf) - 1;
			if (t->kind == TASK_MODEM_GET_IMEI && cp > 15)
				cp = 15;
			memcpy(buf, v, cp);
			buf[cp] = '\0';
			err = HRIL_ERR_SUCCESS;
		} else if (t->kind == TASK_MODEM_GET_IMEISV && rc == 0 && qerr == 0) {
			strcpy(buf, "00");
			err = HRIL_ERR_SUCCESS;
		} else if (t->kind == TASK_MODEM_GET_MEID && rc == 0 && qerr == 0) {
			strcpy(buf, "00000000000000");
			err = HRIL_ERR_SUCCESS;
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnModemReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    err, err == HRIL_ERR_SUCCESS ? buf : NULL,
			    err == HRIL_ERR_SUCCESS ? strlen(buf) : 0);
		memset(buf, 0, sizeof(buf));
		return;
	}
	case TASK_MODEM_GET_VOICE_TECH: {
		uint8_t resp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		HRilVoiceRadioInfo info;
		memset(&info, 0, sizeof(info));
		info.sysModeName = "Unknown";
		info.actName = "Unknown";
		int32_t act = resolve_active_slot_locked();
		nas_ensure_dsb_locked(act);
		int32_t qslot = (slot == 0 || slot == 1) ? slot : act;
		if (!g_slot_present_known || g_slot_present_cached[qslot]) {
			int rc = qmi_nas_rpc_slot_locked(qslot, QMI_NAS_GET_SYS_INFO, NULL, 0,
							 resp, sizeof(resp), &rl, &qerr);
			bool ok = (rc == 0 && qerr == 0 &&
				   parse_nas_sys_info(resp, rl, false, NULL, &info, NULL, 0, NULL, 0));
			if (!ok) {
				rc = qmi_nas_rpc_slot_locked(qslot, QMI_NAS_GET_SERVING_SYSTEM, NULL, 0,
							     resp, sizeof(resp), &rl, &qerr);
				if (rc == 0 && qerr == 0)
					parse_nas_serving_info(resp, rl, false, NULL, &info, NULL, 0, NULL, 0);
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnModemReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &info, sizeof(info));
		return;
	}
	case TASK_MODEM_GET_BASEBAND: {
		uint8_t resp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		char rev[128] = "marble-qmi";
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		if (qmi_rpc_locked(SVC_DMS, QMI_DMS_GET_DEVICE_REV_ID, NULL, 0,
				   resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
			if ((qmi_find_tlv(resp, rl, 0x01, &v, &vl) == 0 ||
			     qmi_find_tlv(resp, rl, 0x10, &v, &vl) == 0) && vl > 0) {
				size_t cp = vl < sizeof(rev) - 1 ? vl : sizeof(rev) - 1;
				memcpy(rev, v, cp);
				rev[cp] = '\0';
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnModemReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, rev, strlen(rev));
		return;
	}
	case TASK_MODEM_SHUTDOWN: {
		uint8_t mode = 1;
		uint8_t tlvs[8], resp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		size_t tlen = tlv_append(tlvs, 0x01, &mode, 1);
		(void)qmi_rpc_locked(SVC_DMS, QMI_DMS_SET_OP_MODE, tlvs, tlen, resp, sizeof(resp), &rl, &qerr);
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnModemReport, slot, req, 0, HRIL_REPORT_RESPONSE, HRIL_ERR_SUCCESS, NULL, 0);
		return;
	}
	case TASK_SIM_GET_STATUS: {
		struct uim_card_info ci;
		uint8_t resp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		char iccid[32] = "";
		HRilCardState cs;
		memset(&cs, 0, sizeof(cs));
		cs.index = slot;
		cs.simType = HRIL_SIM_TYPE_UNKNOWN;
		cs.simState = HRIL_SIM_NOT_INSERTED;
		cs.iccid = iccid;

		if (qmi_rpc_locked(SVC_UIM, QMI_UIM_GET_CARD_STATUS, NULL, 0,
				   resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
			parse_uim_card_status(resp, rl, &ci);
			if (slot >= 0 && slot < 2 && ci.slot_present[slot]) {
				cs.simType = ci.sim_type[slot];
				cs.simState = ci.sim_state[slot];
				static const uint8_t path_mf[2] = { 0x00, 0x3f };
				uint8_t ef[16];
				size_t efl = 0;
				if (uim_read_ef_bytes_locked(slot, QMI_UIM_READ_TRANSPARENT,
							     0x2fe2u, path_mf, 2, 0, 10,
							     ef, sizeof(ef), &efl) == 0) {
					(void)decode_iccid_bcd(ef, efl, iccid, sizeof(iccid));
					memset(ef, 0, sizeof(ef));
				}
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &cs, sizeof(cs));
		memset(iccid, 0, sizeof(iccid));
		return;
	}
	case TASK_SIM_GET_IO: {
		HRilSimIOResponse io_resp = { .sw1 = 0x6au, .sw2 = 0x82u, .response = "" };
		char hex[640] = "";
		uint8_t path[16];
		uint8_t plen = 0;
		parse_sim_path_le(t->sim_path, (uint16_t)t->sim_io.fileid, path, &plen);

		if (t->sim_io.command == SIM_CMD_READ_BINARY || t->sim_io.command == SIM_CMD_READ_RECORD) {
			uint8_t ef[260];
			size_t efl = 0;
			uint16_t msg = (t->sim_io.command == SIM_CMD_READ_BINARY)
				     ? QMI_UIM_READ_TRANSPARENT : QMI_UIM_READ_RECORD;
			uint16_t p1 = (t->sim_io.command == SIM_CMD_READ_BINARY)
				    ? (uint16_t)(((uint16_t)(t->sim_io.p1 & 0xff) << 8) | (uint16_t)(t->sim_io.p2 & 0xff))
				    : (uint16_t)t->sim_io.p1;
			uint16_t p2 = (uint16_t)(t->sim_io.p3 > 0 ? t->sim_io.p3 : 0);
			if (uim_read_ef_bytes_locked(slot, msg, (uint16_t)t->sim_io.fileid,
						     path, plen, p1, p2, ef, sizeof(ef), &efl) == 0) {
				encode_hex_upper(ef, efl, hex, sizeof(hex));
				io_resp.sw1 = 0x90;
				io_resp.sw2 = 0x00;
				io_resp.response = hex;
				memset(ef, 0, sizeof(ef));
			}
		} else if (t->sim_io.command == SIM_CMD_GET_RESPONSE) {
			struct uim_card_info ci;
			uint8_t card_sess = 6, prov_sess = 0, aid_len = 0;
			const uint8_t *aid = NULL;
			if (uim_resolve_slot_session_locked(slot, &ci, &card_sess, &prov_sess, &aid, &aid_len) == 0) {
				uint8_t tlvs[128], resp[QMI_MAX_BUF];
				size_t rl = 0;
				uint16_t qerr = 0;
				uint8_t sess = (t->sim_io.fileid == 0x2fe2) ? card_sess : prov_sess;
				uint8_t use_alen = (t->sim_io.fileid == 0x2fe2) ? 0 : aid_len;
				size_t tlen = build_uim_read_tlvs(tlvs, sess, use_alen ? aid : NULL, use_alen,
								  (uint16_t)t->sim_io.fileid, path, plen, 0, 0, false);
				if (qmi_rpc_locked(SVC_UIM, QMI_UIM_GET_FILE_ATTRIBUTES, tlvs, tlen,
						   resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
					const uint8_t *v = NULL;
					uint16_t vl = 0;
					if (qmi_find_tlv(resp, rl, 0x11, &v, &vl) == 0 && vl >= 9) {
						uint8_t fcp[15];
						synthesize_gsm_fcp_15(get_le16(v), get_le16(v + 2),
								      v[4], get_le16(v + 5), fcp);
						encode_hex_upper(fcp, sizeof(fcp), hex, sizeof(hex));
						io_resp.sw1 = 0x90;
						io_resp.sw2 = 0x00;
						io_resp.response = hex;
					}
				}
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &io_resp, sizeof(io_resp));
		memset(hex, 0, sizeof(hex));
		return;
	}
	case TASK_SIM_GET_IMSI: {
		static const uint8_t path_adf[4] = { 0x00, 0x3f, 0xff, 0x7f };
		uint8_t ef[16];
		size_t efl = 0;
		char imsi[32] = "";
		int rc = uim_read_ef_bytes_locked(slot, QMI_UIM_READ_TRANSPARENT,
						  0x6f07u, path_adf, 4, 0, 9,
						  ef, sizeof(ef), &efl);
		int32_t err = HRIL_ERR_GENERIC_FAILURE;
		if (rc == 0 && decode_imsi_bcd(ef, efl, imsi, sizeof(imsi)) == 0)
			err = HRIL_ERR_SUCCESS;
		memset(ef, 0, sizeof(ef));
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    err, err == HRIL_ERR_SUCCESS ? imsi : NULL,
			    err == HRIL_ERR_SUCCESS ? strlen(imsi) : 0);
		memset(imsi, 0, sizeof(imsi));
		return;
	}
	case TASK_SIM_GET_LOCK_STATUS: {
		int32_t locked = 0;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &locked, sizeof(locked));
		return;
	}
	case TASK_SIM_SET_ACTIVE:
	case TASK_SIM_STK_READY: {
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	}
	case TASK_SIM_GET_PROTOCOL: {
		if (!g_slot_proto_inited) {
			int32_t pri = g_primary_slot_explicit ? g_primary_slot : resolve_active_slot_locked();
			if (pri < 0 || pri > 1)
				pri = 0;
			g_slot_modem_id[pri] = 0;
			g_slot_modem_id[1 - pri] = 1;
			g_slot_radio_tech[pri] = 0x1fff;
			g_slot_radio_tech[1 - pri] = 0x000f;
			g_slot_proto_inited = true;
		}
		int32_t s = (slot >= 0 && slot <= 1) ? slot : 0;
		HRilRadioProtocol proto = {
			.sessionId = 0,
			.phase = HRIL_RADIO_PROTOCOL_PHASE_INITIAL,
			.technology = g_slot_radio_tech[s],
			.modemId = g_slot_modem_id[s],
			.status = HRIL_RADIO_PROTOCOL_STATUS_SUCCESS,
		};
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &proto, sizeof(proto));
		return;
	}
	case TASK_SIM_SET_PROTOCOL: {
		HRilRadioProtocol rsp = t->radio_proto;
		rsp.status = HRIL_RADIO_PROTOCOL_STATUS_SUCCESS;
		bool send_notify = (rsp.phase == HRIL_RADIO_PROTOCOL_PHASE_UPDATE);
		if (slot >= 0 && slot <= 1 &&
		    (rsp.phase == HRIL_RADIO_PROTOCOL_PHASE_UPDATE ||
		     rsp.phase == HRIL_RADIO_PROTOCOL_PHASE_COMPLETE)) {
			if (!g_slot_proto_inited) {
				g_slot_modem_id[0] = 0;
				g_slot_modem_id[1] = 1;
				g_slot_proto_inited = true;
			}
			g_slot_modem_id[slot] = rsp.modemId;
			if (rsp.technology != 0)
				g_slot_radio_tech[slot] = rsp.technology;
			if (rsp.modemId == 0) {
				g_primary_slot = slot;
				g_primary_slot_explicit = true;
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &rsp, sizeof(rsp));
		if (send_notify) {
			HRilRadioProtocol nfy = rsp;
			nfy.phase = HRIL_RADIO_PROTOCOL_PHASE_NOTIFY;
			nfy.status = HRIL_RADIO_PROTOCOL_STATUS_SUCCESS;
			emit_report(rep->OnSimReport, slot, NULL, HNOTI_SIM_RADIO_PROTOCOL_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, &nfy, sizeof(nfy));
		}
		return;
	}
	case TASK_SIM_GET_PRIMARY: {
		if (!g_primary_slot_explicit)
			(void)resolve_active_slot_locked();
		int32_t pri = g_primary_slot;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &pri, sizeof(pri));
		return;
	}
	case TASK_SIM_SET_PRIMARY: {
		if (t->arg0 == 0 || t->arg0 == 1) {
			g_primary_slot = t->arg0;
			g_primary_slot_explicit = true;
			g_slot_modem_id[t->arg0] = 0;
			g_slot_modem_id[1 - t->arg0] = 1;
			g_slot_radio_tech[t->arg0] = 0x1fff;
			g_slot_radio_tech[1 - t->arg0] = 0x000f;
			g_slot_proto_inited = true;
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	}
	case TASK_SIM_SEND_MATCHED_OP: {
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSimReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	}
	case TASK_NET_GET_SIGNAL: {
		HRilRssi rssi;
		init_empty_rssi(&rssi);
		int32_t act = resolve_active_slot_locked();
		if (slot >= 0 && slot <= 1 && (!g_slot_present_known || g_slot_present_cached[slot])) {
			uint8_t resp[QMI_MAX_BUF];
			size_t rl = 0;
			uint16_t qerr = 0;
			nas_ensure_dsb_locked(act);
			if (qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_SIGNAL_INFO, NULL, 0,
						    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				parse_nas_signal_info(resp, rl, &rssi);
				g_last_rssi[slot] = rssi;
				g_has_last_rssi[slot] = true;
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &rssi, sizeof(rssi));
		return;
	}
	case TASK_NET_GET_CS_REG:
	case TASK_NET_GET_PS_REG: {
		HRilRegStatusInfo reg;
		memset(&reg, 0, sizeof(reg));
		reg.notifyMode = 2;
		reg.lac = -1;
		reg.cellId = -1;
		int32_t act = resolve_active_slot_locked();
		bool is_ps = (t->kind == TASK_NET_GET_PS_REG);
		if (slot >= 0 && slot <= 1 && (!g_slot_present_known || g_slot_present_cached[slot])) {
			uint8_t resp[QMI_MAX_BUF];
			size_t rl = 0;
			uint16_t qerr = 0;
			nas_ensure_dsb_locked(act);
			bool ok = false;
			if (qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_SYS_INFO, NULL, 0,
						    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				ok = parse_nas_sys_info(resp, rl, is_ps, &reg, NULL, NULL, 0, NULL, 0);
			}
			if (!ok && qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_SERVING_SYSTEM, NULL, 0,
							   resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				parse_nas_serving_info(resp, rl, is_ps, &reg, NULL, NULL, 0, NULL, 0);
			}
			if (is_ps) {
				g_last_ps_reg[slot] = reg;
				g_has_last_ps[slot] = true;
			} else {
				if (reg.regStatus == HRIL_REG_SEARCHING &&
				    (g_ims_voice_avail || g_ims_registered) &&
				    g_has_last_ps[slot] &&
				    (g_last_ps_reg[slot].regStatus == HRIL_REG_HOME ||
				     g_last_ps_reg[slot].regStatus == HRIL_REG_ROAMING)) {
					reg.regStatus = g_last_ps_reg[slot].regStatus;
				}
				g_last_cs_reg[slot] = reg;
				g_has_last_cs[slot] = true;
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &reg, sizeof(reg));
		return;
	}
	case TASK_NET_GET_OPERATOR: {
		char num[16] = "";
		char long_name[64] = "";
		char short_name[64] = "";
		int32_t act = resolve_active_slot_locked();
		if (slot >= 0 && slot <= 1 && (!g_slot_present_known || g_slot_present_cached[slot])) {
			uint8_t resp[QMI_MAX_BUF];
			size_t rl = 0;
			uint16_t qerr = 0;
			nas_ensure_dsb_locked(act);
			if (qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_SYS_INFO, NULL, 0,
						    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				(void)parse_nas_sys_info(resp, rl, true, NULL, NULL,
							 num, sizeof(num), long_name, sizeof(long_name));
			}
			if (num[0] == '\0' &&
			    qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_SERVING_SYSTEM, NULL, 0,
						    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				parse_nas_serving_info(resp, rl, false, NULL, NULL,
						       num, sizeof(num), long_name, sizeof(long_name));
			}
			if (qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_HOME_NETWORK, NULL, 0,
						    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				const uint8_t *v = NULL;
				uint16_t vl = 0;
				if ((qmi_find_tlv(resp, rl, 0x01, &v, &vl) == 0 ||
				     qmi_find_tlv(resp, rl, 0x10, &v, &vl) == 0) && vl >= 5) {
					uint16_t mcc = get_le16(v);
					uint16_t mnc = get_le16(v + 2);
					uint8_t nlen = v[4];
					if (num[0] == '\0' && mcc != 0xffffu && mnc != 0xffffu) {
						if (mnc >= 100u)
							snprintf(num, sizeof(num), "%03u%03u", mcc, mnc);
						else
							snprintf(num, sizeof(num), "%03u%02u", mcc, mnc);
					}
					if (nlen > 0 && (size_t)(5 + nlen) <= vl) {
						size_t cp = nlen < sizeof(short_name) - 1 ? nlen : sizeof(short_name) - 1;
						memcpy(short_name, v + 5, cp);
						short_name[cp] = '\0';
					}
				}
			}
			if (strcmp(num, "46011") == 0) {
				if (long_name[0] == '\0' || strcmp(short_name, "CT") == 0)
					snprintf(long_name, sizeof(long_name), "China Telecom");
				if (short_name[0] == '\0')
					snprintf(short_name, sizeof(short_name), "CT");
			} else if (long_name[0] == '\0' && short_name[0] != '\0') {
				snprintf(long_name, sizeof(long_name), "%s", short_name);
			}
			if (short_name[0] == '\0' && long_name[0] != '\0')
				snprintf(short_name, sizeof(short_name), "%s", long_name);
		}
		HRilOperatorInfo op = { .longName = long_name, .shortName = short_name, .numeric = num };
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &op, sizeof(op));
		return;
	}
	case TASK_NET_GET_NEIGHBOR_CELL: {
		/* OHOS HRilNetwork::BuildNeighboringCellList checks items != NULL even when itemNum == 0. */
		static const uint8_t s_empty_cell_buf[64] = {0};
		HRilCurrentCellList cells = { .itemNum = 0, ._pad = 0, .items = s_empty_cell_buf };
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &cells, sizeof(cells));
		return;
	}
	case TASK_NET_GET_SEARCH:
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	case TASK_NET_GET_CURRENT_CELL: {
		static const uint8_t s_empty_cell_buf[64] = {0};
		HRilCurrentCellInfoVendor cell;
		HRilCurrentCellList cells = { .itemNum = 0, ._pad = 0, .items = s_empty_cell_buf };
		int32_t act = resolve_active_slot_locked();
		if (slot >= 0 && slot <= 1 && (!g_slot_present_known || g_slot_present_cached[slot])) {
			uint8_t resp[QMI_MAX_BUF];
			size_t rl = 0;
			uint16_t qerr = 0;
			HRilRegStatusInfo reg;
			HRilRssi rssi;
			struct nas_cell_loc loc;
			char plmn[16] = "";
			bool have_reg = false;

			memset(&reg, 0, sizeof(reg));
			init_empty_rssi(&rssi);
			memset(&loc, 0, sizeof(loc));
			nas_ensure_dsb_locked(act);

			if (qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_SYS_INFO, NULL, 0,
						    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				have_reg = parse_nas_sys_info(resp, rl, true, &reg, NULL,
							      plmn, sizeof(plmn), NULL, 0);
			}
			if (!have_reg &&
			    qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_SERVING_SYSTEM, NULL, 0,
						    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				parse_nas_serving_info(resp, rl, true, &reg, NULL,
						       plmn, sizeof(plmn), NULL, 0);
				have_reg = true;
			}
			if (have_reg) {
				if (qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_SIGNAL_INFO, NULL, 0,
							    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
					parse_nas_signal_info(resp, rl, &rssi);
				} else if (g_has_last_rssi[slot]) {
					rssi = g_last_rssi[slot];
				}
				if (qmi_nas_rpc_slot_locked(slot, QMI_NAS_GET_CELL_LOCATION_INFO, NULL, 0,
							    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
					parse_nas_cell_location_info(resp, rl, &loc);
				}
				if (build_current_cell_info(&reg, plmn, &rssi, &loc, &cell)) {
					cells.itemNum = 1;
					cells.items = &cell;
				}
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &cells, sizeof(cells));
		return;
	}
	case TASK_NET_GET_SEL_MODE: {
		int32_t m = g_sel_mode;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &m, sizeof(m));
		return;
	}
	case TASK_NET_SET_SEL_MODE:
		g_sel_mode = t->arg0;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	case TASK_NET_GET_PREF_NET: {
		int32_t p = g_pref_net;
		int32_t act = resolve_active_slot_locked();
		int32_t qslot = (slot == 0 || slot == 1) ? slot : act;
		uint8_t resp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		if (qmi_nas_rpc_slot_locked(qslot, QMI_NAS_GET_SYSTEM_SELECTION_PREF, NULL, 0,
					    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
			const uint8_t *v = NULL;
			uint16_t vl = 0;
			if (qmi_find_tlv(resp, rl, 0x11, &v, &vl) == 0 && vl >= 2) {
				uint16_t mp = get_le16(v);
				if ((mp & 0x0040u) != 0u)
					p = 0; /* Auto / NR+LTE+WCDMA+GSM */
				else if ((mp & 0x0010u) != 0u)
					p = 5; /* LTE */
			}
		}
		g_pref_net = p;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &p, sizeof(p));
		return;
	}
	case TASK_NET_SET_PREF_NET: {
		g_pref_net = t->arg0;
		int32_t act = resolve_active_slot_locked();
		int32_t qslot = (slot == 0 || slot == 1) ? slot : act;
		/* Keep NR5G+LTE+WCDMA+GSM (0x005c) for auto/5G modes; LTE+WCDMA+GSM (0x001c) for 4G-only */
		uint16_t mp = 0x005cu;
		if (t->arg0 == 1)
			mp = 0x0004u; /* GSM only */
		else if (t->arg0 == 2)
			mp = 0x0008u; /* WCDMA only */
		else if (t->arg0 == 3)
			mp = 0x0010u; /* LTE only */
		else if (t->arg0 == 4 || t->arg0 == 5)
			mp = 0x001cu; /* LTE+WCDMA+GSM */
		uint8_t req_tlvs[16], resp[QMI_MAX_BUF];
		uint8_t mpb[2], dur = 0x01u; /* permanent */
		put_le16(mpb, mp);
		size_t ql = 0;
		ql += tlv_append(req_tlvs + ql, 0x11, mpb, 2);
		ql += tlv_append(req_tlvs + ql, 0x17, &dur, 1);
		size_t rl = 0;
		uint16_t qerr = 0;
		(void)qmi_nas_rpc_slot_locked(qslot, QMI_NAS_SET_SYSTEM_SELECTION_PREF,
					      req_tlvs, ql, resp, sizeof(resp), &rl, &qerr);
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	}
	case TASK_NET_GET_PHY_CHNL: {
		HRilPhyChannelConfig cfgs[4];
		size_t ncfg = 0;
		int32_t act = resolve_active_slot_locked();
		int32_t qslot = (slot == 0 || slot == 1) ? slot : act;
		if (!g_slot_present_known || g_slot_present_cached[qslot]) {
			uint8_t resp[QMI_MAX_BUF];
			size_t rl = 0;
			uint16_t qerr = 0;
			int32_t pci = 0;
			if (qmi_nas_rpc_slot_locked(qslot, QMI_NAS_GET_CELL_LOCATION_INFO, NULL, 0,
						    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				struct nas_cell_loc loc;
				parse_nas_cell_location_info(resp, rl, &loc);
				if (loc.has_nr)
					pci = loc.nr_pci;
				else if (loc.has_lte)
					pci = loc.lte_pci;
			}
			if (qmi_nas_rpc_slot_locked(qslot, QMI_NAS_GET_RF_BAND_INFO, NULL, 0,
						    resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
				ncfg = parse_nas_rf_band_info(resp, rl, pci, cfgs, 4);
			}
		}
		HRilPhyChannelConfigList phy = {
			.itemNum = (int32_t)ncfg,
			._pad = 0,
			.item = ncfg > 0 ? (const void *)cfgs : NULL,
		};
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &phy, sizeof(phy));
		return;
	}
	case TASK_NET_SET_FILTER:
		g_notif_filter = t->arg0;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	case TASK_NET_SET_DEV_STATE:
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	case TASK_NET_GET_RRC: {
		int32_t rrc = 0;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &rrc, sizeof(rrc));
		return;
	}
	case TASK_NET_SET_NR_OPT: {
		int32_t mode = (t->arg0 >= 1 && t->arg0 <= 3) ? t->arg0 : 3;
		g_nr_opt_mode = mode;
		int32_t act = resolve_active_slot_locked();
		int32_t qslot = (slot == 0 || slot == 1) ? slot : act;
		/* QMI NAS nr5g_disable_mode (TLV 0x2b): 0=enable SA+NSA, 1=disable SA (NSA only), 2=disable NSA (SA only) */
		uint32_t dis_mode = (mode == 1) ? 1u : (mode == 2) ? 2u : 0u;
		uint8_t req_tlvs[16], resp[QMI_MAX_BUF], d4[4], dur = 0x01u;
		put_le32(d4, dis_mode);
		size_t ql = 0;
		ql += tlv_append(req_tlvs + ql, 0x2b, d4, 4);
		ql += tlv_append(req_tlvs + ql, 0x17, &dur, 1);
		size_t rl = 0;
		uint16_t qerr = 0;
		(void)qmi_nas_rpc_slot_locked(qslot, QMI_NAS_SET_SYSTEM_SELECTION_PREF,
					      req_tlvs, ql, resp, sizeof(resp), &rl, &qerr);
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	}
	case TASK_NET_GET_NR_OPT: {
		int32_t nr_opt = g_nr_opt_mode;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnNetworkReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &nr_opt, sizeof(nr_opt));
		return;
	}
	case TASK_DATA_SET_APN:
		if (t->has_data_info && t->apn[0] != '\0')
			snprintf(g_init_apn, sizeof(g_init_apn), "%s", t->apn);
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	case TASK_DATA_GET_LINK_BW: {
		HRilLinkBandwidthInfo bw = {
			.cid = (t->arg0 > 0) ? t->arg0 : (g_pdp.active ? g_pdp.cid : 1),
			.qi = 9,
			.dlGfbr = 0,
			.ulGfbr = 0,
			.dlMfbr = 0,
			.ulMfbr = 0,
			.dlAmbr = 150000,
			.ulAmbr = 50000,
			.qosId = 0,
		};
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &bw, sizeof(bw));
		return;
	}
	case TASK_DATA_SET_LINK_BW:
	case TASK_DATA_SET_PROFILE:
	case TASK_DATA_SET_PERF_MODE:
	case TASK_DATA_SET_SLEEP_MODE:
	case TASK_DATA_SET_PERMITTED:
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	case TASK_DATA_DEACTIVATE_PDP:
	case TASK_DATA_CLEAN_ALL: {
		bool stopped = false;
		if (g_pdp.active &&
		    (t->kind == TASK_DATA_CLEAN_ALL || t->arg0 <= 0 || t->arg0 == g_pdp.cid)) {
			wds_stop_locked();
			stopped = true;
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		if (stopped) {
			emit_report(rep->OnDataReport, slot, NULL,
				    HNOTI_DATA_PDP_CONTEXT_LIST_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, NULL, 0);
		}
		return;
	}
	case TASK_DATA_ACTIVATE_PDP: {
		if (!t->has_data_info || slot < 0 || slot > 1 ||
		    (g_slot_present_known && !g_slot_present_cached[slot])) {
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}
		const char *use_apn = (t->apn[0] != '\0') ? t->apn
				    : (g_init_apn[0] != '\0' ? g_init_apn : "ctnet");
		/* Single-mux rmnet_data0 is reserved for internet data; never let secondary IMS/XCAP tear it down. */
		if (is_secondary_non_internet_apn(use_apn)) {
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}
		int32_t use_cid = (t->arg0 > 0) ? t->arg0 : 1;
		int32_t act = resolve_active_slot_locked();
		nas_ensure_dsb_locked(act);

		if (g_pdp.active && g_pdp.wds_sock >= 0 && g_pdp.slot_id == slot &&
		    strcmp(g_pdp.apn, use_apn) == 0) {
			struct pdp_session snap = g_pdp;
			HRilDataCallResponse dresp;
			fill_pdp_response(&snap, &dresp);
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_SUCCESS, &dresp, sizeof(dresp));
			return;
		}

		if (g_pdp.active || g_pdp.wds_sock >= 0)
			wds_stop_locked();

		if (qrtr_discover_locked() < 0 || !g_qrtr.wds.valid) {
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}

		int wsock = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (wsock < 0) {
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}

		uint8_t qreq[256], qresp[QMI_MAX_BUF];
		size_t qlen = 0, rl = 0;
		uint16_t qerr = 0;

		qlen = build_wds_bind_sub_tlvs(qreq, (uint32_t)slot);
		(void)qmi_rpc_on_sock_locked(wsock, SVC_WDS, QMI_WDS_BIND_SUBSCRIPTION,
					     qreq, qlen, qresp, sizeof(qresp), &rl, &qerr);

		if (g_qrtr.dpm.valid) {
			qlen = build_dpm_open_port_tlvs(qreq, MARBLE_EP_TYPE, MARBLE_IFACE_ID,
							MARBLE_IPA_RX_EP, MARBLE_IPA_TX_EP);
			(void)qmi_rpc_locked(SVC_DPM, QMI_DPM_OPEN_PORT,
					     qreq, qlen, qresp, sizeof(qresp), &rl, &qerr);
		}

		int wda_rc = -1;
		uint16_t wda_qerr = 0;
		if (g_qrtr.wda.valid) {
			/* UL=5 (4B QMAP), DL=9 (8B QMAPv5 with next_hdr=1 for IPA v5.0 V2 ingress) */
			qlen = build_wda_set_data_fmt_tlvs(qreq, 2u, 5u, 9u,
							   MARBLE_EP_TYPE, MARBLE_IFACE_ID);
			wda_rc = qmi_rpc_locked(SVC_WDA, QMI_WDA_SET_DATA_FORMAT,
						qreq, qlen, qresp, sizeof(qresp), &rl, &wda_qerr);
		}

		qlen = build_wds_bind_mux_tlvs(qreq, MARBLE_EP_TYPE, MARBLE_IFACE_ID,
					       MARBLE_MUX_ID, -1);
		int brc = qmi_rpc_on_sock_locked(wsock, SVC_WDS, QMI_WDS_BIND_MUX_DATA_PORT,
						 qreq, qlen, qresp, sizeof(qresp), &rl, &qerr);
		if (brc != 0 || qerr != 0) {
			qlen = build_wds_bind_mux_tlvs(qreq, MARBLE_EP_TYPE, MARBLE_IFACE_ID,
						       MARBLE_MUX_ID, 1);
			brc = qmi_rpc_on_sock_locked(wsock, SVC_WDS, QMI_WDS_BIND_MUX_DATA_PORT,
						     qreq, qlen, qresp, sizeof(qresp), &rl, &qerr);
		}

		uint8_t ip4 = 4u;
		qlen = tlv_append(qreq, 0x01, &ip4, 1);
		(void)qmi_rpc_on_sock_locked(wsock, SVC_WDS, QMI_WDS_SET_CLIENT_IP_FAMILY_PREF,
					     qreq, qlen, qresp, sizeof(qresp), &rl, &qerr);

		qlen = build_wds_start_tlvs(qreq, use_apn, 4u);
		int src = qmi_rpc_on_sock_locked(wsock, SVC_WDS, QMI_WDS_START_NETWORK_INTERFACE,
						 qreq, qlen, qresp, sizeof(qresp), &rl, &qerr);
		ril_trace_op("pdp_act", slot, wda_rc, (int)wda_qerr, brc, src, (src == 0 && qerr == 0) ? 0 : -1, qerr);
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		uint32_t handle = 0;
		if (src == 0 && qerr == 0 && qmi_find_tlv(qresp, rl, 0x01, &v, &vl) == 0 && vl >= 4)
			handle = get_le32(v);
		if (handle == 0) {
			close(wsock);
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}

		struct pdp_session sess;
		memset(&sess, 0, sizeof(sess));
		sess.active = true;
		sess.wds_sock = wsock;
		sess.handle = handle;
		sess.slot_id = slot;
		sess.cid = use_cid;
		snprintf(sess.apn, sizeof(sess.apn), "%s", use_apn);
		snprintf(sess.pdp_type, sizeof(sess.pdp_type), "%s",
			 (t->pdp_type[0] != '\0' && strcmp(t->pdp_type, "IPV4V6") != 0) ? t->pdp_type : "IP");

		qlen = build_wds_curr_settings_tlvs(qreq, 0x00003ff0u);
		int crc = qmi_rpc_on_sock_locked(wsock, SVC_WDS, QMI_WDS_GET_CURRENT_SETTINGS,
						 qreq, qlen, qresp, sizeof(qresp), &rl, &qerr);
		if (crc != 0 || qerr != 0 || !parse_wds_current_settings(qresp, rl, &sess)) {
			qlen = build_wds_stop_tlvs(qreq, handle);
			(void)qmi_rpc_on_sock_locked(wsock, SVC_WDS, QMI_WDS_STOP_NETWORK_INTERFACE,
						     qreq, qlen, qresp, sizeof(qresp), &rl, &qerr);
			close(wsock);
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}

		g_pdp = sess;
		struct pdp_session snap = g_pdp;
		HRilDataCallResponse dresp;
		fill_pdp_response(&snap, &dresp);
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &dresp, sizeof(dresp));
		emit_report(rep->OnDataReport, slot, NULL, HNOTI_DATA_PDP_CONTEXT_LIST_UPDATED,
			    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, &dresp, sizeof(dresp));
		return;
	}
	case TASK_DATA_GET_PDP_LIST: {
		if (g_pdp.active && (slot < 0 || slot == g_pdp.slot_id)) {
			struct pdp_session snap = g_pdp;
			HRilDataCallResponse dresp;
			fill_pdp_response(&snap, &dresp);
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_SUCCESS, &dresp, sizeof(dresp));
			return;
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	}
	case TASK_DATA_GET_LINK_CAP: {
		HRilDataLinkCapability cap = { 150000, 50000, 0, 0 };
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnDataReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &cap, sizeof(cap));
		return;
	}
	case TASK_CALL_GET_LIST: {
		struct cached_call_entry all_entries[MAX_VOICE_CALLS];
		struct cached_call_entry entries[MAX_VOICE_CALLS];
		size_t n = 0;
		uint8_t resp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		if (qmi_voice_rpc_slot_locked(slot, QMI_VOICE_GET_ALL_CALL_INFO, NULL, 0,
					      resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
			size_t n_all = parse_voice_call_list(resp, rl, false, all_entries, MAX_VOICE_CALLS);
			size_t ims_cnt = 0;
			n = filter_cs_call_entries(all_entries, n_all, ims_call_bridge_active(),
						   entries, MAX_VOICE_CALLS, &ims_cnt);
			g_cached_call_cnt = n;
			if (n > 0)
				memcpy(g_cached_calls, entries, n * sizeof(entries[0]));
		}
		HRilCallInfo out_calls[MAX_VOICE_CALLS];
		for (size_t i = 0; i < n; i++) {
			memset(&out_calls[i], 0, sizeof(out_calls[i]));
			out_calls[i].index = entries[i].index;
			out_calls[i].dir = entries[i].dir;
			out_calls[i].state = entries[i].state;
			out_calls[i].mode = entries[i].mode;
			out_calls[i].mpty = entries[i].mpty;
			out_calls[i].voiceDomain = 0; /* CSControl in SA 4006 */
			out_calls[i].callType = 0;
			out_calls[i].number = entries[i].number;
			out_calls[i].type = entries[i].type;
			out_calls[i].alpha = "";
			out_calls[i].name = "";
			out_calls[i].namePresentation = 0;
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnCallReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, n > 0 ? (const void *)out_calls : NULL,
			    n * sizeof(HRilCallInfo));
		return;
	}
	case TASK_CALL_DIAL: {
		if (!t->has_data_info || t->str0[0] == '\0') {
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnCallReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}
		bool prefer_volte = g_ims_voice_avail || g_ims_registered ||
			(slot >= 0 && slot <= 1 && g_has_last_ps[slot] &&
			 (g_last_ps_reg[slot].actType == HRIL_TECH_LTE ||
			  g_last_ps_reg[slot].actType == HRIL_TECH_NR));
		int try_types[3];
		int ntry = 0;
		if (prefer_volte) {
			try_types[ntry++] = 0x08; /* QMI CALL_TYPE_VOICE_IP (VoLTE/VoNR) */
			try_types[ntry++] = 0x00; /* QMI CALL_TYPE_VOICE */
			try_types[ntry++] = -1;   /* bare TLV 0x01 */
		} else {
			try_types[ntry++] = 0x00;
			try_types[ntry++] = 0x08;
			try_types[ntry++] = -1;
		}
		uint8_t qreq[160], qresp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		int rc = -EIO;
		for (int ti = 0; ti < ntry; ti++) {
			size_t ql = build_voice_dial_tlvs(qreq, sizeof(qreq), t->str0, try_types[ti]);
			if (ql == 0)
				continue;
			rl = 0;
			qerr = 0;
			rc = qmi_voice_rpc_slot_locked(slot, QMI_VOICE_DIAL_CALL,
						       qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
			if (rc == 0 && qerr == 0)
				break;
		}
		int32_t err = (rc == 0 && qerr == 0) ? HRIL_ERR_SUCCESS : HRIL_ERR_GENERIC_FAILURE;
		if (err == HRIL_ERR_SUCCESS) {
			g_hangup_pending = false;
			g_hangup_slot = slot;
			g_last_call_fail_reason = 16;
			g_call_watchdog_ticks = 30;
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnCallReport, slot, req, 0, HRIL_REPORT_RESPONSE, err, NULL, 0);
		if (err == HRIL_ERR_SUCCESS) {
			emit_report(rep->OnCallReport, slot, NULL, HNOTI_CALL_STATE_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, NULL, 0);
		}
		return;
	}
	case TASK_CALL_HANGUP: {
		uint8_t cid = (t->arg0 > 0 && t->arg0 <= 255) ? (uint8_t)t->arg0 : 1u;
		uint8_t qreq[16], qresp[QMI_MAX_BUF];
		size_t ql = tlv_append(qreq, 0x01, &cid, 1);
		size_t rl = 0;
		uint16_t qerr = 0;
		int rc = qmi_voice_rpc_slot_locked(slot, QMI_VOICE_END_CALL,
						   qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		bool already_empty = false;
		if (rc != 0 || qerr != 0) {
			/* Fallback: refresh active calls and end any live call IDs */
			size_t crl = 0;
			uint16_t cqerr = 0;
			if (qmi_voice_rpc_slot_locked(slot, QMI_VOICE_GET_ALL_CALL_INFO, NULL, 0,
						      qresp, sizeof(qresp), &crl, &cqerr) == 0 && cqerr == 0) {
				struct cached_call_entry entries[MAX_VOICE_CALLS];
				size_t nc = parse_voice_call_list(qresp, crl, false, entries, MAX_VOICE_CALLS);
				g_cached_call_cnt = nc;
				if (nc > 0)
					memcpy(g_cached_calls, entries, nc * sizeof(entries[0]));
				if (nc == 0) {
					already_empty = true;
					rc = 0;
					qerr = 0;
				} else {
					for (size_t i = 0; i < nc; i++) {
						uint8_t ecid = (uint8_t)entries[i].index;
						size_t eql = tlv_append(qreq, 0x01, &ecid, 1);
						size_t erl = 0;
						uint16_t eqerr = 0;
						int erc = qmi_voice_rpc_slot_locked(slot, QMI_VOICE_END_CALL,
										    qreq, eql, qresp, sizeof(qresp),
										    &erl, &eqerr);
						if (erc == 0 && eqerr == 0) {
							rc = 0;
							qerr = 0;
						}
					}
				}
			}
		}
		int32_t err = (rc == 0 && qerr == 0) ? HRIL_ERR_SUCCESS : HRIL_ERR_GENERIC_FAILURE;
		if (err == HRIL_ERR_SUCCESS) {
			g_last_call_fail_reason = 16;
			if (!already_empty) {
				/* Wait for QMI_VOICE_ALL_CALL_STATUS_IND or watchdog poll to confirm SIP BYE
				 * finished before firing HNOTI_CALL_STATE_UPDATED so GetCallList sees 0 calls. */
				g_hangup_pending = true;
				g_hangup_slot = slot;
				g_call_watchdog_ticks = 20;
			} else {
				g_hangup_pending = false;
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnCallReport, slot, req, 0, HRIL_REPORT_RESPONSE, err, NULL, 0);
		if (already_empty) {
			emit_report(rep->OnCallReport, slot, NULL, HNOTI_CALL_STATE_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, NULL, 0);
		}
		return;
	}
	case TASK_CALL_ANSWER:
	case TASK_CALL_REJECT: {
		uint8_t cid = 0;
		for (size_t i = 0; i < g_cached_call_cnt; i++) {
			if (g_cached_calls[i].state == 4 || g_cached_calls[i].state == 5) {
				cid = (uint8_t)g_cached_calls[i].index;
				break;
			}
		}
		if (cid == 0) {
			uint8_t qresp[QMI_MAX_BUF];
			size_t rl = 0;
			uint16_t qerr = 0;
			if (qmi_voice_rpc_slot_locked(slot, QMI_VOICE_GET_ALL_CALL_INFO, NULL, 0,
						      qresp, sizeof(qresp), &rl, &qerr) == 0 && qerr == 0) {
				g_cached_call_cnt = parse_voice_call_list(qresp, rl, false,
									  g_cached_calls, MAX_VOICE_CALLS);
				for (size_t i = 0; i < g_cached_call_cnt; i++) {
					if (g_cached_calls[i].state == 4 || g_cached_calls[i].state == 5) {
						cid = (uint8_t)g_cached_calls[i].index;
						break;
					}
				}
				if (cid == 0 && g_cached_call_cnt > 0)
					cid = (uint8_t)g_cached_calls[0].index;
			}
		}
		if (cid == 0)
			cid = 1u;
		uint8_t qreq[16], qresp[QMI_MAX_BUF];
		size_t ql = tlv_append(qreq, 0x01, &cid, 1);
		size_t rl = 0;
		uint16_t qerr = 0;
		uint16_t mid = (t->kind == TASK_CALL_ANSWER) ? QMI_VOICE_ANSWER_CALL : QMI_VOICE_END_CALL;
		int rc = qmi_voice_rpc_slot_locked(slot, mid, qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		int32_t err = (rc == 0 && qerr == 0) ? HRIL_ERR_SUCCESS : HRIL_ERR_GENERIC_FAILURE;
		if (err == HRIL_ERR_SUCCESS) {
			if (t->kind == TASK_CALL_REJECT) {
				g_last_call_fail_reason = 21; /* CALL_REJECTED */
				g_hangup_pending = true;
				g_hangup_slot = slot;
				g_call_watchdog_ticks = 20;
			} else {
				g_hangup_pending = false;
				g_hangup_slot = slot;
				g_call_watchdog_ticks = 30;
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnCallReport, slot, req, 0, HRIL_REPORT_RESPONSE, err, NULL, 0);
		if (err == HRIL_ERR_SUCCESS && t->kind == TASK_CALL_ANSWER) {
			emit_report(rep->OnCallReport, slot, NULL, HNOTI_CALL_STATE_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, NULL, 0);
		}
		return;
	}
	case TASK_CALL_HOLD:
	case TASK_CALL_UNHOLD:
	case TASK_CALL_SWITCH: {
		uint8_t stype = 0x03u; /* HOLD_ACTIVE_ACCEPT_WAITING_OR_HELD */
		uint8_t qreq[16], qresp[QMI_MAX_BUF];
		size_t ql = tlv_append(qreq, 0x01, &stype, 1);
		size_t rl = 0;
		uint16_t qerr = 0;
		int rc = qmi_voice_rpc_slot_locked(slot, QMI_VOICE_MANAGE_CALLS,
						   qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		int32_t err = (rc == 0 && qerr == 0) ? HRIL_ERR_SUCCESS : HRIL_ERR_GENERIC_FAILURE;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnCallReport, slot, req, 0, HRIL_REPORT_RESPONSE, err, NULL, 0);
		if (err == HRIL_ERR_SUCCESS) {
			emit_report(rep->OnCallReport, slot, NULL, HNOTI_CALL_STATE_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, NULL, 0);
		}
		return;
	}
	case TASK_CALL_SUPPLEMENT: {
		/* OHOS CSControl::HangUp CallSupplement types:
		 *   1 = TYPE_HANG_UP_HOLD_WAIT -> QMI MANAGE_CALLS 0x01 (RELEASE_HELD_OR_WAITING)
		 *   2 = TYPE_HANG_UP_ACTIVE    -> QMI MANAGE_CALLS 0x02 (RELEASE_ACTIVE_ACCEPT_HELD_OR_WAITING) */
		uint8_t stype = (t->arg0 == 1) ? 0x01u : 0x02u;
		uint8_t qreq[16], qresp[QMI_MAX_BUF];
		size_t ql = tlv_append(qreq, 0x01, &stype, 1);
		size_t rl = 0;
		uint16_t qerr = 0;
		int rc = qmi_voice_rpc_slot_locked(slot, QMI_VOICE_MANAGE_CALLS,
						   qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		bool already_empty = false;
		if (rc != 0 || qerr != 0) {
			size_t crl = 0;
			uint16_t cqerr = 0;
			if (qmi_voice_rpc_slot_locked(slot, QMI_VOICE_GET_ALL_CALL_INFO, NULL, 0,
						      qresp, sizeof(qresp), &crl, &cqerr) == 0 && cqerr == 0) {
				struct cached_call_entry entries[MAX_VOICE_CALLS];
				size_t nc = parse_voice_call_list(qresp, crl, false, entries, MAX_VOICE_CALLS);
				g_cached_call_cnt = nc;
				if (nc > 0)
					memcpy(g_cached_calls, entries, nc * sizeof(entries[0]));
				if (nc == 0) {
					already_empty = true;
					rc = 0;
					qerr = 0;
				} else {
					for (size_t i = 0; i < nc; i++) {
						bool match = (t->arg0 == 1)
							? (entries[i].state == 1 || entries[i].state == 5)
							: (entries[i].state == 0 || entries[i].state == 2 || entries[i].state == 3);
						if (!match && nc > 1)
							continue;
						uint8_t ecid = (uint8_t)entries[i].index;
						size_t eql = tlv_append(qreq, 0x01, &ecid, 1);
						size_t erl = 0;
						uint16_t eqerr = 0;
						int erc = qmi_voice_rpc_slot_locked(slot, QMI_VOICE_END_CALL,
										    qreq, eql, qresp, sizeof(qresp),
										    &erl, &eqerr);
						if (erc == 0 && eqerr == 0) {
							rc = 0;
							qerr = 0;
						}
					}
				}
			}
		}
		int32_t err = (rc == 0 && qerr == 0) ? HRIL_ERR_SUCCESS : HRIL_ERR_GENERIC_FAILURE;
		if (err == HRIL_ERR_SUCCESS) {
			g_last_call_fail_reason = 16;
			if (!already_empty) {
				g_hangup_pending = true;
				g_hangup_slot = slot;
				g_call_watchdog_ticks = 20;
			} else {
				g_hangup_pending = false;
			}
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnCallReport, slot, req, 0, HRIL_REPORT_RESPONSE, err, NULL, 0);
		if (already_empty) {
			emit_report(rep->OnCallReport, slot, NULL, HNOTI_CALL_STATE_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, NULL, 0);
		}
		return;
	}
	case TASK_CALL_GET_FAIL_REASON: {
		int32_t reason = (g_last_call_fail_reason > 0) ? g_last_call_fail_reason : 16;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnCallReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &reason, sizeof(reason));
		return;
	}
	case TASK_CALL_START_DTMF:
	case TASK_CALL_SEND_DTMF:
	case TASK_CALL_STOP_DTMF: {
		uint8_t cid = (t->arg0 > 0 && t->arg0 <= 255) ? (uint8_t)t->arg0 : 1u;
		uint8_t qreq[64], qresp[QMI_MAX_BUF];
		size_t ql = 0, rl = 0;
		uint16_t qerr = 0;
		uint16_t mid = QMI_VOICE_STOP_CONT_DTMF;
		if (t->kind == TASK_CALL_START_DTMF) {
			uint8_t d[2] = { cid, (uint8_t)(t->str0[0] ? t->str0[0] : '0') };
			ql = tlv_append(qreq, 0x01, d, 2);
			mid = QMI_VOICE_START_CONT_DTMF;
		} else if (t->kind == TASK_CALL_SEND_DTMF) {
			size_t sl = strlen(t->str0);
			if (sl > 32)
				sl = 32;
			uint8_t d[40];
			d[0] = cid;
			d[1] = (uint8_t)sl;
			if (sl > 0)
				memcpy(d + 2, t->str0, sl);
			ql = tlv_append(qreq, 0x01, d, (uint16_t)(2 + sl));
			mid = QMI_VOICE_BURST_DTMF;
		} else {
			ql = tlv_append(qreq, 0x01, &cid, 1);
			mid = QMI_VOICE_STOP_CONT_DTMF;
		}
		int rc = qmi_voice_rpc_slot_locked(slot, mid, qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		int32_t err = (rc == 0 && qerr == 0) ? HRIL_ERR_SUCCESS : HRIL_ERR_GENERIC_FAILURE;
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnCallReport, slot, req, 0, HRIL_REPORT_RESPONSE, err, NULL, 0);
		return;
	}
	case TASK_SMS_SEND_GSM: {
		if (!t->has_data_info || t->str1[0] == '\0') {
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnSmsReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}
		/* Lazily cache SMSC from QMI_WMS if not yet populated */
		if (g_cached_smsc_raw_len == 0) {
			uint8_t qresp_sc[QMI_MAX_BUF];
			size_t rl_sc = 0;
			uint16_t qerr_sc = 0;
			if (qmi_wms_rpc_slot_locked(slot, QMI_WMS_GET_SMSC_ADDRESS, NULL, 0,
						    qresp_sc, sizeof(qresp_sc), &rl_sc, &qerr_sc) == 0 &&
			    qerr_sc == 0) {
				const uint8_t *vsc = NULL;
				uint16_t vlsc = 0;
				if (qmi_find_tlv(qresp_sc, rl_sc, 0x01, &vsc, &vlsc) == 0 && vlsc >= 4) {
					uint8_t alen = vsc[3];
					if ((size_t)(4 + alen) <= vlsc && alen > 0 && alen < sizeof(g_cached_smsc_addr)) {
						memcpy(g_cached_smsc_addr, vsc + 4, alen);
						g_cached_smsc_addr[alen] = '\0';
						g_cached_smsc_tosca = (vsc[0] == '1' && vsc[1] == '2' && vsc[2] == '9') ? 129 : 145;
						g_cached_smsc_raw_len = encode_smsc_bcd(
							g_cached_smsc_addr, g_cached_smsc_tosca,
							g_cached_smsc_raw, sizeof(g_cached_smsc_raw));
					}
				}
			}
		}

		uint8_t tpdu[280];
		size_t tlen = decode_hex_bytes(t->str1, tpdu, sizeof(tpdu));
		if (tlen == 0) {
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnSmsReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}

		uint8_t explicit_sc[24];
		size_t explicit_sc_len = 0;
		if (t->str0[0] != '\0') {
			if (t->str0[0] == '+' || strlen(t->str0) <= 15) {
				/* Could be ASCII SMSC address or hex TS 24.011 SMSC */
				size_t hlen = decode_hex_bytes(t->str0, explicit_sc, sizeof(explicit_sc));
				if (hlen >= 2 && explicit_sc[0] == (uint8_t)(hlen - 1))
					explicit_sc_len = hlen;
				else
					explicit_sc_len = encode_smsc_bcd(t->str0, 145, explicit_sc, sizeof(explicit_sc));
			} else {
				explicit_sc_len = decode_hex_bytes(t->str0, explicit_sc, sizeof(explicit_sc));
			}
		}
		if (explicit_sc_len == 0 && g_cached_smsc_raw_len > 0 && g_cached_smsc_raw_len <= 16) {
			memcpy(explicit_sc, g_cached_smsc_raw, g_cached_smsc_raw_len);
			explicit_sc_len = g_cached_smsc_raw_len;
		}

		uint8_t pdu_sc[300], pdu_def[300];
		size_t len_sc = 0, len_def = 0;
		if (explicit_sc_len > 0 && explicit_sc_len + tlen <= sizeof(pdu_sc)) {
			memcpy(pdu_sc, explicit_sc, explicit_sc_len);
			memcpy(pdu_sc + explicit_sc_len, tpdu, tlen);
			len_sc = explicit_sc_len + tlen;
		}
		if (1u + tlen <= sizeof(pdu_def)) {
			pdu_def[0] = 0x00u;
			memcpy(pdu_def + 1, tpdu, tlen);
			len_def = 1u + tlen;
		}

		struct gsm_send_attempt {
			const uint8_t *data;
			size_t len;
			bool ims;
		} attempts[4];
		int natt = 0;
		bool prefer_ims = (g_ims_sms_avail || g_ims_registered);
		if (len_sc > 0)
			attempts[natt++] = (struct gsm_send_attempt){ pdu_sc, len_sc, prefer_ims };
		if (len_def > 0)
			attempts[natt++] = (struct gsm_send_attempt){ pdu_def, len_def, prefer_ims };
		if (prefer_ims) {
			if (len_sc > 0)
				attempts[natt++] = (struct gsm_send_attempt){ pdu_sc, len_sc, false };
			if (len_def > 0)
				attempts[natt++] = (struct gsm_send_attempt){ pdu_def, len_def, false };
		}

		uint8_t qreq[340], qresp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		int rc = -EIO;
		for (int ai = 0; ai < natt; ai++) {
			size_t ql = build_wms_raw_send_tlvs(qreq, sizeof(qreq),
							    attempts[ai].data, attempts[ai].len,
							    attempts[ai].ims);
			if (ql == 0)
				continue;
			rl = 0;
			qerr = 0;
			rc = qmi_wms_rpc_slot_locked(slot, QMI_WMS_RAW_SEND,
						     qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
			ril_trace_op("sms_gsm", slot, ai, attempts[ai].ims ? 1 : 0,
				     (int)explicit_sc_len, (int)tlen, rc, qerr);
			if (rc == 0 && qerr == 0)
				break;
		}
		HRilSmsResponse sresp;
		memset(&sresp, 0, sizeof(sresp));
		sresp.pdu = "";
		int32_t err = HRIL_ERR_GENERIC_FAILURE;
		if (rc == 0 && qerr == 0) {
			const uint8_t *v = NULL;
			uint16_t vl = 0;
			if (qmi_find_tlv(qresp, rl, 0x01, &v, &vl) == 0 && vl >= 2)
				sresp.msgRef = (int32_t)get_le16(v);
			sresp.errCode = 0;
			err = HRIL_ERR_SUCCESS;
		} else {
			sresp.errCode = (qerr != 0) ? (int32_t)qerr : 1;
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSmsReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    err, err == HRIL_ERR_SUCCESS ? &sresp : NULL,
			    err == HRIL_ERR_SUCCESS ? sizeof(sresp) : 0);
		return;
	}
	case TASK_SMS_SEND_ACK: {
		bool ok = (t->arg0 != 0);
		uint8_t cause = (uint8_t)(t->arg1 & 0xff);
		if (g_has_mt_sms_ack) {
			uint8_t qreq[32], qresp[QMI_MAX_BUF];
			size_t ql = build_wms_send_ack_tlvs(qreq, g_last_mt_sms_tx_id, ok,
							    cause, g_last_mt_sms_on_ims);
			size_t rl = 0;
			uint16_t qerr = 0;
			(void)qmi_wms_rpc_slot_locked(slot, QMI_WMS_SEND_ACK,
						      qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
			g_has_mt_sms_ack = false;
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSmsReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	}
	case TASK_SMS_SEND_CDMA: {
		if (!t->has_data_info || t->str1[0] == '\0') {
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnSmsReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}
		if (g_cached_smsc_raw_len == 0) {
			uint8_t qresp_sc[QMI_MAX_BUF];
			size_t rl_sc = 0;
			uint16_t qerr_sc = 0;
			if (qmi_wms_rpc_slot_locked(slot, QMI_WMS_GET_SMSC_ADDRESS, NULL, 0,
						    qresp_sc, sizeof(qresp_sc), &rl_sc, &qerr_sc) == 0 &&
			    qerr_sc == 0) {
				const uint8_t *vsc = NULL;
				uint16_t vlsc = 0;
				if (qmi_find_tlv(qresp_sc, rl_sc, 0x01, &vsc, &vlsc) == 0 && vlsc >= 4) {
					uint8_t alen = vsc[3];
					if ((size_t)(4 + alen) <= vlsc && alen > 0 && alen < sizeof(g_cached_smsc_addr)) {
						memcpy(g_cached_smsc_addr, vsc + 4, alen);
						g_cached_smsc_addr[alen] = '\0';
						g_cached_smsc_tosca = (vsc[0] == '1' && vsc[1] == '2' && vsc[2] == '9') ? 129 : 145;
						g_cached_smsc_raw_len = encode_smsc_bcd(
							g_cached_smsc_addr, g_cached_smsc_tosca,
							g_cached_smsc_raw, sizeof(g_cached_smsc_raw));
					}
				}
			}
		}
		uint8_t cdma_pdu[300];
		size_t cdma_len = decode_hex_bytes(t->str1, cdma_pdu, sizeof(cdma_pdu));
		if (cdma_len == 0) {
			pthread_mutex_unlock(&g_state_lock);
			emit_report(rep->OnSmsReport, slot, req, 0, HRIL_REPORT_RESPONSE,
				    HRIL_ERR_GENERIC_FAILURE, NULL, 0);
			return;
		}
		uint8_t gsm_pdu_sc[300], gsm_pdu_def[300];
		size_t gsm_len_sc = 0, gsm_len_def = 0;
		bool have_gsm_sc = (g_cached_smsc_raw_len > 0) && transcode_cdma_pdu_to_gsm_submit(
			cdma_pdu, cdma_len,
			g_cached_smsc_raw, g_cached_smsc_raw_len,
			gsm_pdu_sc, sizeof(gsm_pdu_sc), &gsm_len_sc);
		bool have_gsm_def = transcode_cdma_pdu_to_gsm_submit(
			cdma_pdu, cdma_len, NULL, 0,
			gsm_pdu_def, sizeof(gsm_pdu_def), &gsm_len_def);

		/* China Telecom 4G/5G VoLTE/VoNR IMS SMS uses 3GPP format 0x06 over SIP;
		 * try transcoded 3GPP SMS-SUBMIT (explicit SMSC & 0x00 default SMSC) first,
		 * falling back to native 3GPP2 format 0x00. */
		struct cdma_send_attempt {
			uint8_t fmt;
			const uint8_t *data;
			size_t len;
			bool ims;
		} attempts[6];
		int natt = 0;
		bool prefer_ims = (g_ims_sms_avail || g_ims_registered);
		if (prefer_ims) {
			if (have_gsm_sc)
				attempts[natt++] = (struct cdma_send_attempt){ 0x06u, gsm_pdu_sc, gsm_len_sc, true };
			if (have_gsm_def)
				attempts[natt++] = (struct cdma_send_attempt){ 0x06u, gsm_pdu_def, gsm_len_def, true };
			if (have_gsm_sc)
				attempts[natt++] = (struct cdma_send_attempt){ 0x06u, gsm_pdu_sc, gsm_len_sc, false };
			if (have_gsm_def)
				attempts[natt++] = (struct cdma_send_attempt){ 0x06u, gsm_pdu_def, gsm_len_def, false };
			attempts[natt++] = (struct cdma_send_attempt){ 0x00u, cdma_pdu, cdma_len, true };
			attempts[natt++] = (struct cdma_send_attempt){ 0x00u, cdma_pdu, cdma_len, false };
		} else {
			attempts[natt++] = (struct cdma_send_attempt){ 0x00u, cdma_pdu, cdma_len, false };
			if (have_gsm_sc)
				attempts[natt++] = (struct cdma_send_attempt){ 0x06u, gsm_pdu_sc, gsm_len_sc, false };
			if (have_gsm_def)
				attempts[natt++] = (struct cdma_send_attempt){ 0x06u, gsm_pdu_def, gsm_len_def, false };
		}

		uint8_t qreq[340], qresp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		int rc = -EIO;
		for (int ai = 0; ai < natt; ai++) {
			size_t ql = build_wms_raw_send_fmt_tlvs(qreq, sizeof(qreq),
								attempts[ai].fmt,
								attempts[ai].data, attempts[ai].len,
								attempts[ai].ims);
			if (ql == 0)
				continue;
			rl = 0;
			qerr = 0;
			rc = qmi_wms_rpc_slot_locked(slot, QMI_WMS_RAW_SEND,
						     qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
			ril_trace_op("sms_cdma", slot, ai, (int)attempts[ai].fmt,
				     attempts[ai].ims ? 1 : 0, (int)attempts[ai].len, rc, qerr);
			if (rc == 0 && qerr == 0)
				break;
		}

		HRilSmsResponse sresp;
		memset(&sresp, 0, sizeof(sresp));
		sresp.pdu = "";
		int32_t err = HRIL_ERR_GENERIC_FAILURE;
		if (rc == 0 && qerr == 0) {
			const uint8_t *v = NULL;
			uint16_t vl = 0;
			if (qmi_find_tlv(qresp, rl, 0x01, &v, &vl) == 0 && vl >= 2)
				sresp.msgRef = (int32_t)get_le16(v);
			sresp.errCode = 0;
			err = HRIL_ERR_SUCCESS;
		} else {
			sresp.errCode = (qerr != 0) ? (int32_t)qerr : 1;
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSmsReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    err, err == HRIL_ERR_SUCCESS ? &sresp : NULL,
			    err == HRIL_ERR_SUCCESS ? sizeof(sresp) : 0);
		return;
	}
	case TASK_SMS_SET_SMSC: {
		if (t->str0[0] != '\0') {
			snprintf(g_cached_smsc_addr, sizeof(g_cached_smsc_addr), "%.47s", t->str0);
			g_cached_smsc_tosca = (t->arg0 > 0) ? t->arg0 : 145;
			uint8_t qreq[80], qresp[QMI_MAX_BUF];
			size_t alen = strlen(t->str0);
			if (alen > 32)
				alen = 32;
			char tstr[4];
			snprintf(tstr, sizeof(tstr), "%03d",
				 (g_cached_smsc_tosca >= 0 && g_cached_smsc_tosca <= 999) ? g_cached_smsc_tosca : 145);
			size_t ql = 0;
			ql += tlv_append(qreq + ql, 0x01, tstr, 3);
			ql += tlv_append(qreq + ql, 0x10, t->str0, (uint16_t)alen);
			size_t rl = 0;
			uint16_t qerr = 0;
			(void)qmi_wms_rpc_slot_locked(slot, QMI_WMS_SET_SMSC_ADDRESS,
						      qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		}
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSmsReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, NULL, 0);
		return;
	}
	case TASK_SMS_GET_SMSC: {
		char addr[48] = "";
		int32_t tosca = 145;
		uint8_t qresp[QMI_MAX_BUF];
		size_t rl = 0;
		uint16_t qerr = 0;
		/* 1. Try QMI_WMS_GET_SMSC_ADDRESS (0x0034) on per-slot bound WMS client */
		if (qmi_wms_rpc_slot_locked(slot, QMI_WMS_GET_SMSC_ADDRESS, NULL, 0,
					    qresp, sizeof(qresp), &rl, &qerr) == 0 && qerr == 0) {
			const uint8_t *v = NULL;
			uint16_t vl = 0;
			if (qmi_find_tlv(qresp, rl, 0x01, &v, &vl) == 0 && vl >= 4) {
				/* type(3 bytes ASCII e.g. "145") + len(1) + digits */
				uint8_t alen = v[3];
				if ((size_t)(4 + alen) <= vl && alen > 0) {
					size_t cp = alen < sizeof(addr) - 1 ? alen : sizeof(addr) - 1;
					memcpy(addr, v + 4, cp);
					addr[cp] = '\0';
					if (v[0] == '1' && v[1] == '4' && v[2] == '5')
						tosca = 145;
					else if (v[0] == '1' && v[1] == '2' && v[2] == '9')
						tosca = 129;
				}
			}
		}
		/* 2. Fallback to USIM EF_SMSP (0x6F42, path 3F007FFF, rec 1) via QMI_UIM */
		if (addr[0] == '\0') {
			int32_t act = resolve_active_slot_locked();
			int32_t qslot = (slot == 0 || slot == 1) ? slot : act;
			uint8_t path[4] = { 0x00, 0x3f, 0xff, 0x7f };
			uint8_t rec[128];
			size_t rec_len = 0;
			if (uim_read_ef_bytes_locked(qslot, QMI_UIM_READ_RECORD, 0x6f42u,
						     path, 4, 1, 0, rec, sizeof(rec), &rec_len) == 0) {
				(void)parse_smsp_record(rec, rec_len, addr, sizeof(addr), &tosca,
							g_cached_smsc_raw, sizeof(g_cached_smsc_raw),
							&g_cached_smsc_raw_len);
			}
		}
		if (addr[0] != '\0') {
			snprintf(g_cached_smsc_addr, sizeof(g_cached_smsc_addr), "%s", addr);
			g_cached_smsc_tosca = tosca;
			if (g_cached_smsc_raw_len == 0) {
				g_cached_smsc_raw_len = encode_smsc_bcd(
					g_cached_smsc_addr, g_cached_smsc_tosca,
					g_cached_smsc_raw, sizeof(g_cached_smsc_raw));
			}
		} else if (g_cached_smsc_addr[0] != '\0') {
			snprintf(addr, sizeof(addr), "%s", g_cached_smsc_addr);
			tosca = g_cached_smsc_tosca;
		}
		HRilServiceCenterAddress smsc = {
			.tosca = tosca,
			._pad = 0,
			.address = addr,
		};
		pthread_mutex_unlock(&g_state_lock);
		emit_report(rep->OnSmsReport, slot, req, 0, HRIL_REPORT_RESPONSE,
			    HRIL_ERR_SUCCESS, &smsc, sizeof(smsc));
		return;
	}
	}
	pthread_mutex_unlock(&g_state_lock);
}

#ifndef HRIL_VENDOR_HOST_TEST
static void poll_nas_delta_tick(void)
{
	const HRilReport *rep = g_report_ops;
	if (rep == NULL || rep->OnNetworkReport == NULL)
		return;

	HRilRegStatusInfo cs_reg, ps_reg;
	HRilVoiceRadioInfo voice_on;
	HRilRssi rssi;
	bool send_cs = false, send_ps = false, send_sig = false;
	bool notify_radio_on = false, send_voice = false;
	int32_t act = 0;

	memset(&voice_on, 0, sizeof(voice_on));
	voice_on.sysModeName = "Unknown";
	voice_on.actName = "Unknown";

	pthread_mutex_lock(&g_state_lock);
	uint8_t dm_resp[QMI_MAX_BUF];
	size_t dm_rl = 0;
	uint16_t dm_err = 0;
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	bool radio_on = false;
	if (qmi_rpc_locked(SVC_DMS, QMI_DMS_GET_OP_MODE, NULL, 0,
			   dm_resp, sizeof(dm_resp), &dm_rl, &dm_err) == 0 && dm_err == 0 &&
	    qmi_find_tlv(dm_resp, dm_rl, 0x01, &v, &vl) == 0 && vl >= 1) {
		radio_on = (v[0] == 0);
	}
	if (!radio_on) {
		pthread_mutex_unlock(&g_state_lock);
		return;
	}

	int32_t slot_states[2] = { HRIL_RADIO_POWER_OFF, HRIL_RADIO_POWER_OFF };
	if (!g_radio_on_notified) {
		sync_slot_radio_defaults_locked(true);
		slot_states[0] = g_slot_radio_state[0];
		slot_states[1] = g_slot_radio_state[1];
		g_radio_on_notified = true;
		notify_radio_on = true;
	}

	act = resolve_active_slot_locked();
	if (act >= 0 && act <= 1 && (!g_slot_present_known || g_slot_present_cached[act])) {
		uint8_t si[QMI_MAX_BUF], sig[QMI_MAX_BUF];
		size_t sil = 0, sigl = 0;
		uint16_t serr = 0;
		nas_ensure_dsb_locked(act);
		if (qmi_nas_rpc_slot_locked(act, QMI_NAS_GET_SYS_INFO, NULL, 0,
					    si, sizeof(si), &sil, &serr) == 0 && serr == 0) {
			if (parse_nas_sys_info(si, sil, true, &ps_reg, NULL, NULL, 0, NULL, 0)) {
				if (!g_has_last_ps[act] || !nas_reg_equal(&g_last_ps_reg[act], &ps_reg)) {
					g_last_ps_reg[act] = ps_reg;
					g_has_last_ps[act] = true;
					send_ps = true;
				}
			}
			if (parse_nas_sys_info(si, sil, false, &cs_reg, &voice_on, NULL, 0, NULL, 0)) {
				if (cs_reg.regStatus == HRIL_REG_SEARCHING &&
				    (g_ims_voice_avail || g_ims_registered) &&
				    g_has_last_ps[act] &&
				    (g_last_ps_reg[act].regStatus == HRIL_REG_HOME ||
				     g_last_ps_reg[act].regStatus == HRIL_REG_ROAMING)) {
					cs_reg.regStatus = g_last_ps_reg[act].regStatus;
					voice_on.srvStatus = 2;
				}
				if (notify_radio_on)
					send_voice = true;
				if (!g_has_last_cs[act] || !nas_reg_equal(&g_last_cs_reg[act], &cs_reg)) {
					g_last_cs_reg[act] = cs_reg;
					g_has_last_cs[act] = true;
					send_cs = true;
				}
			}
		}
		if (qmi_nas_rpc_slot_locked(act, QMI_NAS_GET_SIGNAL_INFO, NULL, 0,
					    sig, sizeof(sig), &sigl, &serr) == 0 && serr == 0) {
			parse_nas_signal_info(sig, sigl, &rssi);
			if (!g_has_last_rssi[act] || rssi_changed_significantly(&g_last_rssi[act], &rssi)) {
				g_last_rssi[act] = rssi;
				g_has_last_rssi[act] = true;
				send_sig = true;
			}
		}
	}
	pthread_mutex_unlock(&g_state_lock);

	if (notify_radio_on && rep->OnModemReport != NULL) {
		for (int32_t s = 0; s < 2; s++) {
			emit_report(rep->OnModemReport, s, NULL, HNOTI_MODEM_RADIO_STATE_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS,
				    &slot_states[s], sizeof(slot_states[s]));
		}
		if (rep->OnSimReport != NULL) {
			for (int32_t s = 0; s < 2; s++) {
				emit_report(rep->OnSimReport, s, NULL, HNOTI_SIM_STATUS_UPDATED,
					    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, NULL, 0);
			}
		}
		if (send_voice) {
			emit_report(rep->OnModemReport, act, NULL, HNOTI_MODEM_VOICE_TECH_UPDATED,
				    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, &voice_on, sizeof(voice_on));
		}
	}
	if (send_cs)
		emit_report(rep->OnNetworkReport, act, NULL, HNOTI_NETWORK_CS_REG_STATUS_UPDATED,
			    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, &cs_reg, sizeof(cs_reg));
	if (send_ps)
		emit_report(rep->OnNetworkReport, act, NULL, HNOTI_NETWORK_PS_REG_STATUS_UPDATED,
			    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, &ps_reg, sizeof(ps_reg));
	if (send_sig)
		emit_report(rep->OnNetworkReport, act, NULL, HNOTI_NETWORK_SIGNAL_STRENGTH_UPDATED,
			    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, &rssi, sizeof(rssi));
}

/* --- F10 background threads: QRTR 0x302 IMSDCM (VoLTE/VoNR/IMS-SMS) + WMS/VOICE IND reader --- */

struct imsdcm_wds_slot {
	bool active;
	int sock;
	uint32_t handle;
	uint8_t local_id;
	uint32_t seq;
	uint32_t inst_id;
	uint32_t family;
	char ip[64];
};

static int standalone_qmi_rpc(int s, uint32_t node, uint32_t port, uint16_t tx, uint16_t msg,
			      const uint8_t *tlvs, size_t tlen, uint8_t *out, size_t cap,
			      size_t *olen, uint16_t *qerr_out)
{
	if (s < 0 || 7 + tlen > QMI_MAX_BUF)
		return -EINVAL;
	uint8_t req[QMI_MAX_BUF];
	req[0] = 0x00;
	put_le16(req + 1, tx);
	put_le16(req + 3, msg);
	put_le16(req + 5, (uint16_t)tlen);
	if (tlen > 0 && tlvs != NULL)
		memcpy(req + 7, tlvs, tlen);
	struct sockaddr_qrtr dst = { .sq_family = AF_QIPCRTR, .sq_node = node, .sq_port = port };
	if (sendto(s, req, 7 + tlen, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
		return -errno;
	for (int i = 0; i < 12; i++) {
		struct pollfd pfd = { .fd = s, .events = POLLIN };
		int pr = poll(&pfd, 1, 1500);
		if (pr == 0)
			return -ETIMEDOUT;
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		uint8_t rbuf[QMI_MAX_BUF];
		struct sockaddr_qrtr src;
		socklen_t sl = sizeof(src);
		ssize_t n = recvfrom(s, rbuf, sizeof(rbuf), 0, (struct sockaddr *)&src, &sl);
		if (n < 7 || src.sq_port == QRTR_PORT_CTRL)
			continue;
		if (rbuf[0] == 0x02 && get_le16(rbuf + 1) == tx && get_le16(rbuf + 3) == msg) {
			uint16_t plen = get_le16(rbuf + 5);
			if (7 + (size_t)plen > (size_t)n || plen > cap)
				return -EPROTO;
			memcpy(out, rbuf + 7, plen);
			*olen = plen;
			*qerr_out = 0;
			(void)qmi_parse_result(out, plen, qerr_out);
			return 0;
		}
	}
	return -ETIMEDOUT;
}

static int imsdcm_open_wds(uint32_t wds_n, uint32_t wds_p,
			   const struct qmi_ep *ep_dpm, const struct qmi_ep *ep_wda,
			   uint32_t sub_id, const char *apn, uint32_t req_fam, uint16_t prof3gpp,
			   uint8_t mux_id, struct imsdcm_wds_slot *slot_out)
{
	int ws = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (ws < 0)
		return -errno;
	uint8_t q[256], r[QMI_MAX_BUF], b4[4], ep[8];
	size_t ql = 0, rl = 0;
	uint16_t tx = 1, qerr = 0;

	/* Lock MPSS IPA QMAPv5 format (llp=2, ul=5, dl=9) BEFORE any IMS WDS session binds/starts
	 * on ep=4 iface=1; otherwise MPSS latches ul=0,dl=0 and drops all rmnet0 RX packets. */
	if (ep_dpm && ep_dpm->valid) {
		ql = build_dpm_open_port_tlvs(q, MARBLE_EP_TYPE, MARBLE_IFACE_ID,
					      MARBLE_IPA_RX_EP, MARBLE_IPA_TX_EP);
		(void)standalone_qmi_rpc(ws, ep_dpm->node, ep_dpm->port, tx++, QMI_DPM_OPEN_PORT,
					 q, ql, r, sizeof(r), &rl, &qerr);
	}
	if (ep_wda && ep_wda->valid) {
		ql = build_wda_set_data_fmt_tlvs(q, 2u, 5u, 9u, MARBLE_EP_TYPE, MARBLE_IFACE_ID);
		int wrc = standalone_qmi_rpc(ws, ep_wda->node, ep_wda->port, tx++, QMI_WDA_SET_DATA_FORMAT,
					     q, ql, r, sizeof(r), &rl, &qerr);
		ril_trace_op("imsdcm_wda", (int32_t)sub_id, (int)mux_id, 5, 9, 0, wrc, qerr);
	}

	put_le32(b4, sub_id);
	ql = tlv_append(q, 0x01, b4, 4);
	(void)standalone_qmi_rpc(ws, wds_n, wds_p, tx++, QMI_WDS_BIND_SUBSCRIPTION,
				 q, ql, r, sizeof(r), &rl, &qerr);

	put_le32(ep, MARBLE_EP_TYPE);
	put_le32(ep + 4, MARBLE_IFACE_ID);
	ql = 0;
	ql += tlv_append(q + ql, 0x10, ep, 8);
	ql += tlv_append(q + ql, 0x11, &mux_id, 1);
	put_le32(b4, 1u);
	ql += tlv_append(q + ql, 0x13, b4, 4);
	(void)standalone_qmi_rpc(ws, wds_n, wds_p, tx++, QMI_WDS_BIND_MUX_DATA_PORT,
				 q, ql, r, sizeof(r), &rl, &qerr);

	uint8_t try_fams[2] = { (req_fam == 1u) ? 6u : 4u, (req_fam == 1u) ? 4u : 6u };
	for (int fi = 0; fi < 2; fi++) {
		uint8_t ipf = try_fams[fi];
		ql = tlv_append(q, 0x01, &ipf, 1);
		(void)standalone_qmi_rpc(ws, wds_n, wds_p, tx++, QMI_WDS_SET_CLIENT_IP_FAMILY_PREF,
					 q, ql, r, sizeof(r), &rl, &qerr);
		ql = 0;
		if (prof3gpp > 0 && prof3gpp < 255) {
			uint8_t p8 = (uint8_t)prof3gpp;
			ql += tlv_append(q + ql, 0x31, &p8, 1);
		} else if (apn && apn[0]) {
			size_t al = strlen(apn);
			if (al > 64)
				al = 64;
			ql += tlv_append(q + ql, 0x14, apn, (uint16_t)al);
		}
		ql += tlv_append(q + ql, 0x19, &ipf, 1);
		int rc = standalone_qmi_rpc(ws, wds_n, wds_p, tx++, QMI_WDS_START_NETWORK_INTERFACE,
					    q, ql, r, sizeof(r), &rl, &qerr);
		const uint8_t *v = NULL;
		uint16_t vl = 0;
		if (rc == 0 && qerr == 0 && qmi_find_tlv(r, rl, 0x01, &v, &vl) == 0 && vl >= 4) {
			uint32_t h = get_le32(v);
			put_le32(b4, 0x0000ffffu);
			ql = tlv_append(q, 0x10, b4, 4);
			if (standalone_qmi_rpc(ws, wds_n, wds_p, tx++, QMI_WDS_GET_CURRENT_SETTINGS,
					       q, ql, r, sizeof(r), &rl, &qerr) == 0 && qerr == 0) {
				char ipbuf[64] = "";
				uint32_t got_fam = 0;
				if (ipf == 6u && qmi_find_tlv(r, rl, 0x25, &v, &vl) == 0 && vl >= 16) {
					inet_ntop(AF_INET6, v, ipbuf, sizeof(ipbuf));
					got_fam = 1u;
				} else if (ipf == 4u && qmi_find_tlv(r, rl, 0x1e, &v, &vl) == 0 && vl >= 4) {
					format_ipv4_u32(get_le32(v), ipbuf, sizeof(ipbuf));
					got_fam = 0u;
				}
				if (ipbuf[0] != '\0') {
					slot_out->active = true;
					slot_out->sock = ws;
					slot_out->handle = h;
					slot_out->family = got_fam;
					snprintf(slot_out->ip, sizeof(slot_out->ip), "%s", ipbuf);
					return 0;
				}
			}
			put_le32(b4, h);
			ql = tlv_append(q, 0x01, b4, 4);
			(void)standalone_qmi_rpc(ws, wds_n, wds_p, tx++, QMI_WDS_STOP_NETWORK_INTERFACE,
						 q, ql, r, sizeof(r), &rl, &qerr);
		}
	}
	close(ws);
	return -EIO;
}

static void imsdcm_poll_imsa(uint32_t imsa_n, uint32_t imsa_p, uint32_t sub_id)
{
	int s = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return;
	uint8_t q[64], r[512], b4[4];
	size_t ql = 0, rl = 0;
	uint16_t tx = 1, qerr = 0;
	bool reg = false, v_ok = false, s_ok = false;

	put_le32(b4, sub_id);
	ql = tlv_append(q, 0x10, b4, 4);
	if (standalone_qmi_rpc(s, imsa_n, imsa_p, tx++, QMI_IMSA_BIND, q, ql, r, sizeof(r), &rl, &qerr) == 0) {
		if (standalone_qmi_rpc(s, imsa_n, imsa_p, tx++, QMI_IMSA_GET_IMS_REG_STATUS,
				       NULL, 0, r, sizeof(r), &rl, &qerr) == 0 && qerr == 0) {
			const uint8_t *v = NULL;
			uint16_t vl = 0;
			if (qmi_find_tlv(r, rl, 0x12, &v, &vl) == 0 && vl >= 4 && get_le32(v) == 2u)
				reg = true;
		}
		if (reg && standalone_qmi_rpc(s, imsa_n, imsa_p, tx++, QMI_IMSA_GET_IMS_SRV_STATUS,
					      NULL, 0, r, sizeof(r), &rl, &qerr) == 0 && qerr == 0) {
			const uint8_t *v = NULL;
			uint16_t vl = 0;
			if (qmi_find_tlv(r, rl, 0x10, &v, &vl) == 0 && vl >= 4 && get_le32(v) == 2u)
				s_ok = true;
			if (qmi_find_tlv(r, rl, 0x11, &v, &vl) == 0 && vl >= 4 && get_le32(v) == 2u)
				v_ok = true;
		}
	}
	pthread_mutex_lock(&g_state_lock);
	g_ims_registered = reg;
	g_ims_voice_avail = v_ok;
	g_ims_sms_avail = s_ok;
	pthread_mutex_unlock(&g_state_lock);
	close(s);
}

static void imsdcm_set_imss_enable(uint32_t imss_n, uint32_t imss_p,
				   uint32_t sub_id, bool enable)
{
	if (imss_p == 0)
		return;
	int s = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return;
	uint8_t q[64], r[512], b4[4];
	size_t ql = 0, rl = 0;
	uint16_t tx = 1, qerr = 0;
	put_le32(b4, sub_id);
	ql = tlv_append(q, 0x01, b4, 4);
	if (standalone_qmi_rpc(s, imss_n, imss_p, tx++, QMI_IMSS_BIND, q, ql, r, sizeof(r), &rl, &qerr) == 0) {
		uint8_t val = enable ? 1u : 0u;
		ql = 0;
		ql += tlv_append(q + ql, 0x10, &val, 1); /* VoLTE */
		ql += tlv_append(q + ql, 0x18, &val, 1); /* IMS */
		if (enable) {
			ql += tlv_append(q + ql, 0x11, &val, 1); /* VT */
			ql += tlv_append(q + ql, 0x12, &val, 1); /* Mobile data */
			ql += tlv_append(q + ql, 0x1a, &val, 1); /* SMS over IMS */
		}
		(void)standalone_qmi_rpc(s, imss_n, imss_p, tx++, QMI_IMSS_SET_IMS_SERVICES_ENABLED,
					 q, ql, r, sizeof(r), &rl, &qerr);
	}
	close(s);
}

static void *imsdcm_thread(void *arg)
{
	(void)arg;
	/* Wait until modem endpoints are discovered */
	struct qmi_ep ep_wds = {0}, ep_imsa = {0}, ep_imss = {0}, ep_dpm = {0}, ep_wda = {0};
	for (int w = 0; w < 60; w++) {
		pthread_mutex_lock(&g_state_lock);
		(void)qrtr_discover_locked();
		ep_wds = g_qrtr.wds;
		ep_imsa = g_qrtr.imsa;
		ep_imss = g_qrtr.imss;
		ep_dpm = g_qrtr.dpm;
		ep_wda = g_qrtr.wda;
		pthread_mutex_unlock(&g_state_lock);
		if (ep_wds.valid && ep_imsa.valid && ep_wda.valid)
			break;
		sleep(1);
	}
	if (!ep_wds.valid)
		return NULL;

	/* Open QRTR server socket WITHOUT bind() (matches kernel qrtr_ns ctrl_cmd_new_server) */
	int srv = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (srv < 0)
		return NULL;
	struct sockaddr_qrtr me;
	socklen_t slen = sizeof(me);
	if (getsockname(srv, (struct sockaddr *)&me, &slen) < 0) {
		close(srv);
		return NULL;
	}
	struct qrtr_ctrl_pkt cpkt;
	memset(&cpkt, 0, sizeof(cpkt));
	cpkt.cmd = le32_swap_host(QRTR_TYPE_NEW_SERVER);
	cpkt.server.service = le32_swap_host(SVC_IMSDCM);
	cpkt.server.instance = le32_swap_host(1u); /* version=1, instance=0 */
	cpkt.server.node = le32_swap_host(me.sq_node);
	cpkt.server.port = le32_swap_host(me.sq_port);
	struct sockaddr_qrtr ctrl = {
		.sq_family = AF_QIPCRTR,
		.sq_node = me.sq_node,
		.sq_port = QRTR_PORT_CTRL,
	};
	(void)sendto(srv, &cpkt, sizeof(cpkt), 0, (struct sockaddr *)&ctrl, sizeof(ctrl));

	struct imsdcm_wds_slot conns[4];
	memset(conns, 0, sizeof(conns));
	for (int i = 0; i < 4; i++)
		conns[i].sock = -1;

	uint16_t ind_tx = 1;
	time_t start_ts = time(NULL);
	time_t last_check = 0;
	time_t last_kick = 0;
	time_t kick_resume_at = 0;
	int kick_phase = 0;
	int kick_attempts = 0;
	uint32_t kick_sub = 1u;

	for (;;) {
		struct pollfd pfd = { .fd = srv, .events = POLLIN };
		int pr = poll(&pfd, 1, 500);
		if (pr > 0) {
			uint8_t buf[2048];
			struct sockaddr_qrtr peer;
			socklen_t pl = sizeof(peer);
			ssize_t n = recvfrom(srv, buf, sizeof(buf), 0,
					     (struct sockaddr *)&peer, &pl);
			if (n >= 7 && peer.sq_port != QRTR_PORT_CTRL && buf[0] == 0x00) {
				uint16_t qtx = get_le16(buf + 1);
				uint16_t qmsg = get_le16(buf + 3);
				const uint8_t *tlvs = buf + 7;
				size_t tlen = (size_t)n - 7;

				if (qmsg == QMI_IMSDCM_START_CONN) {
					const uint8_t *v = NULL;
					uint16_t vl = 0;
					char apn[96] = "IMS";
					uint32_t req_fam = 1u, seq = 0, sub_id = 1u, inst_id = 1u;
					uint16_t prof3gpp = 2u;
					if (qmi_find_tlv(tlvs, tlen, 0x01, &v, &vl) == 0 && vl >= 1) {
						uint8_t al = v[0];
						if ((size_t)(1 + al) <= vl) {
							size_t cp = al < sizeof(apn) - 1 ? al : sizeof(apn) - 1;
							memcpy(apn, v + 1, cp);
							apn[cp] = '\0';
							size_t o = 1 + al;
							if (o + 14 <= vl) {
								req_fam = get_le32(v + o + 8);
								prof3gpp = get_le16(v + o + 12);
							}
						}
					}
					if (qmi_find_tlv(tlvs, tlen, 0x10, &v, &vl) == 0 && vl >= 4)
						seq = get_le32(v);
					if (qmi_find_tlv(tlvs, tlen, 0x11, &v, &vl) == 0 && vl >= 4)
						sub_id = get_le32(v);
					if (qmi_find_tlv(tlvs, tlen, 0x13, &v, &vl) == 0 && vl >= 4)
						inst_id = get_le32(v);

					int slot_idx = 0;
					for (int i = 0; i < 4; i++) {
						if (!conns[i].active) {
							slot_idx = i;
							break;
						}
					}
					if (conns[slot_idx].active && conns[slot_idx].sock >= 0) {
						uint8_t sq[16], sr[256], h4[4];
						size_t srl = 0;
						uint16_t sqerr = 0;
						put_le32(h4, conns[slot_idx].handle);
						size_t sql = tlv_append(sq, 0x01, h4, 4);
						(void)standalone_qmi_rpc(conns[slot_idx].sock, ep_wds.node, ep_wds.port,
									 900, QMI_WDS_STOP_NETWORK_INTERFACE,
									 sq, sql, sr, sizeof(sr), &srl, &sqerr);
						close(conns[slot_idx].sock);
						conns[slot_idx].sock = -1;
						conns[slot_idx].active = false;
					}
					uint8_t local_id = (uint8_t)slot_idx;
					uint8_t rpkt[256];
					size_t rlen = build_imsdcm_start_resp(rpkt, qtx, local_id, seq, inst_id);
					(void)sendto(srv, rpkt, rlen, 0, (struct sockaddr *)&peer, sizeof(peer));

					pthread_mutex_lock(&g_state_lock);
					int32_t act = resolve_active_slot_locked();
					pthread_mutex_unlock(&g_state_lock);
					uint32_t use_sub = (sub_id <= 1u) ? sub_id : (uint32_t)(act >= 0 ? act : 1);
					int wrc = imsdcm_open_wds(ep_wds.node, ep_wds.port,
								  &ep_dpm, &ep_wda, use_sub,
								  apn, req_fam, prof3gpp,
								  (uint8_t)(MARBLE_IMS_MUX_BASE + slot_idx),
								  &conns[slot_idx]);
					conns[slot_idx].local_id = local_id;
					conns[slot_idx].seq = seq;
					conns[slot_idx].inst_id = inst_id;

					uint8_t ipkt[256];
					size_t ilen = build_imsdcm_conn_ind(ipkt, ind_tx++, local_id, seq, inst_id,
									    conns[slot_idx].family,
									    wrc == 0 ? conns[slot_idx].ip : NULL);
					(void)sendto(srv, ipkt, ilen, 0, (struct sockaddr *)&peer, sizeof(peer));
				} else if (qmsg == QMI_IMSDCM_STOP_CONN) {
					const uint8_t *v = NULL;
					uint16_t vl = 0;
					uint8_t lid = 0xffu;
					uint32_t echo = 0;
					if (qmi_find_tlv(tlvs, tlen, 0x01, &v, &vl) == 0 && vl >= 1)
						lid = v[0];
					if (qmi_find_tlv(tlvs, tlen, 0x10, &v, &vl) == 0 && vl >= 4)
						echo = get_le32(v);
					bool found = false;
					uint32_t stop_seq = 0, stop_inst = 0, stop_fam = 1u;
					for (int i = 0; i < 4; i++) {
						if (conns[i].active && conns[i].local_id == lid) {
							found = true;
							stop_seq = conns[i].seq;
							stop_inst = conns[i].inst_id;
							stop_fam = conns[i].family;
							uint8_t sq[16], sr[256], h4[4];
							size_t srl = 0;
							uint16_t sqerr = 0;
							put_le32(h4, conns[i].handle);
							size_t sql = tlv_append(sq, 0x01, h4, 4);
							(void)standalone_qmi_rpc(conns[i].sock, ep_wds.node, ep_wds.port,
										 901, QMI_WDS_STOP_NETWORK_INTERFACE,
										 sq, sql, sr, sizeof(sr), &srl, &sqerr);
							close(conns[i].sock);
							conns[i].sock = -1;
							conns[i].active = false;
							break;
						}
					}
					if (found) {
						/* Match 81voltd: emit IMSD_CONNECTION_CHANGED IND (0x0020, err_code=13) + 0x0021 RESP */
						uint8_t ipkt[128];
						size_t ilen = build_imsdcm_conn_ind(ipkt, ind_tx++, lid,
										    stop_seq, stop_inst, stop_fam, NULL);
						(void)sendto(srv, ipkt, ilen, 0, (struct sockaddr *)&peer, sizeof(peer));
						uint8_t rpkt[64];
						size_t ro = 7;
						uint8_t ok4[4] = { 0, 0, 0, 0 }, fa = 0xfau, e4[4];
						ro += tlv_append(rpkt + ro, 0x02, ok4, 4);
						ro += tlv_append(rpkt + ro, 0x10, &fa, 1);
						put_le32(e4, echo);
						ro += tlv_append(rpkt + ro, 0x11, e4, 4);
						rpkt[0] = 0x02;
						put_le16(rpkt + 1, qtx);
						put_le16(rpkt + 3, qmsg);
						put_le16(rpkt + 5, (uint16_t)(ro - 7));
						(void)sendto(srv, rpkt, ro, 0, (struct sockaddr *)&peer, sizeof(peer));
					} else {
						uint8_t rpkt[32];
						size_t ro = 7;
						uint8_t err4[4] = { 1, 0, 9, 0 };
						ro += tlv_append(rpkt + ro, 0x02, err4, 4);
						rpkt[0] = 0x02;
						put_le16(rpkt + 1, qtx);
						put_le16(rpkt + 3, qmsg);
						put_le16(rpkt + 5, (uint16_t)(ro - 7));
						(void)sendto(srv, rpkt, ro, 0, (struct sockaddr *)&peer, sizeof(peer));
					}
				} else {
					/* Handshake / auxiliary IMSDCM commands -> reply OK */
					uint8_t rpkt[32];
					size_t ro = 7;
					uint8_t ok4[4] = { 0, 0, 0, 0 };
					ro += tlv_append(rpkt + ro, 0x02, ok4, 4);
					rpkt[0] = 0x02;
					put_le16(rpkt + 1, qtx);
					put_le16(rpkt + 3, qmsg);
					put_le16(rpkt + 5, (uint16_t)(ro - 7));
					(void)sendto(srv, rpkt, ro, 0, (struct sockaddr *)&peer, sizeof(peer));
				}
			}
		}

		time_t now = time(NULL);
		if (kick_phase == 1 && now >= kick_resume_at) {
			/* Phase 2: re-enable VoLTE + VT + IMS + SMS after STOP_CONN IND has drained */
			imsdcm_set_imss_enable(ep_imss.node, ep_imss.port, kick_sub, true);
			kick_phase = 0;
			last_check = now;
		}
		if (ep_imsa.valid && kick_phase == 0 && now - last_check >= 6) {
			last_check = now;
			pthread_mutex_lock(&g_state_lock);
			int32_t act = resolve_active_slot_locked();
			bool online = g_radio_on_notified;
			pthread_mutex_unlock(&g_state_lock);
			if (online && act >= 0) {
				imsdcm_poll_imsa(ep_imsa.node, ep_imsa.port, (uint32_t)act);
				pthread_mutex_lock(&g_state_lock);
				bool ims_ready = (g_ims_registered && g_ims_voice_avail && g_ims_sms_avail);
				pthread_mutex_unlock(&g_state_lock);
				if (!ims_ready && ep_imss.valid &&
				    (now - start_ts >= 6) && kick_attempts < 5 &&
				    (last_kick == 0 || now - last_kick >= 20)) {
					kick_sub = (uint32_t)act;
					last_kick = now;
					kick_attempts++;
					imsdcm_set_imss_enable(ep_imss.node, ep_imss.port, kick_sub, false);
					kick_resume_at = now + 2;
					kick_phase = 1;
				}
			}
		}
	}
	return NULL;
}

static void *ind_event_thread(void *arg)
{
	(void)arg;
	struct qmi_ep ep_wms = {0}, ep_voice = {0};
	for (int w = 0; w < 60; w++) {
		pthread_mutex_lock(&g_state_lock);
		(void)qrtr_discover_locked();
		ep_wms = g_qrtr.wms;
		ep_voice = g_qrtr.voice;
		pthread_mutex_unlock(&g_state_lock);
		if (ep_wms.valid && ep_voice.valid)
			break;
		sleep(1);
	}
	int sw[2] = { -1, -1 };
	int sv[2] = { -1, -1 };
	uint8_t q[64], r[QMI_MAX_BUF];
	size_t ql = 0, rl = 0;
	uint16_t qerr = 0, rtx = 1;
	int opened = 0;

	for (int s = 0; s < 2; s++) {
		uint8_t sub = (uint8_t)s;
		uint8_t on = 1u;
		if (ep_wms.valid) {
			sw[s] = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
			if (sw[s] >= 0) {
				opened++;
				ql = tlv_append(q, 0x01, &sub, 1);
				(void)standalone_qmi_rpc(sw[s], ep_wms.node, ep_wms.port, rtx++,
							 QMI_WMS_BIND_SUBSCRIPTION,
							 q, ql, r, sizeof(r), &rl, &qerr);
				ql = tlv_append(q, 0x10, &on, 1);
				(void)standalone_qmi_rpc(sw[s], ep_wms.node, ep_wms.port, rtx++,
							 QMI_WMS_SET_EVENT_REPORT,
							 q, ql, r, sizeof(r), &rl, &qerr);
			}
		}
		if (ep_voice.valid) {
			sv[s] = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
			if (sv[s] >= 0) {
				opened++;
				ql = tlv_append(q, 0x01, &sub, 1);
				(void)standalone_qmi_rpc(sv[s], ep_voice.node, ep_voice.port, rtx++,
							 QMI_VOICE_BIND_SUBSCRIPTION,
							 q, ql, r, sizeof(r), &rl, &qerr);
				ql = 0;
				ql += tlv_append(q + ql, 0x10, &on, 1); /* DTMF */
				ql += tlv_append(q + ql, 0x12, &on, 1); /* SS */
				ql += tlv_append(q + ql, 0x13, &on, 1); /* Call notif */
				ql += tlv_append(q + ql, 0x14, &on, 1); /* Handover */
				ql += tlv_append(q + ql, 0x15, &on, 1); /* Speech codec */
				(void)standalone_qmi_rpc(sv[s], ep_voice.node, ep_voice.port, rtx++,
							 QMI_VOICE_INDICATION_REGISTER,
							 q, ql, r, sizeof(r), &rl, &qerr);
			}
		}
	}
	if (opened == 0)
		return NULL;

	for (;;) {
		struct pollfd pfds[4];
		int pslot[4];
		int pkind[4]; /* 0 = WMS, 1 = VOICE */
		nfds_t nf = 0;
		for (int s = 0; s < 2; s++) {
			if (sw[s] >= 0) {
				pfds[nf].fd = sw[s];
				pfds[nf].events = POLLIN;
				pfds[nf].revents = 0;
				pslot[nf] = s;
				pkind[nf] = 0;
				nf++;
			}
			if (sv[s] >= 0) {
				pfds[nf].fd = sv[s];
				pfds[nf].events = POLLIN;
				pfds[nf].revents = 0;
				pslot[nf] = s;
				pkind[nf] = 1;
				nf++;
			}
		}
		bool need_watch = false;
		int32_t watch_slot = -1;
		pthread_mutex_lock(&g_state_lock);
		if (g_cached_call_cnt > 0 || g_hangup_pending || g_call_watchdog_ticks > 0) {
			need_watch = true;
			watch_slot = (g_hangup_slot == 0 || g_hangup_slot == 1)
				? g_hangup_slot : resolve_active_slot_locked();
		}
		pthread_mutex_unlock(&g_state_lock);

		int pr = poll(pfds, nf, need_watch ? 350 : 5000);
		if (pr == 0) {
			/* Watchdog tick: poll QMI_VOICE_GET_ALL_CALL_INFO while calls are active or
			 * hanging up so HNOTI_CALL_STATE_UPDATED is guaranteed even if SIP BYE IND drops. */
			const HRilReport *rep = g_report_ops;
			if (need_watch && ep_voice.valid && watch_slot >= 0 && watch_slot <= 1 &&
			    sv[watch_slot] >= 0 && rep && rep->OnCallReport) {
				uint8_t vresp[QMI_MAX_BUF];
				size_t vrl = 0;
				uint16_t vqerr = 0;
				if (standalone_qmi_rpc(sv[watch_slot], ep_voice.node, ep_voice.port, rtx++,
						       QMI_VOICE_GET_ALL_CALL_INFO, NULL, 0,
						       vresp, sizeof(vresp), &vrl, &vqerr) == 0 && vqerr == 0) {
					struct cached_call_entry all_entries[MAX_VOICE_CALLS];
					struct cached_call_entry entries[MAX_VOICE_CALLS];
					size_t nc_all = parse_voice_call_list(vresp, vrl, false, all_entries, MAX_VOICE_CALLS);
					size_t ims_cnt = 0;
					size_t nc = filter_cs_call_entries(all_entries, nc_all,
									   ims_call_bridge_active(),
									   entries, MAX_VOICE_CALLS, &ims_cnt);
					bool changed = false;
					pthread_mutex_lock(&g_state_lock);
					if (nc != g_cached_call_cnt) {
						changed = true;
					} else {
						for (size_t k = 0; k < nc; k++) {
							if (entries[k].index != g_cached_calls[k].index ||
							    entries[k].state != g_cached_calls[k].state) {
								changed = true;
								break;
							}
						}
					}
					if (nc == 0 && g_hangup_pending)
						changed = true;
					g_cached_call_cnt = nc;
					if (nc > 0)
						memcpy(g_cached_calls, entries, nc * sizeof(entries[0]));
					if (nc == 0) {
						g_hangup_pending = false;
						g_call_watchdog_ticks = 0;
					} else if (g_call_watchdog_ticks > 0) {
						g_call_watchdog_ticks--;
					}
					pthread_mutex_unlock(&g_state_lock);
					if (changed) {
						emit_report(rep->OnCallReport, watch_slot, NULL,
							    HNOTI_CALL_STATE_UPDATED,
							    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, NULL, 0);
					}
				} else {
					pthread_mutex_lock(&g_state_lock);
					if (g_call_watchdog_ticks > 0)
						g_call_watchdog_ticks--;
					if (g_call_watchdog_ticks == 0)
						g_hangup_pending = false;
					pthread_mutex_unlock(&g_state_lock);
				}
			}
			continue;
		}
		if (pr < 0)
			continue;
		for (nfds_t i = 0; i < nf; i++) {
			if ((pfds[i].revents & POLLIN) == 0)
				continue;
			uint8_t buf[QMI_MAX_BUF];
			struct sockaddr_qrtr src;
			socklen_t sl = sizeof(src);
			ssize_t n = recvfrom(pfds[i].fd, buf, sizeof(buf), 0,
					     (struct sockaddr *)&src, &sl);
			if (n < 7 || src.sq_port == QRTR_PORT_CTRL || buf[0] != 0x04)
				continue;
			uint16_t msg = get_le16(buf + 3);
			uint16_t plen = get_le16(buf + 5);
			if (7 + (size_t)plen > (size_t)n)
				continue;
			const uint8_t *tlvs = buf + 7;
			const HRilReport *rep = g_report_ops;
			int32_t ind_slot = pslot[i];

			if (pkind[i] == 0 && msg == QMI_WMS_EVENT_REPORT_IND && rep && rep->OnSmsReport) {
				uint32_t tx_id = 0;
				bool on_ims = false;
				char hex_pdu[700];
				bool got = parse_wms_event_report_ind(tlvs, plen, &tx_id, &on_ims,
								      hex_pdu, sizeof(hex_pdu));
				if (!got) {
					/* Fallback: stored MT SMS TLV 0x10 {storage(1), index(4)} -> RAW_READ + DELETE */
					const uint8_t *mv = NULL;
					uint16_t ml = 0;
					if (qmi_find_tlv(tlvs, plen, 0x10, &mv, &ml) == 0 && ml >= 5) {
						uint8_t rreq[16], rresp[QMI_MAX_BUF];
						size_t rql = tlv_append(rreq, 0x01, mv, 5);
						size_t rrl = 0;
						uint16_t rqerr = 0;
						if (standalone_qmi_rpc(pfds[i].fd, ep_wms.node, ep_wms.port, rtx++,
								       QMI_WMS_RAW_READ, rreq, rql,
								       rresp, sizeof(rresp), &rrl, &rqerr) == 0 && rqerr == 0) {
							const uint8_t *rv = NULL;
							uint16_t rvl = 0;
							if (qmi_find_tlv(rresp, rrl, 0x01, &rv, &rvl) == 0 && rvl >= 4) {
								uint16_t pdul = get_le16(rv + 2);
								if ((size_t)(4 + pdul) <= rvl && pdul > 0 && (size_t)pdul * 2 < sizeof(hex_pdu)) {
									encode_hex_upper(rv + 4, pdul, hex_pdu, sizeof(hex_pdu));
									got = true;
								}
							}
						}
						uint8_t dreq[16];
						size_t dql = tlv_append(dreq, 0x01, mv, 1);
						dql += tlv_append(dreq + dql, 0x10, mv + 1, 4);
						(void)standalone_qmi_rpc(pfds[i].fd, ep_wms.node, ep_wms.port, rtx++,
									 QMI_WMS_DELETE, dreq, dql,
									 rresp, sizeof(rresp), &rrl, &rqerr);
					}
				} else {
					pthread_mutex_lock(&g_state_lock);
					g_last_mt_sms_tx_id = tx_id;
					g_last_mt_sms_on_ims = on_ims;
					g_has_mt_sms_ack = true;
					pthread_mutex_unlock(&g_state_lock);
				}
				if (got) {
					HRilSmsResponse snoti;
					memset(&snoti, 0, sizeof(snoti));
					snoti.pdu = hex_pdu;
					emit_report(rep->OnSmsReport, ind_slot, NULL, HNOTI_SMS_NEW_SMS,
						    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS,
						    &snoti, sizeof(snoti));
				}
			} else if (pkind[i] == 1 && msg == QMI_VOICE_ALL_CALL_STATUS_IND && rep && rep->OnCallReport) {
				struct cached_call_entry all_entries[MAX_VOICE_CALLS];
				struct cached_call_entry entries[MAX_VOICE_CALLS];
				size_t nc_all = parse_voice_call_list(tlvs, plen, true, all_entries, MAX_VOICE_CALLS);
				bool ims_bridge = ims_call_bridge_active();
				size_t ims_cnt = 0;
				size_t nc = filter_cs_call_entries(all_entries, nc_all, ims_bridge,
								   entries, MAX_VOICE_CALLS, &ims_cnt);
				int32_t end_reason = parse_voice_call_end_reason(tlvs, plen);
				size_t prev_cs = 0;
				bool was_hangup = false;
				pthread_mutex_lock(&g_state_lock);
				prev_cs = g_cached_call_cnt;
				was_hangup = g_hangup_pending;
				g_cached_call_cnt = nc;
				if (nc > 0) {
					memcpy(g_cached_calls, entries, nc * sizeof(entries[0]));
					g_hangup_slot = ind_slot;
				} else {
					g_last_call_fail_reason = end_reason;
					g_hangup_pending = false;
					g_call_watchdog_ticks = 0;
				}
				pthread_mutex_unlock(&g_state_lock);
				/* When SA 4014 ImsCallStub is active and only IP/IMS calls exist
				 * (with no active/ending CS call), skip CS HNOTI_CALL_STATE_UPDATED
				 * to prevent duplicate incoming call screens. */
				if (!ims_bridge || nc > 0 || prev_cs > 0 || was_hangup) {
					emit_report(rep->OnCallReport, ind_slot, NULL, HNOTI_CALL_STATE_UPDATED,
						    HRIL_REPORT_NOTIFICATION, HRIL_ERR_SUCCESS, NULL, 0);
				}
			}
		}
	}
	return NULL;
}
#endif

static void *worker_loop(void *arg)
{
	(void)arg;
	bool first_poll = true;
	for (;;) {
		struct ril_task task;
		bool have_task = false;
		pthread_mutex_lock(&g_queue_lock);
#ifdef HRIL_VENDOR_HOST_TEST
		(void)first_poll;
		while (g_q_count == 0)
			pthread_cond_wait(&g_queue_cond, &g_queue_lock);
#else
		while (g_q_count == 0) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += first_poll ? 1 : 5;
			int wrc = pthread_cond_timedwait(&g_queue_cond, &g_queue_lock, &ts);
			if (wrc == ETIMEDOUT && g_q_count == 0)
				break;
		}
		first_poll = false;
#endif
		if (g_q_count > 0) {
			task = g_queue[g_q_head];
			g_q_head = (g_q_head + 1) % WORK_QUEUE_CAP;
			g_q_count--;
			have_task = true;
		}
		pthread_mutex_unlock(&g_queue_lock);
		if (have_task)
			handle_task(&task);
#ifndef HRIL_VENDOR_HOST_TEST
		else
			poll_nas_delta_tick();
#endif
	}
	return NULL;
}

static void enqueue_task(const struct ril_task *task)
{
	pthread_mutex_lock(&g_queue_lock);
	if (g_q_count < WORK_QUEUE_CAP) {
		g_queue[g_q_tail] = *task;
		g_q_tail = (g_q_tail + 1) % WORK_QUEUE_CAP;
		g_q_count++;
		pthread_cond_signal(&g_queue_cond);
		pthread_mutex_unlock(&g_queue_lock);
		return;
	}
	pthread_mutex_unlock(&g_queue_lock);
	/* Queue full fallback: fail request gracefully with copied ReqDataInfo untouched on caller errors */
	handle_task(task);
}

static void queue_simple(enum ril_task_kind kind, const ReqDataInfo *req, int32_t arg0, int32_t arg1)
{
	struct ril_task t;
	memset(&t, 0, sizeof(t));
	t.kind = kind;
	t.req = req;
	t.slot_id = req ? req->slotId : 0;
	t.arg0 = arg0;
	t.arg1 = arg1;
	enqueue_task(&t);
}

/* --- Vendor ops entry points --- */

static void v_SetRadioState(const ReqDataInfo *req, int32_t fun, int32_t rst)
{
	queue_simple(TASK_MODEM_SET_RADIO_STATE, req, fun, rst);
}
static void v_GetRadioState(const ReqDataInfo *req) { queue_simple(TASK_MODEM_GET_RADIO_STATE, req, 0, 0); }
static void v_GetImei(const ReqDataInfo *req) { queue_simple(TASK_MODEM_GET_IMEI, req, 0, 0); }
static void v_GetImeiSv(const ReqDataInfo *req) { queue_simple(TASK_MODEM_GET_IMEISV, req, 0, 0); }
static void v_GetMeid(const ReqDataInfo *req) { queue_simple(TASK_MODEM_GET_MEID, req, 0, 0); }
static void v_GetVoiceRadioTechnology(const ReqDataInfo *req) { queue_simple(TASK_MODEM_GET_VOICE_TECH, req, 0, 0); }
static void v_GetBasebandVersion(const ReqDataInfo *req) { queue_simple(TASK_MODEM_GET_BASEBAND, req, 0, 0); }
static void v_ShutDown(const ReqDataInfo *req) { queue_simple(TASK_MODEM_SHUTDOWN, req, 0, 0); }

static void v_GetSimStatus(const ReqDataInfo *req) { queue_simple(TASK_SIM_GET_STATUS, req, 0, 0); }
static void v_GetSimIO(const ReqDataInfo *req, const HRilSimIO *io, size_t len)
{
	struct ril_task t;
	memset(&t, 0, sizeof(t));
	t.kind = TASK_SIM_GET_IO;
	t.req = req;
	t.slot_id = req ? req->slotId : 0;
	if (io != NULL && len >= sizeof(*io)) {
		t.sim_io = *io;
		if (io->pathid != NULL)
			snprintf(t.sim_path, sizeof(t.sim_path), "%s", io->pathid);
		t.sim_io.data = NULL;
		t.sim_io.pathid = NULL;
		t.sim_io.pin2 = NULL;
	}
	enqueue_task(&t);
}
static void v_GetSimImsi(const ReqDataInfo *req) { queue_simple(TASK_SIM_GET_IMSI, req, 0, 0); }
static void v_GetSimLockStatus(const ReqDataInfo *req, const void *d, size_t l)
{
	(void)d; (void)l;
	queue_simple(TASK_SIM_GET_LOCK_STATUS, req, 0, 0);
}
static void v_SetActiveSim(const ReqDataInfo *req, int32_t index, int32_t enable)
{
	queue_simple(TASK_SIM_SET_ACTIVE, req, index, enable);
}
static void v_SimStkIsReady(const ReqDataInfo *req) { queue_simple(TASK_SIM_STK_READY, req, 0, 0); }
static void v_GetRadioProtocol(const ReqDataInfo *req) { queue_simple(TASK_SIM_GET_PROTOCOL, req, 0, 0); }
static void v_SetRadioProtocol(const ReqDataInfo *req, const HRilRadioProtocol *data)
{
	struct ril_task t;
	memset(&t, 0, sizeof(t));
	t.kind = TASK_SIM_SET_PROTOCOL;
	t.req = req;
	t.slot_id = req ? req->slotId : 0;
	if (data != NULL) {
		t.radio_proto = *data;
		t.has_data_info = true;
	}
	enqueue_task(&t);
}
static void v_SendSimMatchedOperatorInfo(const ReqDataInfo *req, const void *d)
{
	(void)d;
	queue_simple(TASK_SIM_SEND_MATCHED_OP, req, 0, 0);
}
static void v_GetPrimarySlot(const ReqDataInfo *req) { queue_simple(TASK_SIM_GET_PRIMARY, req, 0, 0); }
static void v_SetPrimarySlot(const ReqDataInfo *req, int32_t slotId)
{
	queue_simple(TASK_SIM_SET_PRIMARY, req, slotId, 0);
}

static void v_GetSignalStrength(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_SIGNAL, req, 0, 0); }
static void v_GetCsRegStatus(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_CS_REG, req, 0, 0); }
static void v_GetPsRegStatus(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_PS_REG, req, 0, 0); }
static void v_GetOperatorInfo(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_OPERATOR, req, 0, 0); }
static void v_GetNeighboringCellInfoList(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_NEIGHBOR_CELL, req, 0, 0); }
static void v_GetCurrentCellInfo(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_CURRENT_CELL, req, 0, 0); }
static void v_GetNetworkSearchInformation(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_SEARCH, req, 0, 0); }
static void v_GetNetworkSelectionMode(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_SEL_MODE, req, 0, 0); }
static void v_SetNetworkSelectionMode(const ReqDataInfo *req, const void *d)
{
	(void)d;
	queue_simple(TASK_NET_SET_SEL_MODE, req, 0, 0);
}
static void v_GetPreferredNetwork(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_PREF_NET, req, 0, 0); }
static void v_SetPreferredNetwork(const ReqDataInfo *req, const int32_t *pref)
{
	queue_simple(TASK_NET_SET_PREF_NET, req, pref ? *pref : 0, 0);
}
static void v_GetPhysicalChannelConfig(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_PHY_CHNL, req, 0, 0); }
static void v_SetNotificationFilter(const ReqDataInfo *req, const int32_t *filter)
{
	queue_simple(TASK_NET_SET_FILTER, req, filter ? *filter : 0, 0);
}
static void v_SetDeviceState(const ReqDataInfo *req, const int32_t *type, const int32_t *on)
{
	queue_simple(TASK_NET_SET_DEV_STATE, req, type ? *type : 0, on ? *on : 0);
}
static void v_GetRrcConnectionState(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_RRC, req, 0, 0); }
static void v_SetNrOptionMode(const ReqDataInfo *req, const int32_t *mode)
{
	queue_simple(TASK_NET_SET_NR_OPT, req, mode ? *mode : 3, 0);
}
static void v_GetNrOptionMode(const ReqDataInfo *req) { queue_simple(TASK_NET_GET_NR_OPT, req, 0, 0); }

static void queue_data_info(enum ril_task_kind kind, const ReqDataInfo *req, const void *d)
{
	struct ril_task t;
	memset(&t, 0, sizeof(t));
	t.kind = kind;
	t.req = req;
	t.slot_id = req ? req->slotId : 0;
	if (d != NULL) {
		const HRilDataInfo *di = (const HRilDataInfo *)d;
		t.has_data_info = true;
		t.arg0 = di->cid;
		t.arg1 = di->reason;
		if (di->apn != NULL)
			snprintf(t.apn, sizeof(t.apn), "%s", di->apn);
		if (di->type != NULL)
			snprintf(t.pdp_type, sizeof(t.pdp_type), "%s", di->type);
	}
	enqueue_task(&t);
}

static void v_SetInitApnInfo(const ReqDataInfo *req, const void *d) { queue_data_info(TASK_DATA_SET_APN, req, d); }
static void v_ActivatePdpContext(const ReqDataInfo *req, const void *d) { queue_data_info(TASK_DATA_ACTIVATE_PDP, req, d); }
static void v_DeactivatePdpContext(const ReqDataInfo *req, const void *d) { queue_data_info(TASK_DATA_DEACTIVATE_PDP, req, d); }
static void v_GetPdpContextList(const ReqDataInfo *req) { queue_simple(TASK_DATA_GET_PDP_LIST, req, 0, 0); }
static void v_GetLinkBandwidthInfo(const ReqDataInfo *req, int32_t cid) { queue_simple(TASK_DATA_GET_LINK_BW, req, cid, 0); }
static void v_SetLinkBandwidthReportingRule(const ReqDataInfo *req, const void *d)
{
	(void)d;
	queue_simple(TASK_DATA_SET_LINK_BW, req, 0, 0);
}
static void v_SetDataProfileInfo(const ReqDataInfo *req, const void *d, int32_t size)
{
	(void)d; (void)size;
	queue_simple(TASK_DATA_SET_PROFILE, req, 0, 0);
}
static void v_SendDataPerformanceMode(const ReqDataInfo *req, const void *d)
{
	(void)d;
	queue_simple(TASK_DATA_SET_PERF_MODE, req, 0, 0);
}
static void v_SendDataSleepMode(const ReqDataInfo *req, const void *d)
{
	(void)d;
	queue_simple(TASK_DATA_SET_SLEEP_MODE, req, 0, 0);
}
static void v_SetDataPermitted(const ReqDataInfo *req, int32_t enabled) { queue_simple(TASK_DATA_SET_PERMITTED, req, enabled, 0); }
static void v_GetLinkCapability(const ReqDataInfo *req) { queue_simple(TASK_DATA_GET_LINK_CAP, req, 0, 0); }
static void v_CleanAllConnections(const ReqDataInfo *req) { queue_simple(TASK_DATA_CLEAN_ALL, req, 0, 0); }

static void v_GetCallList(const ReqDataInfo *req) { queue_simple(TASK_CALL_GET_LIST, req, 0, 0); }
static void v_Dial(const ReqDataInfo *req, const void *d, size_t l)
{
	struct ril_task t;
	memset(&t, 0, sizeof(t));
	t.kind = TASK_CALL_DIAL;
	t.req = req;
	t.slot_id = req ? req->slotId : 0;
	if (d != NULL && l >= sizeof(HRilDial)) {
		const HRilDial *dial = (const HRilDial *)d;
		if (dial->address != NULL && dial->address[0] != '\0') {
			snprintf(t.str0, sizeof(t.str0), "%s", dial->address);
			t.arg0 = dial->clir;
			t.has_data_info = true;
		}
	}
	enqueue_task(&t);
}
static void v_Hangup(const ReqDataInfo *req, const int32_t *d, size_t l)
{
	int32_t cid = (d != NULL && l >= sizeof(int32_t)) ? *d : 1;
	queue_simple(TASK_CALL_HANGUP, req, cid, 0);
}
static void v_Reject(const ReqDataInfo *req) { queue_simple(TASK_CALL_REJECT, req, 0, 0); }
static void v_Answer(const ReqDataInfo *req) { queue_simple(TASK_CALL_ANSWER, req, 0, 0); }
static void v_HoldCall(const ReqDataInfo *req) { queue_simple(TASK_CALL_HOLD, req, 0, 0); }
static void v_UnHoldCall(const ReqDataInfo *req) { queue_simple(TASK_CALL_UNHOLD, req, 0, 0); }
static void v_SwitchCall(const ReqDataInfo *req) { queue_simple(TASK_CALL_SWITCH, req, 0, 0); }
static void v_CallSupplement(const ReqDataInfo *req, int32_t type)
{
	queue_simple(TASK_CALL_SUPPLEMENT, req, type, 0);
}
static void v_GetCallFailReason(const ReqDataInfo *req)
{
	queue_simple(TASK_CALL_GET_FAIL_REASON, req, 0, 0);
}

static void queue_dtmf_info(enum ril_task_kind kind, const ReqDataInfo *req, const HRilCallDtmfInfo *d)
{
	struct ril_task t;
	memset(&t, 0, sizeof(t));
	t.kind = kind;
	t.req = req;
	t.slot_id = req ? req->slotId : 0;
	if (d != NULL) {
		t.arg0 = d->callId;
		if (d->dtmfKey != NULL)
			snprintf(t.str0, sizeof(t.str0), "%s", d->dtmfKey);
	}
	enqueue_task(&t);
}
static void v_StartDtmf(const ReqDataInfo *req, const HRilCallDtmfInfo *d) { queue_dtmf_info(TASK_CALL_START_DTMF, req, d); }
static void v_SendDtmf(const ReqDataInfo *req, const HRilCallDtmfInfo *d) { queue_dtmf_info(TASK_CALL_SEND_DTMF, req, d); }
static void v_StopDtmf(const ReqDataInfo *req, const HRilCallDtmfInfo *d) { queue_dtmf_info(TASK_CALL_STOP_DTMF, req, d); }

static void v_SendGsmSms(const ReqDataInfo *req, const void *d, size_t l)
{
	struct ril_task t;
	memset(&t, 0, sizeof(t));
	t.kind = TASK_SMS_SEND_GSM;
	t.req = req;
	t.slot_id = req ? req->slotId : 0;
	/* HRilSms::RequestWithStrings passes argv (const char *const *) and count (l == 2) */
	if (d != NULL && l >= 2) {
		const char *const *argv = (const char *const *)d;
		if (argv[0] != NULL)
			snprintf(t.str0, sizeof(t.str0), "%s", argv[0]);
		if (argv[1] != NULL && argv[1][0] != '\0') {
			snprintf(t.str1, sizeof(t.str1), "%s", argv[1]);
			t.has_data_info = true;
		}
	}
	enqueue_task(&t);
}
static void v_SendSmsAck(const ReqDataInfo *req, const int32_t *d, size_t l)
{
	int32_t ok = (d != NULL && l >= sizeof(int32_t)) ? d[0] : 1;
	int32_t cause = (d != NULL && l >= 2 * sizeof(int32_t)) ? d[1] : 0;
	queue_simple(TASK_SMS_SEND_ACK, req, ok, cause);
}
static void v_SendCdmaSms(const ReqDataInfo *req, const void *d, size_t l)
{
	struct ril_task t;
	memset(&t, 0, sizeof(t));
	t.kind = TASK_SMS_SEND_CDMA;
	t.req = req;
	t.slot_id = req ? req->slotId : 0;
	/* HRilSms::SendCdmaSms passes x1 = const char *pdu (hex string), x2 = strlen(pdu). */
	if (d != NULL && l > 0) {
		const char *s = (const char *)d;
		size_t cp = l < sizeof(t.str1) - 1 ? l : sizeof(t.str1) - 1;
		memcpy(t.str1, s, cp);
		t.str1[cp] = '\0';
		if (t.str1[0] != '\0')
			t.has_data_info = true;
	}
	enqueue_task(&t);
}
static void v_SetSmscAddr(const ReqDataInfo *req, const HRilServiceCenterAddress *d, size_t l)
{
	struct ril_task t;
	memset(&t, 0, sizeof(t));
	t.kind = TASK_SMS_SET_SMSC;
	t.req = req;
	t.slot_id = req ? req->slotId : 0;
	if (d != NULL && l >= sizeof(*d)) {
		t.arg0 = d->tosca;
		if (d->address != NULL)
			snprintf(t.str0, sizeof(t.str0), "%s", d->address);
	}
	enqueue_task(&t);
}
static void v_GetSmscAddr(const ReqDataInfo *req) { queue_simple(TASK_SMS_GET_SMSC, req, 0, 0); }

static const HRilCallReq g_call_ops = {
	.GetCallList = v_GetCallList,
	.Dial = v_Dial,
	.Hangup = v_Hangup,
	.Reject = v_Reject,
	.Answer = v_Answer,
	.HoldCall = v_HoldCall,
	.UnHoldCall = v_UnHoldCall,
	.SwitchCall = v_SwitchCall,
	.CallSupplement = v_CallSupplement,
	.StartDtmf = v_StartDtmf,
	.SendDtmf = v_SendDtmf,
	.StopDtmf = v_StopDtmf,
	.GetCallFailReason = v_GetCallFailReason,
};

static const HRilSimReq g_sim_ops = {
	.GetSimStatus = v_GetSimStatus,
	.GetSimIO = v_GetSimIO,
	.GetSimImsi = v_GetSimImsi,
	.GetSimLockStatus = v_GetSimLockStatus,
	.SetActiveSim = v_SetActiveSim,
	.SimStkIsReady = v_SimStkIsReady,
	.GetRadioProtocol = v_GetRadioProtocol,
	.SetRadioProtocol = v_SetRadioProtocol,
	.SendSimMatchedOperatorInfo = v_SendSimMatchedOperatorInfo,
	.GetPrimarySlot = v_GetPrimarySlot,
	.SetPrimarySlot = v_SetPrimarySlot,
};

static const HRilSmsReq g_sms_ops = {
	.SendGsmSms = v_SendGsmSms,
	.SendSmsAck = v_SendSmsAck,
	.SendCdmaSms = v_SendCdmaSms,
	.SetSmscAddr = v_SetSmscAddr,
	.GetSmscAddr = v_GetSmscAddr,
};

static const HRilDataReq g_data_ops = {
	.SetInitApnInfo = v_SetInitApnInfo,
	.ActivatePdpContext = v_ActivatePdpContext,
	.DeactivatePdpContext = v_DeactivatePdpContext,
	.GetPdpContextList = v_GetPdpContextList,
	.GetLinkBandwidthInfo = v_GetLinkBandwidthInfo,
	.SetLinkBandwidthReportingRule = v_SetLinkBandwidthReportingRule,
	.SetDataProfileInfo = v_SetDataProfileInfo,
	.SendDataPerformanceMode = v_SendDataPerformanceMode,
	.SendDataSleepMode = v_SendDataSleepMode,
	.SetDataPermitted = v_SetDataPermitted,
	.GetLinkCapability = v_GetLinkCapability,
	.CleanAllConnections = v_CleanAllConnections,
	.ActivatePdpContextWithApnTypes = v_ActivatePdpContext,
	.ActivatePdpContextWithApnTypesforSlice = v_ActivatePdpContext,
};

static const HRilNetworkReq g_network_ops = {
	.GetSignalStrength = v_GetSignalStrength,
	.GetCsRegStatus = v_GetCsRegStatus,
	.GetPsRegStatus = v_GetPsRegStatus,
	.GetOperatorInfo = v_GetOperatorInfo,
	.GetNeighboringCellInfoList = v_GetNeighboringCellInfoList,
	.GetCurrentCellInfo = v_GetCurrentCellInfo,
	.GetNetworkSearchInformation = v_GetNetworkSearchInformation,
	.GetNetworkSelectionMode = v_GetNetworkSelectionMode,
	.SetNetworkSelectionMode = v_SetNetworkSelectionMode,
	.GetPreferredNetwork = v_GetPreferredNetwork,
	.SetPreferredNetwork = v_SetPreferredNetwork,
	.GetPhysicalChannelConfig = v_GetPhysicalChannelConfig,
	.SetNotificationFilter = v_SetNotificationFilter,
	.SetDeviceState = v_SetDeviceState,
	.GetRrcConnectionState = v_GetRrcConnectionState,
	.SetNrOptionMode = v_SetNrOptionMode,
	.GetNrOptionMode = v_GetNrOptionMode,
};

static const HRilModemReq g_modem_ops = {
	.SetRadioState = v_SetRadioState,
	.GetRadioState = v_GetRadioState,
	.GetImei = v_GetImei,
	.GetImeiSv = v_GetImeiSv,
	.GetMeid = v_GetMeid,
	.GetVoiceRadioTechnology = v_GetVoiceRadioTechnology,
	.GetBasebandVersion = v_GetBasebandVersion,
	.ShutDown = v_ShutDown,
};

static const HRilOps g_hril_ops = {
	.version = HRIL_VERSION_1,
	.callOps = &g_call_ops,
	.simOps = &g_sim_ops,
	.smsOps = &g_sms_ops,
	.dataOps = &g_data_ops,
	.networkOps = &g_network_ops,
	.modemOps = &g_modem_ops,
	.emcRescueOps = NULL,
};

__attribute__((visibility("default")))
const HRilOps *RilInitOps(const HRilReport *reportOps)
{
	if (reportOps == NULL)
		return NULL;
	pthread_mutex_lock(&g_state_lock);
	g_report_ops = reportOps;
	if (!g_worker_started) {
		pthread_t tid;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&tid, &attr, worker_loop, NULL) == 0)
			g_worker_started = true;
#ifndef HRIL_VENDOR_HOST_TEST
		(void)pthread_create(&tid, &attr, imsdcm_thread, NULL);
		(void)pthread_create(&tid, &attr, ind_event_thread, NULL);
#endif
		pthread_attr_destroy(&attr);
	}
	pthread_mutex_unlock(&g_state_lock);
	return &g_hril_ops;
}

#ifdef HRIL_VENDOR_HOST_TEST
#include <assert.h>

static pthread_mutex_t g_test_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_test_cv = PTHREAD_COND_INITIALIZER;
static int g_test_done = 0;
static int32_t g_last_err = -999;
static int32_t g_last_slot = -999;
static size_t g_last_len = 999;
static uint8_t g_last_data[128];
static int g_noti_count = 0;
static int32_t g_last_noti_msg = 0;
static int32_t g_last_noti_slot = -1;
static uint8_t g_last_noti_data[128];

static void test_on_report(int32_t slotId, const ReportInfo *ri, const uint8_t *data, size_t dataLen)
{
	pthread_mutex_lock(&g_test_mu);
	if (ri->type == HRIL_REPORT_NOTIFICATION) {
		g_last_noti_msg = ri->notifyMessage;
		g_last_noti_slot = slotId;
		memset(g_last_noti_data, 0, sizeof(g_last_noti_data));
		if (data != NULL && dataLen > 0) {
			size_t cp = dataLen < sizeof(g_last_noti_data) ? dataLen : sizeof(g_last_noti_data);
			memcpy(g_last_noti_data, data, cp);
		}
		g_noti_count++;
		pthread_mutex_unlock(&g_test_mu);
		return;
	}
	g_last_slot = slotId;
	g_last_err = ri->error;
	g_last_len = dataLen;
	memset(g_last_data, 0, sizeof(g_last_data));
	if (data != NULL && dataLen > 0) {
		size_t cp = dataLen < sizeof(g_last_data) ? dataLen : sizeof(g_last_data);
		memcpy(g_last_data, data, cp);
	}
	g_test_done = 1;
	pthread_cond_signal(&g_test_cv);
	pthread_mutex_unlock(&g_test_mu);
}

static void wait_report(void)
{
	pthread_mutex_lock(&g_test_mu);
	while (!g_test_done)
		pthread_cond_wait(&g_test_cv, &g_test_mu);
	g_test_done = 0;
	pthread_mutex_unlock(&g_test_mu);
}

int main(void)
{
	/* 1. EF decoders & SIM path parser */
	const uint8_t iccid_raw[10] = { 0x98, 0x68, 0x00, 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0xf3 };
	char iccid[32];
	assert(decode_iccid_bcd(iccid_raw, sizeof(iccid_raw), iccid, sizeof(iccid)) == 0);
	assert(strcmp(iccid, "8986002143658709213") == 0);

	const uint8_t imsi_raw[9] = { 0x08, 0x49, 0x06, 0x00, 0x12, 0x34, 0x56, 0x78, 0x90 };
	char imsi[32];
	assert(decode_imsi_bcd(imsi_raw, sizeof(imsi_raw), imsi, sizeof(imsi)) == 0);
	assert(strcmp(imsi, "460002143658709") == 0);

	uint8_t path[16], plen = 0;
	parse_sim_path_le("3F007FFF", 0x6f07, path, &plen);
	assert(plen == 4 && path[0] == 0x00 && path[1] == 0x3f && path[2] == 0xff && path[3] == 0x7f);

	/* 2. QMI NAS & UIM parsers */
	uint8_t nas_tlvs[64];
	size_t nl = 0;
	uint8_t sig_gsm[1] = { (uint8_t)-75 };
	uint8_t sig_wcdma[3] = { (uint8_t)-81, 0xf4, 0xff }; /* rssi=-81, ecio=-12 */
	uint8_t sig_lte[6] = { (uint8_t)-67, (uint8_t)-9, 0x9b, 0xff, 0xc8, 0x00 }; /* rssi=-67, rsrq=-9, rsrp=-101, snr=200 */
	uint8_t sig_nr4[4] = { 0xa8, 0xff, 0x78, 0x00 }; /* rsrp=-88, snr=120 */
	uint8_t sig_nr_rsrq[2] = { 0xf5, 0xff }; /* rsrq=-11 */
	nl += tlv_append(nas_tlvs + nl, 0x12, sig_gsm, sizeof(sig_gsm));
	nl += tlv_append(nas_tlvs + nl, 0x13, sig_wcdma, sizeof(sig_wcdma));
	nl += tlv_append(nas_tlvs + nl, 0x14, sig_lte, sizeof(sig_lte));
	nl += tlv_append(nas_tlvs + nl, 0x17, sig_nr4, sizeof(sig_nr4));
	nl += tlv_append(nas_tlvs + nl, 0x18, sig_nr_rsrq, sizeof(sig_nr_rsrq));
	HRilRssi rssi;
	parse_nas_signal_info(nas_tlvs, nl, &rssi);
	assert(rssi.vals[HRIL_RSSI_GSM_RXLEV] == -75);
	assert(rssi.vals[HRIL_RSSI_WCDMA_RXLEV] == -81 && rssi.vals[HRIL_RSSI_WCDMA_ECIO] == -12);
	assert(rssi.vals[HRIL_RSSI_LTE_RXLEV] == -67);
	assert(rssi.vals[HRIL_RSSI_LTE_RSRQ] == -9);
	assert(rssi.vals[HRIL_RSSI_LTE_RSRP] == -101);
	assert(rssi.vals[HRIL_RSSI_LTE_SNR] == 200);
	assert(rssi.vals[HRIL_RSSI_NR_RSRP] == -88 && rssi.vals[HRIL_RSSI_NR_SINR] == 120 && rssi.vals[HRIL_RSSI_NR_RSRQ] == -11);

	uint8_t srv_tlvs[96];
	size_t sl = 0;
	uint8_t srv_01[6] = { 1, 1, 1, 1, 1, 8 }; /* reg=1, cs=1, ps=1, LTE */
	uint8_t roam_10[1] = { 1 }; /* home */
	uint8_t plmn_12[11] = { 0xcc, 0x01, 0x00, 0x00, 6, 'C', 'M', 'C', 'C', '-', 'T' };
	uint8_t lac_1c[2] = { 0x34, 0x12 }; /* 0x1234 */
	uint8_t cid_1d[4] = { 0x04, 0x03, 0x02, 0x01 }; /* 0x01020304 */
	sl += tlv_append(srv_tlvs + sl, 0x01, srv_01, sizeof(srv_01));
	sl += tlv_append(srv_tlvs + sl, 0x10, roam_10, sizeof(roam_10));
	sl += tlv_append(srv_tlvs + sl, 0x12, plmn_12, sizeof(plmn_12));
	sl += tlv_append(srv_tlvs + sl, 0x1c, lac_1c, sizeof(lac_1c));
	sl += tlv_append(srv_tlvs + sl, 0x1d, cid_1d, sizeof(cid_1d));
	HRilRegStatusInfo reg;
	HRilVoiceRadioInfo voice;
	char num[16], opname[32];
	parse_nas_serving_info(srv_tlvs, sl, true, &reg, &voice, num, sizeof(num), opname, sizeof(opname));
	assert(reg.regStatus == HRIL_REG_HOME && reg.actType == HRIL_TECH_LTE);
	assert(reg.lac == 0x1234 && reg.cellId == 0x01020304);
	assert(voice.actType == HRIL_TECH_LTE && strcmp(num, "46000") == 0 && strcmp(opname, "CMCC-T") == 0);

	uint8_t dsb_req[32];
	assert(build_nas_set_dsb_tlvs(dsb_req, 0x05u, 1u, 1u, 1u, 0x03ULL) == 27);
	assert(dsb_req[0] == 0x10 && dsb_req[3] == 0x05 && dsb_req[4] == 0x11 && dsb_req[7] == 1);
	assert(dsb_req[16] == 0x14 && dsb_req[19] == 0x03);

	uint8_t uim_tlvs[64];
	uint8_t card_10[31] = {
		0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1, /* gw_pri=slot0, num_cards=1 */
		1, 0, 0, 0, 0, 1, /* card0: present, 1 app */
		2, 7, 0, 0, 0, 0, 2, /* USIM, READY, aid_len=2 */
		0xa0, 0x00,
		0, 3, 0, 0, 0, 0, 0 /* pin1 disabled/ready */
	};
	size_t ul = tlv_append(uim_tlvs, 0x10, card_10, sizeof(card_10));
	struct uim_card_info ci;
	parse_uim_card_status(uim_tlvs, ul, &ci);
	assert(ci.slot_present[0] && !ci.slot_present[1]);
	assert(ci.sim_type[0] == HRIL_SIM_TYPE_USIM && ci.sim_state[0] == HRIL_SIM_READY);

	/* 2b. Slot 0 error + Slot 1 present USIM ready => active_slot == 1 */
	uint8_t card_slot1_10[37] = {
		0xff, 0xff, 0xff, 0xff, 0x01, 0x01, 0xff, 0xff, 2, /* gw_sec=slot1, num_cards=2 */
		2, 3, 0, 0, 0, 0, /* card0: error, 0 apps */
		1, 0, 0, 0, 0, 1, /* card1: present, 1 app */
		2, 7, 0, 0, 0, 0, 2, /* USIM, READY, aid_len=2 */
		0xa0, 0x00,
		0, 3, 0, 0, 0, 0, 0
	};
	size_t ul1 = tlv_append(uim_tlvs, 0x10, card_slot1_10, sizeof(card_slot1_10));
	parse_uim_card_status(uim_tlvs, ul1, &ci);
	assert(!ci.slot_present[0] && ci.slot_present[1] && ci.active_slot == 1);
	assert(ci.sim_type[1] == HRIL_SIM_TYPE_USIM && ci.sim_state[1] == HRIL_SIM_READY);

	uint8_t fcp[15];
	synthesize_gsm_fcp_15(9, 0x6f07, 0, 0, fcp);
	char fcp_hex[32];
	encode_hex_upper(fcp, sizeof(fcp), fcp_hex, sizeof(fcp_hex));
	assert(strncmp(fcp_hex, "000000096F0704", 14) == 0);

	/* 3. Blocked Call/SMS/Data + NULL safety + worker queue */
	HRilReport rep = {
		.OnCallReport = test_on_report,
		.OnDataReport = test_on_report,
		.OnModemReport = test_on_report,
		.OnNetworkReport = test_on_report,
		.OnSimReport = test_on_report,
		.OnSmsReport = test_on_report,
	};
	assert(RilInitOps(NULL) == NULL);
	const HRilOps *ops = RilInitOps(&rep);
	assert(ops != NULL && ops->version == HRIL_VERSION_1);

	int32_t raw_req[3] = { 7, 42, 1 };
	const ReqDataInfo *wire_req = (const ReqDataInfo *)raw_req;
	assert(wire_req->serial == 7 && wire_req->request == 42 && wire_req->slotId == 1);
	ReqDataInfo req = *wire_req;
	ops->callOps->Dial(wire_req, "10086", 5);
	wait_report();
	assert(g_last_slot == 1 && g_last_err == HRIL_ERR_GENERIC_FAILURE);

	ops->smsOps->SendGsmSms(&req, "x", 1);
	wait_report();
	assert(g_last_err == HRIL_ERR_GENERIC_FAILURE);

	ops->smsOps->SendCdmaSms(&req, "x", 1);
	wait_report();
	assert(g_last_err == HRIL_ERR_GENERIC_FAILURE);

	ops->dataOps->ActivatePdpContext(&req, NULL);
	wait_report();
	assert(g_last_err == HRIL_ERR_GENERIC_FAILURE);

	ops->callOps->GetCallList(&req);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == 0);

	ops->networkOps->SetPreferredNetwork(&req, NULL);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS);

	ops->networkOps->SetNotificationFilter(&req, NULL);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS);

	ops->networkOps->SetDeviceState(&req, NULL, NULL);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS);

	ops->simOps->GetPrimarySlot(&req);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == sizeof(int32_t));

	/* 4. F6 NAS SYS_INFO parser (46011 LTE+VoLTE registered, LIMITED, empty), BIND_SUB & delta helpers */
	const uint8_t ct_tlvs[] = {
		0x14, 3, 2, 0, 0,
		0x19, 29, 0,
		/* Wait: TLV header is type(1), len_le16(2) */
		0
	};
	(void)ct_tlvs;
	uint8_t si_buf[128];
	size_t sil = 0;
	uint8_t lte_st_srv[3] = { 2, 0, 0 };
	uint8_t lte_info_46011[29] = {
		1, 2, 1, 2, 1, 0, 1, 0, 0, 0xff, 0xff, 1,
		0x65, 0x63, 0x56, 0x04, 0, 0, 0,
		1, '4', '6', '0', '1', '1', 0xff,
		1, 0x34, 0x12
	};
	uint8_t ims_v[1] = { 1 };
	sil += tlv_append(si_buf + sil, 0x14, lte_st_srv, sizeof(lte_st_srv));
	sil += tlv_append(si_buf + sil, 0x19, lte_info_46011, sizeof(lte_info_46011));
	sil += tlv_append(si_buf + sil, 0x29, ims_v, sizeof(ims_v));

	HRilRegStatusInfo si_ps, si_cs;
	HRilVoiceRadioInfo si_voice;
	char si_num[16], si_name[64];
	assert(parse_nas_sys_info(si_buf, sil, true, &si_ps, &si_voice, si_num, sizeof(si_num), si_name, sizeof(si_name)));
	assert(si_ps.regStatus == HRIL_REG_HOME && si_ps.actType == HRIL_TECH_LTE);
	assert(si_ps.cellId == 0x04566365 && si_ps.lac == 0x1234);
	assert(strcmp(si_num, "46011") == 0 && strcmp(si_name, "China Telecom") == 0);
	assert(si_voice.actType == HRIL_TECH_LTE && si_voice.srvDomain == 2);
	assert(parse_nas_sys_info(si_buf, sil, false, &si_cs, NULL, NULL, 0, NULL, 0));
	assert(si_cs.regStatus == HRIL_REG_HOME && si_cs.actType == HRIL_TECH_LTE);

	/* LIMITED case */
	uint8_t lim_buf[64];
	size_t llim = 0;
	uint8_t lte_st_lim[3] = { 1, 3, 0 };
	uint8_t lte_info_lim[29] = { 0 };
	memcpy(lte_info_lim, lte_info_46011, sizeof(lte_info_lim));
	lte_info_lim[1] = 4; /* CAMPED */
	llim += tlv_append(lim_buf + llim, 0x14, lte_st_lim, sizeof(lte_st_lim));
	llim += tlv_append(lim_buf + llim, 0x19, lte_info_lim, sizeof(lte_info_lim));
	HRilRegStatusInfo reg_lim;
	assert(parse_nas_sys_info(lim_buf, llim, true, &reg_lim, NULL, NULL, 0, NULL, 0));
	assert(reg_lim.regStatus == HRIL_REG_SEARCHING);

	/* Empty Sub 0 case */
	uint8_t emp_buf[16];
	uint8_t lte_st_emp[3] = { 0, 0, 0 };
	size_t lemp = tlv_append(emp_buf, 0x14, lte_st_emp, sizeof(lte_st_emp));
	HRilRegStatusInfo reg_emp;
	assert(parse_nas_sys_info(emp_buf, lemp, true, &reg_emp, NULL, NULL, 0, NULL, 0));
	assert(reg_emp.regStatus == HRIL_REG_NOT_REGISTERED && reg_emp.actType == HRIL_TECH_UNKNOWN);

	uint8_t bind_req[8];
	assert(build_nas_bind_sub_tlvs(bind_req, 1u) == 4);
	assert(bind_req[0] == 0x01 && bind_req[1] == 1 && bind_req[3] == 1);

	assert(nas_reg_equal(&si_ps, &si_cs));
	assert(!nas_reg_equal(&si_ps, &reg_lim));
	HRilRssi r1, r2;
	init_empty_rssi(&r1);
	init_empty_rssi(&r2);
	for (size_t i = 0; i < 16; i++)
		assert(r1.vals[i] == 0);
	assert(rssi_equal(&r1, &r2));
	r2.vals[HRIL_RSSI_LTE_RSRP] = -105;
	assert(!rssi_equal(&r1, &r2));
	assert(rssi_changed_significantly(&r1, &r2));
	HRilRssi r3 = r2;
	r3.vals[HRIL_RSSI_LTE_RSRP] = -106;
	assert(!rssi_changed_significantly(&r2, &r3));
	r3.vals[HRIL_RSSI_LTE_RSRP] = -107;
	assert(rssi_changed_significantly(&r2, &r3));

	/* 5. F7 ABI IDs, NeighborCellInfoList(16B), missing ops, hysteresis, WDS parsers */
	assert(HNOTI_NETWORK_CS_REG_STATUS_UPDATED == 1401);
	assert(HNOTI_NETWORK_SIGNAL_STRENGTH_UPDATED == 1402);
	assert(HNOTI_NETWORK_TIME_UPDATED == 1403);
	assert(HNOTI_NETWORK_PS_REG_STATUS_UPDATED == 1406);
	assert(HNOTI_MODEM_RADIO_STATE_UPDATED == 1501);
	assert(HNOTI_MODEM_VOICE_TECH_UPDATED == 1502);

	assert(ops->simOps->SendSimMatchedOperatorInfo != NULL);
	assert(ops->dataOps->SetLinkBandwidthReportingRule != NULL);
	assert(ops->dataOps->SetDataProfileInfo != NULL);

	ops->networkOps->GetNeighboringCellInfoList(&req);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == sizeof(HRilCurrentCellList));
	{
		HRilCurrentCellList got_cells;
		memcpy(&got_cells, g_last_data, sizeof(got_cells));
		assert(got_cells.itemNum == 0 && got_cells.items != NULL);
	}

	ops->simOps->SendSimMatchedOperatorInfo(&req, NULL);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS);

	ops->dataOps->SetLinkBandwidthReportingRule(&req, NULL);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS);

	ops->dataOps->SetDataProfileInfo(&req, NULL, 0);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS);

	ops->dataOps->GetLinkCapability(&req);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == sizeof(HRilDataLinkCapability));

	ops->dataOps->GetPdpContextList(&req);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == 0);

	assert(ipv4_mask_to_prefix(0xffffff00u) == 24);
	assert(ipv4_mask_to_prefix(0xfffffffcu) == 30);
	assert(ipv4_mask_to_prefix(0u) == 32);

	uint8_t wbuf[128];
	assert(build_dpm_open_port_tlvs(wbuf, 4u, 1u, 2u, 23u) == 20);
	assert(wbuf[0] == 0x11 && wbuf[1] == 17 && wbuf[3] == 1u && wbuf[4] == 4u && wbuf[12] == 2u && wbuf[16] == 23u);
	assert(build_wda_set_data_fmt_tlvs(wbuf, 2u, 5u, 9u, 4u, 1u) == 46);
	assert(wbuf[7] == 0x12 && wbuf[10] == 5u && wbuf[14] == 0x13 && wbuf[17] == 9u);
	assert(build_wds_bind_mux_tlvs(wbuf, 4u, 1u, 1u, 1) == 22);
	assert(build_wds_bind_mux_tlvs(wbuf, 4u, 1u, 1u, -1) == 15);
	assert(build_wds_start_tlvs(wbuf, "ctnet", 4u) == 12);
	assert(build_wds_stop_tlvs(wbuf, 0x12345678u) == 7);
	assert(build_wds_curr_settings_tlvs(wbuf, 0x00003ff0u) == 7);

	uint8_t wcs[128];
	size_t wcl = 0;
	uint8_t wip[4] = { 56, 34, 12, 10 };    /* 10.12.34.56 */
	uint8_t wgw[4] = { 1, 34, 12, 10 };     /* 10.12.34.1 */
	uint8_t wmask[4] = { 0, 255, 255, 255 };/* 255.255.255.0 */
	uint8_t wd1[4] = { 114, 114, 114, 114 };
	uint8_t wd2[4] = { 5, 5, 5, 223 };      /* 223.5.5.5 */
	uint8_t wmtu[4] = { 0xb4, 0x05, 0, 0 }; /* 1460 */
	wcl += tlv_append(wcs + wcl, 0x1e, wip, sizeof(wip));
	wcl += tlv_append(wcs + wcl, 0x20, wgw, sizeof(wgw));
	wcl += tlv_append(wcs + wcl, 0x21, wmask, sizeof(wmask));
	wcl += tlv_append(wcs + wcl, 0x15, wd1, sizeof(wd1));
	wcl += tlv_append(wcs + wcl, 0x16, wd2, sizeof(wd2));
	wcl += tlv_append(wcs + wcl, 0x29, wmtu, sizeof(wmtu));

	struct pdp_session ps;
	memset(&ps, 0, sizeof(ps));
	ps.active = true;
	ps.cid = 1;
	snprintf(ps.pdp_type, sizeof(ps.pdp_type), "IP");
	assert(parse_wds_current_settings(wcs, wcl, &ps));
	assert(strcmp(ps.ip_prefix, "10.12.34.56/24") == 0);
	assert(strcmp(ps.gw, "10.12.34.1") == 0);
	assert(strcmp(ps.dns1, "114.114.114.114") == 0);
	assert(strcmp(ps.dns2, "223.5.5.5") == 0);
	assert(ps.mtu == 1460);

	HRilDataCallResponse dcall;
	fill_pdp_response(&ps, &dcall);
	assert(dcall.cid == 1 && dcall.active == 2 && strcmp(dcall.netPortName, "rmnet0") == 0);
	assert(strcmp(dcall.address, "10.12.34.56/24") == 0);
	assert(dcall.maxTransferUnit == 1460);

	/* 6. Per-slot radio OFF on empty slot 0 keeps active slot 1 ON locally */
	pthread_mutex_lock(&g_state_lock);
	g_slot_radio_inited = true;
	g_slot_radio_state[0] = HRIL_RADIO_POWER_ON;
	g_slot_radio_state[1] = HRIL_RADIO_POWER_ON;
	pthread_mutex_unlock(&g_state_lock);
	ReqDataInfo req0 = { .serial = 99, .request = 1, .slotId = 0 };
	ops->modemOps->SetRadioState(&req0, 0, 0);
	wait_report();
	assert(g_last_slot == 0 && g_last_err == HRIL_ERR_SUCCESS);
	assert(g_slot_radio_state[0] == HRIL_RADIO_POWER_OFF && g_slot_radio_state[1] == HRIL_RADIO_POWER_ON);

	/* 7. F8 OHOS RadioTech enum (LTE=9, NR=12) + V1_3/V1_4 HRilDataReq slots & IMS guard */
	assert(HRIL_TECH_LTE == 9 && HRIL_TECH_NR == 12 && HRIL_TECH_GSM == 1 && HRIL_TECH_WCDMA == 3);
	assert(ops->dataOps->GetLinkBandwidthInfo != NULL);
	assert(ops->dataOps->SendDataPerformanceMode != NULL);
	assert(ops->dataOps->SendDataSleepMode != NULL);
	assert(ops->dataOps->ActivatePdpContextWithApnTypes == ops->dataOps->ActivatePdpContext);
	assert(ops->dataOps->ActivatePdpContextWithApnTypesforSlice == ops->dataOps->ActivatePdpContext);

	ops->dataOps->GetLinkBandwidthInfo(&req, 1);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == sizeof(HRilLinkBandwidthInfo));

	ops->dataOps->SendDataPerformanceMode(&req, NULL);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS);

	ops->dataOps->SendDataSleepMode(&req, NULL);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS);

	HRilDataInfo ims_di = { .cid = 2, .apn = "ims", .type = "IPV4V6" };
	ops->dataOps->ActivatePdpContextWithApnTypesforSlice(&req, &ims_di);
	wait_report();
	assert(g_last_err == HRIL_ERR_GENERIC_FAILURE);
	assert(is_secondary_non_internet_apn("IMS") && is_secondary_non_internet_apn("xcap"));
	assert(!is_secondary_non_internet_apn("ctnet") && !is_secondary_non_internet_apn("cmnet"));

	/* 8. F9 SetRadioProtocol(slot 19) + per-slot modemId + CurrentCellInfoVendor(56B LTE/NR) */
	assert(HNOTI_SIM_RADIO_PROTOCOL_UPDATED == 1208);
	assert(ops->simOps->SetRadioProtocol != NULL);

	ReqDataInfo req1 = { .serial = 101, .request = 214, .slotId = 1 };
	ops->simOps->GetRadioProtocol(&req0);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == sizeof(HRilRadioProtocol));
	HRilRadioProtocol rp0, rp1;
	memcpy(&rp0, g_last_data, sizeof(rp0));
	ops->simOps->GetRadioProtocol(&req1);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == sizeof(HRilRadioProtocol));
	memcpy(&rp1, g_last_data, sizeof(rp1));
	assert(rp0.modemId != rp1.modemId);
	assert(rp0.status == HRIL_RADIO_PROTOCOL_STATUS_SUCCESS && rp1.status == HRIL_RADIO_PROTOCOL_STATUS_SUCCESS);

	g_noti_count = 0;
	HRilRadioProtocol setp = {
		.sessionId = 7,
		.phase = HRIL_RADIO_PROTOCOL_PHASE_UPDATE,
		.technology = 0x1fff,
		.modemId = 0,
		.status = HRIL_RADIO_PROTOCOL_STATUS_NONE,
	};
	ops->simOps->SetRadioProtocol(&req1, &setp);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == sizeof(HRilRadioProtocol));
	HRilRadioProtocol set_rsp;
	memcpy(&set_rsp, g_last_data, sizeof(set_rsp));
	assert(set_rsp.sessionId == 7 && set_rsp.modemId == 0 && set_rsp.status == HRIL_RADIO_PROTOCOL_STATUS_SUCCESS);
	usleep(20000);
	assert(g_noti_count >= 1 && g_last_noti_msg == HNOTI_SIM_RADIO_PROTOCOL_UPDATED && g_last_noti_slot == 1);
	assert(g_primary_slot == 1 && g_slot_modem_id[1] == 0);

	/* Cell location parser (TLV 0x13 LTE + 0x1c earfcn32 + 0x22 NR + 0x23 nr_arfcn) */
	uint8_t cloc_buf[96];
	size_t cll = 0;
	uint8_t lte_13[14] = {
		0, 0x64, 0xf0, 0x11,
		0x23, 0x7e,             /* tac = 0x7e23 */
		0x03, 0x25, 0x53, 0x06, /* cell_id = 0x06532503 */
		0x3a, 0x07,             /* earfcn = 1850 */
		0x9b, 0x00,             /* pci = 155 */
	};
	uint8_t earfcn_1c[4] = { 0x3a, 0x07, 0x00, 0x00 };
	uint8_t nr_22[16] = {
		0x64, 0xf0, 0x11,
		0x23, 0x7e, 0x00,                   /* nr_tac = 0x007e23 */
		0x01, 0x02, 0x03, 0x04, 0x05, 0, 0, 0, /* nr_nci = 0x0504030201 */
		0x2c, 0x01,                         /* nr_pci = 300 */
	};
	uint8_t nrarfcn_23[4] = { 0x80, 0x9c, 0x09, 0x00 }; /* 629888 */
	cll += tlv_append(cloc_buf + cll, 0x13, lte_13, sizeof(lte_13));
	cll += tlv_append(cloc_buf + cll, 0x1c, earfcn_1c, sizeof(earfcn_1c));
	cll += tlv_append(cloc_buf + cll, 0x22, nr_22, sizeof(nr_22));
	cll += tlv_append(cloc_buf + cll, 0x23, nrarfcn_23, sizeof(nrarfcn_23));
	struct nas_cell_loc cloc;
	parse_nas_cell_location_info(cloc_buf, cll, &cloc);
	assert(cloc.has_lte && cloc.lte_tac == 0x7e23 && cloc.lte_cell_id == 0x06532503 && cloc.lte_earfcn == 1850 && cloc.lte_pci == 155);
	assert(cloc.has_nr && cloc.nr_tac == 0x7e23 && cloc.nr_nci == 0x0504030201LL && cloc.nr_pci == 300 && cloc.nr_arfcn == 629888);

	HRilCurrentCellInfoVendor cv;
	HRilRegStatusInfo lte_reg = { .regStatus = HRIL_REG_HOME, .lac = 0x7e23, .cellId = 0x06532503, .actType = HRIL_TECH_LTE };
	assert(build_current_cell_info(&lte_reg, "46011", &rssi, &cloc, &cv));
	assert(cv.ratType == HRIL_CELL_RAT_LTE && cv.mcc == 460 && cv.mnc == 11);
	assert(cv.serviceCellParas.lte.arfcn == 1850 && cv.serviceCellParas.lte.cellId == 0x06532503);
	assert(cv.serviceCellParas.lte.pci == 155 && cv.serviceCellParas.lte.tac == 0x7e23);
	assert(cv.serviceCellParas.lte.rsrp == 101 && cv.serviceCellParas.lte.rsrq == 9 && cv.serviceCellParas.lte.rssi == 67);

	HRilRegStatusInfo nr_reg = { .regStatus = HRIL_REG_HOME, .lac = 0x7e23, .cellId = 0x1234, .actType = HRIL_TECH_NR };
	assert(build_current_cell_info(&nr_reg, "46011", &rssi, &cloc, &cv));
	assert(cv.ratType == HRIL_CELL_RAT_NR && cv.mcc == 460 && cv.mnc == 11);
	assert(cv.serviceCellParas.nr.nrArfcn == 629888 && cv.serviceCellParas.nr.pci == 300 && cv.serviceCellParas.nr.tac == 0x7e23);
	assert(cv.serviceCellParas.nr.nci == 0x0504030201LL && cv.serviceCellParas.nr.rsrp == 88 && cv.serviceCellParas.nr.rsrq == 11);

	/* 9. F10: 5G ENDC/DCNR fix + NR sentinel filter + RF band + SMSP/WMS + VOICE + IMSDCM */
	assert(HNOTI_CALL_STATE_UPDATED == 1001 && HNOTI_SMS_NEW_SMS == 1101);
	assert(ops->networkOps->SetNrOptionMode != NULL);
	assert(ops->smsOps->SendSmsAck != NULL && ops->smsOps->SetSmscAddr != NULL);
	assert(ops->callOps->HoldCall != NULL && ops->callOps->UnHoldCall != NULL && ops->callOps->SwitchCall != NULL);
	assert(ops->callOps->StartDtmf != NULL && ops->callOps->SendDtmf != NULL && ops->callOps->StopDtmf != NULL);

	/* 9a. ENDC available (0x4f=1) + DCNR allowed (0x4e=0) => enDcAvailable=1, dcnr=0 (NOT inverted!) */
	uint8_t si_endc[128];
	size_t sel = 0;
	uint8_t dcnr0 = 0, endc1 = 1, dcnr1 = 1;
	sel += tlv_append(si_endc + sel, 0x14, lte_st_srv, sizeof(lte_st_srv));
	sel += tlv_append(si_endc + sel, 0x19, lte_info_46011, sizeof(lte_info_46011));
	sel += tlv_append(si_endc + sel, 0x29, ims_v, sizeof(ims_v));
	sel += tlv_append(si_endc + sel, 0x4e, &dcnr0, 1);
	sel += tlv_append(si_endc + sel, 0x4f, &endc1, 1);
	HRilRegStatusInfo endc_reg;
	assert(parse_nas_sys_info(si_endc, sel, true, &endc_reg, NULL, NULL, 0, NULL, 0));
	assert(endc_reg.enDcAvailable == 1 && endc_reg.dcnr == 0 && endc_reg.actType == HRIL_TECH_LTE);

	/* DCNR restricted (0x4e=1, 0x4f=1) => enDcAvailable=1, dcnr=1 */
	size_t sel2 = 0;
	sel2 += tlv_append(si_endc + sel2, 0x14, lte_st_srv, sizeof(lte_st_srv));
	sel2 += tlv_append(si_endc + sel2, 0x19, lte_info_46011, sizeof(lte_info_46011));
	sel2 += tlv_append(si_endc + sel2, 0x4e, &dcnr1, 1);
	sel2 += tlv_append(si_endc + sel2, 0x4f, &endc1, 1);
	assert(parse_nas_sys_info(si_endc, sel2, true, &endc_reg, NULL, NULL, 0, NULL, 0));
	assert(endc_reg.enDcAvailable == 1 && endc_reg.dcnr == 1);

	/* 9b. QMI NR sentinel -32768 (0x8000) sanitized to 0 while LTE remains intact */
	uint8_t sent_sig[64];
	size_t ssl = 0;
	uint8_t nr_sent[4] = { 0x00, 0x80, 0x00, 0x80 }; /* rsrp=-32768, snr=-32768 */
	ssl += tlv_append(sent_sig + ssl, 0x14, sig_lte, sizeof(sig_lte));
	ssl += tlv_append(sent_sig + ssl, 0x17, nr_sent, sizeof(nr_sent));
	HRilRssi rssi_s;
	parse_nas_signal_info(sent_sig, ssl, &rssi_s);
	assert(rssi_s.vals[HRIL_RSSI_LTE_RSRP] == -101);
	assert(rssi_s.vals[HRIL_RSSI_NR_RSRP] == 0 && rssi_s.vals[HRIL_RSSI_NR_SINR] == 0);

	/* 9c. RF Band Info -> HRilPhyChannelConfig */
	uint8_t rf_buf[64];
	size_t rfl = 0;
	uint8_t rf_ext[8] = { 1, 8, 0x7a, 0x00, 0xe2, 0x05, 0x00, 0x00 }; /* 1 entry: LTE, band=122, ch=1506 */
	uint8_t rf_bw[6] = { 1, 8, 3, 0, 0, 0 }; /* 10 MHz */
	rfl += tlv_append(rf_buf + rfl, 0x11, rf_ext, sizeof(rf_ext));
	rfl += tlv_append(rf_buf + rfl, 0x12, rf_bw, sizeof(rf_bw));
	HRilPhyChannelConfig pcfgs[2];
	assert(parse_nas_rf_band_info(rf_buf, rfl, 155, pcfgs, 2) == 1);
	assert(pcfgs[0].ratType == HRIL_TECH_LTE && pcfgs[0].downlinkChannelNum == 1506);
	assert(pcfgs[0].cellBandwidthDownlinkKhz == 10000 && pcfgs[0].physicalCellId == 155);

	/* 9d. EF_SMSP parser (real 43-byte China Telecom record) + WMS builders/IND parser */
	const uint8_t ct_smsp[43] = {
		0x29, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x08, 0x91, 0x68, 0x31, 0x33, 0x24, 0x26, 0x02, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	char smsc_str[48];
	int32_t smsc_tosca = 0;
	uint8_t smsc_raw[16];
	size_t smsc_raw_len = 0;
	assert(parse_smsp_record(ct_smsp, sizeof(ct_smsp), smsc_str, sizeof(smsc_str), &smsc_tosca,
				 smsc_raw, sizeof(smsc_raw), &smsc_raw_len));
	assert(strcmp(smsc_str, "+8613334262200") == 0 && smsc_tosca == 145 && smsc_raw_len == 9);

	uint8_t wms_req[64], sample_pdu[4] = { 0x00, 0x01, 0x00, 0x0a };
	size_t wlen = build_wms_raw_send_tlvs(wms_req, sizeof(wms_req), sample_pdu, sizeof(sample_pdu), true);
	assert(wlen == 14 && wms_req[0] == 0x01 && wms_req[3] == 0x06 && wms_req[10] == 0x13 && wms_req[13] == 1);
	size_t alen = build_wms_send_ack_tlvs(wms_req, 0x1234u, true, 0, true);
	assert(alen == 13 && wms_req[0] == 0x01 && wms_req[7] == 0x01 && wms_req[8] == 0x01);

	uint8_t wms_ind[64];
	size_t wil = 0;
	uint8_t tr_mt[12] = { 0x00, 0x78, 0x56, 0x00, 0x00, 0x06, 0x04, 0x00, 0xaa, 0xbb, 0xcc, 0xdd };
	uint8_t ims1 = 1;
	wil += tlv_append(wms_ind + wil, 0x11, tr_mt, sizeof(tr_mt));
	wil += tlv_append(wms_ind + wil, 0x16, &ims1, 1);
	uint32_t got_tx = 0;
	bool got_ims = false;
	char got_hex[32];
	assert(parse_wms_event_report_ind(wms_ind, wil, &got_tx, &got_ims, got_hex, sizeof(got_hex)));
	assert(got_tx == 0x5678u && got_ims && strcmp(got_hex, "AABBCCDD") == 0);

	/* 9e. VOICE call list parser (both 0x002f GET_ALL_CALL_INFO & 0x002e ALL_CALL_STATUS_IND) */
	uint8_t v_buf[64];
	size_t vl_b = 0;
	uint8_t v_info[15] = {
		2,
		1, 0x03, 0x02, 0x01, 0x04, 0, 0, /* id=1, CONVERSATION(active=0), MO(dir=0) */
		2, 0x09, 0x02, 0x02, 0x04, 0, 0, /* id=2, END -> filtered out */
	};
	uint8_t v_num[16] = { 1, 1, 0, 6, '+', '1', '0', '0', '0', '1' };
	vl_b += tlv_append(v_buf + vl_b, 0x10, v_info, sizeof(v_info));
	vl_b += tlv_append(v_buf + vl_b, 0x11, v_num, 10);
	struct cached_call_entry ce[4];
	assert(parse_voice_call_list(v_buf, vl_b, false, ce, 4) == 1);
	assert(ce[0].index == 1 && ce[0].state == 0 && ce[0].dir == 0 && ce[0].type == 145);
	assert(strcmp(ce[0].number, "+10001") == 0);

	/* 9f. IMSDCM 0x0020 response & IPv6 indication builders */
	uint8_t dcm_buf[128];
	size_t dl = build_imsdcm_start_resp(dcm_buf, 4, 1, 100, 2);
	assert(dl == 32 && dcm_buf[0] == 0x02 && get_le16(dcm_buf + 3) == QMI_IMSDCM_START_CONN);
	size_t il = build_imsdcm_conn_ind(dcm_buf, 1, 1, 100, 2, 1, "240e::1");
	assert(il == 47 && dcm_buf[0] == 0x04 && get_le16(dcm_buf + 3) == QMI_IMSDCM_START_CONN);
	size_t il_fail = build_imsdcm_conn_ind(dcm_buf, 2, 1, 100, 2, 1, NULL);
	assert(il_fail == 32 && dcm_buf[0] == 0x04 && dcm_buf[10] == 0 && dcm_buf[12] == 13);

	/* 9g. SetNrOptionMode + SendSmsAck worker dispatch */
	int32_t nr_m = 3;
	ops->networkOps->SetNrOptionMode(&req, &nr_m);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_nr_opt_mode == 3);
	int32_t ack_args[2] = { 1, 0 };
	ops->smsOps->SendSmsAck(&req, ack_args, sizeof(ack_args));
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS);

	/* 10. F12: CallSupplement/GetCallFailReason + VoLTE dial TLVs + NR SA CS reg + CDMA->GSM SMS + SA 4014 wire */
	assert(ops->callOps->CallSupplement != NULL && ops->callOps->GetCallFailReason != NULL);
	ops->callOps->GetCallFailReason(&req);
	wait_report();
	assert(g_last_err == HRIL_ERR_SUCCESS && g_last_len == sizeof(int32_t));
	{
		int32_t fail_r = 0;
		memcpy(&fail_r, g_last_data, sizeof(fail_r));
		assert(fail_r == 16);
	}

	/* 10a. VoLTE dial TLV builder (CALL_TYPE_VOICE_IP=0x08, VOICE=0x00, bare=-1) */
	{
		uint8_t dbuf[64];
		size_t dl1 = build_voice_dial_tlvs(dbuf, sizeof(dbuf), "10001", 0x08);
		assert(dl1 == 12 && dbuf[0] == 0x01 && dbuf[1] == 5 && memcmp(dbuf + 3, "10001", 5) == 0);
		assert(dbuf[8] == 0x10 && dbuf[9] == 1 && dbuf[11] == 0x08);
		size_t dl2 = build_voice_dial_tlvs(dbuf, sizeof(dbuf), "10001", -1);
		assert(dl2 == 8);
	}

	/* 10b. Call end reason TLV 0x16 parser */
	{
		uint8_t cer_buf[32];
		uint8_t cer_busy[4] = { 1, 1, 0x86, 0x03 }; /* SIP 486 -> 17 */
		size_t cer_l = tlv_append(cer_buf, 0x16, cer_busy, sizeof(cer_busy));
		assert(parse_voice_call_end_reason(cer_buf, cer_l) == 17);
		assert(parse_voice_call_end_reason(cer_buf, 0) == 16);
	}

	/* 10c. NR SA TLV 0x56 (nr5g_ims_voice_avail=1) keeps CS regStatus == HRIL_REG_HOME */
	{
		uint8_t nr_si[128];
		size_t nsl = 0;
		uint8_t nr_st[3] = { 2, 0, 0 };
		nsl += tlv_append(nr_si + nsl, 0x4a, nr_st, sizeof(nr_st));
		nsl += tlv_append(nr_si + nsl, 0x4b, lte_info_46011, sizeof(lte_info_46011));
		nsl += tlv_append(nr_si + nsl, 0x56, ims_v, sizeof(ims_v));
		HRilRegStatusInfo nr_cs;
		assert(parse_nas_sys_info(nr_si, nsl, false, &nr_cs, NULL, NULL, 0, NULL, 0));
		assert(nr_cs.regStatus == HRIL_REG_HOME && nr_cs.actType == HRIL_TECH_NR);
	}

	/* 10d. 3GPP2 CDMA MO SMS -> 3GPP GSM SMS-SUBMIT transcoder (ASCII 7-bit + Chinese UTF-16BE) */
	{
		/* Case 1: Point-to-Point(0x00), DestAddr(0x04) DTMF "10001", BearerData(0x08) ASCII "Hi" */
		/* DestAddr: digit_mode=0, num_mode=0, num_fields=5 (00_00000101), digits 1,0(a),0(a),0(a),1 -> 0x01,0x45,0xaa,0x84? Let's pack exact bits:
		 * bits: 0,0, 00000101 (5), 0001(1), 1010(0), 1010(0), 1010(0), 0001(1), 00 -> 32 bits = 4 bytes:
		 * byte0: 0000_0001 = 0x01
		 * byte1: 0100_0110 = 0x46
		 * byte2: 1010_1010 = 0xaa
		 * byte3: 1000_0100 = 0x84
		 * BearerData:
		 *   sub0 (msgId): 0x00, 0x03, 0x20, 0x04, 0x20 (type=SUBMIT, id=0x0042, header_ind=0)
		 *   sub1 (UserData): 0x01, 0x04, encoding=0x02(00010), num_fields=2(00000010), 'H'(1001000), 'i'(1101001), pad(00000)
		 *     byte0: 00010_000 = 0x10
		 *     byte1: 00010_100 = 0x14
		 *     byte2: 1000_1101 = 0x8d
		 *     byte3: 001_00000 = 0x20 */
		const uint8_t cdma_ascii[] = {
			0x00,
			0x04, 0x04, 0x01, 0x46, 0xaa, 0x84,
			0x08, 0x0b,
			0x00, 0x03, 0x20, 0x04, 0x20,
			0x01, 0x04, 0x10, 0x14, 0x8d, 0x20
		};
		uint8_t gsm_out[128];
		size_t gsm_l = 0;
		assert(transcode_cdma_pdu_to_gsm_submit(cdma_ascii, sizeof(cdma_ascii),
							smsc_raw, smsc_raw_len,
							gsm_out, sizeof(gsm_out), &gsm_l));
		/* Check SMSC prefix(9B) + TP-FO(0x11) + TP-MR(0x42) + TP-DA(len=5, ton=0x81, 01 00 F1) + PID(0) + DCS(0) + VP(0xA7) + UDL(2) + "Hi" packed(C8 34) */
		assert(gsm_l == 9 + 13);
		assert(gsm_out[9] == 0x11 && gsm_out[10] == 0x42 && gsm_out[11] == 5 && gsm_out[12] == 0x81);
		assert(gsm_out[13] == 0x01 && gsm_out[14] == 0x00 && gsm_out[15] == 0xf1);
		assert(gsm_out[16] == 0x00 && gsm_out[17] == 0x00 && gsm_out[18] == 0xa7 && gsm_out[19] == 2);
		assert(gsm_out[20] == 0xc8 && gsm_out[21] == 0x34);

		/* Case 2: Chinese UTF-16BE (encoding=0x04) "你好" (0x4F60 0x597D) + 8-bit ASCII intl "8610001"
		 * DestAddr: digit_mode=1, num_mode=0, num_type=1(001 intl), num_plan=1(0001), num_fields=7(00000111), ASCII '8','6','1','0','0','0','1'
		 *   bits: 1,0,001,0001 = 9 bits; 00000111 = 8 bits -> total 17 + 56 = 73 bits (10 bytes):
		 *   byte0: 10001000 = 0x88
		 *   byte1: 1_0000011 = 0x83
		 *   byte2: 1_0011100 = 0x9c  ('8'=0x38=00111000 -> top 7 bits)
		 *   byte3: 0_0011011 = 0x1b  ('6'=0x36=00110110)
		 *   byte4: 0_0011000 = 0x18  ('1'=0x31=00110001)
		 *   byte5: 1_0011000 = 0x98  ('0'=0x30=00110000)
		 *   byte6: 0_0011000 = 0x18  ('0')
		 *   byte7: 0_0011000 = 0x18  ('0')
		 *   byte8: 0_0011000 = 0x18  ('1'=0x31)
		 *   byte9: 1_0000000 = 0x80
		 * UserData: encoding=0x04(00100), num_fields=2(00000010), 0x4F60, 0x597D -> 5+8+32=45 bits (6 bytes):
		 *   byte0: 00100_000 = 0x20
		 *   byte1: 00010_010 = 0x12 (top 3 bits of 0x4F=01001111)
		 *   byte2: 01111_011 = 0x7b (bot 5 of 0x4F + top 3 of 0x60=01100000)
		 *   byte3: 00000_010 = 0x02 (bot 5 of 0x60 + top 3 of 0x59=01011001)
		 *   byte4: 11001_011 = 0xcb (bot 5 of 0x59 + top 3 of 0x7D=01111101)
		 *   byte5: 11101_000 = 0xe8 (bot 5 of 0x7D + 000) */
		const uint8_t cdma_ucs2[] = {
			0x00,
			0x04, 0x0a, 0x88, 0x83, 0x9c, 0x1b, 0x18, 0x98, 0x18, 0x18, 0x18, 0x80,
			0x08, 0x0d,
			0x00, 0x03, 0x20, 0x00, 0x70,
			0x01, 0x06, 0x20, 0x12, 0x7b, 0x02, 0xcb, 0xe8
		};
		assert(transcode_cdma_pdu_to_gsm_submit(cdma_ucs2, sizeof(cdma_ucs2),
							NULL, 0, gsm_out, sizeof(gsm_out), &gsm_l));
		assert(gsm_out[0] == 0x00 && gsm_out[1] == 0x11 && gsm_out[2] == 0x07);
		assert(gsm_out[3] == 7 && gsm_out[4] == 0x91); /* international +8610001 */
		assert(gsm_out[5] == 0x68 && gsm_out[6] == 0x01 && gsm_out[7] == 0x00 && gsm_out[8] == 0xf1);
		assert(gsm_out[9] == 0x00 && gsm_out[10] == 0x08 && gsm_out[11] == 0xa7 && gsm_out[12] == 4);
		assert(gsm_out[13] == 0x4f && gsm_out[14] == 0x60 && gsm_out[15] == 0x59 && gsm_out[16] == 0x7d);
	}

	/* 10e. SA 4014 QMI_IMSA wire parsers (ims_core_wire.h) */
	{
		uint8_t ireg[16], isrv[48];
		uint8_t r2[4] = { 2, 0, 0, 0 };
		uint8_t rat_nr[4] = { 8, 0, 0, 0 };
		size_t irl = tlv_append(ireg, 0x12, r2, 4);
		assert(parse_imsa_reg_status_tlvs(ireg, irl));
		size_t isl = 0;
		isl += tlv_append(isrv + isl, 0x10, r2, 4);     /* sms=2 */
		isl += tlv_append(isrv + isl, 0x11, r2, 4);     /* voice=2 */
		isl += tlv_append(isrv + isl, 0x14, rat_nr, 4); /* voip_rat=NR(8) */
		ImsServiceStatusWire sw;
		ImsRegistrationStatusWire rw;
		parse_imsa_srv_status_tlvs(isrv, isl, true, &sw, &rw);
		assert(sw.supportImsVoice == 1 && sw.supportImsSms == 1 && sw.supportImsVideo == 0);
		assert(sw.imsRegTech == OHOS_IMS_REG_TECH_NR);
		assert(rw.isRegisterd == 1 && rw.imsRegTech == OHOS_IMS_REG_TECH_NR);
	}

	/* 10f. F13 SMSC BCD encoder (+8613334262200 -> 08 91 68 31 33 24 26 02 f0) */
	{
		uint8_t bcd[16];
		size_t bl = encode_smsc_bcd("+8613334262200", 145, bcd, sizeof(bcd));
		const uint8_t want[] = { 0x08, 0x91, 0x68, 0x31, 0x33, 0x24, 0x26, 0x02, 0xf0 };
		assert(bl == sizeof(want) && memcmp(bcd, want, sizeof(want)) == 0);
	}

	/* 10g. M5 CS/IMS voice call dedup coordination (call_type 0x00 CS vs 0x08 VOICE_IP / 0x09 VT) */
	{
		uint8_t vbuf[96];
		size_t vlen = 0;
		uint8_t vinfo[22] = {
			3,
			1, 0x02, 0x08, 0x02, 0x04, 0, 0, /* id=1, INCOMING(4), VOICE_IP(0x08), MT */
			2, 0x03, 0x00, 0x01, 0x02, 0, 0, /* id=2, ACTIVE(0),   CS(0x00),       MO */
			3, 0x05, 0x09, 0x01, 0x04, 0, 0, /* id=3, ALERTING(3), VT(0x09),       MO */
		};
		vlen += tlv_append(vbuf + vlen, 0x01, vinfo, sizeof(vinfo));
		struct cached_call_entry all_e[4], cs_e[4];
		size_t n_all = parse_voice_call_list(vbuf, vlen, true, all_e, 4);
		assert(n_all == 3);
		assert(all_e[0].call_type == 0x08u && all_e[1].call_type == 0x00u && all_e[2].call_type == 0x09u);

		/* When ims-call-active is NOT active, fallback reports ALL 3 calls */
		size_t ims_cnt = 0;
		g_host_test_ims_call_bridge = false;
		size_t n_fb = filter_cs_call_entries(all_e, n_all, ims_call_bridge_active(), cs_e, 4, &ims_cnt);
		assert(n_fb == 3 && ims_cnt == 2);

		/* When ims-call-active IS active, filter out 0x08/0x09 and keep only 0x00 CS */
		g_host_test_ims_call_bridge = true;
		size_t n_cs = filter_cs_call_entries(all_e, n_all, ims_call_bridge_active(), cs_e, 4, &ims_cnt);
		assert(n_cs == 1 && ims_cnt == 2 && cs_e[0].index == 2 && cs_e[0].call_type == 0x00u);

		/* IP-only call list yields 0 CS calls (so CSControl skips duplicate notification) */
		size_t n_ip_only = filter_cs_call_entries(all_e, 1, ims_call_bridge_active(), cs_e, 4, &ims_cnt);
		assert(n_ip_only == 0 && ims_cnt == 1);
		g_host_test_ims_call_bridge = false;
	}

	printf("RIL_VENDOR_QMI_HOST_TEST_OK=1\n");
	return 0;
}
#endif /* HRIL_VENDOR_HOST_TEST */
