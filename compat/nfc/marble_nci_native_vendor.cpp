/* SPDX-License-Identifier: MIT */
/*
 * marble_nci_native_vendor.cpp - Clean-room SM7475 (Xiaomi marble) OHOS
 * libnci_native_vendor.z.so implementation bridging stock HarmonyOS
 * nfc_service (SA 1140) to stock NXP SN110 NCI 2.0 stack (libnci_native_t001.z.so
 * -> libnfc_nci_t001.z.so -> Binder libnfc_proxy_1.1.z.so -> nfc_host host-10).
 *
 * Why needed on marble:
 *   Donor libnci_native_vendor.z.so reads /sys/nfc/nfc_chip_type to map the chip
 *   name to libnci_native_t001.z.so. SM7475's kernel nq-nci driver does not create
 *   /sys/nfc/nfc_chip_type, so stock GetChipType() returns "" and fails to dlopen
 *   the SN110 NCI implementation. This clean-room drop-in reads /sys/nfc/nfc_chip_type
 *   when present and falls back cleanly to "sn110u" -> "libnci_native_t001.z.so",
 *   matching 100% of the donor C++ & C ABI (NciLibsLoader=80B, NciNativeAdapter=16B,
 *   NciNativepProxyImpl vtable=56B, NewInterface/DeleteInterface).
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace OHOS {
namespace NFC {
namespace NCI {

class INciCeInterface;
class INciNfccInterface;
class INciTagInterface;
class INciExtInterface;

/* Base NCI native interface expected by stock libnfc_service.z.so (vtable 56 B). */
class INciNativeInterface {
public:
    virtual ~INciNativeInterface() = default;
    virtual std::shared_ptr<INciCeInterface> GetNciCeInterface() = 0;
    virtual std::shared_ptr<INciNfccInterface> GetNciNfccInterface() = 0;
    virtual std::shared_ptr<INciTagInterface> GetNciTagInterface() = 0;
};

/* Extended NCI native interface exported by donor libnci_native_t001.z.so (slot +40). */
class INciNativeExtInterface : public INciNativeInterface {
public:
    ~INciNativeExtInterface() override = default;
    virtual std::shared_ptr<INciExtInterface> GetNciExtInterface() = 0;
};

class NciLibsLoader {
public:
    NciLibsLoader(const std::string &libPath,
                  const std::string &newSym,
                  const std::string &delSym);
    ~NciLibsLoader();
    bool LoadLib();
    bool CloseLib();
    std::shared_ptr<INciNativeInterface> NewInstance();

private:
    void *handle_;
    std::string libPath_;
    std::string newSym_;
    std::string delSym_;
};

NciLibsLoader::NciLibsLoader(const std::string &libPath,
                             const std::string &newSym,
                             const std::string &delSym)
    : handle_(nullptr), libPath_(libPath), newSym_(newSym), delSym_(delSym) {}

NciLibsLoader::~NciLibsLoader() {
    (void)CloseLib();
}

bool NciLibsLoader::LoadLib() {
    if (libPath_.empty()) {
        return false;
    }
    if (handle_ != nullptr) {
        return true;
    }
    handle_ = ::dlopen(libPath_.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle_ == nullptr) {
        const char *err = ::dlerror();
        std::fprintf(stderr, "marble_nci_vendor: dlopen(%s) failed: %s\n",
                     libPath_.c_str(), err ? err : "unknown");
        return false;
    }
    return true;
}

bool NciLibsLoader::CloseLib() {
    if (handle_ == nullptr) {
        return true;
    }
    void *h = handle_;
    handle_ = nullptr;
    return ::dlclose(h) == 0;
}

std::shared_ptr<INciNativeInterface> NciLibsLoader::NewInstance() {
    if (handle_ == nullptr && !LoadLib()) {
        return nullptr;
    }
    using NewFn = INciNativeInterface *(*)();
    using DelFn = void (*)(INciNativeInterface *);

    auto newFn = reinterpret_cast<NewFn>(::dlsym(handle_, newSym_.c_str()));
    auto delFn = reinterpret_cast<DelFn>(::dlsym(handle_, delSym_.c_str()));
    if (newFn == nullptr || delFn == nullptr) {
        std::fprintf(stderr, "marble_nci_vendor: dlsym(%s/%s) failed in %s\n",
                     newSym_.c_str(), delSym_.c_str(), libPath_.c_str());
        (void)CloseLib();
        return nullptr;
    }
    INciNativeInterface *raw = newFn();
    if (raw == nullptr) {
        return nullptr;
    }
    return std::shared_ptr<INciNativeInterface>(raw, delFn);
}

class NciNativeFactory {
public:
    static std::string GetChipLibPath(const std::string &chipType);
    static std::shared_ptr<INciNativeInterface> GetNciNativeInterface(const std::string &chipType);
    static inline std::unique_ptr<NciLibsLoader> loader_ = nullptr;
};

std::string NciNativeFactory::GetChipLibPath(const std::string &chipType) {
    static const std::unordered_map<std::string, std::string> kChipMap = {
        { "sn110u",  "libnci_native_t001.z.so" },
        { "thn31",   "libnci_native_t001.z.so" },
        { "thn41",   "libnci_native_t001.z.so" },
        { "ca11",    "libnci_native_t001.z.so" },
        { "st21nfc", "libnci_native_t011.z.so" },
        { "st54k",   "libnci_native_t011.z.so" },
        { "pn80t",   "libnci_native_t010.z.so" },
    };
    auto it = kChipMap.find(chipType);
    if (it != kChipMap.end()) {
        return it->second;
    }
    /* Default to NXP SN110T NCI 2.0 library on Xiaomi marble (SM7475). */
    return "libnci_native_t001.z.so";
}

std::shared_ptr<INciNativeInterface> NciNativeFactory::GetNciNativeInterface(const std::string &chipType) {
    std::string libPath = GetChipLibPath(chipType);
    loader_.reset(new NciLibsLoader(libPath, "NewInterface", "DeleteInterface"));
    if (!loader_->LoadLib()) {
        return nullptr;
    }
    return loader_->NewInstance();
}

class NciNativeAdapter {
public:
    static NciNativeAdapter &GetInstance();
    NciNativeAdapter();
    void InitNativeInterface();
    std::string GetChipType() const;
    std::shared_ptr<INciCeInterface> GetNciCeInterface();
    std::shared_ptr<INciNfccInterface> GetNciNfccInterface();
    std::shared_ptr<INciTagInterface> GetNciTagInterface();
    std::shared_ptr<INciExtInterface> GetNciExtInterface();

private:
    std::shared_ptr<INciNativeInterface> nativeInterface_;
};

NciNativeAdapter &NciNativeAdapter::GetInstance() {
    static NciNativeAdapter instance;
    return instance;
}

NciNativeAdapter::NciNativeAdapter() : nativeInterface_(nullptr) {
    InitNativeInterface();
}

void NciNativeAdapter::InitNativeInterface() {
    std::string chip = GetChipType();
    nativeInterface_ = NciNativeFactory::GetNciNativeInterface(chip);
}

std::string NciNativeAdapter::GetChipType() const {
    std::ifstream ifs("/sys/nfc/nfc_chip_type");
    if (ifs.is_open()) {
        std::string val;
        std::getline(ifs, val);
        while (!val.empty() &&
               (val.back() == '\n' || val.back() == '\r' ||
                val.back() == ' ' || val.back() == '\t')) {
            val.pop_back();
        }
        if (!val.empty()) {
            return val;
        }
    }
    /* Xiaomi marble carries NXP SN110T/SN220 (/dev/nq-nci, chip=0xa4 fw=16.104). */
    return "sn110u";
}

std::shared_ptr<INciCeInterface> NciNativeAdapter::GetNciCeInterface() {
    if (!nativeInterface_) {
        return nullptr;
    }
    return nativeInterface_->GetNciCeInterface();
}

std::shared_ptr<INciNfccInterface> NciNativeAdapter::GetNciNfccInterface() {
    if (!nativeInterface_) {
        return nullptr;
    }
    return nativeInterface_->GetNciNfccInterface();
}

std::shared_ptr<INciTagInterface> NciNativeAdapter::GetNciTagInterface() {
    if (!nativeInterface_) {
        return nullptr;
    }
    return nativeInterface_->GetNciTagInterface();
}

std::shared_ptr<INciExtInterface> NciNativeAdapter::GetNciExtInterface() {
    if (!nativeInterface_) {
        return nullptr;
    }
    auto *ext = static_cast<INciNativeExtInterface *>(nativeInterface_.get());
    return ext->GetNciExtInterface();
}

class NciNativepProxyImpl : public INciNativeInterface {
public:
    ~NciNativepProxyImpl() override;
    std::shared_ptr<INciCeInterface> GetNciCeInterface() override;
    std::shared_ptr<INciNfccInterface> GetNciNfccInterface() override;
    std::shared_ptr<INciTagInterface> GetNciTagInterface() override;
};

NciNativepProxyImpl::~NciNativepProxyImpl() = default;

std::shared_ptr<INciCeInterface> NciNativepProxyImpl::GetNciCeInterface() {
    return NciNativeAdapter::GetInstance().GetNciCeInterface();
}

std::shared_ptr<INciNfccInterface> NciNativepProxyImpl::GetNciNfccInterface() {
    return NciNativeAdapter::GetInstance().GetNciNfccInterface();
}

std::shared_ptr<INciTagInterface> NciNativepProxyImpl::GetNciTagInterface() {
    return NciNativeAdapter::GetInstance().GetNciTagInterface();
}

static_assert(sizeof(NciNativeAdapter) == 16, "NciNativeAdapter ABI must match donor 16 B");
static_assert(sizeof(NciNativepProxyImpl) == 8, "NciNativepProxyImpl ABI must match donor 8 B");
#ifndef MARBLE_NCI_VENDOR_HOST_TEST
static_assert(sizeof(NciLibsLoader) == 80, "NciLibsLoader ABI must match donor 80 B (libc++)");
#endif

extern "C" {

__attribute__((visibility("default")))
INciNativeInterface *NewInterface() {
    return new NciNativepProxyImpl();
}

__attribute__((visibility("default")))
void DeleteInterface(INciNativeInterface *instance) {
    delete instance;
}

} // extern "C"

} // namespace NCI
} // namespace NFC
} // namespace OHOS

#ifdef MARBLE_NCI_VENDOR_HOST_TEST
int main(void) {
    using namespace OHOS::NFC::NCI;
    if (NciNativeFactory::GetChipLibPath("sn110u") != "libnci_native_t001.z.so") return 1;
    if (NciNativeFactory::GetChipLibPath("thn31") != "libnci_native_t001.z.so") return 2;
    if (NciNativeFactory::GetChipLibPath("st21nfc") != "libnci_native_t011.z.so") return 3;
    if (NciNativeFactory::GetChipLibPath("pn80t") != "libnci_native_t010.z.so") return 4;
    if (NciNativeFactory::GetChipLibPath("") != "libnci_native_t001.z.so") return 5;
    if (NciNativeFactory::GetChipLibPath("unknown_chip") != "libnci_native_t001.z.so") return 6;

    /* Check fallback chip type when /sys/nfc/nfc_chip_type is absent on host. */
    NciNativeAdapter adapter;
    std::string chip = adapter.GetChipType();
    if (chip != "sn110u") return 7;

    INciNativeInterface *proxy = NewInterface();
    if (proxy == nullptr) return 8;
    DeleteInterface(proxy);

    std::printf("MARBLE_NCI_VENDOR_HOST_TEST_OK=1 chip=%s lib=%s adapter_sz=%zu proxy_sz=%zu\n",
                chip.c_str(),
                NciNativeFactory::GetChipLibPath(chip).c_str(),
                sizeof(NciNativeAdapter),
                sizeof(NciNativepProxyImpl));
    return 0;
}
#endif
