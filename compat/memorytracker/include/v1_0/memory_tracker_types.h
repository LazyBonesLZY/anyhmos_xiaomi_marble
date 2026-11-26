/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_MEMORYTRACKER_V1_0_MEMORY_TRACKER_TYPES_H
#define OHOS_HDI_MEMORYTRACKER_V1_0_MEMORY_TRACKER_TYPES_H

#include <cstddef>
#include <cstdint>

namespace OHOS {
namespace HDI {
namespace Memorytracker {
namespace V1_0 {

enum MemoryTrackerType : int32_t {
    MEMORY_TRACKER_TYPE_MM = 0,
    MEMORY_TRACKER_TYPE_GL = 1,
    MEMORY_TRACKER_TYPE_CAM = 2,
    MEMORY_TRACKER_TYPE_GRAPH = 3,
    MEMORY_TRACKER_TYPE_OTHER = 4,
    MEMORY_TRACKER_TYPE_COUNTS = 5,
};

enum MemoryTrackerFlag : int32_t {
    FLAG_SHARED_RSS = 2,
    FLAG_SHARED_PSS = 4,
    FLAG_PRIVATE = 8,
    FLAG_MAPPED = 16,
    FLAG_UNMAPPED = 32,
    FLAG_PROTECTED = 64,
    FLAG_UNPROTECTED = 128,
    FLAG_SYSTEM = 256,
    FLAG_SYSTEM_EXCEPT = 512,
};

/* Exact donor ABI (libmemorytracker_stub_1.0.z.so MemoryRecordBlockMarshalling -> 16 B) */
struct MemoryRecord {
    int32_t flags = 0;
    int64_t size = 0;
};

static_assert(sizeof(MemoryRecord) == 16, "MemoryRecord ABI mismatch with donor libmemorytracker_stub_1.0");
static_assert(offsetof(MemoryRecord, flags) == 0, "MemoryRecord.flags offset mismatch");
static_assert(offsetof(MemoryRecord, size) == 8, "MemoryRecord.size offset mismatch");

} // namespace V1_0
} // namespace Memorytracker
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_MEMORYTRACKER_V1_0_MEMORY_TRACKER_TYPES_H
