#!/busybox sh
# SPDX-License-Identifier: MIT
# Explicit manual/COM9 Batch F3 bring-up:
#   --tftp       : transient RO->RAM copy of persist/rfs -> umount -> hmos_tqftpserv
#   --modem      : --tftp BEFORE MPSS start + start-modem-f1-marble.sh + dms/loc probes
#   --telephony  : verify MPSS running + start-modem-f2-marble.sh + SA 66250 check
#   --gnss       : start gnss_hdi_marble (condition trigger) + Binder --check
#   --all        : --modem + --telephony + --gnss
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
RAM_RFS=/run/marble-rfs-ram
PERSIST_BLK=/dev/block/by-name/persist
PERSIST_MNT=/tmp/marble-persist-ro
TQFTP_LOG="$R/data/local/tmp/marble-tqftpserv.log"

say() { echo "$*"; }

param_get() {
  $B chroot "$R" /system/bin/param get "$1" 2>/dev/null | $B tr -d ' \t\r\n'
}

param_set() {
  $B chroot "$R" /system/bin/param set "$1" "$2" >/dev/null 2>&1 || return 1
}

sa_ready() {
  $B chroot "$R" /compat/bin/native_sa_check "$1" 2>/dev/null | $B grep -q "NATIVE_SA_READY id=$1"
}

check_persist_unmounted() {
  if $B grep -qE "(/tmp/marble-persist-ro|/dev/block/by-name/persist|/dev/block/sdf7| /persist )" /proc/mounts 2>/dev/null; then
    say "F3_ERR_PERSIST_STILL_MOUNTED"
    return 1
  fi
  say "F3_PERSIST_UNMOUNTED=1"
  return 0
}

do_tftp() {
  $B mkdir -p "$R/data/local/tmp"
  if $B ps -ef 2>/dev/null | $B grep -q '[/]compat/bin/hmos_tqftpserv'; then
    say "F3_TQFTP=already_running"
    check_persist_unmounted || return 1
    return 0
  fi

  if ! $B grep -q " /compat/original-firmware " /proc/mounts 2>/dev/null && [ -e /dev/block/by-name/modem_a ]; then
    $B mkdir -p /compat/original-firmware
    $B mount -t vfat -o ro /dev/block/by-name/modem_a /compat/original-firmware 2>/dev/null || true
  fi

  $B mkdir -p "$RAM_RFS"
  if ! $B grep -q " $RAM_RFS " /proc/mounts 2>/dev/null; then
    $B mount -t tmpfs -o mode=0755,size=32M tmpfs "$RAM_RFS" || { say "F3_ERR_RAM_TMPFS"; return 1; }
  fi

  if [ ! -f "$RAM_RFS/.seeded" ]; then
    if [ -e "$PERSIST_BLK" ]; then
      if $B grep -q " $PERSIST_MNT " /proc/mounts 2>/dev/null; then
        $B umount "$PERSIST_MNT" 2>/dev/null || true
      fi
      $B mkdir -p "$PERSIST_MNT"
      if $B mount -t ext4 -o ro,noload "$PERSIST_BLK" "$PERSIST_MNT" 2>/dev/null; then
        [ -d "$PERSIST_MNT/rfs" ] && $B cp -a "$PERSIST_MNT/rfs" "$RAM_RFS/" 2>/dev/null || true
        [ -d "$PERSIST_MNT/hlos_rfs" ] && $B cp -a "$PERSIST_MNT/hlos_rfs" "$RAM_RFS/" 2>/dev/null || true
        $B sync
        $B umount "$PERSIST_MNT" || { say "F3_ERR_PERSIST_UMOUNT"; return 1; }
      else
        say "F3_WARN_PERSIST_RO_MOUNT_SKIP"
      fi
      $B rmdir "$PERSIST_MNT" 2>/dev/null || true
    fi
    $B mkdir -p "$RAM_RFS/rfs/msm/mpss" "$RAM_RFS/rfs/shared" "$RAM_RFS/hlos_rfs/shared"
    [ -f "$RAM_RFS/rfs/msm/mpss/server_check.txt" ] || printf 'hello' > "$RAM_RFS/rfs/msm/mpss/server_check.txt"
    $B chmod -R u+rwX "$RAM_RFS" 2>/dev/null || true
    : > "$RAM_RFS/.seeded"
  fi
  check_persist_unmounted || return 1

  ro_args=""
  for cand in /compat/original-firmware/image/modem_pr/mcfg \
              "$R/compat/original-firmware/image/modem_pr/mcfg" \
              /compat/original-firmware/image \
              "$R/compat/original-firmware/image"; do
    if [ -d "$cand" ]; then
      ro_args="$ro_args --ro-dir $cand"
    fi
  done

  : > "$TQFTP_LOG"
  # shellcheck disable=SC2086
  $B setsid /compat/bin/hmos_tqftpserv --root "$RAM_RFS" $ro_args > "$TQFTP_LOG" 2>&1 </dev/null &
  ready=0
  i=0
  while [ "$i" -lt 25 ]; do
    if $B grep -q 'TQFTPSERV_READY' "$TQFTP_LOG" 2>/dev/null; then
      ready=1
      break
    fi
    i=$((i + 1))
    $B sleep 0.2
  done
  if [ "$ready" != "1" ]; then
    say "F3_ERR_TQFTP_NOT_READY"
    $B cat "$TQFTP_LOG" 2>/dev/null || true
    return 1
  fi
  pid=$($B pidof hmos_tqftpserv 2>/dev/null || echo none)
  say "F3_TQFTP=ready pid=$pid"
  return 0
}

do_modem() {
  do_tftp || return 1
  if [ ! -x /compat/bin/start-modem-f1-marble.sh ]; then
    say "F3_ERR_MISSING_F1_SCRIPT"
    return 1
  fi
  $B sh /compat/bin/start-modem-f1-marble.sh || { rc=$?; say "F3_F1_RC=$rc"; return "$rc"; }
  $B timeout 15 /compat/bin/marble_modem_probe --set-mode online || say "F3_SET_MODE_ONLINE_RC=$?"
  $B timeout 15 /compat/bin/marble_modem_probe --dms || true
  $B timeout 15 /compat/bin/marble_modem_probe --dms-raw || say "F3_DMS_RAW_RC=$?"
  $B timeout 15 /compat/bin/marble_modem_probe --loc-probe || say "F3_LOC_PROBE_RC=$?"
  return 0
}

do_telephony() {
  rproc_state=$($B cat /sys/class/remoteproc/remoteproc2/state 2>/dev/null || true)
  if [ "$rproc_state" != "running" ]; then
    say "F3_ERR_MPSS_NOT_RUNNING=$rproc_state"
    return 1
  fi
  [ -x /compat/bin/start-modem-f2-marble.sh ] || { say "F3_ERR_MISSING_F2_SCRIPT"; return 1; }
  $B sh /compat/bin/start-modem-f2-marble.sh --telephony || { rc=$?; say "F3_F2_RC=$rc"; return "$rc"; }
  esim_sa=0
  sa_ready 66250 && esim_sa=1
  say "F3_TELEPHONY=done sa4010=$(sa_ready 4010 && echo 1 || echo 0) sa66250=$esim_sa"
  return 0
}

do_gnss() {
  if [ ! -x "$R/compat/bin/native_gnss_hdi_publisher" ]; then
    say "F3_ERR_MISSING_GNSS_PUBLISHER"
    return 1
  fi
  param_set marble.debug.start.gnss true || { say "F3_ERR_PARAM_SET_GNSS"; return 1; }
  i=0
  pid=""
  while [ "$i" -lt 25 ]; do
    pid=$($B pidof native_gnss_hdi_publisher 2>/dev/null || true)
    [ -n "$pid" ] && break
    i=$((i + 1))
    $B sleep 0.2
  done
  $B sleep 1
  say "F3_GNSS_PID=${pid:-none}"
  [ -f "$R/data/local/tmp/gnss-hdi/status.log" ] && $B cat "$R/data/local/tmp/gnss-hdi/status.log" || true
  $B chroot "$R" /compat/bin/native_gnss_hdi_publisher --check
  crc=$?
  say "F3_GNSS_CHECK_RC=$crc"
  return "$crc"
}

do_status() {
  say "F3_STATUS tqftp=$($B pidof hmos_tqftpserv 2>/dev/null || echo none) rmtfs=$($B pidof hmos_rmtfs 2>/dev/null || echo none) gnss=$($B pidof native_gnss_hdi_publisher 2>/dev/null || echo none)"
  say "F3_PARAMS vmodem=$(param_get const.booster.virtual_modem_switch) mdev=$(param_get persist.distributedmodem.multidevice.enable) esim=$(param_get const.ril.esim_type) close_esim=$(param_get vendor.hw_mc.ril.esim.close_esim)"
  check_persist_unmounted || true
  return 0
}

case "${1:-}" in
  --tftp) do_tftp ;;
  --modem) do_modem ;;
  --telephony) do_telephony ;;
  --gnss) do_gnss ;;
  --all)
    do_modem || exit $?
    do_telephony || exit $?
    do_gnss || exit $?
    ;;
  --status) do_status ;;
  *)
    echo "usage: $0 [--tftp|--modem|--telephony|--gnss|--all|--status]" >&2
    exit 2
    ;;
esac
