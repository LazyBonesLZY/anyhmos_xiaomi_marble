/* SPDX-License-Identifier: MIT
 * Clean-room OHOS SA 4014 (ImsCoreService) QMI_IMSA bridge for Xiaomi marble.
 *
 * Bridges live Qualcomm MPSS QMI_IMSA (service 0x21) registration & capability
 * status into OHOS CoreService (SA 4010) via ImsCoreServiceCallback, allowing:
 *   - CallManager (SA 4005) to pass the China Telecom IMS registration check
 *     (CALL_FAILED_CTCARD_NO_IMS) on VoLTE/VoNR networks,
 *   - CellularCall (SA 4006) & SmsMms (SA 4008) to continue routing through
 *     our QMI RIL (GetProxyObjectPtr code 101 returns nullptr).
 *
 * Privacy: never logs phone numbers, IMPU/IMSI/ICCID, or SMS content.
 */
#include "ims_core_wire.h"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unistd.h>

#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>

#include "ipc_skeleton.h"
#include "iremote_object.h"
#include "message_option.h"
#include "message_parcel.h"

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

namespace {

constexpr uint32_t kSvcWms = 0x05u;
constexpr uint32_t kSvcImsa = 0x21u;
constexpr uint16_t kQmiWmsRawSend = 0x0020u;
constexpr uint16_t kQmiWmsGetSmscAddress = 0x0034u;
constexpr uint16_t kQmiWmsBindSubscription = 0x0047u;
constexpr uint16_t kQmiImsaGetRegStatus = 0x0020u;
constexpr uint16_t kQmiImsaGetSrvStatus = 0x0021u;
constexpr uint16_t kQmiImsaBind = 0x0033u;

const char16_t *const kServiceDescriptor = u"OHOS.Telephony.ImsCoreServiceInterface";
const char16_t *const kCallbackDescriptor = u"OHOS.Telephony.ImsCoreServiceCallback";
const char16_t *const kSmsServiceDescriptor = u"OHOS.Telephony.ImsSmsInterface";
const char16_t *const kSmsCallbackDescriptor = u"OHOS.Telephony.ImsSmsCallback";

constexpr size_t kStubObjectBytes = 4096;
constexpr size_t kVtableEntries = 128;
constexpr size_t kPrimaryPrefix = 4;
constexpr size_t kDispatchSlot = 200 / 8;

using StubCtor = void (*)(void *, std::u16string, bool);
using StubDispatch = int (*)(void *, uint32_t, OHOS::MessageParcel &, OHOS::MessageParcel &,
                             OHOS::MessageOption &);
using SetThreads = bool (*)(int);
using JoinThread = void (*)();
using GetInstance = void *(*)();
using GetManager = void *(*)(void *);
using AddAbility = int (*)(void *, int32_t, const void *, const void *);

struct SamgrApi {
	void *lib = nullptr;
	GetInstance instance = nullptr;
	GetManager manager = nullptr;
	AddAbility addSystemAbility = nullptr;
};

struct IpcApi {
	StubCtor ctor = nullptr;
	StubDispatch originalDispatch = nullptr;
	SetThreads setThreads = nullptr;
	JoinThread joinThread = nullptr;
};

struct SptrCarrier {
	void *ptr;
	~SptrCarrier() {}
};

struct SAExtraProp {
	bool isDistributed;
	uint32_t dumpLevel;
	std::u16string capability;
	std::u16string permission;
};
static_assert(sizeof(SAExtraProp) == 56, "SAExtraProp must match the donor ABI");

struct SlotImsState {
	bool valid = false;
	ImsServiceStatusWire svc = {};
	ImsRegistrationStatusWire reg = {};
};

std::mutex gLogMutex;
FILE *gLog = nullptr;

std::mutex gStateMutex;
pthread_cond_t gStateCond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t gCondMutex = PTHREAD_MUTEX_INITIALIZER;
OHOS::sptr<OHOS::IRemoteObject> gCallback;
OHOS::sptr<OHOS::IRemoteObject> gSmsStub;
OHOS::sptr<OHOS::IRemoteObject> gSmsCallback;
int32_t gSmsConfig[2] = {1, 1};
SlotImsState gSlotState[2];
bool gForcePush[2] = {false, false};
bool gShutdown = false;

void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void Log(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	std::lock_guard<std::mutex> lock(gLogMutex);
	std::fprintf(stderr, "HMOS_IMS_CORE %s\n", line);
	if (gLog != nullptr) {
		std::fprintf(gLog, "HMOS_IMS_CORE %s\n", line);
		std::fflush(gLog);
	}
}

void OpenLog()
{
	static const char *const paths[] = {
		"/data/service/el1/public/telephony/hmos-ims-core.log",
		"/data/local/tmp/hmos-ims-core.log",
	};
	for (const char *path : paths) {
		gLog = std::fopen(path, "a");
		if (gLog != nullptr)
			return;
	}
}

void WakePoller(int32_t slotId)
{
	{
		std::lock_guard<std::mutex> lock(gStateMutex);
		if (slotId == 0 || slotId == 1) {
			gForcePush[slotId] = true;
		} else {
			gForcePush[0] = true;
			gForcePush[1] = true;
		}
	}
	pthread_mutex_lock(&gCondMutex);
	pthread_cond_signal(&gStateCond);
	pthread_mutex_unlock(&gCondMutex);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = static_cast<uint8_t>(v & 0xffu);
	p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = static_cast<uint8_t>(v & 0xffu);
	p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
	p[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
	p[3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

static inline size_t tlv_append(uint8_t *buf, uint8_t type, const void *val, uint16_t len)
{
	buf[0] = type;
	put_le16(buf + 1, len);
	if (len > 0 && val != nullptr)
		std::memcpy(buf + 3, val, len);
	return 3u + len;
}

bool DiscoverQrtrEndpoint(uint32_t service_id, uint32_t *out_node, uint32_t *out_port)
{
	int s = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return false;

	struct sockaddr_qrtr me = {};
	socklen_t slen = sizeof(me);
	if (getsockname(s, reinterpret_cast<struct sockaddr *>(&me), &slen) < 0) {
		close(s);
		return false;
	}

	struct qrtr_ctrl_pkt pkt = {};
	pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
	pkt.server.service = service_id;
	pkt.server.instance = 0;

	struct sockaddr_qrtr ctrl = {};
	ctrl.sq_family = AF_QIPCRTR;
	ctrl.sq_node = me.sq_node;
	ctrl.sq_port = QRTR_PORT_CTRL;

	if (sendto(s, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&ctrl), sizeof(ctrl)) < 0) {
		close(s);
		return false;
	}

	bool found = false;
	for (int i = 0; i < 16; ++i) {
		struct pollfd pfd = {s, POLLIN, 0};
		if (poll(&pfd, 1, 250) <= 0)
			break;
		struct qrtr_ctrl_pkt resp = {};
		struct sockaddr_qrtr src = {};
		socklen_t srclen = sizeof(src);
		ssize_t n = recvfrom(s, &resp, sizeof(resp), 0,
				     reinterpret_cast<struct sockaddr *>(&src), &srclen);
		if (n < static_cast<ssize_t>(sizeof(resp)))
			continue;
		if (resp.cmd == QRTR_TYPE_NEW_SERVER) {
			if (resp.server.service == 0 && resp.server.node == 0 && resp.server.port == 0)
				break;
			if (resp.server.service == service_id) {
				*out_node = resp.server.node;
				*out_port = resp.server.port;
				found = true;
			}
		}
	}
	close(s);
	return found;
}

bool DiscoverImsaEndpoint(uint32_t *out_node, uint32_t *out_port)
{
	return DiscoverQrtrEndpoint(kSvcImsa, out_node, out_port);
}

int QmiRpc(int sock, uint32_t node, uint32_t port, uint16_t tx, uint16_t msg_id,
	   const uint8_t *req_tlvs, size_t req_len,
	   uint8_t *resp_tlvs, size_t resp_cap, size_t *out_len, uint16_t *out_err)
{
	uint8_t buf[512];
	if (7 + req_len > sizeof(buf))
		return -EINVAL;
	buf[0] = 0x00;
	put_le16(buf + 1, tx);
	put_le16(buf + 3, msg_id);
	put_le16(buf + 5, static_cast<uint16_t>(req_len));
	if (req_len > 0 && req_tlvs != nullptr)
		std::memcpy(buf + 7, req_tlvs, req_len);

	struct sockaddr_qrtr dst = {};
	dst.sq_family = AF_QIPCRTR;
	dst.sq_node = node;
	dst.sq_port = port;

	if (sendto(sock, buf, 7 + req_len, 0, reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst)) < 0)
		return -errno;

	for (int attempt = 0; attempt < 8; ++attempt) {
		struct pollfd pfd = {sock, POLLIN, 0};
		int pr = poll(&pfd, 1, 800);
		if (pr == 0)
			return -ETIMEDOUT;
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		uint8_t rbuf[1024];
		struct sockaddr_qrtr src = {};
		socklen_t sl = sizeof(src);
		ssize_t n = recvfrom(sock, rbuf, sizeof(rbuf), 0,
				     reinterpret_cast<struct sockaddr *>(&src), &sl);
		if (n < 7 || src.sq_port == QRTR_PORT_CTRL || src.sq_node != node || src.sq_port != port)
			continue;
		if (rbuf[0] != 0x02 || ims_get_le16(rbuf + 1) != tx || ims_get_le16(rbuf + 3) != msg_id)
			continue;
		uint16_t plen = ims_get_le16(rbuf + 5);
		if (7u + plen > static_cast<size_t>(n) || plen > resp_cap)
			return -EPROTO;
		std::memcpy(resp_tlvs, rbuf + 7, plen);
		*out_len = plen;
		*out_err = 0;
		const uint8_t *rv = nullptr;
		uint16_t rl = 0;
		if (ims_qmi_find_tlv(resp_tlvs, plen, 0x02, &rv, &rl) == 0 && rl >= 4) {
			if (ims_get_le16(rv) != 0)
				*out_err = ims_get_le16(rv + 2);
		}
		return 0;
	}
	return -ETIMEDOUT;
}

int PollSlotImsa(uint32_t node, uint32_t port, uint32_t slot, SlotImsState *out, bool verbose)
{
	int s = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return -1;

	uint8_t q[32], r[512], b4[4];
	size_t rl = 0;
	uint16_t berr = 0, rerr = 0, sqerr = 0, tx = 1;
	put_le32(b4, slot);
	size_t ql = tlv_append(q, 0x10, b4, 4);

	/* Note: Qualcomm MPSS QMI_IMSA defaults to sub 0, so QMI_IMSA_BIND(0) can return
	 * QMI_ERR_NO_EFFECT (0x001a) or non-zero qerr on empty slots. Only transport
	 * failures (rc < 0) invalidate the QRTR endpoint! */
	int brc = QmiRpc(s, node, port, tx++, kQmiImsaBind, q, ql, r, sizeof(r), &rl, &berr);
	if (brc < 0) {
		close(s);
		return -1;
	}

	bool reg = false;
	ImsServiceStatusWire svc = {};
	ImsRegistrationStatusWire regw = {};

	int rrc = QmiRpc(s, node, port, tx++, kQmiImsaGetRegStatus, nullptr, 0,
			 r, sizeof(r), &rl, &rerr);
	if (rrc < 0) {
		close(s);
		return -1;
	}
	int src = -1;
	if (rerr == 0) {
		reg = parse_imsa_reg_status_tlvs(r, rl);
		if (reg) {
			size_t srl = 0;
			src = QmiRpc(s, node, port, tx++, kQmiImsaGetSrvStatus, nullptr, 0,
				     r, sizeof(r), &srl, &sqerr);
			if (src == 0 && sqerr == 0) {
				parse_imsa_srv_status_tlvs(r, srl, true, &svc, &regw);
				if (svc.supportImsVoice == 0 && svc.supportImsSms == 0) {
					svc.supportImsVoice = 1;
					svc.supportImsSms = 1;
				}
			} else {
				/* Registered at SIP layer; advertise LTE voice+SMS fallback */
				svc.supportImsVoice = 1;
				svc.supportImsSms = 1;
				svc.imsRegTech = OHOS_IMS_REG_TECH_LTE;
				regw.isRegisterd = 1;
				regw.imsRegTech = OHOS_IMS_REG_TECH_LTE;
			}
		}
	}
	close(s);

	static bool logged_once[2] = {false, false};
	if (verbose || (slot < 2 && !logged_once[slot])) {
		if (slot < 2)
			logged_once[slot] = true;
		Log("IMSA_POLL slot=%u bind_rc=%d bind_err=0x%x reg_rc=%d reg_err=0x%x srv_rc=%d srv_err=0x%x reg=%d voice=%u sms=%u tech=%d",
		    slot, brc, berr, rrc, rerr, src, sqerr, reg ? 1 : 0,
		    svc.supportImsVoice, svc.supportImsSms, regw.imsRegTech);
	}

	out->valid = true;
	out->svc = svc;
	out->reg = regw;
	return 0;
}

void PushCallback(const OHOS::sptr<OHOS::IRemoteObject> &cb, int32_t slot,
		  const ImsServiceStatusWire &svc, const ImsRegistrationStatusWire &reg)
{
	if (cb == nullptr)
		return;

	/* Advertise live MPSS supportImsSms (1 on VoLTE/VoNR) so SmsMms (SA 4008) routes
	 * China Telecom 4G/5G IMS SMS through our OHOS.Telephony.ImsSmsInterface stub (proxy type 0)
	 * -> QMI_WMS_RAW_SEND over IMS. */
	ImsServiceStatusWire wire_svc = svc;

	/* CRITICAL ORDER: push cb_cmd 0 (ImsServiceStatus) BEFORE cb_cmd 1 (ImsRegStatus)
	 * so NetworkSearchState::imsServiceStatus_ is populated when NotifyImsRegInfo runs! */
	OHOS::MessageParcel d0, r0;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	int rc0 = -1;
	if (d0.WriteInterfaceToken(std::u16string(kCallbackDescriptor)) &&
	    d0.WriteInt32(slot) &&
	    d0.WriteRawData(&wire_svc, sizeof(wire_svc))) {
		rc0 = cb->SendRequest(IMS_CORE_CB_SERVICE_STATUS, d0, r0, opt);
	}

	OHOS::MessageParcel d1, r1;
	int rc1 = -1;
	if (d1.WriteInterfaceToken(std::u16string(kCallbackDescriptor)) &&
	    d1.WriteInt32(slot) &&
	    d1.WriteRawData(&reg, sizeof(reg))) {
		rc1 = cb->SendRequest(IMS_CORE_CB_REG_STATUS, d1, r1, opt);
	}

	Log("IMSA_PUSH slot=%d reg=%u voice=%u video=%u sms_wire=%u(mpss=%u) tech=%d rc0=%d rc1=%d",
	    slot, reg.isRegisterd, wire_svc.supportImsVoice, wire_svc.supportImsVideo,
	    wire_svc.supportImsSms, svc.supportImsSms, reg.imsRegTech, rc0, rc1);
}

static int HexVal(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

static size_t DecodeHexBytes(const std::string &hex, uint8_t *out, size_t cap)
{
	size_t n = 0;
	for (size_t i = 0; i + 1 < hex.size() && n < cap; i += 2) {
		int hi = HexVal(hex[i]);
		int lo = HexVal(hex[i + 1]);
		if (hi < 0 || lo < 0)
			return 0;
		out[n++] = static_cast<uint8_t>((hi << 4) | lo);
	}
	return n;
}

static size_t EncodeSmscBcd(const char *addr, int tosca, uint8_t *out, size_t cap)
{
	if (!addr || !out || cap < 3)
		return 0;
	const char *p = addr;
	uint8_t ton = (tosca == 129) ? 0x81u : 0x91u;
	if (*p == '+') {
		ton = 0x91u;
		p++;
	}
	char digits[32];
	size_t nd = 0;
	while (*p && nd < sizeof(digits)) {
		if (*p >= '0' && *p <= '9')
			digits[nd++] = *p;
		p++;
	}
	if (nd == 0)
		return 0;
	size_t bcd_len = (nd + 1u) / 2u;
	if (2u + bcd_len > cap)
		return 0;
	out[0] = static_cast<uint8_t>(1u + bcd_len);
	out[1] = ton;
	for (size_t i = 0; i < bcd_len; ++i) {
		uint8_t lo = static_cast<uint8_t>(digits[2u * i] - '0');
		uint8_t hi = (2u * i + 1u < nd) ? static_cast<uint8_t>(digits[2u * i + 1u] - '0') : 0x0fu;
		out[2u + i] = static_cast<uint8_t>((hi << 4) | lo);
	}
	return 2u + bcd_len;
}

static size_t BuildWmsRawSendTlvs(uint8_t *req, size_t cap, uint8_t fmt,
				  const uint8_t *pdu, size_t pdu_len, bool on_ims)
{
	if (!req || !pdu || pdu_len == 0 || pdu_len > 255 || (3u + 3u + pdu_len + 4u) > cap)
		return 0;
	uint8_t raw_val[260];
	raw_val[0] = fmt;
	put_le16(raw_val + 1, static_cast<uint16_t>(pdu_len));
	std::memcpy(raw_val + 3, pdu, pdu_len);
	size_t off = tlv_append(req, 0x01, raw_val, static_cast<uint16_t>(3u + pdu_len));
	if (on_ims) {
		uint8_t ims_v = 1u;
		off += tlv_append(req + off, 0x13, &ims_v, 1);
	}
	return off;
}

struct ImsSmsSendJob {
	int32_t slotId;
	int64_t msgRefId;
	std::string smscPdu;
	std::string pdu;
	int32_t tech;
};

static void SendSmsCallbackReply(int32_t slotId, int64_t msgRefId, bool ok, int32_t msgRef, int32_t errCode)
{
	OHOS::sptr<OHOS::IRemoteObject> cb;
	{
		std::lock_guard<std::mutex> lock(gStateMutex);
		cb = gSmsCallback;
	}
	if (cb == nullptr) {
		Log("IMS_SMS_CB_MISSING slot=%d ok=%d", slotId, ok ? 1 : 0);
		return;
	}
	OHOS::MessageParcel d, r;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	int rc = -1;
	if (d.WriteInterfaceToken(std::u16string(kSmsCallbackDescriptor)) &&
	    d.WriteInt32(slotId)) {
		if (ok) {
			ImsSendSmsResultWire res = {};
			res.msgRef = msgRef;
			res.errCode = 0;
			res.flag = msgRefId;
			if (d.WriteRawData(&res, sizeof(res)))
				rc = cb->SendRequest(IMS_SMS_CB_SEND_MESSAGE, d, r, opt);
		} else {
			ImsRadioResponseInfoWire info = {};
			info.flag = static_cast<int32_t>(msgRefId);
			info.serial = 0;
			info.error = (errCode != 0) ? errCode : 1;
			info.type = 0;
			if (d.WriteRawData(&info, sizeof(info)))
				rc = cb->SendRequest(IMS_SMS_CB_SEND_MESSAGE, d, r, opt);
		}
	}
	Log("IMS_SMS_CB_DONE slot=%d ok=%d ref=%d err=%d ipc_rc=%d", slotId, ok ? 1 : 0, msgRef, errCode, rc);
}

static void *ImsSmsSendWorker(void *arg)
{
	ImsSmsSendJob *job = reinterpret_cast<ImsSmsSendJob *>(arg);
	if (!job)
		return nullptr;

	uint32_t wms_node = 0, wms_port = 0;
	if (!DiscoverQrtrEndpoint(kSvcWms, &wms_node, &wms_port)) {
		Log("IMS_SMS_SEND slot=%d err=no_wms_ep", job->slotId);
		SendSmsCallbackReply(job->slotId, job->msgRefId, false, 0, 1);
		delete job;
		return nullptr;
	}

	int s = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0) {
		SendSmsCallbackReply(job->slotId, job->msgRefId, false, 0, 1);
		delete job;
		return nullptr;
	}

	uint16_t tx = 1;
	uint8_t q[340], r[512];
	size_t rl = 0;
	uint16_t qerr = 0;

	/* Bind subscription (QMI_WMS_BIND_SUBSCRIPTION: 0=PRIMARY/slot0, 1=SECONDARY/slot1) */
	uint32_t qsub = (job->slotId == 1) ? 1u : 0u;
	uint8_t b4[4];
	put_le32(b4, qsub);
	size_t ql = tlv_append(q, 0x01, b4, 4);
	(void)QmiRpc(s, wms_node, wms_port, tx++, kQmiWmsBindSubscription, q, ql, r, sizeof(r), &rl, &qerr);

	/* Decode TPDU bytes */
	uint8_t tpdu[280];
	size_t tlen = DecodeHexBytes(job->pdu, tpdu, sizeof(tpdu));
	if (tlen == 0) {
		Log("IMS_SMS_SEND slot=%d err=bad_tpdu_hex len=%zu", job->slotId, job->pdu.size());
		close(s);
		SendSmsCallbackReply(job->slotId, job->msgRefId, false, 0, 1);
		delete job;
		return nullptr;
	}

	/* Build explicit SMSC BCD from smscPdu or SIM QMI_WMS_GET_SMSC_ADDRESS */
	uint8_t smsc_raw[24];
	size_t smsc_len = 0;
	if (!job->smscPdu.empty()) {
		if (job->smscPdu[0] == '+') {
			smsc_len = EncodeSmscBcd(job->smscPdu.c_str(), 145, smsc_raw, sizeof(smsc_raw));
		} else {
			size_t hlen = DecodeHexBytes(job->smscPdu, smsc_raw, sizeof(smsc_raw));
			if (hlen >= 2 && smsc_raw[0] == static_cast<uint8_t>(hlen - 1))
				smsc_len = hlen;
			else
				smsc_len = EncodeSmscBcd(job->smscPdu.c_str(), 145, smsc_raw, sizeof(smsc_raw));
		}
	}
	if (smsc_len == 0) {
		rl = 0;
		qerr = 0;
		if (QmiRpc(s, wms_node, wms_port, tx++, kQmiWmsGetSmscAddress, nullptr, 0,
			   r, sizeof(r), &rl, &qerr) == 0 && qerr == 0) {
			const uint8_t *vsc = nullptr;
			uint16_t vlsc = 0;
			if (ims_qmi_find_tlv(r, rl, 0x01, &vsc, &vlsc) == 0 && vlsc >= 4) {
				uint8_t alen = vsc[3];
				if (static_cast<size_t>(4u + alen) <= vlsc && alen > 0 && alen < 48) {
					char addr[48];
					std::memcpy(addr, vsc + 4, alen);
					addr[alen] = '\0';
					int tosca = (vsc[0] == '1' && vsc[1] == '2' && vsc[2] == '9') ? 129 : 145;
					smsc_len = EncodeSmscBcd(addr, tosca, smsc_raw, sizeof(smsc_raw));
				}
			}
		}
	}

	struct SendAttempt {
		uint8_t fmt;
		const uint8_t *data;
		size_t len;
		bool ims;
	} attempts[6];
	int natt = 0;
	uint8_t pdu_sc[300], pdu_def[300];
	size_t len_sc = 0, len_def = 0;

	if (job->tech == 1) {
		/* 3GPP2 CDMA PDU over IMS / CS */
		attempts[natt++] = {0x00u, tpdu, tlen, true};
		attempts[natt++] = {0x00u, tpdu, tlen, false};
	} else {
		/* 3GPP GSM SMS-SUBMIT over IMS (VoLTE/VoNR) with explicit SMSC + 0x00 default SMSC + CS fallback */
		if (smsc_len > 0 && smsc_len + tlen <= sizeof(pdu_sc)) {
			std::memcpy(pdu_sc, smsc_raw, smsc_len);
			std::memcpy(pdu_sc + smsc_len, tpdu, tlen);
			len_sc = smsc_len + tlen;
		}
		if (1u + tlen <= sizeof(pdu_def)) {
			pdu_def[0] = 0x00u;
			std::memcpy(pdu_def + 1, tpdu, tlen);
			len_def = 1u + tlen;
		}
		if (len_sc > 0)
			attempts[natt++] = {0x06u, pdu_sc, len_sc, true};
		if (len_def > 0)
			attempts[natt++] = {0x06u, pdu_def, len_def, true};
		if (len_sc > 0)
			attempts[natt++] = {0x06u, pdu_sc, len_sc, false};
		if (len_def > 0)
			attempts[natt++] = {0x06u, pdu_def, len_def, false};
	}

	int rc = -EIO;
	int32_t msg_ref = 0;
	qerr = 0;
	for (int ai = 0; ai < natt; ++ai) {
		ql = BuildWmsRawSendTlvs(q, sizeof(q), attempts[ai].fmt,
					 attempts[ai].data, attempts[ai].len, attempts[ai].ims);
		if (ql == 0)
			continue;
		rl = 0;
		qerr = 0;
		rc = QmiRpc(s, wms_node, wms_port, tx++, kQmiWmsRawSend, q, ql, r, sizeof(r), &rl, &qerr);
		Log("IMS_SMS_SEND slot=%d tech=%d att=%d fmt=0x%02x ims=%d sc=%zu tpdu=%zu rc=%d qerr=0x%x",
		    job->slotId, job->tech, ai, attempts[ai].fmt, attempts[ai].ims ? 1 : 0,
		    smsc_len, tlen, rc, qerr);
		if (rc == 0 && qerr == 0) {
			const uint8_t *v = nullptr;
			uint16_t vl = 0;
			if (ims_qmi_find_tlv(r, rl, 0x01, &v, &vl) == 0 && vl >= 2)
				msg_ref = static_cast<int32_t>(ims_get_le16(v));
			break;
		}
	}
	close(s);

	bool ok = (rc == 0 && qerr == 0);
	SendSmsCallbackReply(job->slotId, job->msgRefId, ok, msg_ref, ok ? 0 : (qerr ? static_cast<int32_t>(qerr) : 1));
	delete job;
	return nullptr;
}

struct ImsSmsCfgJob {
	int32_t slotId;
	bool isSet;
	int32_t cfgVal;
};

static void *ImsSmsConfigWorker(void *arg)
{
	ImsSmsCfgJob *job = reinterpret_cast<ImsSmsCfgJob *>(arg);
	if (!job)
		return nullptr;
	OHOS::sptr<OHOS::IRemoteObject> cb;
	{
		std::lock_guard<std::mutex> lock(gStateMutex);
		cb = gSmsCallback;
	}
	if (cb != nullptr) {
		OHOS::MessageParcel d, r;
		OHOS::MessageOption opt(0 /* TF_SYNC */);
		int rc = -1;
		if (d.WriteInterfaceToken(std::u16string(kSmsCallbackDescriptor)) &&
		    d.WriteInt32(job->slotId)) {
			if (job->isSet) {
				ImsRadioResponseInfoWire info = {};
				if (d.WriteRawData(&info, sizeof(info)))
					rc = cb->SendRequest(IMS_SMS_CB_SET_CONFIG, d, r, opt);
			} else {
				if (d.WriteInt32(job->cfgVal))
					rc = cb->SendRequest(IMS_SMS_CB_GET_CONFIG, d, r, opt);
			}
		}
		Log("IMS_SMS_CFG_CB slot=%d is_set=%d val=%d ipc_rc=%d",
		    job->slotId, job->isSet ? 1 : 0, job->cfgVal, rc);
	}
	delete job;
	return nullptr;
}

int DispatchSms(void *self, uint32_t code, OHOS::MessageParcel &data, OHOS::MessageParcel &reply,
		OHOS::MessageOption &option)
{
	(void)self;
	(void)option;
	std::u16string descriptor = data.ReadInterfaceToken();
	if (descriptor != std::u16string(kSmsServiceDescriptor)) {
		Log("IMS_SMS_REJECT bad descriptor code=%u", code);
		return -1;
	}

	if (code == IMS_SMS_CMD_REGISTER_CALLBACK) {
		OHOS::sptr<OHOS::IRemoteObject> cb = data.ReadRemoteObject();
		if (cb == nullptr) {
			Log("IMS_SMS_REG_CB null");
			reply.WriteInt32(-1);
			return 0;
		}
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			gSmsCallback = cb;
		}
		Log("IMS_SMS_REG_CB ok=1");
		return reply.WriteInt32(0) ? 0 : -1;
	}

	int32_t slotId = data.ReadInt32();
	if (code == IMS_SMS_CMD_GET_CONFIG) {
		int32_t val = 1;
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			if (slotId >= 0 && slotId < 2)
				val = gSmsConfig[slotId];
		}
		auto *job = new ImsSmsCfgJob{slotId, false, val};
		pthread_t tid;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&tid, &attr, &ImsSmsConfigWorker, job) != 0)
			delete job;
		pthread_attr_destroy(&attr);
		return reply.WriteInt32(0) ? 0 : -1;
	}

	if (code == IMS_SMS_CMD_SET_CONFIG) {
		int32_t enable = data.ReadInt32();
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			if (slotId >= 0 && slotId < 2)
				gSmsConfig[slotId] = enable ? 1 : 0;
		}
		auto *job = new ImsSmsCfgJob{slotId, true, enable};
		pthread_t tid;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&tid, &attr, &ImsSmsConfigWorker, job) != 0)
			delete job;
		pthread_attr_destroy(&attr);
		return reply.WriteInt32(0) ? 0 : -1;
	}

	if (code == IMS_SMS_CMD_SEND_MESSAGE) {
		int64_t msgRefId = data.ReadInt64();
		std::string smscPdu = data.ReadString();
		std::string pdu = data.ReadString();
		int32_t tech = data.ReadInt32();
		Log("IMS_SMS_REQ slot=%d ref=%lld tech=%d smsc_len=%zu pdu_len=%zu",
		    slotId, static_cast<long long>(msgRefId), tech, smscPdu.size(), pdu.size());
		auto *job = new ImsSmsSendJob{slotId, msgRefId, std::move(smscPdu), std::move(pdu), tech};
		pthread_t tid;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&tid, &attr, &ImsSmsSendWorker, job) != 0) {
			delete job;
			return reply.WriteInt32(-1) ? 0 : -1;
		}
		pthread_attr_destroy(&attr);
		return reply.WriteInt32(0) ? 0 : -1;
	}

	Log("IMS_SMS_UNSUPPORTED code=%u slot=%d", code, slotId);
	return reply.WriteInt32(-1) ? 0 : -1;
}

void *ImsaPollerThread(void *)
{
	uint32_t imsa_node = 0, imsa_port = 0;
	bool ep_valid = false;

	for (;;) {
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			if (gShutdown)
				break;
		}

		if (!ep_valid) {
			if (DiscoverImsaEndpoint(&imsa_node, &imsa_port)) {
				ep_valid = true;
				Log("IMSA_EP_FOUND node=%u port=%u", imsa_node, imsa_port);
			}
		}

		bool transport_err = false;
		for (int32_t slot = 0; slot < 2; ++slot) {
			SlotImsState cur = {};
			if (ep_valid) {
				if (PollSlotImsa(imsa_node, imsa_port, static_cast<uint32_t>(slot), &cur, false) < 0)
					transport_err = true;
			}

			OHOS::sptr<OHOS::IRemoteObject> cb;
			bool should_push = false;
			ImsServiceStatusWire push_svc = {};
			ImsRegistrationStatusWire push_reg = {};

			{
				std::lock_guard<std::mutex> lock(gStateMutex);
				bool changed = false;
				if (cur.valid) {
					if (!gSlotState[slot].valid ||
					    std::memcmp(&gSlotState[slot].svc, &cur.svc, sizeof(cur.svc)) != 0 ||
					    std::memcmp(&gSlotState[slot].reg, &cur.reg, sizeof(cur.reg)) != 0) {
						gSlotState[slot] = cur;
						changed = true;
					}
				}
				if (gCallback != nullptr && (changed || gForcePush[slot])) {
					cb = gCallback;
					push_svc = gSlotState[slot].svc;
					push_reg = gSlotState[slot].reg;
					gForcePush[slot] = false;
					should_push = true;
				}
			}

			if (should_push)
				PushCallback(cb, slot, push_svc, push_reg);
		}
		if (transport_err)
			ep_valid = false;

		struct timespec ts = {};
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 3;
		pthread_mutex_lock(&gCondMutex);
		pthread_cond_timedwait(&gStateCond, &gCondMutex, &ts);
		pthread_mutex_unlock(&gCondMutex);
	}
	return nullptr;
}

int Dispatch(void *self, uint32_t code, OHOS::MessageParcel &data, OHOS::MessageParcel &reply,
	     OHOS::MessageOption &option)
{
	(void)self;
	(void)option;
	std::u16string descriptor = data.ReadInterfaceToken();
	if (descriptor != std::u16string(kServiceDescriptor)) {
		Log("REJECT bad descriptor code=%u", code);
		return -1;
	}

	if (code == IMS_CORE_CMD_GET_REG_STATUS) {
		int32_t slotId = data.ReadInt32();
		bool ok = reply.WriteInt32(0);
		Log("GET_REG_STATUS slot=%d ok=%d", slotId, ok ? 1 : 0);
		WakePoller(slotId);
		return ok ? 0 : -1;
	}
	if (code == IMS_CORE_CMD_GET_IMPU_NUMBER) {
		int32_t slotId = data.ReadInt32();
		bool ok = reply.WriteInt32(0) && reply.WriteString(std::string());
		Log("GET_IMPU slot=%d ok=%d", slotId, ok ? 1 : 0);
		return ok ? 0 : -1;
	}
	if (code == IMS_CORE_CMD_REGISTER_CALLBACK) {
		OHOS::sptr<OHOS::IRemoteObject> cb = data.ReadRemoteObject();
		if (cb == nullptr) {
			Log("REGISTER_CB null");
			reply.WriteInt32(-1);
			return 0;
		}
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			gCallback = cb;
		}
		bool ok = reply.WriteInt32(0);
		Log("CB_REGISTERED ok=%d", ok ? 1 : 0);
		WakePoller(-1);
		return ok ? 0 : -1;
	}
	if (code == IMS_CORE_CMD_GET_PROXY_OBJ) {
		int32_t proxyType = data.ReadInt32();
		if (proxyType == IMS_PROXY_TYPE_SMS && gSmsStub != nullptr) {
			bool ok = reply.WriteRemoteObject(gSmsStub);
			Log("GET_PROXY type=%d(IMS_SMS) -> ImsSmsStub ok=%d", proxyType, ok ? 1 : 0);
			return ok ? 0 : -1;
		}
		/* Keep CellularCall (type=0) returning nullptr so VoLTE voice calling continues
		 * routing cleanly through CellularCall -> QMI RIL. */
		static std::atomic<uint32_t> proxyLogs{0};
		if (proxyLogs.fetch_add(1, std::memory_order_relaxed) < 8)
			Log("GET_PROXY type=%d -> nullptr", proxyType);
		return 0;
	}

	bool ok = reply.WriteInt32(-1);
	Log("UNSUPPORTED code=%u ok=%d", code, ok ? 1 : 0);
	return ok ? 0 : -1;
}

IpcApi LoadIpc()
{
	IpcApi api;
	void *lib = dlopen("libipc_single.z.so", RTLD_NOW | RTLD_GLOBAL);
	if (lib == nullptr) {
		Log("FAIL dlopen libipc_single: %s", dlerror());
		return api;
	}
	api.ctor = reinterpret_cast<StubCtor>(dlsym(
		lib, "_ZN4OHOS13IPCObjectStubC1ENSt3__h12basic_stringIDsNS1_11char_traitsIDsEENS1_9allocatorIDsEEEEb"));
	api.originalDispatch = reinterpret_cast<StubDispatch>(dlsym(
		lib, "_ZN4OHOS13IPCObjectStub15OnRemoteRequestEjRNS_13MessageParcelES2_RNS_13MessageOptionE"));
	api.setThreads = reinterpret_cast<SetThreads>(
		dlsym(lib, "_ZN4OHOS11IPCSkeleton19SetMaxWorkThreadNumEi"));
	api.joinThread = reinterpret_cast<JoinThread>(
		dlsym(lib, "_ZN4OHOS11IPCSkeleton14JoinWorkThreadEv"));
	if (api.ctor == nullptr || api.originalDispatch == nullptr ||
	    api.setThreads == nullptr || api.joinThread == nullptr) {
		Log("FAIL resolve IPCObjectStub/IPCSkeleton symbols");
	}
	return api;
}

void *BuildCustomStub(const IpcApi &api, const char16_t *desc, StubDispatch fn, uintptr_t *tableBuf, const char *name)
{
	if (api.ctor == nullptr || api.originalDispatch == nullptr)
		return nullptr;
	void *object = ::operator new(kStubObjectBytes);
	std::memset(object, 0, kStubObjectBytes);
	api.ctor(object, std::u16string(desc), false);

	void **table = *reinterpret_cast<void ***>(object);
	if (table == nullptr || table[kDispatchSlot] != reinterpret_cast<void *>(api.originalDispatch)) {
		Log("FAIL donor stub vtable guard (%s)", name);
		return nullptr;
	}
	std::memcpy(tableBuf, reinterpret_cast<const char *>(table) - kPrimaryPrefix * sizeof(void *),
		    kVtableEntries * sizeof(uintptr_t));
	tableBuf[kPrimaryPrefix + kDispatchSlot] = reinterpret_cast<uintptr_t>(fn);
	*reinterpret_cast<void ***>(object) =
		reinterpret_cast<void **>(tableBuf + kPrimaryPrefix);
	Log("STUB_READY name=%s slot=%zu", name, kDispatchSlot);
	return object;
}

void *BuildStub(const IpcApi &api)
{
	alignas(16) static uintptr_t coreTable[kVtableEntries];
	return BuildCustomStub(api, kServiceDescriptor, &Dispatch, coreTable, "ImsCoreServiceInterface");
}

void *BuildSmsStub(const IpcApi &api)
{
	alignas(16) static uintptr_t smsTable[kVtableEntries];
	return BuildCustomStub(api, kSmsServiceDescriptor, &DispatchSms, smsTable, "ImsSmsInterface");
}

SamgrApi LoadSamgr()
{
	SamgrApi api;
	static const char *const paths[] = {
		"libsamgr_proxy.z.so",
		"/system/lib64/chipset-sdk-sp/libsamgr_proxy.z.so",
		"/system/lib64/platformsdk/libsamgr_proxy.z.so",
		"/system/lib64/libsamgr_proxy.z.so",
	};
	for (const char *path : paths) {
		api.lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
		if (api.lib != nullptr)
			break;
	}
	if (api.lib == nullptr) {
		Log("FAIL dlopen libsamgr_proxy: %s", dlerror());
		return api;
	}
	api.instance = reinterpret_cast<GetInstance>(
		dlsym(api.lib, "_ZN4OHOS26SystemAbilityManagerClient11GetInstanceEv"));
	api.manager = reinterpret_cast<GetManager>(
		dlsym(api.lib, "_ZN4OHOS26SystemAbilityManagerClient23GetSystemAbilityManagerEv"));
	api.addSystemAbility = reinterpret_cast<AddAbility>(dlsym(
		api.lib,
		"_ZThn112_N4OHOS25SystemAbilityManagerProxy16AddSystemAbilityEiRKNS_4sptrINS_13IRemoteObjectEEERKNS_21ISystemAbilityManager11SAExtraPropE"));
	if (api.instance == nullptr || api.manager == nullptr || api.addSystemAbility == nullptr)
		Log("FAIL resolve SystemAbilityManager symbols");
	return api;
}

bool RegisterAbility(const SamgrApi &api, const OHOS::sptr<OHOS::IRemoteObject> &stub)
{
	void *client = api.instance();
	if (client == nullptr) {
		Log("FAIL samgr client");
		return false;
	}
	SptrCarrier manager = reinterpret_cast<SptrCarrier (*)(void *)>(api.manager)(client);
	if (manager.ptr == nullptr) {
		Log("FAIL samgr manager");
		return false;
	}
	SAExtraProp extra{false, 0, std::u16string(), std::u16string()};
	int rc = api.addSystemAbility(manager.ptr, MARBLE_IMS_CORE_SA_ID, &stub, &extra);
	Log("ADD_SA id=%d rc=%d", MARBLE_IMS_CORE_SA_ID, rc);
	return rc == 0;
}

}  // namespace

int main(int argc, char **argv)
{
	if (argc >= 2 && std::strcmp(argv[1], "--probe") == 0) {
		uint32_t n = 0, p = 0;
		if (!DiscoverImsaEndpoint(&n, &p)) {
			std::fprintf(stderr, "HMOS_IMS_CORE PROBE_NO_IMSA\n");
			return 1;
		}
		std::fprintf(stderr, "HMOS_IMS_CORE PROBE_EP node=%u port=%u\n", n, p);
		for (uint32_t s = 0; s < 2; ++s) {
			SlotImsState st = {};
			int rc = PollSlotImsa(n, p, s, &st, true);
			std::fprintf(stderr, "HMOS_IMS_CORE PROBE_SLOT slot=%u rc=%d reg=%u voice=%u sms=%u tech=%d\n",
				     s, rc, st.reg.isRegisterd, st.svc.supportImsVoice,
				     st.svc.supportImsSms, st.reg.imsRegTech);
		}
		return 0;
	}

	OpenLog();
	Log("BOOT pid=%d uid=%d ims-core-qmi-bridge", getpid(), getuid());

	IpcApi ipc = LoadIpc();
	void *stub = BuildStub(ipc);
	if (stub == nullptr) {
		Log("FAIL stub");
		return 2;
	}
	OHOS::sptr<OHOS::IRemoteObject> stubRef(reinterpret_cast<OHOS::IRemoteObject *>(stub));

	void *smsStub = BuildSmsStub(ipc);
	if (smsStub != nullptr) {
		gSmsStub = OHOS::sptr<OHOS::IRemoteObject>(reinterpret_cast<OHOS::IRemoteObject *>(smsStub));
	}

	SamgrApi samgr = LoadSamgr();
	bool registered = false;
	for (int attempt = 0; attempt < 60 && !registered; ++attempt) {
		registered = RegisterAbility(samgr, stubRef);
		if (!registered) {
			if (attempt == 0)
				Log("WAIT samgr not ready, retrying");
			sleep(1);
		}
	}
	if (!registered) {
		Log("FAIL register SA %d", MARBLE_IMS_CORE_SA_ID);
		return 3;
	}

	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&tid, &attr, &ImsaPollerThread, nullptr) != 0)
		Log("WARN poller thread create failed");
	pthread_attr_destroy(&attr);

	if (!ipc.setThreads(4))
		Log("WARN SetMaxWorkThreadNum failed");

	Log("READY sa=%d", MARBLE_IMS_CORE_SA_ID);
	ipc.joinThread();
	{
		std::lock_guard<std::mutex> lock(gStateMutex);
		gShutdown = true;
	}
	Log("EXIT binder loop returned");
	return 0;
}
