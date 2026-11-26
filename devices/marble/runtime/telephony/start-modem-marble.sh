#!/bin/sh
# SPDX-License-Identifier: MIT
# marble telephony stage 1: check the Qualcomm MSS remote processor state and,
# when the diagnostic is staged, independently report QMI/QRTR advertisements.
#
# STATUS: advisory artifact. It is NOT wired into devices/marble/runtime/boot/
# start-phase4-marble.sh or stage-phase4-marble.sh. See
# work/marble-telephony-report.md section 4 for the exact insertion point
# (stage-phase4-marble.sh, after line 158 / before line 160).
#
# Safety contract of this script:
#   * modem_a is only ever mounted read-only; nothing writes to flash.
#   * it never touches the modem partition device beyond a `-o ro` mount, never writes super,
#     modem, vbmeta, persist or TEE partitions.
#   * every step is idempotent and individually skippable; a missing optional piece
#     degrades to a logged reason, not a silent success.
#
# Environment overrides (all optional, used by tests and by --dry-run):
#   MARBLE_BB                 busybox path                 (default /busybox)
#   MARBLE_ROOT               chroot target                (default /hmos-root)
#   MARBLE_MODEM_PART         modem partition device       (default /dev/block/by-name/modem_a)
#   MARBLE_MODEM_IMAGE        staged firmware source dir   (default $MNT/image)
#   MARBLE_MODEM_MNT          read-only mountpoint         (default /compat/original-firmware)
#   MARBLE_MODEM_RPROC_NAME   remoteproc sysfs name        (default 4080000.remoteproc-mss)
#   MARBLE_MODEM_BOOT         "0" to skip the state=start kick
#   MARBLE_MODEM_DRY_RUN      "1" print the plan only, change nothing
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
PART=${MARBLE_MODEM_PART:-/dev/block/by-name/modem_a}
MNT=${MARBLE_MODEM_MNT:-/compat/original-firmware}
IMG=${MARBLE_MODEM_IMAGE:-$MNT/image}
RPROC_NAME=${MARBLE_MODEM_RPROC_NAME:-4080000.remoteproc-mss}
DO_BOOT=${MARBLE_MODEM_BOOT:-1}
DRY=${MARBLE_MODEM_DRY_RUN:-0}

say() { echo "$*"; }
plan() { say "PLAN $*"; }

if [ "$DRY" = "1" ]; then
  say "MARBLE_MODEM_DRY_RUN=1"
  plan "mount -t vfat -o ro $PART $MNT"
  plan "stage $IMG/modem.* $IMG/ipa_fws.* $IMG/modemr.jsn -> /compat/firmware + $R/compat/firmware"
  plan "load modules: qcom-scm smem qcom_glink_smem qcom_glink glink_pkt qmi_helpers pdr_interface qcom_sysmon smp2p qcom_q6v5 qcom_q6v5_pas qrtr qrtr-smd qcom_pil_info qcom_ramdump mem_buf secure_buffer ipam ipanetm rmnet_core rmnet_ctl"
  plan "resolve /sys/class/remoteproc/* whose name == $RPROC_NAME"
  if [ "$DO_BOOT" = "1" ]; then
    plan "echo start > /sys/class/remoteproc/<idx>/state"
  fi
  plan "poll state for MARBLE_MODEM_RPROC_WAIT_SECONDS=${MARBLE_MODEM_RPROC_WAIT_SECONDS:-20}s"
  plan "print /sys/class/remoteproc/*/name|firmware|state and QRTR channel presence"
  say "MARBLE_MODEM_RESULT=DRY_RUN"
  exit 0
fi

mkdir_p() { $B mkdir -p "$1" 2>/dev/null || mkdir -p "$1"; }

# ---------------------------------------------------------------------------
# 1. Mount the stock modem firmware partition read-only (never rw, never flash).
# ---------------------------------------------------------------------------
say "MARBLE_MODEM_STAGE_BEGIN"
mkdir_p "$MNT"
mkdir_p /compat/firmware
mkdir_p "$R/compat/firmware"
mkdir_p "$R/data/local/tmp"

if $B grep -q " $MNT " /proc/mounts 2>/dev/null; then
  say "MARBLE_MODEM_MOUNT=present"
else
  if $B mount -t vfat -o ro "$PART" "$MNT" 2>/dev/null; then
    say "MARBLE_MODEM_MOUNT=ok"
  else
    say "MARBLE_MODEM_MOUNT=fail"
    say "MARBLE_MODEM_RESULT=FAIL_MOUNT"
    exit 31
  fi
fi

# ---------------------------------------------------------------------------
# 2. Stage the modem PD image + IPA firmware + the modem PDR description.
#    modem.b* numbering is NOT contiguous on this device (b14/b21/b30 absent),
#    so a glob is mandatory; iterating 00..31 would abort the load.
# ---------------------------------------------------------------------------
if [ ! -f "$IMG/modem.mdt" ]; then
  say "MARBLE_MODEM_FIRMWARE=missing_mdt path=$IMG/modem.mdt"
  say "MARBLE_MODEM_RESULT=FAIL_NO_MODEM_IMAGE"
  exit 32
fi

n=0
for f in "$IMG"/modem.mdt "$IMG"/modem.b* "$IMG"/ipa_fws.mdt "$IMG"/ipa_fws.b* "$IMG"/modemr.jsn; do
  [ -f "$f" ] || continue
  base=${f##*/}
  $B cp "$f" "/compat/firmware/$base" 2>/dev/null || cp "$f" "/compat/firmware/$base"
  $B cp "$f" "$R/compat/firmware/$base" 2>/dev/null || cp "$f" "$R/compat/firmware/$base"
  $B chmod 0644 "/compat/firmware/$base" "$R/compat/firmware/$base" 2>/dev/null || true
  n=$((n + 1))
done
say "MARBLE_MODEM_FIRMWARE_STAGED=$n"
if [ "$n" -lt 20 ]; then
  # 30 files expected (modem.mdt + 29 modem.b*), plus 6 ipa_fws + modemr.jsn.
  say "MARBLE_MODEM_FIRMWARE_WARN=low_file_count"
fi

# firmware_class for the root namespace and for PID1's /hmos-root namespace
if $B test -w /sys/module/firmware_class/parameters/path; then
  printf '/compat/firmware' > /sys/module/firmware_class/parameters/path 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 3. Load the transport / PAS modules. Already-loaded ones are skipped by
#    load_ko, so this block is safe to run on every boot re-entry.
# ---------------------------------------------------------------------------
load_ko() {
  ko="$1"
  mod=$(printf '%s' "${ko%.ko}" | $B tr - _)
  if $B grep -q "^$mod " /proc/modules 2>/dev/null; then
    say "MARBLE_MODEM_KO=$ko=present"
    return 0
  fi
  kpath=""
  for cand in "/graphics/$ko" "$R/vendor_dlkm/lib/modules/$ko" "/lib/modules/$ko" "$R/lib/modules/$ko"; do
    if $B test -f "$cand"; then kpath="$cand"; break; fi
  done
  if [ -z "$kpath" ]; then
    say "MARBLE_MODEM_KO=$ko=missing"
    return 1
  fi
  if $B insmod "$kpath" 2>/dev/null; then
    say "MARBLE_MODEM_KO=$ko=loaded"
  else
    say "MARBLE_MODEM_KO=$ko=insmod_failed"
    return 1
  fi
}

for ko in \
  qcom-scm.ko smem.ko qcom_glink_smem.ko qcom_glink.ko glink_pkt.ko \
  qmi_helpers.ko pdr_interface.ko qcom_sysmon.ko smp2p.ko qcom_pil_info.ko \
  qcom_ramdump.ko qcom_q6v5.ko qcom_q6v5_pas.ko qrtr.ko qrtr-smd.ko \
  mem_buf.ko secure_buffer.ko ipam.ko ipanetm.ko rmnet_core.ko rmnet_ctl.ko; do
  load_ko "$ko" || true
done

# ---------------------------------------------------------------------------
# 4. Resolve the MSS remoteproc index by NAME (indices move when adsp/cdsp/slpi
#    probe order changes; never hardcode remoteproc2).
# ---------------------------------------------------------------------------
idx=""
i=0
while [ "$i" -lt 40 ]; do
  for d in /sys/class/remoteproc/remoteproc*; do
    [ -e "$d" ] || continue
    name=$($B cat "$d/name" 2>/dev/null || cat "$d/name" 2>/dev/null)
    if [ "$name" = "$RPROC_NAME" ]; then
      idx=${d##*/remoteproc}
      break
    fi
  done
  [ -n "$idx" ] && break
  i=$((i + 1))
  $B sleep 0.2
done

if [ -z "$idx" ]; then
  say "MARBLE_MODEM_RPROC=none name=$RPROC_NAME"
  say "MARBLE_MODEM_RESULT=FAIL_NO_RPROC"
  # Diagnostic dump so lead can tell "DTB node missing" from "module not loaded".
  for d in /sys/class/remoteproc/remoteproc*; do
    [ -e "$d" ] || continue
    say "MARBLE_MODEM_RPROC_SEEN=${d##*/} name=$($B cat "$d/name" 2>/dev/null)"
  done
  $B grep -c qcom_q6v5_pas /proc/modules 2>/dev/null | sed 's/^/MARBLE_MODEM_PAS_MODULE_LINES=/'
  exit 33
fi

say "MARBLE_MODEM_RPROC=$idx name=$RPROC_NAME"
say "MARBLE_MODEM_RPROC_FIRMWARE=$($B cat /sys/class/remoteproc/remoteproc$idx/firmware 2>/dev/null)"
state=$($B cat /sys/class/remoteproc/remoteproc$idx/state 2>/dev/null)
say "MARBLE_MODEM_RPROC_STATE_INITIAL=$state"

# ---------------------------------------------------------------------------
# 5. Kick the MSS. cape_mpss_resource has auto_boot=false, so nothing else will.
# ---------------------------------------------------------------------------
if [ "$DO_BOOT" = "1" ]; then
  case "$state" in
    running) say "MARBLE_MODEM_BOOT=already_running" ;;
    *)
      if $B test -w "/sys/class/remoteproc/remoteproc$idx/state"; then
        echo start > "/sys/class/remoteproc/remoteproc$idx/state" 2>/dev/null \
          && say "MARBLE_MODEM_BOOT_REQUESTED" \
          || say "MARBLE_MODEM_BOOT=write_failed"
      else
        say "MARBLE_MODEM_BOOT=not_writable"
      fi
      ;;
  esac
fi

wait_s=${MARBLE_MODEM_RPROC_WAIT_SECONDS:-20}
i=0
state=""
while [ "$i" -lt $((wait_s * 5)) ]; do
  state=$($B cat "/sys/class/remoteproc/remoteproc$idx/state" 2>/dev/null)
  [ "$state" = "running" ] && break
  i=$((i + 1))
  $B sleep 0.2
done
say "MARBLE_MODEM_RPROC_STATE=$state"

if [ "$state" != "running" ]; then
  say "MARBLE_MODEM_RESULT=FAIL_RPROC_NOT_RUNNING"
  # Failure triage: firmware auth (SCM/TZ), memory carveout, glink, PD map.
  for f in /sys/class/remoteproc/remoteproc$idx/name /sys/class/remoteproc/remoteproc$idx/firmware; do
    say "MARBLE_MODEM_TRIAGE $(basename "$f")=$($B cat "$f" 2>/dev/null)"
  done
  $B dmesg 2>/dev/null | $B grep -iE "mpss|mss|q6v5|pas|scm" | $B tail -n 20 || true
  exit 34
fi

# ---------------------------------------------------------------------------
# 6. Report transport separately. The GLINK remote-pid in the device tree
#    is not a QRTR node address. The optional qmi_probe resolves real endpoints
#    via the local nameserver; remoteproc running is not a radio/SIM result.
# ---------------------------------------------------------------------------
if $B grep -q "^qrtr " /proc/modules 2>/dev/null; then
  say "MARBLE_MODEM_QRTR=module_loaded"
else
  say "MARBLE_MODEM_QRTR=module_missing"
fi

probe_result=not_staged
for p in /compat/bin/qmi_probe "$R/compat/bin/qmi_probe"; do
  if [ -x "$p" ]; then
    say "MARBLE_MODEM_QMI_PROBE=$p"
    probe_rc=0
    probe_out=$("$B" timeout -k 1 8 "$p" --services 2>&1) || probe_rc=$?
    [ -z "$probe_out" ] || printf '%s\n' "$probe_out"
    if [ "$probe_rc" = 0 ] &&
       printf '%s\n' "$probe_out" | "$B" grep -qE '^QRTR_UNIQUE_REMOTE_TOTAL=[1-9][0-9]*$'; then
      probe_result=remote_advertisement_seen
    else
      say "MARBLE_MODEM_QMI_PROBE_RC=$probe_rc"
      probe_result=unconfirmed
    fi
    break
  fi
done
say "MARBLE_MODEM_QRTR_DISCOVERY=$probe_result"
# This stage's exit code covers MSS boot only. QRTR advertisements, QMI
# transactions and framework SIM/radio results are separate observations.
say "MARBLE_MODEM_RESULT=MSS_RUNNING telephony=untested"
say "MARBLE_MODEM_STAGE_END"
exit 0