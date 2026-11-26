#!/bin/sh
# SPDX-License-Identifier: MIT
# Cross-link only. Never starts a player or contacts a device.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)
sdk=${HMOS_SDK:-$root/work/ohos-sdk/installed/native}
out=${AVPLAYER_PROBE_OUT:-$root/work/marble-avplayer-build}
sysroot=$sdk/sysroot
lib=$sysroot/usr/lib/aarch64-linux-ohos

if [ ! -x "$sdk/llvm/bin/clang" ]; then
    echo "avplayer-build: missing OHOS clang under $sdk/llvm/bin" >&2
    exit 2
fi
if [ ! -f "$sysroot/usr/include/multimedia/player_framework/avplayer.h" ]; then
    echo "avplayer-build: missing avplayer.h under $sysroot/usr/include" >&2
    exit 2
fi
for soname in libavplayer.so libnative_media_core.so; do
    if [ ! -f "$lib/$soname" ]; then
        echo "avplayer-build: missing $lib/$soname" >&2
        exit 2
    fi
done

mkdir -p -- "$out"
"$sdk/llvm/bin/clang" \
    --target=aarch64-linux-ohos --sysroot="$sysroot" \
    -std=c11 -O2 -Wall -Wextra -Werror -pthread \
    "$here/native_avplayer_probe.c" -o "$out/native_avplayer_probe" \
    -L"$lib" -lavplayer -lnative_media_core
printf 'AVPLAYER_PROBE_BUILD_OK out=%s/native_avplayer_probe\n' "$out"
printf '%s\n' 'OHOS runtime only: timeout -k 2 30 ./native_avplayer_probe /path/to/tones.wav (or tones.m4a)'
