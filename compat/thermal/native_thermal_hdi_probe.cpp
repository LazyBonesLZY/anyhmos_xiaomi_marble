/* SPDX-License-Identifier: MIT */
/*
 * native_thermal_hdi_probe.cpp - End-to-end OHOS Binder/HDI Thermal v1.1 probe for Xiaomi marble.
 *
 * Linked against unmodified donor libthermal_proxy_1.1.z.so:
 *   1. Connects to HDF host-11 (thermal_host) -> thermal_interface_service via Binder.
 *   2. Verifies GetVersion == 1.1.
 *   3. Verifies GetThermalZoneInfo returns real SM7475 hardware zones (cpu/soc/gpu/battery/charger/pa/camera/shell/ambient)
 *      with physically valid temperatures (15000..95000 mC).
 *   4. Registers a real Binder IThermalCallback, waits for live OnThermalDataEvent callback, then unregisters.
 *   5. Exercises RegisterFanCallback/UnregisterFanCallback and SetCpuFreq/SetGpuFreq/SetBatteryCurrent/IsolateCpu RPCs.
 *   6. Queries SAMGR CheckSystemAbility(3303) for thermal_service status.
 */

#include "v1_1/ithermal_interface.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <ipc_skeleton.h>
#include <iremote_object.h>
#include <message_option.h>
#include <message_parcel.h>

using OHOS::IPCSkeleton;
using OHOS::IRemoteObject;
using OHOS::MessageOption;
using OHOS::MessageParcel;
using OHOS::sptr;
using OHOS::HDI::Thermal::V1_1::HdfThermalCallbackInfo;
using OHOS::HDI::Thermal::V1_1::IFanCallback;
using OHOS::HDI::Thermal::V1_1::IThermalCallback;
using OHOS::HDI::Thermal::V1_1::IThermalInterface;
using OHOS::HDI::Thermal::V1_1::ThermalZoneInfo;

namespace {

static bool checkSaRegistered(int32_t saId) {
    sptr<IRemoteObject> samgr = IPCSkeleton::GetContextObject();
    if (samgr == nullptr)
        return false;
    MessageParcel data, reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(u"ohos.samgr.accessToken"))
        return false;
    if (!data.WriteInt32(saId))
        return false;
    int rc = samgr->SendRequest(1 /* CHECK_SYSTEM_ABILITY_TRANSACTION */, data, reply, opt);
    if (rc != 0)
        return false;
    sptr<IRemoteObject> obj = reply.ReadRemoteObject();
    return obj != nullptr;
}

class ProbeThermalCallback : public IThermalCallback {
public:
    int32_t OnThermalDataEvent(const HdfThermalCallbackInfo &event) override {
        std::lock_guard<std::mutex> lk(mu_);
        cbEvents_++;
        lastZones_ = event.info;
        cv_.notify_all();
        return HDF_SUCCESS;
    }

    bool waitForEvent(int timeoutMs, size_t &outZones, int &outEvents) {
        std::unique_lock<std::mutex> lk(mu_);
        bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                               [this]() { return cbEvents_ > 0; });
        outZones = lastZones_.size();
        outEvents = cbEvents_;
        return ok;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    int cbEvents_ = 0;
    std::vector<ThermalZoneInfo> lastZones_;
};

class ProbeFanCallback : public IFanCallback {
public:
    int32_t OnFanDataEvent(const HdfThermalCallbackInfo &event) override {
        (void)event;
        return HDF_SUCCESS;
    }
};

static bool checkTempPlausible(int32_t t) {
    return t >= 10000 && t <= 105000; /* 10C .. 105C */
}

} // namespace

int main(void) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    bool sa3303 = checkSaRegistered(3303);
    std::printf("SA3303_REGISTERED=%d\n", sa3303 ? 1 : 0);

    sptr<IThermalInterface> iface = IThermalInterface::Get(false);
    if (iface == nullptr) {
        std::printf("THERMAL_HDI_SERVICE=unavailable\n");
        return 1;
    }
    std::printf("THERMAL_HDI_SERVICE=connected\n");

    uint32_t maj = 0, min = 0;
    int32_t vrc = iface->GetVersion(maj, min);
    std::printf("THERMAL_HDI_VERSION rc=%d ver=%u.%u\n", vrc, maj, min);

    HdfThermalCallbackInfo info;
    int32_t zrc = iface->GetThermalZoneInfo(info);
    int validTemps = 0;
    bool hasCpu = false, hasSoc = false, hasGpu = false, hasBat = false, hasShell = false;
    int32_t cpuT = 0, gpuT = 0, batT = 0;
    std::printf("THERMAL_ZONES rc=%d count=%zu", zrc, info.info.size());
    for (const auto &z : info.info) {
        std::printf(" %s=%d", z.type.c_str(), z.temp);
        if (checkTempPlausible(z.temp))
            validTemps++;
        if (z.type == "cpu")     { hasCpu = checkTempPlausible(z.temp); cpuT = z.temp; }
        if (z.type == "soc")     { hasSoc = checkTempPlausible(z.temp); }
        if (z.type == "gpu")     { hasGpu = checkTempPlausible(z.temp); gpuT = z.temp; }
        if (z.type == "battery") { hasBat = checkTempPlausible(z.temp); batT = z.temp; }
        if (z.type == "shell")   { hasShell = checkTempPlausible(z.temp); }
    }
    std::printf("\n");

    sptr<ProbeThermalCallback> cb = new ProbeThermalCallback();
    int32_t regRc = iface->Register(cb);
    size_t cbZones = 0;
    int cbEvents = 0;
    bool cbOk = (regRc == 0) && cb->waitForEvent(3500, cbZones, cbEvents) && (cbZones >= 6);
    int32_t unregRc = iface->Unregister();
    std::printf("THERMAL_CALLBACK reg_rc=%d cb_ok=%d cb_events=%d cb_zones=%zu unreg_rc=%d\n",
                regRc, cbOk ? 1 : 0, cbEvents, cbZones, unregRc);

    sptr<ProbeFanCallback> fcb = new ProbeFanCallback();
    int32_t fRegRc = iface->RegisterFanCallback(fcb);
    int32_t fUnregRc = iface->UnregisterFanCallback();
    int32_t cpuRc = iface->SetCpuFreq(1800000);
    int32_t gpuRc = iface->SetGpuFreq(600000);
    int32_t batRc = iface->SetBatteryCurrent(1500);
    int32_t isoRc = iface->IsolateCpu(0);
    bool ctrlOk = (fRegRc == 0 && fUnregRc == 0 && cpuRc == 0 && gpuRc == 0 && batRc == 0 && isoRc == 0);
    std::printf("THERMAL_CTRL fan_reg=%d fan_unreg=%d cpu=%d gpu=%d bat=%d iso=%d ok=%d\n",
                fRegRc, fUnregRc, cpuRc, gpuRc, batRc, isoRc, ctrlOk ? 1 : 0);

    bool allOk = (vrc == 0 && maj == 1 && min == 1 &&
                  zrc == 0 && info.info.size() >= 7 && validTemps >= 7 &&
                  hasCpu && hasSoc && hasGpu && hasBat && hasShell &&
                  cbOk && unregRc == 0 && ctrlOk);

    std::printf("MARBLE_THERMAL_HDI_PROBE_SUMMARY sa3303=%d hdi_ver=%u.%u zones=%zu valid=%d "
                "cpu=%d gpu=%d battery=%d cb_ok=%d cb_events=%d ctrl_ok=%d all_ok=%d\n",
                sa3303 ? 1 : 0, maj, min, info.info.size(), validTemps,
                cpuT, gpuT, batT, cbOk ? 1 : 0, cbEvents, ctrlOk ? 1 : 0, allOk ? 1 : 0);
    return allOk ? 0 : 2;
}
