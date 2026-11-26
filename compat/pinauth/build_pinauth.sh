#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build pure-userspace software OHOS pin_auth v3.0 HDI for Xiaomi marble (SM7475):
#   - libpin_auth_interface_service_3.0.z.so (links donor libpin_auth_stub_3.0.z.so + libcrypto_openssl.z.so)
#   - libpin_auth_driver.z.so (HDF driverDesc -> drivers_peripheral_pin_auth)
#   - Offline host unit test exercising real OpenSSL PBKDF2/Ed25519 + PinIndexDb persistence
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="${HMOS_SDK:-$ROOT/work/ohos-sdk/installed/native}"
UPSTREAM="${HMOS_UPSTREAM:-$ROOT/work/upstream}"
DONOR_LINK="${HMOS_DONOR_LINK:-$ROOT/work/donor-hdi-link}"
DONOR_ROM="${HMOS_DONOR_ROM:-$ROOT/work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs}"
OPENSSL_INC="${HMOS_OPENSSL_INCLUDE:-$ROOT/work/openssl-inc}"
HDI_GEN="${HMOS_HDI_GEN:-$ROOT/work/marble-d-build/hdi-gen}"
OUT="${PINAUTH_BUILD_OUT:-$ROOT/work/marble-m3-build/pinauth}"

CLANG="$SDK/llvm/bin/clang"
CLANGPP="$SDK/llvm/bin/clang++"
READELF="$SDK/llvm/bin/llvm-readelf"
NM="$SDK/llvm/bin/llvm-nm"

PA_SRC="$UPSTREAM/drivers_peripheral/pin_auth/hdi_service"
PA_IDL="$UPSTREAM/drivers_interface/pin_auth/v3_0"
HDF="$UPSTREAM/drivers_hdf_core"
CHIPSET_SP="$DONOR_ROM/system/system/lib64/chipset-sdk-sp"
VENDOR_LIB="$DONOR_ROM/vendor/lib64"

mkdir -p "$OUT/generated" "$OUT/obj"

# 1. Generate pin_auth v3.0 C++ HDI headers matching donor libpin_auth_stub_3.0.z.so ABI
"$HDI_GEN" \
  -D "$PA_IDL" \
  -r "ohos.hdi:$UPSTREAM/drivers_interface" \
  -d "$OUT/generated" \
  -l cpp -m ipc -s full

INCS=(
  "-I$OPENSSL_INC"
  "-I$ROOT/compat/pinauth/include"
  "-I$OUT/generated"
  "-I$OUT/generated/pin_auth"
  "-I$PA_SRC/service/inc"
  "-I$PA_SRC/adaptor/inc"
  "-I$PA_SRC/common/inc"
  "-I$PA_SRC/database/inc"
  "-I$PA_SRC/main/inc"
  "-I$UPSTREAM/drivers_peripheral/pin_auth/common/logs"
  "-I$UPSTREAM/drivers_peripheral/pin_auth/common/utils"
  "-I$UPSTREAM/bounds_checking_function/include"
  "-I$UPSTREAM/hiviewdfx_hilog/interfaces/native/innerkits/include"
  "-I$UPSTREAM/commonlibrary_c_utils/base/include"
  "-I$UPSTREAM/communication_ipc/interfaces/innerkits/ipc_core/include"
  "-I$HDF/interfaces/inner_api/utils"
  "-I$HDF/interfaces/inner_api/hdi"
  "-I$HDF/interfaces/inner_api/hdi/base"
  "-I$HDF/interfaces/inner_api/host/shared"
  "-I$HDF/interfaces/inner_api/ipc"
  "-I$HDF/interfaces/inner_api/osal/uhdf"
  "-I$HDF/interfaces/inner_api/osal/shared"
)

COMMON_FLAGS=(
  "--target=aarch64-linux-ohos"
  "-O2"
  "-g"
  "-fPIC"
  "-Wall"
  "-Wextra"
  "-Wno-unused-parameter"
  "-Wno-deprecated-declarations"
  "${INCS[@]}"
)

CPP_FLAGS=(
  "-fno-rtti"
  "-std=c++17"
  "-nostdlib++"
  "-Wno-c++11-narrowing"
  "-include" "$ROOT/compat/platform/platform_cpp_config.h"
)

SERVICE_SOURCES=(
  "adaptor/src/adaptor_algorithm.c"
  "adaptor/src/adaptor_file.c"
  "adaptor/src/adaptor_memory.c"
  "adaptor/src/adaptor_time.c"
  "adaptor/src/buffer.c"
  "adaptor/src/file_operator.c"
  "common/src/attribute.c"
  "common/src/c_array.c"
  "database/src/pin_db.c"
  "database/src/pin_db_ops.c"
  "database/src/pin_db_ops_base.c"
  "database/src/pin_db_ops_v0.c"
  "database/src/pin_db_ops_v1.c"
  "main/src/all_in_one_func.c"
  "main/src/collector_func.c"
  "main/src/executor_func_common.c"
  "main/src/pin_auth.cpp"
  "main/src/verifier_func.c"
  "service/src/all_in_one_impl.cpp"
  "service/src/collector_impl.cpp"
  "service/src/executor_impl_common.cpp"
  "service/src/pin_auth_interface_service.cpp"
  "service/src/verifier_impl.cpp"
)

OBJS=()
for rel in "${SERVICE_SOURCES[@]}"; do
  src="$PA_SRC/$rel"
  obj="$OUT/obj/${rel//\//_}.o"
  OBJS+=("$obj")
  if [[ "$rel" == *.cpp ]]; then
    "$CLANGPP" "${COMMON_FLAGS[@]}" "${CPP_FLAGS[@]}" -c "$src" -o "$obj"
  else
    "$CLANG" "${COMMON_FLAGS[@]}" -std=gnu11 -c "$src" -o "$obj"
  fi
done

# 2. Link libpin_auth_interface_service_3.0.z.so
SERVICE_SO="$OUT/libpin_auth_interface_service_3.0.z.so"
"$CLANGPP" "${COMMON_FLAGS[@]}" "${CPP_FLAGS[@]}" \
  -shared -Wl,-z,defs -Wl,-soname,libpin_auth_interface_service_3.0.z.so \
  "${OBJS[@]}" \
  "$DONOR_LINK/libc++.so" \
  "$DONOR_LINK/libutils.z.so" \
  "$DONOR_LINK/libhilog.so" \
  "$CHIPSET_SP/libhdf_utils.z.so" \
  "$DONOR_LINK/libhdi.z.so" \
  "$DONOR_LINK/libipc_single.z.so" \
  "$DONOR_LINK/libbegetutil.z.so" \
  "$DONOR_LINK/libcrypto_openssl.z.so" \
  "$CHIPSET_SP/libsec_shared.z.so" \
  "$VENDOR_LIB/libpin_auth_stub_3.0.z.so" \
  -Wl,--allow-shlib-undefined \
  -o "$SERVICE_SO"

# 3. Compile & link libpin_auth_driver.z.so
DRV_INCS=()
while IFS= read -r d; do
  DRV_INCS+=("-I$d")
done < <(find "$HDF/interfaces/inner_api" -name '*.h' -not -path '*khdf*' -exec dirname {} \; | sort -u)
for hdr in hdf_device_desc.h hdf_sbuf_ipc.h hdf_object_manager.h hdf_device_section.h; do
  while IFS= read -r d; do
    DRV_INCS+=("-I$d")
  done < <(find "$HDF" -name "$hdr" -exec dirname {} \; | sort -u)
done

DRIVER_SO="$OUT/libpin_auth_driver.z.so"
"$CLANGPP" "${COMMON_FLAGS[@]}" "${DRV_INCS[@]}" "${CPP_FLAGS[@]}" \
  -D__USER__ -shared -Wl,-z,defs -Wl,-soname,libpin_auth_driver.z.so \
  "$PA_SRC/service/src/pin_auth_interface_driver.cpp" \
  "$SERVICE_SO" \
  "$VENDOR_LIB/libpin_auth_stub_3.0.z.so" \
  "$DONOR_LINK/libc++.so" \
  "$DONOR_LINK/libutils.z.so" \
  "$DONOR_LINK/libhilog.so" \
  "$CHIPSET_SP/libhdf_utils.z.so" \
  "$DONOR_LINK/libhdi.z.so" \
  "$DONOR_LINK/libipc_single.z.so" \
  "$CHIPSET_SP/libhdf_host.z.so" \
  "$CHIPSET_SP/libhdf_ipc_adapter.z.so" \
  -Wl,--allow-shlib-undefined \
  -o "$DRIVER_SO"

echo "=== libpin_auth_interface_service_3.0.z.so SONAME & NEEDED ==="
"$READELF" -d "$SERVICE_SO" | grep -E 'SONAME|NEEDED'
echo "=== libpin_auth_interface_service_3.0.z.so entry ==="
"$NM" -D --defined-only "$SERVICE_SO" | grep -E 'PinAuthInterfaceImplGetInstance'

echo "=== libpin_auth_driver.z.so SONAME & NEEDED ==="
"$READELF" -d "$DRIVER_SO" | grep -E 'SONAME|NEEDED'
echo "=== libpin_auth_driver.z.so entry ==="
"$NM" -D --defined-only "$DRIVER_SO" | grep -E 'driverDesc'

# 4. Build & run offline host unit test for the pure-userspace pin_auth C engine
HOST_C_SOURCES=(
  "$PA_SRC/adaptor/src/adaptor_algorithm.c"
  "$PA_SRC/adaptor/src/adaptor_file.c"
  "$PA_SRC/adaptor/src/adaptor_memory.c"
  "$PA_SRC/adaptor/src/adaptor_time.c"
  "$PA_SRC/adaptor/src/buffer.c"
  "$PA_SRC/common/src/attribute.c"
  "$PA_SRC/common/src/c_array.c"
  "$PA_SRC/database/src/pin_db.c"
  "$PA_SRC/database/src/pin_db_ops.c"
  "$PA_SRC/database/src/pin_db_ops_base.c"
  "$PA_SRC/database/src/pin_db_ops_v0.c"
  "$PA_SRC/database/src/pin_db_ops_v1.c"
  "$PA_SRC/main/src/all_in_one_func.c"
  "$PA_SRC/main/src/collector_func.c"
  "$PA_SRC/main/src/executor_func_common.c"
  "$PA_SRC/main/src/verifier_func.c"
  "$UPSTREAM/bounds_checking_function/src/memcpy_s.c"
  "$UPSTREAM/bounds_checking_function/src/memset_s.c"
  "$UPSTREAM/bounds_checking_function/src/memmove_s.c"
  "$UPSTREAM/bounds_checking_function/src/strcpy_s.c"
  "$UPSTREAM/bounds_checking_function/src/strncpy_s.c"
  "$UPSTREAM/bounds_checking_function/src/strcat_s.c"
  "$UPSTREAM/bounds_checking_function/src/sprintf_s.c"
  "$UPSTREAM/bounds_checking_function/src/snprintf_s.c"
  "$UPSTREAM/bounds_checking_function/src/vsprintf_s.c"
  "$UPSTREAM/bounds_checking_function/src/vsnprintf_s.c"
  "$UPSTREAM/bounds_checking_function/src/secureprintoutput_a.c"
  "$UPSTREAM/bounds_checking_function/src/securecutil.c"
  "$ROOT/compat/pinauth/host_test_pinauth.c"
)

cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-format \
  -I"$PA_SRC/adaptor/inc" \
  -I"$PA_SRC/common/inc" \
  -I"$PA_SRC/database/inc" \
  -I"$PA_SRC/main/inc" \
  -I"$UPSTREAM/drivers_peripheral/pin_auth/common/logs" \
  -I"$UPSTREAM/bounds_checking_function/include" \
  -isystem "$UPSTREAM/hiviewdfx_hilog/interfaces/native/innerkits/include" \
  -isystem "$HDF/interfaces/inner_api/utils" \
  -isystem "$HDF/interfaces/inner_api/osal/uhdf" \
  -isystem "$HDF/interfaces/inner_api/osal/shared" \
  "${HOST_C_SOURCES[@]}" \
  -lcrypto \
  -o "$OUT/pin_auth_host_test"

"$OUT/pin_auth_host_test"
echo "PINAUTH_BUILD_OK out=$OUT"
