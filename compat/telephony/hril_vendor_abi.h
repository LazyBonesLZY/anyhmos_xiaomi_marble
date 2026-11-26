#ifndef MARBLE_HRIL_VENDOR_ABI_H
#define MARBLE_HRIL_VENDOR_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Clean-room C ABI header matching OpenHarmony libhril_hdf.z.so / libhril.z.so
 * vendor entrypoint (`RilInitOps`). Field layouts and sizes are verified via
 * compile-time _Static_assert checks.
 */

#define HRIL_VERSION_1 0

enum {
	HRIL_REPORT_RESPONSE = 0,
	HRIL_REPORT_NOTIFICATION = 1,
};

enum {
	HRIL_ERR_SUCCESS = 0,
	HRIL_ERR_GENERIC_FAILURE = 1,
	HRIL_ERR_MODEM_DEVICE_CLOSE = 6,
};

enum {
	HNOTI_CALL_STATE_UPDATED = 1001,
	HNOTI_SMS_NEW_SMS = 1101,
	HNOTI_SMS_NEW_CDMA_SMS = 1102,
	HNOTI_SMS_STATUS_REPORT = 1103,
	HNOTI_SIM_STATUS_UPDATED = 1201,
	HNOTI_SIM_RADIO_PROTOCOL_UPDATED = 1208,
	HNOTI_DATA_PDP_CONTEXT_LIST_UPDATED = 1301,
	HNOTI_NETWORK_CS_REG_STATUS_UPDATED = 1401,
	HNOTI_NETWORK_SIGNAL_STRENGTH_UPDATED = 1402,
	HNOTI_NETWORK_TIME_UPDATED = 1403,
	HNOTI_NETWORK_PHY_CHNL_CFG_UPDATED = 1404,
	HNOTI_NETWORK_PS_REG_STATUS_UPDATED = 1406,
	HNOTI_MODEM_RADIO_STATE_UPDATED = 1501,
	HNOTI_MODEM_VOICE_TECH_UPDATED = 1502,
};

enum {
	HRIL_RADIO_PROTOCOL_PHASE_INITIAL = 0,
	HRIL_RADIO_PROTOCOL_PHASE_CHECK = 1,
	HRIL_RADIO_PROTOCOL_PHASE_UPDATE = 2,
	HRIL_RADIO_PROTOCOL_PHASE_NOTIFY = 3,
	HRIL_RADIO_PROTOCOL_PHASE_COMPLETE = 4,
};

enum {
	HRIL_RADIO_PROTOCOL_STATUS_NONE = 0,
	HRIL_RADIO_PROTOCOL_STATUS_SUCCESS = 1,
	HRIL_RADIO_PROTOCOL_STATUS_FAIL = 2,
};

/* Verified from libhril.z.so BuildCurrentCellList_V1_2 (0x2d8a0) & libtel_core_service.z.so (0x3af80) */
enum {
	HRIL_CELL_RAT_GSM = 1,
	HRIL_CELL_RAT_CDMA = 2,
	HRIL_CELL_RAT_WCDMA = 3,
	HRIL_CELL_RAT_TDSCDMA = 4,
	HRIL_CELL_RAT_LTE = 5,
	HRIL_CELL_RAT_NR = 6,
};

enum {
	HRIL_SIM_NOT_INSERTED = 0,
	HRIL_SIM_READY = 1,
	HRIL_SIM_PIN = 2,
	HRIL_SIM_PUK = 3,
	HRIL_SIM_NOT_READY = 4,
};

enum {
	HRIL_SIM_TYPE_UNKNOWN = 0,
	HRIL_SIM_TYPE_SIM = 1,
	HRIL_SIM_TYPE_USIM = 2,
};

enum {
	HRIL_RADIO_POWER_UNAVAILABLE = -1,
	HRIL_RADIO_POWER_OFF = 0,
	HRIL_RADIO_POWER_ON = 1,
};

enum {
	HRIL_REG_NOT_REGISTERED = 0,
	HRIL_REG_HOME = 1,
	HRIL_REG_SEARCHING = 2,
	HRIL_REG_DENIED = 3,
	HRIL_REG_UNKNOWN = 4,
	HRIL_REG_ROAMING = 5,
};

/* Exact OHOS Telephony_RadioTechnology / CoreService 0x4209c lookup table values */
enum {
	HRIL_TECH_UNKNOWN = 0,
	HRIL_TECH_GSM = 1,
	HRIL_TECH_CDMA1X = 2,
	HRIL_TECH_WCDMA = 3,
	HRIL_TECH_HSPA = 4,
	HRIL_TECH_HSPAP = 5,
	HRIL_TECH_TD_SCDMA = 6,
	HRIL_TECH_EVDO = 7,
	HRIL_TECH_EHRPD = 8,
	HRIL_TECH_LTE = 9,
	HRIL_TECH_LTE_CA = 10,
	HRIL_TECH_IWLAN = 11,
	HRIL_TECH_NR = 12,
};

/* Verified from libhril.z.so HRilManager::CreateHRilRequest (0x5f940/0x5f94c): +0 serial, +4 request, +8 slotId */
typedef struct ReqDataInfo {
	int32_t serial;
	int32_t request;
	int32_t slotId;
} ReqDataInfo;
_Static_assert(sizeof(ReqDataInfo) == 12, "ReqDataInfo must be 12 bytes");
_Static_assert(offsetof(ReqDataInfo, serial) == 0, "ReqDataInfo.serial");
_Static_assert(offsetof(ReqDataInfo, request) == 4, "ReqDataInfo.request");
_Static_assert(offsetof(ReqDataInfo, slotId) == 8, "ReqDataInfo.slotId");

typedef struct ReportInfo {
	const ReqDataInfo *requestInfo;
	int32_t notifyMessage;
	int32_t type;
	int32_t error;
	int32_t _pad1;
	int32_t _pad2;
	uint32_t flag;
} ReportInfo;
_Static_assert(sizeof(ReportInfo) == 32, "ReportInfo must be 32 bytes");
_Static_assert(offsetof(ReportInfo, requestInfo) == 0, "ReportInfo.requestInfo");
_Static_assert(offsetof(ReportInfo, notifyMessage) == 8, "ReportInfo.notifyMessage");
_Static_assert(offsetof(ReportInfo, type) == 12, "ReportInfo.type");
_Static_assert(offsetof(ReportInfo, error) == 16, "ReportInfo.error");
_Static_assert(offsetof(ReportInfo, flag) == 28, "ReportInfo.flag");

typedef void (*HRilReportFn)(int32_t slotId, const ReportInfo *reportInfo,
			     const uint8_t *data, size_t dataLen);

typedef struct HRilReport {
	HRilReportFn OnCallReport;
	HRilReportFn OnDataReport;
	HRilReportFn OnModemReport;
	HRilReportFn OnNetworkReport;
	HRilReportFn OnSimReport;
	HRilReportFn OnSmsReport;
	HRilReportFn OnEmcRescueReport;
	void (*OnTimerCallback)(void *func, uint8_t *param, const void *tv);
} HRilReport;
_Static_assert(sizeof(HRilReport) == 64, "HRilReport must be 64 bytes");

typedef struct HRilCardState {
	int32_t index;
	int32_t simType;
	int32_t simState;
	int32_t _pad;
	const char *iccid;
} HRilCardState;
_Static_assert(sizeof(HRilCardState) == 24, "HRilCardState must be 24 bytes");
_Static_assert(offsetof(HRilCardState, iccid) == 16, "HRilCardState.iccid");

typedef struct HRilSimIO {
	int32_t command;
	int32_t fileid;
	int32_t p1;
	int32_t p2;
	int32_t p3;
	int32_t _pad;
	char *data;
	char *pathid;
	char *pin2;
} HRilSimIO;
_Static_assert(sizeof(HRilSimIO) == 48, "HRilSimIO must be 48 bytes");
_Static_assert(offsetof(HRilSimIO, data) == 24, "HRilSimIO.data");
_Static_assert(offsetof(HRilSimIO, pathid) == 32, "HRilSimIO.pathid");

typedef struct HRilSimIOResponse {
	int32_t sw1;
	int32_t sw2;
	const char *response;
} HRilSimIOResponse;
_Static_assert(sizeof(HRilSimIOResponse) == 16, "HRilSimIOResponse must be 16 bytes");
_Static_assert(offsetof(HRilSimIOResponse, response) == 8, "HRilSimIOResponse.response");

typedef struct HRilRegStatusInfo {
	int32_t _reserved0;
	int32_t _reserved1;
	int32_t notifyMode;
	int32_t regStatus;
	int32_t lac;
	int32_t cellId;
	int32_t actType;
	int32_t nrAvailable;
	int32_t enDcAvailable;
	int32_t dcnr;
} HRilRegStatusInfo;
_Static_assert(sizeof(HRilRegStatusInfo) == 40, "HRilRegStatusInfo must be 40 bytes");
_Static_assert(offsetof(HRilRegStatusInfo, notifyMode) == 8, "HRilRegStatusInfo.notifyMode");
_Static_assert(offsetof(HRilRegStatusInfo, regStatus) == 12, "HRilRegStatusInfo.regStatus");
_Static_assert(offsetof(HRilRegStatusInfo, lac) == 16, "HRilRegStatusInfo.lac");
_Static_assert(offsetof(HRilRegStatusInfo, cellId) == 20, "HRilRegStatusInfo.cellId");
_Static_assert(offsetof(HRilRegStatusInfo, actType) == 24, "HRilRegStatusInfo.actType");
_Static_assert(offsetof(HRilRegStatusInfo, nrAvailable) == 28, "HRilRegStatusInfo.nrAvailable");
_Static_assert(offsetof(HRilRegStatusInfo, enDcAvailable) == 32, "HRilRegStatusInfo.enDcAvailable");
_Static_assert(offsetof(HRilRegStatusInfo, dcnr) == 36, "HRilRegStatusInfo.dcnr");

typedef struct HRilOperatorInfo {
	const char *longName;
	const char *shortName;
	const char *numeric;
} HRilOperatorInfo;
_Static_assert(sizeof(HRilOperatorInfo) == 24, "HRilOperatorInfo must be 24 bytes");

typedef struct HRilRssi {
	int32_t vals[16];
} HRilRssi;
_Static_assert(sizeof(HRilRssi) == 64, "HRilRssi must be 64 bytes");

enum {
	HRIL_RSSI_GSM_RXLEV = 0,
	HRIL_RSSI_GSM_BER = 1,
	HRIL_RSSI_CDMA_DB = 2,
	HRIL_RSSI_CDMA_ECIO = 3,
	HRIL_RSSI_WCDMA_RXLEV = 4,
	HRIL_RSSI_WCDMA_BER = 5,
	HRIL_RSSI_WCDMA_RSCP = 6,
	HRIL_RSSI_WCDMA_ECIO = 7,
	HRIL_RSSI_LTE_RXLEV = 8,
	HRIL_RSSI_LTE_RSRQ = 9,
	HRIL_RSSI_LTE_RSRP = 10,
	HRIL_RSSI_LTE_SNR = 11,
	HRIL_RSSI_TDSCDMA_RSCP = 12,
	HRIL_RSSI_NR_RSRP = 13,
	HRIL_RSSI_NR_RSRQ = 14,
	HRIL_RSSI_NR_SINR = 15,
};

typedef struct HRilVoiceRadioInfo {
	int32_t srvStatus;
	int32_t srvDomain;
	int32_t roamStatus;
	int32_t simStatus;
	int32_t lockStatus;
	int32_t sysMode;
	const char *sysModeName;
	int32_t actType;
	int32_t _pad;
	const char *actName;
} HRilVoiceRadioInfo;
_Static_assert(sizeof(HRilVoiceRadioInfo) == 48, "HRilVoiceRadioInfo must be 48 bytes");
_Static_assert(offsetof(HRilVoiceRadioInfo, sysModeName) == 24, "HRilVoiceRadioInfo.sysModeName");
_Static_assert(offsetof(HRilVoiceRadioInfo, actName) == 40, "HRilVoiceRadioInfo.actName");

typedef struct HRilRadioProtocol {
	int32_t sessionId;
	int32_t phase;
	int32_t technology;
	int32_t modemId;
	int32_t status;
} HRilRadioProtocol;
_Static_assert(sizeof(HRilRadioProtocol) == 20, "HRilRadioProtocol must be 20 bytes");

/* Verified from libhril.z.so HRilNetwork::GetPhysicalChannelConfigResponse (0x78464): stride 0x30 (48 B) */
typedef struct HRilPhyChannelConfig {
	int32_t cellConnStatus;
	int32_t cellBandwidthDownlinkKhz;
	int32_t cellBandwidthUplinkKhz;
	int32_t ratType;
	int32_t freqRange;
	int32_t downlinkChannelNum;
	int32_t uplinkChannelNum;
	int32_t physicalCellId;
	int32_t contextIdNum;
	int32_t _pad;
	int32_t *contextIds;
} HRilPhyChannelConfig;
_Static_assert(sizeof(HRilPhyChannelConfig) == 48, "HRilPhyChannelConfig must be 48 bytes");
_Static_assert(offsetof(HRilPhyChannelConfig, ratType) == 12, "HRilPhyChannelConfig.ratType");
_Static_assert(offsetof(HRilPhyChannelConfig, downlinkChannelNum) == 20, "HRilPhyChannelConfig.downlinkChannelNum");
_Static_assert(offsetof(HRilPhyChannelConfig, physicalCellId) == 28, "HRilPhyChannelConfig.physicalCellId");
_Static_assert(offsetof(HRilPhyChannelConfig, contextIds) == 40, "HRilPhyChannelConfig.contextIds");

typedef struct HRilPhyChannelConfigList {
	int32_t itemNum;
	int32_t _pad;
	const void *item;
} HRilPhyChannelConfigList;
_Static_assert(sizeof(HRilPhyChannelConfigList) == 16, "HRilPhyChannelConfigList must be 16 bytes");

typedef struct HRilCurrentCellInfoLte {
	int32_t arfcn;
	int32_t cellId;
	int32_t pci;
	int32_t tac;
	int32_t rsrp;
	int32_t rsrq;
	int32_t rssi;
} HRilCurrentCellInfoLte;

typedef struct HRilCurrentCellInfoNr {
	int32_t nrArfcn;
	int32_t pci;
	int32_t tac;
	int32_t _pad;
	int64_t nci;
	int32_t rsrp;
	int32_t rsrq;
} HRilCurrentCellInfoNr;

/* Verified byte-for-byte from libhril.z.so BuildCurrentCellList_V1_2 (0x78ec8) & CoreService (0x9bf8c) */
typedef struct HRilCurrentCellInfoVendor {
	int32_t ratType;
	int32_t mcc;
	int32_t mnc;
	int32_t _pad;
	union {
		HRilCurrentCellInfoLte lte;
		HRilCurrentCellInfoNr nr;
		int32_t raw[10];
	} serviceCellParas;
} HRilCurrentCellInfoVendor;
_Static_assert(sizeof(HRilCurrentCellInfoVendor) == 56, "HRilCurrentCellInfoVendor must be 56 bytes");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.lte.arfcn) == 0x10, "lte.arfcn");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.lte.cellId) == 0x14, "lte.cellId");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.lte.pci) == 0x18, "lte.pci");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.lte.tac) == 0x1c, "lte.tac");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.lte.rsrp) == 0x20, "lte.rsrp");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.lte.rsrq) == 0x24, "lte.rsrq");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.lte.rssi) == 0x28, "lte.rssi");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.nr.nrArfcn) == 0x10, "nr.nrArfcn");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.nr.pci) == 0x14, "nr.pci");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.nr.tac) == 0x18, "nr.tac");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.nr.nci) == 0x20, "nr.nci");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.nr.rsrp) == 0x28, "nr.rsrp");
_Static_assert(offsetof(HRilCurrentCellInfoVendor, serviceCellParas.nr.rsrq) == 0x2c, "nr.rsrq");

typedef struct HRilCurrentCellList {
	int32_t itemNum;
	int32_t _pad;
	const void *items;
} HRilCurrentCellList;
_Static_assert(sizeof(HRilCurrentCellList) == 16, "HRilCurrentCellList must be 16 bytes");

typedef struct HRilDataLinkCapability {
	int32_t primaryDownlinkKbps;
	int32_t primaryUplinkKbps;
	int32_t secondaryDownlinkKbps;
	int32_t secondaryUplinkKbps;
} HRilDataLinkCapability;
_Static_assert(sizeof(HRilDataLinkCapability) == 16, "HRilDataLinkCapability must be 16 bytes");

/* Verified from libhril.z.so HRilData::GetLinkBandwidthInfoResponse (0x55964): 36 bytes */
typedef struct HRilLinkBandwidthInfo {
	int32_t cid;
	int32_t qi;
	int32_t dlGfbr;
	int32_t ulGfbr;
	int32_t dlMfbr;
	int32_t ulMfbr;
	int32_t dlAmbr;
	int32_t ulAmbr;
	int32_t qosId;
} HRilLinkBandwidthInfo;
_Static_assert(sizeof(HRilLinkBandwidthInfo) == 36, "HRilLinkBandwidthInfo must be 36 bytes");

typedef struct HRilDataInfo {
	int32_t cid;
	int32_t reason;
	int32_t rat;
	int32_t roamingAllowed;
	int32_t verType;
	int32_t _pad;
	const char *userName;
	const char *password;
	const char *apn;
	const char *type;
	const char *roamingType;
} HRilDataInfo;
_Static_assert(sizeof(HRilDataInfo) == 64, "HRilDataInfo must be 64 bytes");
_Static_assert(offsetof(HRilDataInfo, cid) == 0x00, "HRilDataInfo.cid");
_Static_assert(offsetof(HRilDataInfo, reason) == 0x04, "HRilDataInfo.reason");
_Static_assert(offsetof(HRilDataInfo, rat) == 0x08, "HRilDataInfo.rat");
_Static_assert(offsetof(HRilDataInfo, roamingAllowed) == 0x0c, "HRilDataInfo.roamingAllowed");
_Static_assert(offsetof(HRilDataInfo, verType) == 0x10, "HRilDataInfo.verType");
_Static_assert(offsetof(HRilDataInfo, userName) == 0x18, "HRilDataInfo.userName");
_Static_assert(offsetof(HRilDataInfo, password) == 0x20, "HRilDataInfo.password");
_Static_assert(offsetof(HRilDataInfo, apn) == 0x28, "HRilDataInfo.apn");
_Static_assert(offsetof(HRilDataInfo, type) == 0x30, "HRilDataInfo.type");
_Static_assert(offsetof(HRilDataInfo, roamingType) == 0x38, "HRilDataInfo.roamingType");

typedef struct HRilDataCallResponse {
	int32_t reason;
	int32_t retryTime;
	int32_t cid;
	int32_t active;
	const char *type;
	const char *netPortName;
	const char *address;
	const char *dns;
	const char *dnsSec;
	const char *gateway;
	int32_t maxTransferUnit;
	int32_t _pad;
	const char *pCscfPrimAddr;
	const char *pCscfSecAddr;
	int32_t pduSessionId;
	int32_t _pad2;
} HRilDataCallResponse;
_Static_assert(sizeof(HRilDataCallResponse) == 96, "HRilDataCallResponse must be 96 bytes");
_Static_assert(offsetof(HRilDataCallResponse, reason) == 0x00, "HRilDataCallResponse.reason");
_Static_assert(offsetof(HRilDataCallResponse, retryTime) == 0x04, "HRilDataCallResponse.retryTime");
_Static_assert(offsetof(HRilDataCallResponse, cid) == 0x08, "HRilDataCallResponse.cid");
_Static_assert(offsetof(HRilDataCallResponse, active) == 0x0c, "HRilDataCallResponse.active");
_Static_assert(offsetof(HRilDataCallResponse, type) == 0x10, "HRilDataCallResponse.type");
_Static_assert(offsetof(HRilDataCallResponse, netPortName) == 0x18, "HRilDataCallResponse.netPortName");
_Static_assert(offsetof(HRilDataCallResponse, address) == 0x20, "HRilDataCallResponse.address");
_Static_assert(offsetof(HRilDataCallResponse, dns) == 0x28, "HRilDataCallResponse.dns");
_Static_assert(offsetof(HRilDataCallResponse, dnsSec) == 0x30, "HRilDataCallResponse.dnsSec");
_Static_assert(offsetof(HRilDataCallResponse, gateway) == 0x38, "HRilDataCallResponse.gateway");
_Static_assert(offsetof(HRilDataCallResponse, maxTransferUnit) == 0x40, "HRilDataCallResponse.maxTransferUnit");
_Static_assert(offsetof(HRilDataCallResponse, pCscfPrimAddr) == 0x48, "HRilDataCallResponse.pCscfPrimAddr");
_Static_assert(offsetof(HRilDataCallResponse, pCscfSecAddr) == 0x50, "HRilDataCallResponse.pCscfSecAddr");
_Static_assert(offsetof(HRilDataCallResponse, pduSessionId) == 0x58, "HRilDataCallResponse.pduSessionId");

typedef struct HRilServiceCenterAddress {
	int32_t tosca;
	int32_t _pad;
	const char *address;
} HRilServiceCenterAddress;
_Static_assert(sizeof(HRilServiceCenterAddress) == 16, "HRilServiceCenterAddress must be 16 bytes");

/* Verified from libhril.z.so HRilSms::MakeSendSmsResult (0x8fe10) & NewSmsNotify (0x8f778): 24 B */
typedef struct HRilSmsResponse {
	int32_t msgRef;
	int32_t _pad0;
	const char *pdu;
	int32_t errCode;
	int32_t _pad1;
} HRilSmsResponse;
_Static_assert(sizeof(HRilSmsResponse) == 24, "HRilSmsResponse must be 24 bytes");
_Static_assert(offsetof(HRilSmsResponse, pdu) == 8, "HRilSmsResponse.pdu");
_Static_assert(offsetof(HRilSmsResponse, errCode) == 16, "HRilSmsResponse.errCode");

/* Verified from libhril.z.so HRilCall::Dial (0x4ee24): 16 B */
typedef struct HRilDial {
	const char *address;
	int32_t clir;
	int32_t _pad;
} HRilDial;
_Static_assert(sizeof(HRilDial) == 16, "HRilDial must be 16 bytes");
_Static_assert(offsetof(HRilDial, clir) == 8, "HRilDial.clir");

/* Verified from libhril.z.so HRilCall::StartDtmf (0x5097c..0x50a2c): 32 B */
typedef struct HRilCallDtmfInfo {
	int32_t callId;
	int32_t _pad0;
	const char *dtmfKey;
	int32_t onLength;
	int32_t offLength;
	int32_t stringLength;
	int32_t _pad1;
} HRilCallDtmfInfo;
_Static_assert(sizeof(HRilCallDtmfInfo) == 32, "HRilCallDtmfInfo must be 32 bytes");
_Static_assert(offsetof(HRilCallDtmfInfo, dtmfKey) == 8, "HRilCallDtmfInfo.dtmfKey");

/* Verified from libhril.z.so HRilCall::BuildICallList V1_1 (0x50dfc) & V1_4 (0x4d0e4): 72 B (0x48) */
typedef struct HRilCallInfo {
	int32_t index;
	int32_t dir;
	int32_t state;
	int32_t mode;
	int32_t mpty;
	int32_t voiceDomain;
	int32_t callType;
	int32_t _pad0;
	const char *number;
	int32_t type;
	int32_t _pad1;
	const char *alpha;
	const char *name;
	int32_t namePresentation;
	int32_t _pad2;
} HRilCallInfo;
_Static_assert(sizeof(HRilCallInfo) == 72, "HRilCallInfo must be 72 bytes");
_Static_assert(offsetof(HRilCallInfo, voiceDomain) == 0x14, "HRilCallInfo.voiceDomain");
_Static_assert(offsetof(HRilCallInfo, callType) == 0x18, "HRilCallInfo.callType");
_Static_assert(offsetof(HRilCallInfo, number) == 0x20, "HRilCallInfo.number");
_Static_assert(offsetof(HRilCallInfo, type) == 0x28, "HRilCallInfo.type");
_Static_assert(offsetof(HRilCallInfo, alpha) == 0x30, "HRilCallInfo.alpha");
_Static_assert(offsetof(HRilCallInfo, name) == 0x38, "HRilCallInfo.name");
_Static_assert(offsetof(HRilCallInfo, namePresentation) == 0x40, "HRilCallInfo.namePresentation");

typedef struct HRilCallReq {
	void (*GetCallList)(const ReqDataInfo *requestInfo);
	void (*Dial)(const ReqDataInfo *requestInfo, const void *data, size_t dataLen);
	void (*Hangup)(const ReqDataInfo *requestInfo, const int32_t *data, size_t dataLen);
	void (*Reject)(const ReqDataInfo *requestInfo);
	void (*Answer)(const ReqDataInfo *requestInfo);
	void *slot5_to_6[7 - 5];
	void (*HoldCall)(const ReqDataInfo *requestInfo);
	void (*UnHoldCall)(const ReqDataInfo *requestInfo);
	void (*SwitchCall)(const ReqDataInfo *requestInfo);
	void *slot10_to_11[12 - 10];
	void (*CallSupplement)(const ReqDataInfo *requestInfo, int32_t type);
	void *slot13_to_20[21 - 13];
	void (*StartDtmf)(const ReqDataInfo *requestInfo, const HRilCallDtmfInfo *data);
	void (*SendDtmf)(const ReqDataInfo *requestInfo, const HRilCallDtmfInfo *data);
	void (*StopDtmf)(const ReqDataInfo *requestInfo, const HRilCallDtmfInfo *data);
	void *slot24_to_33[34 - 24];
	void (*GetCallFailReason)(const ReqDataInfo *requestInfo);
	void *reserved_tail[43 - 35];
} HRilCallReq;
_Static_assert(sizeof(HRilCallReq) == 43 * 8, "HRilCallReq must have 43 slots");
_Static_assert(offsetof(HRilCallReq, HoldCall) == 7 * 8, "HRilCallReq.HoldCall");
_Static_assert(offsetof(HRilCallReq, UnHoldCall) == 8 * 8, "HRilCallReq.UnHoldCall");
_Static_assert(offsetof(HRilCallReq, SwitchCall) == 9 * 8, "HRilCallReq.SwitchCall");
_Static_assert(offsetof(HRilCallReq, CallSupplement) == 12 * 8, "HRilCallReq.CallSupplement");
_Static_assert(offsetof(HRilCallReq, StartDtmf) == 21 * 8, "HRilCallReq.StartDtmf");
_Static_assert(offsetof(HRilCallReq, SendDtmf) == 22 * 8, "HRilCallReq.SendDtmf");
_Static_assert(offsetof(HRilCallReq, StopDtmf) == 23 * 8, "HRilCallReq.StopDtmf");
_Static_assert(offsetof(HRilCallReq, GetCallFailReason) == 34 * 8, "HRilCallReq.GetCallFailReason");

typedef struct HRilSimReq {
	void (*GetSimStatus)(const ReqDataInfo *requestInfo);
	void (*GetSimIO)(const ReqDataInfo *requestInfo, const HRilSimIO *data, size_t dataLen);
	void (*GetSimImsi)(const ReqDataInfo *requestInfo);
	void (*GetSimLockStatus)(const ReqDataInfo *requestInfo, const void *data, size_t dataLen);
	void *slot4_to_11[12 - 4];
	void (*SetActiveSim)(const ReqDataInfo *requestInfo, int32_t index, int32_t enable);
	void *slot13_to_16[17 - 13];
	void (*SimStkIsReady)(const ReqDataInfo *requestInfo);
	void (*GetRadioProtocol)(const ReqDataInfo *requestInfo);
	void (*SetRadioProtocol)(const ReqDataInfo *requestInfo, const HRilRadioProtocol *data);
	void *slot20_to_25[26 - 20];
	void (*SendSimMatchedOperatorInfo)(const ReqDataInfo *requestInfo, const void *data);
	void *slot27;
	void (*GetPrimarySlot)(const ReqDataInfo *requestInfo);
	void (*SetPrimarySlot)(const ReqDataInfo *requestInfo, int32_t slotId);
	void *reserved_tail[31 - 30];
} HRilSimReq;
_Static_assert(sizeof(HRilSimReq) == 31 * 8, "HRilSimReq must have 31 slots");
_Static_assert(offsetof(HRilSimReq, SetActiveSim) == 12 * 8, "HRilSimReq.SetActiveSim");
_Static_assert(offsetof(HRilSimReq, SimStkIsReady) == 17 * 8, "HRilSimReq.SimStkIsReady");
_Static_assert(offsetof(HRilSimReq, GetRadioProtocol) == 18 * 8, "HRilSimReq.GetRadioProtocol");
_Static_assert(offsetof(HRilSimReq, SetRadioProtocol) == 19 * 8, "HRilSimReq.SetRadioProtocol");
_Static_assert(offsetof(HRilSimReq, SendSimMatchedOperatorInfo) == 26 * 8, "HRilSimReq.SendSimMatchedOperatorInfo");
_Static_assert(offsetof(HRilSimReq, GetPrimarySlot) == 28 * 8, "HRilSimReq.GetPrimarySlot");
_Static_assert(offsetof(HRilSimReq, SetPrimarySlot) == 29 * 8, "HRilSimReq.SetPrimarySlot");

typedef struct HRilSmsReq {
	void (*SendGsmSms)(const ReqDataInfo *requestInfo, const void *data, size_t dataLen);
	void (*SendSmsAck)(const ReqDataInfo *requestInfo, const int32_t *data, size_t dataLen);
	void (*SendCdmaSms)(const ReqDataInfo *requestInfo, const void *data, size_t dataLen);
	void *slot3_to_6[7 - 3];
	void (*SetSmscAddr)(const ReqDataInfo *requestInfo, const HRilServiceCenterAddress *data, size_t dataLen);
	void (*GetSmscAddr)(const ReqDataInfo *requestInfo);
	void *reserved_tail[18 - 9];
} HRilSmsReq;
_Static_assert(sizeof(HRilSmsReq) == 18 * 8, "HRilSmsReq must have 18 slots");
_Static_assert(offsetof(HRilSmsReq, SendSmsAck) == 1 * 8, "HRilSmsReq.SendSmsAck");
_Static_assert(offsetof(HRilSmsReq, SendCdmaSms) == 2 * 8, "HRilSmsReq.SendCdmaSms");
_Static_assert(offsetof(HRilSmsReq, SetSmscAddr) == 7 * 8, "HRilSmsReq.SetSmscAddr");
_Static_assert(offsetof(HRilSmsReq, GetSmscAddr) == 8 * 8, "HRilSmsReq.GetSmscAddr");

typedef struct HRilDataReq {
	void (*SetInitApnInfo)(const ReqDataInfo *requestInfo, const void *data);
	void (*ActivatePdpContext)(const ReqDataInfo *requestInfo, const void *data);
	void (*DeactivatePdpContext)(const ReqDataInfo *requestInfo, const void *data);
	void (*GetPdpContextList)(const ReqDataInfo *requestInfo);
	void (*GetLinkBandwidthInfo)(const ReqDataInfo *requestInfo, int32_t cid);
	void (*SetLinkBandwidthReportingRule)(const ReqDataInfo *requestInfo, const void *data);
	void (*SetDataProfileInfo)(const ReqDataInfo *requestInfo, const void *data, int32_t size);
	void (*SendDataPerformanceMode)(const ReqDataInfo *requestInfo, const void *data);
	void (*SendDataSleepMode)(const ReqDataInfo *requestInfo, const void *data);
	void (*SetDataPermitted)(const ReqDataInfo *requestInfo, int32_t enabled);
	void (*GetLinkCapability)(const ReqDataInfo *requestInfo);
	void (*CleanAllConnections)(const ReqDataInfo *requestInfo);
	void (*ActivatePdpContextWithApnTypes)(const ReqDataInfo *requestInfo, const void *data);
	void *slot13_to_17[18 - 13];
	void (*ActivatePdpContextWithApnTypesforSlice)(const ReqDataInfo *requestInfo, const void *data);
} HRilDataReq;
_Static_assert(sizeof(HRilDataReq) == 19 * 8, "HRilDataReq must have 19 slots");
_Static_assert(offsetof(HRilDataReq, GetLinkBandwidthInfo) == 4 * 8, "HRilDataReq.GetLinkBandwidthInfo");
_Static_assert(offsetof(HRilDataReq, SetLinkBandwidthReportingRule) == 5 * 8, "HRilDataReq.SetLinkBandwidthReportingRule");
_Static_assert(offsetof(HRilDataReq, SetDataProfileInfo) == 6 * 8, "HRilDataReq.SetDataProfileInfo");
_Static_assert(offsetof(HRilDataReq, SendDataPerformanceMode) == 7 * 8, "HRilDataReq.SendDataPerformanceMode");
_Static_assert(offsetof(HRilDataReq, SendDataSleepMode) == 8 * 8, "HRilDataReq.SendDataSleepMode");
_Static_assert(offsetof(HRilDataReq, SetDataPermitted) == 9 * 8, "HRilDataReq.SetDataPermitted");
_Static_assert(offsetof(HRilDataReq, GetLinkCapability) == 10 * 8, "HRilDataReq.GetLinkCapability");
_Static_assert(offsetof(HRilDataReq, CleanAllConnections) == 11 * 8, "HRilDataReq.CleanAllConnections");
_Static_assert(offsetof(HRilDataReq, ActivatePdpContextWithApnTypes) == 12 * 8, "HRilDataReq.ActivatePdpContextWithApnTypes");
_Static_assert(offsetof(HRilDataReq, ActivatePdpContextWithApnTypesforSlice) == 18 * 8, "HRilDataReq.ActivatePdpContextWithApnTypesforSlice");

typedef struct HRilNetworkReq {
	void *slot0;
	void (*GetSignalStrength)(const ReqDataInfo *requestInfo);
	void (*GetCsRegStatus)(const ReqDataInfo *requestInfo);
	void (*GetPsRegStatus)(const ReqDataInfo *requestInfo);
	void (*GetOperatorInfo)(const ReqDataInfo *requestInfo);
	void (*GetNeighboringCellInfoList)(const ReqDataInfo *requestInfo);
	void (*GetCurrentCellInfo)(const ReqDataInfo *requestInfo);
	void (*GetNetworkSearchInformation)(const ReqDataInfo *requestInfo);
	void (*GetNetworkSelectionMode)(const ReqDataInfo *requestInfo);
	void (*SetNetworkSelectionMode)(const ReqDataInfo *requestInfo, const void *data);
	void (*GetPreferredNetwork)(const ReqDataInfo *requestInfo);
	void (*SetPreferredNetwork)(const ReqDataInfo *requestInfo, const int32_t *preferredNetworkType);
	void (*GetPhysicalChannelConfig)(const ReqDataInfo *requestInfo);
	void *slot13;
	void (*SetNotificationFilter)(const ReqDataInfo *requestInfo, const int32_t *newFilter);
	void (*SetDeviceState)(const ReqDataInfo *requestInfo, const int32_t *deviceStateType,
			       const int32_t *deviceStateOn);
	void (*GetRrcConnectionState)(const ReqDataInfo *requestInfo);
	void (*SetNrOptionMode)(const ReqDataInfo *requestInfo, const int32_t *mode);
	void (*GetNrOptionMode)(const ReqDataInfo *requestInfo);
	void *slot19;
} HRilNetworkReq;
_Static_assert(sizeof(HRilNetworkReq) == 20 * 8, "HRilNetworkReq must have 20 slots");
_Static_assert(offsetof(HRilNetworkReq, SetPreferredNetwork) == 11 * 8, "HRilNetworkReq.SetPreferredNetwork");
_Static_assert(offsetof(HRilNetworkReq, SetNotificationFilter) == 14 * 8, "HRilNetworkReq.SetNotificationFilter");
_Static_assert(offsetof(HRilNetworkReq, SetDeviceState) == 15 * 8, "HRilNetworkReq.SetDeviceState");
_Static_assert(offsetof(HRilNetworkReq, GetRrcConnectionState) == 16 * 8, "HRilNetworkReq.GetRrcConnectionState");
_Static_assert(offsetof(HRilNetworkReq, SetNrOptionMode) == 17 * 8, "HRilNetworkReq.SetNrOptionMode");
_Static_assert(offsetof(HRilNetworkReq, GetNrOptionMode) == 18 * 8, "HRilNetworkReq.GetNrOptionMode");

typedef struct HRilModemReq {
	void (*SetRadioState)(const ReqDataInfo *requestInfo, int32_t fun, int32_t rst);
	void (*GetRadioState)(const ReqDataInfo *requestInfo);
	void (*GetImei)(const ReqDataInfo *requestInfo);
	void (*GetImeiSv)(const ReqDataInfo *requestInfo);
	void (*GetMeid)(const ReqDataInfo *requestInfo);
	void (*GetVoiceRadioTechnology)(const ReqDataInfo *requestInfo);
	void (*GetBasebandVersion)(const ReqDataInfo *requestInfo);
	void (*ShutDown)(const ReqDataInfo *requestInfo);
	void *reserved_tail[22 - 8];
} HRilModemReq;
_Static_assert(sizeof(HRilModemReq) == 22 * 8, "HRilModemReq must have 22 slots");
_Static_assert(offsetof(HRilModemReq, GetBasebandVersion) == 6 * 8, "HRilModemReq.GetBasebandVersion");
_Static_assert(offsetof(HRilModemReq, ShutDown) == 7 * 8, "HRilModemReq.ShutDown");

typedef struct HRilOps {
	int32_t version;
	const HRilCallReq *callOps;
	const HRilSimReq *simOps;
	const HRilSmsReq *smsOps;
	const HRilDataReq *dataOps;
	const HRilNetworkReq *networkOps;
	const HRilModemReq *modemOps;
	const void *emcRescueOps;
} HRilOps;
_Static_assert(sizeof(HRilOps) == 64, "HRilOps must be 64 bytes");
_Static_assert(offsetof(HRilOps, callOps) == 0x08, "HRilOps.callOps");
_Static_assert(offsetof(HRilOps, simOps) == 0x10, "HRilOps.simOps");
_Static_assert(offsetof(HRilOps, smsOps) == 0x18, "HRilOps.smsOps");
_Static_assert(offsetof(HRilOps, dataOps) == 0x20, "HRilOps.dataOps");
_Static_assert(offsetof(HRilOps, networkOps) == 0x28, "HRilOps.networkOps");
_Static_assert(offsetof(HRilOps, modemOps) == 0x30, "HRilOps.modemOps");
_Static_assert(offsetof(HRilOps, emcRescueOps) == 0x38, "HRilOps.emcRescueOps");

const HRilOps *RilInitOps(const HRilReport *reportOps);

#ifdef __cplusplus
}
#endif

#endif /* MARBLE_HRIL_VENDOR_ABI_H */
