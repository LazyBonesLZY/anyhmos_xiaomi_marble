#!/busybox sh
# SPDX-License-Identifier: MIT
# Explicit manual/COM9 Batch F5 bring-up:
#   --tftp       : transient RO->RAM copy of persist/rfs -> umount -> hmos_tqftpserv
#   --modem      : --tftp BEFORE MPSS start + F1 modem start + TQFTP/PDC/NAS/DSB/UIM/LOC
#   --telephony  : verify MPSS running + start-modem-f3-marble.sh --telephony
#   --gnss       : start-modem-f3-marble.sh --gnss
#   --all        : --modem + --telephony + --gnss
#   --status     : F3/F4/F5 status & sim_slot_cfg / vsimModemCount parameters
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
TQFTP_LOG="$R/data/local/tmp/marble-tqftpserv.log"

say() { echo "$*"; }

param_get() {
  $B chroot "$R" /system/bin/param get "$1" 2>/dev/null | $B tr -d ' \t\r\n'
}

do_tftp() {
  [ -x /compat/bin/start-modem-f3-marble.sh ] || { say "F5_ERR_MISSING_F3_SCRIPT"; return 1; }
  $B sh /compat/bin/start-modem-f3-marble.sh --tftp
}

do_modem() {
  do_tftp || return 1
  [ -x /compat/bin/start-modem-f1-marble.sh ] || { say "F5_ERR_MISSING_F1_SCRIPT"; return 1; }
  $B sh /compat/bin/start-modem-f1-marble.sh || { rc=$?; say "F5_F1_RC=$rc"; return "$rc"; }
  $B sleep 2

  lines=0
  ro_ok=0
  ro_rej=0
  unlink_ok=0
  tmo_cnt=0
  if [ -f "$TQFTP_LOG" ]; then
    lines=$($B wc -l < "$TQFTP_LOG" 2>/dev/null | $B tr -d ' \t\r\n' || echo 0)
    ro_ok=$($B grep -cE 'TQFTP_DONE op=RRQ path=/readonly/.*rc=0' "$TQFTP_LOG" 2>/dev/null || true)
    ro_rej=$($B grep -cE 'TQFTP_REJECT op=RRQ path=/readonly/' "$TQFTP_LOG" 2>/dev/null || true)
    unlink_ok=$($B grep -cE 'TQFTP_DONE op=WRQ .*action=unlink' "$TQFTP_LOG" 2>/dev/null || true)
    tmo_cnt=$($B grep -cE 'TQFTP_DONE .*rc=-110' "$TQFTP_LOG" 2>/dev/null || true)
  fi
  say "TQFTP_LOG_LINES=${lines:-0} TQFTP_RO_MCFG_OK=${ro_ok:-0} TQFTP_RO_MCFG_REJECT=${ro_rej:-0} TQFTP_UNLINK_OK=${unlink_ok:-0} TQFTP_TIMEOUT_CNT=${tmo_cnt:-0}"
  if [ -f "$TQFTP_LOG" ]; then
    say "=== TQFTP_LOG_HEAD ==="
    $B head -n 60 "$TQFTP_LOG" 2>/dev/null || true
    say "=== TQFTP_LOG_TAIL ==="
    $B tail -n 30 "$TQFTP_LOG" 2>/dev/null || true
  fi

  $B timeout 20 /compat/bin/marble_modem_probe --dms --dms-raw --set-mode online || say "F5_SET_ONLINE_RC=$?"
  if $B timeout 15 /compat/bin/marble_modem_probe --dms 2>/dev/null | $B grep -q 'op_mode=3(offline)'; then
    say "F5_DMS_STILL_OFFLINE=1"
    $B timeout 15 /compat/bin/marble_modem_probe --set-mode lpm || say "F5_SET_LPM_RC=$?"
    $B sleep 1
    $B timeout 15 /compat/bin/marble_modem_probe --set-mode online || say "F5_RETRY_ONLINE_RC=$?"
  fi

  $B timeout 15 /compat/bin/marble_modem_probe --nas-set-dsb both || say "F5_NAS_DSB_RC=$?"
  $B sleep 3
  $B timeout 20 /compat/bin/marble_modem_probe --dms --uim --sim-ef --nas-ext --loc-probe || say "F5_POST_PROBE_RC=$?"
  return 0
}

do_telephony() {
  [ -x /compat/bin/start-modem-f3-marble.sh ] || { say "F5_ERR_MISSING_F3_SCRIPT"; return 1; }
  $B sh /compat/bin/start-modem-f3-marble.sh --telephony
}

do_gnss() {
  [ -x /compat/bin/start-modem-f3-marble.sh ] || { say "F5_ERR_MISSING_F3_SCRIPT"; return 1; }
  $B sh /compat/bin/start-modem-f3-marble.sh --gnss
}

do_status() {
  [ -x /compat/bin/start-modem-f3-marble.sh ] && $B sh /compat/bin/start-modem-f3-marble.sh --status || true
  say "F4_PARAMS vsim=$(param_get const.telephony.vsimModemCount)"
  say "F5_PARAMS slot_cfg=$(param_get persist.radio.sim_slot_cfg) vsim=$(param_get const.telephony.vsimModemCount)"
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
