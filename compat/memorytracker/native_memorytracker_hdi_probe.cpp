/* SPDX-License-Identifier: MIT */
/*
 * End-to-end OHOS Binder & Passthrough probe for SM7475 Hardware MemoryTracker HDI v1.0 (Batch H5).
 *
 * Links against unmodified donor /system/lib64/libmemorytracker_proxy_1.0.z.so and validates:
 *   1. SA 1909 (memmgrservice) registration
 *   2. IPC Binder mode (IMemoryTrackerInterface::Get(false) -> HDF memorytracker_host host-14)
 *      - GetVersion == 1.0
 *      - GetDevMem(0, GRAPH) > 0 (live SM7475 /sys/kernel/dmabuf/buffers total bytes)
 *      - GetDevMem(0, GL) >= 0 (Adreno KGSL / system DMA-BUF accounting)
 *      - GetDevMem(0, OTHER) > 0 (live /proc/meminfo Slab + Shmem)
 *      - GetDevMem(getpid(), OTHER) > 0 (live /proc/self/smaps_rollup)
 *      - Invalid pid/type rejection (rc != 0)
 */

#include "v1_0/imemory_tracker_interface.h"

#include <cinttypes>
#include <cstdio>
#include <unistd.h>
#include <vector>

#include <ipc_skeleton.h>
#include <iremote_object.h>
#include <message_option.h>
#include <message_parcel.h>

using OHOS::IPCSkeleton;
using OHOS::IRemoteObject;
using OHOS::MessageOption;
using OHOS::MessageParcel;
using OHOS::sptr;
using OHOS::HDI::Memorytracker::V1_0::IMemoryTrackerInterface;
using OHOS::HDI::Memorytracker::V1_0::MEMORY_TRACKER_TYPE_CAM;
using OHOS::HDI::Memorytracker::V1_0::MEMORY_TRACKER_TYPE_COUNTS;
using OHOS::HDI::Memorytracker::V1_0::MEMORY_TRACKER_TYPE_GL;
using OHOS::HDI::Memorytracker::V1_0::MEMORY_TRACKER_TYPE_GRAPH;
using OHOS::HDI::Memorytracker::V1_0::MEMORY_TRACKER_TYPE_MM;
using OHOS::HDI::Memorytracker::V1_0::MEMORY_TRACKER_TYPE_OTHER;
using OHOS::HDI::Memorytracker::V1_0::MemoryRecord;

namespace {

bool CheckSaRegistered(int32_t saId)
{
    sptr<IRemoteObject> samgr = IPCSkeleton::GetContextObject();
    if (samgr == nullptr) {
        return false;
    }
    MessageParcel data, reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(u"ohos.samgr.accessToken")) {
        return false;
    }
    if (!data.WriteInt32(saId)) {
        return false;
    }
    if (samgr->SendRequest(1 /* CHECK_SYSTEM_ABILITY_TRANSACTION */, data, reply, opt) != 0) {
        return false;
    }
    return reply.ReadRemoteObject() != nullptr;
}

int64_t SumBytes(const std::vector<MemoryRecord> &recs)
{
    int64_t sum = 0;
    for (const auto &r : recs) {
        sum += r.size;
    }
    return sum;
}

} // namespace

int main()
{
    const bool sa1909 = CheckSaRegistered(1909);
    std::printf("SA1909_REGISTERED=%d\n", sa1909 ? 1 : 0);

    sptr<IMemoryTrackerInterface> svc = IMemoryTrackerInterface::Get(false);
    if (svc == nullptr) {
        std::printf("MEMORYTRACKER_HDI_SERVICE=unavailable\n");
        std::printf("MARBLE_MEMORYTRACKER_HDI_PROBE_SUMMARY sa1909=%d hdi_ver=0.0 graph_bytes=0"
                    " gl_bytes=0 other_bytes=0 self_bytes=0 inv_ok=0 all_ok=0\n",
                    sa1909 ? 1 : 0);
        return 2;
    }
    std::printf("MEMORYTRACKER_HDI_SERVICE=connected\n");

    uint32_t major = 0;
    uint32_t minor = 0;
    const int32_t verRc = svc->GetVersion(major, minor);
    std::printf("MEMORYTRACKER_HDI_VERSION rc=%d ver=%u.%u\n", verRc, major, minor);

    std::vector<MemoryRecord> graphRecs, glRecs, mmRecs, camRecs, otherRecs, selfRecs, invRecs;
    const int32_t graphRc = svc->GetDevMem(0, MEMORY_TRACKER_TYPE_GRAPH, graphRecs);
    const int32_t glRc = svc->GetDevMem(0, MEMORY_TRACKER_TYPE_GL, glRecs);
    const int32_t mmRc = svc->GetDevMem(0, MEMORY_TRACKER_TYPE_MM, mmRecs);
    const int32_t camRc = svc->GetDevMem(0, MEMORY_TRACKER_TYPE_CAM, camRecs);
    const int32_t otherRc = svc->GetDevMem(0, MEMORY_TRACKER_TYPE_OTHER, otherRecs);
    const int32_t selfRc = svc->GetDevMem(static_cast<int32_t>(getpid()), MEMORY_TRACKER_TYPE_OTHER, selfRecs);
    const int32_t badTypeRc = svc->GetDevMem(0, MEMORY_TRACKER_TYPE_COUNTS, invRecs);
    const int32_t badPidRc = svc->GetDevMem(-1, MEMORY_TRACKER_TYPE_GRAPH, invRecs);

    const int64_t graphBytes = SumBytes(graphRecs);
    const int64_t glBytes = SumBytes(glRecs);
    const int64_t mmBytes = SumBytes(mmRecs);
    const int64_t camBytes = SumBytes(camRecs);
    const int64_t otherBytes = SumBytes(otherRecs);
    const int64_t selfBytes = SumBytes(selfRecs);

    std::printf("MEMORYTRACKER_GLOBAL graph_rc=%d graph=%" PRId64 " gl_rc=%d gl=%" PRId64
                " mm_rc=%d mm=%" PRId64 " cam_rc=%d cam=%" PRId64 " other_rc=%d other=%" PRId64 "\n",
                graphRc, graphBytes, glRc, glBytes, mmRc, mmBytes, camRc, camBytes, otherRc, otherBytes);
    std::printf("MEMORYTRACKER_SELF pid=%d rc=%d recs=%zu bytes=%" PRId64 "\n",
                static_cast<int>(getpid()), selfRc, selfRecs.size(), selfBytes);
    const bool invOk = (badTypeRc != 0 && badPidRc != 0);
    std::printf("MEMORYTRACKER_INVALID bad_type=%d bad_pid=%d ok=%d\n", badTypeRc, badPidRc, invOk ? 1 : 0);

    const bool allOk = (sa1909 && verRc == 0 && major == 1 && minor == 0 &&
                        graphRc == 0 && graphBytes > 0 &&
                        glRc == 0 && mmRc == 0 && camRc == 0 &&
                        otherRc == 0 && otherBytes > 0 &&
                        selfRc == 0 && selfBytes > 0 && invOk);
    std::printf("MARBLE_MEMORYTRACKER_HDI_PROBE_SUMMARY sa1909=%d hdi_ver=%u.%u graph_bytes=%" PRId64
                " gl_bytes=%" PRId64 " other_bytes=%" PRId64 " self_bytes=%" PRId64
                " inv_ok=%d all_ok=%d\n",
                sa1909 ? 1 : 0, major, minor, graphBytes, glBytes, otherBytes, selfBytes,
                invOk ? 1 : 0, allOk ? 1 : 0);
    return allOk ? 0 : 2;
}
