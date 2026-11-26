/* SPDX-License-Identifier: MIT */
/* Clean-room OHOS Camera HDI V1_0 types (ABI-verified against donor libcamera_stub_1.0.z.so). */
#ifndef OHOS_HDI_CAMERA_V1_0_TYPES_H
#define OHOS_HDI_CAMERA_V1_0_TYPES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <refbase.h>
#include "buffer_producer_sequenceable.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_0 {

using OHOS::sptr;

enum CamRetCode : int32_t {
    NO_ERROR = 0,
    CAMERA_BUSY = -1,
    INSUFFICIENT_RESOURCES = -2,
    INVALID_ARGUMENT = -3,
    METHOD_NOT_SUPPORTED = -4,
    CAMERA_CLOSED = -5,
    DEVICE_ERROR = -6,
    NO_PERMISSION = -7,
};

enum ResultCallbackMode : int32_t {
    PER_FRAME = 0,
    ON_CHANGED = 1,
};

enum OperationMode : int32_t {
    NORMAL = 0,
};

enum StreamIntent : int32_t {
    PREVIEW = 0,
    VIDEO = 1,
    STILL_CAPTURE = 2,
    POST_VIEW = 3,
    ANALYZE = 4,
    CUSTOM = 5,
};

enum EncodeType : int32_t {
    ENCODE_TYPE_NULL = 0,
    ENCODE_TYPE_H264 = 1,
    ENCODE_TYPE_H265 = 2,
    ENCODE_TYPE_JPEG = 3,
};

enum StreamSupportType : int32_t {
    DYNAMIC_SUPPORTED = 0,
    RE_CONFIGURED_REQUIRED = 1,
    NOT_SUPPORTED = 2,
};

enum CameraStatus : int32_t {
    UN_AVAILABLE = 0,
    AVAILABLE = 1,
};

enum FlashlightStatus : int32_t {
    FLASHLIGHT_OFF = 0,
    FLASHLIGHT_ON = 1,
    FLASHLIGHT_UNAVAILABLE = 2,
};

enum CameraEvent : int32_t {
    CAMERA_EVENT_DEVICE_ADD = 0,
    CAMERA_EVENT_DEVICE_RMV = 1,
};

enum ErrorType : int32_t {
    FATAL_ERROR = 0,
    REQUEST_TIMEOUT = 1,
    DRIVER_ERROR = 2,
    DEVICE_PREEMPT = 3,
    DEVICE_DISCONNECT = 4,
    DCAMERA_ERROR_BEGIN = 1024,
    DCAMERA_ERROR_DEVICE_IN_USE,
    DCAMERA_ERROR_NO_PERMISSION,
};

enum StreamError : int32_t {
    UNKNOWN_ERROR = 0,
    BUFFER_LOST = 1,
};

struct StreamInfo {
    int32_t streamId_;
    int32_t width_;
    int32_t height_;
    int32_t format_;
    int32_t dataspace_;
    StreamIntent intent_;
    bool tunneledMode_;
    sptr<BufferProducerSequenceable> bufferQueue_;
    int32_t minFrameDuration_;
    EncodeType encodeType_;
};

struct StreamAttribute {
    int32_t streamId_;
    int32_t width_;
    int32_t height_;
    int32_t overrideFormat_;
    int32_t overrideDataspace_;
    int32_t producerUsage_;
    int32_t producerBufferCount_;
    int32_t maxBatchCaptureCount_;
    int32_t maxCaptureCount_;
} __attribute__((aligned(8)));

struct CaptureInfo {
    std::vector<int32_t> streamIds_;
    std::vector<uint8_t> captureSetting_;
    bool enableShutterCallback_;
};

struct CaptureEndedInfo {
    int32_t streamId_;
    int32_t frameCount_;
} __attribute__((aligned(8)));

struct CaptureErrorInfo {
    int32_t streamId_;
    StreamError error_;
} __attribute__((aligned(8)));

static_assert(sizeof(StreamInfo) == 48, "V1_0::StreamInfo ABI size mismatch");
static_assert(offsetof(StreamInfo, bufferQueue_) == 32, "V1_0::StreamInfo bufferQueue_ offset mismatch");
static_assert(offsetof(StreamInfo, encodeType_) == 44, "V1_0::StreamInfo encodeType_ offset mismatch");
static_assert(sizeof(StreamAttribute) == 40, "V1_0::StreamAttribute ABI size mismatch");
static_assert(sizeof(CaptureInfo) == 56, "V1_0::CaptureInfo ABI size mismatch");
static_assert(sizeof(CaptureEndedInfo) == 8, "V1_0::CaptureEndedInfo ABI size mismatch");
static_assert(sizeof(CaptureErrorInfo) == 8, "V1_0::CaptureErrorInfo ABI size mismatch");

} // namespace V1_0
} // namespace Camera
} // namespace HDI
} // namespace OHOS

#endif /* OHOS_HDI_CAMERA_V1_0_TYPES_H */
