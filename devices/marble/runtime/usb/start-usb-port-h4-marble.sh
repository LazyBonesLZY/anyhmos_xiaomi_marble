#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch H4: Bring up OHOS USB Port HDI v2.0 host-13 (usb_host) backed by
# SM7475 /sys/class/typec/port0 (read-only telemetry; never mutates UDC/COM9).
# Strictly zero audio/modem/persist side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,usb,*|*,usb_port,*|*,usb_port_hdi,*)
    say "USB_PORT_H4_SKIP=requested"
    exit 0
    ;;
esac

if [ ! -f "$R/compat/lib64/libmarble_usb_port_interface_service_2.0.z.so" ]; then
  say "USB_PORT_H4_ERR=missing_usb_port_hdi_lib"
  exit 0
fi
if [ -r /sys/class/typec/port0/power_role ]; then
  say "USB_PORT_H4_SYSFS_READY power=$($B cat /sys/class/typec/port0/power_role 2>/dev/null || echo NA) data=$($B cat /sys/class/typec/port0/data_role 2>/dev/null || echo NA)"
else
  say "USB_PORT_H4_SYSFS_FALLBACK online=$($B cat /sys/class/power_supply/usb/online 2>/dev/null || echo NA)"
fi

if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.usb_port.ready true >/dev/null 2>&1 || say "USB_PORT_H4_PARAM_RC=$?"
else
  say "USB_PORT_H4_ERR=missing_param_bin"
  exit 0
fi

host_pid=""
i=0
while [ "$i" -lt 40 ]; do
  host_pid=$($B pgrep -f 'usb_host.*-i 13|hdf_devhost.*usb_host' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$host_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "USB_PORT_H4_PROCS host_pid=${host_pid:-none} wait_steps=$i"
say "USB_PORT_H4_DONE=1"
exit 0
