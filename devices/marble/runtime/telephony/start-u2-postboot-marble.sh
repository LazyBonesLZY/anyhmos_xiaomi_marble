#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch U2/F6 asynchronous post-boot bring-up:
#   Step A: Venus VPU (/compat/bin/start-venus-marble.sh)
#   Step B: RO->RAM persist TFTP + F1 MSS remoteproc + F6 clean 8-KO IPA / multi-SIM NAS / Telephony (with LPM fallback) + GNSS HDI
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
skipped() {
  [ -n "$SKIP" ] || return 1
  case ",$SKIP," in
    *",$1,"*) return 0 ;;
  esac
  return 1
}

say "U2_POSTBOOT_BEGIN skip=${SKIP:-none}"

# Step A: Qualcomm Venus VPU firmware + V4L2 nodes.
if skipped venus; then
  say "U2_VENUS_SKIP=requested"
elif [ -x /compat/bin/start-venus-marble.sh ]; then
  $B sh /compat/bin/start-venus-marble.sh || say "U2_VENUS_RC=$?"
else
  say "U2_VENUS_SKIP=script_missing"
fi

# Step B: Real hardware GNSS + Cellular (RO->RAM tftp -> F1 MSS -> F6 IPA/NAS/Telephony or LPM fallback).
if skipped gnss || skipped modem; then
  say "U2_MODEM_GNSS_SKIP=requested"
  say "U2_POSTBOOT_DONE=1"
  exit 0
fi

if [ -x /compat/bin/start-modem-f3-marble.sh ]; then
  $B sh /compat/bin/start-modem-f3-marble.sh --tftp || say "U2_TFTP_RC=$?"
else
  say "U2_ERR_MISSING_F3_SCRIPT"
fi

if [ -x /compat/bin/start-modem-f1-marble.sh ]; then
  $B sh /compat/bin/start-modem-f1-marble.sh || say "U2_F1_RC=$?"
else
  say "U2_ERR_MISSING_F1_SCRIPT"
fi

# Ensure firmware_class keeps /compat/firmware after F1 (harmless if already set).
if [ -f /compat/firmware/vpu20_4v.mbn ] && $B test -w /sys/module/firmware_class/parameters/path; then
  $B printf '/compat/firmware' > /sys/module/firmware_class/parameters/path 2>/dev/null || true
fi

if ! skipped cellular && [ -x /compat/bin/start-modem-f7-marble.sh ]; then
  $B sh /compat/bin/start-modem-f7-marble.sh --all || say "U2_F7_RC=$?"
elif ! skipped cellular && [ -x /compat/bin/start-modem-f6-marble.sh ]; then
  $B sh /compat/bin/start-modem-f6-marble.sh --all || say "U2_F6_RC=$?"
else
  say "U2_F6_SKIP=skipped cellular (op_mode=1(low_power))"
  $B sleep 2
  if [ -x /compat/bin/marble_modem_probe ]; then
    i=0
    while [ "$i" -lt 8 ]; do
      if $B timeout 8 /compat/bin/marble_modem_probe --services 2>/dev/null | $B grep -q 'name=dms'; then
        break
      fi
      i=$((i + 1))
      $B sleep 1
    done

    dms_out=$($B timeout 15 /compat/bin/marble_modem_probe --dms 2>/dev/null || true)
    if ! printf '%s\n' "$dms_out" | $B grep -q 'op_mode=1(low_power)'; then
      $B timeout 20 /compat/bin/marble_modem_probe --set-mode lpm || say "U2_SET_LPM_RC=$?"
      $B sleep 1
      dms_out=$($B timeout 15 /compat/bin/marble_modem_probe --dms 2>/dev/null || true)
    fi
    op_mode=$(printf '%s\n' "$dms_out" | $B grep -o 'op_mode=[0-9]*([^)]*)' | $B head -n 1 || true)
    say "DMS_OP_MODE=${op_mode:-unknown}"
  fi
fi

say "U2_GNSS_MODE=hdf_location_host"
say "U2_POSTBOOT_DONE=1"
exit 0
