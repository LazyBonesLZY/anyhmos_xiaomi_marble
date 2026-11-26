/* SPDX-License-Identifier: MIT */
/*
 * Clean-room SM7475 Hardware MemoryTracker HDI v1.0 implementation for Xiaomi marble.
 *
 * SONAME: libmemory_tracker_interface_service_1.0.z.so
 * Works in BOTH:
 *   - HDF IPC host-14 (memorytracker_host via unmodified donor libmemorytracker_driver.z.so)
 *   - Passthrough mode (IMemoryTrackerInterface::Get(true) via /vendor/lib64/passthrough/)
 *
 * Live SM7475 hardware memory sources (100% read-only):
 *   - /sys/kernel/dmabuf/buffers/<id>/{size,exporter_name} (live Adreno/Display/Venus/Camera DMA-BUFs)
 *   - /proc/<pid>/fdinfo/<fd> (per-process DMA-BUF attribution via size/exp_name)
 *   - /proc/<pid>/smaps & /proc/<pid>/smaps_rollup (KGSL / GPU / graphics / private/PSS accounting)
 *   - /proc/meminfo (global kernel memory fallback)
 */

#include "v1_0/imemory_tracker_interface.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace OHOS {
namespace HDI {
namespace Memorytracker {
namespace V1_0 {

namespace {

constexpr const char *kDmabufRoot = "/sys/kernel/dmabuf/buffers";

bool ReadSmallFile(const std::string &path, std::string &out)
{
    FILE *fp = std::fopen(path.c_str(), "re");
    if (fp == nullptr) {
        return false;
    }
    char buf[256] = {0};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    if (n == 0) {
        return false;
    }
    out.assign(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return !out.empty();
}

int64_t ParseInt64(const std::string &s)
{
    char *endp = nullptr;
    long long v = std::strtoll(s.c_str(), &endp, 10);
    if (endp == s.c_str() || v < 0) {
        return 0;
    }
    return static_cast<int64_t>(v);
}

bool ContainsICase(const std::string &hay, const char *needle)
{
    if (needle == nullptr || *needle == '\0') {
        return true;
    }
    const size_t nlen = std::strlen(needle);
    if (hay.size() < nlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= hay.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < nlen; ++j) {
            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool ExporterMatchesType(const std::string &exporter, MemoryTrackerType type)
{
    switch (type) {
        case MEMORY_TRACKER_TYPE_GRAPH:
            return true; /* All live DMA-BUFs contribute to graphics/DMA-BUF pool */
        case MEMORY_TRACKER_TYPE_GL:
            return ContainsICase(exporter, "kgsl") || ContainsICase(exporter, "gpu") ||
                   ContainsICase(exporter, "system");
        case MEMORY_TRACKER_TYPE_MM:
            return ContainsICase(exporter, "venus") || ContainsICase(exporter, "video") ||
                   ContainsICase(exporter, "codec") || ContainsICase(exporter, "qseecom");
        case MEMORY_TRACKER_TYPE_CAM:
            return ContainsICase(exporter, "cam") || ContainsICase(exporter, "isp");
        default:
            return false;
    }
}

struct DmabufTotals {
    int64_t totalBytes = 0;
    int64_t matchedBytes = 0;
    int32_t bufferCount = 0;
};

DmabufTotals ScanGlobalDmabufs(MemoryTrackerType type)
{
    DmabufTotals totals {};
    DIR *dir = opendir(kDmabufRoot);
    if (dir == nullptr) {
        return totals;
    }
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        const std::string base = std::string(kDmabufRoot) + "/" + ent->d_name;
        std::string sizeStr;
        if (!ReadSmallFile(base + "/size", sizeStr)) {
            continue;
        }
        const int64_t sz = ParseInt64(sizeStr);
        if (sz <= 0) {
            continue;
        }
        std::string exporter;
        (void)ReadSmallFile(base + "/exporter_name", exporter);
        totals.totalBytes += sz;
        totals.bufferCount++;
        if (ExporterMatchesType(exporter, type)) {
            totals.matchedBytes += sz;
        }
    }
    closedir(dir);
    return totals;
}

int64_t ScanPidDmabufs(int32_t pid, MemoryTrackerType type)
{
    const std::string fdinfoDir = "/proc/" + std::to_string(pid) + "/fdinfo";
    DIR *dir = opendir(fdinfoDir.c_str());
    if (dir == nullptr) {
        return 0;
    }
    int64_t sum = 0;
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        const std::string p = fdinfoDir + "/" + ent->d_name;
        FILE *fp = std::fopen(p.c_str(), "re");
        if (fp == nullptr) {
            continue;
        }
        int64_t sz = 0;
        std::string expName;
        char line[256];
        while (std::fgets(line, sizeof(line), fp) != nullptr) {
            if (std::strncmp(line, "size:", 5) == 0) {
                sz = std::strtoll(line + 5, nullptr, 10);
            } else if (std::strncmp(line, "exp_name:", 9) == 0) {
                const char *v = line + 9;
                while (*v == ' ' || *v == '\t') {
                    ++v;
                }
                expName.assign(v);
                while (!expName.empty() && (expName.back() == '\n' || expName.back() == '\r')) {
                    expName.pop_back();
                }
            }
        }
        std::fclose(fp);
        if (sz > 0 && !expName.empty() && ExporterMatchesType(expName, type)) {
            sum += sz;
        }
    }
    closedir(dir);
    return sum;
}

struct SmapsStats {
    int64_t pssBytes = 0;
    int64_t privateBytes = 0;
    int64_t sharedRssBytes = 0;
};

SmapsStats ScanPidSmapsRollup(int32_t pid)
{
    SmapsStats st {};
    const std::string path = "/proc/" + std::to_string(pid) + "/smaps_rollup";
    FILE *fp = std::fopen(path.c_str(), "re");
    if (fp == nullptr) {
        return st;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), fp) != nullptr) {
        long long kb = 0;
        if (std::sscanf(line, "Pss: %lld kB", &kb) == 1 && kb > 0) {
            st.pssBytes = static_cast<int64_t>(kb) * 1024LL;
        } else if (std::sscanf(line, "Private_Dirty: %lld kB", &kb) == 1 && kb > 0) {
            st.privateBytes += static_cast<int64_t>(kb) * 1024LL;
        } else if (std::sscanf(line, "Private_Clean: %lld kB", &kb) == 1 && kb > 0) {
            st.privateBytes += static_cast<int64_t>(kb) * 1024LL;
        } else if (std::sscanf(line, "Shared_Clean: %lld kB", &kb) == 1 && kb > 0) {
            st.sharedRssBytes += static_cast<int64_t>(kb) * 1024LL;
        } else if (std::sscanf(line, "Shared_Dirty: %lld kB", &kb) == 1 && kb > 0) {
            st.sharedRssBytes += static_cast<int64_t>(kb) * 1024LL;
        }
    }
    std::fclose(fp);
    return st;
}

int64_t ScanPidKgslSmaps(int32_t pid)
{
    const std::string path = "/proc/" + std::to_string(pid) + "/smaps";
    FILE *fp = std::fopen(path.c_str(), "re");
    if (fp == nullptr) {
        return 0;
    }
    int64_t kgslBytes = 0;
    bool inKgsl = false;
    char line[512];
    while (std::fgets(line, sizeof(line), fp) != nullptr) {
        if (std::strchr(line, '-') != nullptr && std::isxdigit(static_cast<unsigned char>(line[0]))) {
            inKgsl = (std::strstr(line, "kgsl") != nullptr || std::strstr(line, "dmabuf") != nullptr);
            continue;
        }
        if (inKgsl) {
            long long kb = 0;
            if (std::sscanf(line, "Pss: %lld kB", &kb) == 1 && kb > 0) {
                kgslBytes += static_cast<int64_t>(kb) * 1024LL;
            }
        }
    }
    std::fclose(fp);
    return kgslBytes;
}

int64_t ReadMeminfoFieldBytes(const char *key)
{
    FILE *fp = std::fopen("/proc/meminfo", "re");
    if (fp == nullptr) {
        return 0;
    }
    const size_t klen = std::strlen(key);
    int64_t bytes = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), fp) != nullptr) {
        if (std::strncmp(line, key, klen) == 0 && line[klen] == ':') {
            long long kb = std::strtoll(line + klen + 1, nullptr, 10);
            if (kb > 0) {
                bytes = static_cast<int64_t>(kb) * 1024LL;
            }
            break;
        }
    }
    std::fclose(fp);
    return bytes;
}

} // namespace

class MemoryTrackerInterfaceImpl : public IMemoryTrackerInterface {
public:
    MemoryTrackerInterfaceImpl() = default;
    ~MemoryTrackerInterfaceImpl() override = default;

    int32_t GetDevMem(int32_t pid, MemoryTrackerType type, std::vector<MemoryRecord> &records) override;
};

int32_t MemoryTrackerInterfaceImpl::GetDevMem(int32_t pid, MemoryTrackerType type,
                                              std::vector<MemoryRecord> &records)
{
    records.clear();
    if (pid < 0 || type < MEMORY_TRACKER_TYPE_MM || type >= MEMORY_TRACKER_TYPE_COUNTS) {
        return HDF_ERR_INVALID_PARAM;
    }

    if (pid == 0) {
        if (type == MEMORY_TRACKER_TYPE_OTHER) {
            const int64_t slab = ReadMeminfoFieldBytes("Slab");
            const int64_t shmem = ReadMeminfoFieldBytes("Shmem");
            records.push_back(MemoryRecord {FLAG_SYSTEM | FLAG_UNPROTECTED, slab});
            records.push_back(MemoryRecord {FLAG_SHARED_PSS | FLAG_MAPPED | FLAG_UNPROTECTED, shmem});
            return HDF_SUCCESS;
        }
        const DmabufTotals dmabuf = ScanGlobalDmabufs(type);
        const int64_t bytes = (type == MEMORY_TRACKER_TYPE_GRAPH) ? dmabuf.totalBytes : dmabuf.matchedBytes;
        records.push_back(MemoryRecord {
            FLAG_SHARED_PSS | FLAG_MAPPED | FLAG_UNPROTECTED,
            bytes
        });
        return HDF_SUCCESS;
    }

    /* Per-process accounting */
    if (type == MEMORY_TRACKER_TYPE_OTHER) {
        const SmapsStats st = ScanPidSmapsRollup(pid);
        records.push_back(MemoryRecord {FLAG_PRIVATE | FLAG_MAPPED | FLAG_UNPROTECTED, st.privateBytes});
        records.push_back(MemoryRecord {FLAG_SHARED_PSS | FLAG_MAPPED | FLAG_UNPROTECTED, st.pssBytes});
        return HDF_SUCCESS;
    }

    int64_t devBytes = ScanPidDmabufs(pid, type);
    if (type == MEMORY_TRACKER_TYPE_GL) {
        devBytes += ScanPidKgslSmaps(pid);
    }
    records.push_back(MemoryRecord {
        FLAG_SHARED_PSS | FLAG_MAPPED | FLAG_UNPROTECTED,
        devBytes
    });
    return HDF_SUCCESS;
}

extern "C" IMemoryTrackerInterface *MemoryTrackerInterfaceImplGetInstance(void)
{
    return new (std::nothrow) MemoryTrackerInterfaceImpl();
}

} // namespace V1_0
} // namespace Memorytracker
} // namespace HDI
} // namespace OHOS
