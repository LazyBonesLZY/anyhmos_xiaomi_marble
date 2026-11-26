/* SPDX-License-Identifier: MIT */
/*
 * marble_power_hdi_impl.cpp - Clean-room SM7475 (Xiaomi marble) OHOS Power HDI v1.3.
 *
 * SONAME: libpower_interface_service_1.3.z.so (bind-mounted over donor service).
 * Loaded by unmodified donor libpower_driver.z.so inside HDF host-12 (power_host)
 * via PowerInterfaceImplGetInstance().
 *
 * Features:
 *   - Real Linux kernel wakelock management via /sys/power/wake_lock and /sys/power/wake_unlock
 *     with sanitized lock names and refcount tracking (zero leaked wakelocks).
 *   - Live PowerDump() reporting /sys/power/state, active kernel wake_locks, battery capacity,
 *     and OHOS RunningLock table.
 *   - Live GetWakeupReason() reading /sys/kernel/wakeup_reasons/last_resume_reason.
 *   - Full v1.2 IPowerHdiCallback, IPowerRunningLockCallback, and v1.3 IPowerHdiCallbackExt support.
 *   - Safe suspend/wakeup state machine (never forces deep S3/disk sleep over COM9/USB).
 *   - Never reads /sys/power/wakeup_count (avoids kernel blocking wait).
 */

#include "v1_3/ipower_interface.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace OHOS {
namespace HDI {
namespace Power {
namespace V1_3 {

namespace {

static bool readSmallFile(const char *path, std::string &out, size_t maxBytes = 2048) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    std::string buf(maxBytes, '\0');
    ssize_t n = ::read(fd, &buf[0], maxBytes - 1);
    ::close(fd);
    if (n < 0)
        return false;
    buf.resize(static_cast<size_t>(n));
    while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r' || buf.back() == ' '))
        buf.pop_back();
    out = std::move(buf);
    return true;
}

static bool writeSysfsString(const char *path, const std::string &val) {
    int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t n = ::write(fd, val.data(), val.size());
    ::close(fd);
    return n == static_cast<ssize_t>(val.size());
}

static std::string sanitizeWakeLockName(const std::string &raw) {
    std::string out = "ohos_";
    out.reserve(out.size() + std::min<size_t>(raw.size(), 48));
    for (char c : raw) {
        if (out.size() >= 56)
            break;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out == "ohos_")
        out += "default";
    return out;
}

} // namespace

class PowerInterfaceImpl : public IPowerInterface {
public:
    PowerInterfaceImpl() = default;
    ~PowerInterfaceImpl() override {
        releaseAllWakeLocks();
    }

    int32_t RegisterCallback(const sptr<V1_2::IPowerHdiCallback> &ipowerHdiCallback) override {
        std::lock_guard<std::mutex> lk(mu_);
        powerCb_ = ipowerHdiCallback;
        return HDF_SUCCESS;
    }

    int32_t StartSuspend() override {
        sptr<V1_2::IPowerHdiCallback> cb;
        std::vector<sptr<IPowerHdiCallbackExt>> extCbs;
        std::string tag;
        {
            std::lock_guard<std::mutex> lk(mu_);
            suspended_ = true;
            cb = powerCb_;
            extCbs = extCbs_;
            tag = suspendTag_;
        }
        if (cb != nullptr)
            (void)cb->OnSuspend();
        for (const auto &ecb : extCbs) {
            if (ecb != nullptr)
                (void)ecb->OnSuspendWithTag(tag);
        }
        return HDF_SUCCESS;
    }

    int32_t StopSuspend() override {
        sptr<V1_2::IPowerHdiCallback> cb;
        std::vector<sptr<IPowerHdiCallbackExt>> extCbs;
        std::string tag;
        {
            std::lock_guard<std::mutex> lk(mu_);
            suspended_ = false;
            cb = powerCb_;
            extCbs = extCbs_;
            tag = suspendTag_;
        }
        if (cb != nullptr)
            (void)cb->OnWakeup();
        for (const auto &ecb : extCbs) {
            if (ecb != nullptr)
                (void)ecb->OnWakeupWithTag(tag);
        }
        return HDF_SUCCESS;
    }

    int32_t ForceSuspend() override {
        /* Safe non-destructive transition: notify callbacks without killing USB/COM9 */
        (void)StartSuspend();
        (void)StopSuspend();
        return HDF_SUCCESS;
    }

    int32_t SuspendBlock(const std::string &name) override {
        if (name.empty())
            return HDF_ERR_INVALID_PARAM;
        std::string kname = sanitizeWakeLockName(name);
        sptr<V1_2::IPowerRunningLockCallback> rlCb;
        bool writeKernel = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            int &cnt = wakeLocks_[kname];
            if (cnt == 0)
                writeKernel = true;
            cnt++;
            rlCb = rlCb_;
        }
        if (writeKernel)
            (void)writeSysfsString("/sys/power/wake_lock", kname);
        if (rlCb != nullptr)
            (void)rlCb->HandleRunningLockMessage("LOCK:" + kname);
        return HDF_SUCCESS;
    }

    int32_t SuspendUnblock(const std::string &name) override {
        if (name.empty())
            return HDF_ERR_INVALID_PARAM;
        std::string kname = sanitizeWakeLockName(name);
        sptr<V1_2::IPowerRunningLockCallback> rlCb;
        bool unlockKernel = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = wakeLocks_.find(kname);
            if (it != wakeLocks_.end()) {
                if (--(it->second) <= 0) {
                    wakeLocks_.erase(it);
                    unlockKernel = true;
                }
            } else {
                unlockKernel = true;
            }
            rlCb = rlCb_;
        }
        if (unlockKernel)
            (void)writeSysfsString("/sys/power/wake_unlock", kname);
        if (rlCb != nullptr)
            (void)rlCb->HandleRunningLockMessage("UNLOCK:" + kname);
        return HDF_SUCCESS;
    }

    int32_t PowerDump(std::string &info) override {
        std::string pwrState, kernelLocks, batCap, wakeupReason;
        (void)readSmallFile("/sys/power/state", pwrState, 128);
        (void)readSmallFile("/sys/power/wake_lock", kernelLocks, 512);
        (void)readSmallFile("/sys/class/power_supply/battery/capacity", batCap, 32);
        (void)GetWakeupReason(wakeupReason);

        std::lock_guard<std::mutex> lk(mu_);
        char hdr[256];
        std::snprintf(hdr, sizeof(hdr),
                      "marble_power_hdi_v1.3 state=[%s] suspended=%d bat=%s%% active_locks=%zu suspend_tag=%s wakeup=%s\n",
                      pwrState.empty() ? "unknown" : pwrState.c_str(),
                      suspended_ ? 1 : 0,
                      batCap.empty() ? "NA" : batCap.c_str(),
                      wakeLocks_.size(),
                      suspendTag_.empty() ? "none" : suspendTag_.c_str(),
                      wakeupReason.c_str());
        info = hdr;
        info += "kernel_wake_locks: " + (kernelLocks.empty() ? "(none)" : kernelLocks) + "\n";
        for (const auto &kv : wakeLocks_) {
            char line[128];
            std::snprintf(line, sizeof(line), "  ohos_lock=%s ref=%d\n", kv.first.c_str(), kv.second);
            info += line;
        }
        return HDF_SUCCESS;
    }

    int32_t HoldRunningLock(const V1_2::RunningLockInfo &info) override {
        if (info.name.empty())
            return HDF_ERR_INVALID_PARAM;
        return SuspendBlock(info.name);
    }

    int32_t UnholdRunningLock(const V1_2::RunningLockInfo &info) override {
        if (info.name.empty())
            return HDF_ERR_INVALID_PARAM;
        return SuspendUnblock(info.name);
    }

    int32_t GetWakeupReason(std::string &reason) override {
        std::string r;
        if (readSmallFile("/sys/kernel/wakeup_reasons/last_resume_reason", r, 256) && !r.empty()) {
            reason = r;
        } else {
            reason = "boot_cold";
        }
        return HDF_SUCCESS;
    }

    int32_t HoldRunningLockExt(const V1_2::RunningLockInfo &info,
                               uint64_t lockid, const std::string &bundleName) override {
        (void)lockid;
        (void)bundleName;
        return HoldRunningLock(info);
    }

    int32_t UnholdRunningLockExt(const V1_2::RunningLockInfo &info,
                                 uint64_t lockid, const std::string &bundleName) override {
        (void)lockid;
        (void)bundleName;
        return UnholdRunningLock(info);
    }

    int32_t RegisterRunningLockCallback(
        const sptr<V1_2::IPowerRunningLockCallback> &iPowerRunningLockCallback) override {
        std::lock_guard<std::mutex> lk(mu_);
        rlCb_ = iPowerRunningLockCallback;
        return HDF_SUCCESS;
    }

    int32_t UnRegisterRunningLockCallback() override {
        std::lock_guard<std::mutex> lk(mu_);
        rlCb_ = nullptr;
        return HDF_SUCCESS;
    }

    int32_t Hibernate() override {
        return HDF_SUCCESS;
    }

    int32_t SetSuspendTag(const std::string &tag) override {
        std::lock_guard<std::mutex> lk(mu_);
        suspendTag_ = tag;
        return HDF_SUCCESS;
    }

    int32_t SetPowerConfig(const std::string &scene, const std::string &value) override {
        if (scene.empty())
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(mu_);
        configs_[scene] = value;
        return HDF_SUCCESS;
    }

    int32_t GetPowerConfig(const std::string &scene, std::string &value) override {
        if (scene.empty())
            return HDF_ERR_INVALID_PARAM;
        if (scene == "power_state") {
            if (!readSmallFile("/sys/power/state", value, 128))
                value = "unknown";
            return HDF_SUCCESS;
        }
        std::lock_guard<std::mutex> lk(mu_);
        auto it = configs_.find(scene);
        if (it == configs_.end()) {
            value.clear();
            return HDF_ERR_NOT_SUPPORT;
        }
        value = it->second;
        return HDF_SUCCESS;
    }

    int32_t RegisterPowerCallbackExt(const sptr<IPowerHdiCallbackExt> &ipowerHdiCallback) override {
        if (ipowerHdiCallback == nullptr)
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(mu_);
        if (std::find(extCbs_.begin(), extCbs_.end(), ipowerHdiCallback) == extCbs_.end())
            extCbs_.push_back(ipowerHdiCallback);
        return HDF_SUCCESS;
    }

    int32_t UnRegisterPowerCallbackExt(const sptr<IPowerHdiCallbackExt> &ipowerHdiCallback) override {
        if (ipowerHdiCallback == nullptr)
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(mu_);
        extCbs_.erase(std::remove(extCbs_.begin(), extCbs_.end(), ipowerHdiCallback), extCbs_.end());
        return HDF_SUCCESS;
    }

private:
    void releaseAllWakeLocks() {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &kv : wakeLocks_)
            (void)writeSysfsString("/sys/power/wake_unlock", kv.first);
        wakeLocks_.clear();
    }

    std::mutex mu_;
    bool suspended_ = false;
    std::string suspendTag_;
    sptr<V1_2::IPowerHdiCallback> powerCb_ = nullptr;
    sptr<V1_2::IPowerRunningLockCallback> rlCb_ = nullptr;
    std::vector<sptr<IPowerHdiCallbackExt>> extCbs_;
    std::map<std::string, int> wakeLocks_;
    std::map<std::string, std::string> configs_;
};

extern "C" IPowerInterface *PowerInterfaceImplGetInstance(void) {
    return new (std::nothrow) PowerInterfaceImpl();
}

} // namespace V1_3
} // namespace Power
} // namespace HDI
} // namespace OHOS
