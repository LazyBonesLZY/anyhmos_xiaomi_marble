/* SPDX-License-Identifier: MIT */
/*
 * Clean-room pure-C GNSS HDI V2.0 publisher for Xiaomi marble (ohos.hdi.location.gnss.v2_0.IGnssInterface).
 * Registers "gnss_interface_service" in HDF Device Service Manager (SA 5100) using libhdf_ipc_adapter.z.so
 * / libhdf_utils.z.so loaded dynamically via dlopen() and bridges QMI_LOC (service 0x10) over QRTR.
 *
 * Security & Runtime Notes:
 * - Zero C++ runtime / libstdc++ dependencies (NEEDED=[libc.so] on OHOS musl clang).
 * - Never prints coordinates or PII in status logs.
 * - Offline host tests compile cleanly with -DGNSS_HDI_HOST_TEST.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
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
#include <time.h>
#include <unistd.h>
#if defined(__has_include) && __has_include(<linux/qrtr.h>) && !defined(__OHOS__)
#include <linux/qrtr.h>
#else
struct sockaddr_qrtr {
	sa_family_t sq_family;
	uint32_t sq_node;
	uint32_t sq_port;
};
struct qrtr_ctrl_pkt {
	uint32_t cmd;
	union {
		struct {
			uint32_t node;
			uint32_t port;
		} client;
		struct {
			uint32_t service;
			uint32_t instance;
			uint32_t node;
			uint32_t port;
		} server;
		struct {
			uint32_t node;
		} bye;
	};
};
#endif

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

#ifndef QRTR_PORT_CTRL
#define QRTR_PORT_CTRL 0xfffffffeu
#endif

#ifndef QRTR_TYPE_NEW_SERVER
#define QRTR_TYPE_NEW_SERVER 4u
#define QRTR_TYPE_NEW_LOOKUP 10u
#define QRTR_TYPE_DEL_LOOKUP 11u
#endif

#if defined(__has_include) && __has_include("hdf_remote_service.h")
#include "hdf_remote_service.h"
#include "hdf_sbuf.h"
#else
enum HdfSbufType {
	SBUF_RAW = 0,
	SBUF_IPC,
	SBUF_IPC_HW,
};

struct HdfSBuf;
struct HdfRemoteService;

struct HdfObject {
	int32_t objectId;
};

struct HdfRemoteDispatcher {
	int (*Dispatch)(struct HdfRemoteService *service, int code,
			struct HdfSBuf *data, struct HdfSBuf *reply);
	int (*DispatchAsync)(struct HdfRemoteService *service, int code,
			     struct HdfSBuf *data, struct HdfSBuf *reply);
};

struct HdfRemoteService {
	struct HdfObject object;
	struct HdfObject *target;
	struct HdfRemoteDispatcher *dispatcher;
	uint64_t index;
};
#endif

#ifndef HDF_SUCCESS
#define HDF_SUCCESS 0
#define HDF_FAILURE (-1)
#define HDF_ERR_INVALID_PARAM (-3)
#endif

#if defined(GNSS_HDI_HDF_DRIVER) || defined(GNSS_HDI_HOST_TEST)
#if defined(__has_include) && __has_include("hdf_device_desc.h") && \
    __has_include("hdf_device_section.h") && __has_include("hdf_io_service_if.h")
#include "hdf_device_desc.h"
#include "hdf_device_object.h"
#else
struct HdfDeviceObject;
struct HdfDeviceIoClient;

struct IDeviceIoService {
	struct HdfObject object;
	int32_t (*Open)(struct HdfDeviceIoClient *client);
	int32_t (*Dispatch)(struct HdfDeviceIoClient *client, int cmdId,
			    struct HdfSBuf *data, struct HdfSBuf *reply);
	void (*Release)(struct HdfDeviceIoClient *client);
};

struct HdfDeviceObject {
	struct IDeviceIoService *service;
	const void *property;
	uint32_t deviceClass;
	void *priv;
};

struct HdfDeviceIoClient {
	struct HdfDeviceObject *device;
	void *priv;
};

struct HdfDriverEntry {
	int32_t moduleVersion;
	const char *moduleName;
	int32_t (*Bind)(struct HdfDeviceObject *deviceObject);
	int32_t (*Init)(struct HdfDeviceObject *deviceObject);
	void (*Release)(struct HdfDeviceObject *deviceObject);
};
#endif

_Static_assert(offsetof(struct HdfDeviceObject, service) == 0, "HdfDeviceObject service offset");
_Static_assert(offsetof(struct HdfDeviceIoClient, device) == 0, "HdfDeviceIoClient device offset");
_Static_assert(offsetof(struct IDeviceIoService, Open) == 8, "IDeviceIoService Open offset");
_Static_assert(offsetof(struct IDeviceIoService, Dispatch) == 16, "IDeviceIoService Dispatch offset");
_Static_assert(offsetof(struct IDeviceIoService, Release) == 24, "IDeviceIoService Release offset");
_Static_assert(offsetof(struct HdfDriverEntry, moduleName) == 8, "HdfDriverEntry moduleName offset");
_Static_assert(offsetof(struct HdfDriverEntry, Bind) == 16, "HdfDriverEntry Bind offset");
_Static_assert(offsetof(struct HdfDriverEntry, Init) == 24, "HdfDriverEntry Init offset");
_Static_assert(offsetof(struct HdfDriverEntry, Release) == 32, "HdfDriverEntry Release offset");
#endif

#define GNSS_SERVICE_NAME "gnss_interface_service"
#define GNSS_INTERFACE_DESC "ohos.hdi.location.gnss.v2_0.IGnssInterface"
#define GNSS_CALLBACK_DESC "ohos.hdi.location.gnss.v2_0.IGnssCallback"
#define DEVMGR_SERVICE_DESC "HDI.IServiceManager.V1_0"
#define DEVICE_SERVICE_MANAGER_SA_ID 5100
#define DEVSVC_MANAGER_ADD_SERVICE 1
#define DEVSVC_MANAGER_REMOVE_SERVICE 8
#define DEVICE_CLASS_DEFAULT 0u

/* IGnssInterface V2.0 IPC command codes */
enum {
	GNSS_CMD_GET_VERSION = 0,
	GNSS_CMD_SET_CONFIG_PARA = 1,
	GNSS_CMD_ENABLE_GNSS = 2,
	GNSS_CMD_DISABLE_GNSS = 3,
	GNSS_CMD_START_GNSS = 4,
	GNSS_CMD_STOP_GNSS = 5,
	GNSS_CMD_SET_REF_INFO = 6,
	GNSS_CMD_DELETE_AUX_DATA = 7,
	GNSS_CMD_SET_PREDICT_DATA = 8,
	GNSS_CMD_GET_CACHED_SIZE = 9,
	GNSS_CMD_GET_CACHED_LOCS = 10,
	GNSS_CMD_SEND_NI_RESPONSE = 11,
	GNSS_CMD_SEND_NI_MSG = 12,
	GNSS_CMD_ENABLE_MOCK = 13,
	GNSS_CMD_ADD_FENCE = 14,
};

/* IGnssCallback V2.0 IPC command codes */
enum {
	GNSS_CB_REPORT_LOCATION = 1,
	GNSS_CB_REPORT_WORKING_STATUS = 2,
	GNSS_CB_REPORT_NMEA = 3,
	GNSS_CB_REPORT_CAPABILITIES = 4,
	GNSS_CB_REPORT_SV_INFO = 5,
};

/* GnssWorkingStatus V2.0 enum values */
enum {
	GNSS_WS_NONE = 0,
	GNSS_WS_SESSION_BEGIN = 1,
	GNSS_WS_SESSION_END = 2,
	GNSS_WS_ENGINE_ON = 3,
	GNSS_WS_ENGINE_OFF = 4,
};

/* LocationInfo V2.0 validity mask bits */
enum {
	LOC_VALID_LAT = 1u << 0,
	LOC_VALID_LON = 1u << 1,
	LOC_VALID_ALT = 1u << 2,
	LOC_VALID_SPEED = 1u << 3,
	LOC_VALID_BEARING = 1u << 4,
	LOC_VALID_HACC = 1u << 5,
	LOC_VALID_VACC = 1u << 6,
	LOC_VALID_SACC = 1u << 7,
	LOC_VALID_BACC = 1u << 8,
	LOC_VALID_TIME_SINCE_BOOT = 1u << 9,
	LOC_VALID_TIME_UNC = 1u << 10,
};

/* Exact 80-byte ABI verified against OHOS liblocation_gnss_stub_2.0.z.so */
struct LocationInfoV2_0 {
	uint32_t fieldValidity;
	uint32_t _pad0;
	double latitude;
	double longitude;
	double altitude;
	float speed;
	float bearing;
	float horizontalAccuracy;
	float verticalAccuracy;
	float speedAccuracy;
	float bearingAccuracy;
	int64_t timeForFix;
	uint64_t timeSinceBoot;
	uint64_t timeUncertainty;
};

_Static_assert(sizeof(struct LocationInfoV2_0) == 80, "LocationInfoV2_0 must be 80 bytes");
_Static_assert(offsetof(struct LocationInfoV2_0, fieldValidity) == 0, "fieldValidity offset");
_Static_assert(offsetof(struct LocationInfoV2_0, latitude) == 8, "latitude offset");
_Static_assert(offsetof(struct LocationInfoV2_0, longitude) == 16, "longitude offset");
_Static_assert(offsetof(struct LocationInfoV2_0, altitude) == 24, "altitude offset");
_Static_assert(offsetof(struct LocationInfoV2_0, speed) == 32, "speed offset");
_Static_assert(offsetof(struct LocationInfoV2_0, horizontalAccuracy) == 40, "hacc offset");
_Static_assert(offsetof(struct LocationInfoV2_0, timeForFix) == 56, "timeForFix offset");
_Static_assert(offsetof(struct LocationInfoV2_0, timeSinceBoot) == 64, "timeSinceBoot offset");
_Static_assert(offsetof(struct LocationInfoV2_0, timeUncertainty) == 72, "timeUncertainty offset");

_Static_assert(offsetof(struct HdfRemoteService, dispatcher) == 16, "HdfRemoteService dispatcher offset");
_Static_assert(offsetof(struct HdfRemoteDispatcher, Dispatch) == 0, "HdfRemoteDispatcher Dispatch offset");

/* QMI_LOC definitions */
#define SVC_LOC 0x10u
#define QMI_REQUEST 0x00u
#define QMI_RESPONSE 0x02u
#define QMI_INDICATION 0x04u
#define QMI_HDR_LEN 7u

#define QMI_LOC_INFORM_CLIENT_REVISION 0x0020u
#define QMI_LOC_REG_EVENTS 0x0021u
#define QMI_LOC_START 0x0022u
#define QMI_LOC_STOP 0x0023u
#define QMI_LOC_POSITION_REPORT_IND 0x0024u
#define QMI_LOC_GNSS_SV_INFO_IND 0x0025u
#define QMI_LOC_NMEA_IND 0x0026u
#define QMI_LOC_ENGINE_STATE_IND 0x002bu
#define QMI_LOC_GET_SERVICE_REVISION 0x0032u
#define QMI_LOC_INJECT_UTC_TIME 0x0038u
#define QMI_LOC_SET_NMEA_TYPES 0x003eu

struct HDIServiceManager {
	struct HdfRemoteService *(*GetService)(struct HDIServiceManager *self, const char *serviceName);
	void *reserved1;
	void *reserved2;
	void *reserved3;
	struct HdfRemoteService *remote;
};
_Static_assert(sizeof(struct HDIServiceManager) == 40, "HDIServiceManager size");

struct hdf_api {
	struct HdfRemoteService *(*Obtain)(struct HdfObject *, struct HdfRemoteDispatcher *);
	struct HdfRemoteService *(*GetSa)(int32_t);
	void (*RecycleRemote)(struct HdfRemoteService *);
	bool (*SetDesc)(struct HdfRemoteService *, const char *);
	bool (*WriteToken)(struct HdfRemoteService *, struct HdfSBuf *);
	bool (*CheckToken)(struct HdfRemoteService *, struct HdfSBuf *);
	struct HdfSBuf *(*SbufObtain)(enum HdfSbufType);
	void (*SbufRecycle)(struct HdfSBuf *);
	bool (*WriteStr)(struct HdfSBuf *, const char *);
	bool (*WriteU16)(struct HdfSBuf *, uint16_t);
	bool (*WriteU32)(struct HdfSBuf *, uint32_t);
	bool (*WriteI32)(struct HdfSBuf *, int32_t);
	bool (*WriteI64)(struct HdfSBuf *, int64_t);
	bool (*WriteU64)(struct HdfSBuf *, uint64_t);
	bool (*WriteUnpad)(struct HdfSBuf *, const uint8_t *, uint32_t);
	int (*WriteRemote)(struct HdfSBuf *, struct HdfRemoteService *);
	struct HdfRemoteService *(*ReadRemote)(struct HdfSBuf *);
	bool (*ReadU32)(struct HdfSBuf *, uint32_t *);
	bool (*ReadI32)(struct HdfSBuf *, int32_t *);
	bool (*ReadI64)(struct HdfSBuf *, int64_t *);
	bool (*ReadU64)(struct HdfSBuf *, uint64_t *);
	const char *(*ReadStr)(struct HdfSBuf *);
	const uint8_t *(*ReadUnpad)(struct HdfSBuf *, uint32_t);
	struct HDIServiceManager *(*HdiMgrGet)(void);
	void (*HdiMgrRel)(struct HDIServiceManager *);
};

struct gnss_service_state {
	struct HdfObject base;
	struct HdfRemoteService *remote;
	struct HdfRemoteService *callback;
	pthread_mutex_t mu;
	bool enabled;
	bool started;
	int qrtr_fd;
	uint32_t loc_node;
	uint32_t loc_port;
	bool loc_ready;
	uint16_t txn;
	uint64_t reg_mask_sent;
	uint16_t reg_txn;
	uint16_t start_txn;
	bool reg_ok;
	bool svc_ready_logged;
	uint32_t eng_ind_cnt;
	uint32_t sv_ind_cnt;
	uint32_t nmea_ind_cnt;
	uint32_t pos_ind_cnt;
	uint32_t loc_sent_cnt;
	pthread_t loc_thread;
	bool loc_thread_started;
};

static struct hdf_api g_api;
static struct gnss_service_state g_gnss = {
	.base = { .objectId = 0 },
	.mu = PTHREAD_MUTEX_INITIALIZER,
	.qrtr_fd = -1,
};
#ifndef GNSS_HDI_HOST_TEST
static volatile sig_atomic_t g_stop = 0;

#ifndef GNSS_HDI_HDF_DRIVER
static void on_sig(int sig)
{
	(void)sig;
	g_stop = 1;
}
#endif
#endif

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
	return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static float get_f32_le(const uint8_t *p)
{
	uint32_t u = get_le32(p);
	float v = 0.0f;
	memcpy(&v, &u, sizeof(v));
	return v;
}

static double get_f64_le(const uint8_t *p)
{
	uint64_t u = get_le64(p);
	double v = 0.0;
	memcpy(&v, &u, sizeof(v));
	return v;
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_le64(uint8_t *p, uint64_t v)
{
	put_le32(p, (uint32_t)(v & 0xffffffffu));
	put_le32(p + 4, (uint32_t)(v >> 32));
}

static int64_t realtime_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return 0;
	return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000LL);
}

static uint64_t boottime_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int qmi_find_tlv(const uint8_t *tlvs, size_t len, uint8_t target,
			const uint8_t **out_val, uint16_t *out_len)
{
	size_t off = 0;
	while (off + 3 <= len) {
		uint8_t type = tlvs[off];
		uint16_t tlen = get_le16(tlvs + off + 1);
		if (off + 3 + (size_t)tlen > len)
			return -EPROTO;
		if (type == target) {
			*out_val = tlvs + off + 3;
			*out_len = tlen;
			return 0;
		}
		off += 3 + (size_t)tlen;
	}
	return -ENOENT;
}

/* Parse QMI_LOC_POSITION_REPORT_IND (0x0024) TLV payload into LocationInfoV2_0. */
static bool parse_qmi_loc_position_report(const uint8_t *tlvs, size_t len,
					  struct LocationInfoV2_0 *loc,
					  uint32_t *out_sess_status,
					  uint32_t *out_tech_mask,
					  float *out_hacc)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	memset(loc, 0, sizeof(*loc));
	if (out_sess_status)
		*out_sess_status = 0xffffffffu;
	if (out_tech_mask)
		*out_tech_mask = 0u;
	if (out_hacc)
		*out_hacc = -1.0f;

	if (qmi_find_tlv(tlvs, len, 0x01, &v, &vl) != 0 || vl < 4)
		return false;
	uint32_t sess_status = get_le32(v);
	if (out_sess_status)
		*out_sess_status = sess_status;
	if (sess_status != 0 && sess_status != 1)
		return false;

	/* QMI_LOC TLV 0x23: technologyMask (bit 0 = SATELLITE, bit 1 = CELLID, bit 2 = WIFI).
	 * Reject non-satellite rough fixes (e.g. CELLID-only tech=0x2 with sv=0 once cellular
	 * is online) so GNSS HDI never overrides accurate location with coarse tower guesses. */
	bool has_tech = false;
	uint32_t tech_mask = 0u;
	if (qmi_find_tlv(tlvs, len, 0x23, &v, &vl) == 0 && vl >= 4) {
		has_tech = true;
		tech_mask = get_le32(v);
		if (out_tech_mask)
			*out_tech_mask = tech_mask;
		if ((tech_mask & 0x01u) == 0u)
			return false;
	}

	if (qmi_find_tlv(tlvs, len, 0x10, &v, &vl) != 0 || vl < 8)
		return false;
	double lat = get_f64_le(v);
	if (!isfinite(lat) || lat < -90.0 || lat > 90.0)
		return false;
	loc->latitude = lat;
	loc->fieldValidity |= LOC_VALID_LAT;

	if (qmi_find_tlv(tlvs, len, 0x11, &v, &vl) != 0 || vl < 8)
		return false;
	double lon = get_f64_le(v);
	if (!isfinite(lon) || lon < -180.0 || lon > 180.0)
		return false;
	if (fabs(lat) < 1e-6 && fabs(lon) < 1e-6)
		return false;
	loc->longitude = lon;
	loc->fieldValidity |= LOC_VALID_LON;

	if (qmi_find_tlv(tlvs, len, 0x12, &v, &vl) == 0 && vl >= 4) {
		float hacc = get_f32_le(v);
		if (isfinite(hacc) && hacc >= 0.0f) {
			if (out_hacc)
				*out_hacc = hacc;
			/* Drop coarse IN_PROGRESS estimates (>250m) even without tech TLV. */
			if (sess_status != 0 && hacc > 250.0f)
				return false;
			if (!has_tech && hacc > 1000.0f)
				return false;
			loc->horizontalAccuracy = hacc;
			loc->fieldValidity |= LOC_VALID_HACC;
		}
	}
	if ((qmi_find_tlv(tlvs, len, 0x1a, &v, &vl) == 0 ||
	     qmi_find_tlv(tlvs, len, 0x1b, &v, &vl) == 0) && vl >= 4) {
		float alt = get_f32_le(v);
		if (isfinite(alt)) {
			loc->altitude = (double)alt;
			loc->fieldValidity |= LOC_VALID_ALT;
		}
	}
	if (qmi_find_tlv(tlvs, len, 0x1c, &v, &vl) == 0 && vl >= 4) {
		float vacc = get_f32_le(v);
		if (isfinite(vacc) && vacc >= 0.0f) {
			loc->verticalAccuracy = vacc;
			loc->fieldValidity |= LOC_VALID_VACC;
		}
	}
	if (qmi_find_tlv(tlvs, len, 0x18, &v, &vl) == 0 && vl >= 4) {
		float spd = get_f32_le(v);
		if (isfinite(spd) && spd >= 0.0f) {
			loc->speed = spd;
			loc->fieldValidity |= LOC_VALID_SPEED;
		}
	}
	if (qmi_find_tlv(tlvs, len, 0x19, &v, &vl) == 0 && vl >= 4) {
		float sacc = get_f32_le(v);
		if (isfinite(sacc) && sacc >= 0.0f) {
			loc->speedAccuracy = sacc;
			loc->fieldValidity |= LOC_VALID_SACC;
		}
	}
	if (qmi_find_tlv(tlvs, len, 0x20, &v, &vl) == 0 && vl >= 4) {
		float brg = get_f32_le(v);
		if (isfinite(brg) && brg >= 0.0f && brg <= 360.0f) {
			loc->bearing = brg;
			loc->fieldValidity |= LOC_VALID_BEARING;
		}
	}
	if (qmi_find_tlv(tlvs, len, 0x21, &v, &vl) == 0 && vl >= 4) {
		float bacc = get_f32_le(v);
		if (isfinite(bacc) && bacc >= 0.0f) {
			loc->bearingAccuracy = bacc;
			loc->fieldValidity |= LOC_VALID_BACC;
		}
	}
	if (qmi_find_tlv(tlvs, len, 0x25, &v, &vl) == 0 && vl >= 8)
		loc->timeForFix = (int64_t)get_le64(v);
	else
		loc->timeForFix = realtime_ms();

	loc->timeSinceBoot = boottime_ns();
	loc->fieldValidity |= LOC_VALID_TIME_SINCE_BOOT;
	return true;
}

static bool parse_qmi_loc_nmea(const uint8_t *tlvs, size_t len, char *out, size_t cap, int32_t *out_len)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (qmi_find_tlv(tlvs, len, 0x01, &v, &vl) != 0 || vl == 0)
		return false;
	size_t n = vl;
	if (n >= cap)
		n = cap - 1;
	size_t j = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = v[i];
		if (c == '\0')
			break;
		if (c >= 0x20 || c == '\r' || c == '\n')
			out[j++] = (char)c;
	}
	out[j] = '\0';
	if (j == 0)
		return false;
	*out_len = (int32_t)j;
	return true;
}

static bool parse_qmi_loc_engine_state(const uint8_t *tlvs, size_t len, uint64_t *ws_out)
{
	const uint8_t *v = NULL;
	uint16_t vl = 0;
	if (qmi_find_tlv(tlvs, len, 0x01, &v, &vl) != 0 || vl < 4)
		return false;
	uint32_t st = get_le32(v);
	if (st == 1) {
		*ws_out = GNSS_WS_ENGINE_ON;
		return true;
	}
	if (st == 2) {
		*ws_out = GNSS_WS_ENGINE_OFF;
		return true;
	}
	return false;
}

#ifndef GNSS_HDI_HOST_TEST
static struct HdfRemoteService *copy_cb_ref(void)
{
	pthread_mutex_lock(&g_gnss.mu);
	struct HdfRemoteService *cb = g_gnss.callback;
	pthread_mutex_unlock(&g_gnss.mu);
	return cb;
}
#endif

static int dispatch_cb_working_status(struct HdfRemoteService *cb, uint64_t status)
{
	if (!cb || !cb->dispatcher || !cb->dispatcher->Dispatch)
		return HDF_ERR_INVALID_PARAM;
	struct HdfSBuf *d = g_api.SbufObtain(SBUF_IPC);
	struct HdfSBuf *r = g_api.SbufObtain(SBUF_IPC);
	int rc = HDF_FAILURE;
	if (d && r && g_api.WriteToken(cb, d) && g_api.WriteU64(d, status))
		rc = cb->dispatcher->Dispatch(cb, GNSS_CB_REPORT_WORKING_STATUS, d, r);
	if (d) g_api.SbufRecycle(d);
	if (r) g_api.SbufRecycle(r);
	return rc;
}

static int dispatch_cb_caps(struct HdfRemoteService *cb, uint32_t caps)
{
	if (!cb || !cb->dispatcher || !cb->dispatcher->Dispatch)
		return HDF_ERR_INVALID_PARAM;
	struct HdfSBuf *d = g_api.SbufObtain(SBUF_IPC);
	struct HdfSBuf *r = g_api.SbufObtain(SBUF_IPC);
	int rc = HDF_FAILURE;
	if (d && r && g_api.WriteToken(cb, d) && g_api.WriteU32(d, caps))
		rc = cb->dispatcher->Dispatch(cb, GNSS_CB_REPORT_CAPABILITIES, d, r);
	if (d) g_api.SbufRecycle(d);
	if (r) g_api.SbufRecycle(r);
	return rc;
}

static int dispatch_cb_location(struct HdfRemoteService *cb, const struct LocationInfoV2_0 *loc)
{
	if (!cb || !cb->dispatcher || !cb->dispatcher->Dispatch)
		return HDF_ERR_INVALID_PARAM;
	struct HdfSBuf *d = g_api.SbufObtain(SBUF_IPC);
	struct HdfSBuf *r = g_api.SbufObtain(SBUF_IPC);
	int rc = HDF_FAILURE;
	if (d && r && g_api.WriteToken(cb, d) &&
	    g_api.WriteUnpad(d, (const uint8_t *)loc, (uint32_t)sizeof(*loc)))
		rc = cb->dispatcher->Dispatch(cb, GNSS_CB_REPORT_LOCATION, d, r);
	if (d) g_api.SbufRecycle(d);
	if (r) g_api.SbufRecycle(r);
	return rc;
}

static int dispatch_cb_nmea(struct HdfRemoteService *cb, int64_t ts, const char *nmea, int32_t len)
{
	if (!cb || !cb->dispatcher || !cb->dispatcher->Dispatch)
		return HDF_ERR_INVALID_PARAM;
	struct HdfSBuf *d = g_api.SbufObtain(SBUF_IPC);
	struct HdfSBuf *r = g_api.SbufObtain(SBUF_IPC);
	int rc = HDF_FAILURE;
	if (d && r && g_api.WriteToken(cb, d) &&
	    g_api.WriteI64(d, ts) && g_api.WriteStr(d, nmea) && g_api.WriteI32(d, len))
		rc = cb->dispatcher->Dispatch(cb, GNSS_CB_REPORT_NMEA, d, r);
	if (d) g_api.SbufRecycle(d);
	if (r) g_api.SbufRecycle(r);
	return rc;
}

static uint16_t qmi_loc_send_cmd(uint16_t msg_id, const uint8_t *tlvs, uint16_t tlv_len)
{
	pthread_mutex_lock(&g_gnss.mu);
	int fd = g_gnss.qrtr_fd;
	bool ready = g_gnss.loc_ready;
	uint32_t node = g_gnss.loc_node;
	uint32_t port = g_gnss.loc_port;
	uint16_t txn = ++g_gnss.txn;
	if (txn == 0)
		txn = ++g_gnss.txn;
	pthread_mutex_unlock(&g_gnss.mu);

	if (fd < 0 || !ready || QMI_HDR_LEN + tlv_len > 256)
		return 0;

	uint8_t pkt[256];
	pkt[0] = QMI_REQUEST;
	put_le16(pkt + 1, txn);
	put_le16(pkt + 3, msg_id);
	put_le16(pkt + 5, tlv_len);
	if (tlv_len > 0 && tlvs)
		memcpy(pkt + QMI_HDR_LEN, tlvs, tlv_len);

	struct sockaddr_qrtr dst = {
		.sq_family = AF_QIPCRTR,
		.sq_node = node,
		.sq_port = port,
	};
	(void)sendto(fd, pkt, QMI_HDR_LEN + tlv_len, MSG_DONTWAIT,
		     (struct sockaddr *)&dst, sizeof(dst));
	return txn;
}

static void send_loc_inform_rev(void)
{
	uint8_t tlv[7];
	tlv[0] = 0x01;
	put_le16(tlv + 1, 4);
	put_le32(tlv + 3, 2u);
	(void)qmi_loc_send_cmd(QMI_LOC_INFORM_CLIENT_REVISION, tlv, sizeof(tlv));
}

static size_t build_loc_reg_events_tlv(uint8_t *tlv, size_t cap, uint64_t mask)
{
	if (!tlv || cap < 29)
		return 0;
	/* TLV 0x01: eventRegMask (u64) */
	tlv[0] = 0x01;
	put_le16(tlv + 1, 8);
	put_le64(tlv + 3, mask);
	/* TLV 0x10: clientStrId ("MHAL", 4 raw ASCII bytes, no length prefix) */
	tlv[11] = 0x10;
	put_le16(tlv + 12, 4);
	memcpy(tlv + 14, "MHAL", 4);
	/* TLV 0x11: clientType = 1 (eQMI_LOC_CLIENT_AFW_V02, u32) */
	tlv[18] = 0x11;
	put_le16(tlv + 19, 4);
	put_le32(tlv + 21, 1u);
	/* TLV 0x12: enablePosRequestNotification = 0 (u8) */
	tlv[25] = 0x12;
	put_le16(tlv + 26, 1);
	tlv[28] = 0;
	return 29u;
}

static void send_loc_reg_events(uint64_t mask)
{
	uint8_t tlv[32];
	uint16_t tlen = (uint16_t)build_loc_reg_events_tlv(tlv, sizeof(tlv), mask);
	pthread_mutex_lock(&g_gnss.mu);
	g_gnss.reg_mask_sent = mask;
	pthread_mutex_unlock(&g_gnss.mu);
	uint16_t txn = qmi_loc_send_cmd(QMI_LOC_REG_EVENTS, tlv, tlen);
	if (txn != 0) {
		pthread_mutex_lock(&g_gnss.mu);
		g_gnss.reg_txn = txn;
		pthread_mutex_unlock(&g_gnss.mu);
	}
}

static size_t build_loc_set_nmea_types_tlv(uint8_t *tlv, size_t cap, uint32_t types)
{
	if (!tlv || cap < 7)
		return 0;
	tlv[0] = 0x01;
	put_le16(tlv + 1, 4);
	put_le32(tlv + 3, types);
	return 7u;
}

#ifndef GNSS_HDI_HOST_TEST
static void send_loc_set_nmea_types(uint32_t types)
{
	uint8_t tlv[8];
	uint16_t tlen = (uint16_t)build_loc_set_nmea_types_tlv(tlv, sizeof(tlv), types);
	(void)qmi_loc_send_cmd(QMI_LOC_SET_NMEA_TYPES, tlv, tlen);
}
#endif

static size_t build_loc_inject_utc_time_tlv(uint8_t *tlv, size_t cap,
					    uint64_t utc_ms, uint32_t unc_ms)
{
	if (!tlv || cap < 18 || utc_ms < 1700000000000ULL)
		return 0;
	/* TLV 0x01: timeUtc (ms since 1970-01-01 UTC, u64) */
	tlv[0] = 0x01;
	put_le16(tlv + 1, 8);
	put_le64(tlv + 3, utc_ms);
	/* TLV 0x02: timeUnc (uncertainty in ms, u32) */
	tlv[11] = 0x02;
	put_le16(tlv + 12, 4);
	put_le32(tlv + 14, unc_ms);
	return 18u;
}

static void send_loc_inject_utc_time(void)
{
	int64_t now = realtime_ms();
	if (now < 1700000000000LL)
		return;
	uint8_t tlv[20];
	uint16_t tlen = (uint16_t)build_loc_inject_utc_time_tlv(tlv, sizeof(tlv),
								 (uint64_t)now, 5000u);
	if (tlen > 0)
		(void)qmi_loc_send_cmd(QMI_LOC_INJECT_UTC_TIME, tlv, tlen);
}

static size_t build_loc_start_tlv(uint8_t *tlv, size_t cap)
{
	if (!tlv || cap < 25)
		return 0;
	/* TLV 0x01: sessionId = 1 (u8) */
	tlv[0] = 0x01;
	put_le16(tlv + 1, 1);
	tlv[3] = 1;
	/* TLV 0x10: fixRecurrenceType = 1 (periodic, u32) */
	tlv[4] = 0x10;
	put_le16(tlv + 5, 4);
	put_le32(tlv + 7, 1u);
	/* TLV 0x12: intermediateReportState = 1 (enable, u32) */
	tlv[11] = 0x12;
	put_le16(tlv + 12, 4);
	put_le32(tlv + 14, 1u);
	/* TLV 0x13: minimumInterval = 1000ms (u32) */
	tlv[18] = 0x13;
	put_le16(tlv + 19, 4);
	put_le32(tlv + 21, 1000u);
	return 25u;
}

static void send_loc_start(void)
{
	uint8_t tlv[32];
	uint16_t tlen = (uint16_t)build_loc_start_tlv(tlv, sizeof(tlv));
	uint16_t txn = qmi_loc_send_cmd(QMI_LOC_START, tlv, tlen);
	if (txn != 0) {
		pthread_mutex_lock(&g_gnss.mu);
		g_gnss.start_txn = txn;
		pthread_mutex_unlock(&g_gnss.mu);
	}
}

static void send_loc_stop(void)
{
	uint8_t tlv[4];
	tlv[0] = 0x01;
	put_le16(tlv + 1, 1);
	tlv[3] = 1; /* sessionId = 1 */
	(void)qmi_loc_send_cmd(QMI_LOC_STOP, tlv, sizeof(tlv));
}

#ifndef GNSS_HDI_HOST_TEST
static void gnss_append_status_line(const char *line)
{
	printf("%s\n", line);
	FILE *sf = fopen("/data/local/tmp/gnss-hdi/status.log", "a");
	if (sf) {
		fprintf(sf, "%s\n", line);
		fclose(sf);
	}
}

static void *qmi_loc_worker(void *arg)
{
	(void)arg;
	int fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0)
		return NULL;
	struct sockaddr_qrtr local = {0};
	socklen_t sl = sizeof(local);
	if (getsockname(fd, (struct sockaddr *)&local, &sl) < 0 ||
	    local.sq_family != AF_QIPCRTR) {
		close(fd);
		return NULL;
	}

	pthread_mutex_lock(&g_gnss.mu);
	g_gnss.qrtr_fd = fd;
	pthread_mutex_unlock(&g_gnss.mu);

	/* Start continuous QRTR lookup for SVC_LOC (0x10) so modem up/down works dynamically. */
	struct qrtr_ctrl_pkt req = {0};
	uint8_t *raw = (uint8_t *)&req;
	struct sockaddr_qrtr ctrl = {
		.sq_family = AF_QIPCRTR,
		.sq_node = local.sq_node,
		.sq_port = QRTR_PORT_CTRL,
	};
	put_le32(raw + offsetof(struct qrtr_ctrl_pkt, cmd), QRTR_TYPE_NEW_LOOKUP);
	put_le32(raw + offsetof(struct qrtr_ctrl_pkt, server.service), SVC_LOC);
	(void)sendto(fd, raw, sizeof(req), 0, (struct sockaddr *)&ctrl, sizeof(ctrl));

	while (!g_stop) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int prc = poll(&pfd, 1, 1000);
		if (prc <= 0 || !(pfd.revents & POLLIN))
			continue;

		uint8_t buf[4096];
		struct sockaddr_qrtr from = {0};
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
				     (struct sockaddr *)&from, &fl);
		if (n <= 0)
			continue;

		if (from.sq_port == QRTR_PORT_CTRL && (size_t)n == sizeof(struct qrtr_ctrl_pkt)) {
			uint32_t cmd = get_le32(buf + offsetof(struct qrtr_ctrl_pkt, cmd));
			uint32_t svc = get_le32(buf + offsetof(struct qrtr_ctrl_pkt, server.service));
			uint32_t node = get_le32(buf + offsetof(struct qrtr_ctrl_pkt, server.node));
			uint32_t port = get_le32(buf + offsetof(struct qrtr_ctrl_pkt, server.port));
			if (cmd == QRTR_TYPE_NEW_SERVER && svc == SVC_LOC && node != local.sq_node && port) {
				bool need_enable = false, need_start = false, log_ready = false;
				pthread_mutex_lock(&g_gnss.mu);
				g_gnss.loc_node = node;
				g_gnss.loc_port = port;
				g_gnss.loc_ready = true;
				g_gnss.reg_ok = false;
				if (!g_gnss.svc_ready_logged) {
					g_gnss.svc_ready_logged = true;
					log_ready = true;
				}
				need_enable = g_gnss.enabled;
				need_start = g_gnss.started;
				pthread_mutex_unlock(&g_gnss.mu);
				if (log_ready)
					gnss_append_status_line("GNSS_QMI_LOC_SVC_READY");
				qmi_loc_send_cmd(QMI_LOC_GET_SERVICE_REVISION, NULL, 0);
				if (need_enable) {
					send_loc_inform_rev();
					send_loc_reg_events(0x0187ULL);
					send_loc_inject_utc_time();
				}
				if (need_start)
					send_loc_start();
			}
			continue;
		}

		if ((size_t)n < QMI_HDR_LEN)
			continue;
		uint8_t mtype = buf[0];
		uint16_t txn = get_le16(buf + 1);
		uint16_t msg_id = get_le16(buf + 3);
		uint16_t tlv_len = get_le16(buf + 5);
		if (QMI_HDR_LEN + (size_t)tlv_len > (size_t)n)
			continue;
		const uint8_t *tlvs = buf + QMI_HDR_LEN;

		if (mtype == QMI_RESPONSE) {
			const uint8_t *rv = NULL;
			uint16_t rvl = 0;
			if (qmi_find_tlv(tlvs, tlv_len, 0x02, &rv, &rvl) == 0 && rvl >= 4) {
				uint16_t qres = get_le16(rv);
				uint16_t qerr = get_le16(rv + 2);
				pthread_mutex_lock(&g_gnss.mu);
				uint64_t smask = g_gnss.reg_mask_sent;
				uint16_t rtxn = g_gnss.reg_txn;
				uint16_t stxn = g_gnss.start_txn;
				pthread_mutex_unlock(&g_gnss.mu);
				char msg[128];
				if (qres == 0 && qerr == 0) {
					if (msg_id == QMI_LOC_REG_EVENTS && (rtxn == 0 || txn == rtxn)) {
						pthread_mutex_lock(&g_gnss.mu);
						g_gnss.reg_ok = true;
						pthread_mutex_unlock(&g_gnss.mu);
						snprintf(msg, sizeof(msg), "GNSS_QMI_LOC_REG_OK mask=0x%llx",
							 (unsigned long long)smask);
						gnss_append_status_line(msg);
						send_loc_set_nmea_types(0x0000000fu);
						send_loc_inject_utc_time();
					} else if (msg_id == QMI_LOC_START && (stxn == 0 || txn == stxn)) {
						gnss_append_status_line("GNSS_QMI_LOC_START_OK");
					}
				} else if (msg_id == QMI_LOC_REG_EVENTS && (rtxn == 0 || txn == rtxn)) {
					snprintf(msg, sizeof(msg), "GNSS_QMI_LOC_REG_ERR mask=0x%llx err=0x%x",
						 (unsigned long long)smask, qerr);
					gnss_append_status_line(msg);
					if (smask == 0x0187ULL)
						send_loc_reg_events(0x0087ULL);
					else if (smask == 0x0087ULL)
						send_loc_reg_events(0x0007ULL);
					else if (smask == 0x0007ULL)
						send_loc_reg_events(0x0003ULL);
				} else if (msg_id == QMI_LOC_START && (stxn == 0 || txn == stxn)) {
					snprintf(msg, sizeof(msg), "GNSS_QMI_LOC_START_ERR err=0x%x", qerr);
					gnss_append_status_line(msg);
				}
			}
			continue;
		}
		if (mtype != QMI_INDICATION)
			continue;

		char ind_log[128] = "";
		if (msg_id == QMI_LOC_ENGINE_STATE_IND) {
			uint32_t cnt = ++g_gnss.eng_ind_cnt;
			if (cnt == 1 || cnt == 5)
				snprintf(ind_log, sizeof(ind_log), "GNSS_QMI_LOC_IND type=eng count=%u", cnt);
		} else if (msg_id == QMI_LOC_GNSS_SV_INFO_IND) {
			uint32_t cnt = ++g_gnss.sv_ind_cnt;
			const uint8_t *svv = NULL;
			uint16_t svl = 0;
			uint8_t sc = (qmi_find_tlv(tlvs, tlv_len, 0x10, &svv, &svl) == 0 && svl >= 1) ? svv[0] : 0;
			if (cnt == 1 || cnt == 5)
				snprintf(ind_log, sizeof(ind_log), "GNSS_QMI_LOC_IND type=sv count=%u sv=%u", cnt, sc);
		} else if (msg_id == QMI_LOC_NMEA_IND) {
			uint32_t cnt = ++g_gnss.nmea_ind_cnt;
			if (cnt == 1 || cnt == 10)
				snprintf(ind_log, sizeof(ind_log), "GNSS_QMI_LOC_IND type=nmea count=%u", cnt);
		} else if (msg_id == QMI_LOC_POSITION_REPORT_IND) {
			uint32_t cnt = ++g_gnss.pos_ind_cnt;
			if (cnt == 1 || cnt == 5)
				snprintf(ind_log, sizeof(ind_log), "GNSS_QMI_LOC_IND type=pos count=%u", cnt);
		}
		if (ind_log[0] != '\0')
			gnss_append_status_line(ind_log);

		struct HdfRemoteService *cb = copy_cb_ref();
		if (!cb)
			continue;

		if (msg_id == QMI_LOC_POSITION_REPORT_IND) {
			struct LocationInfoV2_0 loc;
			uint32_t ss = 0xffffffffu, tm = 0u;
			float hacc = -1.0f;
			if (parse_qmi_loc_position_report(tlvs, tlv_len, &loc, &ss, &tm, &hacc)) {
				int drc = dispatch_cb_location(cb, &loc);
				pthread_mutex_lock(&g_gnss.mu);
				uint32_t sc = ++g_gnss.loc_sent_cnt;
				pthread_mutex_unlock(&g_gnss.mu);
				if (sc == 1 || sc == 5 || sc == 20) {
					char dmsg[96];
					snprintf(dmsg, sizeof(dmsg),
						 "GNSS_HDI_LOC_DISPATCH count=%u rc=%d sess=%u tech=0x%x",
						 sc, drc, ss, tm);
					gnss_append_status_line(dmsg);
				}
			} else {
				static uint32_t skip_cnt = 0;
				skip_cnt++;
				if (skip_cnt == 1 || skip_cnt == 5 || skip_cnt == 20) {
					char smsg[96];
					snprintf(smsg, sizeof(smsg),
						 "GNSS_QMI_LOC_SKIP_NONSAT count=%u sess=%u tech=0x%x hacc=%.1f",
						 skip_cnt, ss, tm, (double)hacc);
					gnss_append_status_line(smsg);
				}
			}
		} else if (msg_id == QMI_LOC_NMEA_IND) {
			char nmea[256];
			int32_t nlen = 0;
			if (parse_qmi_loc_nmea(tlvs, tlv_len, nmea, sizeof(nmea), &nlen))
				(void)dispatch_cb_nmea(cb, realtime_ms(), nmea, nlen);
		} else if (msg_id == QMI_LOC_ENGINE_STATE_IND) {
			uint64_t ws = 0;
			if (parse_qmi_loc_engine_state(tlvs, tlv_len, &ws))
				(void)dispatch_cb_working_status(cb, ws);
		}
	}

	pthread_mutex_lock(&g_gnss.mu);
	g_gnss.qrtr_fd = -1;
	g_gnss.loc_ready = false;
	pthread_mutex_unlock(&g_gnss.mu);
	close(fd);
	return NULL;
}
#endif

static int gnss_dispatch_common(struct gnss_service_state *st, int code,
				struct HdfSBuf *data, struct HdfSBuf *reply)
{
	if (!st || !data)
		return HDF_ERR_INVALID_PARAM;

	switch (code) {
	case GNSS_CMD_GET_VERSION:
		if (!reply || !g_api.WriteU32(reply, 2u) || !g_api.WriteU32(reply, 0u))
			return HDF_FAILURE;
		return HDF_SUCCESS;

	case GNSS_CMD_SET_CONFIG_PARA:
		return HDF_SUCCESS;

	case GNSS_CMD_ENABLE_GNSS: {
		struct HdfRemoteService *cb = g_api.ReadRemote(data);
		if (!cb)
			return HDF_ERR_INVALID_PARAM;
		(void)g_api.SetDesc(cb, GNSS_CALLBACK_DESC);
		pthread_mutex_lock(&st->mu);
		struct HdfRemoteService *old_cb = st->callback;
		st->callback = cb;
		st->enabled = true;
		st->reg_ok = false;
		pthread_mutex_unlock(&st->mu);
		if (old_cb && g_api.RecycleRemote)
			g_api.RecycleRemote(old_cb);
#ifndef GNSS_HDI_HOST_TEST
		gnss_append_status_line("GNSS_HDI_ENABLE");
#endif
		(void)dispatch_cb_caps(cb, 0x01u);
		(void)dispatch_cb_working_status(cb, GNSS_WS_ENGINE_ON);
		send_loc_inform_rev();
		send_loc_reg_events(0x0187ULL);
		send_loc_inject_utc_time();
		return HDF_SUCCESS;
	}

	case GNSS_CMD_DISABLE_GNSS: {
		pthread_mutex_lock(&st->mu);
		bool was_started = st->started;
		st->started = false;
		st->enabled = false;
		st->reg_ok = false;
		struct HdfRemoteService *cb = st->callback;
		st->callback = NULL;
		pthread_mutex_unlock(&st->mu);
#ifndef GNSS_HDI_HOST_TEST
		gnss_append_status_line("GNSS_HDI_DISABLE");
#endif
		if (was_started)
			send_loc_stop();
		if (cb) {
			(void)dispatch_cb_working_status(cb, GNSS_WS_ENGINE_OFF);
			if (g_api.RecycleRemote)
				g_api.RecycleRemote(cb);
		}
		return HDF_SUCCESS;
	}

	case GNSS_CMD_START_GNSS: {
		uint64_t start_type = 0;
		(void)g_api.ReadU64(data, &start_type);
		pthread_mutex_lock(&st->mu);
		st->started = true;
		bool need_reg = !st->reg_ok;
		struct HdfRemoteService *cb = st->callback;
		pthread_mutex_unlock(&st->mu);
#ifndef GNSS_HDI_HOST_TEST
		gnss_append_status_line("GNSS_HDI_START");
#endif
		if (cb)
			(void)dispatch_cb_working_status(cb, GNSS_WS_SESSION_BEGIN);
		send_loc_inform_rev();
		if (need_reg)
			send_loc_reg_events(0x0187ULL);
		send_loc_inject_utc_time();
		send_loc_start();
		return HDF_SUCCESS;
	}

	case GNSS_CMD_STOP_GNSS: {
		uint64_t start_type = 0;
		(void)g_api.ReadU64(data, &start_type);
		pthread_mutex_lock(&st->mu);
		st->started = false;
		struct HdfRemoteService *cb = st->callback;
		pthread_mutex_unlock(&st->mu);
#ifndef GNSS_HDI_HOST_TEST
		gnss_append_status_line("GNSS_HDI_STOP");
#endif
		send_loc_stop();
		if (cb)
			(void)dispatch_cb_working_status(cb, GNSS_WS_SESSION_END);
		return HDF_SUCCESS;
	}

	case GNSS_CMD_GET_CACHED_SIZE:
		if (!reply || !g_api.WriteI32(reply, 0))
			return HDF_FAILURE;
		return HDF_SUCCESS;

	case GNSS_CMD_SET_REF_INFO:
	case GNSS_CMD_DELETE_AUX_DATA:
	case GNSS_CMD_SET_PREDICT_DATA:
	case GNSS_CMD_GET_CACHED_LOCS:
	case GNSS_CMD_SEND_NI_RESPONSE:
	case GNSS_CMD_SEND_NI_MSG:
	case GNSS_CMD_ENABLE_MOCK:
	case GNSS_CMD_ADD_FENCE:
		return HDF_SUCCESS;

	default:
		return HDF_ERR_INVALID_PARAM;
	}
}

#ifndef GNSS_HDI_HDF_DRIVER
static int gnss_stub_dispatch(struct HdfRemoteService *target_arg, int code,
			      struct HdfSBuf *data, struct HdfSBuf *reply)
{
	struct gnss_service_state *st = (struct gnss_service_state *)target_arg;
	if (!st || !st->remote || !data)
		return HDF_ERR_INVALID_PARAM;
	if (!g_api.CheckToken(st->remote, data))
		return HDF_ERR_INVALID_PARAM;
	return gnss_dispatch_common(st, code, data, reply);
}
#endif

#ifndef GNSS_HDI_HOST_TEST
static int load_hdf_api(bool need_svcmgr)
{
	if (g_api.SbufObtain && g_api.SbufRecycle && g_api.WriteToken &&
	    g_api.CheckToken && g_api.ReadRemote && (!need_svcmgr || g_api.HdiMgrGet))
		return 0;

	void *ipc = dlopen("libhdf_ipc_adapter.z.so", RTLD_NOW | RTLD_GLOBAL);
	if (!ipc)
		ipc = dlopen("/system/lib64/chipset-sdk-sp/libhdf_ipc_adapter.z.so", RTLD_NOW | RTLD_GLOBAL);
	if (!ipc)
		ipc = dlopen("/system/lib64/chipset-pub-sdk/libhdf_ipc_adapter.z.so", RTLD_NOW | RTLD_GLOBAL);
	void *utils = dlopen("libhdf_utils.z.so", RTLD_NOW | RTLD_GLOBAL);
	if (!utils)
		utils = dlopen("/system/lib64/chipset-sdk-sp/libhdf_utils.z.so", RTLD_NOW | RTLD_GLOBAL);
	if (!utils)
		utils = dlopen("/system/lib64/chipset-sdk/libhdf_utils.z.so", RTLD_NOW | RTLD_GLOBAL);
	void *hdi = NULL;
	if (need_svcmgr) {
		hdi = dlopen("libhdi.z.so", RTLD_NOW | RTLD_GLOBAL);
		if (!hdi)
			hdi = dlopen("/system/lib64/chipset-pub-sdk/libhdi.z.so", RTLD_NOW | RTLD_GLOBAL);
		if (!hdi)
			hdi = dlopen("/system/lib64/chipset-sdk-sp/libhdi.z.so", RTLD_NOW | RTLD_GLOBAL);
	}
	if (need_svcmgr && !hdi) {
		fprintf(stderr, "GNSS_HDI_DLOPEN_ERR ipc=%p utils=%p hdi=%p err=%s\n",
			ipc, utils, hdi, dlerror() ? dlerror() : "none");
		gnss_append_status_line("GNSS_HDI_DLOPEN_ERR");
		return -1;
	}

#define SYM(field, handle, name) do { \
	void *fn_ = (handle) ? dlsym((handle), (name)) : NULL; \
	if (!fn_) \
		fn_ = dlsym(RTLD_DEFAULT, (name)); \
	*(void **)(&(g_api.field)) = fn_; \
	if (!g_api.field) { \
		fprintf(stderr, "GNSS_HDI_MISSING_SYM=%s\n", (name)); \
		gnss_append_status_line("GNSS_HDI_MISSING_SYM"); \
		return -1; \
	} \
} while (0)

	SYM(Obtain, ipc, "HdfRemoteServiceObtain");
	SYM(GetSa, ipc, "HdfRemoteServiceGet");
	SYM(RecycleRemote, ipc, "HdfRemoteServiceRecycle");
	SYM(SetDesc, ipc, "HdfRemoteServiceSetInterfaceDesc");
	SYM(WriteToken, ipc, "HdfRemoteServiceWriteInterfaceToken");
	SYM(CheckToken, ipc, "HdfRemoteServiceCheckInterfaceToken");
	SYM(SbufObtain, utils, "HdfSbufTypedObtain");
	SYM(SbufRecycle, utils, "HdfSbufRecycle");
	SYM(WriteStr, utils, "HdfSbufWriteString");
	SYM(WriteU16, utils, "HdfSbufWriteUint16");
	SYM(WriteU32, utils, "HdfSbufWriteUint32");
	SYM(WriteI32, utils, "HdfSbufWriteInt32");
	SYM(WriteI64, utils, "HdfSbufWriteInt64");
	SYM(WriteU64, utils, "HdfSbufWriteUint64");
	SYM(WriteUnpad, utils, "HdfSbufWriteUnpadBuffer");
	SYM(WriteRemote, utils, "HdfSbufWriteRemoteService");
	SYM(ReadRemote, utils, "HdfSbufReadRemoteService");
	SYM(ReadU32, utils, "HdfSbufReadUint32");
	SYM(ReadI32, utils, "HdfSbufReadInt32");
	SYM(ReadI64, utils, "HdfSbufReadInt64");
	SYM(ReadU64, utils, "HdfSbufReadUint64");
	SYM(ReadStr, utils, "HdfSbufReadString");
	SYM(ReadUnpad, utils, "HdfSbufReadUnpadBuffer");
	if (need_svcmgr) {
		SYM(HdiMgrGet, hdi, "HDIServiceManagerGet");
		SYM(HdiMgrRel, hdi, "HDIServiceManagerRelease");
	}
#undef SYM
	return 0;
}
#endif

#if defined(GNSS_HDI_HDF_DRIVER) || defined(GNSS_HDI_HOST_TEST)
struct hdf_host_api {
	int (*SetObjDesc)(struct HdfDeviceObject *, const char *);
	bool (*CheckObjDesc)(struct HdfDeviceObject *, struct HdfSBuf *);
};

static struct hdf_host_api g_host_api;

#ifndef GNSS_HDI_HOST_TEST
static int load_hdf_host_api(void)
{
	if (g_host_api.SetObjDesc && g_host_api.CheckObjDesc)
		return 0;

	*(void **)(&g_host_api.SetObjDesc) = dlsym(RTLD_DEFAULT, "HdfDeviceObjectSetInterfaceDesc");
	*(void **)(&g_host_api.CheckObjDesc) = dlsym(RTLD_DEFAULT, "HdfDeviceObjectCheckInterfaceDesc");
	if (g_host_api.SetObjDesc && g_host_api.CheckObjDesc)
		return 0;

	void *host = dlopen("libhdf_host.z.so", RTLD_NOW | RTLD_GLOBAL);
	if (!host)
		host = dlopen("/system/lib64/chipset-sdk-sp/libhdf_host.z.so", RTLD_NOW | RTLD_GLOBAL);
	if (!host) {
		fprintf(stderr, "GNSS_HDF_HOST_DLOPEN_ERR err=%s\n", dlerror() ? dlerror() : "none");
		gnss_append_status_line("GNSS_HDF_HOST_DLOPEN_ERR");
		return -1;
	}

	*(void **)(&g_host_api.SetObjDesc) = dlsym(host, "HdfDeviceObjectSetInterfaceDesc");
	*(void **)(&g_host_api.CheckObjDesc) = dlsym(host, "HdfDeviceObjectCheckInterfaceDesc");
	if (!g_host_api.SetObjDesc || !g_host_api.CheckObjDesc) {
		gnss_append_status_line("GNSS_HDF_HOST_SYM_ERR");
		return -1;
	}
	return 0;
}
#endif

static int32_t gnss_hdf_dispatch(struct HdfDeviceIoClient *client, int cmdId,
				 struct HdfSBuf *data, struct HdfSBuf *reply)
{
	if (!client || !client->device || !data || !g_host_api.CheckObjDesc)
		return HDF_ERR_INVALID_PARAM;
	if (!g_host_api.CheckObjDesc(client->device, data))
		return HDF_ERR_INVALID_PARAM;
	return gnss_dispatch_common(&g_gnss, cmdId, data, reply);
}

static struct IDeviceIoService g_gnss_io_service = {
	.object = { .objectId = 0 },
	.Open = NULL,
	.Dispatch = gnss_hdf_dispatch,
	.Release = NULL,
};

static int32_t HdfGnssDriverBind(struct HdfDeviceObject *deviceObject)
{
	if (!deviceObject)
		return HDF_ERR_INVALID_PARAM;
#ifndef GNSS_HDI_HOST_TEST
	if (load_hdf_api(false) != 0 || load_hdf_host_api() != 0)
		return HDF_FAILURE;
#endif
	if (!g_host_api.SetObjDesc ||
	    g_host_api.SetObjDesc(deviceObject, GNSS_INTERFACE_DESC) != HDF_SUCCESS) {
#ifndef GNSS_HDI_HOST_TEST
		gnss_append_status_line("GNSS_HDF_DRIVER_BIND_DESC_ERR");
#endif
		return HDF_FAILURE;
	}
	deviceObject->service = &g_gnss_io_service;
#ifndef GNSS_HDI_HOST_TEST
	gnss_append_status_line("GNSS_HDF_DRIVER_BIND_OK");
#endif
	return HDF_SUCCESS;
}

static int32_t HdfGnssDriverInit(struct HdfDeviceObject *deviceObject)
{
	if (!deviceObject)
		return HDF_ERR_INVALID_PARAM;
#ifndef GNSS_HDI_HOST_TEST
	g_stop = 0;
	if (!g_gnss.loc_thread_started) {
		if (pthread_create(&g_gnss.loc_thread, NULL, qmi_loc_worker, NULL) != 0) {
			gnss_append_status_line("GNSS_HDF_DRIVER_INIT_THREAD_ERR");
			return HDF_FAILURE;
		}
		g_gnss.loc_thread_started = true;
	}
	gnss_append_status_line("GNSS_HDF_DRIVER_INIT_OK");
#endif
	return HDF_SUCCESS;
}

static void HdfGnssDriverRelease(struct HdfDeviceObject *deviceObject)
{
	(void)deviceObject;
	pthread_mutex_lock(&g_gnss.mu);
	bool was_started = g_gnss.started;
	g_gnss.started = false;
	g_gnss.enabled = false;
	g_gnss.reg_ok = false;
	struct HdfRemoteService *cb = g_gnss.callback;
	g_gnss.callback = NULL;
	pthread_mutex_unlock(&g_gnss.mu);
	if (was_started)
		send_loc_stop();
	if (cb && g_api.RecycleRemote)
		g_api.RecycleRemote(cb);
#ifndef GNSS_HDI_HOST_TEST
	g_stop = 1;
	if (g_gnss.loc_thread_started) {
		pthread_join(g_gnss.loc_thread, NULL);
		g_gnss.loc_thread_started = false;
	}
	gnss_append_status_line("GNSS_HDF_DRIVER_RELEASE");
#endif
}

#ifdef GNSS_HDI_HDF_DRIVER
static struct HdfDriverEntry g_marbleGnssDriverEntry = {
	.moduleVersion = 1,
	.moduleName = "libmarble_gnss_hdi_driver.z.so",
	.Bind = HdfGnssDriverBind,
	.Init = HdfGnssDriverInit,
	.Release = HdfGnssDriverRelease,
};

__attribute__((visibility("default")))
struct HdfDriverEntry *driverDesc = &g_marbleGnssDriverEntry;
#endif
#endif

#if !defined(GNSS_HDI_HOST_TEST) && !defined(GNSS_HDI_HDF_DRIVER)

static struct {
	pthread_mutex_t mu;
	struct HdfRemoteService *remote;
	uint32_t caps_cnt;
	uint32_t caps_val;
	uint32_t ws_cnt;
	uint32_t last_ws;
	uint32_t loc_cnt;
	uint32_t nmea_cnt;
} g_chk = { .mu = PTHREAD_MUTEX_INITIALIZER };

static int check_cb_dispatch(struct HdfRemoteService *target_arg, int code,
			     struct HdfSBuf *data, struct HdfSBuf *reply)
{
	(void)target_arg;
	(void)reply;
	if (!g_chk.remote || !data || !g_api.CheckToken(g_chk.remote, data))
		return HDF_ERR_INVALID_PARAM;

	pthread_mutex_lock(&g_chk.mu);
	if (code == GNSS_CB_REPORT_CAPABILITIES) {
		uint32_t c = 0;
		if (g_api.ReadU32(data, &c)) {
			g_chk.caps_cnt++;
			g_chk.caps_val = c;
		}
	} else if (code == GNSS_CB_REPORT_WORKING_STATUS) {
		uint64_t ws = 0;
		if (g_api.ReadU64(data, &ws)) {
			g_chk.ws_cnt++;
			g_chk.last_ws = (uint32_t)ws;
		}
	} else if (code == GNSS_CB_REPORT_LOCATION) {
		if (g_api.ReadUnpad(data, sizeof(struct LocationInfoV2_0)) != NULL)
			g_chk.loc_cnt++;
	} else if (code == GNSS_CB_REPORT_NMEA) {
		g_chk.nmea_cnt++;
	}
	pthread_mutex_unlock(&g_chk.mu);
	return HDF_SUCCESS;
}

static int run_check(void)
{
	if (load_hdf_api(true) != 0)
		return 1;
	struct HDIServiceManager *mgr = g_api.HdiMgrGet();
	if (!mgr) {
		printf("GNSS_HDI_CHECK err=no_hdimgr\n");
		return 1;
	}
	struct HdfRemoteService *svc = mgr->GetService(mgr, GNSS_SERVICE_NAME);
	g_api.HdiMgrRel(mgr);
	if (!svc || !svc->dispatcher || !svc->dispatcher->Dispatch) {
		printf("GNSS_HDI_CHECK err=no_service\n");
		return 1;
	}
	if (!g_api.SetDesc(svc, GNSS_INTERFACE_DESC)) {
		printf("GNSS_HDI_CHECK err=set_desc\n");
		return 1;
	}

	/* 1. GetVersion */
	uint32_t maj = 0, min = 0;
	struct HdfSBuf *d = g_api.SbufObtain(SBUF_IPC);
	struct HdfSBuf *r = g_api.SbufObtain(SBUF_IPC);
	int rc = -1;
	if (d && r && g_api.WriteToken(svc, d))
		rc = svc->dispatcher->Dispatch(svc, GNSS_CMD_GET_VERSION, d, r);
	if (rc == 0) {
		(void)g_api.ReadU32(r, &maj);
		(void)g_api.ReadU32(r, &min);
	}
	if (d) g_api.SbufRecycle(d);
	if (r) g_api.SbufRecycle(r);
	if (rc != 0 || maj != 2 || min != 0) {
		printf("GNSS_HDI_CHECK err=get_version rc=%d maj=%u min=%u\n", rc, maj, min);
		g_api.RecycleRemote(svc);
		return 1;
	}

	/* 2. Register IGnssCallback and verify Enable -> Start -> Stop -> Disable */
	static struct HdfRemoteDispatcher cb_disp = { .Dispatch = check_cb_dispatch };
	g_chk.remote = g_api.Obtain(NULL, &cb_disp);
	if (!g_chk.remote || !g_api.SetDesc(g_chk.remote, GNSS_CALLBACK_DESC)) {
		printf("GNSS_HDI_CHECK err=cb_obtain\n");
		g_api.RecycleRemote(svc);
		return 1;
	}

	d = g_api.SbufObtain(SBUF_IPC);
	r = g_api.SbufObtain(SBUF_IPC);
	int en_rc = -1;
	if (d && r && g_api.WriteToken(svc, d) && g_api.WriteRemote(d, g_chk.remote) == 0)
		en_rc = svc->dispatcher->Dispatch(svc, GNSS_CMD_ENABLE_GNSS, d, r);
	if (d) g_api.SbufRecycle(d);
	if (r) g_api.SbufRecycle(r);

	d = g_api.SbufObtain(SBUF_IPC);
	r = g_api.SbufObtain(SBUF_IPC);
	int st_rc = -1;
	if (d && r && g_api.WriteToken(svc, d) && g_api.WriteU64(d, 1u))
		st_rc = svc->dispatcher->Dispatch(svc, GNSS_CMD_START_GNSS, d, r);
	if (d) g_api.SbufRecycle(d);
	if (r) g_api.SbufRecycle(r);

	usleep(100000);

	d = g_api.SbufObtain(SBUF_IPC);
	r = g_api.SbufObtain(SBUF_IPC);
	int sp_rc = -1;
	if (d && r && g_api.WriteToken(svc, d) && g_api.WriteU64(d, 1u))
		sp_rc = svc->dispatcher->Dispatch(svc, GNSS_CMD_STOP_GNSS, d, r);
	if (d) g_api.SbufRecycle(d);
	if (r) g_api.SbufRecycle(r);

	d = g_api.SbufObtain(SBUF_IPC);
	r = g_api.SbufObtain(SBUF_IPC);
	int dis_rc = -1;
	if (d && r && g_api.WriteToken(svc, d))
		dis_rc = svc->dispatcher->Dispatch(svc, GNSS_CMD_DISABLE_GNSS, d, r);
	if (d) g_api.SbufRecycle(d);
	if (r) g_api.SbufRecycle(r);

	pthread_mutex_lock(&g_chk.mu);
	uint32_t caps_cnt = g_chk.caps_cnt, caps_val = g_chk.caps_val;
	uint32_t ws_cnt = g_chk.ws_cnt, last_ws = g_chk.last_ws;
	pthread_mutex_unlock(&g_chk.mu);

	g_api.RecycleRemote(g_chk.remote);
	g_api.RecycleRemote(svc);

	printf("GNSS_HDI_CHECK rc=%d:%d:%d:%d maj=%u min=%u cb_caps=%u(0x%x) cb_ws=%u(last=%u)\n",
	       en_rc, st_rc, sp_rc, dis_rc, maj, min, caps_cnt, caps_val, ws_cnt, last_ws);
	return (en_rc == 0 && st_rc == 0 && sp_rc == 0 && dis_rc == 0 &&
		caps_cnt >= 1 && ws_cnt >= 4) ? 0 : 1;
}

static int run_serve(void)
{
	if (load_hdf_api(false) != 0)
		return 1;

	static struct HdfRemoteDispatcher gnss_disp = { .Dispatch = gnss_stub_dispatch };
	g_gnss.remote = g_api.Obtain(&g_gnss.base, &gnss_disp);
	if (!g_gnss.remote || !g_api.SetDesc(g_gnss.remote, GNSS_INTERFACE_DESC)) {
		fprintf(stderr, "GNSS_HDI_ERR failed to allocate stub\n");
		return 1;
	}

	struct HdfRemoteService *registry = g_api.GetSa(DEVICE_SERVICE_MANAGER_SA_ID);
	if (!registry || !registry->dispatcher || !registry->dispatcher->Dispatch ||
	    !g_api.SetDesc(registry, DEVMGR_SERVICE_DESC)) {
		fprintf(stderr, "GNSS_HDI_ERR failed to reach SA 5100\n");
		return 1;
	}

	struct HdfSBuf *data = g_api.SbufObtain(SBUF_IPC);
	struct HdfSBuf *reply = g_api.SbufObtain(SBUF_IPC);
	int rc = -1;
	if (data && reply && g_api.WriteToken(registry, data) &&
	    g_api.WriteStr(data, GNSS_SERVICE_NAME) &&
	    g_api.WriteU16(data, DEVICE_CLASS_DEFAULT) &&
	    g_api.WriteU32(data, 0u) &&
	    g_api.WriteRemote(data, g_gnss.remote) == HDF_SUCCESS &&
	    g_api.WriteStr(data, "") &&
	    g_api.WriteStr(data, GNSS_INTERFACE_DESC)) {
		rc = registry->dispatcher->Dispatch(registry, DEVSVC_MANAGER_ADD_SERVICE, data, reply);
	}
	if (data) g_api.SbufRecycle(data);
	if (reply) g_api.SbufRecycle(reply);

	printf("GNSS_HDI_PUBLISH_RC=%d service=%s uid=%d pid=%d\n",
	       rc, GNSS_SERVICE_NAME, getuid(), getpid());
	{
		FILE *sf = fopen("/data/local/tmp/gnss-hdi/status.log", "w");
		if (sf) {
			fprintf(sf, "GNSS_HDI_PUBLISH_RC=%d service=%s uid=%d pid=%d\n",
				rc, GNSS_SERVICE_NAME, getuid(), getpid());
			fclose(sf);
		}
	}
	if (rc != HDF_SUCCESS)
		return 1;

	if (pthread_create(&g_gnss.loc_thread, NULL, qmi_loc_worker, NULL) == 0)
		g_gnss.loc_thread_started = true;

	while (!g_stop)
		sleep(1);

	data = g_api.SbufObtain(SBUF_IPC);
	reply = g_api.SbufObtain(SBUF_IPC);
	if (data && reply && g_api.WriteToken(registry, data) &&
	    g_api.WriteStr(data, GNSS_SERVICE_NAME)) {
		int urc = registry->dispatcher->Dispatch(registry, DEVSVC_MANAGER_REMOVE_SERVICE,
							 data, reply);
		printf("GNSS_HDI_UNPUBLISH_RC=%d\n", urc);
	}
	if (data) g_api.SbufRecycle(data);
	if (reply) g_api.SbufRecycle(reply);

	if (g_gnss.loc_thread_started)
		pthread_join(g_gnss.loc_thread, NULL);
	g_api.RecycleRemote(g_gnss.remote);
	g_api.RecycleRemote(registry);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	if (argc == 2 && strcmp(argv[1], "--serve") == 0)
		return run_serve();
	if (argc == 2 && strcmp(argv[1], "--check") == 0)
		return run_check();
	fprintf(stderr, "usage: %s [--serve|--check]\n", argv[0]);
	return 2;
}
#elif defined(GNSS_HDI_HOST_TEST)
/* ---- Host mock unit test harness (-DGNSS_HDI_HOST_TEST) ---- */
struct HdfSBuf {
	uint8_t buf[1024];
	size_t wpos;
	size_t rpos;
	struct HdfRemoteService *rem;
	bool has_token;
};

struct mock_remote_storage {
	struct HdfRemoteService svc;
	char desc[128];
};

static struct HdfSBuf *m_sbuf_obtain(enum HdfSbufType t)
{
	(void)t;
	return calloc(1, sizeof(struct HdfSBuf));
}
static void m_sbuf_recycle(struct HdfSBuf *s) { free(s); }
static bool m_write_token(struct HdfRemoteService *r, struct HdfSBuf *s)
{
	(void)r;
	s->has_token = true;
	return true;
}
static bool m_check_token(struct HdfRemoteService *r, struct HdfSBuf *s)
{
	return r != NULL && s != NULL && s->has_token;
}
static bool m_write_u32(struct HdfSBuf *s, uint32_t v)
{
	memcpy(s->buf + s->wpos, &v, 4);
	s->wpos += 4;
	return true;
}
static bool m_write_i32(struct HdfSBuf *s, int32_t v) { return m_write_u32(s, (uint32_t)v); }
static bool m_write_u64(struct HdfSBuf *s, uint64_t v)
{
	memcpy(s->buf + s->wpos, &v, 8);
	s->wpos += 8;
	return true;
}
static bool m_write_i64(struct HdfSBuf *s, int64_t v) { return m_write_u64(s, (uint64_t)v); }
static bool m_write_u16(struct HdfSBuf *s, uint16_t v)
{
	memcpy(s->buf + s->wpos, &v, 2);
	s->wpos += 2;
	return true;
}
static bool m_write_str(struct HdfSBuf *s, const char *str)
{
	size_t n = strlen(str) + 1;
	memcpy(s->buf + s->wpos, str, n);
	s->wpos += n;
	return true;
}
static bool m_write_unpad(struct HdfSBuf *s, const uint8_t *p, uint32_t n)
{
	memcpy(s->buf + s->wpos, p, n);
	s->wpos += n;
	return true;
}
static int m_write_remote(struct HdfSBuf *s, struct HdfRemoteService *r)
{
	s->rem = r;
	return 0;
}
static struct HdfRemoteService *m_read_remote(struct HdfSBuf *s) { return s->rem; }
static bool m_read_u32(struct HdfSBuf *s, uint32_t *v)
{
	if (s->rpos + 4 > s->wpos) return false;
	memcpy(v, s->buf + s->rpos, 4);
	s->rpos += 4;
	return true;
}
static bool m_read_i32(struct HdfSBuf *s, int32_t *v) { return m_read_u32(s, (uint32_t *)v); }
static bool m_read_u64(struct HdfSBuf *s, uint64_t *v)
{
	if (s->rpos + 8 > s->wpos) return false;
	memcpy(v, s->buf + s->rpos, 8);
	s->rpos += 8;
	return true;
}
static bool m_read_i64(struct HdfSBuf *s, int64_t *v) { return m_read_u64(s, (uint64_t *)v); }
static const char *m_read_str(struct HdfSBuf *s)
{
	const char *p = (const char *)(s->buf + s->rpos);
	s->rpos += strlen(p) + 1;
	return p;
}
static const uint8_t *m_read_unpad(struct HdfSBuf *s, uint32_t n)
{
	const uint8_t *p = s->buf + s->rpos;
	s->rpos += n;
	return p;
}
static bool m_set_desc(struct HdfRemoteService *r, const char *d)
{
	(void)r;
	return d != NULL && strlen(d) > 0;
}
static void m_recycle_rem(struct HdfRemoteService *r) { free(r); }

static uint32_t g_host_caps = 0;
static uint64_t g_host_ws[8];
static size_t g_host_ws_cnt = 0;
static struct LocationInfoV2_0 g_host_loc;
static size_t g_host_loc_cnt = 0;
static char g_host_nmea[128];
static size_t g_host_nmea_cnt = 0;

static int host_cb_dispatch(struct HdfRemoteService *self, int code,
			    struct HdfSBuf *data, struct HdfSBuf *reply)
{
	(void)reply;
	assert(m_check_token(self, data));
	if (code == GNSS_CB_REPORT_CAPABILITIES) {
		assert(m_read_u32(data, &g_host_caps));
	} else if (code == GNSS_CB_REPORT_WORKING_STATUS) {
		uint64_t ws = 0;
		assert(m_read_u64(data, &ws));
		g_host_ws[g_host_ws_cnt++] = ws;
	} else if (code == GNSS_CB_REPORT_LOCATION) {
		const uint8_t *p = m_read_unpad(data, sizeof(struct LocationInfoV2_0));
		memcpy(&g_host_loc, p, sizeof(g_host_loc));
		g_host_loc_cnt++;
	} else if (code == GNSS_CB_REPORT_NMEA) {
		int64_t ts = 0;
		int32_t nlen = 0;
		assert(m_read_i64(data, &ts));
		const char *s = m_read_str(data);
		assert(m_read_i32(data, &nlen));
		snprintf(g_host_nmea, sizeof(g_host_nmea), "%s", s);
		g_host_nmea_cnt++;
	}
	return HDF_SUCCESS;
}

static struct HdfDeviceObject *g_mock_dev_desc_bound = NULL;
static int m_set_obj_desc(struct HdfDeviceObject *dev, const char *desc)
{
	if (!dev || !desc || strcmp(desc, GNSS_INTERFACE_DESC) != 0)
		return HDF_ERR_INVALID_PARAM;
	g_mock_dev_desc_bound = dev;
	return HDF_SUCCESS;
}
static bool m_check_obj_desc(struct HdfDeviceObject *dev, struct HdfSBuf *s)
{
	return dev != NULL && dev == g_mock_dev_desc_bound && s != NULL && s->has_token;
}

int main(void)
{
	g_api.SbufObtain = m_sbuf_obtain;
	g_api.SbufRecycle = m_sbuf_recycle;
	g_api.WriteToken = m_write_token;
	g_api.CheckToken = m_check_token;
	g_api.WriteU32 = m_write_u32;
	g_api.WriteI32 = m_write_i32;
	g_api.WriteU64 = m_write_u64;
	g_api.WriteI64 = m_write_i64;
	g_api.WriteU16 = m_write_u16;
	g_api.WriteStr = m_write_str;
	g_api.WriteUnpad = m_write_unpad;
	g_api.WriteRemote = m_write_remote;
	g_api.ReadRemote = m_read_remote;
	g_api.ReadU32 = m_read_u32;
	g_api.ReadI32 = m_read_i32;
	g_api.ReadU64 = m_read_u64;
	g_api.ReadI64 = m_read_i64;
	g_api.ReadStr = m_read_str;
	g_api.ReadUnpad = m_read_unpad;
	g_api.SetDesc = m_set_desc;
	g_api.RecycleRemote = m_recycle_rem;

	struct HdfRemoteDispatcher gnss_disp = { .Dispatch = gnss_stub_dispatch };
	struct HdfRemoteService *gnss_rem = calloc(1, sizeof(*gnss_rem));
	gnss_rem->target = &g_gnss.base;
	gnss_rem->dispatcher = &gnss_disp;
	g_gnss.remote = gnss_rem;

	struct HdfRemoteDispatcher cb_disp = { .Dispatch = host_cb_dispatch };
	struct HdfRemoteService *cb_rem = calloc(1, sizeof(*cb_rem));
	cb_rem->dispatcher = &cb_disp;

	/* 1. GetVersion */
	struct HdfSBuf *d = m_sbuf_obtain(SBUF_IPC);
	struct HdfSBuf *r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	assert(gnss_stub_dispatch((struct HdfRemoteService *)&g_gnss.base, GNSS_CMD_GET_VERSION, d, r) == 0);
	uint32_t maj = 0, min = 0;
	assert(m_read_u32(r, &maj) && m_read_u32(r, &min) && maj == 2 && min == 0);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);

	/* 2. EnableGnss + StartGnss */
	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	assert(m_write_remote(d, cb_rem) == 0);
	assert(gnss_stub_dispatch((struct HdfRemoteService *)&g_gnss.base, GNSS_CMD_ENABLE_GNSS, d, r) == 0);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);
	assert(g_host_caps == 0x01u && g_host_ws_cnt == 1 && g_host_ws[0] == GNSS_WS_ENGINE_ON);

	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	m_write_u64(d, 1u);
	assert(gnss_stub_dispatch((struct HdfRemoteService *)&g_gnss.base, GNSS_CMD_START_GNSS, d, r) == 0);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);
	assert(g_host_ws_cnt == 2 && g_host_ws[1] == GNSS_WS_SESSION_BEGIN);

	/* 3. Synthetic QMI_LOC_POSITION_REPORT_IND + NMEA_IND + AFW/START/UTC builder check -> callback */
	uint8_t reg_tlv[32], start_tlv[32], nmea_cfg_tlv[8], utc_tlv[20];
	assert(build_loc_reg_events_tlv(reg_tlv, sizeof(reg_tlv), 0x0187ULL) == 29);
	assert(reg_tlv[0] == 0x01 && reg_tlv[11] == 0x10 && memcmp(reg_tlv + 14, "MHAL", 4) == 0);
	assert(reg_tlv[18] == 0x11 && get_le32(reg_tlv + 21) == 1u && reg_tlv[25] == 0x12 && reg_tlv[28] == 0);
	assert(build_loc_start_tlv(start_tlv, sizeof(start_tlv)) == 25);
	assert(start_tlv[0] == 0x01 && start_tlv[4] == 0x10 && start_tlv[11] == 0x12 && start_tlv[18] == 0x13);
	assert(build_loc_set_nmea_types_tlv(nmea_cfg_tlv, sizeof(nmea_cfg_tlv), 0x0000000fu) == 7);
	assert(nmea_cfg_tlv[0] == 0x01 && get_le32(nmea_cfg_tlv + 3) == 0x0000000fu);
	assert(build_loc_inject_utc_time_tlv(utc_tlv, sizeof(utc_tlv), 1600000000000ULL, 5000u) == 0);
	assert(build_loc_inject_utc_time_tlv(utc_tlv, sizeof(utc_tlv), 1795600000000ULL, 5000u) == 18);
	assert(utc_tlv[0] == 0x01 && get_le16(utc_tlv + 1) == 8 && get_le64(utc_tlv + 3) == 1795600000000ULL);
	assert(utc_tlv[11] == 0x02 && get_le16(utc_tlv + 12) == 4 && get_le32(utc_tlv + 14) == 5000u);

	uint8_t pos_tlvs[128];
	size_t p = 0;
	pos_tlvs[p++] = 0x01; put_le16(pos_tlvs + p, 4); p += 2; put_le32(pos_tlvs + p, 1u); p += 4;
	double lat = 31.2304, lon = 121.4737;
	uint64_t lat_u = 0, lon_u = 0;
	memcpy(&lat_u, &lat, 8);
	memcpy(&lon_u, &lon, 8);
	pos_tlvs[p++] = 0x10; put_le16(pos_tlvs + p, 8); p += 2; put_le64(pos_tlvs + p, lat_u); p += 8;
	pos_tlvs[p++] = 0x11; put_le16(pos_tlvs + p, 8); p += 2; put_le64(pos_tlvs + p, lon_u); p += 8;
	float alt_f = 25.5f, spd_f = 1.5f, sacc_f = 0.3f, vacc_f = 4.0f, brg_f = 90.0f, bacc_f = 2.0f;
	uint32_t u32 = 0;
	memcpy(&u32, &spd_f, 4); pos_tlvs[p++] = 0x18; put_le16(pos_tlvs + p, 4); p += 2; put_le32(pos_tlvs + p, u32); p += 4;
	memcpy(&u32, &sacc_f, 4); pos_tlvs[p++] = 0x19; put_le16(pos_tlvs + p, 4); p += 2; put_le32(pos_tlvs + p, u32); p += 4;
	memcpy(&u32, &alt_f, 4); pos_tlvs[p++] = 0x1a; put_le16(pos_tlvs + p, 4); p += 2; put_le32(pos_tlvs + p, u32); p += 4;
	memcpy(&u32, &vacc_f, 4); pos_tlvs[p++] = 0x1c; put_le16(pos_tlvs + p, 4); p += 2; put_le32(pos_tlvs + p, u32); p += 4;
	memcpy(&u32, &brg_f, 4); pos_tlvs[p++] = 0x20; put_le16(pos_tlvs + p, 4); p += 2; put_le32(pos_tlvs + p, u32); p += 4;
	memcpy(&u32, &bacc_f, 4); pos_tlvs[p++] = 0x21; put_le16(pos_tlvs + p, 4); p += 2; put_le32(pos_tlvs + p, u32); p += 4;
	pos_tlvs[p++] = 0x25; put_le16(pos_tlvs + p, 8); p += 2; put_le64(pos_tlvs + p, 1700000000123ULL); p += 8;
	pos_tlvs[p++] = 0x23; put_le16(pos_tlvs + p, 4); p += 2; put_le32(pos_tlvs + p, 0x01u); p += 4; /* SATELLITE */
	struct LocationInfoV2_0 loc;
	uint32_t t_ss = 0, t_tech = 0;
	float t_hacc = -1.0f;
	assert(parse_qmi_loc_position_report(pos_tlvs, p, &loc, &t_ss, &t_tech, &t_hacc));
	assert(t_ss == 1u && t_tech == 0x01u);
	assert(dispatch_cb_location(cb_rem, &loc) == 0);
	assert(g_host_loc_cnt == 1 && fabs(g_host_loc.latitude - 31.2304) < 1e-6 &&
	       fabs(g_host_loc.longitude - 121.4737) < 1e-6 &&
	       fabs(g_host_loc.altitude - 25.5) < 1e-4 &&
	       fabs(g_host_loc.speed - 1.5f) < 1e-4 &&
	       fabs(g_host_loc.verticalAccuracy - 4.0f) < 1e-4 &&
	       fabs(g_host_loc.bearing - 90.0f) < 1e-4 &&
	       g_host_loc.timeForFix == 1700000000123LL);

	/* 3b. Reject CELLID-only (tech=0x2), null-island (0,0), and coarse IN_PROGRESS hacc>250m */
	{
		uint8_t bad_cell[128];
		memcpy(bad_cell, pos_tlvs, p);
		put_le32(bad_cell + p - 4, 0x02u); /* CELLID only, no SATELLITE bit */
		assert(!parse_qmi_loc_position_report(bad_cell, p, &loc, &t_ss, &t_tech, &t_hacc));
		assert(t_tech == 0x02u);

		uint8_t bad_zero[32];
		size_t zp = 0;
		bad_zero[zp++] = 0x01; put_le16(bad_zero + zp, 4); zp += 2; put_le32(bad_zero + zp, 0u); zp += 4;
		bad_zero[zp++] = 0x10; put_le16(bad_zero + zp, 8); zp += 2; put_le64(bad_zero + zp, 0ULL); zp += 8;
		bad_zero[zp++] = 0x11; put_le16(bad_zero + zp, 8); zp += 2; put_le64(bad_zero + zp, 0ULL); zp += 8;
		assert(!parse_qmi_loc_position_report(bad_zero, zp, &loc, &t_ss, &t_tech, &t_hacc));

		uint8_t bad_hacc[140];
		memcpy(bad_hacc, pos_tlvs, p);
		float big_hacc = 1500.0f;
		uint32_t big_u = 0;
		memcpy(&big_u, &big_hacc, 4);
		size_t hp = p;
		bad_hacc[hp++] = 0x12; put_le16(bad_hacc + hp, 4); hp += 2; put_le32(bad_hacc + hp, big_u); hp += 4;
		assert(!parse_qmi_loc_position_report(bad_hacc, hp, &loc, &t_ss, &t_tech, &t_hacc));
	}

	const char *sample_nmea = "$GPGGA,000000.00,,,,,0,00,,,M,,M,,*48\r\n";
	uint8_t nmea_tlvs[64];
	nmea_tlvs[0] = 0x01;
	put_le16(nmea_tlvs + 1, (uint16_t)strlen(sample_nmea));
	memcpy(nmea_tlvs + 3, sample_nmea, strlen(sample_nmea));
	char parsed_nmea[128];
	int32_t plen = 0;
	assert(parse_qmi_loc_nmea(nmea_tlvs, 3 + strlen(sample_nmea), parsed_nmea, sizeof(parsed_nmea), &plen));
	assert(dispatch_cb_nmea(cb_rem, 123456LL, parsed_nmea, plen) == 0);
	assert(g_host_nmea_cnt == 1 && strncmp(g_host_nmea, "$GPGGA", 6) == 0);

	uint8_t eng_tlvs[7] = { 0x01, 4, 0, 1, 0, 0, 0 };
	uint64_t eng_ws = 0;
	assert(parse_qmi_loc_engine_state(eng_tlvs, sizeof(eng_tlvs), &eng_ws) && eng_ws == GNSS_WS_ENGINE_ON);
	put_le32(eng_tlvs + 3, 2u);
	assert(parse_qmi_loc_engine_state(eng_tlvs, sizeof(eng_tlvs), &eng_ws) && eng_ws == GNSS_WS_ENGINE_OFF);

	/* 4. Stop + Disable */
	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	m_write_u64(d, 1u);
	assert(gnss_stub_dispatch((struct HdfRemoteService *)&g_gnss.base, GNSS_CMD_STOP_GNSS, d, r) == 0);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);

	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	assert(gnss_stub_dispatch((struct HdfRemoteService *)&g_gnss.base, GNSS_CMD_DISABLE_GNSS, d, r) == 0);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);
	assert(g_host_ws_cnt == 4 && g_host_ws[2] == GNSS_WS_SESSION_END && g_host_ws[3] == GNSS_WS_ENGINE_OFF);

	/* 5. Native HDF driver Bind -> Dispatch -> bad token -> Release test */
	g_host_api.SetObjDesc = m_set_obj_desc;
	g_host_api.CheckObjDesc = m_check_obj_desc;
	struct HdfDeviceObject mock_dev = { 0 };
	assert(HdfGnssDriverBind(&mock_dev) == HDF_SUCCESS);
	assert(mock_dev.service != NULL && mock_dev.service->Dispatch != NULL);
	assert(HdfGnssDriverInit(&mock_dev) == HDF_SUCCESS);

	struct HdfDeviceIoClient mock_client = { .device = &mock_dev, .priv = NULL };
	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	assert(mock_dev.service->Dispatch(&mock_client, GNSS_CMD_GET_VERSION, d, r) == HDF_ERR_INVALID_PARAM);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);

	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	assert(mock_dev.service->Dispatch(&mock_client, GNSS_CMD_GET_VERSION, d, r) == HDF_SUCCESS);
	maj = 0; min = 0;
	assert(m_read_u32(r, &maj) && m_read_u32(r, &min) && maj == 2 && min == 0);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);

	struct HdfRemoteService *cb_rem2 = calloc(1, sizeof(*cb_rem2));
	cb_rem2->dispatcher = &cb_disp;
	g_host_caps = 0;
	g_host_ws_cnt = 0;
	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	assert(m_write_remote(d, cb_rem2) == 0);
	assert(mock_dev.service->Dispatch(&mock_client, GNSS_CMD_ENABLE_GNSS, d, r) == HDF_SUCCESS);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);
	assert(g_host_caps == 0x01u && g_host_ws_cnt == 1 && g_host_ws[0] == GNSS_WS_ENGINE_ON);

	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	m_write_u64(d, 1u);
	assert(mock_dev.service->Dispatch(&mock_client, GNSS_CMD_START_GNSS, d, r) == HDF_SUCCESS);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);
	assert(g_host_ws_cnt == 2 && g_host_ws[1] == GNSS_WS_SESSION_BEGIN);

	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	m_write_u64(d, 1u);
	assert(mock_dev.service->Dispatch(&mock_client, GNSS_CMD_STOP_GNSS, d, r) == HDF_SUCCESS);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);

	d = m_sbuf_obtain(SBUF_IPC);
	r = m_sbuf_obtain(SBUF_IPC);
	m_write_token(gnss_rem, d);
	assert(mock_dev.service->Dispatch(&mock_client, GNSS_CMD_DISABLE_GNSS, d, r) == HDF_SUCCESS);
	m_sbuf_recycle(d);
	m_sbuf_recycle(r);
	assert(g_host_ws_cnt == 4 && g_host_ws[2] == GNSS_WS_SESSION_END && g_host_ws[3] == GNSS_WS_ENGINE_OFF);
	HdfGnssDriverRelease(&mock_dev);

	free(gnss_rem);
	puts("GNSS_HDI_HOST_TEST_OK=1");
	return 0;
}
#endif
