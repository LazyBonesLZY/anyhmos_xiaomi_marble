#!/busybox sh
# SPDX-License-Identifier: MIT
# Xiaomi marble Phase 4 desktop bring-up orchestrator (Display + Adreno 725 GPU + Desktop).
set -eu
B=/busybox
R=/hmos-root
LOG="$R/data/local/tmp/marble-phase4-autostart.log"
stage=prepare
now() { read -r seconds _rest < /proc/uptime; echo "${seconds%.*}"; }
log() {
 read -r s _idle < /proc/uptime
 msg="MARBLE_P4_STAGE name=$stage uptime=$s $*"
 echo "$msg"
 echo "$msg" >> "$LOG" 2>/dev/null || true
}
pset() {
 $B chroot "$R" /system/bin/param set "$1" "$2" >/dev/null 2>&1 || true
}
pget() {
 $B chroot "$R" /system/bin/param get "$1" 2>/dev/null | $B tr -d ' \t\r\n' || true
}
wait_param() {
 key="$1"; want="$2"; limit="$3"
 deadline=$(($(now) + limit))
 while :; do
  val=$(pget "$key")
  if [ "$val" = "$want" ]; then
   log "PARAM_OK $key=$val"
   return 0
  fi
  [ "$(now)" -lt "$deadline" ] || { log "PARAM_TIMEOUT $key=$val want=$want"; return 1; }
  $B sleep 1
 done
}
# Same selective skip as the stage script: a markdown-free one-liner in
# /data/local/tmp/marble-skip-stages disables one subsystem for the next boot,
# which is how a regression gets bisected without reflashing.
SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d " ")
skipped() {
 [ -n "$SKIP" ] || return 1
 case ",$SKIP," in
  *",$1,"*) return 0 ;;
 esac
 return 1
}
log "SKIP_STAGES=${SKIP:-none}"

wait_sa() {
 id="$1"; limit="$2"
 deadline=$(($(now) + limit))
 while :; do
  if $B timeout 3 $B chroot "$R" /compat/bin/native_sa_check "$id" >/dev/null 2>&1; then
   log "SA_READY id=$id"
   return 0
  fi
  [ "$(now)" -lt "$deadline" ] || { log "SA_TIMEOUT id=$id"; return 1; }
  $B sleep 1
 done
}

: > "$LOG"
boot_id=$($B cat /proc/sys/kernel/random/boot_id)
log "BEGIN boot_id=$boot_id"

# 1. Wait for hilogd, samgr, hdf_devmgr (allocator_host & composer_host auto-start on hdf_devmgr ready).
stage=hilog
deadline=$(($(now) + 40))
while ! $B test -S "$R/dev/unix/socket/hilogInput"; do
 [ "$(now)" -lt "$deadline" ] || { log "HILOG_SOCKET_TIMEOUT"; exit 21; }
 $B sleep 1
done
log "HILOG_SOCKET_READY"
wait_param bootevent.samgr.ready true 40 || exit 22
wait_param bootevent.hdf_devmgr.ready true 40 || exit 23

# Start continuous background hilog capture on ext4 /data.
$B setsid $B timeout 900 $B chroot "$R" /system/bin/hilog -L I,W,E,F > "$R/data/local/tmp/marble-p4-hilog.txt" 2>&1 </dev/null &

# 2. Core deps & storage daemon.
stage=coredeps
pset marble.debug.start.coredeps true
if skipped storage; then
 log "STORAGE_SKIP=requested"
else
 pset marble.debug.start.storaged true
fi
$B sleep 2

stage=memmgr
pset marble.debug.start.memmgr true
wait_sa 1909 12 || true

# 3. Initialize main user storage directories via sdc.
stage=sdc
sdc_out=$($B timeout 15 $B chroot "$R" /system/bin/sdc filecrypt init_main_user 2>&1 || true)
log "SDC_INIT=$sdc_out"

# 4. Core framework & account & appspawn & useriam & UI support services.
stage=services
for gate in storage kvdata useriam account appspawn theme wallpaper ui diagnostics deviceproviders; do
 pset "marble.debug.start.$gate" true
done
wait_sa 1301 60 || true

stage=time_framework
pset marble.debug.start.time true
wait_sa 3702 15 || true
pset marble.debug.start.framework true
wait_sa 1602 8 || true
wait_sa 404 8 || true

# 5. Graphics render_service (Adreno 725 Turnip + single-DSI composer_host) & installs!
stage=graphics
pset marble.debug.start.render true
pset marble.debug.start.installs true
wait_param bootevent.renderservice.ready true 35 || true
wait_sa 10 15 || true

# 4b. hisysevent sink supervision.  Starting it from the stage script is not
# enough: it was observed dead with the socket files left behind, which silently
# brings the errno=2 storm back.  Re-check and restart here, keeping a log.
if [ -x "$R/compat/bin/hmos_hisysevent_sink" ]; then
 if ! $B pidof hmos_hisysevent_sink >/dev/null 2>&1; then
  $B setsid $B chroot "$R" /compat/bin/hmos_hisysevent_sink -q --socket-dir /dev/unix/socket \
   --stats /data/local/tmp/hisysevent-sink.log \
   > "$R/data/local/tmp/hisysevent-sink.out" 2>&1 </dev/null &
  $B sleep 1
 fi
 log "HISYSEVENT_SINK=$($B pidof hmos_hisysevent_sink 2>/dev/null || echo none) sockets=$($B ls "$R/dev/unix/socket" 2>/dev/null | $B grep -c hisysevent)"
fi

# 6. Hardware (battery HDI + powermgr), multimodalinput, pasteboard/msdp.
stage=hardware_input
for ev in /sys/class/input/event*; do
 [ -r "$ev/dev" ] || continue
 base=${ev##*/}
 nums=$($B cat "$ev/dev")
 $B test -e "$R/dev/input/$base" || $B mknod "$R/dev/input/$base" c "${nums%:*}" "${nums#*:}"
 $B chown 6696:6696 "$R/dev/input/$base"
 $B chmod 0660 "$R/dev/input/$base"
 ev_name=$($B cat "$ev/device/name" 2>/dev/null || echo unknown)
 log "INPUT_READY node=$base dev=$nums name=$ev_name"
done
pset marble.debug.start.hardware true
pset marble.debug.start.input true
pset marble.debug.start.uideps true
wait_sa 3701 15 || true
wait_sa 2902 15 || true

# 6a. PAL/AGM PCM service. audio_server\'s IAudioPolicy waits up to 5000 ms for
# the PAL socket; without it the audio SAs retry and volume keys stay dead.
stage=audio_pal
ART=/compat/audio-runtime
AGM_LOG="$R/data/local/tmp/marble-agmipc.log"
if [ -x "$R$ART/bin/agmipc-service" ]; then
 # Android HIDL service; it needs /dev/hwbinder, which OHOS exposes as
 # /dev/hmos_hwbinder.  Best effort: failure only costs one log line.
 [ -e /dev/hwbinder ] || $B ln -sf /dev/hmos_hwbinder /dev/hwbinder 2>/dev/null || true
 LD_LIBRARY_PATH=$ART/lib64 $B chroot "$R" $ART/bin/linker64 $ART/bin/agmipc-service \
  > "$AGM_LOG" 2>&1 </dev/null &
 agm_pid=$!
 $B sleep 2
 log "AGMIPC_STATUS=$($B tail -n 1 "$AGM_LOG" 2>/dev/null | $B cut -c1-90) pid=$agm_pid"
else
 log "AGMIPC_SKIP=no-binary"
fi
ALOG="$R/data/local/tmp/marble-audio.log"
if [ -x "$R$ART/bin/linker64" ] && [ -x "$R$ART/bin/agmplay" ]; then
 LD_LIBRARY_PATH=$ART/lib64 \
 LD_PRELOAD="$ART/lib64/libhmos_agmipc_shim.so $ART/lib64/libhmos_pal_lifecycle.so $ART/lib64/libhmos_pal_server.so" \
 HMOS_AUDIO_CARD=0 \
 HMOS_AUDIO_MIXER_XML=/vendor/etc/audio/sku_ukee/mixer_paths_ukee_mtp.xml \
  $B chroot "$R" $ART/bin/linker64 $ART/bin/agmplay > "$ALOG" 2>&1 &
 pal_pid=$!
 pal_wait=0
 while [ "$pal_wait" -lt 20 ]; do
  $B grep -q 'PAL_SERVER_READY=1' "$ALOG" 2>/dev/null && break
  $B sleep 1
  pal_wait=$((pal_wait+1))
 done
 pal_state=$($B grep -m1 -E 'PAL_SERVER_READY=1|PAL_SERVER_AGM_INIT_RC|PAL_SERVER_PAL_INIT_RC|AUDIOROUTE_SHIM_INIT_FAIL' "$ALOG" 2>/dev/null || echo not-ready)
 log "PAL_SERVER_STATUS=$pal_state pid=$pal_pid wait=${pal_wait}s"
else
 log "PAL_SERVER_SKIP=no-runtime"
fi

# 6b. Audio policy/server & media/av_session/av_codec (enables volume keys & media SAs).
stage=audio_media
pset marble.debug.start.audio true
pset marble.debug.start.media true
wait_sa 3009 15 || true
wait_sa 3001 10 || true
wait_sa 3002 10 || true
wait_sa 3010 10 || true
wait_sa 3011 10 || true

# Load Venus VPU (/dev/video32..33) before boot animation starts so HCODEC HW H.264 decode succeeds.
if [ -x /compat/bin/start-venus-marble.sh ]; then
 if $B timeout -k 2 8 $B sh /compat/bin/start-venus-marble.sh > "$R/data/local/tmp/marble-venus-early.log" 2>&1; then
  log "VENUS_EARLY_READY=1"
 else
  log "VENUS_EARLY_RC=$?"
 fi
fi

# BEGIN marble native boot animation
# Start the existing init-owned original player while boot is still in progress.
# This does not wait for playback or require it for desktop readiness.
bootanim_owned=0
bootanim_early_wake=0
bootanim_param() {
 $B timeout -k 1 2 $B chroot "$R" /system/bin/param "$@"
}
bootanim_get() {
 avalue=$(bootanim_param get "$1" 2>/dev/null) || return 1
 printf '%s' "$avalue" | $B tr -d ' \t\r\n'
}
bootanim_start() {
 askip="$R/data/local/tmp/marble-skip-stages"
 if [ -f "$askip" ] && [ ! -L "$askip" ]; then
  askip_value=$($B tr -d ' \t\r\n' < "$askip" || true)
  case ",$askip_value," in *,bootanim,*|*,bootanimation,*) log 'BOOTANIM_SKIP=requested'; return 0;; esac
 fi
 acfg="$R/sys_prod/etc/bootanimation/bootanimation_custom_config.json"
 avideo="$R/sys_prod/etc/bootanimation/cust_bootvideo.mp4"
 if [ ! -x "$R/compat/bin/bootanimation-launcher" ] || [ ! -x "$R/compat/bin/bootanimation-real" ] ||
    [ ! -s "$R/compat/lib64/libhmos_boot_vulkan.so" ] || [ ! -f "$acfg" ] || [ ! -r "$acfg" ] ||
    [ ! -s "$acfg" ] || [ ! -f "$avideo" ] || [ ! -r "$avideo" ] || [ ! -s "$avideo" ]; then
  log 'BOOTANIM_SKIP=missing_player_or_donor_asset'
  return 0
 fi
 if [ "$(bootanim_get bootevent.boot.completed || true)" = true ] ||
    [ "$(bootanim_get bootevent.bootanimation.started || true)" = true ] ||
    [ "$(bootanim_get marble.debug.start.bootanim || true)" = true ]; then
  log 'BOOTANIM_SKIP=already_started_or_completed'
  return 0
 fi
 if [ "$(bootanim_get bootevent.renderservice.ready || true)" != true ]; then
  log 'BOOTANIM_SKIP=render_not_ready'
  return 0
 fi
 for asa in 10 3002 3011 3301; do
  if ! $B timeout -k 1 2 $B chroot "$R" /compat/bin/native_sa_check "$asa" >/dev/null 2>&1; then
   log "BOOTANIM_SKIP=sa_not_ready id=$asa"
   return 0
  fi
 done
 if ! $B mkdir "$R/dev/marble-bootanim-attempt" 2>/dev/null; then
  log 'BOOTANIM_SKIP=attempt_already_owned'
  return 0
 fi
 if [ "$(bootanim_get marble.debug.display.wake || true)" != true ]; then
  # The request may reach init even if the parameter client times out.
  bootanim_early_wake=1
  if ! bootanim_param set marble.debug.display.wake true >/dev/null 2>&1; then
   log 'BOOTANIM_SKIP=early_wake_request_failed'
   return 0
  fi
 fi
 bootanim_owned=1
 if bootanim_param set marble.debug.start.bootanim true >/dev/null 2>&1; then
  log 'BOOTANIM_START_REQUESTED=1 pixels_unverified=1'
 else
  log 'BOOTANIM_START_REQUEST_FAILED'
 fi
}
bootanim_rearm_wake() {
 # Preserve the established late desktop wake transition, even if the early
 # startup-animation wake already requested the same init job.
 if [ "$bootanim_early_wake" = 1 ]; then
  bootanim_param set marble.debug.display.wake false >/dev/null 2>&1 || log 'BOOTANIM_WAKE_REARM_FAILED'
 fi
}
bootanim_finish() {
 [ "$bootanim_owned" = 1 ] || return 0
 anative=$(bootanim_get bootevent.bootanimation.finished || true)
 case "$anative" in true|false) :;; *) anative=unknown;; esac
 # The donor gets the normal boot.completed notification and the existing
 # handoff window. Stop only our init request if its gate is still open.
 # Do not abandon cleanup on an inconclusive parameter read.
 if [ "$(bootanim_get marble.debug.start.bootanim || true)" != false ]; then
  if bootanim_param set marble.debug.start.bootanim false >/dev/null 2>&1; then
   log "BOOTANIM_STOP_REQUESTED=1 native_finished=$anative"
  else
   log "BOOTANIM_STOP_REQUEST_FAILED native_finished=$anative"
  fi
 fi
}
bootanim_start
# END marble native boot animation

# 6c. QCA6490 Wi-Fi (CNSS2 + MHI + wlan0 + wifi_host/wpa_host/wifi_manager) & Netstack.
stage=wifi_netstack
if $B test -x "$R/compat/bin/start-wifi-marble.sh"; then
 if $B timeout 50 $B sh "$R/compat/bin/start-wifi-marble.sh" > "$R/data/local/tmp/marble-wifi-startup.log" 2>&1; then
  log "WIFI_BRINGUP_REQUESTED=1 wlan_verified=0"
 else
  log "WIFI_START_WARN rc=$?"
 fi
fi
pset marble.debug.start.netstack true
for id in 1120 1124 1126 1158 1151; do
 wait_sa "$id" 12 || true
done

# 6e. Bluetooth H4 bridge (opt-in through /data/local/tmp/bt-autostart).
if skipped bluetooth; then log "BLUETOOTH_SKIP=requested";
elif [ -f "$R/data/local/tmp/bt-autostart" ] && [ -x "$R/compat/bin/hmos_bt_bridge_marble" ]; then
 BT_LOG="$R/data/local/tmp/marble-bluetooth.log"
 $B setsid $B chroot "$R" /compat/bin/hmos_bt_bridge_marble > "$BT_LOG" 2>&1 </dev/null &
 bt_pid=$!
 bt_wait=0
 while [ "$bt_wait" -lt 12 ]; do
  $B grep -q 'BRIDGE_LISTENING=1' "$BT_LOG" 2>/dev/null && break
  $B sleep 1
  bt_wait=$((bt_wait+1))
 done
 log "BLUETOOTH_STATUS=$($B grep -m1 -E 'BRIDGE_LISTENING=1|BRINGUP_FAILED|HCI_TIMEOUT' "$BT_LOG" 2>/dev/null || echo not-ready) pid=$bt_pid wait=${bt_wait}s"
 $B grep -q 'BRIDGE_LISTENING=1' "$BT_LOG" 2>/dev/null && pset marble.debug.start.bluetooth true
else
 log "BLUETOOTH_SKIP=not-requested"
fi

# 6f. USB-only HDC (explicit /data/local/tmp/usb-autostart marker).
# The host-namespace helper prepares FunctionFS and requests the native init
# service. Developer mode, daemon identity, and host authorization stay native.
usb_marker="$R/data/local/tmp/usb-autostart"
if skipped usb; then
 log "USB_SKIP=requested"
elif [ -L "$usb_marker" ] || [ ! -f "$usb_marker" ]; then
 log "USB_SKIP=not-requested"
elif [ ! -x "$R/compat/bin/start-usb-marble.sh" ]; then
 log "USB_SKIP=helper-missing"
elif usb_log=$($B mktemp "$R/dev/marble-usb.XXXXXX"); then
 usb_rc=0
 MODE=hdc $B timeout -k 2 55 $B sh "$R/compat/bin/start-usb-marble.sh" > "$usb_log" 2>&1 || usb_rc=$?
 log "USB_START=rc=$usb_rc trace=$usb_log host_untested=1"
 if [ "$usb_rc" -eq 0 ] && $B grep -q '^USB_FFS_READY=1 ' "$usb_log" &&
    $B grep -qE '^USB_UDC_(BOUND|ALREADY_BOUND) ' "$usb_log"; then
  log "HDC_ENDPOINT_READY=1 host_untested=1"
 fi
else
 log "USB_SKIP=no-ram-log"
fi

# 7. Foundation (BMS + AMS + WMS + ScreenLock -> launches SceneBoard!).
stage=scene
pset marble.debug.start.screen true
pset marble.debug.start.compconfig true
pset marble.debug.start.ime true
# Theme/wallpaper/location are plain system services; bring them up by default
# so the desktop can render themed surfaces and apps get a location provider.
pset marble.debug.start.theme true
pset marble.debug.start.wallpaper true
pset marble.debug.start.location true
pset marble.debug.start.storage true
if skipped storage; then log "STORAGE_EXTRA_SKIP=requested"; else pset marble.debug.start.extra true; fi
wait_sa 3703 20 || true

# native_sa_dump opens with O_EXCL. Keep attempts in a fresh RAM directory
# so old userdata snapshots cannot delay this boot.
if scene_dir_host=$($B mktemp -d "$R/dev/hmos-phase4-scene.XXXXXX"); then
 scene_dir=${scene_dir_host#"$R"}
 deadline=$(($(now) + 240))
 i=0
 scene_registered=0
 while [ "$(now)" -lt "$deadline" ]; do
  path="$scene_dir/$i.txt"
  if $B timeout 3 $B chroot "$R" /compat/bin/native_sa_dump 4606 "$path" -a >/dev/null 2>&1 &&
     $B grep -qE '^SCB(ScreenLock|Desktop)[0-9]*[[:space:]]' "$R$path"; then
   log "SCENEBOARD_WINDOW_REGISTERED dump=$path"
   rects=$($B awk -F '[][]' '/^SCB(ScreenLock|Desktop)[0-9]*[[:space:]]/ {print $2}' "$R$path" |
     $B awk '{$1=$1; printf "%s%s", NR==1?"":";", $0}')
   log "SCENEBOARD_RECTS=${rects:-unknown}"
   scene_registered=1
   break
  fi
  $B rm -f "$R$path"
  i=$((i + 1))
  $B sleep 2
 done
 [ "$scene_registered" = 1 ] || log "SCENEBOARD_POLL_TIMEOUT"
else
 log "SCENEBOARD_DUMP_DIR_UNAVAILABLE"
fi

# 8. Wake display, unlock screenlock, emit boot completed, and capture display0 screenshot.
stage=wake_unlock
bootanim_rearm_wake
pset marble.debug.display.wake true
$B sleep 2
# Apply the saved brightness only after the display power manager has come up,
# otherwise it re-applies its own default on top of ours (observed: 40 -> 3).
if [ -x "$R/compat/bin/hmos_brightness_persist" ]; then
 log "BRIGHTNESS_APPLY=$($B chroot "$R" /compat/bin/hmos_brightness_persist apply 2>&1 | $B tail -n 1)"
 $B sleep 3
 log "BRIGHTNESS_APPLY2=$($B chroot "$R" /compat/bin/hmos_brightness_persist apply 2>&1 | $B tail -n 1)"
fi
$B sleep 2
pset marble.debug.screen.unlock true
$B sleep 4
if $B test -s "$R/data/local/tmp/hmos-capture/unlock-request.txt"; then
 log "UNLOCK_OUT=$($B tr '\n' ' ' < "$R/data/local/tmp/hmos-capture/unlock-request.txt")"
fi

pset bootevent.boot.completed true
log "BOOT_COMPLETED_EMITTED"
$B sleep 10
bootanim_finish

stage=capture
shot_dir="$R/data/local/tmp/hmos-phase4-capture"
shot="$shot_dir/display0.jpeg"
shot_root=$($B stat -c '%d:%i' "$R/dev/hmos-phase4-capture" 2>/dev/null || true)
shot_mount=$($B stat -c '%d:%i' "$shot_dir" 2>/dev/null || true)
if [ -n "$shot_root" ] && [ "$shot_root" = "$shot_mount" ] && [ ! -e "$shot" ] && [ ! -L "$shot" ]; then
 pset marble.debug.start.capture true
 deadline=$(($(now) + 15))
 snapshot_written() {
  [ -f "$shot" ] && [ ! -L "$shot" ] &&
   [ "$( $B od -An -tx1 -N2 "$shot" | $B tr -d ' \n')" = ffd8 ] &&
   [ "$( $B tail -c 2 "$shot" | $B od -An -tx1 | $B tr -d ' \n')" = ffd9 ]
 }
 while ! snapshot_written && [ "$(now)" -lt "$deadline" ]; do $B sleep 1; done
 if snapshot_written; then
  sz=$($B stat -c '%s' "$shot")
  log "SNAPSHOT_FILE_READY bytes=$sz image_content_unverified=1"
 else
  log "SNAPSHOT_FILE_MISSING_OR_INCOMPLETE"
 fi
else
 log "SNAPSHOT_FILE_SKIPPED reason=not_fresh_ram_path"
fi

stage=complete
log "PHASE4_ORCHESTRATOR_DONE procs=$($B ps | $B wc -l)"
