#!/bin/sh
# SPDX-License-Identifier: MIT
# Explicit manual/COM9 Batch F1 bring-up: RO EFS RAM-shadow RMTFS + memshare + MSS remoteproc.
# NOT invoked automatically at boot. Never mounts modem_a rw and never writes flash partitions.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
MNT=/compat/original-firmware
IMG="$MNT/image"
KDIR=/hmos/vendor_dlkm/lib/modules
LOG="$R/data/local/tmp/marble-rmtfs.log"

say() { echo "$*"; }

# 1. Verify RO modem_a mount (in root + PID 1 /hmos-root chroot) and required firmware files.
if ! $B grep -q " $MNT " /proc/mounts 2>/dev/null; then
  $B mkdir -p "$MNT"
  $B mount -t vfat -o ro /dev/block/by-name/modem_a "$MNT" || { say "F1_ERR_MODEM_MOUNT"; exit 11; }
fi
$B mkdir -p "$R/compat/original-firmware"
if ! $B grep -q " $R/compat/original-firmware " /proc/mounts 2>/dev/null; then
  $B mount --bind "$MNT" "$R/compat/original-firmware" || { say "F1_ERR_MODEM_BIND"; exit 11; }
  $B mount -o remount,ro,bind "$R/compat/original-firmware" 2>/dev/null || true
fi
if [ ! -f "$IMG/modem.mdt" ] || [ ! -f "$IMG/ipa_fws.mdt" ] || [ ! -f "$IMG/modemr.jsn" ]; then
  say "F1_ERR_MISSING_MODEM_FILES"
  exit 12
fi
modem_cnt=$($B ls -1 "$IMG"/modem.mdt "$IMG"/modem.b* 2>/dev/null | $B wc -l)
say "F1_MODEM_FW_COUNT=$modem_cnt"
if [ "$modem_cnt" -lt 30 ]; then
  say "F1_ERR_LOW_MODEM_COUNT=$modem_cnt"
  exit 13
fi

# 2. Symlink modem.* and ipa_fws.* (zero 214 MiB copy overhead) + ensure firmware_class path.
$B mkdir -p /compat/firmware "$R/compat/firmware" "$R/data/local/tmp"
linked=0
for f in "$IMG"/modem.mdt "$IMG"/modem.b* "$IMG"/ipa_fws.mdt "$IMG"/ipa_fws.b* "$IMG"/modemr.jsn; do
  [ -f "$f" ] || continue
  base=${f##*/}
  $B ln -sf "$f" "/compat/firmware/$base"
  $B ln -sf "$f" "$R/compat/firmware/$base"
  linked=$((linked + 1))
done
if $B test -w /sys/module/firmware_class/parameters/path; then
  $B printf '/compat/firmware' > /sys/module/firmware_class/parameters/path 2>/dev/null || true
fi
say "F1_MODEM_FW_SYMLINKED=$linked"

# 3. Load sharedmem / memshare / qsee_ipc_irq_bridge / glink_pkt (no ipam/rmnet in F1).
for ko in msm_sharedmem.ko heap_mem_ext_v01.ko msm_memshare.ko qsee_ipc_irq_bridge.ko glink_pkt.ko; do
  mod=$(printf '%s' "${ko%.ko}" | $B tr - _)
  if $B grep -q "^$mod " /proc/modules 2>/dev/null; then
    say "F1_KO $ko=present"
    continue
  fi
  if [ -f "$KDIR/$ko" ] && $B insmod "$KDIR/$ko" 2>/dev/null; then
    say "F1_KO $ko=loaded"
  else
    say "F1_KO $ko=failed"
    exit 14
  fi
done

# 4. Ensure /dev/uioN node exists for qcom,sharedmem-uio ('rmtfs').
uio_idx=""
for d in /sys/class/uio/uio*; do
  [ -d "$d" ] || continue
  nm=$($B cat "$d/name" 2>/dev/null || true)
  if [ "$nm" = "rmtfs" ]; then
    uio_idx=${d##*/uio}
    break
  fi
done
if [ -z "$uio_idx" ]; then
  say "F1_ERR_NO_UIO_RMTFS"
  exit 15
fi
dev_mm=$($B cat "/sys/class/uio/uio$uio_idx/dev" 2>/dev/null || true)
maj=${dev_mm%:*}
min=${dev_mm#*:}
if [ -z "$maj" ] || [ -z "$min" ]; then
  say "F1_ERR_UIO_DEV=$dev_mm"
  exit 16
fi
if [ ! -c "/dev/uio$uio_idx" ]; then
  $B mknod "/dev/uio$uio_idx" c "$maj" "$min" || { say "F1_ERR_MKNOD_UIO"; exit 17; }
fi
$B chmod 0600 "/dev/uio$uio_idx"
say "F1_UIO_NODE=/dev/uio$uio_idx dev=$maj:$min"

# 5. Start clean-room RAM-backed hmos_rmtfs (RO EFS -> RAM COW only) and wait for RMTFS_READY.
if $B ps -ef | $B grep -q '[/]compat/bin/hmos_rmtfs' 2>/dev/null; then
  say "F1_RMTFS=already_running"
else
  : > "$LOG"
  $B setsid /compat/bin/hmos_rmtfs > "$LOG" 2>&1 </dev/null &
  ready=0
  i=0
  while [ "$i" -lt 25 ]; do
    if $B grep -q 'RMTFS_READY' "$LOG" 2>/dev/null; then
      ready=1
      break
    fi
    i=$((i + 1))
    $B sleep 0.2
  done
  if [ "$ready" != "1" ]; then
    say "F1_ERR_RMTFS_NOT_READY"
    $B cat "$LOG" 2>/dev/null || true
    exit 18
  fi
  say "F1_RMTFS=ready"
fi

# 6. Resolve 4080000.remoteproc-mss by name; enforce recovery=enabled BEFORE echo start.
idx=""
for d in /sys/class/remoteproc/remoteproc*; do
  [ -d "$d" ] || continue
  if [ "$($B cat "$d/name" 2>/dev/null || true)" = "4080000.remoteproc-mss" ]; then
    idx=${d##*/remoteproc}
    break
  fi
done
if [ -z "$idx" ]; then
  say "F1_ERR_NO_MSS_RPROC"
  exit 19
fi

rproc="/sys/class/remoteproc/remoteproc$idx"
$B printf 'enabled' > "$rproc/recovery" 2>/dev/null || echo enabled > "$rproc/recovery"
rec=$($B cat "$rproc/recovery" 2>/dev/null || true)
say "F1_MSS_RECOVERY=$rec idx=$idx"
if [ "$rec" != "enabled" ]; then
  say "F1_ERR_RECOVERY_NOT_ENABLED=$rec"
  exit 20
fi

st=$($B cat "$rproc/state" 2>/dev/null || true)
say "F1_MSS_STATE_INITIAL=$st"
if [ "$st" != "running" ]; then
  echo start > "$rproc/state" || { say "F1_ERR_START_WRITE"; exit 21; }
fi

i=0
st=""
while [ "$i" -lt 100 ]; do
  st=$($B cat "$rproc/state" 2>/dev/null || true)
  [ "$st" = "running" ] && break
  i=$((i + 1))
  $B sleep 0.2
done
say "F1_MSS_STATE=$st"
if [ "$st" != "running" ]; then
  $B dmesg | $B grep -iE '4080000|mpss|mss|q6v5|sharedmem|memshare|glink' | $B tail -n 30 || true
  $B tail -n 30 "$LOG" 2>/dev/null || true
  exit 22
fi

# 7. Wait 8s for QRTR/EFS handshake and all QMI service registrations, then run read-only probe.
$B sleep 8
$B timeout 15 /compat/bin/marble_modem_probe --all || say "F1_MODEM_PROBE_RC=$?"
say "=== RMTFS_LOG_TAIL ==="
$B tail -n 30 "$LOG" 2>/dev/null || true
say "=== DMESG_MODEM_TAIL ==="
$B dmesg | $B grep -iE '4080000|mpss|mss|q6v5|sharedmem|memshare|glink' | $B tail -n 30 || true
exit 0
