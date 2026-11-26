#!/busybox sh
set -eu
B=/busybox
R=/hmos-root
D=${MARBLE_SENSOR_PKG:-/graphics}
ST="$R/data/service/el1/public/marble_sensors"
L="$R/system/lib/ld-musl-aarch64.so.1"

if [ "${1-}" = private ]; then
 "$B" mount --make-rprivate /
 "$B" mkdir -p "$ST"
 exec 9>"$ST/launcher.lock"
 "$B" flock -n 9 || {
  echo "MARBLE_SENSORSPD_ALREADY_RUNNING=1"
  exit 0
 }
 "$B" touch "$ST/startup.log"
 exec >"$ST/startup.log" 2>&1
 RT="$ST/runtime"
 RR="$RT/root"
 "$B" umount -l "$RR/mnt/vendor/persist/sensors" 2>/dev/null || true
 "$B" umount -l "$RT" 2>/dev/null || true
 "$B" mkdir -p "$RT"
 "$B" mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs "$RT"
 "$B" mkdir -m 0755 "$RR"
 "$B" tar -xzf "$D/marble-sensors-root.tar.gz" -C "$RR"
 "$B" mount --bind "$ST/sensors" "$RR/mnt/vendor/persist/sensors"
 for n in sys proc; do
  "$B" mount --bind "/$n" "$RR/$n"
  "$B" mount -o remount,bind,ro "$RR/$n"
 done
 "$B" mount --bind "$R/dev" "$RR/dev"
 for n in dsp vendor/dsp vendor/lib/rfsa/adsp vendor/lib/rfsa/dsp; do
  if [ -d /compat/original-firmware/image ]; then
   "$B" mount --bind /compat/original-firmware/image "$RR/$n"
   "$B" mount -o remount,bind,ro "$RR/$n"
  fi
 done
 LD_PRELOAD= LD_LIBRARY_PATH="$R/system/lib64" exec "$L" "$R/compat/bin/native_sensorspd_launcher" "$RR"
fi

echo "MARBLE_SENSORS_STAGE=start"
# 1. Stage SLPI firmware & PD maps if not already copied in early stage.
if [ -d /compat/original-firmware/image ]; then
 "$B" mkdir -p /compat/firmware /compat/pd-maps "$R/compat/firmware"
 for f in /compat/original-firmware/image/slpi2.* /compat/original-firmware/image/slpi*.jsn; do
  [ -f "$f" ] || continue
  base=${f##*/}
  "$B" cp "$f" "/compat/firmware/$base" 2>/dev/null || true
  "$B" cp "$f" "$R/compat/firmware/$base" 2>/dev/null || true
  "$B" chmod 0644 "/compat/firmware/$base" "$R/compat/firmware/$base" 2>/dev/null || true
  case "$base" in *.jsn) "$B" cp "$f" "/compat/pd-maps/$base" 2>/dev/null || true ;; esac
 done
fi

# 2. Ensure SSC kernel module is loaded.
if ! "$B" grep -q '^sensors_ssc ' /proc/modules 2>/dev/null; then
 for ko in /lib/modules/sensors_ssc.ko "$R/vendor_dlkm/lib/modules/sensors_ssc.ko"; do
  if [ -f "$ko" ]; then
   "$B" insmod "$ko" >/dev/null 2>&1 && break || true
  fi
 done
fi
echo "MARBLE_SENSORS_SSC_MOD=$($B grep -q '^sensors_ssc ' /proc/modules 2>/dev/null && echo loaded || echo missing)"

# 2b. Load FastRPC driver BEFORE booting SLPI so dsps rpmsg/PDR bind cleanly.
if [ ! -d /sys/module/frpc_adsprpc ]; then
 for ko in /lib/modules/frpc-adsprpc.ko "$R/vendor_dlkm/lib/modules/frpc-adsprpc.ko"; do
  if [ -f "$ko" ]; then
   "$B" insmod "$ko" >/dev/null 2>&1 && break || true
  fi
 done
fi
echo "MARBLE_SENSORS_FASTRPC_MOD=$([ -d /sys/module/frpc_adsprpc ] && echo loaded || echo missing)"

# 3. Configure SLPI recovery=enabled and coredump=disabled BEFORE booting SLPI.
slpi=""
for r in /sys/class/remoteproc/remoteproc*; do
 [ -r "$r/name" ] || continue
 if [ "$($B cat "$r/name" 2>/dev/null || true)" = "2400000.remoteproc-slpi" ]; then
  slpi="$r"
  break
 fi
done
if [ -n "$slpi" ]; then
 [ -w "$slpi/recovery" ] && echo enabled > "$slpi/recovery" 2>/dev/null || true
 [ -w "$slpi/coredump" ] && echo disabled > "$slpi/coredump" 2>/dev/null || true
 state=$($B cat "$slpi/state" 2>/dev/null || echo unknown)
 rec=$($B cat "$slpi/recovery" 2>/dev/null || echo unknown)
 if [ "$state" != running ] && [ "$rec" = enabled ] && [ -w /sys/kernel/boot_slpi/boot ]; then
  echo 1 > /sys/kernel/boot_slpi/boot 2>/dev/null || true
  i=0
  while [ "$i" -lt 25 ]; do
   state=$($B cat "$slpi/state" 2>/dev/null || echo unknown)
   [ "$state" = running ] && break
   i=$((i + 1))
   "$B" sleep 0.5
  done
 fi
 echo "MARBLE_SLPI_STATE=$state recovery=$($B cat "$slpi/recovery" 2>/dev/null || echo '?') coredump=$($B cat "$slpi/coredump" 2>/dev/null || echo '?')"
else
 echo "MARBLE_SLPI_RPROC=missing"
fi

# 4. Stage FastRPC nodes & seed persist safely (ro,noload -> umount).
"$B" sh "$D/stage-fastrpc-nodes.sh"
"$B" sh "$D/prepare-private-persist.sh"

# 5. Start isolated single-instance sscrpcd daemon (if not already alive).
if [ -z "$($B pgrep -f '/vendor/bin/sscrpcd' 2>/dev/null || true)" ]; then
 "$B" setsid "$B" unshare -m "$B" sh "$0" private </dev/null >/dev/null 2>&1 &
 i=0
 while [ "$i" -lt 20 ]; do
  [ -n "$($B pgrep -f '/vendor/bin/sscrpcd' 2>/dev/null || true)" ] && break
  i=$((i + 1))
  "$B" sleep 0.25
 done
fi
echo "MARBLE_SSCRPCD_PID=$($B pgrep -f '/vendor/bin/sscrpcd' 2>/dev/null | $B tr '\n' ' ' || echo none)"

# 6. Trigger OHOS sensor_host (V3_1) -> WaitForSensors -> sensors (SA 3601).
"$B" chroot "$R" /system/bin/param set marble.debug.start.sensors true >/dev/null 2>&1 || true
echo "MARBLE_SENSORS_PARAM=requested"
