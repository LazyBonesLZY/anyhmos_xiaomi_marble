/* SPDX-License-Identifier: MIT
 * marble DCM software-TEE replacement: a clean-room SA 65866 provider.
 *
 * Wire contract reverse engineered from the donor's
 * libdevice_cert_mgr_sdk.z.so / libdevice_cert_mgr_service.z.so:
 *
 *   service descriptor : "ohos.security.devicecertmanager.service" (UTF-16)
 *   code 3 (device token):
 *     data     = interface token + callback IRemoteObject
 *     reply    = int32 errCode + uint64 requestId
 *     callback = code 1, interface token
 *                "ohos.security.devicecertmanager.dcmDeviceTokCallback"
 *                + uint64 requestId + int32 errCode + string token
 *   all other codes (1,2,4..13,23):
 *     reply    = int32 errCode (non-zero; the SDK reads no further data)
 *
 * The donor service registers 14 handler codes in its constructor. Only code 3
 * is implemented locally; the rest return the donor's "not supported" family
 * code instead of an IPC-level failure so callers get a defined answer.
 *
 * The token is derived from this device's own values (serial/model/UDID) with
 * a per-device HMAC key. It is a local stand-in for the Huawei cloud token and
 * is explicitly not a Huawei-issued attestation; see
 * docs/MARBLE_DCM_SOFT_TEE.md.
 */
#include "dcm_local_ca.h"
#include "dcm_token.h"

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
#include <fcntl.h>
#include <sys/stat.h>

#include "ipc_skeleton.h"
#include "iremote_object.h"
#include "message_option.h"
#include "message_parcel.h"

namespace {

constexpr int32_t kDcmSaId = 65866;
constexpr uint32_t kGetDeviceTokenCode = 3;
constexpr uint32_t kDeviceTokenCallbackCode = 1;

/* The donor uses -10009 for "not supported" (DcmAttestIdsCommon). */
constexpr int32_t kErrNotSupported = -10009;

const char16_t *const kServiceDescriptor = u"ohos.security.devicecertmanager.service";
const char16_t *const kCallbackDescriptor =
    u"ohos.security.devicecertmanager.dcmDeviceTokCallback";
const char16_t *const kAnonCallbackDescriptor =
    u"ohos.security.devicecertmanager.dcmAnonymousCallback";

constexpr size_t kStubObjectBytes = 4096;
constexpr size_t kVtableEntries = 128;
constexpr size_t kPrimaryPrefix = 4;  // offset-to-top + typeinfo occupy two slots each
constexpr size_t kDispatchSlot = 200 / 8;

using StubCtor = void (*)(void *, std::u16string, bool);
using StubDispatch = int (*)(void *, uint32_t, OHOS::MessageParcel &, OHOS::MessageParcel &,
                             OHOS::MessageOption &);
using SetThreads = bool (*)(int);
using JoinThread = void (*)();
using GetInstance = void *(*)();
using GetManager = void *(*)(void *);
/* Returns 0 on success (donor returns int, not bool). */
using AddAbility = int (*)(void *, int32_t, const void *, const void *);
using GetCallingUidFn = uint32_t (*)();
/* Parcel::WriteUnpadBuffer lives in libutils, not in the SDK message_parcel.h. */
using WriteUnpadBufferFn = bool (*)(void *, const void *, size_t);

/* code -> donor handler name, from DeviceCertManagerService::DeviceCertManagerService()
 * in libdevice_cert_mgr_service.z.so (map node key + handler pair). */
struct CodeEntry {
    uint32_t code;
    const char *name;
};

constexpr CodeEntry kKnownCodes[] = {
    {1, "AttestIds"},
    {2, "AnonymousAttest"},
    {3, "DeviceToken"},
    {4, "AttestSecInfo"},
    {5, "AttestIdsCompat"},
    {6, "CertUpdate"},
    {7, "GetCertState"},
    {8, "CertUpdateNoCallback"},
    {9, "AttestIdsWithCertType"},
    {10, "GetProvisionedInfoStatus"},
    {11, "ApplyAnonymousAttest"},
    {12, "LocalApplyAnonymousAttest"},
    {13, "AnonymousAttestSe"},
    {23, "ApplyHctcAttest"},
};

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
    GetCallingUidFn getCallingUid = nullptr;
};

/* AArch64 sptr<ISystemAbilityManager> indirect return carrier. The non-trivial
 * destructor keeps the compiler on the donor's sret ABI instead of returning
 * the 8-byte POD in x0. */
struct SptrCarrier {
    void *ptr;
    ~SptrCarrier() {}
};

/* Layout matches the donor's ISystemAbilityManager::SAExtraProp. */
struct SAExtraProp {
    bool isDistributed;
    uint32_t dumpLevel;
    std::u16string capability;
    std::u16string permission;
};
static_assert(sizeof(SAExtraProp) == 56, "SAExtraProp must match the donor ABI");

std::mutex gLogMutex;
FILE *gLog = nullptr;
std::string gSerial;
std::string gModel;
std::string gUdid;

void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void Log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(gLogMutex);
    std::fprintf(stderr, "HMOS_DCM %s\n", line);
    if (gLog != nullptr) {
        std::fprintf(gLog, "HMOS_DCM %s\n", line);
        std::fflush(gLog);
    }
}

void OpenLog()
{
    static const char *const paths[] = {
        "/data/service/el1/public/device_cert_manager_server/hmos-dcm-service.log",
        "/data/local/tmp/hmos-dcm-service.log",
    };
    for (const char *path : paths) {
        gLog = std::fopen(path, "a");
        if (gLog != nullptr) {
            return;
        }
    }
}

std::string ReadFileText(const char *path, size_t cap = 4096)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::string();
    }
    std::string data;
    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        data.append(buf, static_cast<size_t>(n));
        if (data.size() >= cap) {
            break;
        }
    }
    close(fd);
    return data;
}

/* /proc/bootconfig uses `key = "value"`; /proc/cmdline uses `key=value`. */
std::string ExtractBootValue(const char *key)
{
    std::string text = ReadFileText("/proc/bootconfig");
    std::string needle = std::string("\n") + key + " = \"";
    size_t pos = text.find(needle);
    if (pos != std::string::npos) {
        size_t start = pos + needle.size();
        size_t end = text.find('"', start);
        if (end != std::string::npos) {
            return text.substr(start, end - start);
        }
    }
    if (text.rfind(std::string(key) + " = \"", 0) == 0) {
        size_t start = std::strlen(key) + 4;
        size_t end = text.find('"', start);
        if (end != std::string::npos) {
            return text.substr(start, end - start);
        }
    }
    text = ReadFileText("/proc/cmdline");
    needle = std::string(key) + "=";
    pos = text.find(needle);
    if (pos != std::string::npos &&
        (pos == 0 || text[pos - 1] == ' ' || text[pos - 1] == '\n')) {
        size_t start = pos + needle.size();
        size_t end = text.find_first_of(" \n", start);
        if (end == std::string::npos) {
            end = text.size();
        }
        return text.substr(start, end - start);
    }
    return std::string();
}

bool LoadFactsFile()
{
    std::string text = ReadFileText("/compat/etc/dcm-device.json");
    if (text.empty()) {
        return false;
    }
    auto field = [&text](const char *key) -> std::string {
        std::string needle = std::string("\"") + key + "\":\"";
        size_t pos = text.find(needle);
        if (pos == std::string::npos) {
            return std::string();
        }
        pos += needle.size();
        size_t end = text.find('"', pos);
        if (end == std::string::npos) {
            return std::string();
        }
        return text.substr(pos, end - pos);
    };
    gSerial = field("sn");
    gModel = field("model");
    gUdid = field("udid");
    return !gSerial.empty() || !gModel.empty();
}

std::string ReadModel()
{
    std::string model = ReadFileText("/proc/device-tree/model");
    while (!model.empty() && (model.back() == '\n' || model.back() == '\0' || model.back() == ' ')) {
        model.pop_back();
    }
    if (model.empty()) {
        model = ExtractBootValue("androidboot.hardware.sku");
    }
    if (model.empty()) {
        model = "marble";
    }
    return model;
}

/* The stage-generated file may appear after this process starts (services job
 * ordering); load lazily on first request instead of only once at boot. */
void EnsureFacts()
{
    if (!gSerial.empty() || !gModel.empty()) {
        return;
    }
    if (!LoadFactsFile()) {
        gSerial = ExtractBootValue("androidboot.serialno");
        if (gSerial.empty()) {
            gSerial = ExtractBootValue("ro.serialno");
        }
        gModel = ReadModel();
        gUdid = ExtractBootValue("ohos.boot.udid");
    }
    Log("DEVICE model=%s sn_len=%zu udid_len=%zu", gModel.c_str(), gSerial.size(), gUdid.size());
}

std::atomic<uint64_t> gRequestCounter{0};
GetCallingUidFn gGetCallingUid = nullptr;
WriteUnpadBufferFn gWriteUnpadBuffer = nullptr;

uint64_t NextRequestId()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t v = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                 static_cast<uint64_t>(ts.tv_nsec);
    v ^= (static_cast<uint64_t>(getpid()) << 40);
    v += gRequestCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    return v == 0 ? 1 : v;
}

uint32_t CallingUid()
{
    if (gGetCallingUid != nullptr) {
        return gGetCallingUid();
    }
    return static_cast<uint32_t>(getuid());
}

std::string BuildToken(uint64_t requestId)
{
    int64_t now = static_cast<int64_t>(time(nullptr));
    char token[1024];
    if (dcm_build_token(gSerial.c_str(), gModel.c_str(), gUdid.c_str(), requestId, now,
                        token, sizeof(token)) != 0) {
        return std::string();
    }
    return std::string(token);
}

const char *CodeName(uint32_t code)
{
    for (const auto &entry : kKnownCodes) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "Unknown";
}

int DispatchDeviceToken(OHOS::MessageParcel &data, OHOS::MessageParcel &reply)
{
    std::u16string descriptor = data.ReadInterfaceToken();
    if (descriptor != std::u16string(kServiceDescriptor)) {
        Log("REJECT bad descriptor");
        return -1;
    }
    OHOS::sptr<OHOS::IRemoteObject> callback = data.ReadRemoteObject();
    if (callback == nullptr) {
        Log("REJECT no callback object");
        return -1;
    }
    EnsureFacts();
    uint64_t requestId = NextRequestId();
    std::string token = BuildToken(requestId);
    if (token.empty()) {
        Log("REJECT token build failed");
        return -1;
    }
    if (!reply.WriteInt32(0) || !reply.WriteUint64(requestId)) {
        Log("REJECT reply write failed");
        return -1;
    }

    OHOS::MessageParcel cbData;
    OHOS::MessageParcel cbReply;
    OHOS::MessageOption cbOption(1 /* TF_ASYNC */, 8);
    bool written = cbData.WriteInterfaceToken(std::u16string(kCallbackDescriptor)) &&
                   cbData.WriteUint64(requestId) && cbData.WriteInt32(0) &&
                   cbData.WriteString(token);
    int rc = written ? callback->SendRequest(kDeviceTokenCallbackCode, cbData, cbReply, cbOption)
                     : -1;

    char digest[17] = {0};
    dcm_token_digest(token.c_str(), digest);
    Log("TOKEN_ISSUED caller_uid=%u request_id=%016llx len=%zu digest=%s cb_rc=%d",
        CallingUid(), static_cast<unsigned long long>(requestId), token.size(), digest, rc);
    return 0;
}

struct AnonCallbackJob {
    OHOS::sptr<OHOS::IRemoteObject> callback;
    uint64_t requestId;
    unsigned char *dev;
    size_t dev_len;
    unsigned char *root;
    size_t root_len;
};

void *AnonCallbackThread(void *arg)
{
    auto *job = static_cast<AnonCallbackJob *>(arg);
    /* DcmGenerateCertChain() registers the requestId *after* the binder reply
     * returns. A callback sent before that registration is dropped by
     * HksDcmCallback (GetProxyWithoutLock fails). Delay just past the return. */
    usleep(200 * 1000);
    OHOS::MessageParcel cbData;
    OHOS::MessageParcel cbReply;
    OHOS::MessageOption cbOption(1 /* TF_ASYNC */, 8);
    bool written = cbData.WriteInterfaceToken(std::u16string(kAnonCallbackDescriptor)) &&
                   cbData.WriteUint64(job->requestId) && cbData.WriteInt32(0) &&
                   cbData.WriteString(std::string());
    if (written) {
        written = cbData.WriteUint32(2) &&
                  cbData.WriteUint32(static_cast<uint32_t>(job->dev_len)) &&
                  gWriteUnpadBuffer(&cbData, job->dev, job->dev_len) &&
                  cbData.WriteUint32(static_cast<uint32_t>(job->root_len)) &&
                  gWriteUnpadBuffer(&cbData, job->root, job->root_len);
    }
    int rc = written ? job->callback->SendRequest(1, cbData, cbReply, cbOption) : -1;
    Log("ANON_ATTEST delayed_cb request_id=%016llx dev=%zu root=%zu rc=%d",
        static_cast<unsigned long long>(job->requestId), job->dev_len, job->root_len, rc);
    dcm_ca_free(job->dev);
    dcm_ca_free(job->root);
    delete job;
    return nullptr;
}

int DispatchAnonymousAttest(OHOS::MessageParcel &data, OHOS::MessageParcel &reply)
{
    std::u16string descriptor = data.ReadInterfaceToken();
    if (descriptor != std::u16string(kServiceDescriptor)) {
        Log("REJECT bad descriptor code=anon_attest");
        return -1;
    }
    OHOS::sptr<OHOS::IRemoteObject> callback = data.ReadRemoteObject();
    if (callback == nullptr) {
        Log("REJECT no callback object code=anon_attest");
        return -1;
    }
    uint32_t in_size = 0;
    if (!data.ReadUint32(in_size)) {
        Log("REJECT bad in-blob size code=anon_attest");
        return -1;
    }
    if (in_size != 0 && data.ReadBuffer(in_size) == nullptr) {
        Log("REJECT bad in-blob code=anon_attest");
        return -1;
    }
    EnsureFacts();
    unsigned char *dev = nullptr;
    unsigned char *root = nullptr;
    size_t dev_len = 0;
    size_t root_len = 0;
    int rc_dev = dcm_ca_cert_der(0, &dev, &dev_len);
    int rc_root = dcm_ca_cert_der(1, &root, &root_len);
    if (rc_dev != 0 || rc_root != 0 || gWriteUnpadBuffer == nullptr) {
        Log("ANON_ATTEST local chain unavailable dev=%d root=%d", rc_dev, rc_root);
        bool ok = reply.WriteInt32(kErrNotSupported) && reply.WriteUint64(0);
        dcm_ca_free(dev);
        dcm_ca_free(root);
        return ok ? 0 : -1;
    }
    uint64_t requestId = NextRequestId();
    if (!reply.WriteInt32(0) || !reply.WriteUint64(requestId)) {
        Log("ANON_ATTEST reply write failed");
        dcm_ca_free(dev);
        dcm_ca_free(root);
        return -1;
    }
    auto *job = new (std::nothrow) AnonCallbackJob{callback, requestId, dev, dev_len, root,
                                                    root_len};
    if (job == nullptr) {
        dcm_ca_free(dev);
        dcm_ca_free(root);
        return -1;
    }
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int prc = pthread_create(&tid, &attr, &AnonCallbackThread, job);
    pthread_attr_destroy(&attr);
    if (prc != 0) {
        Log("ANON_ATTEST thread create failed rc=%d", prc);
        dcm_ca_free(dev);
        dcm_ca_free(root);
        delete job;
        return -1;
    }
    Log("ANON_ATTEST queued dev=%zu root=%zu caller_uid=%u", dev_len, root_len, CallingUid());
    return 0;
}

int DispatchAttestIds(OHOS::MessageParcel &data, OHOS::MessageParcel &reply)
{
    std::u16string descriptor = data.ReadInterfaceToken();
    if (descriptor != std::u16string(kServiceDescriptor)) {
        Log("REJECT bad descriptor code=attest_ids");
        return -1;
    }
    EnsureFacts();
    unsigned char *blob = nullptr;
    size_t blob_len = 0;
    int rc = dcm_ca_cert_chain_blob(&blob, &blob_len);
    if (rc != 0 || blob == nullptr) {
        Log("ATTEST_IDS fail rc=%d", rc);
        bool ok = reply.WriteInt32(kErrNotSupported) && reply.WriteUint32(0);
        dcm_ca_free(blob);
        return ok ? 0 : -1;
    }
    bool ok = reply.WriteInt32(0) && reply.WriteUint32(static_cast<uint32_t>(blob_len)) &&
              reply.WriteBuffer(blob, blob_len);
    Log("ATTEST_IDS local_chain len=%zu caller_uid=%u ok=%d", blob_len, CallingUid(),
        ok ? 1 : 0);
    dcm_ca_free(blob);
    return ok ? 0 : -1;
}

int DispatchCertState(OHOS::MessageParcel &data, OHOS::MessageParcel &reply)
{
    std::u16string descriptor = data.ReadInterfaceToken();
    if (descriptor != std::u16string(kServiceDescriptor)) {
        Log("REJECT bad descriptor code=cert_state");
        return -1;
    }
    EnsureFacts();
    int state = dcm_ca_state();
    bool ok = reply.WriteInt32(0) && reply.WriteUint32(4) && reply.WriteInt32(state);
    Log("CERT_STATE local=%d caller_uid=%u ok=%d", state, CallingUid(), ok ? 1 : 0);
    return ok ? 0 : -1;
}

int Dispatch(void *self, uint32_t code, OHOS::MessageParcel &data, OHOS::MessageParcel &reply,
             OHOS::MessageOption &option)
{
    (void)self;
    (void)option;
    if (code == kGetDeviceTokenCode) {
        return DispatchDeviceToken(data, reply);
    }
    /* Attest ids (1/5/9): serve the local certificate chain in the donor's
     * DcmClientUnpackCertChainFromService blob format. Cert state (7/10):
     * serve the local provisioned state. Both are local-only. */
    if (code == 2) {
        return DispatchAnonymousAttest(data, reply);
    }
    if (code == 1 || code == 5 || code == 9) {
        return DispatchAttestIds(data, reply);
    }
    if (code == 7 || code == 10) {
        return DispatchCertState(data, reply);
    }
    /* Certificate issuance/update (2/6/8/11/12/13/23) and attest sec-info (4)
     * still have no local backend. Answer at the business layer with the
     * donor's "not supported" code and leave the parcel otherwise empty; the
     * SDK's common SendRequest reads only int32 when it is non-zero. */
    bool ok = reply.WriteInt32(kErrNotSupported);
    Log("UNSUPPORTED code=%u name=%s caller_uid=%u reply_written=%d", code, CodeName(code),
        CallingUid(), ok ? 1 : 0);
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
    api.getCallingUid = reinterpret_cast<GetCallingUidFn>(
        dlsym(lib, "_ZN4OHOS11IPCSkeleton13GetCallingUidEv"));
    gWriteUnpadBuffer = reinterpret_cast<WriteUnpadBufferFn>(
        dlsym(RTLD_DEFAULT, "_ZN4OHOS6Parcel16WriteUnpadBufferEPKvm"));
    if (api.ctor == nullptr || api.originalDispatch == nullptr || api.setThreads == nullptr ||
        api.joinThread == nullptr) {
        Log("FAIL resolve IPCObjectStub/IPCSkeleton symbols");
    }
    return api;
}

void *BuildStub(const IpcApi &api)
{
    if (api.ctor == nullptr || api.originalDispatch == nullptr) {
        return nullptr;
    }
    void *object = ::operator new(kStubObjectBytes);
    std::memset(object, 0, kStubObjectBytes);
    api.ctor(object, std::u16string(kServiceDescriptor), false);

    void **table = *reinterpret_cast<void ***>(object);
    if (table == nullptr || table[kDispatchSlot] != reinterpret_cast<void *>(api.originalDispatch)) {
        Log("FAIL donor stub vtable guard");
        return nullptr;
    }
    alignas(16) static uintptr_t privateTable[kVtableEntries];
    std::memcpy(privateTable, reinterpret_cast<const char *>(table) - kPrimaryPrefix * sizeof(void *),
                sizeof(privateTable));
    privateTable[kPrimaryPrefix + kDispatchSlot] = reinterpret_cast<uintptr_t>(&Dispatch);
    *reinterpret_cast<void ***>(object) =
        reinterpret_cast<void **>(privateTable + kPrimaryPrefix);
    Log("STUB_READY descriptor=ohos.security.devicecertmanager.service slot=%zu", kDispatchSlot);
    return object;
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
        if (api.lib != nullptr) {
            break;
        }
    }
    if (api.lib == nullptr) {
        Log("FAIL dlopen libsamgr_proxy: %s", dlerror());
        return api;
    }
    api.instance = reinterpret_cast<GetInstance>(
        dlsym(api.lib, "_ZN4OHOS26SystemAbilityManagerClient11GetInstanceEv"));
    api.manager = reinterpret_cast<GetManager>(dlsym(
        api.lib, "_ZN4OHOS26SystemAbilityManagerClient23GetSystemAbilityManagerEv"));
    /* GetSystemAbilityManager() returns sptr<ISystemAbilityManager>, pointing
     * to the +112 subobject inside SystemAbilityManagerProxy; use the -112 thunk. */
    api.addSystemAbility = reinterpret_cast<AddAbility>(dlsym(
        api.lib,
        "_ZThn112_N4OHOS25SystemAbilityManagerProxy16AddSystemAbilityEiRKNS_4sptrINS_13IRemoteObjectEEERKNS_21ISystemAbilityManager11SAExtraPropE"));
    if (api.instance == nullptr || api.manager == nullptr || api.addSystemAbility == nullptr) {
        Log("FAIL resolve SystemAbilityManager symbols");
    }
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
    int rc = api.addSystemAbility(manager.ptr, kDcmSaId, &stub, &extra);
    Log("ADD_SA id=%d rc=%d", kDcmSaId, rc);
    return rc == 0;
}

}  // namespace

int main()
{
    OpenLog();
    Log("BOOT pid=%d uid=%d dcm-soft-tee", getpid(), getuid());

    IpcApi ipc = LoadIpc();
    gGetCallingUid = ipc.getCallingUid;
    void *stub = BuildStub(ipc);
    if (stub == nullptr) {
        Log("FAIL stub");
        return 2;
    }
    /* Keep a strong ref alive across retries and the entire binder loop so a
     * transient AddSystemAbility failure cannot release the stub early. */
    OHOS::sptr<OHOS::IRemoteObject> stubRef(reinterpret_cast<OHOS::IRemoteObject *>(stub));
    SamgrApi samgr = LoadSamgr();
    bool registered = false;
    for (int attempt = 0; attempt < 60 && !registered; ++attempt) {
        registered = RegisterAbility(samgr, stubRef);
        if (!registered) {
            if (attempt == 0) {
                Log("WAIT samgr not ready, retrying");
            }
            sleep(1);
        }
    }
    if (!registered) {
        Log("FAIL register SA %d", kDcmSaId);
        return 3;
    }
    if (!ipc.setThreads(4)) {
        Log("WARN SetMaxWorkThreadNum failed");
    }
    Log("READY sa=%d", kDcmSaId);
    ipc.joinThread();
    Log("EXIT binder loop returned");
    return 0;
}
