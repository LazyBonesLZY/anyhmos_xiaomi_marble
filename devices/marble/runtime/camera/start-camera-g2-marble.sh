#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch G2: Prepare camera /dev permissions (root:dev_camera 0660) and bring up
# OHOS Camera HDI (camera_host host-9 -> libcamera_host_service_1.0.z.so -> marble VDI)
# and SA 3008 (camera_service). Strictly zero audio/modem/persist side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,camera,*|*,camera_hdi,*)
    say "CAMERA_G2_SKIP=requested"
    exit 0
    ;;
esac

# 1. Grant dev_camera (GID 3090) RW access to SM7475 camera V4L2/media nodes inside chroot.
cam_chown=0
for devp in "$R"/dev/video0 "$R"/dev/video1 "$R"/dev/media* "$R"/dev/v4l-subdev*; do
  [ -e "$devp" ] || continue
  $B chown 0:3090 "$devp" 2>/dev/null || true
  $B chmod 0660 "$devp" 2>/dev/null || true
  cam_chown=$((cam_chown + 1))
done
led_chown=0
for ledp in "$R"/sys/class/leds/led:torch_*/brightness "$R"/sys/class/leds/led:switch_*/brightness; do
  [ -e "$ledp" ] || continue
  $B chown 0:3090 "$ledp" 2>/dev/null || true
  $B chmod 0660 "$ledp" 2>/dev/null || true
  led_chown=$((led_chown + 1))
done
say "CAMERA_G2_DEV_CHOWN=$cam_chown CAMERA_G2_LED_CHOWN=$led_chown"

# 2. Verify staged Camera VDI, optional V1_2 host service, and camera_host_config.hcb in chroot.
if [ ! -f "$R/compat/lib64/libmarble_camera_vdi_impl.z.so" ]; then
  say "CAMERA_G2_ERR=missing_vdi_lib"
  exit 0
fi
if [ -f "$R/compat/lib64/libmarble_camera_host_service_1.2.z.so" ]; then
  say "CAMERA_G2_HOST_V12=staged"
fi
if [ ! -f "$R/vendor/etc/hdfconfig/camera_host_config.hcb" ]; then
  say "CAMERA_G2_ERR=missing_camera_host_config_hcb"
  exit 0
fi

# 3. Trigger OHOS init param:marble-camera-ready (starts camera_host & camera_service SA 3008).
if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.camera.ready true >/dev/null 2>&1 || say "CAMERA_G2_PARAM_RC=$?"
else
  say "CAMERA_G2_ERR=missing_param_bin"
  exit 0
fi

# 4. Wait up to 8s for camera_host (host-9) and camera_service (SA 3008) processes.
host_pid=""
sa_pid=""
i=0
while [ "$i" -lt 40 ]; do
  host_pid=$($B pgrep -f 'camera_host.*-i 9|hdf_devhost.*camera_host' 2>/dev/null | $B head -n 1 || true)
  sa_pid=$($B pgrep -x 'camera_service' 2>/dev/null | $B head -n 1 || true)
  [ -n "$sa_pid" ] || sa_pid=$($B pgrep -f 'sa_main.*/system/profile/camera_service\.json' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$host_pid" ] && [ -n "$sa_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "CAMERA_G2_PROCS host_pid=${host_pid:-none} sa3008_pid=${sa_pid:-none} wait_steps=$i"
say "CAMERA_G2_DONE=1"
exit 0
