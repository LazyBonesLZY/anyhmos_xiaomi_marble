/* SPDX-License-Identifier: MIT
 * Clean-room QMI_IMSA / OHOS SA 4014 (ImsCoreService) wire layout & helpers.
 *
 * Wire contract verified from donor libtel_core_service.z.so:
 *   SA ID               : 4014 (TELEPHONY_IMS_SYS_ABILITY_ID)
 *   Service descriptor  : u"OHOS.Telephony.ImsCoreServiceInterface"
 *     code   0 : GetImsRegistrationStatus(int32 slotId) -> reply int32(0) + async cb
 *     code   1 : GetPhoneNumberFromIMPU(int32 slotId) -> reply int32(0) + string("")
 *     code 100 : RegisterImsCoreServiceCallback(IRemoteObject cb) -> reply int32(0)
 *     code 101 : GetProxyObjectPtr(int32 proxyType) -> empty reply (nullptr keeps CS/SMS on QMI RIL)
 *   Callback descriptor : u"OHOS.Telephony.ImsCoreServiceCallback"
 *     cb_cmd 0 : onImsServiceStatusReportInner(int32 slotId, ReadRawData(8)) -> reply int32
 *     cb_cmd 1 : OnGetImsRegistrationStatusResponseInner(int32 slotId, ReadRawData(8)) -> reply int32
 */
#ifndef MARBLE_IMS_CORE_WIRE_H
#define MARBLE_IMS_CORE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MARBLE_IMS_CORE_SA_ID 4014

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

/* OHOS ImsRegTech enum values (verified in libtel_core_service.z.so). */
#define OHOS_IMS_REG_TECH_NONE 0
#define OHOS_IMS_REG_TECH_LTE 1
#define OHOS_IMS_REG_TECH_IWLAN 2
#define OHOS_IMS_REG_TECH_NR 3

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

#ifdef __cplusplus
static_assert(sizeof(ImsServiceStatusWire) == 8, "ImsServiceStatusWire must be 8 bytes");
static_assert(sizeof(ImsRegistrationStatusWire) == 8, "ImsRegistrationStatusWire must be 8 bytes");
static_assert(sizeof(ImsSendSmsResultWire) == 48, "ImsSendSmsResultWire must be 48 bytes");
static_assert(sizeof(ImsRadioResponseInfoWire) == 16, "ImsRadioResponseInfoWire must be 16 bytes");
#else
_Static_assert(sizeof(ImsServiceStatusWire) == 8, "ImsServiceStatusWire must be 8 bytes");
_Static_assert(sizeof(ImsRegistrationStatusWire) == 8, "ImsRegistrationStatusWire must be 8 bytes");
_Static_assert(sizeof(ImsSendSmsResultWire) == 48, "ImsSendSmsResultWire must be 48 bytes");
_Static_assert(sizeof(ImsRadioResponseInfoWire) == 16, "ImsRadioResponseInfoWire must be 16 bytes");
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

#ifdef __cplusplus
}
#endif

#endif /* MARBLE_IMS_CORE_WIRE_H */
