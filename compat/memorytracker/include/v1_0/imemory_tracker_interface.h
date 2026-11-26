/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_MEMORYTRACKER_V1_0_IMEMORY_TRACKER_INTERFACE_H
#define OHOS_HDI_MEMORYTRACKER_V1_0_IMEMORY_TRACKER_INTERFACE_H

#include <cstdint>
#include <string>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/memory_tracker_types.h"

namespace OHOS {
namespace HDI {
namespace Memorytracker {
namespace V1_0 {

class IMemoryTrackerInterface : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.memorytracker.v1_0.IMemoryTrackerInterface");

    virtual ~IMemoryTrackerInterface() = default;

    static sptr<IMemoryTrackerInterface> Get(bool isStub = false);
    static sptr<IMemoryTrackerInterface> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t GetDevMem(int32_t pid, MemoryTrackerType type, std::vector<MemoryRecord> &records) = 0;

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
} // namespace Memorytracker
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_MEMORYTRACKER_V1_0_IMEMORY_TRACKER_INTERFACE_H
