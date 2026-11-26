#!/bin/sh
# SPDX-License-Identifier: MIT
# Build compat/codec/native_codec_probe.c for marble (aarch64-linux-ohos) with the
# local OHOS NDK. Output goes to work/marble-codec-build/ (host scratch, not a
# package input). Never touches a device.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
sdk=${HMOS_SDK:-$root/work/ohos-sdk/installed/native}
out=${CODEC_PROBE_OUT:-$root/work/marble-codec-build}

if [ ! -x "$sdk/llvm/bin/clang" ]; then
    echo "probe-build: missing OHOS clang under $sdk/llvm/bin" >&2
    exit 2
fi
if [ ! -f "$sdk/sysroot/usr/include/multimedia/player_framework/native_avcapability.h" ]; then
    echo "probe-build: missing AVCodec headers under $sdk/sysroot/usr/include" >&2
    exit 2
fi

mkdir -p "$out"
"$sdk/llvm/bin/clang" \
    --target=aarch64-linux-ohos \
    --sysroot="$sdk/sysroot" \
    -O2 -Wall -Wextra -Werror \
    -I"$sdk/sysroot/usr/include" \
    "$here/native_codec_probe.c" \
    -o "$out/native_codec_probe" \
    -L"$sdk/sysroot/usr/lib/aarch64-linux-ohos" \
    -lnative_media_codecbase -lnative_media_vdec -lnative_media_core

echo "PROBE_BUILD_OK out=$out/native_codec_probe"