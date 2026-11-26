// SPDX-License-Identifier: MIT
/*
 * Clean-room Qualcomm SM7475 DisplayEngine wrapper module for Xiaomi marble.
 * Exports the exact DisplayEngineWrapperOH C++ ABI consumed by vendor/lib64/
 * libdisplay_engine_wrapper_service_1.1.z.so inside composer_host, bridging
 * DE_FEATURE_XCC (4) and DE_FEATURE_GMP (3) to libdisplay_composer_vdi_impl.z.so
 * SDE DSPP PCC v4 hardware color correction.
 */
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace OHOS {
template <typename T>
class sptr;
namespace HDI::Displayengine::V1_0 {
class IDisplayEngineCallback;
} // namespace HDI::Displayengine::V1_0
} // namespace OHOS

class DisplayEngineWrapperOH {
public:
    DisplayEngineWrapperOH();
    int32_t GetSupported(int32_t feature);
    int32_t SetHardwareEffect(int32_t feature, int32_t type, const std::vector<uint8_t> &data);
    int32_t GetHardwareEffect(int32_t feature, int32_t type, std::vector<uint8_t> &data);
    int32_t GetHardwareEffectExt(int32_t feature, int32_t type, int32_t size, std::vector<uint8_t> &data);
    int32_t RegisterCallback(const OHOS::sptr<OHOS::HDI::Displayengine::V1_0::IDisplayEngineCallback> &cb);

private:
    [[maybe_unused]] void *state_;
};

static_assert(sizeof(DisplayEngineWrapperOH) == 8, "Must match donor 8-byte make_shared allocation");

namespace {
constexpr int32_t kDeFeatureGmp = 3;
constexpr int32_t kDeFeatureXcc = 4;
constexpr const char *kDispParamPath = "/sys/class/mi_display/disp-DSI-0/disp_param";

// Linear sRGB -> Display P3 (D65) gamut mapping matrix in Q15 (0x8000 == 1.0).
// Every row sums to exactly 32768 so D65 white is preserved without shift.
constexpr int32_t kSrgbInP3Q15[9] = {
    26950,  5818,     0,
     1088, 31680,     0,
      560,  2372, 29836,
};

using SetPccMatrixFn = int32_t (*)(bool, const int32_t *);
using SetColorModeMatrixFn = int32_t (*)(bool, const int32_t *);

std::once_flag g_initOnce;
int32_t g_maxBacklight = -1;
std::mutex g_stateMu;
uint32_t g_supportedSeenMask = 0;
uint32_t g_setSeenMask = 0;
uint32_t g_xccLogCount = 0;
uint32_t g_gmpLogCount = 0;
int32_t g_lastDispParamMode = -1;
SetPccMatrixFn g_setPccFn = nullptr;
SetColorModeMatrixFn g_setColorModeFn = nullptr;

void AppendDiagLineLocked(const char *line)
{
    FILE *f = std::fopen("/data/local/tmp/marble-codec/displayengine.log", "a");
    if (f != nullptr) {
        std::fprintf(f, "%s\n", line);
        std::fclose(f);
    }
}

void WriteSysfsBestEffort(const char *path, const char *value)
{
    int saved = errno;
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        errno = saved;
        return;
    }
    const size_t len = std::strlen(value);
    ssize_t n;
    do {
        n = write(fd, value, len);
    } while (n < 0 && errno == EINTR);
    (void)n;
    close(fd);
    errno = saved;
}

void WriteDispParamIfChanged(int32_t mode, const char *value)
{
    {
        std::lock_guard<std::mutex> lk(g_stateMu);
        if (g_lastDispParamMode == mode) {
            return;
        }
        g_lastDispParamMode = mode;
    }
    WriteSysfsBestEffort(kDispParamPath, value);
}

void *ResolveComposerSymbol(const char *symbol)
{
    void *sym = dlsym(RTLD_DEFAULT, symbol);
    if (sym != nullptr) {
        return sym;
    }
    static const char *const kCandidates[] = {
        "libdisplay_composer_vdi_impl.z.so",
        "/vendor/lib64/libdisplay_composer_vdi_impl.z.so",
        "/compat/lib64/libdisplay_composer_vdi_impl.z.so",
    };
    for (const char *path : kCandidates) {
        void *handle = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
        if (handle != nullptr) {
            sym = dlsym(handle, symbol);
            if (sym != nullptr) {
                return sym;
            }
        }
    }
    return nullptr;
}

SetPccMatrixFn GetSetPccMatrixFn()
{
    {
        std::lock_guard<std::mutex> lk(g_stateMu);
        if (g_setPccFn != nullptr) {
            return g_setPccFn;
        }
    }
    auto fn = reinterpret_cast<SetPccMatrixFn>(ResolveComposerSymbol("MarbleSetPccMatrix"));
    if (fn != nullptr) {
        std::lock_guard<std::mutex> lk(g_stateMu);
        g_setPccFn = fn;
    }
    return fn;
}

SetColorModeMatrixFn GetSetColorModeMatrixFn()
{
    {
        std::lock_guard<std::mutex> lk(g_stateMu);
        if (g_setColorModeFn != nullptr) {
            return g_setColorModeFn;
        }
    }
    auto fn = reinterpret_cast<SetColorModeMatrixFn>(ResolveComposerSymbol("MarbleSetColorModeMatrix"));
    if (fn != nullptr) {
        std::lock_guard<std::mutex> lk(g_stateMu);
        g_setColorModeFn = fn;
    }
    return fn;
}

int32_t ReadMaxBacklightOnce()
{
    int fd = open("/sys/class/backlight/panel0-backlight/max_brightness", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    int32_t val = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            val = val * 10 + (buf[i] - '0');
        } else {
            break;
        }
    }
    return val > 0 ? val : -1;
}
} // namespace

DisplayEngineWrapperOH::DisplayEngineWrapperOH() : state_(nullptr)
{
    std::call_once(g_initOnce, []() {
        g_maxBacklight = ReadMaxBacklightOnce();
        char msg[96];
        std::snprintf(msg, sizeof(msg), "DISPLAYENGINE_MODULE_INIT max_bl=%d xcc=1 gmp=1", g_maxBacklight);
        std::lock_guard<std::mutex> lk(g_stateMu);
        AppendDiagLineLocked(msg);
    });
}

int32_t DisplayEngineWrapperOH::GetSupported(int32_t feature)
{
    const int32_t ret = (feature == kDeFeatureXcc || feature == kDeFeatureGmp) ? 1 : 0;
    std::lock_guard<std::mutex> lk(g_stateMu);
    uint32_t bit = (feature >= 0 && feature < 32) ? (1u << static_cast<uint32_t>(feature)) : (1u << 31);
    if ((g_supportedSeenMask & bit) == 0u) {
        g_supportedSeenMask |= bit;
        char msg[96];
        std::snprintf(msg, sizeof(msg), "DISPLAYENGINE_GET_SUPPORTED feature=%d ret=%d", feature, ret);
        AppendDiagLineLocked(msg);
    }
    return ret;
}

int32_t DisplayEngineWrapperOH::SetHardwareEffect(int32_t feature, int32_t type, const std::vector<uint8_t> &data)
{
    const uint32_t subType = static_cast<uint32_t>(type) & 0xffu;

    if (feature == kDeFeatureXcc) {
        SetPccMatrixFn setPcc = GetSetPccMatrixFn();
        int32_t rc = -1;
        int32_t diag[3] = {0, 0, 0};
        const char *action = "ignored";

        if (subType == 0u) {
            action = "disable";
            if (setPcc != nullptr) {
                rc = setPcc(false, nullptr);
            }
        } else if ((subType == 0x2du && data.size() >= 48u) ||
                   (subType == 0xcdu && data.size() >= 64u)) {
            action = "apply";
            int32_t xcc12[12] = {0};
            std::memcpy(xcc12, data.data(), sizeof(xcc12));
            diag[0] = xcc12[1];
            diag[1] = xcc12[6];
            diag[2] = xcc12[11];
            if (setPcc != nullptr) {
                rc = setPcc(true, xcc12);
            }
        }

        std::lock_guard<std::mutex> lk(g_stateMu);
        if (g_xccLogCount < 32u || (g_xccLogCount & 0x1fu) == 0u) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "DISPLAYENGINE_XCC action=%s type=0x%x sub=0x%x len=%zu diag=[%d,%d,%d] resolved=%d rc=%d",
                          action, static_cast<unsigned>(type), static_cast<unsigned>(subType),
                          data.size(), diag[0], diag[1], diag[2], setPcc != nullptr ? 1 : 0, rc);
            AppendDiagLineLocked(msg);
        }
        ++g_xccLogCount;
        return 0;
    }

    if (feature == kDeFeatureGmp) {
        SetColorModeMatrixFn setColorMode = GetSetColorModeMatrixFn();
        int32_t rc = -1;
        const char *action = "srgb";
        uint32_t head0 = 0;
        uint32_t head1 = 0;
        if (data.size() >= 4u) {
            std::memcpy(&head0, data.data(), 4u);
        }
        if (data.size() >= 8u) {
            std::memcpy(&head1, data.data() + 4u, 4u);
        }

        if (subType == 0xa1u) {
            // Huawei DisplayEngine UI-transition scene/metadata hint, not a gamut LUT.
            // Ignore so it never triggers redundant PCC/DRM commits during launcher animations.
            action = "hint_ignored";
            rc = 0;
        } else if (subType == 0u || (subType == 0xcdu && data.size() < 36u && head0 == 0u)) {
            action = "disable_vivid";
            if (setColorMode != nullptr) {
                rc = setColorMode(false, nullptr);
            }
            WriteDispParamIfChanged(0, "6 0");
        } else if (data.size() == 36u) {
            action = "matrix3x3";
            int32_t mat3x3[9] = {0};
            std::memcpy(mat3x3, data.data(), sizeof(mat3x3));
            if (setColorMode != nullptr) {
                rc = setColorMode(true, mat3x3);
            }
        } else if ((subType == 0x2du && data.size() >= 48u) ||
                   (subType == 0xcdu && data.size() >= 64u)) {
            action = "matrix12";
            int32_t xcc12[12] = {0};
            std::memcpy(xcc12, data.data(), sizeof(xcc12));
            const int32_t mat3x3[9] = {
                xcc12[1], xcc12[2], xcc12[3],
                xcc12[5], xcc12[6], xcc12[7],
                xcc12[9], xcc12[10], xcc12[11],
            };
            if (setColorMode != nullptr) {
                rc = setColorMode(true, mat3x3);
            }
        } else {
            action = "srgb_p3";
            if (setColorMode != nullptr) {
                rc = setColorMode(true, kSrgbInP3Q15);
            }
            WriteDispParamIfChanged(1, "6 1");
        }

        std::lock_guard<std::mutex> lk(g_stateMu);
        if (g_gmpLogCount < 32u || (g_gmpLogCount & 0x1fu) == 0u) {
            char msg[224];
            std::snprintf(msg, sizeof(msg),
                          "DISPLAYENGINE_GMP action=%s type=0x%x sub=0x%x len=%zu h0=0x%08x h1=0x%08x resolved=%d rc=%d",
                          action, static_cast<unsigned>(type), static_cast<unsigned>(subType),
                          data.size(), head0, head1, setColorMode != nullptr ? 1 : 0, rc);
            AppendDiagLineLocked(msg);
        }
        ++g_gmpLogCount;
        return 0;
    }

    std::lock_guard<std::mutex> lk(g_stateMu);
    uint32_t bit = (feature >= 0 && feature < 32) ? (1u << static_cast<uint32_t>(feature)) : (1u << 31);
    if ((g_setSeenMask & bit) == 0u) {
        g_setSeenMask |= bit;
        char msg[112];
        std::snprintf(msg, sizeof(msg), "DISPLAYENGINE_SET_EFFECT feature=%d type=0x%x len=%zu",
                      feature, static_cast<unsigned>(type), data.size());
        AppendDiagLineLocked(msg);
    }
    return 0;
}

int32_t DisplayEngineWrapperOH::GetHardwareEffect(int32_t feature, int32_t type, std::vector<uint8_t> &data)
{
    (void)feature;
    (void)type;
    (void)data;
    return -1;
}

int32_t DisplayEngineWrapperOH::GetHardwareEffectExt(int32_t feature, int32_t type, int32_t size,
                                                     std::vector<uint8_t> &data)
{
    (void)feature;
    (void)type;
    (void)size;
    (void)data;
    return -1;
}

int32_t DisplayEngineWrapperOH::RegisterCallback(
    const OHOS::sptr<OHOS::HDI::Displayengine::V1_0::IDisplayEngineCallback> &cb)
{
    (void)cb;
    return 0;
}
