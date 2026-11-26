#ifndef OHOS_HDI_SENSOR_V3_1_ISENSORINTERFACE_H
#define OHOS_HDI_SENSOR_V3_1_ISENSORINTERFACE_H

#include "sensor/v3_0/isensor_interface.h"

namespace OHOS {
namespace HDI {
namespace Sensor {
namespace V3_1 {
using OHOS::HDI::Sensor::V3_0::DeviceSensorInfo;
using OHOS::HDI::Sensor::V3_0::HdfSensorEvents;
using OHOS::HDI::Sensor::V3_0::HdfSensorInformation;
using OHOS::HDI::Sensor::V3_0::ISensorCallback;
using OHOS::HDI::Sensor::V3_0::ISensorPlugCallback;
using OHOS::HDI::Sensor::V3_0::Location;
using OHOS::HDI::Sensor::V3_0::SdcSensorInfo;

enum {
    CMD_SENSOR_INTERFACE_ENABLE_WITH_CALLBACK_ID = 17,
    CMD_SENSOR_INTERFACE_DISABLE_WITH_CALLBACK_ID = 18,
    CMD_SENSOR_INTERFACE_SET_BATCH_WITH_CALLBACK_ID = 19,
    CMD_SENSOR_INTERFACE_REGISTER_WITH_CALLBACK_ID = 20,
    CMD_SENSOR_INTERFACE_UNREGISTER_WITH_CALLBACK_ID = 21,
};

class ISensorInterface : public OHOS::HDI::Sensor::V3_0::ISensorInterface {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.sensor.v3_1.ISensorInterface");

    virtual ~ISensorInterface() = default;

    static sptr<ISensorInterface> Get(bool isStub = false);
    static sptr<ISensorInterface> Get(const std::string &serviceName, bool isStub = false);

    int32_t GetVersion(uint32_t& majorVer, uint32_t& minorVer) override
    {
        majorVer = 3;
        minorVer = 1;
        return HDF_SUCCESS;
    }

    virtual int32_t EnableWithCallbackId(const DeviceSensorInfo& deviceSensorInfo, uint32_t callbackId) = 0;

    virtual int32_t DisableWithCallbackId(const DeviceSensorInfo& deviceSensorInfo, uint32_t callbackId) = 0;

    virtual int32_t SetBatchWithCallbackId(const DeviceSensorInfo& deviceSensorInfo,
        uint32_t callbackId, int64_t samplingInterval, int64_t reportInterval) = 0;

    virtual int32_t RegisterWithCallbackId(int32_t groupId,
        const sptr<ISensorCallback>& callbackObj, uint32_t callbackId) = 0;

    virtual int32_t UnregisterWithCallbackId(int32_t groupId,
        const sptr<ISensorCallback>& callbackObj, uint32_t callbackId) = 0;
};
} // namespace V3_1
} // namespace Sensor
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_SENSOR_V3_1_ISENSORINTERFACE_H
