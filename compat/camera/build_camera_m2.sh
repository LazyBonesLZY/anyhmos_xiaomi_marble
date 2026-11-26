#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch M2 Camera HDI V1_2 + Torch + Camera App fixes for Xiaomi marble (aarch64-linux-ohos):
#   - work/marble-m2-build/camera/marble_camera_hw_capture
#   - work/marble-m2-build/camera/libmarble_camera_vdi_impl.z.so
#   - work/marble-m2-build/camera/libmarble_camera_host_service_1.2.z.so
#   - work/marble-m2-build/camera/native_camera_hdi_probe
#   - work/marble-m2-build/camera/ohcamera_fwk_probe
#   + offline host unit tests (marble_camera_hw_capture_host, marble_camera_vdi_host_test, native_camera_hdi_probe_host)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${M2_BUILD_OUT:-$ROOT/work/marble-m2-build/camera}"
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
VEND_DIR="$DONOR_FS/vendor/lib64"

INC_COMMON=(
  -I"$ROOT/compat/camera"
  -I"$ROOT/compat/camera/include"
  -I"$UAPI/camera"
  -I"$UP/drivers_interface/camera/sequenceable/buffer_producer"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/hdi"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/host/uhdf"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/host/shared"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/ipc"
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
  -I"$ROOT"
  -I"$SYSROOT/usr/include"
)

CXX_ABI=(
  --target=aarch64-linux-ohos
  --sysroot="$SYSROOT"
  -O2 -std=c++17
  -DOHOS_PLATFORM
  -include "$ROOT/compat/platform/platform_cpp_config.h"
  -fno-rtti -fno-exceptions
  -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations -Wno-c99-designator
)

# 1. C11 SM7475 all-4 physical sensor capture engine + standalone CLI
"$CLANG" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -O2 -fPIC -std=gnu11 -Wall -Wextra -Werror \
  -I"$UAPI/camera" -I"$ROOT/compat/camera" \
  -c "$ROOT/compat/camera/marble_isp_capture.c" -o "$OUT/marble_isp_capture.o"

"$CLANG" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -O2 -std=gnu11 -Wall -Wextra -Werror \
  -I"$UAPI/camera" -I"$ROOT/compat/camera" \
  "$OUT/marble_isp_capture.o" "$ROOT/compat/camera/marble_camera_hw_capture.c" \
  -o "$OUT/marble_camera_hw_capture"

# 2. OHOS Camera VDI 1.0 shared library (all 4 optical sensors + multi-stream resize + JPEG ExtraData + 3A + modes 0/1/2)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/camera/marble_camera_vdi_impl.cpp" -o "$OUT/marble_camera_vdi_impl.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libmarble_camera_vdi_impl.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_camera_vdi_impl.o" "$OUT/marble_isp_capture.o" \
  -L"$META_DIR" -L"$SURF_DIR" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lmetadata.z -lsurface.z -lutils.z -lipc_single.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_camera_vdi_impl.z.so"

# 3. Clean-room OHOS Camera HDI V1_2 HDF host service (SONAME libcamera_host_service_1.0.z.so)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/camera/marble_camera_host_service_1.2.cpp" -o "$OUT/marble_camera_host_service_1.2.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libcamera_host_service_1.0.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_camera_host_service_1.2.o" \
  -L"$VEND_DIR" -L"$SURF_DIR" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -Wl,--no-as-needed -lcamera_stub_1.2.z -Wl,--as-needed \
  -lhdf_host.z -lhdf_ipc_adapter.z -lhdf_utils.z \
  -lhdi.z -lipc_single.z -lutils.z -lhilog "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_camera_host_service_1.2.z.so"

# 4. OHOS Camera HDI Binder/ConsumerSurface probe (V1_0 + V1_2 GetVersion/SetFlashlight_V1_2)
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" -nostdlib++ \
  "$ROOT/compat/camera/native_camera_hdi_probe.cpp" \
  -L"$META_DIR" -L"$SURF_DIR" -L"$NIMG_DIR" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lmetadata.z -lnative_image -lsurface.z -lutils.z -lipc_single.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_camera_hdi_probe"

# 5. Public OHOS NDK libohcamera.so -> SA 3008 framework probe (Torch + NORMAL_PHOTO + NORMAL_VIDEO)
"$CLANGXX" "${CXX_ABI[@]}" -nostdlib++ \
  "$ROOT/compat/camera/ohcamera_fwk_probe.cpp" \
  -L"$SYSROOT/usr/lib/aarch64-linux-ohos" -L"$LINK_DIR" \
  -lohcamera "$LINK_DIR/libc++.so" \
  -o "$OUT/ohcamera_fwk_probe"

# 6. Build & run strict offline host unit tests
gcc -O2 -std=gnu11 -Wall -Wextra -Werror \
  -I"$UAPI/camera" -I"$ROOT/compat/camera" \
  "$ROOT/compat/camera/marble_isp_capture.c" "$ROOT/compat/camera/marble_camera_hw_capture.c" \
  -lm -o "$OUT/marble_camera_hw_capture_host"

g++ -O2 -std=c++17 -Wall -Wextra -Werror -DMARBLE_CAMERA_VDI_HOST_TEST \
  -I"$ROOT/compat/camera" \
  "$ROOT/compat/camera/marble_camera_vdi_impl.cpp" \
  -o "$OUT/marble_camera_vdi_host_test"

g++ -O2 -std=c++17 -Wall -Wextra -Werror -DMARBLE_CAMERA_HDI_PROBE_HOST_TEST \
  "$ROOT/compat/camera/native_camera_hdi_probe.cpp" \
  -o "$OUT/native_camera_hdi_probe_host"

echo "=== RUNNING HOST UNIT TESTS ==="
"$OUT/marble_camera_hw_capture_host" --self-test
"$OUT/marble_camera_vdi_host_test"
"$OUT/native_camera_hdi_probe_host"

echo "=== VDI ELF & VTABLE AUDIT ==="
"$READELF" -d "$OUT/libmarble_camera_vdi_impl.z.so" | grep -E 'NEEDED|SONAME'
"$NM" -D "$OUT/libmarble_camera_vdi_impl.z.so" | grep -E 'hdfVdiDesc|marble_hw_session_open|marble_encode_baseline_jpeg_rgb24'
"$READELF" -sW "$OUT/libmarble_camera_vdi_impl.z.so" | grep -E '_ZTV.*Marble(CameraHost|CameraDevice|StreamOperator)Vdi'

echo "=== HOST V1_2 ELF & VTABLE AUDIT ==="
"$READELF" -d "$OUT/libmarble_camera_host_service_1.2.z.so" | grep -E 'NEEDED|SONAME'
"$NM" -D "$OUT/libmarble_camera_host_service_1.2.z.so" | grep driverDesc
"$READELF" -sW "$OUT/libmarble_camera_host_service_1.2.z.so" | grep -E '_ZTV.*Marble(CameraHost|CameraDevice|StreamOperator)ServiceV1_2'

echo "=== PROBE & CLI ELF AUDITS ==="
"$READELF" -d "$OUT/native_camera_hdi_probe" | grep NEEDED
"$READELF" -d "$OUT/ohcamera_fwk_probe" | grep NEEDED
"$READELF" -d "$OUT/marble_camera_hw_capture" | grep NEEDED

echo "=== ARTIFACT SIZES & SHA256 ==="
ls -la "$OUT/libmarble_camera_vdi_impl.z.so" \
       "$OUT/libmarble_camera_host_service_1.2.z.so" \
       "$OUT/marble_camera_hw_capture" \
       "$OUT/native_camera_hdi_probe" \
       "$OUT/ohcamera_fwk_probe"
sha256sum "$OUT/libmarble_camera_vdi_impl.z.so" \
          "$OUT/libmarble_camera_host_service_1.2.z.so" \
          "$OUT/marble_camera_hw_capture" \
          "$OUT/native_camera_hdi_probe" \
          "$OUT/ohcamera_fwk_probe"
