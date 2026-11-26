#!/busybox sh
# Read-only readiness census; functional acceptance is a separate test.
B=/busybox; R=/hmos-root; fail=0
native() { $B timeout -k 1 2 $B chroot "$R" "$@"; }
sample_sa() {
 rc=0
 out=$(native /compat/bin/native_sa_check "$1" 2>&1) || rc=$?
 case "$rc:$out" in
  "0:NATIVE_SA_READY id=$1") state=ready ;;
  "7:SA $1 unavailable") state=not-published ;;
  *) state=probe-error ;;
 esac
}
echo "BOOT id=$($B cat /proc/sys/kernel/random/boot_id) up=$($B cut -d' ' -f1 /proc/uptime)"
vb=$($B sha256sum /dev/block/by-name/vendor_boot_a 2>/dev/null | $B cut -c1-16)
echo "IMAGE vb=${vb:-unknown}"
[ -n "$vb" ] || fail=1
for id in 1301 5002 5003 3203 3505 66090 4700 1901 801; do
 sample_sa "$id"
 echo "SA   $id=$state"
 [ "$state" = ready ] || fail=1
done
for id in 5000 8640 8650 4101; do
 sample_sa "$id"
 echo "SA   $id=$state load_untested=1"
 [ "$state" != probe-error ] || fail=1
done
sink=$($B pidof hmos_hisysevent_sink 2>/dev/null || true)
if [ -n "$sink" ]; then echo "SINK_PID alive $sink"; else echo 'SINK_PID none'; fail=1; fi
sock=0
for name in hisysevent hisysevent_fast; do
 [ ! -S "$R/dev/unix/socket/$name" ] || sock=$((sock+1))
done
echo "SOCK hisysevent=$sock"
if [ "$sock" -ne 2 ]; then fail=1; fi
se=$({
 if $B timeout 5 $B chroot "$R" /system/bin/hilog -x -T HISYSEVENT_TRANSPORT 2>/dev/null; then
  echo HMOS_LOG_SAMPLE_END
 fi
} | $B awk '/unix\/socket\/hisysevent.*errno=2/{n++} /^HMOS_LOG_SAMPLE_END$/{ok=1} END{if(ok)print n+0;else print "unavailable"}')
echo "HISYSEVENT_ERRNO2=$se sampled_hilog=1"
echo "BRIGHT saved=$($B cat "$R/data/service/el1/public/display/brightness_last" 2>/dev/null || echo none) now=$($B cat /sys/class/backlight/panel0-backlight/brightness 2>/dev/null)"
if [ -d /dev/snd ]; then D=/dev/snd; else D="$R/dev/snd"; fi
echo "SND dir=$D total=$($B ls "$D" 2>/dev/null | $B wc -l) capture=$($B ls "$D" 2>/dev/null | $B awk '/^pcm.*c$/{n++}END{print n+0}')"
if [ -e /dev/gpr_channel ] || [ -e "$R/dev/gpr_channel" ]; then g=node; else g=missing; fi
echo "GPR $g"
pal=$($B awk '/PAL_SERVER_READY=1|PAL_SERVER_[A-Z_]*RC/{last=$0} END{print last}' "$R/data/local/tmp/marble-audio.log" 2>/dev/null)
echo "PAL ${pal:-no-log} log_only=1"
echo "HDC daemon=$($B pidof hdcd 2>/dev/null || echo none) ffs_ready=$(native /system/bin/param get sys.usb.ffs.ready 2>/dev/null | $B head -n 1) host_untested=1"
w=$($B pidof wpa_host 2>/dev/null | $B awk '{print $1;exit}')
if [ -z "$w" ]; then t=none; else t=$($B awk '/^WpaMainThread$/{n++}END{print n+0}' /proc/"$w"/task/*/comm 2>/dev/null); fi
echo "WIFI wpa_host=${w:-none} thread=${t:-unavailable} wlan_verified=0"
if [ "$fail" = 0 ]; then result=pass; else result=fail; fi
echo "VERDICT $result scope=readiness end_to_end=untested"
[ "$fail" = 0 ]
