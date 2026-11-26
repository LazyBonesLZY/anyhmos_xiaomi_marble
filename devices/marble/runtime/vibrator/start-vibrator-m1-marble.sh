#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch M1: Bring up OHOS Vibrator / Haptics HDI v2.0 host-15 (vibrator_host)
# backed by SM7475 qcom-hv-haptics (/dev/input/event* FF_CONSTANT).
# Strictly zero audio/modem/persist side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,vibrator,*|*,haptics,*|*,vibrator_hdi,*)
    say "VIBRATOR_M1_SKIP=requested"
    exit 0
    ;;
esac

# 1. Discover SM7475 haptics /dev/input/event* node (live marble: event6, qcom-hv-haptics).
haptics_ev=""
haptics_name=""
for namef in /sys/class/input/event*/device/name; do
  [ -f "$namef" ] || continue
  nm=$($B cat "$namef" 2>/dev/null | $B tr -d '\r\n' || true)
  nm_lc=$(echo "$nm" | $B tr 'A-Z' 'a-z')
  case "$nm_lc" in
    *haptics*|*vibrator*)
      evdir=$($B dirname "$($B dirname "$namef")")
      haptics_ev=$($B basename "$evdir")
      haptics_name="$nm"
      break
      ;;
  esac
done

if [ -z "$haptics_ev" ]; then
  say "VIBRATOR_M1_ERR=no_haptics_input_device"
  exit 0
fi

# Ensure $R/dev/input/<eventN> exists inside HMOS chroot and is writable by vibrator_host (3034).
$B mkdir -p "$R/dev/input" 2>/dev/null || true
if [ ! -c "$R/dev/input/$haptics_ev" ]; then
  dev_mm=$($B cat "/sys/class/input/$haptics_ev/dev" 2>/dev/null | $B tr -d ' \t\r\n' || true)
  if [ -n "$dev_mm" ]; then
    maj=${dev_mm%:*}
    min=${dev_mm#*:}
    $B mknod -m 0660 "$R/dev/input/$haptics_ev" c "$maj" "$min" 2>/dev/null || true
  fi
fi
if [ -c "$R/dev/input/$haptics_ev" ]; then
  $B chown 3034:3034 "$R/dev/input/$haptics_ev" 2>/dev/null || true
  $B chmod 0660 "$R/dev/input/$haptics_ev" 2>/dev/null || true
fi
say "VIBRATOR_M1_DEV_READY event=/dev/input/$haptics_ev name=$haptics_name"

# 2. Verify staged Vibrator HDI library and bind-mount over donor vendor service.
SRC_LIB="$R/compat/lib64/libmarble_vibrator_interface_service_2.0.z.so"
DST_LIB="$R/vendor/lib64/libvibrator_interface_service_2.0.z.so"
if [ ! -f "$SRC_LIB" ]; then
  say "VIBRATOR_M1_ERR=missing_vibrator_hdi_lib"
  exit 0
fi
if [ ! -f "$DST_LIB" ]; then
  say "VIBRATOR_M1_ERR=missing_donor_vibrator_lib"
  exit 0
fi

if ! $B grep -q " $DST_LIB " /proc/mounts 2>/dev/null; then
  if ! $B mount --bind "$SRC_LIB" "$DST_LIB" 2>/dev/null; then
    say "VIBRATOR_M1_ERR=bind_mount_failed"
    exit 0
  fi
fi
say "VIBRATOR_M1_BIND_OK dst=$DST_LIB"

# 3. Trigger OHOS init param:marble-vibrator-ready (starts vibrator_host host-15).
if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.vibrator.ready true >/dev/null 2>&1 || say "VIBRATOR_M1_PARAM_RC=$?"
else
  say "VIBRATOR_M1_ERR=missing_param_bin"
  exit 0
fi

# 4. Wait up to 8s for vibrator_host (host-15) process.
host_pid=""
i=0
while [ "$i" -lt 40 ]; do
  host_pid=$($B pgrep -f 'vibrator_host.*-i 15|hdf_devhost.*vibrator_host' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$host_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "VIBRATOR_M1_PROCS host_pid=${host_pid:-none} wait_steps=$i"
say "VIBRATOR_M1_DONE=1"
exit 0
