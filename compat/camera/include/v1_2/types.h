/* SPDX-License-Identifier: MIT */
/* Clean-room OHOS Camera HDI V1_2 types (ABI-verified against donor libcamera_stub_1.2.z.so). */
#ifndef OHOS_HDI_CAMERA_V1_2_TYPES_H
#define OHOS_HDI_CAMERA_V1_2_TYPES_H

#include <cstdint>
#include "v1_1/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_2 {

enum CamRetCode : int32_t {
    NO_ERROR = 0,
    CAMERA_BUSY = -1,
    INSUFFICIENT_RESOURCES = -2,
    INVALID_ARGUMENT = -3,
    METHOD_NOT_SUPPORTED = -4,
    CAMERA_CLOSED = -5,
    DEVICE_ERROR = -6,
    NO_PERMISSION = -7,
    DEVICE_CONFLICT = -8,
};

enum ExtendedStreamInfoType_V1_2 : int32_t {
    EXTENDED_STREAM_INFO_QUICK_THUMBNAIL = 0,
    EXTENDED_STREAM_INFO_SKETCH = 1,
};

enum OperationMode_V1_2 : int32_t {
    NORMAL = 0,
    CAPTURE = 1,
    VIDEO = 2,
    PORTRAIT = 3,
    NIGHT = 4,
    PROFESSIONAL = 5,
    SLOW_MOTION = 6,
    SCAN_CODE = 7,
    CAPTURE_MACRO = 8,
    VIDEO_MACRO = 9,
    SUPER_STAB = 10,
    OPERATION_MODE_MAX,
};

struct CaptureStartedInfo {
    int32_t streamId_;
    int32_t exposureTime_;
} __attribute__((aligned(8)));

static_assert(sizeof(CaptureStartedInfo) == 8, "V1_2::CaptureStartedInfo ABI size mismatch");

} // namespace V1_2
} // namespace Camera
} // namespace HDI
} // namespace OHOS

#endif /* OHOS_HDI_CAMERA_V1_2_TYPES_H */
