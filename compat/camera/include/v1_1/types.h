/* SPDX-License-Identifier: MIT */
/* Clean-room OHOS Camera HDI V1_1 types (ABI-verified against donor libcamera_stub_1.1.z.so). */
#ifndef OHOS_HDI_CAMERA_V1_1_TYPES_H
#define OHOS_HDI_CAMERA_V1_1_TYPES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "v1_0/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_1 {

using OHOS::sptr;

enum ExtendedStreamInfoType : int32_t {
    EXTENDED_STREAM_INFO_QUICK_THUMBNAIL = 0,
};

struct ExtendedStreamInfo {
    ExtendedStreamInfoType type;
    int32_t width;
    int32_t height;
    int32_t format;
    int32_t dataspace;
    sptr<V1_0::BufferProducerSequenceable> bufferQueue;
};

struct StreamInfo_V1_1 {
    V1_0::StreamInfo v1_0;
    std::vector<ExtendedStreamInfo> extendedStreamInfos;
};

struct PrelaunchConfig {
    std::string cameraId;
    std::vector<StreamInfo_V1_1> streamInfos_V1_1;
    std::vector<uint8_t> setting;
};

enum OperationMode_V1_1 : int32_t {
    NORMAL = 0,
    CAPTURE = 1,
    VIDEO = 2,
    PORTRAIT = 3,
    NIGHT = 4,
    PROFESSIONAL = 5,
    SLOW_MOTION = 6,
};

static_assert(sizeof(ExtendedStreamInfo) == 32, "V1_1::ExtendedStreamInfo ABI size mismatch");
static_assert(offsetof(ExtendedStreamInfo, bufferQueue) == 24, "V1_1::ExtendedStreamInfo bufferQueue offset mismatch");
static_assert(sizeof(StreamInfo_V1_1) == 72, "V1_1::StreamInfo_V1_1 ABI size mismatch");
static_assert(offsetof(StreamInfo_V1_1, extendedStreamInfos) == 48, "V1_1::StreamInfo_V1_1 extendedStreamInfos offset mismatch");
static_assert(sizeof(PrelaunchConfig) == 72, "V1_1::PrelaunchConfig ABI size mismatch");
static_assert(offsetof(PrelaunchConfig, streamInfos_V1_1) == 24, "V1_1::PrelaunchConfig streamInfos_V1_1 offset mismatch");

} // namespace V1_1
} // namespace Camera
} // namespace HDI
} // namespace OHOS

#endif /* OHOS_HDI_CAMERA_V1_1_TYPES_H */
