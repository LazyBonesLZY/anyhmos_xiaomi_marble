/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_MOTION_V1_1_MOTION_TYPES_H
#define OHOS_HDI_MOTION_V1_1_MOTION_TYPES_H

#include <cstdint>
#include "v1_0/motion_types.h"

namespace OHOS {
namespace HDI {
namespace Motion {
namespace V1_1 {

using OHOS::HDI::Motion::V1_0::HdfMotionEvent;

struct WaveParam {
    int32_t waveFrequency;
    int32_t waveAmplitude;
    bool isUseGyroscope;
};

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
    HDF_MOTION_TYPE_TOUCH_LINK,
    HDF_MOTION_TYPE_HOVER,
    HDF_MOTION_TYPE_PUT_IN_POCKET,
    HDF_MOTION_TYPE_RESERVED,
    HDF_MOTION_TYPE_MAX,
};

} // namespace V1_1
} // namespace Motion
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_MOTION_V1_1_MOTION_TYPES_H
