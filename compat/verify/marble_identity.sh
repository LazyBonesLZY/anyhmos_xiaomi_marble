#!/busybox sh
# Four-world identity dump (source/candidate/integration/device).
# Run before and after a flash; the device values are what matters here.
# Serial-callable: keep this file under 2800 bytes.
B=/busybox; R=/hmos-root; n=0
h() { $B sha256sum "$1" 2>/dev/null | $B cut -c1-16; }
say() { echo "$*"; n=$((n + 1)); }

say "ID boot=$($B cut -c1-8 /proc/sys/kernel/random/boot_id) up=$($B cut -d' ' -f1 /proc/uptime)"
say "ID vendor_boot=$(h /dev/block/by-name/vendor_boot_a) boot_a=$(h /dev/block/by-name/boot_a)"
for f in hmos_bt_bridge_marble hmos_hisysevent_sink hmos_brightness_persist tee_backend_probe native_codec_probe; do
 say "ID bin/$f=$(h $R/compat/bin/$f)"
done
say "ID cfg/init-platform=$(h $R/compat/etc/init-platform-marble.cfg)"
say "ID cfg/foundation=$(h $R/system/profile/foundation.json)"
say "ID cfg/stage=$(h /graphics/stage-phase4-marble.sh)"
say "ID cfg/start=$(h /graphics/start-phase4-marble.sh)"
say "ID cfg/init-seed=$(h /hmos-init-debug.cfg) effective=$(h /proc/1/root/system/etc/init.cfg)"
say "ID audio/ukee-policy=$(h $R/vendor/etc/audio/sku_ukee/audio_policy_configuration.xml)"
say "ID audio/runtime-libs=$($B ls $R/compat/audio-runtime/lib64 2>/dev/null | $B wc -l) agmshim=$(h $R/compat/audio-runtime/lib64/libhmos_agmipc_shim.so)"
say "ID nodes gpr=$([ -e $R/dev/gpr_channel ] && echo yes || echo no) snd=$($B ls $R/dev/snd 2>/dev/null | $B wc -l) dsp=$($B grep -c ' /hmos-root/dsp ' /proc/mounts 2>/dev/null || echo 0)"
if [ "$n" -lt 10 ]; then say "SELFCHECK lines=$n truncated"; fi
say "ID done lines=$n"