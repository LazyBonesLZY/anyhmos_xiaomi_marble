/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_POWER_V1_2_RUNNINGLOCKTYPES_H
#define OHOS_HDI_POWER_V1_2_RUNNINGLOCKTYPES_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace OHOS {
namespace HDI {
namespace Power {
namespace V1_2 {

enum BaseRunningLockType : int32_t {
    RUNNINGLOCK_SCREEN = 0,
    RUNNINGLOCK_BACKGROUND = 1,
    RUNNINGLOCK_PROXIMITY_SCREEN_CONTROL = 2,
};

enum RunningLockType : int32_t {
    RUNNINGLOCK_BACKGROUND_PHONE = 3,
    RUNNINGLOCK_BACKGROUND_NOTIFICATION = 5,
    RUNNINGLOCK_BACKGROUND_AUDIO = 9,
    RUNNINGLOCK_BACKGROUND_SPORT = 17,
    RUNNINGLOCK_BACKGROUND_NAVIGATION = 33,
    RUNNINGLOCK_BACKGROUND_TASK = 65,
    RUNNINGLOCK_BUTT,
};

struct RunningLockInfo {
    std::string name;
    RunningLockType type;
    int32_t timeoutMs;
    int32_t pid;
    int32_t uid;
};

static_assert(sizeof(RunningLockInfo) == 40, "RunningLockInfo ABI size mismatch vs donor libpower_stub_1.2.z.so");
static_assert(offsetof(RunningLockInfo, name) == 0, "RunningLockInfo::name offset mismatch");
static_assert(offsetof(RunningLockInfo, type) == 24, "RunningLockInfo::type offset mismatch");
static_assert(offsetof(RunningLockInfo, timeoutMs) == 28, "RunningLockInfo::timeoutMs offset mismatch");
static_assert(offsetof(RunningLockInfo, pid) == 32, "RunningLockInfo::pid offset mismatch");
static_assert(offsetof(RunningLockInfo, uid) == 36, "RunningLockInfo::uid offset mismatch");

} // namespace V1_2
} // namespace Power
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_POWER_V1_2_RUNNINGLOCKTYPES_H
