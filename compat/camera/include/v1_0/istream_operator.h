/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_0_ISTREAMOPERATOR_H
#define OHOS_HDI_CAMERA_V1_0_ISTREAMOPERATOR_H

#include <cstdint>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/types.h"
#include "v1_0/istream_operator_callback.h"
#include "v1_0/ioffline_stream_operator.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_0 {

class IStreamOperator : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_0.IStreamOperator");

    virtual ~IStreamOperator() = default;

    virtual int32_t IsStreamsSupported(OperationMode mode, const std::vector<uint8_t> &modeSetting,
                                       const std::vector<StreamInfo> &infos,
                                       StreamSupportType &type) = 0;

    virtual int32_t CreateStreams(const std::vector<StreamInfo> &streamInfos) = 0;

    virtual int32_t ReleaseStreams(const std::vector<int32_t> &streamIds) = 0;

    virtual int32_t CommitStreams(OperationMode mode, const std::vector<uint8_t> &modeSetting) = 0;

    virtual int32_t GetStreamAttributes(std::vector<StreamAttribute> &attributes) = 0;

    virtual int32_t AttachBufferQueue(int32_t streamId,
                                      const sptr<BufferProducerSequenceable> &bufferProducer) = 0;

    virtual int32_t DetachBufferQueue(int32_t streamId) = 0;

    virtual int32_t Capture(int32_t captureId, const CaptureInfo &info, bool isStreaming) = 0;

    virtual int32_t CancelCapture(int32_t captureId) = 0;

    virtual int32_t ChangeToOfflineStream(const std::vector<int32_t> &streamIds,
                                          const sptr<IStreamOperatorCallback> &callbackObj,
                                          sptr<IOfflineStreamOperator> &offlineOperator) = 0;

    virtual int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) {
        majorVer = 1;
        minorVer = 0;
        return HDF_SUCCESS;
    }

    virtual bool IsProxy() {
        return false;
    }

    virtual const std::u16string GetDesc() {
        return metaDescriptor_;
    }
};

} // namespace V1_0
} // namespace Camera
} // namespace HDI
} // namespace OHOS

#endif /* OHOS_HDI_CAMERA_V1_0_ISTREAMOPERATOR_H */
