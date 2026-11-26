#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch M4: Bring up OHOS Motion HDI v1.1 host-18 (motion_host) for raise-to-wake (pickup)
# backed by Qualcomm SLPI tilt_to_wake / 25 Hz accel fallback.
# Strictly zero audio/modem/persist side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,motion,*|*,motion_hdi,*|*,pickup,*)
    say "MOTION_M4_SKIP=requested"
    exit 0
    ;;
esac

# 1. Verify staged Motion HDI v1.1 service library and bind-mount over donor vendor service.
SRC_LIB="$R/compat/lib64/libmarble_motion_interface_service_1.1.z.so"
DST_LIB="$R/vendor/lib64/libmotion_interface_service_1.1.z.so"
if [ ! -f "$SRC_LIB" ]; then
  say "MOTION_M4_ERR=missing_motion_hdi_lib"
  exit 0
fi
if [ ! -f "$DST_LIB" ]; then
  say "MOTION_M4_ERR=missing_donor_motion_lib"
  exit 0
fi

if ! $B grep -q " $DST_LIB " /proc/mounts 2>/dev/null; then
  if ! $B mount --bind "$SRC_LIB" "$DST_LIB" 2>/dev/null; then
    say "MOTION_M4_ERR=bind_mount_failed"
    exit 0
  fi
fi
say "MOTION_M4_BIND_OK dst=$DST_LIB"

# 2. Trigger OHOS init param:marble-motion-ready (starts motion_host host-18).
if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.motion.ready true >/dev/null 2>&1 || say "MOTION_M4_PARAM_RC=$?"
else
  say "MOTION_M4_ERR=missing_param_bin"
  exit 0
fi

# 3. Wait up to 6s for motion_host (host-18) process.
host_pid=""
i=0
while [ "$i" -lt 30 ]; do
  host_pid=$($B pgrep -f 'motion_host.*-i 18|hdf_devhost.*motion_host' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$host_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "MOTION_M4_PROCS host_pid=${host_pid:-none} wait_steps=$i"
say "MOTION_M4_DONE=1"
exit 0
