/* SPDX-License-Identifier: MIT
 * marble DCM SA 65866 client probe.
 *
 * Speaks the donor's real wire contract so the software-TEE service can be
 * accepted without the AppGallery/ThemeManager UI:
 *   request : SA 65866, code (default 3), interface token + callback
 *   reply   : code 3 -> int32 errCode + uint64 requestId
 *             other  -> int32 errCode (donor "not supported" family)
 *   callback: code 1, interface token + requestId + errCode + token
 * Run after the service is started; never writes device state.
 */
#include "dcm_token.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unistd.h>

#include "ipc_skeleton.h"
#include "iremote_object.h"
#include "message_option.h"
#include "message_parcel.h"

namespace {

constexpr int32_t kDcmSaId = 65866;
constexpr uint32_t kGetDeviceTokenCode = 3;
constexpr uint32_t kDeviceTokenCallbackCode = 1;
constexpr int32_t kErrNotSupported = -10009;
const char16_t *const kCallbackDescriptor =
    u"ohos.security.devicecertmanager.dcmDeviceTokCallback";
const char16_t *const kAnonCallbackDescriptor =
    u"ohos.security.devicecertmanager.dcmAnonymousCallback";
constexpr uint32_t kAnonymousAttestCode = 2;
constexpr uint32_t kAnonymousCallbackCode = 1;
constexpr uint32_t kChallengeSize = 32;

using StubCtor = void (*)(void *, std::u16string, bool);
using StubDispatch = int (*)(void *, uint32_t, OHOS::MessageParcel &, OHOS::MessageParcel &,
                             OHOS::MessageOption &);
using SetThreads = bool (*)(int);
using JoinThread = void (*)();
using GetInstance = void *(*)();
using GetManager = void *(*)(void *);

struct SptrCarrier {
    void *ptr;
    ~SptrCarrier() {}
};

struct CallbackState {
    std::mutex mutex;
    bool received = false;
    int32_t errCode = -1;
    uint64_t requestId = 0;
    std::string token;
};

CallbackState gState;

/* Anonymous Attest (code 2) reply: requestId + errCode + errInfo + certCount +
 * certCount x (uint32 len + unpadded DER). */
struct AnonState {
    std::mutex mutex;
    bool received = false;
    int32_t errCode = -1;
    uint64_t requestId = 0;
    uint32_t certCount = 0;
    size_t certLen[8] = {0};
    uint32_t certHead[8] = {0};
};

AnonState gAnon;
JoinThread gJoinThread = nullptr;

int CallbackDispatch(void *self, uint32_t code, OHOS::MessageParcel &data,
                     OHOS::MessageParcel &reply, OHOS::MessageOption &option)
{
    (void)self;
    (void)reply;
    (void)option;
    if (code != kDeviceTokenCallbackCode) {
        return -1;
    }
    std::u16string descriptor = data.ReadInterfaceToken();
    if (descriptor != std::u16string(kCallbackDescriptor)) {
        std::fprintf(stderr, "CALLBACK bad descriptor\n");
        return -1;
    }
    uint64_t requestId = 0;
    int32_t errCode = -1;
    std::string token;
    if (!data.ReadUint64(requestId) || !data.ReadInt32(errCode)) {
        return -1;
    }
    if (errCode == 0 && !data.ReadString(token)) {
        return -1;
    }
    char digest[17] = {0};
    if (!token.empty()) {
        dcm_token_digest(token.c_str(), digest);
    }
    {
        std::lock_guard<std::mutex> lock(gState.mutex);
        gState.received = true;
        gState.errCode = errCode;
        gState.requestId = requestId;
        gState.token = token;
    }
    std::printf("CALLBACK_REQUEST_ID=%016llx ERRCODE=%d TOKEN_LEN=%zu TOKEN_DIGEST=%s\n",
                static_cast<unsigned long long>(requestId), errCode, token.size(), digest);
    return 0;
}

int AnonCallbackDispatch(void *self, uint32_t code, OHOS::MessageParcel &data,
                         OHOS::MessageParcel &reply, OHOS::MessageOption &option)
{
    (void)self;
    (void)reply;
    (void)option;
    if (code != kAnonymousCallbackCode) {
        return -1;
    }
    std::u16string descriptor = data.ReadInterfaceToken();
    if (descriptor != std::u16string(kAnonCallbackDescriptor)) {
        std::fprintf(stderr, "ANON_CALLBACK bad descriptor\n");
        return -1;
    }
    uint64_t requestId = 0;
    int32_t errCode = -1;
    std::string errInfo;
    if (!data.ReadUint64(requestId) || !data.ReadInt32(errCode) || !data.ReadString(errInfo)) {
        std::fprintf(stderr, "ANON_CALLBACK parse failed\n");
        return -1;
    }
    uint32_t certCount = 0;
    uint32_t lens[8] = {0};
    uint32_t heads[8] = {0};
    if (errCode == 0 && data.ReadUint32(certCount)) {
        for (uint32_t i = 0; i < certCount && i < 8; ++i) {
            uint32_t len = 0;
            if (!data.ReadUint32(len)) {
                break;
            }
            const uint8_t *buf = (len != 0) ? data.ReadUnpadBuffer(len) : nullptr;
            if (len != 0 && buf == nullptr) {
                break;
            }
            lens[i] = len;
            heads[i] = (len >= 4) ? (static_cast<uint32_t>(buf[0]) << 24 |
                                     static_cast<uint32_t>(buf[1]) << 16 |
                                     static_cast<uint32_t>(buf[2]) << 8 |
                                     static_cast<uint32_t>(buf[3]))
                                  : 0;
        }
    }
    std::printf("ANON_CALLBACK request_id=%016llx err=%d info=%s count=%u",
                static_cast<unsigned long long>(requestId), errCode, errInfo.c_str(), certCount);
    for (uint32_t i = 0; i < certCount && i < 8; ++i) {
        std::printf(" cert%u_len=%u head=%08x", i, lens[i], heads[i]);
    }
    std::printf("\n");
    {
        std::lock_guard<std::mutex> lock(gAnon.mutex);
        gAnon.received = true;
        gAnon.errCode = errCode;
        gAnon.requestId = requestId;
        gAnon.certCount = certCount;
        for (uint32_t i = 0; i < certCount && i < 8; ++i) {
            gAnon.certLen[i] = lens[i];
            gAnon.certHead[i] = heads[i];
        }
    }
    return 0;
}

void *MakeCallbackStub(StubCtor ctor, StubDispatch original, const char16_t *descriptor,
                       StubDispatch replacement)
{
    void *object = ::operator new(4096);
    std::memset(object, 0, 4096);
    ctor(object, std::u16string(descriptor), false);
    void **table = *reinterpret_cast<void ***>(object);
    if (table == nullptr || table[200 / 8] != reinterpret_cast<void *>(original)) {
        std::fprintf(stderr, "PROBE_VTABLE_GUARD_FAILED\n");
        return nullptr;
    }
    alignas(16) static uintptr_t privateTable[128];
    std::memcpy(privateTable, reinterpret_cast<const char *>(table) - 4 * sizeof(void *),
                sizeof(privateTable));
    privateTable[4 + 200 / 8] = reinterpret_cast<uintptr_t>(replacement);
    *reinterpret_cast<void ***>(object) = reinterpret_cast<void **>(privateTable + 4);
    return object;
}

}  // namespace

int main(int argc, char **argv)
{
    alarm(30);
    bool dumpToken = false;
    bool sdkState = false;
    bool anonMode = false;
    bool parcelSelftest = false;
    uint32_t requestCode = kGetDeviceTokenCode;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-token") == 0) {
            dumpToken = true;
        } else if (std::strcmp(argv[i], "--sdk-state") == 0) {
            sdkState = true;
        } else if (std::strcmp(argv[i], "--parcel-selftest") == 0) {
            parcelSelftest = true;
        } else if (std::strcmp(argv[i], "--anon") == 0) {
            anonMode = true;
        } else if (std::strcmp(argv[i], "--code") == 0 && i + 1 < argc) {
            requestCode = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        }
    }
    if (parcelSelftest) {
        void *ipcLib = dlopen("libipc_single.z.so", RTLD_NOW | RTLD_GLOBAL);
        if (ipcLib == nullptr) {
            std::printf("SELFTEST dlopen failed\n");
            return 20;
        }
        using WriteUnpadFn = bool (*)(void *, const void *, size_t);
        auto writeUnpad = reinterpret_cast<WriteUnpadFn>(
            dlsym(RTLD_DEFAULT, "_ZN4OHOS6Parcel16WriteUnpadBufferEPKvm"));
        OHOS::MessageParcel p;
        std::string dev(418, 'A');
        std::string root(374, 'B');
        auto putCert = [&](const std::string &blob) -> bool {
            return p.WriteUint32(static_cast<uint32_t>(blob.size())) &&
                   writeUnpad(&p, blob.data(), blob.size());
        };
        bool w = p.WriteInterfaceToken(std::u16string(kAnonCallbackDescriptor)) &&
                 p.WriteUint64(0x1122334455667788ull) && p.WriteInt32(0) &&
                 p.WriteString(std::string()) && p.WriteUint32(2) && putCert(dev) &&
                 putCert(root);
        std::u16string d2 = p.ReadInterfaceToken();
        uint64_t id = 0;
        int32_t err = -1;
        std::string info;
        uint32_t count = 0;
        uint32_t len0 = 0;
        uint32_t len1 = 0;
        const uint8_t *b0 = nullptr;
        const uint8_t *b1 = nullptr;
        bool r = p.ReadUint64(id) && p.ReadInt32(err) && p.ReadString(info) &&
                 p.ReadUint32(count) && p.ReadUint32(len0);
        if (r && len0 != 0) {
            b0 = p.ReadUnpadBuffer(len0);
        }
        r = r && p.ReadUint32(len1);
        if (r && len1 != 0) {
            b1 = p.ReadUnpadBuffer(len1);
        }
        std::printf("SELFTEST write=%d descriptor_ok=%d id=%016llx err=%d count=%u "
                    "len0=%u b0=%p len1=%u b1=%p\n",
                    static_cast<int>(w), static_cast<int>(d2 == std::u16string(kAnonCallbackDescriptor)),
                    static_cast<unsigned long long>(id), err, count, len0,
                    const_cast<void *>(static_cast<const void *>(b0)), len1,
                    const_cast<void *>(static_cast<const void *>(b1)));
        return (w && r && count == 2 && len0 == 418 && len1 == 374) ? 0 : 21;
    }
    if (sdkState) {
        void *sdk = dlopen("/system/lib64/libdevice_cert_mgr_sdk.z.so", RTLD_NOW);
        if (sdk == nullptr) {
            std::fprintf(stderr, "SDK_DLOPEN_FAILED %s\n", dlerror());
            return 17;
        }
        using StateFn = int32_t (*)(int32_t *);
        auto getAvail = reinterpret_cast<StateFn>(dlsym(sdk, "DcmGetCertAvailableState"));
        auto getProv = reinterpret_cast<StateFn>(dlsym(sdk, "DcmGetProvisionedInfoStatus"));
        if (getAvail == nullptr || getProv == nullptr) {
            std::fprintf(stderr, "SDK_SYMBOL_FAILED avail=%p prov=%p\n",
                         reinterpret_cast<void *>(getAvail), reinterpret_cast<void *>(getProv));
            return 17;
        }
        int32_t availState = -1;
        int32_t provStatus = -1;
        int32_t rc1 = getAvail(&availState);
        int32_t rc2 = getProv(&provStatus);
        std::printf("SDK_CERT_STATE rc=%d state=%d\n", rc1, availState);
        std::printf("SDK_PROV_STATUS rc=%d status=%d\n", rc2, provStatus);
        return (rc1 == 0 && availState == 1 && rc2 == 0 && provStatus == 1) ? 0 : 17;
    }
    void *ipc = dlopen("libipc_single.z.so", RTLD_NOW | RTLD_GLOBAL);
    void *samgr = dlopen("libsamgr_proxy.z.so", RTLD_NOW | RTLD_GLOBAL);
    if (samgr == nullptr) {
        samgr = dlopen("/system/lib64/chipset-sdk-sp/libsamgr_proxy.z.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (ipc == nullptr || samgr == nullptr) {
        std::fprintf(stderr, "PROBE_DLOPEN_FAILED ipc=%p samgr=%p %s\n", ipc, samgr, dlerror());
        return 2;
    }
    auto ctor = reinterpret_cast<StubCtor>(dlsym(
        ipc, "_ZN4OHOS13IPCObjectStubC1ENSt3__h12basic_stringIDsNS1_11char_traitsIDsEENS1_9allocatorIDsEEEEb"));
    auto original = reinterpret_cast<StubDispatch>(dlsym(
        ipc, "_ZN4OHOS13IPCObjectStub15OnRemoteRequestEjRNS_13MessageParcelES2_RNS_13MessageOptionE"));
    auto setThreads =
        reinterpret_cast<SetThreads>(dlsym(ipc, "_ZN4OHOS11IPCSkeleton19SetMaxWorkThreadNumEi"));
    auto joinThread =
        reinterpret_cast<JoinThread>(dlsym(ipc, "_ZN4OHOS11IPCSkeleton14JoinWorkThreadEv"));
    auto instance = reinterpret_cast<GetInstance>(
        dlsym(samgr, "_ZN4OHOS26SystemAbilityManagerClient11GetInstanceEv"));
    auto manager = reinterpret_cast<GetManager>(
        dlsym(samgr, "_ZN4OHOS26SystemAbilityManagerClient23GetSystemAbilityManagerEv"));
    if (ctor == nullptr || original == nullptr || setThreads == nullptr || joinThread == nullptr ||
        instance == nullptr || manager == nullptr) {
        std::fprintf(stderr, "PROBE_SYMBOL_FAILED\n");
        return 3;
    }
    if (!setThreads(2)) {
        std::fprintf(stderr, "PROBE_THREAD_FAILED\n");
        return 4;
    }
    gJoinThread = joinThread;
    pthread_t tid;
    if (pthread_create(&tid, nullptr, [](void *) -> void * { gJoinThread(); return nullptr; },
                       nullptr) != 0) {
        return 5;
    }
    pthread_detach(tid);

    void *client = instance();
    SptrCarrier mgr = reinterpret_cast<SptrCarrier (*)(void *)>(manager)(client);
    if (mgr.ptr == nullptr) {
        std::fprintf(stderr, "PROBE_SAMGR_FAILED\n");
        return 6;
    }
    using CheckAbility = SptrCarrier (*)(void *, int);
    CheckAbility check = reinterpret_cast<CheckAbility>((*reinterpret_cast<void ***>(mgr.ptr))[5]);
    SptrCarrier remote = check(mgr.ptr, kDcmSaId);
    if (remote.ptr == nullptr) {
        std::printf("PROBE_SA_65866=missing\n");
        return 7;
    }
    std::printf("PROBE_SA_65866=ready code=%u\n", requestCode);

    void *callback = anonMode
                         ? MakeCallbackStub(ctor, original, kAnonCallbackDescriptor,
                                            &AnonCallbackDispatch)
                         : MakeCallbackStub(ctor, original, kCallbackDescriptor,
                                            &CallbackDispatch);
    if (callback == nullptr) {
        return 8;
    }
    OHOS::sptr<OHOS::IRemoteObject> callbackRef(reinterpret_cast<OHOS::IRemoteObject *>(callback));
    OHOS::sptr<OHOS::IRemoteObject> service(reinterpret_cast<OHOS::IRemoteObject *>(remote.ptr));

    OHOS::MessageParcel data;
    OHOS::MessageParcel reply;
    OHOS::MessageOption option(0, 8);
    data.WriteInterfaceToken(std::u16string(u"ohos.security.devicecertmanager.service"));
    if (anonMode) {
        requestCode = kAnonymousAttestCode;
        using WriteUnpadFn = bool (*)(void *, const void *, size_t);
        auto writeUnpad = reinterpret_cast<WriteUnpadFn>(
            dlsym(RTLD_DEFAULT, "_ZN4OHOS6Parcel16WriteUnpadBufferEPKvm"));
        uint8_t challenge[kChallengeSize];
        for (uint32_t i = 0; i < kChallengeSize; ++i) {
            challenge[i] = static_cast<uint8_t>(0x40 + i);
        }
        bool ok = data.WriteRemoteObject(callbackRef) && data.WriteUint32(kChallengeSize) &&
                  writeUnpad != nullptr && writeUnpad(&data, challenge, kChallengeSize);
        std::printf("ANON_REQUEST challenge=%u write=%d unpad=%p\n", kChallengeSize,
                    static_cast<int>(ok), reinterpret_cast<void *>(writeUnpad));
    } else if (requestCode == kGetDeviceTokenCode) {
        data.WriteRemoteObject(callbackRef);
    } else {
        data.WriteUint32(32788);
        data.WriteUint32(0);
    }
    int rc = service->SendRequest(requestCode, data, reply, option);
    std::printf("REQUEST rc=%d\n", rc);
    if (rc != 0) {
        return 9;
    }
    int32_t errCode = -1;
    if (!reply.ReadInt32(errCode)) {
        std::printf("REPLY_READ_FAILED\n");
        return 10;
    }
    if (anonMode) {
        uint64_t anonId = 0;
        if (!reply.ReadUint64(anonId)) {
            std::printf("ANON_REPLY_READ_FAILED errCode=%d\n", errCode);
            return 10;
        }
        std::printf("ANON_REPLY errCode=%d requestId=%016llx\n", errCode,
                    static_cast<unsigned long long>(anonId));
        if (errCode != 0) {
            return 11;
        }
        for (int i = 0; i < 120; ++i) {
            {
                std::lock_guard<std::mutex> lock(gAnon.mutex);
                if (gAnon.received) {
                    break;
                }
            }
            usleep(100 * 1000);
        }
        std::lock_guard<std::mutex> lock(gAnon.mutex);
        if (!gAnon.received) {
            std::printf("ANON_CALLBACK=timeout\n");
            return 12;
        }
        if (gAnon.requestId != anonId) {
            std::printf("ANON_CALLBACK=request-id-mismatch\n");
            return 13;
        }
        bool okChain = gAnon.errCode == 0 && gAnon.certCount == 2 && gAnon.certLen[0] > 0 &&
                       gAnon.certLen[1] > 0 && (gAnon.certHead[0] >> 24) == 0x30 &&
                       (gAnon.certHead[1] >> 24) == 0x30;
        std::printf("ANON_VERDICT=%s\n", okChain ? "ok" : "fail");
        return okChain ? 0 : 15;
    }
    if (requestCode == 1 || requestCode == 5 || requestCode == 9) {
        uint32_t blobLen = 0;
        const uint8_t *buf = nullptr;
        uint32_t certCount = 0;
        if (errCode == 0 && reply.ReadUint32(blobLen) && blobLen >= 4 && blobLen <= 65536) {
            buf = reply.ReadBuffer(blobLen);
            if (buf != nullptr) {
                std::memcpy(&certCount, buf, sizeof(certCount));
            }
        }
        std::printf("ATTEST_IDS_CODE code=%u rc=%d blob_len=%u cert_count=%u\n",
                    requestCode, errCode, blobLen, certCount);
        return (errCode == 0 && buf != nullptr && blobLen >= 4 && certCount == 2) ? 0 : 15;
    }
    if (requestCode == 7 || requestCode == 10) {
        uint32_t len = 0;
        int32_t state = -1;
        bool ok = (errCode == 0) && reply.ReadUint32(len) && reply.ReadInt32(state);
        std::printf("CERT_STATE_CODE code=%u rc=%d len=%u state=%d\n",
                    requestCode, errCode, len, state);
        return (ok && len == 4 && state == 1) ? 0 : 15;
    }
    if (requestCode != kGetDeviceTokenCode) {
        std::printf("UNSUPPORTED_CODE code=%u rc=%d expected=%d\n", requestCode, errCode,
                    kErrNotSupported);
        return errCode == kErrNotSupported ? 0 : 15;
    }
    uint64_t requestId = 0;
    if (!reply.ReadUint64(requestId)) {
        std::printf("REPLY_READ_FAILED\n");
        return 10;
    }
    std::printf("REPLY errCode=%d requestId=%016llx\n", errCode,
                static_cast<unsigned long long>(requestId));
    if (errCode != 0) {
        return 11;
    }
    bool received = false;
    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard<std::mutex> lock(gState.mutex);
            received = gState.received;
        }
        if (received) {
            break;
        }
        usleep(100 * 1000);
    }
    std::lock_guard<std::mutex> lock(gState.mutex);
    if (!gState.received) {
        std::printf("TOKEN_CALLBACK=timeout\n");
        return 12;
    }
    if (gState.requestId != requestId) {
        std::printf("TOKEN_CALLBACK=request-id-mismatch cb=%016llx reply=%016llx\n",
                    static_cast<unsigned long long>(gState.requestId),
                    static_cast<unsigned long long>(requestId));
        return 13;
    }
    if (gState.errCode == 0 && gState.token.empty()) {
        std::printf("TOKEN_CALLBACK=empty-token\n");
        return 16;
    }
    std::printf("TOKEN_CALLBACK=ok errCode=%d token_len=%zu\n", gState.errCode,
                gState.token.size());
    if (dumpToken) {
        std::printf("TOKEN=%s\n", gState.token.c_str());
    }
    return gState.errCode == 0 ? 0 : 14;
}
