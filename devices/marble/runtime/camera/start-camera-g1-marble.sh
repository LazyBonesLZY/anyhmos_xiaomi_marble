#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch G1: Qualcomm SM7475 (Xiaomi marble) Camera kernel stack + LX7 ICP FW + 4-sensor HW probe.
# Strictly zero audio/modem/persist side effects; leaves all camera sensors powered down after probe.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
KDIR=/hmos/vendor_dlkm/lib/modules
[ -d "$KDIR" ] || KDIR="$R/vendor_dlkm/lib/modules"
[ -d "$KDIR" ] || KDIR=/lib/modules

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,camera,*)
    say "CAMERA_G1_SKIP=requested"
    exit 0
    ;;
esac

# 1. Verify CAMERA_ICP MDT firmware in /compat/firmware and set firmware_class search path.
$B mkdir -p /compat/firmware "$R/compat/firmware"
if [ ! -f /compat/firmware/CAMERA_ICP.mdt ] && [ -f "$R/compat/firmware/CAMERA_ICP.mdt" ]; then
  for fw in "$R"/compat/firmware/CAMERA_ICP.*; do
    [ -f "$fw" ] || continue
    $B cp "$fw" "/compat/firmware/${fw##*/}" 2>/dev/null || true
    $B chmod 0644 "/compat/firmware/${fw##*/}" 2>/dev/null || true
  done
fi
icp_cnt=$($B ls -1 /compat/firmware/CAMERA_ICP.* 2>/dev/null | $B wc -l | $B tr -d ' \t\r\n' || echo 0)
say "CAMERA_ICP_FW_COUNT=${icp_cnt:-0}"
if [ "${icp_cnt:-0}" -lt 23 ] || [ ! -f /compat/firmware/CAMERA_ICP.mdt ]; then
  say "CAMERA_G1_WARN=missing_camera_icp_fw"
fi
if $B test -w /sys/module/firmware_class/parameters/path; then
  $B printf '/compat/firmware' > /sys/module/firmware_class/parameters/path 2>/dev/null || true
fi

# 2. Load SM7475 camera kernel modules in dependency order.
load_ko() {
  ko="$1"
  mod=$(printf '%s' "${ko%.ko}" | $B tr - _)
  if $B grep -q "^$mod " /proc/modules 2>/dev/null; then
    say "CAMERA_KO $ko=present"
    return 0
  fi
  if [ -f "$KDIR/$ko" ] && $B insmod "$KDIR/$ko" 2>/dev/null; then
    say "CAMERA_KO $ko=loaded"
    return 0
  fi
  say "CAMERA_KO $ko=failed"
  return 1
}

for ko in qcom_ipc_lite.ko synx-driver.ko leds-qti-flash.ko camcc-waipio.ko camera.ko; do
  load_ko "$ko" || {
    say "CAMERA_G1_ERR=mod_load_failed_$ko"
    exit 0
  }
done

# 3. Wait up to 4s for cam-req-mgr (/dev/video0), media0, and v4l-subdev22 (cam-icp).
i=0
while [ "$i" -lt 20 ]; do
  if [ -r /sys/class/video4linux/video0/dev ] && \
     [ -r /sys/bus/media/devices/media0/dev ] && \
     [ -r /sys/class/video4linux/v4l-subdev22/dev ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

mk_cam_node() {
  sysdev="$1"
  name="$2"
  [ -r "$sysdev/dev" ] || return 0
  mm=$($B cat "$sysdev/dev" 2>/dev/null || true)
  maj=${mm%:*}
  min=${mm#*:}
  [ -n "$maj" ] && [ -n "$min" ] || return 0
  want=$($B printf '%x:%x' "$maj" "$min")
  for root in "" "$R"; do
    devp="$root/dev/$name"
    if [ -e "$devp" ]; then
      have=$($B stat -c '%t:%T' "$devp" 2>/dev/null || echo "")
      if [ "$have" != "$want" ]; then
        $B rm -f "$devp"
        $B mknod "$devp" c "$maj" "$min" 2>/dev/null || true
      fi
    else
      $B mknod "$devp" c "$maj" "$min" 2>/dev/null || true
    fi
    $B chmod 0660 "$devp" 2>/dev/null || true
  done
}

created=0
for d in /sys/class/video4linux/video0 /sys/class/video4linux/video1 /sys/class/video4linux/v4l-subdev*; do
  [ -r "$d/dev" ] || continue
  mk_cam_node "$d" "${d##*/}"
  created=$((created + 1))
done
for d in /sys/bus/media/devices/media*; do
  [ -r "$d/dev" ] || continue
  mk_cam_node "$d" "${d##*/}"
  created=$((created + 1))
done
say "CAMERA_NODES_READY count=$created wait_steps=$i"

# 4. Run clean-room hardware probe (LX7 ICP FW boot + CPAS/ISP/JPEG/CSIPHY/TPG + 4 physical CCI sensors).
PROBE_LOG="$R/data/local/tmp/marble-camera-g1.log"
if [ -x "$R/compat/bin/marble_camera_probe" ]; then
  $B timeout 25 $B chroot "$R" /compat/bin/marble_camera_probe --all > "$PROBE_LOG" 2>&1 || say "CAMERA_PROBE_RC=$?"
  $B chmod 0644 "$PROBE_LOG" 2>/dev/null || true
  summary=$($B grep '^MARBLE_CAMERA_PROBE_SUMMARY ' "$PROBE_LOG" 2>/dev/null | $B tail -n 1 || true)
  [ -n "$summary" ] && say "$summary"
else
  say "CAMERA_PROBE_SKIP=binary_missing"
fi

say "CAMERA_G1_DONE=1"
exit 0
