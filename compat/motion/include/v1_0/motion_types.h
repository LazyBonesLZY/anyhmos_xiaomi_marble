/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_MOTION_V1_0_MOTION_TYPES_H
#define OHOS_HDI_MOTION_V1_0_MOTION_TYPES_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace OHOS {
namespace HDI {
namespace Motion {
namespace V1_0 {

enum HdfMotionTypeTag : int32_t {
    HDF_MOTION_TYPE_PICKUP = 0,
    HDF_MOTION_TYPE_FLIP,
    HDF_MOTION_CLOSE_TO_EAR,
    HDF_MOTION_TYPE_SHAKE,
    HDF_MOTION_TYPE_ROTATION,
    HDF_MOTION_TYPE_POCKET_MODE,
    HDF_MOTION_TYPE_LEAVE_EAR,
    HDF_MOTION_TYPE_WRIST_UP,
    HDF_MOTION_TYPE_WRIST_DOWN,
    HDF_MOTION_TYPE_WAVE,
    HDF_MOTION_TYPE_STEP_COUNTER,
    HDF_MOTION_TYPE_MAX,
};

struct HdfMotionEvent {
    int32_t motion;
    int32_t result;
    int32_t status;
    int32_t datalen;
    std::vector<int32_t> data;
};

static_assert(offsetof(HdfMotionEvent, motion) == 0, "HdfMotionEvent.motion ABI offset");
static_assert(offsetof(HdfMotionEvent, result) == 4, "HdfMotionEvent.result ABI offset");
static_assert(offsetof(HdfMotionEvent, status) == 8, "HdfMotionEvent.status ABI offset");
static_assert(offsetof(HdfMotionEvent, datalen) == 12, "HdfMotionEvent.datalen ABI offset");
static_assert(offsetof(HdfMotionEvent, data) == 16, "HdfMotionEvent.data ABI offset");
static_assert(sizeof(HdfMotionEvent) == 40, "HdfMotionEvent ABI size");

} // namespace V1_0
} // namespace Motion
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_MOTION_V1_0_MOTION_TYPES_H
