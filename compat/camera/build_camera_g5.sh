#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch G5 Camera (Real-time closed-loop AE + GT9764 VCM AF actuator + zero meta err):
#   - work/marble-g5-build/marble_camera_hw_capture
#   - work/marble-g5-build/libmarble_camera_vdi_impl.z.so
#   - work/marble-g5-build/native_camera_hdi_probe
#   - work/marble-g5-build/ohcamera_fwk_probe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export G4_BUILD_OUT="${G5_BUILD_OUT:-$ROOT/work/marble-g5-build}"
exec bash "$ROOT/compat/camera/build_camera_g4.sh"
