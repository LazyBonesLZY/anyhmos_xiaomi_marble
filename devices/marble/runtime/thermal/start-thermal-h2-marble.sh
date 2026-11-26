#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch H2: Bring up OHOS Thermal HDI v1.1 host-11 (thermal_host) backed by
# live SM7475 /sys/class/thermal/thermal_zone* telemetry.
# Strictly read-only hardware access; zero audio/modem/persist side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,thermal,*|*,thermal_hdi,*)
    say "THERMAL_H2_SKIP=requested"
    exit 0
    ;;
esac

# 1. Verify kernel thermal sysfs and staged Thermal HDI implementation.
tz_cnt=$($B ls -d /sys/class/thermal/thermal_zone* 2>/dev/null | $B wc -l | $B tr -d ' \t\r\n' || echo 0)
if [ "$tz_cnt" -lt 1 ]; then
  say "THERMAL_H2_ERR=no_thermal_zones"
  exit 0
fi
if [ ! -f "$R/compat/lib64/libmarble_thermal_interface_service_1.1.z.so" ]; then
  say "THERMAL_H2_ERR=missing_thermal_hdi_lib"
  exit 0
fi
say "THERMAL_H2_SYSFS_READY zones=$tz_cnt"

# 2. Trigger OHOS init param:marble-thermal-ready (starts thermal_host host-11).
if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.thermal.ready true >/dev/null 2>&1 || say "THERMAL_H2_PARAM_RC=$?"
else
  say "THERMAL_H2_ERR=missing_param_bin"
  exit 0
fi

# 3. Wait up to 8s for thermal_host (host-11) process.
host_pid=""
i=0
while [ "$i" -lt 40 ]; do
  host_pid=$($B pgrep -f 'thermal_host.*-i 11|hdf_devhost.*thermal_host' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$host_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "THERMAL_H2_PROCS host_pid=${host_pid:-none} wait_steps=$i"
say "THERMAL_H2_DONE=1"
exit 0
