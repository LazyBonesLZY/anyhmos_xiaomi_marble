/* SPDX-License-Identifier: MIT
 * Clean-room OHOS SA 4014 (ImsCoreService) QMI_IMSA / QMI_WMS / QMI_VOICE bridge
 * for Xiaomi marble (SM7475).
 *
 * Bridges live Qualcomm MPSS QMI services into OHOS Telephony:
 *   - QMI_IMSA (0x21) -> CoreService (SA 4010) via ImsCoreServiceCallback
 *   - QMI_WMS  (0x05) -> SmsMms (SA 4008) via OHOS.Telephony.ImsSmsInterface (proxy type 1),
 *     including 3GPP2 C.S0015 -> 3GPP TS 23.040 SMS-SUBMIT transcoding for VoLTE/VoNR IMS SMS
 *   - QMI_VOICE (0x09) + QMI_IMSS (0x12) -> CellularCall (SA 4006) via
 *     OHOS.Telephony.ImsCallInterface (proxy type 0) for HD VoLTE/VoNR calling,
 *     coordinated with libril_vendor_qmi.c via /data/service/el1/public/telephony/ims-call-active.
 *
 * Privacy: never logs phone numbers, IMPU/IMSI/ICCID, or SMS content.
 */
#include "ims_core_wire.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
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
constexpr uint32_t kSvcVoice = 0x09u;
constexpr uint32_t kSvcImss = 0x12u;
constexpr uint32_t kSvcImsa = 0x21u;

constexpr uint16_t kQmiWmsRawSend = 0x0020u;
constexpr uint16_t kQmiWmsGetSmscAddress = 0x0034u;
constexpr uint16_t kQmiWmsBindSubscription = 0x004cu; /* u8 sub in TLV 0x01 */

constexpr uint16_t kQmiVoiceIndRegister = 0x0003u;
constexpr uint16_t kQmiVoiceDialCall = 0x0020u;
constexpr uint16_t kQmiVoiceEndCall = 0x0021u;
constexpr uint16_t kQmiVoiceAnswerCall = 0x0022u;
constexpr uint16_t kQmiVoiceBurstDtmf = 0x0028u;
constexpr uint16_t kQmiVoiceStartContDtmf = 0x0029u;
constexpr uint16_t kQmiVoiceStopContDtmf = 0x002au;
constexpr uint16_t kQmiVoiceAllCallStatusInd = 0x002eu;
constexpr uint16_t kQmiVoiceGetAllCallInfo = 0x002fu;
constexpr uint16_t kQmiVoiceManageCalls = 0x0031u;
constexpr uint16_t kQmiVoiceBindSubscription = 0x0044u; /* u8 sub in TLV 0x01 */

constexpr uint16_t kQmiImssSetServicesEnabled = 0x008fu;
constexpr uint16_t kQmiImssGetServicesEnabled = 0x0090u;
constexpr uint16_t kQmiImssBind = 0x0098u; /* u32 sub in TLV 0x01 */

constexpr uint16_t kQmiImsaGetRegStatus = 0x0020u;
constexpr uint16_t kQmiImsaGetSrvStatus = 0x0021u;
constexpr uint16_t kQmiImsaBind = 0x0033u; /* u32 sub in TLV 0x10 */

constexpr size_t kMaxVoiceCalls = 8;

const char16_t *const kServiceDescriptor = u"OHOS.Telephony.ImsCoreServiceInterface";
const char16_t *const kCallbackDescriptor = u"OHOS.Telephony.ImsCoreServiceCallback";
const char16_t *const kSmsServiceDescriptor = u"OHOS.Telephony.ImsSmsInterface";
const char16_t *const kSmsCallbackDescriptor = u"OHOS.Telephony.ImsSmsCallback";
const char16_t *const kCallServiceDescriptor = u"OHOS.Telephony.ImsCallInterface";
const char16_t *const kCallCallbackDescriptor = u"OHOS.Telephony.ImsCallCallback";

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
OHOS::sptr<OHOS::IRemoteObject> gCallStub;
OHOS::sptr<OHOS::IRemoteObject> gCallCallback;

int32_t gSmsConfig[2] = {1, 1};
int32_t gImsSwitch[2] = {1, 1};
int32_t gCallMute[2] = {0, 0};
int32_t gLastCallFailReason[2] = {16, 16};
ImsVoiceCallEntry gImsCalls[2][kMaxVoiceCalls];
size_t gImsCallCnt[2] = {0, 0};
bool gHangupPending[2] = {false, false};
int gVoiceWatchTicks[2] = {0, 0};

SlotImsState gSlotState[2];
bool gForcePush[2] = {false, false};
bool gShutdown = false;

std::mutex gVoiceRpcMutex;
uint32_t gVoiceNode = 0;
uint32_t gVoicePort = 0;
bool gVoiceEpValid = false;
int gVoiceRpcSock[2] = {-1, -1};
bool gVoiceRpcBound[2] = {false, false};
uint16_t gVoiceRpcTx = 1;

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

void RemoveImsCallMarker()
{
	(void)unlink(MARBLE_IMS_CALL_ACTIVE_PATH);
}

void WriteImsCallMarker()
{
	int fd = open(MARBLE_IMS_CALL_ACTIVE_PATH, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		Log("IMS_CALL_MARKER create_failed errno=%d", errno);
		return;
	}
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%d\n", static_cast<int>(getpid()));
	if (n > 0)
		(void)write(fd, buf, static_cast<size_t>(n));
	close(fd);
	Log("IMS_CALL_MARKER active=1 pid=%d", static_cast<int>(getpid()));
}

void ShutdownSignalHandler(int sig)
{
	(void)unlink(MARBLE_IMS_CALL_ACTIVE_PATH);
	_exit(128 + sig);
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
		int pr = poll(&pfd, 1, 1500);
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

	/* Qualcomm MPSS QMI_IMSA defaults to sub 0, so QMI_IMSA_BIND(0) can return
	 * QMI_ERR_NO_EFFECT (0x001a) or non-zero qerr on empty slots. Only transport
	 * failures (rc < 0) invalidate the QRTR endpoint. */
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

	ImsServiceStatusWire wire_svc = svc;

	/* CRITICAL ORDER: push cb_cmd 0 (ImsServiceStatus) BEFORE cb_cmd 1 (ImsRegStatus)
	 * so NetworkSearchState::imsServiceStatus_ is populated when NotifyImsRegInfo runs. */
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

/* ==================== IMS SMS (OHOS.Telephony.ImsSmsInterface) ==================== */

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
	const std::u16string token(kSmsCallbackDescriptor);
	if (ok) {
		ImsSendSmsResultWire res = {};
		res.msgRef = msgRef;
		res.errCode = 0;
		res.flag = msgRefId;
		if (ims_write_sms_send_ok_parcel(d, token, slotId, res))
			rc = cb->SendRequest(IMS_SMS_CB_SEND_MESSAGE, d, r, opt);
	} else {
		ImsRadioResponseInfoWire info = {};
		info.flag = static_cast<int32_t>(msgRefId);
		info.serial = 0;
		info.error = (errCode != 0) ? errCode : 1;
		info.type = 0;
		if (d.WriteInterfaceToken(token) &&
		    d.WriteInt32(slotId) &&
		    d.WriteRawData(&info, sizeof(info)))
			rc = cb->SendRequest(IMS_SMS_CB_SEND_MESSAGE, d, r, opt);
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

	/* Bind subscription (QMI_WMS_BIND_SUBSCRIPTION 0x004c: 1-byte u8 sub in TLV 0x01) */
	uint8_t qsub = (job->slotId == 1) ? 1u : 0u;
	size_t ql = tlv_append(q, 0x01, &qsub, 1);
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
		bool transcoded;
	} attempts[6];
	int natt = 0;
	uint8_t pdu_sc[300], pdu_def[300];
	size_t len_sc = 0, len_def = 0;

	if (ims_pdu_is_cdma_p2p(tpdu, tlen, job->tech)) {
		/* Genuine 3GPP2 C.S0015 CDMA P2P PDU over 4G/5G VoLTE/VoNR IMS:
		 * SM7475 MPSS rejects raw fmt=0x00 on IMS with qerr=0x3a.
		 * Transcode 3GPP2 C.S0015 -> 3GPP TS 23.040 GSM SMS-SUBMIT (fmt=0x06) first. */
		bool have_gsm_sc = (smsc_len > 0) &&
			ims_transcode_cdma_pdu_to_gsm_submit(tpdu, tlen, smsc_raw, smsc_len,
							     pdu_sc, sizeof(pdu_sc), &len_sc);
		bool have_gsm_def = ims_transcode_cdma_pdu_to_gsm_submit(tpdu, tlen, nullptr, 0,
									 pdu_def, sizeof(pdu_def), &len_def);
		if (!have_gsm_sc && !have_gsm_def) {
			Log("IMS_SMS_TRANSCODE_FAIL slot=%d tech=%d tpdu_len=%zu hdr=%02x%02x%02x%02x",
			    job->slotId, job->tech, tlen,
			    tlen > 0 ? tpdu[0] : 0u, tlen > 1 ? tpdu[1] : 0u,
			    tlen > 2 ? tpdu[2] : 0u, tlen > 3 ? tpdu[3] : 0u);
		}
		if (have_gsm_sc)
			attempts[natt++] = {0x06u, pdu_sc, len_sc, true, true};
		if (have_gsm_def)
			attempts[natt++] = {0x06u, pdu_def, len_def, true, true};
		if (have_gsm_sc)
			attempts[natt++] = {0x06u, pdu_sc, len_sc, false, true};
		if (have_gsm_def)
			attempts[natt++] = {0x06u, pdu_def, len_def, false, true};
		attempts[natt++] = {0x00u, tpdu, tlen, true, false};
		attempts[natt++] = {0x00u, tpdu, tlen, false, false};
	} else {
		/* 3GPP TS 23.040 GSM SMS-SUBMIT over IMS (VoLTE/VoNR, tech=1 or 0) with explicit SMSC
		 * + 0x00 default SMSC + CS fallback (always fmt=0x06 GW_PP). */
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
			attempts[natt++] = {0x06u, pdu_sc, len_sc, true, false};
		if (len_def > 0)
			attempts[natt++] = {0x06u, pdu_def, len_def, true, false};
		if (len_sc > 0)
			attempts[natt++] = {0x06u, pdu_sc, len_sc, false, false};
		if (len_def > 0)
			attempts[natt++] = {0x06u, pdu_def, len_def, false, false};
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
		Log("IMS_SMS_SEND slot=%d tech=%d att=%d fmt=0x%02x ims=%d transcoded=%d sc=%zu tpdu=%zu pdu=%zu rc=%d qerr=0x%x",
		    job->slotId, job->tech, ai, attempts[ai].fmt, attempts[ai].ims ? 1 : 0,
		    attempts[ai].transcoded ? 1 : 0, smsc_len, tlen, attempts[ai].len, rc, qerr);
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

/* ==================== IMS CALL (OHOS.Telephony.ImsCallInterface) ==================== */

static int VoiceQmiRpc(int32_t slotId, uint16_t msg_id,
		       const uint8_t *req_tlvs, size_t req_len,
		       uint8_t *resp_tlvs, size_t resp_cap,
		       size_t *out_len, uint16_t *out_err)
{
	int slot = (slotId == 1) ? 1 : 0;
	std::lock_guard<std::mutex> lock(gVoiceRpcMutex);

	for (int retry = 0; retry < 2; ++retry) {
		if (!gVoiceEpValid) {
			if (!DiscoverQrtrEndpoint(kSvcVoice, &gVoiceNode, &gVoicePort))
				return -ENODEV;
			gVoiceEpValid = true;
		}
		if (gVoiceRpcSock[slot] < 0) {
			gVoiceRpcSock[slot] = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
			if (gVoiceRpcSock[slot] < 0)
				return -errno;
			gVoiceRpcBound[slot] = false;
		}
		if (!gVoiceRpcBound[slot]) {
			uint8_t bq[8], br[256];
			uint8_t sub = static_cast<uint8_t>(slot);
			size_t bl = tlv_append(bq, 0x01, &sub, 1);
			size_t brl = 0;
			uint16_t berr = 0;
			int brc = QmiRpc(gVoiceRpcSock[slot], gVoiceNode, gVoicePort, gVoiceRpcTx++,
					 kQmiVoiceBindSubscription, bq, bl, br, sizeof(br), &brl, &berr);
			if (brc < 0) {
				close(gVoiceRpcSock[slot]);
				gVoiceRpcSock[slot] = -1;
				gVoiceRpcBound[slot] = false;
				gVoiceEpValid = false;
				continue;
			}
			if (berr == 0 || berr == 0x001au)
				gVoiceRpcBound[slot] = true;
		}

		int rc = QmiRpc(gVoiceRpcSock[slot], gVoiceNode, gVoicePort, gVoiceRpcTx++,
				msg_id, req_tlvs, req_len, resp_tlvs, resp_cap, out_len, out_err);
		if (rc == -EPIPE || rc == -ENOTCONN || rc == -ECONNRESET) {
			close(gVoiceRpcSock[slot]);
			gVoiceRpcSock[slot] = -1;
			gVoiceRpcBound[slot] = false;
			gVoiceEpValid = false;
			continue;
		}
		return rc;
	}
	return -EIO;
}

static bool QueryImssVoLteEnabled(int32_t slotId, bool *out_enabled)
{
	uint32_t n = 0, p = 0;
	if (!DiscoverQrtrEndpoint(kSvcImss, &n, &p))
		return false;
	int s = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return false;
	uint8_t q[16], r[256], b4[4];
	size_t rl = 0;
	uint16_t qerr = 0, tx = 1;
	put_le32(b4, (slotId == 1) ? 1u : 0u);
	size_t ql = tlv_append(q, 0x01, b4, 4);
	bool ok = false;
	if (QmiRpc(s, n, p, tx++, kQmiImssBind, q, ql, r, sizeof(r), &rl, &qerr) == 0) {
		rl = 0;
		qerr = 0;
		if (QmiRpc(s, n, p, tx++, kQmiImssGetServicesEnabled, nullptr, 0,
			   r, sizeof(r), &rl, &qerr) == 0 && qerr == 0) {
			const uint8_t *v = nullptr;
			uint16_t vl = 0;
			if (ims_qmi_find_tlv(r, rl, 0x10, &v, &vl) == 0 && vl >= 1) {
				*out_enabled = (v[0] != 0u);
				ok = true;
			} else if (ims_qmi_find_tlv(r, rl, 0x18, &v, &vl) == 0 && vl >= 1) {
				*out_enabled = (v[0] != 0u);
				ok = true;
			}
		}
	}
	close(s);
	return ok;
}

static void ApplyImssVoLteEnabled(int32_t slotId, bool enable)
{
	uint32_t n = 0, p = 0;
	if (!DiscoverQrtrEndpoint(kSvcImss, &n, &p))
		return;
	int s = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return;
	uint8_t q[48], r[256], b4[4];
	size_t rl = 0;
	uint16_t qerr = 0, tx = 1;
	put_le32(b4, (slotId == 1) ? 1u : 0u);
	size_t ql = tlv_append(q, 0x01, b4, 4);
	if (QmiRpc(s, n, p, tx++, kQmiImssBind, q, ql, r, sizeof(r), &rl, &qerr) == 0) {
		uint8_t val = enable ? 1u : 0u;
		ql = 0;
		ql += tlv_append(q + ql, 0x10, &val, 1); /* VoLTE */
		ql += tlv_append(q + ql, 0x18, &val, 1); /* IMS */
		if (enable) {
			ql += tlv_append(q + ql, 0x11, &val, 1); /* VT */
			ql += tlv_append(q + ql, 0x12, &val, 1); /* Mobile data */
			ql += tlv_append(q + ql, 0x1a, &val, 1); /* SMS over IMS */
		}
		(void)QmiRpc(s, n, p, tx++, kQmiImssSetServicesEnabled, q, ql, r, sizeof(r), &rl, &qerr);
	}
	close(s);
}

static size_t FilterIpCalls(const ImsVoiceCallEntry *all, size_t n_all,
			    ImsVoiceCallEntry *out, size_t max_out)
{
	size_t n = 0;
	for (size_t i = 0; i < n_all && n < max_out; ++i) {
		if (ims_is_ip_call_type(all[i].callType))
			out[n++] = all[i];
	}
	return n;
}

static OHOS::sptr<OHOS::IRemoteObject> GetCallCallback()
{
	std::lock_guard<std::mutex> lock(gStateMutex);
	return gCallCallback;
}

static void SendCallRadioResp(uint32_t cbCode, int32_t slotId, int32_t error)
{
	auto cb = GetCallCallback();
	if (cb == nullptr) {
		Log("IMS_CALL_CB_MISSING code=%u slot=%d", cbCode, slotId);
		return;
	}
	OHOS::MessageParcel d, r;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	ImsRadioResponseInfoWire info = {};
	info.flag = 0;
	info.serial = 0;
	info.error = error;
	info.type = 0;
	int rc = -1;
	if (d.WriteInterfaceToken(std::u16string(kCallCallbackDescriptor)) &&
	    d.WriteInt32(slotId) &&
	    d.WriteRawData(&info, sizeof(info))) {
		rc = cb->SendRequest(cbCode, d, r, opt);
	}
	Log("IMS_CALL_CB_RESP code=%u slot=%d err=%d ipc_rc=%d", cbCode, slotId, error, rc);
}

static void SendCallSendDtmfResp(int32_t slotId, int32_t callIndex, int32_t error)
{
	auto cb = GetCallCallback();
	if (cb == nullptr)
		return;
	OHOS::MessageParcel d, r;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	ImsRadioResponseInfoWire info = {};
	info.error = error;
	int rc = -1;
	/* Binary-verified OnSendDtmfResponseInner: ReadInt32(slotId), ReadInt32(callIndex), ReadRawData(16) */
	if (d.WriteInterfaceToken(std::u16string(kCallCallbackDescriptor)) &&
	    d.WriteInt32(slotId) &&
	    d.WriteInt32(callIndex) &&
	    d.WriteRawData(&info, sizeof(info))) {
		rc = cb->SendRequest(IMS_CALL_CB_SEND_DTMF, d, r, opt);
	}
	Log("IMS_CALL_CB_SEND_DTMF slot=%d idx=%d err=%d ipc_rc=%d", slotId, callIndex, error, rc);
}

static void SendCallStateChangeReport(int32_t slotId)
{
	auto cb = GetCallCallback();
	if (cb == nullptr)
		return;
	OHOS::MessageParcel d, r;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	int rc = -1;
	if (d.WriteInterfaceToken(std::u16string(kCallCallbackDescriptor)) &&
	    d.WriteInt32(slotId)) {
		rc = cb->SendRequest(IMS_CALL_CB_CALL_STATE_CHANGE, d, r, opt);
	}
	Log("IMS_CALL_STATE_REPORT slot=%d ipc_rc=%d", slotId, rc);
}

static void SendCallLastFailReason(int32_t slotId, int32_t reason)
{
	auto cb = GetCallCallback();
	if (cb == nullptr)
		return;
	OHOS::MessageParcel d, r;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	int rc = -1;
	if (ims_write_last_fail_reason_parcel(d, std::u16string(kCallCallbackDescriptor),
					      slotId, reason, std::string())) {
		rc = cb->SendRequest(IMS_CALL_CB_LAST_CALL_FAIL_REASON, d, r, opt);
	}
	Log("IMS_CALL_FAIL_REASON slot=%d reason=%d ipc_rc=%d", slotId, reason, rc);
}

static void SendCallsDataResponse(int32_t slotId, int32_t flag,
				  const ImsVoiceCallEntry *calls, size_t count)
{
	auto cb = GetCallCallback();
	if (cb == nullptr)
		return;
	OHOS::MessageParcel d, r;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	int rc = -1;
	if (ims_write_calls_data_parcel(d, std::u16string(kCallCallbackDescriptor),
					slotId, flag, calls, count)) {
		rc = cb->SendRequest(IMS_CALL_CB_GET_CALLS_DATA, d, r, opt);
	}
	Log("IMS_CALL_DATA_RESP slot=%d flag=%d cnt=%zu ipc_rc=%d", slotId, flag, count, rc);
}

static void SendGetImsSwitchResponse(int32_t slotId, int32_t active)
{
	auto cb = GetCallCallback();
	if (cb == nullptr)
		return;
	OHOS::MessageParcel d, r;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	int rc = -1;
	if (ims_write_get_ims_switch_parcel(d, std::u16string(kCallCallbackDescriptor), slotId, active))
		rc = cb->SendRequest(IMS_CALL_CB_GET_IMS_SWITCH, d, r, opt);
	Log("IMS_CALL_GET_SWITCH_RESP slot=%d active=%d ipc_rc=%d", slotId, active, rc);
}

static void SendSetMuteResponse(int32_t slotId, int32_t muteVal)
{
	auto cb = GetCallCallback();
	if (cb == nullptr)
		return;
	OHOS::MessageParcel d, r;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	ImsMuteControlResponseWire resp = {0, muteVal};
	int rc = -1;
	if (d.WriteInterfaceToken(std::u16string(kCallCallbackDescriptor)) &&
	    d.WriteInt32(slotId) &&
	    d.WriteRawData(&resp, sizeof(resp))) {
		rc = cb->SendRequest(IMS_CALL_CB_SET_MUTE, d, r, opt);
	}
	Log("IMS_CALL_SET_MUTE_RESP slot=%d mute=%d ipc_rc=%d", slotId, muteVal, rc);
}

static void SendGetImsCapResponse(int32_t slotId, const ImsCapFromChipWire &cap)
{
	auto cb = GetCallCallback();
	if (cb == nullptr)
		return;
	OHOS::MessageParcel d, r;
	OHOS::MessageOption opt(0 /* TF_SYNC */);
	int rc = -1;
	if (ims_write_get_ims_cap_parcel(d, std::u16string(kCallCallbackDescriptor), slotId, cap))
		rc = cb->SendRequest(IMS_CALL_CB_GET_IMS_CAP, d, r, opt);
	Log("IMS_CALL_GET_CAP_RESP slot=%d volte=%d vowifi=%d vonr=%d ut=%d ipc_rc=%d",
	    slotId, cap.volteCap, cap.vowifiCap, cap.vonrCap, cap.utCap, rc);
}

struct ImsCallJob {
	uint32_t code = 0;
	int32_t slotId = 0;
	int32_t arg0 = 0;
	int32_t arg1 = 0;
	int64_t flag64 = 0;
	char dtmfChar = 0;
	ImsCallInfoWire callInfo = {};
};

static uint8_t ResolveIncomingCallId(int32_t slotId, int32_t hintIndex)
{
	if (hintIndex > 0 && hintIndex <= 255)
		return static_cast<uint8_t>(hintIndex);
	int slot = (slotId == 1) ? 1 : 0;
	{
		std::lock_guard<std::mutex> lock(gStateMutex);
		for (size_t i = 0; i < gImsCallCnt[slot]; ++i) {
			if (gImsCalls[slot][i].state == 4 || gImsCalls[slot][i].state == 5)
				return static_cast<uint8_t>(gImsCalls[slot][i].index);
		}
		if (gImsCallCnt[slot] > 0)
			return static_cast<uint8_t>(gImsCalls[slot][0].index);
	}
	uint8_t resp[512];
	size_t rl = 0;
	uint16_t qerr = 0;
	if (VoiceQmiRpc(slotId, kQmiVoiceGetAllCallInfo, nullptr, 0,
			resp, sizeof(resp), &rl, &qerr) == 0 && qerr == 0) {
		ImsVoiceCallEntry all[kMaxVoiceCalls], ip[kMaxVoiceCalls];
		size_t n_all = ims_parse_voice_call_list(resp, rl, false, all, kMaxVoiceCalls);
		size_t n_ip = FilterIpCalls(all, n_all, ip, kMaxVoiceCalls);
		const ImsVoiceCallEntry *use = (n_ip > 0) ? ip : all;
		size_t n_use = (n_ip > 0) ? n_ip : n_all;
		for (size_t i = 0; i < n_use; ++i) {
			if (use[i].state == 4 || use[i].state == 5)
				return static_cast<uint8_t>(use[i].index);
		}
		if (n_use > 0)
			return static_cast<uint8_t>(use[0].index);
	}
	return 1u;
}

static void *ImsCallWorker(void *arg)
{
	ImsCallJob *job = reinterpret_cast<ImsCallJob *>(arg);
	if (!job)
		return nullptr;

	int32_t slotId = job->slotId;
	int slot = (slotId == 1) ? 1 : 0;
	uint8_t qreq[160], qresp[512];
	size_t rl = 0;
	uint16_t qerr = 0;

	switch (job->code) {
	case IMS_CALL_CMD_DIAL: {
		job->callInfo.phoneNum[sizeof(job->callInfo.phoneNum) - 1] = '\0';
		size_t nlen = std::strlen(job->callInfo.phoneNum);
		const int try_types[3] = {
			static_cast<int>(QMI_VOICE_CALL_TYPE_VOICE_IP),
			static_cast<int>(QMI_VOICE_CALL_TYPE_CS),
			-1,
		};
		int rc = -EIO;
		int used_type = -2;
		for (int ti = 0; ti < 3; ++ti) {
			size_t ql = ims_build_voice_dial_tlvs(qreq, sizeof(qreq),
							      job->callInfo.phoneNum, try_types[ti]);
			if (ql == 0)
				continue;
			rl = 0;
			qerr = 0;
			rc = VoiceQmiRpc(slotId, kQmiVoiceDialCall, qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
			used_type = try_types[ti];
			if (rc == 0 && qerr == 0)
				break;
		}
		bool ok = (rc == 0 && qerr == 0);
		Log("IMS_CALL_DIAL slot=%d num_len=%zu video=%d type=%d rc=%d qerr=0x%x",
		    slotId, nlen, job->callInfo.videoState, used_type, rc, qerr);
		if (ok) {
			std::lock_guard<std::mutex> lock(gStateMutex);
			gHangupPending[slot] = false;
			gLastCallFailReason[slot] = 16;
			gVoiceWatchTicks[slot] = 30;
		}
		SendCallRadioResp(IMS_CALL_CB_DIAL, slotId, ok ? 0 : 1);
		if (ok)
			SendCallStateChangeReport(slotId);
		break;
	}
	case IMS_CALL_CMD_HANG_UP: {
		int rc = -EIO;
		bool already_empty = false;
		if (job->callInfo.index > 0 && job->callInfo.index <= 255) {
			uint8_t cid = static_cast<uint8_t>(job->callInfo.index);
			size_t ql = tlv_append(qreq, 0x01, &cid, 1);
			rl = 0;
			qerr = 0;
			rc = VoiceQmiRpc(slotId, kQmiVoiceEndCall, qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		}
		if (rc != 0 || qerr != 0) {
			/* Fallback: refresh active calls and end any live IP call IDs */
			size_t crl = 0;
			uint16_t cqerr = 0;
			if (VoiceQmiRpc(slotId, kQmiVoiceGetAllCallInfo, nullptr, 0,
					qresp, sizeof(qresp), &crl, &cqerr) == 0 && cqerr == 0) {
				ImsVoiceCallEntry all[kMaxVoiceCalls], ip[kMaxVoiceCalls];
				size_t n_all = ims_parse_voice_call_list(qresp, crl, false, all, kMaxVoiceCalls);
				size_t n_ip = FilterIpCalls(all, n_all, ip, kMaxVoiceCalls);
				if (n_ip == 0) {
					already_empty = true;
					rc = 0;
					qerr = 0;
				} else {
					for (size_t i = 0; i < n_ip; ++i) {
						uint8_t ecid = static_cast<uint8_t>(ip[i].index);
						size_t eql = tlv_append(qreq, 0x01, &ecid, 1);
						size_t erl = 0;
						uint16_t eqerr = 0;
						int erc = VoiceQmiRpc(slotId, kQmiVoiceEndCall, qreq, eql,
								      qresp, sizeof(qresp), &erl, &eqerr);
						if (erc == 0 && eqerr == 0) {
							rc = 0;
							qerr = 0;
						}
					}
				}
			}
		}
		bool ok = (rc == 0 && qerr == 0);
		Log("IMS_CALL_HANGUP slot=%d idx=%d empty=%d rc=%d qerr=0x%x",
		    slotId, job->callInfo.index, already_empty ? 1 : 0, rc, qerr);
		if (ok) {
			std::lock_guard<std::mutex> lock(gStateMutex);
			gLastCallFailReason[slot] = 16;
			if (already_empty) {
				gImsCallCnt[slot] = 0;
				gHangupPending[slot] = false;
				gVoiceWatchTicks[slot] = 0;
			} else {
				/* Wait for QMI_VOICE_ALL_CALL_STATUS_IND or 350ms watchdog tick to confirm
				 * SIP BYE completed before firing CallStateChangeReport. */
				gHangupPending[slot] = true;
				gVoiceWatchTicks[slot] = 20;
			}
		}
		SendCallRadioResp(IMS_CALL_CB_HANG_UP, slotId, ok ? 0 : 1);
		if (already_empty)
			SendCallStateChangeReport(slotId);
		break;
	}
	case IMS_CALL_CMD_REJECT_WITH_REASON: {
		uint8_t cid = ResolveIncomingCallId(slotId, job->callInfo.index);
		size_t ql = tlv_append(qreq, 0x01, &cid, 1);
		rl = 0;
		qerr = 0;
		int rc = VoiceQmiRpc(slotId, kQmiVoiceEndCall, qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		bool ok = (rc == 0 && qerr == 0);
		Log("IMS_CALL_REJECT slot=%d cid=%u reason=%d rc=%d qerr=0x%x",
		    slotId, cid, job->arg0, rc, qerr);
		if (ok) {
			std::lock_guard<std::mutex> lock(gStateMutex);
			gLastCallFailReason[slot] = 21; /* CALL_REJECTED */
			gHangupPending[slot] = true;
			gVoiceWatchTicks[slot] = 20;
		}
		SendCallRadioResp(IMS_CALL_CB_REJECT, slotId, ok ? 0 : 1);
		break;
	}
	case IMS_CALL_CMD_ANSWER: {
		uint8_t cid = ResolveIncomingCallId(slotId, job->callInfo.index);
		size_t ql = tlv_append(qreq, 0x01, &cid, 1);
		rl = 0;
		qerr = 0;
		int rc = VoiceQmiRpc(slotId, kQmiVoiceAnswerCall, qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		bool ok = (rc == 0 && qerr == 0);
		Log("IMS_CALL_ANSWER slot=%d cid=%u rc=%d qerr=0x%x", slotId, cid, rc, qerr);
		if (ok) {
			std::lock_guard<std::mutex> lock(gStateMutex);
			gHangupPending[slot] = false;
			gVoiceWatchTicks[slot] = 30;
		}
		SendCallRadioResp(IMS_CALL_CB_ANSWER, slotId, ok ? 0 : 1);
		if (ok)
			SendCallStateChangeReport(slotId);
		break;
	}
	case IMS_CALL_CMD_HOLD_CALL:
	case IMS_CALL_CMD_UNHOLD_CALL:
	case IMS_CALL_CMD_SWITCH_CALL:
	case IMS_CALL_CMD_COMBINE_CONFERENCE: {
		uint8_t stype = (job->code == IMS_CALL_CMD_COMBINE_CONFERENCE) ? 0x04u : 0x03u;
		size_t ql = tlv_append(qreq, 0x01, &stype, 1);
		rl = 0;
		qerr = 0;
		int rc = VoiceQmiRpc(slotId, kQmiVoiceManageCalls, qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		bool ok = (rc == 0 && qerr == 0);
		uint32_t cbCode = IMS_CALL_CB_HOLD_CALL;
		if (job->code == IMS_CALL_CMD_UNHOLD_CALL)
			cbCode = IMS_CALL_CB_UNHOLD_CALL;
		else if (job->code == IMS_CALL_CMD_SWITCH_CALL)
			cbCode = IMS_CALL_CB_SWITCH_CALL;
		else if (job->code == IMS_CALL_CMD_COMBINE_CONFERENCE)
			cbCode = IMS_CALL_CB_COMBINE_CONFERENCE;
		Log("IMS_CALL_MANAGE cmd=%u slot=%d stype=%u rc=%d qerr=0x%x",
		    job->code, slotId, stype, rc, qerr);
		SendCallRadioResp(cbCode, slotId, ok ? 0 : 1);
		if (ok)
			SendCallStateChangeReport(slotId);
		break;
	}
	case IMS_CALL_CMD_INVITE_TO_CONFERENCE: {
		Log("IMS_CALL_INVITE_CONF slot=%d unsupported", slotId);
		SendCallRadioResp(IMS_CALL_CB_INVITE_TO_CONFERENCE, slotId, 1);
		break;
	}
	case IMS_CALL_CMD_GET_CALLS_DATA: {
		ImsVoiceCallEntry all[kMaxVoiceCalls], ip[kMaxVoiceCalls];
		size_t n_all = 0, n_ip = 0;
		rl = 0;
		qerr = 0;
		int rc = VoiceQmiRpc(slotId, kQmiVoiceGetAllCallInfo, nullptr, 0,
				     qresp, sizeof(qresp), &rl, &qerr);
		if (rc == 0 && qerr == 0) {
			n_all = ims_parse_voice_call_list(qresp, rl, false, all, kMaxVoiceCalls);
			n_ip = FilterIpCalls(all, n_all, ip, kMaxVoiceCalls);
			std::lock_guard<std::mutex> lock(gStateMutex);
			gImsCallCnt[slot] = n_ip;
			if (n_ip > 0)
				std::memcpy(gImsCalls[slot], ip, n_ip * sizeof(ip[0]));
		}
		Log("IMS_CALL_GET_DATA slot=%d flag=%lld rc=%d qerr=0x%x total=%zu ip=%zu",
		    slotId, static_cast<long long>(job->flag64), rc, qerr, n_all, n_ip);
		SendCallsDataResponse(slotId, static_cast<int32_t>(job->flag64), ip, n_ip);
		break;
	}
	case IMS_CALL_CMD_GET_LAST_FAIL_REASON: {
		int32_t reason = 16;
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			if (gLastCallFailReason[slot] > 0)
				reason = gLastCallFailReason[slot];
		}
		SendCallLastFailReason(slotId, reason);
		break;
	}
	case IMS_CALL_CMD_START_DTMF:
	case IMS_CALL_CMD_SEND_DTMF:
	case IMS_CALL_CMD_STOP_DTMF: {
		uint8_t cid = (job->arg0 > 0 && job->arg0 <= 255) ? static_cast<uint8_t>(job->arg0) : 1u;
		uint16_t mid = kQmiVoiceStopContDtmf;
		size_t ql = 0;
		if (job->code == IMS_CALL_CMD_START_DTMF) {
			uint8_t d[2] = {cid, static_cast<uint8_t>(job->dtmfChar ? job->dtmfChar : '0')};
			ql = tlv_append(qreq, 0x01, d, 2);
			mid = kQmiVoiceStartContDtmf;
		} else if (job->code == IMS_CALL_CMD_SEND_DTMF) {
			uint8_t d[3] = {cid, 1u, static_cast<uint8_t>(job->dtmfChar ? job->dtmfChar : '0')};
			ql = tlv_append(qreq, 0x01, d, 3);
			mid = kQmiVoiceBurstDtmf;
		} else {
			ql = tlv_append(qreq, 0x01, &cid, 1);
			mid = kQmiVoiceStopContDtmf;
		}
		rl = 0;
		qerr = 0;
		int rc = VoiceQmiRpc(slotId, mid, qreq, ql, qresp, sizeof(qresp), &rl, &qerr);
		bool ok = (rc == 0 && qerr == 0);
		Log("IMS_CALL_DTMF cmd=%u slot=%d cid=%u rc=%d qerr=0x%x", job->code, slotId, cid, rc, qerr);
		if (job->code == IMS_CALL_CMD_START_DTMF)
			SendCallRadioResp(IMS_CALL_CB_START_DTMF, slotId, ok ? 0 : 1);
		else if (job->code == IMS_CALL_CMD_SEND_DTMF)
			SendCallSendDtmfResp(slotId, job->arg0, ok ? 0 : 1);
		else
			SendCallRadioResp(IMS_CALL_CB_STOP_DTMF, slotId, ok ? 0 : 1);
		break;
	}
	case IMS_CALL_CMD_SET_IMS_SWITCH: {
		bool enable = (job->arg0 != 0);
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			gImsSwitch[slot] = enable ? 1 : 0;
		}
		ApplyImssVoLteEnabled(slotId, enable);
		Log("IMS_CALL_SET_SWITCH slot=%d active=%d", slotId, enable ? 1 : 0);
		SendCallRadioResp(IMS_CALL_CB_SET_IMS_SWITCH, slotId, 0);
		WakePoller(slotId);
		break;
	}
	case IMS_CALL_CMD_GET_IMS_SWITCH: {
		int32_t active = 1;
		bool imss_en = false;
		if (QueryImssVoLteEnabled(slotId, &imss_en)) {
			active = imss_en ? 1 : 0;
		} else {
			std::lock_guard<std::mutex> lock(gStateMutex);
			active = (gSlotState[slot].svc.supportImsVoice ||
				  gSlotState[slot].reg.isRegisterd ||
				  gImsSwitch[slot]) ? 1 : 0;
		}
		SendGetImsSwitchResponse(slotId, active);
		break;
	}
	case IMS_CALL_CMD_SET_MUTE: {
		int32_t muteVal = job->arg0 ? 1 : 0;
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			gCallMute[slot] = muteVal;
		}
		SendSetMuteResponse(slotId, muteVal);
		break;
	}
	case IMS_CALL_CMD_GET_IMS_CAPS: {
		ImsCapFromChipWire cap = {1, 0, 1, 1};
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			if (gSlotState[slot].valid && gSlotState[slot].reg.isRegisterd) {
				cap.volteCap = 1;
				cap.vonrCap = 1;
				cap.utCap = gSlotState[slot].svc.supportImsUt ? 1 : 1;
			}
		}
		SendGetImsCapResponse(slotId, cap);
		break;
	}
	default:
		break;
	}

	delete job;
	return nullptr;
}

static bool SpawnCallJob(ImsCallJob *job)
{
	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&tid, &attr, &ImsCallWorker, job);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		delete job;
		return false;
	}
	return true;
}

int DispatchCall(void *self, uint32_t code, OHOS::MessageParcel &data, OHOS::MessageParcel &reply,
		 OHOS::MessageOption &option)
{
	(void)self;
	(void)option;
	std::u16string descriptor = data.ReadInterfaceToken();
	if (descriptor != std::u16string(kCallServiceDescriptor)) {
		Log("IMS_CALL_REJECT bad descriptor code=%u", code);
		return -1;
	}

	if (code == IMS_CALL_CMD_REGISTER_CALLBACK) {
		OHOS::sptr<OHOS::IRemoteObject> cb = data.ReadRemoteObject();
		if (cb == nullptr) {
			Log("IMS_CALL_REG_CB null");
			reply.WriteInt32(-1);
			return 0;
		}
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			gCallCallback = cb;
		}
		WriteImsCallMarker();
		Log("IMS_CALL_REG_CB ok=1");
		return reply.WriteInt32(0) ? 0 : -1;
	}

	/* ImsCallInfo (276B raw data) commands: Dial(5000), HangUp(5001), RejectWithReason(5002), Answer(5003) */
	if (code == IMS_CALL_CMD_DIAL || code == IMS_CALL_CMD_HANG_UP ||
	    code == IMS_CALL_CMD_REJECT_WITH_REASON || code == IMS_CALL_CMD_ANSWER) {
		const void *raw = data.ReadRawData(sizeof(ImsCallInfoWire));
		if (raw == nullptr) {
			Log("IMS_CALL_REQ code=%u err=bad_call_info", code);
			return reply.WriteInt32(-1) ? 0 : -1;
		}
		auto *job = new ImsCallJob();
		job->code = code;
		std::memcpy(&job->callInfo, raw, sizeof(ImsCallInfoWire));
		job->callInfo.phoneNum[sizeof(job->callInfo.phoneNum) - 1] = '\0';
		job->slotId = job->callInfo.slotId;
		if (code == IMS_CALL_CMD_DIAL || code == IMS_CALL_CMD_REJECT_WITH_REASON)
			job->arg0 = data.ReadInt32();
		Log("IMS_CALL_REQ code=%u slot=%d idx=%d video=%d num_len=%zu",
		    code, job->slotId, job->callInfo.index, job->callInfo.videoState,
		    std::strlen(job->callInfo.phoneNum));
		return reply.WriteInt32(SpawnCallJob(job) ? 0 : -1) ? 0 : -1;
	}

	/* Commands without leading slotId (ImsConfigItem / FeatureType) */
	if (code == IMS_CALL_CMD_SET_IMS_CFG_STR || code == IMS_CALL_CMD_SET_IMS_CFG_INT ||
	    code == IMS_CALL_CMD_GET_IMS_CFG || code == IMS_CALL_CMD_SET_IMS_FEATURE ||
	    code == IMS_CALL_CMD_GET_IMS_FEATURE) {
		int32_t item = data.ReadInt32();
		Log("IMS_CALL_CFG code=%u item=%d -> ok", code, item);
		return reply.WriteInt32(0) ? 0 : -1;
	}

	int32_t slotId = data.ReadInt32();

	if (code == IMS_CALL_CMD_HOLD_CALL || code == IMS_CALL_CMD_UNHOLD_CALL ||
	    code == IMS_CALL_CMD_SWITCH_CALL) {
		int32_t callType = data.ReadInt32();
		(void)data.ReadBool();
		auto *job = new ImsCallJob();
		job->code = code;
		job->slotId = slotId;
		job->arg0 = callType;
		return reply.WriteInt32(SpawnCallJob(job) ? 0 : -1) ? 0 : -1;
	}

	if (code == IMS_CALL_CMD_COMBINE_CONFERENCE || code == IMS_CALL_CMD_INVITE_TO_CONFERENCE ||
	    code == IMS_CALL_CMD_GET_LAST_FAIL_REASON || code == IMS_CALL_CMD_GET_IMS_SWITCH ||
	    code == IMS_CALL_CMD_GET_IMS_CAPS) {
		auto *job = new ImsCallJob();
		job->code = code;
		job->slotId = slotId;
		Log("IMS_CALL_REQ code=%u slot=%d", code, slotId);
		return reply.WriteInt32(SpawnCallJob(job) ? 0 : -1) ? 0 : -1;
	}

	if (code == IMS_CALL_CMD_GET_CALLS_DATA) {
		int64_t flag = data.ReadInt64();
		auto *job = new ImsCallJob();
		job->code = code;
		job->slotId = slotId;
		job->flag64 = flag;
		return reply.WriteInt32(SpawnCallJob(job) ? 0 : -1) ? 0 : -1;
	}

	if (code == IMS_CALL_CMD_START_DTMF || code == IMS_CALL_CMD_SEND_DTMF) {
		int8_t ch = data.ReadInt8();
		int32_t callIndex = data.ReadInt32();
		auto *job = new ImsCallJob();
		job->code = code;
		job->slotId = slotId;
		job->dtmfChar = static_cast<char>(ch);
		job->arg0 = callIndex;
		return reply.WriteInt32(SpawnCallJob(job) ? 0 : -1) ? 0 : -1;
	}

	if (code == IMS_CALL_CMD_STOP_DTMF) {
		int32_t callIndex = data.ReadInt32();
		auto *job = new ImsCallJob();
		job->code = code;
		job->slotId = slotId;
		job->arg0 = callIndex;
		return reply.WriteInt32(SpawnCallJob(job) ? 0 : -1) ? 0 : -1;
	}

	if (code == IMS_CALL_CMD_SET_IMS_SWITCH || code == IMS_CALL_CMD_SET_MUTE) {
		int32_t val = data.ReadInt32();
		auto *job = new ImsCallJob();
		job->code = code;
		job->slotId = slotId;
		job->arg0 = val;
		Log("IMS_CALL_REQ code=%u slot=%d val=%d", code, slotId, val);
		return reply.WriteInt32(SpawnCallJob(job) ? 0 : -1) ? 0 : -1;
	}

	if (code == IMS_CALL_CMD_UPDATE_IMS_CAPS) {
		Log("IMS_CALL_UPDATE_CAPS slot=%d -> ok", slotId);
		WakePoller(slotId);
		return reply.WriteInt32(0) ? 0 : -1;
	}

	if (code == IMS_CALL_CMD_CAN_SET_CF_TIME) {
		bool ok = reply.WriteBool(false) && reply.WriteInt32(0);
		return ok ? 0 : -1;
	}

	if (code == IMS_CALL_CMD_SET_DOMAIN_PREF || code == IMS_CALL_CMD_GET_DOMAIN_PREF ||
	    code == IMS_CALL_CMD_SET_CARRIER_VT_CFG || code == IMS_CALL_CMD_GET_MUTE ||
	    code == IMS_CALL_CMD_GET_UT_IMPU || code == IMS_CALL_CMD_NOTIFY_OP_CFG_CHANGED ||
	    code == IMS_CALL_CMD_KICK_OUT_CONFERENCE || code == IMS_CALL_CMD_UPDATE_MEDIA_REQ ||
	    code == IMS_CALL_CMD_UPDATE_MEDIA_RESP || code == IMS_CALL_CMD_CANCEL_CALL_UPGRADE ||
	    code == IMS_CALL_CMD_REQ_CAMERA_CAPS) {
		Log("IMS_CALL_ACK code=%u slot=%d -> ok", code, slotId);
		return reply.WriteInt32(0) ? 0 : -1;
	}

	Log("IMS_CALL_UNSUPPORTED code=%u slot=%d", code, slotId);
	return reply.WriteInt32(0) ? 0 : -1;
}

void *VoiceIndThread(void *)
{
	uint32_t vnode = 0, vport = 0;
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			if (gShutdown)
				return nullptr;
		}
		if (DiscoverQrtrEndpoint(kSvcVoice, &vnode, &vport)) {
			Log("IMS_VOICE_EP_FOUND node=%u port=%u", vnode, vport);
			break;
		}
		sleep(2);
	}

	int sv[2] = {-1, -1};
	uint16_t tx = 1;
	int opened = 0;
	for (int s = 0; s < 2; ++s) {
		sv[s] = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (sv[s] < 0)
			continue;
		uint8_t q[48], r[256];
		uint8_t sub = static_cast<uint8_t>(s);
		size_t rl = 0;
		uint16_t berr = 0, rerr = 0;
		size_t ql = tlv_append(q, 0x01, &sub, 1);
		int brc = QmiRpc(sv[s], vnode, vport, tx++, kQmiVoiceBindSubscription, q, ql, r, sizeof(r), &rl, &berr);
		uint8_t on = 1u;
		ql = 0;
		ql += tlv_append(q + ql, 0x10, &on, 1); /* DTMF */
		ql += tlv_append(q + ql, 0x12, &on, 1); /* SS */
		ql += tlv_append(q + ql, 0x13, &on, 1); /* Call notif */
		ql += tlv_append(q + ql, 0x14, &on, 1); /* Handover */
		ql += tlv_append(q + ql, 0x15, &on, 1); /* Speech codec */
		rl = 0;
		int rrc = QmiRpc(sv[s], vnode, vport, tx++, kQmiVoiceIndRegister, q, ql, r, sizeof(r), &rl, &rerr);
		Log("IMS_VOICE_IND_INIT slot=%d bind_rc=%d bind_err=0x%x reg_rc=%d reg_err=0x%x",
		    s, brc, berr, rrc, rerr);
		opened++;
	}
	if (opened == 0)
		return nullptr;

	for (;;) {
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			if (gShutdown)
				break;
		}

		struct pollfd pfds[2];
		int pslot[2];
		nfds_t nf = 0;
		for (int s = 0; s < 2; ++s) {
			if (sv[s] >= 0) {
				pfds[nf].fd = sv[s];
				pfds[nf].events = POLLIN;
				pfds[nf].revents = 0;
				pslot[nf] = s;
				nf++;
			}
		}

		bool watch_slot[2] = {false, false};
		bool any_watch = false;
		{
			std::lock_guard<std::mutex> lock(gStateMutex);
			for (int s = 0; s < 2; ++s) {
				if (gImsCallCnt[s] > 0 || gHangupPending[s] || gVoiceWatchTicks[s] > 0) {
					watch_slot[s] = true;
					any_watch = true;
				}
			}
		}

		int pr = poll(pfds, nf, any_watch ? 350 : 5000);
		if (pr == 0) {
			/* Watchdog poll: ensures CallStateChangeReport fires if SIP BYE/ALERT IND drops. */
			for (int s = 0; s < 2; ++s) {
				if (!watch_slot[s] || sv[s] < 0)
					continue;
				uint8_t vresp[512];
				size_t vrl = 0;
				uint16_t vqerr = 0;
				if (QmiRpc(sv[s], vnode, vport, tx++, kQmiVoiceGetAllCallInfo,
					   nullptr, 0, vresp, sizeof(vresp), &vrl, &vqerr) == 0 && vqerr == 0) {
					ImsVoiceCallEntry all[kMaxVoiceCalls], ip[kMaxVoiceCalls];
					size_t n_all = ims_parse_voice_call_list(vresp, vrl, false, all, kMaxVoiceCalls);
					size_t n_ip = FilterIpCalls(all, n_all, ip, kMaxVoiceCalls);
					bool changed = false;
					{
						std::lock_guard<std::mutex> lock(gStateMutex);
						if (n_ip != gImsCallCnt[s]) {
							changed = true;
						} else {
							for (size_t k = 0; k < n_ip; ++k) {
								if (ip[k].index != gImsCalls[s][k].index ||
								    ip[k].state != gImsCalls[s][k].state) {
									changed = true;
									break;
								}
							}
						}
						if (n_ip == 0 && gHangupPending[s])
							changed = true;
						gImsCallCnt[s] = n_ip;
						if (n_ip > 0)
							std::memcpy(gImsCalls[s], ip, n_ip * sizeof(ip[0]));
						if (n_ip == 0) {
							gHangupPending[s] = false;
							gVoiceWatchTicks[s] = 0;
						} else if (gVoiceWatchTicks[s] > 0) {
							gVoiceWatchTicks[s]--;
						}
					}
					if (changed) {
						Log("IMS_VOICE_WATCH slot=%d total=%zu ip=%zu", s, n_all, n_ip);
						SendCallStateChangeReport(s);
					}
				} else {
					std::lock_guard<std::mutex> lock(gStateMutex);
					if (gVoiceWatchTicks[s] > 0)
						gVoiceWatchTicks[s]--;
					if (gVoiceWatchTicks[s] == 0)
						gHangupPending[s] = false;
				}
			}
			continue;
		}
		if (pr < 0)
			continue;

		for (nfds_t i = 0; i < nf; ++i) {
			if ((pfds[i].revents & POLLIN) == 0)
				continue;
			uint8_t buf[1024];
			struct sockaddr_qrtr src = {};
			socklen_t sl = sizeof(src);
			ssize_t n = recvfrom(pfds[i].fd, buf, sizeof(buf), 0,
					     reinterpret_cast<struct sockaddr *>(&src), &sl);
			if (n < 7 || src.sq_port == QRTR_PORT_CTRL || buf[0] != 0x04)
				continue;
			uint16_t msg = ims_get_le16(buf + 3);
			uint16_t plen = ims_get_le16(buf + 5);
			if (7u + plen > static_cast<size_t>(n))
				continue;
			if (msg != kQmiVoiceAllCallStatusInd)
				continue;

			int s = pslot[i];
			const uint8_t *tlvs = buf + 7;
			ImsVoiceCallEntry all[kMaxVoiceCalls], ip[kMaxVoiceCalls];
			size_t n_all = ims_parse_voice_call_list(tlvs, plen, true, all, kMaxVoiceCalls);
			size_t n_ip = FilterIpCalls(all, n_all, ip, kMaxVoiceCalls);
			int32_t end_reason = ims_parse_voice_call_end_reason(tlvs, plen);

			size_t prev_ip = 0;
			bool was_hangup = false;
			{
				std::lock_guard<std::mutex> lock(gStateMutex);
				prev_ip = gImsCallCnt[s];
				was_hangup = gHangupPending[s];
				gImsCallCnt[s] = n_ip;
				if (n_ip > 0) {
					std::memcpy(gImsCalls[s], ip, n_ip * sizeof(ip[0]));
				} else {
					gLastCallFailReason[s] = end_reason;
					gHangupPending[s] = false;
					gVoiceWatchTicks[s] = 0;
				}
			}

			uint8_t first_ct = (n_all > 0) ? all[0].callType : 0xffu;
			int32_t first_st = (n_ip > 0) ? ip[0].state : -1;
			Log("IMS_VOICE_IND slot=%d total=%zu ip=%zu prev_ip=%zu first_ct=0x%02x first_st=%d end=%d",
			    s, n_all, n_ip, prev_ip, first_ct, first_st, end_reason);

			if (n_ip > 0 || prev_ip > 0 || was_hangup)
				SendCallStateChangeReport(s);
		}
	}

	for (int s = 0; s < 2; ++s) {
		if (sv[s] >= 0)
			close(sv[s]);
	}
	return nullptr;
}

/* ==================== IMSA Poller & Core SA 4014 Dispatch ==================== */

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
		if (proxyType == IMS_PROXY_TYPE_CALL && gCallStub != nullptr) {
			bool ok = reply.WriteRemoteObject(gCallStub);
			Log("GET_PROXY type=%d(IMS_CALL) -> ImsCallStub ok=%d", proxyType, ok ? 1 : 0);
			return ok ? 0 : -1;
		}
		if (proxyType == IMS_PROXY_TYPE_SMS && gSmsStub != nullptr) {
			bool ok = reply.WriteRemoteObject(gSmsStub);
			Log("GET_PROXY type=%d(IMS_SMS) -> ImsSmsStub ok=%d", proxyType, ok ? 1 : 0);
			return ok ? 0 : -1;
		}
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

void *BuildCallStub(const IpcApi &api)
{
	alignas(16) static uintptr_t callTable[kVtableEntries];
	return BuildCustomStub(api, kCallServiceDescriptor, &DispatchCall, callTable, "ImsCallInterface");
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

	/* Remove any stale marker left by an earlier crashed instance before registering SA 4014. */
	RemoveImsCallMarker();
	std::signal(SIGTERM, &ShutdownSignalHandler);
	std::signal(SIGINT, &ShutdownSignalHandler);

	OpenLog();
	Log("BOOT pid=%d uid=%d ims-core-qmi-bridge-m5", getpid(), getuid());

	IpcApi ipc = LoadIpc();
	void *stub = BuildStub(ipc);
	if (stub == nullptr) {
		Log("FAIL stub");
		return 2;
	}
	OHOS::sptr<OHOS::IRemoteObject> stubRef(reinterpret_cast<OHOS::IRemoteObject *>(stub));

	void *smsStub = BuildSmsStub(ipc);
	if (smsStub != nullptr)
		gSmsStub = OHOS::sptr<OHOS::IRemoteObject>(reinterpret_cast<OHOS::IRemoteObject *>(smsStub));

	void *callStub = BuildCallStub(ipc);
	if (callStub != nullptr)
		gCallStub = OHOS::sptr<OHOS::IRemoteObject>(reinterpret_cast<OHOS::IRemoteObject *>(callStub));

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
		Log("WARN imsa poller thread create failed");
	if (pthread_create(&tid, &attr, &VoiceIndThread, nullptr) != 0)
		Log("WARN voice ind thread create failed");
	pthread_attr_destroy(&attr);

	if (!ipc.setThreads(8))
		Log("WARN SetMaxWorkThreadNum failed");

	Log("READY sa=%d call_stub=%d sms_stub=%d",
	    MARBLE_IMS_CORE_SA_ID, gCallStub != nullptr ? 1 : 0, gSmsStub != nullptr ? 1 : 0);
	ipc.joinThread();
	{
		std::lock_guard<std::mutex> lock(gStateMutex);
		gShutdown = true;
	}
	RemoveImsCallMarker();
	Log("EXIT binder loop returned");
	return 0;
}
