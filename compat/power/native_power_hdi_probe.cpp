/* SPDX-License-Identifier: MIT */
/*
 * native_power_hdi_probe.cpp - End-to-end OHOS Binder/HDI Power v1.3 probe for Xiaomi marble.
 *
 * Linked against unmodified donor libpower_proxy_1.3.z.so & libpower_proxy_1.2.z.so:
 *   1. Connects to HDF host-12 (power_host) -> power_interface_service via Binder.
 *   2. Verifies GetVersion == 1.3.
 *   3. Verifies real Linux kernel wakelock HoldRunningLock -> PowerDump shows lock in /sys/power/wake_lock
 *      -> UnholdRunningLock cleanly removes it (zero leaked wakelocks!).
 *   4. Verifies v1.2 IPowerHdiCallback (OnSuspend/OnWakeup), IPowerRunningLockCallback, and
 *      v1.3 IPowerHdiCallbackExt (OnSuspendWithTag/OnWakeupWithTag) over real Binder callbacks!
 *   5. Verifies GetWakeupReason, SetPowerConfig/GetPowerConfig, and SA 3301 (powermgr) registration.
 */

#include "v1_3/ipower_interface.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <string>

#include <ipc_skeleton.h>
#include <iremote_object.h>
#include <message_option.h>
#include <message_parcel.h>

using OHOS::IPCSkeleton;
using OHOS::IRemoteObject;
using OHOS::MessageOption;
using OHOS::MessageParcel;
using OHOS::sptr;
using OHOS::HDI::Power::V1_2::IPowerHdiCallback;
using OHOS::HDI::Power::V1_2::IPowerRunningLockCallback;
using OHOS::HDI::Power::V1_2::RunningLockInfo;
using OHOS::HDI::Power::V1_2::RUNNINGLOCK_BACKGROUND_TASK;
using OHOS::HDI::Power::V1_3::IPowerHdiCallbackExt;
using OHOS::HDI::Power::V1_3::IPowerInterface;

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
    return reply.ReadRemoteObject() != nullptr;
}

class ProbePowerCb : public IPowerHdiCallback {
public:
    int32_t OnSuspend() override {
        suspendCnt.fetch_add(1);
        return HDF_SUCCESS;
    }
    int32_t OnWakeup() override {
        wakeupCnt.fetch_add(1);
        return HDF_SUCCESS;
    }
    std::atomic<int> suspendCnt {0};
    std::atomic<int> wakeupCnt {0};
};

class ProbePowerCbExt : public IPowerHdiCallbackExt {
public:
    int32_t OnSuspendWithTag(const std::string &tag) override {
        lastSuspendTag = tag;
        suspendCnt.fetch_add(1);
        return HDF_SUCCESS;
    }
    int32_t OnWakeupWithTag(const std::string &tag) override {
        lastWakeupTag = tag;
        wakeupCnt.fetch_add(1);
        return HDF_SUCCESS;
    }
    std::atomic<int> suspendCnt {0};
    std::atomic<int> wakeupCnt {0};
    std::string lastSuspendTag;
    std::string lastWakeupTag;
};

class ProbeRunningLockCb : public IPowerRunningLockCallback {
public:
    int32_t HandleRunningLockMessage(const std::string &message) override {
        msgCnt.fetch_add(1);
        lastMsg = message;
        return HDF_SUCCESS;
    }
    std::atomic<int> msgCnt {0};
    std::string lastMsg;
};

} // namespace

int main(void) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    bool sa3301 = checkSaRegistered(3301);
    std::printf("SA3301_REGISTERED=%d\n", sa3301 ? 1 : 0);

    sptr<IPowerInterface> iface = IPowerInterface::Get(false);
    if (iface == nullptr) {
        std::printf("POWER_HDI_SERVICE=unavailable\n");
        return 1;
    }
    std::printf("POWER_HDI_SERVICE=connected\n");

    uint32_t maj = 0, min = 0;
    int32_t vrc = iface->GetVersion(maj, min);
    std::printf("POWER_HDI_VERSION rc=%d ver=%u.%u\n", vrc, maj, min);

    sptr<ProbePowerCb> cb = new ProbePowerCb();
    sptr<ProbePowerCbExt> cbExt = new ProbePowerCbExt();
    sptr<ProbeRunningLockCb> rlCb = new ProbeRunningLockCb();

    int32_t regRc = iface->RegisterCallback(cb);
    int32_t regExtRc = iface->RegisterPowerCallbackExt(cbExt);
    int32_t regRlRc = iface->RegisterRunningLockCallback(rlCb);

    /* Real kernel wakelock Hold -> verify present in PowerDump (/sys/power/wake_lock) -> Unhold -> verify cleared */
    RunningLockInfo lockInfo {};
    lockInfo.name = "marble_h3_probe_lock";
    lockInfo.type = RUNNINGLOCK_BACKGROUND_TASK;
    lockInfo.timeoutMs = 3000;
    lockInfo.pid = 1234;
    lockInfo.uid = 0;

    int32_t holdRc = iface->HoldRunningLock(lockInfo);
    std::string dumpHeld;
    int32_t dumpHeldRc = iface->PowerDump(dumpHeld);
    bool lockVisibleInKernel = (dumpHeld.find("ohos_marble_h3_probe_lock") != std::string::npos);

    int32_t unholdRc = iface->UnholdRunningLock(lockInfo);
    std::string dumpFree;
    int32_t dumpFreeRc = iface->PowerDump(dumpFree);
    bool lockCleared = (dumpFree.find("ohos_marble_h3_probe_lock") == std::string::npos);
    bool stateOk = (dumpFree.find("freeze") != std::string::npos || dumpFree.find("mem") != std::string::npos);

    std::printf("POWER_WAKELOCK hold=%d unhold=%d visible=%d cleared=%d state_ok=%d rl_cb_msgs=%d\n",
                holdRc, unholdRc, lockVisibleInKernel ? 1 : 0, lockCleared ? 1 : 0,
                stateOk ? 1 : 0, rlCb->msgCnt.load());

    /* Test v1.2 + v1.3 callbacks via SetSuspendTag + StartSuspend/StopSuspend (safe non-S3 state machine) */
    int32_t tagRc = iface->SetSuspendTag("marble_h3_tag");
    int32_t suspRc = iface->StartSuspend();
    int32_t wakeRc = iface->StopSuspend();
    bool cbsOk = (cb->suspendCnt.load() >= 1 && cb->wakeupCnt.load() >= 1 &&
                  cbExt->suspendCnt.load() >= 1 && cbExt->wakeupCnt.load() >= 1 &&
                  cbExt->lastSuspendTag == "marble_h3_tag" && cbExt->lastWakeupTag == "marble_h3_tag");
    std::printf("POWER_CALLBACKS reg=%d ext_reg=%d susp=%d wake=%d cb=%d/%d ext=%d/%d tag=%s ok=%d\n",
                regRc, regExtRc, suspRc, wakeRc,
                cb->suspendCnt.load(), cb->wakeupCnt.load(),
                cbExt->suspendCnt.load(), cbExt->wakeupCnt.load(),
                cbExt->lastWakeupTag.c_str(), cbsOk ? 1 : 0);

    /* Test GetWakeupReason + SetPowerConfig/GetPowerConfig */
    std::string reason;
    int32_t wrRc = iface->GetWakeupReason(reason);
    int32_t scRc = iface->SetPowerConfig("marble_scene", "balanced");
    std::string cfgVal, sysState;
    int32_t gcRc = iface->GetPowerConfig("marble_scene", cfgVal);
    int32_t psRc = iface->GetPowerConfig("power_state", sysState);
    bool cfgOk = (wrRc == 0 && !reason.empty() && scRc == 0 && gcRc == 0 && cfgVal == "balanced" &&
                  psRc == 0 && !sysState.empty());
    std::printf("POWER_CONFIG wakeup_rc=%d reason=%s cfg=%s sys_state=[%s] ok=%d\n",
                wrRc, reason.c_str(), cfgVal.c_str(), sysState.c_str(), cfgOk ? 1 : 0);

    (void)iface->UnRegisterPowerCallbackExt(cbExt);
    (void)iface->UnRegisterRunningLockCallback();
    (void)iface->RegisterCallback(nullptr);

    bool allOk = (vrc == 0 && maj == 1 && min == 3 &&
                  regRc == 0 && regExtRc == 0 && regRlRc == 0 &&
                  holdRc == 0 && unholdRc == 0 && dumpHeldRc == 0 && dumpFreeRc == 0 &&
                  lockVisibleInKernel && lockCleared && stateOk && rlCb->msgCnt.load() >= 2 &&
                  tagRc == 0 && suspRc == 0 && wakeRc == 0 && cbsOk && cfgOk);

    std::printf("MARBLE_POWER_HDI_PROBE_SUMMARY sa3301=%d hdi_ver=%u.%u wakelock_ok=%d "
                "state_ok=%d cb_ok=%d cfg_ok=%d all_ok=%d\n",
                sa3301 ? 1 : 0, maj, min, (lockVisibleInKernel && lockCleared) ? 1 : 0,
                stateOk ? 1 : 0, cbsOk ? 1 : 0, cfgOk ? 1 : 0, allOk ? 1 : 0);
    return allOk ? 0 : 2;
}
