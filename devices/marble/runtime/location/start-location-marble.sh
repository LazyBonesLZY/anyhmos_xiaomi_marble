#!/busybox sh
set -eu
B=/busybox
R=/hmos-root
M="$R/vendor_dlkm/lib/modules"
S=/compat/original-firmware
stage=boot
trap 'c=$?; echo "LOC_START_EXIT=$c STAGE=$stage"' EXIT
st(){ stage=$1; echo "LOC_STAGE=$1"; }
ko(){
 m=$(printf %s "${1%.ko}" | $B tr - _)
 $B grep -q "^$m " /proc/modules && { echo "LOC_MOD_PRESENT $1"; return 0; }
 [ -f "$M/$1" ] || { echo "LOC_MOD_MISSING $1"; return 0; }
 $B insmod "$M/$1" >/dev/null 2>&1 && echo "LOC_MOD_LOADED $1" || echo "LOC_MOD_FAILED $1"
}
st env
L1="$R/data/service/el1/public/location"
L2="$R/data/service/el2/public/location"
$B mkdir -p "$R/data/vendor/gnss" /compat/firmware "$R/compat/firmware"
$B mkdir -p -m 0711 "$L1" "$L2"
$B chown 1021:1021 "$R/data/vendor/gnss" "$L1" "$L2" 2>/dev/null || true
st firmware
if [ ! -f "$S/image/slpi2.mdt" ] && ! $B grep -q ' /compat/original-firmware ' /proc/mounts; then
 $B mkdir -p "$S"
 $B mount -t vfat -o ro /dev/block/by-name/modem_a "$S" 2>/dev/null || true
fi
for f in "$S"/image/slpi2.* "$S"/image/slpir.jsn "$S"/image/slpius.jsn; do
 [ -f "$f" ] || continue
 n=${f##*/}
 for d in /compat/firmware "$R/compat/firmware"; do
  $B cp "$f" "$d/$n" >/dev/null 2>&1 || true
  $B chmod 0644 "$d/$n" 2>/dev/null || true
 done
done
echo "LOC_FW_SLPI=$([ -f "$R/compat/firmware/slpi2.mdt" ] && echo staged || echo missing)"
st modules
for ko in qmi_helpers.ko qrtr.ko qrtr-smd.ko smp2p.ko qcom_pil_info.ko \
 pdr_interface.ko qcom_sysmon.ko qcom_q6v5.ko qcom_q6v5_pas.ko sensors_ssc.ko; do ko "$ko"; done
st rproc
# SLPI name from stock.
slpi=missing
rp(){ $B cat "$1" 2>/dev/null || echo "${2:-?}"; }
for d in /sys/class/remoteproc/remoteproc*; do
 [ -d "$d" ] || continue
 n=$(rp "$d/name")
 echo "LOC_RPROC $n $(rp "$d/state") fw=$(rp "$d/firmware")"
 [ "$n" = 2400000.remoteproc-slpi ] || continue
 if [ "$slpi" = missing ]; then slpi=$d; else slpi=ambiguous; fi
done
st slpi
boot=/sys/kernel/boot_slpi/boot
if [ "${LOC_BOOT_SLPI:-0}" != 1 ]; then
 echo LOC_SLPI_SKIPPED=1
elif [ "$slpi" = missing ] || [ "$slpi" = ambiguous ]; then
 echo "LOC_SLPI_RPROC=$slpi"
elif [ ! -f "$boot" ]; then
 echo LOC_SLPI_KNOB=absent
elif [ -w "$boot" ] && echo 1 2>/dev/null >"$boot"; then
 echo LOC_SLPI_BOOT=requested
 i=0
 while :; do
  s=$(rp "$slpi/state" unknown)
  [ "$s" != running ] && [ "$i" -lt 25 ] || break
  i=$((i + 1))
  $B sleep 1
 done
 echo "LOC_SLPI_STATE=$s"
else
 echo LOC_SLPI_BOOT=failed
fi
st framework
hub=failed
$B chroot "$R" /system/bin/param set marble.debug.start.location true >/dev/null 2>&1 && hub=requested
echo "LOC_HUB_START=$hub"
i=0
ok=missing
while [ "$i" -lt 25 ]; do
 if $B timeout 4 $B chroot "$R" /compat/bin/native_sa_check 2802 >/dev/null 2>&1; then ok=ready; break; fi
 i=$((i + 1))
 $B sleep 1
done
echo "LOC_SA_2802=$ok"
echo LOC_GNSS_FIX=unverified
st complete
