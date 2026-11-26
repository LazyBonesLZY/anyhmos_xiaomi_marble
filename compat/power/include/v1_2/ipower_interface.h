/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_POWER_V1_2_IPOWERINTERFACE_H
#define OHOS_HDI_POWER_V1_2_IPOWERINTERFACE_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_2/ipower_hdi_callback.h"
#include "v1_2/ipower_running_lock_callback.h"
#include "v1_2/running_lock_types.h"

namespace OHOS {
namespace HDI {
namespace Power {
namespace V1_2 {

class IPowerInterface : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.power.v1_2.IPowerInterface");

    virtual ~IPowerInterface() = default;

    static sptr<OHOS::HDI::Power::V1_2::IPowerInterface> Get(bool isStub = false);
    static sptr<OHOS::HDI::Power::V1_2::IPowerInterface> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t RegisterCallback(const sptr<OHOS::HDI::Power::V1_2::IPowerHdiCallback> &ipowerHdiCallback) = 0;

    virtual int32_t StartSuspend() = 0;

    virtual int32_t StopSuspend() = 0;

    virtual int32_t ForceSuspend() = 0;

    virtual int32_t SuspendBlock(const std::string &name) = 0;

    virtual int32_t SuspendUnblock(const std::string &name) = 0;

    virtual int32_t PowerDump(std::string &info) = 0;

    virtual int32_t HoldRunningLock(const OHOS::HDI::Power::V1_2::RunningLockInfo &info) = 0;

    virtual int32_t UnholdRunningLock(const OHOS::HDI::Power::V1_2::RunningLockInfo &info) = 0;

    virtual int32_t GetWakeupReason(std::string &reason) = 0;

    virtual int32_t HoldRunningLockExt(const OHOS::HDI::Power::V1_2::RunningLockInfo &info,
                                       uint64_t lockid, const std::string &bundleName) = 0;

    virtual int32_t UnholdRunningLockExt(const OHOS::HDI::Power::V1_2::RunningLockInfo &info,
                                         uint64_t lockid, const std::string &bundleName) = 0;

    virtual int32_t RegisterRunningLockCallback(
        const sptr<OHOS::HDI::Power::V1_2::IPowerRunningLockCallback> &iPowerRunningLockCallback) = 0;

    virtual int32_t UnRegisterRunningLockCallback() = 0;

    virtual int32_t Hibernate() = 0;

    virtual int32_t SetSuspendTag(const std::string &tag) = 0;

    virtual int32_t SetPowerConfig(const std::string &scene, const std::string &value) = 0;

    virtual int32_t GetPowerConfig(const std::string &scene, std::string &value) = 0;

    virtual int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) {
        majorVer = 1;
        minorVer = 2;
        return HDF_SUCCESS;
    }

    virtual bool IsProxy() {
        return false;
    }

    virtual const std::u16string GetDesc() {
        return metaDescriptor_;
    }
};

} // namespace V1_2
} // namespace Power
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_POWER_V1_2_IPOWERINTERFACE_H
