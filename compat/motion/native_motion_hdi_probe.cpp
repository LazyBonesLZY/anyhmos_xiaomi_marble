/* SPDX-License-Identifier: MIT */
/*
 * native_motion_hdi_probe.cpp - End-to-end OHOS Binder/HDI Motion v1.1 probe for Xiaomi marble.
 *
 * Linked against unmodified donor libmotion_proxy_1.1.z.so + libmotion_proxy_1.0.z.so:
 *   1. Checks SA 2901 (motion_service) registration in SAMGR.
 *   2. Connects to HDF host-18 (motion_host) -> motion_interface_service via Binder.
 *   3. Verifies GetVersion == 1.1.
 *   4. Exercises Register(cb), EnableMotion(0), SetMotionConfig(0, {}), DisableMotion(0), Unregister(cb).
 *   5. Prints MARBLE_MOTION_HDI_PROBE_SUMMARY sa2901=... hdi_ver=1.1 reg_ok=1 en_ok=1 dis_ok=1 all_ok=1.
 */

#include "v1_1/imotion_interface.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
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
using OHOS::HDI::Motion::V1_0::HdfMotionEvent;
using OHOS::HDI::Motion::V1_0::HDF_MOTION_TYPE_PICKUP;
using OHOS::HDI::Motion::V1_0::IMotionCallback;
using OHOS::HDI::Motion::V1_1::IMotionInterface;

namespace {

static bool checkSaRegistered(int32_t saId) {
    sptr<IRemoteObject> samgr = IPCSkeleton::GetContextObject();
    if (samgr == nullptr) {
        return false;
    }
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(u"ohos.samgr.accessToken")) {
        return false;
    }
    if (!data.WriteInt32(saId)) {
        return false;
    }
    int rc = samgr->SendRequest(1 /* CHECK_SYSTEM_ABILITY_TRANSACTION */, data, reply, opt);
    if (rc != 0) {
        return false;
    }
    return reply.ReadRemoteObject() != nullptr;
}

class ProbeMotionCallback : public IMotionCallback {
public:
    int32_t OnDataEvent(const HdfMotionEvent &event) override {
        events_.fetch_add(1, std::memory_order_relaxed);
        lastMotion_.store(event.motion, std::memory_order_relaxed);
        return HDF_SUCCESS;
    }

    int eventCount() const { return events_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> events_{0};
    std::atomic<int32_t> lastMotion_{-1};
};

} // namespace

int main(void) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const bool sa2901 = checkSaRegistered(2901);
    std::printf("SA2901_REGISTERED=%d\n", sa2901 ? 1 : 0);

    sptr<IMotionInterface> iface = IMotionInterface::Get(false);
    if (iface == nullptr) {
        std::printf("MOTION_HDI_SERVICE=unavailable\n");
        std::printf("MARBLE_MOTION_HDI_PROBE_SUMMARY sa2901=%d hdi_ver=0.0 reg_ok=0 en_ok=0 dis_ok=0 all_ok=0\n",
                    sa2901 ? 1 : 0);
        return 1;
    }
    std::printf("MOTION_HDI_SERVICE=connected\n");

    uint32_t maj = 0;
    uint32_t min = 0;
    const int32_t vrc = iface->GetVersion(maj, min);
    std::printf("MOTION_HDI_VERSION rc=%d ver=%u.%u\n", vrc, maj, min);

    sptr<ProbeMotionCallback> cb = new ProbeMotionCallback();
    const int32_t regRc = iface->Register(cb);
    const int32_t enRc = iface->EnableMotion(HDF_MOTION_TYPE_PICKUP);
    std::vector<uint8_t> cfgBytes;
    const int32_t cfgRc = iface->SetMotionConfig(HDF_MOTION_TYPE_PICKUP, cfgBytes);
    const int32_t noopRc = iface->EnableMotion(1 /* HDF_MOTION_TYPE_FLIP */);
    const int32_t disRc = iface->DisableMotion(HDF_MOTION_TYPE_PICKUP);
    const int32_t unregRc = iface->Unregister(cb);

    std::printf("MOTION_HDI_CALLS reg=%d en=%d cfg=%d noop=%d dis=%d unreg=%d cb_events=%d\n",
                regRc, enRc, cfgRc, noopRc, disRc, unregRc, cb->eventCount());

    const bool verOk = (vrc == 0 && maj == 1 && min == 1);
    const bool regOk = (regRc == 0 && unregRc == 0);
    const bool enOk = (enRc == 0 && cfgRc == 0 && noopRc == 0);
    const bool disOk = (disRc == 0);
    const bool allOk = (verOk && regOk && enOk && disOk);

    std::printf("MARBLE_MOTION_HDI_PROBE_SUMMARY sa2901=%d hdi_ver=%u.%u reg_ok=%d en_ok=%d dis_ok=%d all_ok=%d\n",
                sa2901 ? 1 : 0, maj, min, regOk ? 1 : 0, enOk ? 1 : 0, disOk ? 1 : 0, allOk ? 1 : 0);
    return allOk ? 0 : 2;
}
