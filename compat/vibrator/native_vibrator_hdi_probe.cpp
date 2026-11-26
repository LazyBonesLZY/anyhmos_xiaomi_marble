/* SPDX-License-Identifier: MIT */
/*
 * native_vibrator_hdi_probe.cpp - End-to-end OHOS Binder/HDI Vibrator v2.0 probe for Xiaomi marble.
 *
 * Linked against unmodified donor libvibrator_proxy_2.0.z.so:
 *   1. Checks SA 3602 (miscdevice) registration.
 *   2. Connects to HDF host-15 (vibrator_host) -> vibrator_interface_service via Binder.
 *   3. Verifies GetVersion == 2.0.
 *   4. Verifies GetVibratorInfo (32B struct ABI), GetDeviceVibratorInfo, GetVibratorIdSingle.
 *   5. Verifies GetHapticCapacity, GetHapticStartUpTime, GetEffectInfo, GetAllWaveInfo.
 *   6. Verifies StartOnce(50), Start("haptic.effect.soft"), StartByIntensity, PlayPatternBySessionId,
 *      IsVibratorRunning, Stop, and StopVibrateBySessionId over real Binder IPC.
 */

#include "v2_0/ivibrator_interface.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <unistd.h>
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
using namespace OHOS::HDI::Vibrator::V2_0;

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

} // namespace

int main(void) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    bool sa3602 = checkSaRegistered(3602);
    std::printf("SA3602_REGISTERED=%d\n", sa3602 ? 1 : 0);

    sptr<IVibratorInterface> iface = IVibratorInterface::Get(false);
    if (iface == nullptr) {
        std::printf("VIBRATOR_HDI_SERVICE=unavailable\n");
        return 1;
    }
    std::printf("VIBRATOR_HDI_SERVICE=connected\n");

    uint32_t maj = 0, min = 0;
    int32_t vrc = iface->GetVersion(maj, min);
    std::printf("VIBRATOR_HDI_VERSION rc=%d ver=%u.%u\n", vrc, maj, min);

    std::vector<HdfVibratorInfo> infos;
    int32_t infoRc = iface->GetVibratorInfo(infos);
    bool infoOk = (infoRc == 0 && !infos.empty() &&
                   infos[0].deviceId == -1 && infos[0].vibratorId == 1 &&
                   infos[0].isLocal == 1 && infos[0].isSupportIntensity &&
                   infos[0].intensityMaxValue == 100 && infos[0].intensityMinValue == 1);
    if (!infos.empty()) {
        std::printf("VIBRATOR_INFO rc=%d cnt=%zu dev=%d vib=%d local=%d inten=%d[%u..%u] hd=%d res=%d ok=%d\n",
                    infoRc, infos.size(), infos[0].deviceId, infos[0].vibratorId, infos[0].isLocal,
                    infos[0].isSupportIntensity ? 1 : 0, infos[0].intensityMinValue,
                    infos[0].intensityMaxValue, infos[0].isSupportHdHaptic ? 1 : 0,
                    infos[0].reserved, infoOk ? 1 : 0);
    } else {
        std::printf("VIBRATOR_INFO rc=%d cnt=0 ok=0\n", infoRc);
    }

    DeviceVibratorInfo localDev{-1, 1};
    std::vector<HdfVibratorInfo> devInfos;
    int32_t devInfoRc = iface->GetDeviceVibratorInfo(localDev, devInfos);

    HapticCapacity cap{};
    int32_t capRc = iface->GetHapticCapacity(localDev, cap);
    bool capOk = (capRc == 0 && cap.isSupportHdHaptic && cap.isSupportPresetMapping &&
                  cap.isSupportTimeDelay && cap.vibratorId == 1);
    std::printf("VIBRATOR_CAPACITY rc=%d hd=%d preset=%d delay=%d vib=%d ok=%d\n",
                capRc, cap.isSupportHdHaptic ? 1 : 0, cap.isSupportPresetMapping ? 1 : 0,
                cap.isSupportTimeDelay ? 1 : 0, cap.vibratorId, capOk ? 1 : 0);

    int32_t startupMs = -1;
    int32_t startupRc = iface->GetHapticStartUpTime(localDev, 0, startupMs);

    HdfEffectInfo softInfo{};
    int32_t effSoftRc = iface->GetEffectInfo(localDev, "haptic.effect.soft", softInfo);
    HdfEffectInfo timerInfo{};
    int32_t effTimerRc = iface->GetEffectInfo(localDev, "haptic.clock.timer", timerInfo);
    bool effOk = (effSoftRc == 0 && softInfo.isSupportEffect && softInfo.duration >= 15 && softInfo.duration <= 120 &&
                  effTimerRc == 0 && timerInfo.isSupportEffect && timerInfo.duration >= 15 && timerInfo.duration <= 120);
    std::printf("VIBRATOR_EFFECT_INFO soft=(rc=%d sup=%d dur=%d) timer=(rc=%d sup=%d dur=%d) startup=(rc=%d ms=%d) ok=%d\n",
                effSoftRc, softInfo.isSupportEffect ? 1 : 0, softInfo.duration,
                effTimerRc, timerInfo.isSupportEffect ? 1 : 0, timerInfo.duration,
                startupRc, startupMs, effOk ? 1 : 0);

    std::vector<HdfWaveInformation> waves;
    int32_t waveRc = iface->GetAllWaveInfo(localDev, waves);
    std::printf("VIBRATOR_WAVES rc=%d cnt=%zu\n", waveRc, waves.size());

    /* Test StartOnce(50) + IsVibratorRunning + Stop */
    int32_t onceRc = iface->StartOnce(localDev, 50);
    bool runAfterOnce = false;
    int32_t runOnceRc = iface->IsVibratorRunning(localDev, runAfterOnce);
    ::usleep(15000);
    int32_t stopOnceRc = iface->Stop(localDev, HDF_VIBRATOR_MODE_ONCE);
    bool runAfterStop = true;
    int32_t runStopRc = iface->IsVibratorRunning(localDev, runAfterStop);
    bool onceOk = (onceRc == 0 && runOnceRc == 0 && runAfterOnce &&
                   stopOnceRc == 0 && runStopRc == 0 && !runAfterStop);
    std::printf("VIBRATOR_ONCE start=%d run=%d stop=%d run_after_stop=%d ok=%d\n",
                onceRc, runAfterOnce ? 1 : 0, stopOnceRc, runAfterStop ? 1 : 0, onceOk ? 1 : 0);

    /* Test Start("haptic.effect.soft") + Stop */
    int32_t presetRc = iface->Start(localDev, "haptic.effect.soft");
    ::usleep(10000);
    int32_t stopPresetRc = iface->Stop(localDev, HDF_VIBRATOR_MODE_PRESET);
    bool presetOk = (presetRc == 0 && stopPresetRc == 0);
    std::printf("VIBRATOR_PRESET start=%d stop=%d ok=%d\n", presetRc, stopPresetRc, presetOk ? 1 : 0);

    /* Test PlayPatternBySessionId + StopVibrateBySessionId */
    HapticPaket pkt{};
    pkt.time = 0;
    pkt.eventNum = 2;
    HapticEvent ev1{};
    ev1.type = TRANSIENT;
    ev1.time = 0;
    ev1.duration = 25;
    ev1.intensity = 65;
    ev1.frequency = 50;
    ev1.index = 0;
    ev1.pointNum = 0;
    HapticEvent ev2{};
    ev2.type = CONTINUOUS;
    ev2.time = 60;
    ev2.duration = 40;
    ev2.intensity = 80;
    ev2.frequency = 50;
    ev2.index = 0;
    ev2.pointNum = 0;
    pkt.events.push_back(ev1);
    pkt.events.push_back(ev2);

    int32_t patRc = iface->PlayPatternBySessionId(localDev, 42u, pkt);
    bool runPattern = false;
    (void)iface->IsVibratorRunning(localDev, runPattern);
    ::usleep(15000);
    int32_t stopSessRc = iface->StopVibrateBySessionId(localDev, 42u);
    bool runAfterSess = true;
    (void)iface->IsVibratorRunning(localDev, runAfterSess);
    bool patOk = (patRc == 0 && runPattern && stopSessRc == 0 && !runAfterSess);
    std::printf("VIBRATOR_PATTERN play=%d run=%d stop_sess=%d run_after=%d ok=%d\n",
                patRc, runPattern ? 1 : 0, stopSessRc, runAfterSess ? 1 : 0, patOk ? 1 : 0);

    bool allOk = (vrc == 0 && maj == 2 && min == 0 &&
                  infoOk && devInfoRc == 0 && !devInfos.empty() &&
                  capOk && startupRc == 0 && startupMs >= 0 &&
                  effOk && waveRc == 0 && !waves.empty() &&
                  onceOk && presetOk && patOk);

    std::printf("MARBLE_VIBRATOR_HDI_PROBE_SUMMARY sa3602=%d hdi_ver=%u.%u info_ok=%d cap_ok=%d "
                "eff_ok=%d once_ok=%d preset_ok=%d pattern_ok=%d all_ok=%d\n",
                sa3602 ? 1 : 0, maj, min, infoOk ? 1 : 0, capOk ? 1 : 0,
                effOk ? 1 : 0, onceOk ? 1 : 0, presetOk ? 1 : 0, patOk ? 1 : 0, allOk ? 1 : 0);
    return allOk ? 0 : 2;
}
