#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch H1: Expose SM7475 NXP SN110T/SN220 /dev/nq-nci inside HMOS chroot
# (owned by nfc_host:nfc 0660) and bring up OHOS NFC HDI host-10 (nfc_host).
# Strictly zero audio/modem/persist side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,nfc,*|*,nfc_hdi,*)
    say "NFC_H1_SKIP=requested"
    exit 0
    ;;
esac

# 1. Discover dynamic char major:minor for kernel nq-nci driver and create $R/dev/nq-nci.
dev_mm=$($B cat /sys/class/qti-nfc/nq-nci/dev 2>/dev/null | $B tr -d ' \t\r\n' || true)
if [ -z "$dev_mm" ]; then
  for uf in /sys/dev/char/*/uevent; do
    [ -f "$uf" ] || continue
    if $B grep -qx 'DEVNAME=nq-nci' "$uf" 2>/dev/null; then
      d=$($B dirname "$uf")
      dev_mm=$($B basename "$d")
      break
    fi
  done
fi
if [ -z "$dev_mm" ]; then
  say "NFC_H1_ERR=nq_nci_sysfs_missing"
  exit 0
fi

maj=${dev_mm%:*}
min=${dev_mm#*:}
$B rm -f "$R/dev/nq-nci"
if ! $B mknod -m 0660 "$R/dev/nq-nci" c "$maj" "$min" 2>/dev/null; then
  say "NFC_H1_ERR=mknod_failed mm=$dev_mm"
  exit 0
fi
$B chown 3042:1027 "$R/dev/nq-nci" 2>/dev/null || true
$B chmod 0660 "$R/dev/nq-nci" 2>/dev/null || true
say "NFC_H1_DEV_READY path=$R/dev/nq-nci mm=$dev_mm"

# 2. Verify staged NFC HDI implementation is present.
if [ ! -f "$R/compat/lib64/libmarble_nfc_interface_service_1.1.z.so" ]; then
  say "NFC_H1_ERR=missing_nfc_hdi_lib"
  exit 0
fi

# 3. Trigger OHOS init param:marble-nfc-ready (starts nfc_host host-10).
if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.nfc.ready true >/dev/null 2>&1 || say "NFC_H1_PARAM_RC=$?"
else
  say "NFC_H1_ERR=missing_param_bin"
  exit 0
fi

# 4. Wait up to 8s for nfc_host (host-10) process.
host_pid=""
i=0
while [ "$i" -lt 40 ]; do
  host_pid=$($B pgrep -f 'nfc_host.*-i 10|hdf_devhost.*nfc_host' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$host_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "NFC_H1_PROCS host_pid=${host_pid:-none} wait_steps=$i"
say "NFC_H1_DONE=1"
exit 0
