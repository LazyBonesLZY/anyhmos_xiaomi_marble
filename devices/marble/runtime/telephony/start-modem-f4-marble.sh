#!/busybox sh
# SPDX-License-Identifier: MIT
# Explicit manual/COM9 Batch F4 bring-up:
#   --tftp       : transient RO->RAM copy of persist/rfs -> umount -> fixed hmos_tqftpserv
#   --modem      : --tftp BEFORE MPSS start + F1 modem start + TQFTP/RF/PDC/UIM/NAS/LOC diagnostics
#   --telephony  : verify MPSS running + start-modem-f3-marble.sh --telephony
#   --gnss       : start-modem-f3-marble.sh --gnss
#   --all        : --modem + --telephony + --gnss
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
TQFTP_LOG="$R/data/local/tmp/marble-tqftpserv.log"

say() { echo "$*"; }

param_get() {
  $B chroot "$R" /system/bin/param get "$1" 2>/dev/null | $B tr -d ' \t\r\n'
}

do_tftp() {
  [ -x /compat/bin/start-modem-f3-marble.sh ] || { say "F4_ERR_MISSING_F3_SCRIPT"; return 1; }
  $B sh /compat/bin/start-modem-f3-marble.sh --tftp
}

do_modem() {
  do_tftp || return 1
  [ -x /compat/bin/start-modem-f1-marble.sh ] || { say "F4_ERR_MISSING_F1_SCRIPT"; return 1; }
  $B sh /compat/bin/start-modem-f1-marble.sh || { rc=$?; say "F4_F1_RC=$rc"; return "$rc"; }
  $B sleep 2

  lines=0
  ro_ok=0
  ro_rej=0
  if [ -f "$TQFTP_LOG" ]; then
    lines=$($B wc -l < "$TQFTP_LOG" 2>/dev/null | $B tr -d ' \t\r\n' || echo 0)
    ro_ok=$($B grep -cE 'TQFTP_DONE op=RRQ path=/readonly/.*rc=0' "$TQFTP_LOG" 2>/dev/null || true)
    ro_rej=$($B grep -cE 'TQFTP_REJECT op=RRQ path=/readonly/' "$TQFTP_LOG" 2>/dev/null || true)
  fi
  say "TQFTP_LOG_LINES=${lines:-0} TQFTP_RO_MCFG_OK=${ro_ok:-0} TQFTP_RO_MCFG_REJECT=${ro_rej:-0}"
  if [ -f "$TQFTP_LOG" ]; then
    say "=== TQFTP_LOG_HEAD ==="
    $B head -n 80 "$TQFTP_LOG" 2>/dev/null || true
  fi

  $B timeout 15 /compat/bin/marble_modem_probe --services-all || say "F4_SVC_ALL_RC=$?"
  $B timeout 15 /compat/bin/marble_modem_probe --dms || say "F4_DMS_RC=$?"
  $B timeout 15 /compat/bin/marble_modem_probe --dms-raw || say "F4_DMS_RAW_RC=$?"
  $B timeout 15 /compat/bin/marble_modem_probe --set-mode online || say "F4_SET_ONLINE_RC=$?"

  if $B timeout 15 /compat/bin/marble_modem_probe --dms 2>/dev/null | $B grep -q 'op_mode=3(offline)'; then
    say "F4_DMS_STILL_OFFLINE=1"
    $B timeout 15 /compat/bin/marble_modem_probe --set-mode lpm || say "F4_SET_LPM_RC=$?"
    $B sleep 1
    $B timeout 15 /compat/bin/marble_modem_probe --set-mode online || say "F4_RETRY_ONLINE_RC=$?"
  fi

  $B timeout 15 /compat/bin/marble_modem_probe --uim || say "F4_UIM_RC=$?"
  $B timeout 15 /compat/bin/marble_modem_probe --sim-ef || say "F4_SIM_EF_RC=$?"
  $B timeout 15 /compat/bin/marble_modem_probe --nas-ext || say "F4_NAS_EXT_RC=$?"
  $B timeout 15 /compat/bin/marble_modem_probe --loc-probe || say "F4_LOC_PROBE_RC=$?"
  return 0
}

do_telephony() {
  [ -x /compat/bin/start-modem-f3-marble.sh ] || { say "F4_ERR_MISSING_F3_SCRIPT"; return 1; }
  $B sh /compat/bin/start-modem-f3-marble.sh --telephony
}

do_gnss() {
  [ -x /compat/bin/start-modem-f3-marble.sh ] || { say "F4_ERR_MISSING_F3_SCRIPT"; return 1; }
  $B sh /compat/bin/start-modem-f3-marble.sh --gnss
}

do_status() {
  [ -x /compat/bin/start-modem-f3-marble.sh ] && $B sh /compat/bin/start-modem-f3-marble.sh --status || true
  say "F4_PARAMS vsim=$(param_get const.telephony.vsimModemCount)"
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
