/* SPDX-License-Identifier: MIT
 * Clean-room QMI_IMSA / QMI_VOICE / QMI_WMS / OHOS SA 4014 (ImsCoreService)
 * wire layout & helpers.
 *
 * Binary-verified wire contracts (donor libtel_core_service.z.so,
 * libtel_sms_mms.z.so, libtel_cellular_call.z.so, libipc_single.z.so):
 *   SA ID               : 4014 (TELEPHONY_IMS_SYS_ABILITY_ID)
 *   Core descriptor     : u"OHOS.Telephony.ImsCoreServiceInterface"
 *     code   0 : GetImsRegistrationStatus(int32 slotId) -> reply int32(0) + async cb
 *     code   1 : GetPhoneNumberFromIMPU(int32 slotId) -> reply int32(0) + string("")
 *     code 100 : RegisterImsCoreServiceCallback(IRemoteObject cb) -> reply int32(0)
 *     code 101 : GetProxyObjectPtr(int32 proxyType) -> 0=ImsCallStub, 1=ImsSmsStub
 *   Core cb descriptor  : u"OHOS.Telephony.ImsCoreServiceCallback"
 *     cb_cmd 0 : onImsServiceStatusReportInner(int32 slotId, ReadRawData(8)) -> reply int32
 *     cb_cmd 1 : OnGetImsRegistrationStatusResponseInner(int32 slotId, ReadRawData(8)) -> reply int32
 *   SMS descriptor      : u"OHOS.Telephony.ImsSmsInterface" (6000/6100/6101/6500)
 *   SMS cb descriptor   : u"OHOS.Telephony.ImsSmsCallback" (0/1/2)
 *   Call descriptor     : u"OHOS.Telephony.ImsCallInterface" (5000..5500)
 *   Call cb descriptor  : u"OHOS.Telephony.ImsCallCallback" (0..63)
 */
#ifndef MARBLE_IMS_CORE_WIRE_H
#define MARBLE_IMS_CORE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

#define MARBLE_IMS_CORE_SA_ID 4014

/* Runtime coordination marker created by hmos_ims_core_service once CellularCall
 * registers ImsCallCallback, unlinked on startup and shutdown. */
#define MARBLE_IMS_CALL_ACTIVE_PATH "/data/service/el1/public/telephony/ims-call-active"

#define IMS_CORE_CMD_GET_REG_STATUS 0u
#define IMS_CORE_CMD_GET_IMPU_NUMBER 1u
#define IMS_CORE_CMD_REGISTER_CALLBACK 100u
#define IMS_CORE_CMD_GET_PROXY_OBJ 101u

#define IMS_CORE_CB_SERVICE_STATUS 0u
#define IMS_CORE_CB_REG_STATUS 1u

/* OHOS ImsServiceProxyType enum (binary-verified: ImsCallClient::GetImsCallProxy passes w1=0,
 * ImsSmsClient::GetImsSmsProxy passes w1=1). */
#define IMS_PROXY_TYPE_CALL 0
#define IMS_PROXY_TYPE_SMS 1

/* OHOS ImsSmsInterface & ImsSmsCallback IPC codes (binary-verified in libtel_sms_mms.z.so). */
#define IMS_SMS_CMD_SEND_MESSAGE 6000u
#define IMS_SMS_CMD_SET_CONFIG 6100u
#define IMS_SMS_CMD_GET_CONFIG 6101u
#define IMS_SMS_CMD_REGISTER_CALLBACK 6500u

#define IMS_SMS_CB_SEND_MESSAGE 0u
#define IMS_SMS_CB_SET_CONFIG 1u
#define IMS_SMS_CB_GET_CONFIG 2u

/* OHOS ImsCallInterface IPC request codes (binary-verified in libtel_cellular_call.z.so). */
#define IMS_CALL_CMD_DIAL 5000u
#define IMS_CALL_CMD_HANG_UP 5001u
#define IMS_CALL_CMD_REJECT_WITH_REASON 5002u
#define IMS_CALL_CMD_ANSWER 5003u
#define IMS_CALL_CMD_HOLD_CALL 5004u
#define IMS_CALL_CMD_UNHOLD_CALL 5005u
#define IMS_CALL_CMD_SWITCH_CALL 5006u
#define IMS_CALL_CMD_COMBINE_CONFERENCE 5007u
#define IMS_CALL_CMD_INVITE_TO_CONFERENCE 5008u
#define IMS_CALL_CMD_KICK_OUT_CONFERENCE 5009u
#define IMS_CALL_CMD_UPDATE_MEDIA_REQ 5010u
#define IMS_CALL_CMD_GET_CALLS_DATA 5012u
#define IMS_CALL_CMD_GET_LAST_FAIL_REASON 5013u
#define IMS_CALL_CMD_UPDATE_MEDIA_RESP 5014u
#define IMS_CALL_CMD_CANCEL_CALL_UPGRADE 5015u
#define IMS_CALL_CMD_REQ_CAMERA_CAPS 5016u
#define IMS_CALL_CMD_START_DTMF 5100u
#define IMS_CALL_CMD_SEND_DTMF 5101u
#define IMS_CALL_CMD_STOP_DTMF 5102u
#define IMS_CALL_CMD_SET_DOMAIN_PREF 5200u
#define IMS_CALL_CMD_GET_DOMAIN_PREF 5201u
#define IMS_CALL_CMD_SET_IMS_SWITCH 5202u
#define IMS_CALL_CMD_GET_IMS_SWITCH 5203u
#define IMS_CALL_CMD_SET_IMS_CFG_STR 5204u
#define IMS_CALL_CMD_SET_IMS_CFG_INT 5205u
#define IMS_CALL_CMD_GET_IMS_CFG 5206u
#define IMS_CALL_CMD_SET_CARRIER_VT_CFG 5207u
#define IMS_CALL_CMD_SET_IMS_FEATURE 5208u
#define IMS_CALL_CMD_GET_IMS_FEATURE 5209u
#define IMS_CALL_CMD_SET_MUTE 5210u
#define IMS_CALL_CMD_GET_MUTE 5211u
#define IMS_CALL_CMD_UPDATE_IMS_CAPS 5213u
#define IMS_CALL_CMD_GET_UT_IMPU 5214u
#define IMS_CALL_CMD_NOTIFY_OP_CFG_CHANGED 5215u
#define IMS_CALL_CMD_GET_IMS_CAPS 5216u
#define IMS_CALL_CMD_CAN_SET_CF_TIME 5406u
#define IMS_CALL_CMD_REGISTER_CALLBACK 5500u

/* OHOS ImsCallCallback IPC callback codes (binary-verified from ImsCallCallbackStub
 * InitCallBasicFuncMap & InitConfigFuncMap in libtel_cellular_call.z.so). */
#define IMS_CALL_CB_DIAL 0u
#define IMS_CALL_CB_HANG_UP 1u
#define IMS_CALL_CB_REJECT 2u
#define IMS_CALL_CB_ANSWER 3u
#define IMS_CALL_CB_HOLD_CALL 4u
#define IMS_CALL_CB_UNHOLD_CALL 5u
#define IMS_CALL_CB_SWITCH_CALL 6u
#define IMS_CALL_CB_COMBINE_CONFERENCE 7u
#define IMS_CALL_CB_INVITE_TO_CONFERENCE 8u
#define IMS_CALL_CB_CALL_STATE_CHANGE 12u
#define IMS_CALL_CB_LAST_CALL_FAIL_REASON 14u
#define IMS_CALL_CB_GET_CALLS_DATA 15u
#define IMS_CALL_CB_CALL_RINGBACK 18u
#define IMS_CALL_CB_START_DTMF 24u
#define IMS_CALL_CB_SEND_DTMF 25u
#define IMS_CALL_CB_STOP_DTMF 26u
#define IMS_CALL_CB_SET_IMS_SWITCH 33u
#define IMS_CALL_CB_GET_IMS_SWITCH 34u
#define IMS_CALL_CB_SET_MUTE 39u
#define IMS_CALL_CB_GET_IMS_CAP 42u

/* OHOS ImsRegTech enum values (verified in libtel_core_service.z.so). */
#define OHOS_IMS_REG_TECH_NONE 0
#define OHOS_IMS_REG_TECH_LTE 1
#define OHOS_IMS_REG_TECH_IWLAN 2
#define OHOS_IMS_REG_TECH_NR 3

/* Qualcomm QMI_VOICE call_type values (verified on SM7475 MPSS). */
#define QMI_VOICE_CALL_TYPE_CS 0x00u
#define QMI_VOICE_CALL_TYPE_VOICE_IP 0x08u
#define QMI_VOICE_CALL_TYPE_VT 0x09u

static inline bool ims_is_ip_call_type(uint8_t call_type)
{
	return call_type == QMI_VOICE_CALL_TYPE_VOICE_IP ||
	       call_type == QMI_VOICE_CALL_TYPE_VT;
}

typedef struct ImsServiceStatusWire {
	uint8_t supportImsVoice;
	uint8_t supportImsVideo;
	uint8_t supportImsUt;
	uint8_t supportImsSms;
	int32_t imsRegTech;
} ImsServiceStatusWire;

typedef struct ImsRegistrationStatusWire {
	uint8_t isRegisterd;
	uint8_t pad[3];
	int32_t imsRegTech;
} ImsRegistrationStatusWire;

/* Binary-verified 48-byte SendSmsResultInfo wire layout (ImsSmsCallbackStub::OnImsSendMessageResponseInner). */
typedef struct ImsSendSmsResultWire {
	int32_t msgRef;
	int32_t pad0;
	uint8_t pduEmptySso[24]; /* 24 zero bytes == valid empty OHOS libc++ std::string */
	int32_t errCode;
	int32_t pad1;
	int64_t flag;
} ImsSendSmsResultWire;

/* Binary-verified 16-byte RadioResponseInfo wire layout (ReadRawData(16)). */
typedef struct ImsRadioResponseInfoWire {
	int32_t flag;
	int32_t serial;
	int32_t error;
	int32_t type;
} ImsRadioResponseInfoWire;

/* Binary-verified 276-byte ImsCallInfo wire layout (ImsCallProxy::Dial/HangUp/Reject/Answer,
 * CellularCallConnectionIMS::DialRequest/HangUpRequest/AnswerRequest in libtel_cellular_call.z.so). */
typedef struct ImsCallInfoWire {
	char phoneNum[257]; /* offset 0..256 (kMaxNumberLen + 1) */
	uint8_t pad0[3];    /* offset 257..259 */
	int32_t slotId;     /* offset 260 */
	int32_t videoState; /* offset 264 */
	int32_t index;      /* offset 268 */
	uint8_t isRtt;      /* offset 272 */
	uint8_t pad1[3];    /* offset 273..275 */
} ImsCallInfoWire;

/* Binary-verified 8-byte MuteControlResponse wire layout (OnSetMuteResponseInner ReadRawData(8)). */
typedef struct ImsMuteControlResponseWire {
	int32_t result;
	int32_t value;
} ImsMuteControlResponseWire;

/* Binary-verified 16-byte ImsCapFromChip layout (OnGetImsCapResponseInner 4x ReadInt32). */
typedef struct ImsCapFromChipWire {
	int32_t volteCap;
	int32_t vowifiCap;
	int32_t vonrCap;
	int32_t utCap;
} ImsCapFromChipWire;

/* Parsed QMI_VOICE call entry shared between hmos_ims_core_service & unit tests. */
typedef struct ImsVoiceCallEntry {
	int32_t index;
	int32_t dir;       /* 0=MO, 1=MT */
	int32_t state;     /* 0=ACTIVE,1=HOLDING,2=DIALING,3=ALERTING,4=INCOMING,5=WAITING */
	int32_t mode;      /* 0=VOICE */
	int32_t mpty;      /* 0=single, 1=mpty */
	int32_t toa;       /* 129=national, 145=international */
	uint8_t callType;  /* QMI call_type: 0x00=CS, 0x08=VOICE_IP, 0x09=VT */
	char number[64];
} ImsVoiceCallEntry;

#ifdef __cplusplus
static_assert(sizeof(ImsServiceStatusWire) == 8, "ImsServiceStatusWire must be 8 bytes");
static_assert(sizeof(ImsRegistrationStatusWire) == 8, "ImsRegistrationStatusWire must be 8 bytes");
static_assert(sizeof(ImsSendSmsResultWire) == 48, "ImsSendSmsResultWire must be 48 bytes");
static_assert(sizeof(ImsRadioResponseInfoWire) == 16, "ImsRadioResponseInfoWire must be 16 bytes");
static_assert(sizeof(ImsCallInfoWire) == 276, "ImsCallInfoWire must be 276 bytes");
static_assert(offsetof(ImsCallInfoWire, phoneNum) == 0, "phoneNum offset");
static_assert(offsetof(ImsCallInfoWire, slotId) == 260, "slotId offset");
static_assert(offsetof(ImsCallInfoWire, videoState) == 264, "videoState offset");
static_assert(offsetof(ImsCallInfoWire, index) == 268, "index offset");
static_assert(offsetof(ImsCallInfoWire, isRtt) == 272, "isRtt offset");
static_assert(sizeof(ImsMuteControlResponseWire) == 8, "ImsMuteControlResponseWire must be 8 bytes");
static_assert(sizeof(ImsCapFromChipWire) == 16, "ImsCapFromChipWire must be 16 bytes");
#else
_Static_assert(sizeof(ImsServiceStatusWire) == 8, "ImsServiceStatusWire must be 8 bytes");
_Static_assert(sizeof(ImsRegistrationStatusWire) == 8, "ImsRegistrationStatusWire must be 8 bytes");
_Static_assert(sizeof(ImsSendSmsResultWire) == 48, "ImsSendSmsResultWire must be 48 bytes");
_Static_assert(sizeof(ImsRadioResponseInfoWire) == 16, "ImsRadioResponseInfoWire must be 16 bytes");
_Static_assert(sizeof(ImsCallInfoWire) == 276, "ImsCallInfoWire must be 276 bytes");
_Static_assert(offsetof(ImsCallInfoWire, phoneNum) == 0, "phoneNum offset");
_Static_assert(offsetof(ImsCallInfoWire, slotId) == 260, "slotId offset");
_Static_assert(offsetof(ImsCallInfoWire, videoState) == 264, "videoState offset");
_Static_assert(offsetof(ImsCallInfoWire, index) == 268, "index offset");
_Static_assert(offsetof(ImsCallInfoWire, isRtt) == 272, "isRtt offset");
_Static_assert(sizeof(ImsMuteControlResponseWire) == 8, "ImsMuteControlResponseWire must be 8 bytes");
_Static_assert(sizeof(ImsCapFromChipWire) == 16, "ImsCapFromChipWire must be 16 bytes");
#endif

static inline uint16_t ims_get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t ims_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int ims_qmi_find_tlv(const uint8_t *tlvs, size_t len, uint8_t want_type,
				   const uint8_t **out_val, uint16_t *out_len)
{
	size_t off = 0;
	while (off + 3 <= len) {
		uint8_t t = tlvs[off];
		uint16_t l = ims_get_le16(tlvs + off + 1);
		off += 3;
		if (off + l > len)
			break;
		if (t == want_type) {
			if (out_val)
				*out_val = tlvs + off;
			if (out_len)
				*out_len = l;
			return 0;
		}
		off += l;
	}
	return -1;
}

/* Map Qualcomm QMI_IMSA service RAT enum to OHOS ImsRegTech.
 * QMI_IMSA RAT: 0=WLAN, 1=WWAN, 2=IWLAN, 3=LTE, 4=EHRPD, 5=IWLAN_ALT, 8=NR5G. */
static inline int32_t imsa_rat_to_ohos_tech(uint32_t qmi_rat)
{
	switch (qmi_rat) {
	case 8u:
		return OHOS_IMS_REG_TECH_NR;
	case 0u:
	case 2u:
	case 5u:
		return OHOS_IMS_REG_TECH_IWLAN;
	case 1u:
	case 3u:
	case 4u:
	default:
		return OHOS_IMS_REG_TECH_LTE;
	}
}

/* Parse QMI_IMSA_GET_IMS_REG_STATUS (0x0020) response TLVs:
 *   TLV 0x12 (uint32_t): 0=NOT_REGISTERED, 1=REGISTERING, 2=REGISTERED.
 *   TLV 0x10 (uint8_t, legacy): 1=REGISTERED. */
static inline bool parse_imsa_reg_status_tlvs(const uint8_t *tlvs, size_t len)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (ims_qmi_find_tlv(tlvs, len, 0x12, &v, &vl) == 0 && vl >= 4)
		return ims_get_le32(v) == 2u;
	if (ims_qmi_find_tlv(tlvs, len, 0x10, &v, &vl) == 0 && vl >= 1)
		return v[0] != 0u;
	return false;
}

/* Parse QMI_IMSA_GET_IMS_SRV_STATUS (0x0021) response TLVs into OHOS wire structs.
 *   TLV 0x10: sms_service_status (u32, 2=FULL)
 *   TLV 0x11: voip_service_status (u32, 2=FULL)
 *   TLV 0x12: vt_service_status (u32, 2=FULL)
 *   TLV 0x13: sms_service_rat (u32)
 *   TLV 0x14: voip_service_rat (u32)
 *   TLV 0x16: ut_service_status (u32, 2=FULL) */
static inline void parse_imsa_srv_status_tlvs(const uint8_t *tlvs, size_t len,
					      bool registered,
					      ImsServiceStatusWire *svc_out,
					      ImsRegistrationStatusWire *reg_out)
{
	memset(svc_out, 0, sizeof(*svc_out));
	memset(reg_out, 0, sizeof(*reg_out));
	if (!registered)
		return;

	const uint8_t *v = NULL;
	uint16_t vl = 0;
	int32_t tech = OHOS_IMS_REG_TECH_LTE;

	if (ims_qmi_find_tlv(tlvs, len, 0x10, &v, &vl) == 0 && vl >= 4 && ims_get_le32(v) == 2u)
		svc_out->supportImsSms = 1u;
	if (ims_qmi_find_tlv(tlvs, len, 0x11, &v, &vl) == 0 && vl >= 4 && ims_get_le32(v) == 2u)
		svc_out->supportImsVoice = 1u;
	if (ims_qmi_find_tlv(tlvs, len, 0x12, &v, &vl) == 0 && vl >= 4 && ims_get_le32(v) == 2u)
		svc_out->supportImsVideo = 1u;
	if (ims_qmi_find_tlv(tlvs, len, 0x16, &v, &vl) == 0 && vl >= 4 && ims_get_le32(v) == 2u)
		svc_out->supportImsUt = 1u;

	if (ims_qmi_find_tlv(tlvs, len, 0x14, &v, &vl) == 0 && vl >= 4)
		tech = imsa_rat_to_ohos_tech(ims_get_le32(v));
	else if (ims_qmi_find_tlv(tlvs, len, 0x13, &v, &vl) == 0 && vl >= 4)
		tech = imsa_rat_to_ohos_tech(ims_get_le32(v));

	svc_out->imsRegTech = tech;
	reg_out->isRegisterd = 1u;
	reg_out->imsRegTech = tech;
}

/* --- Shared QMI_VOICE helpers --- */

static inline int32_t ims_qmi_voice_state_to_ohos(uint8_t qstate)
{
	switch (qstate) {
	case 0x03u: return 0; /* CONVERSATION -> ACTIVE */
	case 0x06u: return 1; /* HOLD -> HOLDING */
	case 0x01u: /* ORIGINATION */
	case 0x04u: /* CC_IN_PROGRESS */
		return 2; /* DIALING */
	case 0x05u: /* ALERTING */
	case 0x0bu: /* PRE_ALERTING */
		return 3; /* ALERTING */
	case 0x02u: /* INCOMING */
	case 0x0au: /* SETUP */
		return 4; /* INCOMING */
	case 0x07u: return 5; /* WAITING */
	default: return -1; /* END / DISCONNECTING / UNKNOWN */
	}
}

/* Parses both QMI_VOICE_GET_ALL_CALL_INFO (0x002f: info=0x10, num=0x11)
 * and QMI_VOICE_ALL_CALL_STATUS_IND (0x002e: info=0x01, num=0x10). */
static inline size_t ims_parse_voice_call_list(const uint8_t *tlvs, size_t len, bool is_ind,
					       ImsVoiceCallEntry *out, size_t max_out)
{
	const uint8_t *v_info = NULL, *v_num = NULL;
	uint16_t l_info = 0, l_num = 0;
	uint8_t info_tag = is_ind ? 0x01u : 0x10u;
	uint8_t num_tag = is_ind ? 0x10u : 0x11u;

	if (!out || max_out == 0)
		return 0;
	if (ims_qmi_find_tlv(tlvs, len, info_tag, &v_info, &l_info) < 0 || l_info < 1)
		return 0;

	uint8_t cnt = v_info[0];
	size_t n = 0;
	size_t pos = 1;
	for (uint8_t i = 0; i < cnt && n < max_out && pos + 7 <= l_info; i++, pos += 7) {
		uint8_t cid = v_info[pos + 0];
		int32_t hstate = ims_qmi_voice_state_to_ohos(v_info[pos + 1]);
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
		out[n].toa = 129;
		out[n].callType = ctype;
		n++;
	}

	if (n > 0 && ims_qmi_find_tlv(tlvs, len, num_tag, &v_num, &l_num) == 0 && l_num >= 1) {
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
					out[j].toa = (out[j].number[0] == '+') ? 145 : 129;
					break;
				}
			}
			np += nlen;
		}
	}
	return n;
}

/* Parse QMI_VOICE_ALL_CALL_STATUS_IND TLV 0x16 (Call End Reason Info). */
static inline int32_t ims_parse_voice_call_end_reason(const uint8_t *tlvs, size_t len)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (ims_qmi_find_tlv(tlvs, len, 0x16, &v, &vl) != 0 || vl < 4)
		return 16; /* NORMAL_CLEARING */
	if (v[0] == 0)
		return 16;
	uint16_t qcause = ims_get_le16(v + 2);
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
		return 16;
	}
}

static inline size_t ims_build_voice_dial_tlvs(uint8_t *buf, size_t cap,
					       const char *number, int call_type)
{
	if (!buf || !number || number[0] == '\0')
		return 0;
	size_t alen = strlen(number);
	if (alen > 80)
		alen = 80;
	if (cap < alen + 12)
		return 0;
	size_t off = 0;
	buf[off++] = 0x01u;
	buf[off++] = (uint8_t)(alen & 0xffu);
	buf[off++] = (uint8_t)((alen >> 8) & 0xffu);
	memcpy(buf + off, number, alen);
	off += alen;
	if (call_type >= 0) {
		buf[off++] = 0x10u;
		buf[off++] = 0x01u;
		buf[off++] = 0x00u;
		buf[off++] = (uint8_t)(call_type & 0xff);
	}
	return off;
}

/* --- Clean-room 3GPP2 C.S0015 CDMA MO SMS -> 3GPP TS 23.040 GSM SMS-SUBMIT transcoder ---
 * Verified against donor libtel_sms_mms.z.so (CdmaSmsTransportMessage::Encode,
 * CdmaSmsUserData::Encode / EncodeHeaderUnicode / EncodeHeader7Bit / EncodeGsm7Bit). */

static inline uint32_t ims_cdma_read_bits(const uint8_t *buf, size_t total_bits,
					  size_t *bit_pos, int nbits)
{
	uint32_t val = 0;
	for (int i = 0; i < nbits; i++) {
		val <<= 1;
		if (*bit_pos < total_bits) {
			size_t bp = *bit_pos;
			if ((buf[bp >> 3] >> (7 - (bp & 7u))) & 1u)
				val |= 1u;
		}
		(*bit_pos)++;
	}
	return val;
}

static inline int ims_ascii_to_gsm7_septet(uint8_t ch)
{
	if (ch == '@')
		return 0x00;
	if (ch == '$')
		return 0x02;
	if (ch == '\n' || ch == '\r')
		return (int)ch;
	if (ch == '_')
		return 0x11;
	if ((ch >= 0x20 && ch <= 0x5a && ch != '$' && ch != '@') ||
	    (ch >= 0x61 && ch <= 0x7a))
		return (int)ch;
	return -1;
}

static inline size_t ims_pack_gsm7_septets(const uint8_t *septets, size_t n_septets,
					   size_t fill_bits, uint8_t *out, size_t cap)
{
	size_t total_bits = fill_bits + n_septets * 7u;
	size_t n_bytes = (total_bits + 7u) / 8u;
	if (n_bytes > cap)
		return 0;
	memset(out, 0, n_bytes);
	size_t bit_off = fill_bits;
	for (size_t i = 0; i < n_septets; i++) {
		uint8_t s = septets[i] & 0x7fu;
		for (int b = 0; b < 7; b++) {
			if ((s >> b) & 1u)
				out[bit_off >> 3] |= (uint8_t)(1u << (bit_off & 7u));
			bit_off++;
		}
	}
	return n_bytes;
}

static inline void ims_unpack_gsm7_septets(const uint8_t *packed, size_t packed_len,
					   size_t n_septets, uint8_t *septets_out)
{
	size_t total_bits = packed_len * 8u;
	size_t bit_off = 0;
	for (size_t i = 0; i < n_septets; i++) {
		uint8_t s = 0;
		for (int b = 0; b < 7; b++) {
			if (bit_off < total_bits && ((packed[bit_off >> 3] >> (bit_off & 7u)) & 1u))
				s |= (uint8_t)(1u << b);
			bit_off++;
		}
		septets_out[i] = s & 0x7fu;
	}
}

/* Distinguish genuine 3GPP2 C.S0015 Point-to-Point Transport PDUs (leading 0x00)
 * from 3GPP TS 23.040 GSM SMS-SUBMIT TPDUs ((tpdu[0] & 0x03) == 0x01, e.g. 0x01/0x11/0x21).
 * Binary-verified in donor libtel_sms_mms.z.so: BOTH GsmSmsSender::SendImsSms and
 * CdmaSmsSender::SendImsSms pass ImsMessageInfo.tech = 1 (SMS_RADIO_TECH_3GPP)
 * alongside a 3GPP GSM SMS-SUBMIT TPDU, so routing on tech == 1 alone misclassifies
 * GSM IMS SMS as CDMA. */
static inline bool ims_pdu_is_cdma_p2p(const uint8_t *tpdu, size_t tlen, int32_t tech)
{
	if (!tpdu || tlen < 4)
		return false;
	if ((tpdu[0] & 0x03u) == 0x01u)
		return false;
	return (tpdu[0] == 0x00u) && (tech == 1 || tech == 2);
}

static inline bool ims_transcode_cdma_pdu_to_gsm_submit(const uint8_t *cdma, size_t cdma_len,
							const uint8_t *smsc_raw, size_t smsc_raw_len,
							uint8_t *out, size_t cap, size_t *out_len)
{
	if (!cdma || cdma_len < 4 || cdma[0] != 0x00u || !out || !out_len)
		return false;

	char dest[40];
	memset(dest, 0, sizeof(dest));
	bool dest_intl = false;
	const uint8_t *bearer = NULL;
	size_t bearer_len = 0;

	size_t pos = 1;
	while (pos + 2 <= cdma_len) {
		uint8_t pid = cdma[pos];
		uint8_t plen = cdma[pos + 1];
		pos += 2;
		if (pos + plen > cdma_len)
			break;
		const uint8_t *pv = cdma + pos;
		if (pid == 0x04u && plen >= 2) { /* Destination Address */
			size_t bp = 0, tbits = (size_t)plen * 8u;
			uint32_t digit_mode = ims_cdma_read_bits(pv, tbits, &bp, 1);
			uint32_t num_mode = ims_cdma_read_bits(pv, tbits, &bp, 1);
			if (digit_mode == 1u) {
				uint32_t ntype = ims_cdma_read_bits(pv, tbits, &bp, 3);
				if (num_mode == 0u) {
					(void)ims_cdma_read_bits(pv, tbits, &bp, 4); /* num_plan */
					if (ntype == 1u)
						dest_intl = true;
				}
			}
			uint32_t nfields = ims_cdma_read_bits(pv, tbits, &bp, 8);
			size_t di = 0;
			static const char dtmf_map[16] = {
				'0', '1', '2', '3', '4', '5', '6', '7',
				'8', '9', '0', '*', '#', 'a', 'b', 'c'
			};
			for (uint32_t i = 0; i < nfields && di + 1 < sizeof(dest); i++) {
				if (digit_mode == 0u) {
					uint32_t d = ims_cdma_read_bits(pv, tbits, &bp, 4);
					if (d >= 1u && d <= 12u)
						dest[di++] = dtmf_map[d];
				} else {
					uint32_t ch = ims_cdma_read_bits(pv, tbits, &bp, 8);
					if (ch == '+' && di == 0) {
						dest_intl = true;
					} else if ((ch >= '0' && ch <= '9') || ch == '*' || ch == '#') {
						dest[di++] = (char)ch;
					}
				}
			}
			dest[di] = '\0';
		} else if (pid == 0x08u && plen >= 3) { /* Bearer Data */
			bearer = pv;
			bearer_len = plen;
		}
		pos += plen;
	}

	if (dest[0] == '\0' || !bearer || bearer_len < 3)
		return false;

	uint8_t msg_ref = 0;
	bool has_udh = false;
	uint8_t ud_encoding = 0;
	const uint8_t *ud_sub = NULL;
	size_t ud_sub_len = 0;

	size_t bpos = 0;
	while (bpos + 2 <= bearer_len) {
		uint8_t sid = bearer[bpos];
		uint8_t slen = bearer[bpos + 1];
		bpos += 2;
		if (bpos + slen > bearer_len)
			break;
		const uint8_t *sv = bearer + bpos;
		if (sid == 0x00u && slen >= 3) { /* Message Identifier */
			uint16_t mid = (uint16_t)(((sv[0] & 0x0fu) << 12) | (sv[1] << 4) | (sv[2] >> 4));
			msg_ref = (uint8_t)(mid & 0xffu);
			has_udh = ((sv[2] & 0x08u) != 0u);
		} else if (sid == 0x01u && slen >= 2) { /* User Data */
			ud_sub = sv;
			ud_sub_len = slen;
		}
		bpos += slen;
	}

	if (!ud_sub || ud_sub_len < 2)
		return false;

	size_t ubp = 0, utbits = ud_sub_len * 8u;
	ud_encoding = (uint8_t)ims_cdma_read_bits(ud_sub, utbits, &ubp, 5);
	if (ud_encoding == 0x01u || ud_encoding == 0x0au)
		(void)ims_cdma_read_bits(ud_sub, utbits, &ubp, 8); /* IS-91 / GSM-DCS msg_type */
	uint32_t num_fields = ims_cdma_read_bits(ud_sub, utbits, &ubp, 8);

	/* Extract optional UDH (when header_ind == 1) */
	uint8_t udh[64];
	size_t udh_total = 0; /* includes udhl byte */
	if (has_udh && ubp + 8 <= utbits) {
		uint8_t udhl = (uint8_t)ims_cdma_read_bits(ud_sub, utbits, &ubp, 8);
		if ((size_t)(1u + udhl) <= sizeof(udh) && ubp + (size_t)udhl * 8u <= utbits) {
			udh[0] = udhl;
			for (uint8_t i = 0; i < udhl; i++)
				udh[1u + i] = (uint8_t)ims_cdma_read_bits(ud_sub, utbits, &ubp, 8);
			udh_total = (size_t)(1u + udhl);
		} else {
			has_udh = false;
		}
	}

	uint8_t tp_dcs = 0x00u;
	uint8_t tp_udl = 0;
	uint8_t tp_ud[160];
	size_t tp_ud_bytes = 0;

	if (ud_encoding == 0x04u) { /* UTF-16BE UNICODE */
		/* Verified in donor libtel_sms_mms.z.so CdmaSmsUserData::EncodeHeaderUnicode:
		 * num_fields = ((udhl + data_bytes) >> 1) + 1 = hdr_units + text_units where
		 * hdr_units = (udh_total + 1) / 2, and UTF-16BE bytes follow immediately with
		 * NO padding byte even when udh_total is odd. */
		tp_dcs = 0x08u;
		size_t hdr_units = (udh_total + 1u) / 2u;
		size_t text_units = (num_fields > hdr_units) ? (num_fields - hdr_units) : 0;
		if (has_udh && udh_total > 0) {
			memcpy(tp_ud, udh, udh_total);
			tp_ud_bytes = udh_total;
		}
		for (size_t i = 0; i < text_units && tp_ud_bytes + 2 <= 140; i++) {
			if (ubp + 16 > utbits)
				break;
			uint32_t u16 = ims_cdma_read_bits(ud_sub, utbits, &ubp, 16);
			tp_ud[tp_ud_bytes++] = (uint8_t)((u16 >> 8) & 0xffu);
			tp_ud[tp_ud_bytes++] = (uint8_t)(u16 & 0xffu);
		}
		tp_udl = (uint8_t)tp_ud_bytes;
	} else if (ud_encoding == 0x09u) { /* GSM 7-bit default alphabet inside 3GPP2 */
		/* Verified in donor CdmaSmsUserData::EncodeGsm7Bit / EncodeHeader7Bit / DecodeGsm7Bit:
		 * payload is LSB-first packed GSM-7 octets (written 8 bits/byte).
		 * With UDH, OHOS writes num_fields = hdr_septets + data_len + 1 and 8 zero pad bits. */
		size_t hdr_septets = 0;
		size_t text_fields = num_fields;
		if (has_udh && udh_total > 0) {
			size_t hdr_bits = udh_total * 8u;
			hdr_septets = (hdr_bits + 6u) / 7u;
			size_t ohos_hdr_fields = hdr_septets + 1u;
			if (num_fields >= ohos_hdr_fields && ubp + 8u <= utbits) {
				(void)ims_cdma_read_bits(ud_sub, utbits, &ubp, 8);
				text_fields = num_fields - ohos_hdr_fields;
			} else if (num_fields > hdr_septets) {
				size_t pad = hdr_septets * 7u - hdr_bits;
				if (pad > 0)
					(void)ims_cdma_read_bits(ud_sub, utbits, &ubp, (int)pad);
				text_fields = num_fields - hdr_septets;
			} else {
				text_fields = 0;
			}
		}
		if (text_fields > 160u - hdr_septets)
			text_fields = 160u - hdr_septets;
		size_t packed_bytes = (text_fields * 7u + 7u) / 8u;
		uint8_t raw_packed[160];
		for (size_t i = 0; i < packed_bytes; i++)
			raw_packed[i] = (uint8_t)ims_cdma_read_bits(ud_sub, utbits, &ubp, 8);
		uint8_t septets[160];
		ims_unpack_gsm7_septets(raw_packed, packed_bytes, text_fields, septets);

		tp_dcs = 0x00u;
		size_t fill_bits = 0;
		if (has_udh && udh_total > 0) {
			memcpy(tp_ud, udh, udh_total);
			tp_ud_bytes = udh_total;
			hdr_septets = (udh_total * 8u + 6u) / 7u;
			fill_bits = hdr_septets * 7u - udh_total * 8u;
		}
		size_t packed = ims_pack_gsm7_septets(septets, text_fields, fill_bits,
						      tp_ud + tp_ud_bytes,
						      sizeof(tp_ud) - tp_ud_bytes);
		if (text_fields > 0 && packed == 0)
			return false;
		tp_ud_bytes += packed;
		if (tp_ud_bytes > 140)
			return false;
		tp_udl = (uint8_t)(hdr_septets + text_fields);
	} else if (ud_encoding == 0x02u || ud_encoding == 0x03u ||
		   ud_encoding == 0x08u || ud_encoding == 0x00u) {
		int bits_per_field = (ud_encoding == 0x08u || ud_encoding == 0x00u) ? 8 : 7;
		size_t hdr_fields = 0;
		if (has_udh && udh_total > 0) {
			if (bits_per_field == 7) {
				size_t hdr_bits = udh_total * 8u;
				hdr_fields = (hdr_bits + 6u) / 7u;
				size_t pad = hdr_fields * 7u - hdr_bits;
				if (pad > 0)
					(void)ims_cdma_read_bits(ud_sub, utbits, &ubp, (int)pad);
			} else {
				hdr_fields = udh_total;
			}
		}
		size_t text_fields = (num_fields > hdr_fields) ? (num_fields - hdr_fields) : 0;
		if (text_fields > 160)
			text_fields = 160;
		uint8_t chars[160];
		uint8_t septets[160];
		bool all_gsm7 = true;
		for (size_t i = 0; i < text_fields; i++) {
			uint8_t ch = (uint8_t)ims_cdma_read_bits(ud_sub, utbits, &ubp, bits_per_field);
			chars[i] = ch;
			int s = ims_ascii_to_gsm7_septet(ch);
			if (s < 0)
				all_gsm7 = false;
			else
				septets[i] = (uint8_t)s;
		}
		if (all_gsm7) {
			tp_dcs = 0x00u;
			size_t fill_bits = 0;
			size_t hdr_septets = 0;
			if (has_udh && udh_total > 0) {
				memcpy(tp_ud, udh, udh_total);
				tp_ud_bytes = udh_total;
				hdr_septets = (udh_total * 8u + 6u) / 7u;
				fill_bits = hdr_septets * 7u - udh_total * 8u;
			}
			if (hdr_septets + text_fields > 160)
				text_fields = 160 - hdr_septets;
			size_t packed = ims_pack_gsm7_septets(septets, text_fields, fill_bits,
							      tp_ud + tp_ud_bytes,
							      sizeof(tp_ud) - tp_ud_bytes);
			if (text_fields > 0 && packed == 0)
				return false;
			tp_ud_bytes += packed;
			if (tp_ud_bytes > 140)
				return false;
			tp_udl = (uint8_t)(hdr_septets + text_fields);
		} else {
			tp_dcs = 0x08u;
			if (has_udh && udh_total > 0) {
				memcpy(tp_ud, udh, udh_total);
				tp_ud_bytes = udh_total;
			}
			for (size_t i = 0; i < text_fields && tp_ud_bytes + 2 <= 140; i++) {
				tp_ud[tp_ud_bytes++] = 0x00u;
				tp_ud[tp_ud_bytes++] = chars[i];
			}
			tp_udl = (uint8_t)tp_ud_bytes;
		}
	} else {
		return false;
	}

	/* Assemble 3GPP SMS-SUBMIT PDU:
	 *   [SMSC] [TP-FO] [TP-MR] [TP-DA] [TP-PID] [TP-DCS] [TP-VP] [TP-UDL] [TP-UD] */
	size_t off = 0;
	size_t sc_len = (smsc_raw && smsc_raw_len > 0 && smsc_raw_len <= 16) ? smsc_raw_len : 1u;
	size_t dlen = strlen(dest);
	size_t dbcd_len = (dlen + 1u) / 2u;
	if (sc_len + 8u + dbcd_len + tp_ud_bytes > cap)
		return false;

	if (smsc_raw && smsc_raw_len > 0 && smsc_raw_len <= 16) {
		memcpy(out + off, smsc_raw, smsc_raw_len);
		off += smsc_raw_len;
	} else {
		out[off++] = 0x00u; /* use modem/USIM default SMSC */
	}

	out[off++] = has_udh ? 0x51u : 0x11u; /* MTI=SUBMIT(1), VPF=relative(2), optional UDHI(0x40) */
	out[off++] = msg_ref;
	out[off++] = (uint8_t)dlen;
	out[off++] = dest_intl ? 0x91u : 0x81u;
	for (size_t i = 0; i < dlen; i += 2) {
		uint8_t lo = (dest[i] == '*') ? 0x0au : (dest[i] == '#') ? 0x0bu : (uint8_t)(dest[i] - '0');
		uint8_t hi = 0x0fu;
		if (i + 1 < dlen) {
			char c2 = dest[i + 1];
			hi = (c2 == '*') ? 0x0au : (c2 == '#') ? 0x0bu : (uint8_t)(c2 - '0');
		}
		out[off++] = (uint8_t)((hi << 4) | (lo & 0x0fu));
	}
	out[off++] = 0x00u; /* TP-PID */
	out[off++] = tp_dcs;
	out[off++] = 0xa7u; /* TP-VP: 24 hours */
	out[off++] = tp_udl;
	if (tp_ud_bytes > 0) {
		memcpy(out + off, tp_ud, tp_ud_bytes);
		off += tp_ud_bytes;
	}
	*out_len = off;
	return true;
}

#ifdef __cplusplus
}

/* Cross-process OHOS callback parcel serializers (shared between hmos_ims_core_service.cpp
 * and offline host unit tests).
 *
 * CRITICAL CROSS-PROCESS ABI NOTE (binary-verified in libipc_single.z.so 0x7629c,
 * libtel_cellular_call.z.so 0x89910 / 0x8a668, libtel_sms_mms.z.so 0xf1f54):
 * MessageParcel::ReadRawData(16) reads a 4-byte length prefix via Parcel::ReadInt32() and
 * returns nullptr WITHOUT rewinding when bufferSize != 16.
 * Dual-overload callback stubs (OnImsSendMessageResponseInner, OnGetImsSwitchResponseInner,
 * OnGetImsCallsDataResponseInner) probe ReadRawData(16) first for RadioResponseInfo, then
 * read the success payload immediately afterwards. Writing a leading WriteInt32(0) before
 * the success payload satisfies ReadRawData(16) -> nullptr while leaving the cursor positioned
 * at the exact start of the success payload. */

template <typename ParcelLike>
static inline bool ims_write_sms_send_ok_parcel(ParcelLike &d, const std::u16string &token,
						int32_t slotId, const ImsSendSmsResultWire &res)
{
	return d.WriteInterfaceToken(token) &&
	       d.WriteInt32(slotId) &&
	       d.WriteInt32(0) && /* consumed by ReadRawData(16) -> nullptr */
	       d.WriteRawData(&res, sizeof(res));
}

template <typename ParcelLike>
static inline bool ims_write_get_ims_switch_parcel(ParcelLike &d, const std::u16string &token,
						   int32_t slotId, int32_t active)
{
	return d.WriteInterfaceToken(token) &&
	       d.WriteInt32(slotId) &&
	       d.WriteInt32(0) && /* consumed by ReadRawData(16) -> nullptr */
	       d.WriteInt32(active);
}

/* Serialize ImsCurrentCallList (19 fields per call, binary-verified in
 * ImsCallCallbackStub::OnGetImsCallsDataResponseInner 0x89910..0x89e70). */
template <typename ParcelLike>
static inline bool ims_write_calls_data_parcel(ParcelLike &d, const std::u16string &token,
					       int32_t slotId, int32_t flag,
					       const ImsVoiceCallEntry *calls, size_t count)
{
	if (count > 10u)
		count = 10u;
	if (!d.WriteInterfaceToken(token) ||
	    !d.WriteInt32(slotId) ||
	    !d.WriteInt32(0) || /* consumed by ReadRawData(16) -> nullptr */
	    !d.WriteInt32(static_cast<int32_t>(count)) || /* callSize */
	    !d.WriteInt32(flag) ||                        /* flag */
	    !d.WriteInt32(static_cast<int32_t>(count)))   /* vecSize */
		return false;

	const std::string empty;
	for (size_t i = 0; i < count; ++i) {
		const ImsVoiceCallEntry &c = calls[i];
		int32_t callType = (c.callType == QMI_VOICE_CALL_TYPE_VT) ? 3 : 0; /* 0=AUDIO_ONLY, 3=VT */
		std::string num(c.number);
		if (!d.WriteInt32(c.index) ||      /* +0  index */
		    !d.WriteInt32(c.dir) ||        /* +4  dir (0=MO, 1=MT) */
		    !d.WriteInt32(c.state) ||      /* +8  state (TelCallState) */
		    !d.WriteInt32(c.mode) ||       /* +12 mode (0=VOICE) */
		    !d.WriteInt32(c.mpty) ||       /* +16 mpty */
		    !d.WriteInt32(1) ||            /* +20 voiceDomain (1=IMS HD voice) */
		    !d.WriteInt32(callType) ||     /* +24 callType (0=AUDIO_ONLY) */
		    !d.WriteString(num) ||         /* +32 number */
		    !d.WriteString(empty) ||       /* +56 name */
		    !d.WriteInt32(0) ||            /* +80 numPresentation (0=ALLOWED) */
		    !d.WriteString(empty) ||       /* +88 redirectNumber/alpha */
		    !d.WriteInt32(c.toa ? c.toa : 129) || /* +112 type (TOA) */
		    !d.WriteInt32(0) ||            /* +116 crsType */
		    !d.WriteInt32(0) ||            /* +120 originalCallType */
		    !d.WriteInt32(0) ||            /* +124 namePresentation */
		    !d.WriteInt32(0) ||            /* +128 extra0 */
		    !d.WriteInt32(0) ||            /* +132 extra1 */
		    !d.WriteInt32(0) ||            /* +136 extra2 */
		    !d.WriteInt32(0))              /* +140 extra3 */
			return false;
	}
	return true;
}

template <typename ParcelLike>
static inline bool ims_write_get_ims_cap_parcel(ParcelLike &d, const std::u16string &token,
						int32_t slotId, const ImsCapFromChipWire &cap)
{
	return d.WriteInterfaceToken(token) &&
	       d.WriteInt32(slotId) &&
	       d.WriteInt32(cap.volteCap) &&
	       d.WriteInt32(cap.vowifiCap) &&
	       d.WriteInt32(cap.vonrCap) &&
	       d.WriteInt32(cap.utCap);
}

template <typename ParcelLike>
static inline bool ims_write_last_fail_reason_parcel(ParcelLike &d, const std::u16string &token,
						     int32_t slotId, int32_t reason,
						     const std::string &message)
{
	return d.WriteInterfaceToken(token) &&
	       d.WriteInt32(slotId) &&
	       d.WriteInt32(reason) &&
	       d.WriteString(message);
}

#endif /* __cplusplus */

#endif /* MARBLE_IMS_CORE_WIRE_H */
