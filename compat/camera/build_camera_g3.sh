#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch G3 SM7475 Camera artifacts for Xiaomi marble (aarch64-linux-ohos):
#   - work/marble-g3-build/marble_camera_hw_capture
#   - work/marble-g3-build/libmarble_camera_vdi_impl.z.so
#   - work/marble-g3-build/native_camera_hdi_probe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${G3_BUILD_OUT:-$ROOT/work/marble-g3-build}"
mkdir -p "$OUT"

SDK="$ROOT/work/ohos-sdk/installed/native"
CLANG="$SDK/llvm/bin/clang"
CLANGXX="$SDK/llvm/bin/clang++"
READELF="$SDK/llvm/bin/llvm-readelf"
NM="$SDK/llvm/bin/llvm-nm"
SYSROOT="$SDK/sysroot"
UAPI="$ROOT/work/marble-kernel-work/uapi-headers/include"
UP="$ROOT/work/upstream"
DONOR_FS="$ROOT/work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs"
LINK_DIR="$ROOT/work/donor-hdi-link"

META_DIR="$DONOR_FS/system/system/lib64/platformsdk"
SURF_DIR="$DONOR_FS/system/system/lib64/chipset-sdk-sp"
NIMG_DIR="$DONOR_FS/system/system/lib64"

INC_COMMON=(
  -I"$ROOT/compat/camera"
  -I"$ROOT/compat/camera/include"
  -I"$UAPI/camera"
  -I"$UP/drivers_interface/camera/sequenceable/buffer_producer"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/hdi"
  -I"work/upstream/drivers_hdf_core/interfaces/inner_api/host/uhdf"
  -I"."
  -I"$UP/drivers_hdf_core/interfaces/inner_api/utils"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/core"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/osal/uhdf"
  -I"$UP/drivers_hdf_core/framework/include/utils"
  -I"$UP/drivers_hdf_core/framework/include/osal"
  -I"$UP/drivers_hdf_core/framework/include/core"
  -I"$UP/drivers_peripheral/camera/vdi_base/interfaces"
  -I"$UP/drivers_peripheral/camera/vdi_base/interfaces/v1_0"
  -I"$UP/drivers_peripheral/camera/vdi_base/common/utils/include"
  -I"$UP/drivers_interface/camera/metadata/include"
  -I"$UP/drivers_interface/camera/v1_0"
  -I"$UP/communication_ipc/interfaces/innerkits/ipc_core/include"
  -I"$UP/commonlibrary_c_utils/base/include"
  -I"$UP/foundation_systemabilitymgr_samgr/interfaces/innerkits/samgr_proxy/include"
  -I"$UP/graphic_graphic_surface/interfaces/inner_api/surface"
  -I"$UP/graphic_graphic_surface/interfaces/inner_api/common"
  -I"$SYSROOT/usr/include"
)

CXX_ABI=(
  --target=aarch64-linux-ohos
  --sysroot="$SYSROOT"
  -O2 -std=c++17
  -DOHOS_PLATFORM
  -include "$ROOT/compat/platform/platform_cpp_config.h"
  -fno-rtti -fno-exceptions
  -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-c99-designator
)

# 1. C11 SM7475 hardware capture engine + standalone CLI
"$CLANG" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -O2 -fPIC -std=gnu11 -Wall -Wextra -Werror \
  -I"$UAPI/camera" -I"$ROOT/compat/camera" \
  -c "$ROOT/compat/camera/marble_isp_capture.c" -o "$OUT/marble_isp_capture.o"

"$CLANG" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -O2 -std=gnu11 -Wall -Wextra -Werror \
  -I"$UAPI/camera" -I"$ROOT/compat/camera" \
  "$OUT/marble_isp_capture.o" "$ROOT/compat/camera/marble_camera_hw_capture.c" \
  -o "$OUT/marble_camera_hw_capture"

# 2. OHOS Camera VDI 1.0 shared library (wired to live SM7475 IMX355 + TPG0 capture)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/camera/marble_camera_vdi_impl.cpp" -o "$OUT/marble_camera_vdi_impl.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libmarble_camera_vdi_impl.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_camera_vdi_impl.o" "$OUT/marble_isp_capture.o" \
  -L"$META_DIR" -L"$SURF_DIR" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lmetadata.z -lsurface.z -lutils.z -lipc_single.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_camera_vdi_impl.z.so"

# 3. OHOS Camera HDI Binder/ConsumerSurface probe (validates lcam001 TPG0 + lcam003 IMX355 optical)
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" -nostdlib++ \
  "$ROOT/compat/camera/native_camera_hdi_probe.cpp" \
  -L"$META_DIR" -L"$SURF_DIR" -L"$NIMG_DIR" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lmetadata.z -lnative_image -lsurface.z -lutils.z -lipc_single.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_camera_hdi_probe"

echo "=== VDI ELF & VTABLE AUDIT ==="
"$READELF" -d "$OUT/libmarble_camera_vdi_impl.z.so" | grep -E 'NEEDED|SONAME'
"$NM" -D "$OUT/libmarble_camera_vdi_impl.z.so" | grep hdfVdiDesc
"$READELF" -sW "$OUT/libmarble_camera_vdi_impl.z.so" | grep -E '_ZTV.*Marble(CameraHost|CameraDevice|StreamOperator)Vdi'
echo "=== PROBE ELF AUDIT ==="
"$READELF" -d "$OUT/native_camera_hdi_probe" | grep NEEDED
echo "=== HW CAPTURE ELF AUDIT ==="
"$READELF" -d "$OUT/marble_camera_hw_capture" | grep NEEDED
ls -la "$OUT/libmarble_camera_vdi_impl.z.so" "$OUT/native_camera_hdi_probe" "$OUT/marble_camera_hw_capture"
