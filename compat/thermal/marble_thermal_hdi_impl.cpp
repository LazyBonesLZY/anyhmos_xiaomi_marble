/* SPDX-License-Identifier: MIT */
/*
 * marble_thermal_hdi_impl.cpp - Clean-room SM7475 (Xiaomi marble) OHOS Thermal HDI v1.1.
 *
 * SONAME: libthermal_interface_service_1.1.z.so (bind-mounted over donor stub).
 * Loaded by unmodified donor libthermal_driver.z.so inside HDF host-11 (thermal_host)
 * via ThermalInterfaceImplGetInstance().
 *
 * Reads live SM7475 hardware thermal zones under /sys/class/thermal/thermal_zone*
 * (Kryo CPU clusters/cores, Adreno 725 GPU, AOSS/DDR/NSP/Video SoC, 5G Modem MDMSS,
 * Spectra Camera ISP, Li-ion Battery, and USB/PMIC charger) and aggregates them into
 * canonical OpenHarmony ThermalZoneInfo zones ("cpu", "soc", "gpu", "battery",
 * "charger", "pa", "camera", "shell", "ambient") in millidegrees Celsius.
 *
 * Zero audio/modem/persist/PMIC side effects: strictly read-only sysfs telemetry.
 */

#include "v1_1/ithermal_interface.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace OHOS {
namespace HDI {
namespace Thermal {
namespace V1_1 {

namespace {

static bool readTrimmedFile(const std::string &path, std::string &out) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[128];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    size_t len = static_cast<size_t>(n);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                       buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        len--;
    out.assign(buf, len);
    return !out.empty();
}

static bool readIntFile(const std::string &path, int32_t &val) {
    std::string s;
    if (!readTrimmedFile(path, s))
        return false;
    char *endp = nullptr;
    errno = 0;
    long v = std::strtol(s.c_str(), &endp, 10);
    if (errno != 0 || endp == s.c_str() || *endp != '\0')
        return false;
    if (v < INT32_MIN || v > INT32_MAX)
        return false;
    val = static_cast<int32_t>(v);
    return true;
}

static bool startsWith(const std::string &s, const char *pfx) {
    size_t n = std::strlen(pfx);
    return s.size() >= n && std::memcmp(s.data(), pfx, n) == 0;
}

struct AggSlot {
    bool seen = false;
    int32_t maxTemp = INT32_MIN;
    void add(int32_t t) {
        /* Reject uninitialized/faulty readings outside [-20C .. +135C] */
        if (t < -20000 || t > 135000)
            return;
        if (!seen || t > maxTemp) {
            maxTemp = t;
            seen = true;
        }
    }
};

static void collectSm7475ThermalZones(const std::string &sysRoot,
                                      std::vector<ThermalZoneInfo> &outZones) {
    outZones.clear();
    AggSlot cpu, soc, gpu, battery, charger, pa, camera;

    std::string thermDir = sysRoot + "/class/thermal";
    DIR *dir = ::opendir(thermDir.c_str());
    if (dir != nullptr) {
        struct dirent *de = nullptr;
        while ((de = ::readdir(dir)) != nullptr) {
            if (std::strncmp(de->d_name, "thermal_zone", 12) != 0)
                continue;
            std::string zdir = thermDir + "/" + de->d_name;
            std::string ztype;
            int32_t temp = 0;
            if (!readTrimmedFile(zdir + "/type", ztype))
                continue;
            if (!readIntFile(zdir + "/temp", temp))
                continue;

            if (startsWith(ztype, "cpuss-") || startsWith(ztype, "cpu-0-") ||
                startsWith(ztype, "cpu-1-") || ztype == "cpu_therm") {
                cpu.add(temp);
            } else if (startsWith(ztype, "gpuss-")) {
                gpu.add(temp);
            } else if (startsWith(ztype, "aoss-") || startsWith(ztype, "nspss-") ||
                       ztype == "ddr" || ztype == "video") {
                soc.add(temp);
            } else if (ztype == "battery") {
                battery.add(temp);
            } else if (ztype == "usb" || (startsWith(ztype, "pm8350b") && ztype.find("temp") != std::string::npos)) {
                charger.add(temp);
            } else if (startsWith(ztype, "mdmss-") || ztype == "pa_therm" || ztype == "pa-therm") {
                pa.add(temp);
            } else if (startsWith(ztype, "camera-")) {
                camera.add(temp);
            }
        }
        ::closedir(dir);
    }

    /* Fallback battery temp from power_supply (tenths of deg C -> millideg C) */
    if (!battery.seen) {
        int32_t batDeci = 0;
        if (readIntFile(sysRoot + "/class/power_supply/battery/temp", batDeci))
            battery.add(batDeci * 100);
    }

    /* If soc wasn't populated separately, fall back to cpu peak */
    if (!soc.seen && cpu.seen)
        soc.add(cpu.maxTemp);

    auto push = [&](const char *name, const AggSlot &s) {
        if (s.seen)
            outZones.push_back(ThermalZoneInfo { name, s.maxTemp });
    };
    push("cpu", cpu);
    push("soc", soc);
    push("gpu", gpu);
    push("battery", battery);
    push("charger", charger);
    push("pa", pa);
    push("camera", camera);

    /* Derived standard OHOS skin ("shell") and ambient ("ambient") zones */
    if (battery.seen || charger.seen) {
        int32_t bat = battery.seen ? battery.maxTemp : charger.maxTemp;
        int32_t chg = charger.seen ? charger.maxTemp : battery.maxTemp;
        int32_t shell = ((bat + chg) / 2) + 1500;
        if (soc.seen && shell > soc.maxTemp)
            shell = soc.maxTemp;
        outZones.push_back(ThermalZoneInfo { "shell", shell });
        outZones.push_back(ThermalZoneInfo { "ambient", std::min(bat, chg) });
    }
}

} // namespace

class ThermalInterfaceImpl : public IThermalInterface {
public:
    ThermalInterfaceImpl() = default;
    ~ThermalInterfaceImpl() override {
        stopPollThread();
    }

    int32_t SetCpuFreq(int32_t freq) override {
        if (freq < 0)
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(mu_);
        lastCpuFreq_ = freq;
        return HDF_SUCCESS;
    }

    int32_t SetGpuFreq(int32_t freq) override {
        if (freq < 0)
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(mu_);
        lastGpuFreq_ = freq;
        return HDF_SUCCESS;
    }

    int32_t SetBatteryCurrent(int32_t current) override {
        if (current < 0)
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(mu_);
        lastBatCurrent_ = current;
        return HDF_SUCCESS;
    }

    int32_t GetThermalZoneInfo(HdfThermalCallbackInfo &event) override {
        collectSm7475ThermalZones("/sys", event.info);
        return event.info.empty() ? HDF_FAILURE : HDF_SUCCESS;
    }

    int32_t IsolateCpu(int32_t num) override {
        if (num < 0 || num > 8)
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(mu_);
        lastIsolateCpu_ = num;
        return HDF_SUCCESS;
    }

    int32_t Register(const sptr<IThermalCallback> &callbackObj) override {
        if (callbackObj == nullptr)
            return HDF_ERR_INVALID_PARAM;
        {
            std::lock_guard<std::mutex> lk(mu_);
            thermalCb_ = callbackObj;
        }
        startPollThread();
        cv_.notify_all();
        return HDF_SUCCESS;
    }

    int32_t Unregister() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            thermalCb_ = nullptr;
        }
        stopPollThread();
        return HDF_SUCCESS;
    }

    int32_t RegisterFanCallback(const sptr<IFanCallback> &callbackObj) override {
        if (callbackObj == nullptr)
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(mu_);
        fanCb_ = callbackObj;
        return HDF_SUCCESS;
    }

    int32_t UnregisterFanCallback() override {
        std::lock_guard<std::mutex> lk(mu_);
        fanCb_ = nullptr;
        return HDF_SUCCESS;
    }

private:
    void startPollThread() {
        std::lock_guard<std::mutex> lk(pollMu_);
        if (pollRunning_)
            return;
        pollStop_ = false;
        pollRunning_ = true;
        pollThread_ = std::thread([this]() { pollLoop(); });
    }

    void stopPollThread() {
        std::thread toJoin;
        {
            std::lock_guard<std::mutex> lk(pollMu_);
            if (!pollRunning_)
                return;
            pollStop_ = true;
            cv_.notify_all();
            toJoin = std::move(pollThread_);
            pollRunning_ = false;
        }
        if (toJoin.joinable())
            toJoin.join();
    }

    void pollLoop() {
        while (true) {
            sptr<IThermalCallback> cb;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb = thermalCb_;
            }
            if (cb != nullptr) {
                HdfThermalCallbackInfo ev;
                collectSm7475ThermalZones("/sys", ev.info);
                if (!ev.info.empty())
                    (void)cb->OnThermalDataEvent(ev);
            }
            std::unique_lock<std::mutex> plk(pollMu_);
            if (cv_.wait_for(plk, std::chrono::milliseconds(2000), [this]() { return pollStop_; }))
                break;
        }
    }

    std::mutex mu_;
    sptr<IThermalCallback> thermalCb_ = nullptr;
    sptr<IFanCallback> fanCb_ = nullptr;
    int32_t lastCpuFreq_ = 0;
    int32_t lastGpuFreq_ = 0;
    int32_t lastBatCurrent_ = 0;
    int32_t lastIsolateCpu_ = 0;

    std::mutex pollMu_;
    std::condition_variable cv_;
    bool pollRunning_ = false;
    bool pollStop_ = false;
    std::thread pollThread_;
};

extern "C" IThermalInterface *ThermalInterfaceImplGetInstance(void) {
    return new (std::nothrow) ThermalInterfaceImpl();
}

/* Host-testable C-linkage scanner helper for offline unit verification */
extern "C" int marble_thermal_scan_summary(const char *sys_root, char *out_buf, size_t out_cap) {
    if (!sys_root || !out_buf || out_cap == 0)
        return -1;
    std::vector<ThermalZoneInfo> zones;
    collectSm7475ThermalZones(sys_root, zones);
    size_t pos = 0;
    for (size_t i = 0; i < zones.size(); ++i) {
        int n = std::snprintf(out_buf + pos, out_cap - pos, "%s%s=%d",
                              (i > 0) ? " " : "", zones[i].type.c_str(), zones[i].temp);
        if (n < 0 || static_cast<size_t>(n) >= out_cap - pos)
            break;
        pos += static_cast<size_t>(n);
    }
    return static_cast<int>(zones.size());
}

} // namespace V1_1
} // namespace Thermal
} // namespace HDI
} // namespace OHOS
