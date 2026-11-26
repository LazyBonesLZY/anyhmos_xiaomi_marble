#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch H7: Bring up stock HarmonyOS NFC System Ability SA 1140 (nfc_service)
# on Xiaomi marble (SM7475) over live nfc_host (host-10, NXP SN110T /dev/nq-nci).
# Strictly zero audio/modem/persist/vibrator side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,nfc,*|*,nfc_service,*|*,nfc_fwk,*)
    say "NFC_H7_SKIP=requested"
    exit 0
    ;;
esac

# 1. Ensure /data/nfc and /chip_prod/etc SN110 configs are present.
$B mkdir -p "$R/data/nfc" "$R/data/log/nfc" "$R/chip_prod/etc" 2>/dev/null || true
$B chown 1027:1027 "$R/data/nfc" 2>/dev/null || true
$B chmod 0770 "$R/data/nfc" 2>/dev/null || true
$B chown 3042:3042 "$R/data/log/nfc" 2>/dev/null || true
$B chmod 0770 "$R/data/log/nfc" 2>/dev/null || true

for cfg in libnfc-nci-sn110.conf libnfc-nxp-sn110.conf; do
  if [ -f "/graphics/$cfg" ] && [ ! -f "$R/chip_prod/etc/$cfg" ]; then
    $B cp "/graphics/$cfg" "$R/chip_prod/etc/$cfg" 2>/dev/null || true
    $B chmod 0644 "$R/chip_prod/etc/$cfg" 2>/dev/null || true
  fi
done

# 2. Verify clean-room libnci_native_vendor.z.so bind mount & nfc_host (host-10).
vendor_bind=0
if $B grep -q " $R/system/lib64/libnci_native_vendor.z.so " /proc/mounts 2>/dev/null; then
  vendor_bind=1
fi
host_pid=$($B pgrep -f 'nfc_host.*-i 10|hdf_devhost.*nfc_host' 2>/dev/null | $B head -n 1 || true)
say "NFC_H7_PRECHECK vendor_bind=$vendor_bind host_pid=${host_pid:-none}"

# 3. Trigger on-demand activation of SA 1140 (nfc_service) via official NfcController::InitNfcRemoteSA().
if [ -x "$R/compat/bin/native_nfc_fwk_probe" ]; then
  $B chroot "$R" /compat/bin/native_nfc_fwk_probe --load-sa >/dev/null 2>&1 || say "NFC_H7_LOAD_RC=$?"
else
  say "NFC_H7_ERR=missing_fwk_probe"
  exit 0
fi

# 4. Wait up to 8s for nfc_service process.
svc_pid=""
i=0
while [ "$i" -lt 40 ]; do
  svc_pid=$($B pgrep -f 'nfc_service' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$svc_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "NFC_H7_PROCS svc_pid=${svc_pid:-none} wait_steps=$i"
say "NFC_H7_DONE=1"
exit 0
