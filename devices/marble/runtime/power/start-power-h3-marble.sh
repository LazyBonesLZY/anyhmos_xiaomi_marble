#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch H3: Bring up OHOS Power HDI v1.3 host-12 (power_host) backed by
# SM7475 /sys/power/wake_lock, /sys/power/wake_unlock and /sys/power/state.
# Strictly zero audio/modem/persist side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,power,*|*,power_hdi,*)
    say "POWER_H3_SKIP=requested"
    exit 0
    ;;
esac

if [ ! -f /sys/power/state ]; then
  say "POWER_H3_ERR=no_sys_power_state"
  exit 0
fi
if [ ! -f "$R/compat/lib64/libmarble_power_interface_service_1.3.z.so" ]; then
  say "POWER_H3_ERR=missing_power_hdi_lib"
  exit 0
fi
say "POWER_H3_SYSFS_READY state=$($B cat /sys/power/state 2>/dev/null || echo NA)"

if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.power.ready true >/dev/null 2>&1 || say "POWER_H3_PARAM_RC=$?"
else
  say "POWER_H3_ERR=missing_param_bin"
  exit 0
fi

host_pid=""
i=0
while [ "$i" -lt 40 ]; do
  host_pid=$($B pgrep -f 'power_host.*-i 12|hdf_devhost.*power_host' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$host_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "POWER_H3_PROCS host_pid=${host_pid:-none} wait_steps=$i"
say "POWER_H3_DONE=1"
exit 0
