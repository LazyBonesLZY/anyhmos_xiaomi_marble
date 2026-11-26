#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch H5 SM7475 Hardware MemoryTracker HDI v1.0 artifacts for Xiaomi marble (aarch64-linux-ohos):
#   - work/marble-h5-build/libmarble_memory_tracker_interface_service_1.0.z.so
#   - work/marble-h5-build/native_memorytracker_hdi_probe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${H5_BUILD_OUT:-$ROOT/work/marble-h5-build}"
mkdir -p "$OUT"

SDK="$ROOT/work/ohos-sdk/installed/native"
CLANGXX="$SDK/llvm/bin/clang++"
READELF="$SDK/llvm/bin/llvm-readelf"
SYSROOT="$SDK/sysroot"
UP="$ROOT/work/upstream"
DONOR_FS="$ROOT/work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs"
LINK_DIR="$ROOT/work/donor-hdi-link"

SYS_LIB="$DONOR_FS/system/system/lib64"
CHIP_SP="$DONOR_FS/system/system/lib64/chipset-sdk-sp"
PLAT_LIB="$DONOR_FS/system/system/lib64/platformsdk"

INC_COMMON=(
  -I"$ROOT/compat/memorytracker/include"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/hdi"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/utils"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/core"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/osal/uhdf"
  -I"$UP/drivers_hdf_core/framework/include/utils"
  -I"$UP/drivers_hdf_core/framework/include/osal"
  -I"$UP/drivers_hdf_core/framework/include/core"
  -I"$UP/communication_ipc/interfaces/innerkits/ipc_core/include"
  -I"$UP/commonlibrary_c_utils/base/include"
  -I"$UP/foundation_systemabilitymgr_samgr/interfaces/innerkits/samgr_proxy/include"
  -I"$SYSROOT/usr/include"
)

CXX_ABI=(
  --target=aarch64-linux-ohos
  --sysroot="$SYSROOT"
  -O2 -std=c++17
  -DOHOS_PLATFORM
  -include "$ROOT/compat/platform/platform_cpp_config.h"
  -fno-rtti -fno-exceptions
  -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations
)

# 1. Clean-room OHOS MemoryTracker HDI v1.0 implementation (SONAME libmemory_tracker_interface_service_1.0.z.so)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/memorytracker/marble_memorytracker_hdi_impl.cpp" -o "$OUT/marble_memorytracker_hdi_impl.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libmemory_tracker_interface_service_1.0.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_memorytracker_hdi_impl.o" \
  -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lutils.z "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_memory_tracker_interface_service_1.0.z.so"

# 2. End-to-end Binder probe linked against unmodified donor libmemorytracker_proxy_1.0.z.so
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" \
  -c "$ROOT/compat/memorytracker/native_memorytracker_hdi_probe.cpp" -o "$OUT/native_memorytracker_hdi_probe.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -pie \
  -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/native_memorytracker_hdi_probe.o" \
  -L"$CHIP_SP" -L"$PLAT_LIB" -L"$SYS_LIB" -L"$LINK_DIR" \
  -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lmemorytracker_proxy_1.0.z -lipc_single.z -lutils.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_memorytracker_hdi_probe"

echo "=== libmarble_memory_tracker_interface_service_1.0.z.so NEEDED & VTABLE ==="
"$READELF" -d "$OUT/libmarble_memory_tracker_interface_service_1.0.z.so" | grep -E 'SONAME|NEEDED'
"$READELF" -sW "$OUT/libmarble_memory_tracker_interface_service_1.0.z.so" | grep -E '_ZTVN4OHOS3HDI13Memorytracker4V1_026MemoryTrackerInterfaceImplE|MemoryTrackerInterfaceImplGetInstance'
echo "H5_MEMORYTRACKER_BUILD_OK out=$OUT"
