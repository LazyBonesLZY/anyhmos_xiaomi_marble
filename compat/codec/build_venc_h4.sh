#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build compat/codec/native_venc_probe.c for Xiaomi marble (aarch64-linux-ohos).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="${HMOS_SDK:-$ROOT/work/ohos-sdk/installed/native}"
OUT="${H4_BUILD_OUT:-$ROOT/work/marble-h4-build}"
mkdir -p "$OUT"

"$SDK/llvm/bin/clang" \
    --target=aarch64-linux-ohos \
    --sysroot="$SDK/sysroot" \
    -O2 -Wall -Wextra -Werror \
    -I"$SDK/sysroot/usr/include" \
    "$ROOT/compat/codec/native_venc_probe.c" \
    -o "$OUT/native_venc_probe" \
    -L"$SDK/sysroot/usr/lib/aarch64-linux-ohos" \
    -lnative_media_codecbase -lnative_media_venc -lnative_media_vdec -lnative_media_core

"$SDK/llvm/bin/llvm-readelf" -d "$OUT/native_venc_probe" | grep -E 'NEEDED|RPATH|RUNPATH' || true
echo "VENC_PROBE_BUILD_OK out=$OUT/native_venc_probe"
