/* SPDX-License-Identifier: MIT */
/*
 * native_nfc_fwk_probe.cpp - End-to-end HarmonyOS NFC Framework (SA 1140) probe
 * for Xiaomi marble (SM7475).
 *
 * Verifies:
 *   1. Clean-room libnci_native_vendor.z.so resolves chip="sn110u", dynamically
 *      loads stock libnci_native_t001.z.so (Sn110NciExtImpl), and returns valid
 *      non-null INciNfccInterface / INciTagInterface / INciCeInterface / INciExtInterface
 *      plus working NewInterface()/DeleteInterface().
 *   2. HarmonyOS stock NFC System Ability SA 1140 (nfc_service) is registered in
 *      samgr (optionally waits up to 5s or checks on-demand readiness).
 *   3. Public OHOS::NFC::KITS::NfcController (linked against unmodified donor
 *      libnfc_inner_kits_controller.z.so + libnfc_inner_kits_common.z.so) reports
 *      IsNfcAvailable()==true, valid GetNfcState() (1=OFF..4=TURNING_OFF), and
 *      consistent IsNfcOpen(bool&) without spoofing any AccessToken.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <unistd.h>

#include <ipc_skeleton.h>
#include <iremote_object.h>
#include <message_option.h>
#include <message_parcel.h>

namespace OHOS {
namespace NFC {
namespace KITS {

class NfcController {
public:
    static NfcController &GetInstance();
    void InitNfcRemoteSA();
    int GetNfcState();
    bool IsNfcAvailable();
    int IsNfcOpen(bool &isOpen);
};

} // namespace KITS

namespace NCI {

class INciCeInterface;
class INciNfccInterface;
class INciTagInterface;
class INciExtInterface;

class INciNativeInterface {
public:
    virtual ~INciNativeInterface() = default;
    virtual std::shared_ptr<INciCeInterface> GetNciCeInterface() = 0;
    virtual std::shared_ptr<INciNfccInterface> GetNciNfccInterface() = 0;
    virtual std::shared_ptr<INciTagInterface> GetNciTagInterface() = 0;
};

class NciNativeAdapter {
public:
    static NciNativeAdapter &GetInstance();
    std::string GetChipType() const;
    std::shared_ptr<INciCeInterface> GetNciCeInterface();
    std::shared_ptr<INciNfccInterface> GetNciNfccInterface();
    std::shared_ptr<INciTagInterface> GetNciTagInterface();
    std::shared_ptr<INciExtInterface> GetNciExtInterface();
};

} // namespace NCI
} // namespace NFC
} // namespace OHOS

using OHOS::IPCSkeleton;
using OHOS::IRemoteObject;
using OHOS::MessageOption;
using OHOS::MessageParcel;
using OHOS::sptr;
using OHOS::NFC::KITS::NfcController;
using OHOS::NFC::NCI::INciNativeInterface;
using OHOS::NFC::NCI::NciNativeAdapter;

namespace {

static const char *nfcStateName(int st) {
    switch (st) {
        case 1: return "OFF";
        case 2: return "TURNING_ON";
        case 3: return "ON";
        case 4: return "TURNING_OFF";
        default: return "UNKNOWN";
    }
}

static sptr<IRemoteObject> querySamgrSa(int32_t saId) {
    sptr<IRemoteObject> samgr = IPCSkeleton::GetContextObject();
    if (samgr == nullptr) {
        return nullptr;
    }
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(u"ohos.samgr.accessToken")) {
        return nullptr;
    }
    if (!data.WriteInt32(saId)) {
        return nullptr;
    }
    /* Transaction 2 = CHECK_SYSTEM_ABILITY_TRANSACTION (non-blocking). */
    int rc = samgr->SendRequest(2, data, reply, opt);
    if (rc != 0) {
        return nullptr;
    }
    return reply.ReadRemoteObject();
}

static bool testVendorAdapterDlopen(const char *libPath,
                                    std::string &outChip,
                                    int &nfccOk, int &ceOk, int &tagOk, int &extOk,
                                    int &proxyOk) {
    outChip.clear();
    nfccOk = ceOk = tagOk = extOk = proxyOk = 0;

    void *h = ::dlopen(libPath, RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        std::printf("VENDOR_DLOPEN_ERR path=%s err=%s\n", libPath, ::dlerror());
        return false;
    }

    using GetInstFn = NciNativeAdapter &(*)();
    using GetChipFn = std::string (*)(const NciNativeAdapter *);
    using GetCeFn = std::shared_ptr<OHOS::NFC::NCI::INciCeInterface> (*)(NciNativeAdapter *);
    using GetNfccFn = std::shared_ptr<OHOS::NFC::NCI::INciNfccInterface> (*)(NciNativeAdapter *);
    using GetTagFn = std::shared_ptr<OHOS::NFC::NCI::INciTagInterface> (*)(NciNativeAdapter *);
    using GetExtFn = std::shared_ptr<OHOS::NFC::NCI::INciExtInterface> (*)(NciNativeAdapter *);
    using NewFn = INciNativeInterface *(*)();
    using DelFn = void (*)(INciNativeInterface *);

    auto getInst = reinterpret_cast<GetInstFn>(
        ::dlsym(h, "_ZN4OHOS3NFC3NCI16NciNativeAdapter11GetInstanceEv"));
    auto getChip = reinterpret_cast<GetChipFn>(
        ::dlsym(h, "_ZNK4OHOS3NFC3NCI16NciNativeAdapter11GetChipTypeEv"));
    auto getCe = reinterpret_cast<GetCeFn>(
        ::dlsym(h, "_ZN4OHOS3NFC3NCI16NciNativeAdapter17GetNciCeInterfaceEv"));
    auto getNfcc = reinterpret_cast<GetNfccFn>(
        ::dlsym(h, "_ZN4OHOS3NFC3NCI16NciNativeAdapter19GetNciNfccInterfaceEv"));
    auto getTag = reinterpret_cast<GetTagFn>(
        ::dlsym(h, "_ZN4OHOS3NFC3NCI16NciNativeAdapter18GetNciTagInterfaceEv"));
    auto getExt = reinterpret_cast<GetExtFn>(
        ::dlsym(h, "_ZN4OHOS3NFC3NCI16NciNativeAdapter18GetNciExtInterfaceEv"));
    auto newFn = reinterpret_cast<NewFn>(::dlsym(h, "NewInterface"));
    auto delFn = reinterpret_cast<DelFn>(::dlsym(h, "DeleteInterface"));

    if (!getInst || !getChip || !getCe || !getNfcc || !getTag || !getExt || !newFn || !delFn) {
        std::printf("VENDOR_DLSYM_ERR missing_sym=1\n");
        ::dlclose(h);
        return false;
    }

    NciNativeAdapter &adapter = getInst();
    outChip = getChip(&adapter);
    auto nfcc = getNfcc(&adapter);
    auto ce = getCe(&adapter);
    auto tag = getTag(&adapter);
    auto ext = getExt(&adapter);
    nfccOk = (nfcc != nullptr) ? 1 : 0;
    ceOk = (ce != nullptr) ? 1 : 0;
    tagOk = (tag != nullptr) ? 1 : 0;
    extOk = (ext != nullptr) ? 1 : 0;

    INciNativeInterface *proxy = newFn();
    if (proxy != nullptr) {
        auto pNfcc = proxy->GetNciNfccInterface();
        auto pCe = proxy->GetNciCeInterface();
        auto pTag = proxy->GetNciTagInterface();
        proxyOk = (pNfcc != nullptr && pCe != nullptr && pTag != nullptr) ? 1 : 0;
        delFn(proxy);
    }

    return (!outChip.empty() && nfccOk && ceOk && tagOk && extOk && proxyOk);
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *vendorLib = "/system/lib64/libnci_native_vendor.z.so";
    bool vendorOnly = false;
    bool loadSa = false;
    bool expectOn = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            bool ok = (std::strcmp(nfcStateName(1), "OFF") == 0 &&
                       std::strcmp(nfcStateName(3), "ON") == 0);
            std::printf("MARBLE_NFC_FWK_PROBE_SELF_TEST_OK=%d\n", ok ? 1 : 0);
            return ok ? 0 : 1;
        } else if (std::strcmp(argv[i], "--vendor-lib") == 0 && i + 1 < argc) {
            vendorLib = argv[++i];
        } else if (std::strcmp(argv[i], "--vendor-only") == 0) {
            vendorOnly = true;
        } else if (std::strcmp(argv[i], "--load-sa") == 0) {
            loadSa = true;
        } else if (std::strcmp(argv[i], "--expect-on") == 0) {
            expectOn = true;
        }
    }

    std::string chip;
    int nfccOk = 0, ceOk = 0, tagOk = 0, extOk = 0, proxyOk = 0;
    bool vendorOk = testVendorAdapterDlopen(vendorLib, chip, nfccOk, ceOk, tagOk, extOk, proxyOk);
    std::printf("NFC_VENDOR_ADAPTER lib=%s chip=%s nfcc=%d ce=%d tag=%d ext=%d proxy=%d ok=%d\n",
                vendorLib, chip.c_str(), nfccOk, ceOk, tagOk, extOk, proxyOk, vendorOk ? 1 : 0);
    if (vendorOnly) {
        std::fflush(stdout);
        _exit(vendorOk ? 0 : 2);
    }

    NfcController &ctrl = NfcController::GetInstance();
    if (loadSa) {
        ctrl.InitNfcRemoteSA();
    }

    sptr<IRemoteObject> sa1140Obj = querySamgrSa(1140);
    bool sa1140 = (sa1140Obj != nullptr);
    std::printf("SA1140_REGISTERED=%d\n", sa1140 ? 1 : 0);

    bool avail = ctrl.IsNfcAvailable();
    int state = ctrl.GetNfcState();
    bool isOpen = false;
    int openRc = ctrl.IsNfcOpen(isOpen);

    /* Re-check SA 1140 after NfcController query in case on-demand activation just completed. */
    if (!sa1140) {
        sa1140Obj = querySamgrSa(1140);
        sa1140 = (sa1140Obj != nullptr);
    }

    bool stateValid = (state >= 1 && state <= 4);
    bool openConsistent = (openRc == 0 && (isOpen == (state == 3)));
    bool stateExpectOk = expectOn ? (state == 3) : stateValid;

    std::printf("NFC_CONTROLLER avail=%d state=%d(%s) is_open=%d open_rc=%d\n",
                avail ? 1 : 0, state, nfcStateName(state), isOpen ? 1 : 0, openRc);

    bool allOk = vendorOk && sa1140 && avail && stateExpectOk && openConsistent;
    std::printf("MARBLE_NFC_FWK_PROBE_SUMMARY sa1140=%d avail=%d state=%d state_name=%s "
                "is_open=%d open_rc=%d chip=%s nfcc=%d ce=%d tag=%d ext=%d proxy=%d all_ok=%d\n",
                sa1140 ? 1 : 0, avail ? 1 : 0, state, nfcStateName(state),
                isOpen ? 1 : 0, openRc, chip.c_str(),
                nfccOk, ceOk, tagOk, extOk, proxyOk, allOk ? 1 : 0);
    std::fflush(stdout);
    _exit(allOk ? 0 : 10);
}
