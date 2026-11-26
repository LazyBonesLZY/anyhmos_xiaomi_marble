#!/busybox sh
# SPDX-License-Identifier: MIT
# Xiaomi marble (SM7475) OHOS codec stage: av_codec_service (SA 3011) + codec HDI 4.0
# host + capability probe. Idempotent; safe to re-run from the serial shell.
#
# It never flashes, never touches fastboot/adb, never reboots. Everything it does is
# either (a) a read-only capability check, (b) an init `param set` that is already the
# documented way to request the media SAs, or (c) an opt-in hardware attempt that only
# runs with CODEC_HW_ATTEMPT=1.
#
# Env knobs:
#   CODEC_MODE=all|stage|probe   (default all)
#   CODEC_HW_ATTEMPT=0|1         (default 0; loads msm_video.ko + Venus firmware)
#   CODEC_INSTANCE_MIME=<mime>   (optional; also run the create/configure smoke test)
#   CODEC_DECODE_FILE=<in-chroot path>  (optional; runs a real decode)
set -eu

B=/busybox
[ -x "$B" ] || B=
R=${CODEC_HMOS_ROOT:-/hmos-root}
MODE=${CODEC_MODE:-all}
HW=${CODEC_HW_ATTEMPT:-0}
WORK=/data/local/tmp/marble-codec          # chroot-absolute
LOG=$R$WORK/probe.log
stage=env
# shellcheck disable=SC2154  # c is assigned inside the trap
 trap 'c=$?; echo "CODEC_START_EXIT=$c CODEC_STAGE=$stage"' EXIT

say() { echo "$*"; }
set_stage() { stage=$1; echo "CODEC_STAGE=$1"; }
inchroot() { $B chroot "$R" "$@"; }
has() { $B test -e "$R$1"; }

# BusyBox `ps -o` is not guaranteed; read /proc directly instead.
proc_running() {
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        c=$($B cat "$p/comm" 2>/dev/null) || continue
        [ "$c" = "$1" ] && return 0
    done
    return 1
}
proc_pid_matching() {
    for p in /proc/[0-9]*; do
        [ -r "$p/cmdline" ] || continue
        if $B grep -qa -- "$1" "$p/cmdline" 2>/dev/null; then
            echo "${p#/proc/}"
            return 0
        fi
    done
    return 1
}

# ---------------------------------------------------------------- stage: env
set_stage env
say "CODEC_BOOT_ID=$($B cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)"
[ -d "$R" ] || { say "CODEC_FATAL=no-hmos-root R=$R"; exit 20; }
[ -x "$R/system/bin/sa_main" ] || { say "CODEC_FATAL=no-sa_main"; exit 21; }
$B mkdir -p "$R$WORK"
say "CODEC_WORK_DIR=$WORK"

# ------------------------------------------------- stage: static capability map
# Read-only: what the donor OHOS rootfs actually contains. This is the part that
# distinguishes "codec stack present" from "codec stack can decode".
set_stage capability
SOFT_AUDIO=absent
SOFT_VIDEO=absent
has /system/lib64/media/media_plugins/libmedia_plugin_FFmpegAudioDecoders.z.so && SOFT_AUDIO=present
if has /system/lib64/media/media_plugins/libmedia_plugin_FFmpegVideoDecoders.z.so ||
   has /system/lib64/media/media_plugins/libmedia_plugin_VideoDecoder.z.so; then
    SOFT_VIDEO=present
fi
say "CODEC_SOFT_AUDIO_PLUGIN=$SOFT_AUDIO"
say "CODEC_SOFT_VIDEO_PLUGIN=$SOFT_VIDEO"

HDI_DRIVER=absent
HDI_PASSTHROUGH=absent
has /vendor/lib64/libcodec_driver.z.so && HDI_DRIVER=present
has /vendor/lib64/passthrough/libcodec_component_manager_service_4.0.z.so && HDI_PASSTHROUGH=present
say "CODEC_HDI_DRIVER=$HDI_DRIVER"
say "CODEC_HDI_PASSTHROUGH=$HDI_PASSTHROUGH"

# The codec HDI 4.0 service is only reachable when the HDF host config knows
# `codec_host`; the marble Phase 4 HCB ships allocator/composer/user_auth/wifi/wpa only.
HCB=/chip_prod/etc/hdfconfig/hdf_default.hcb
has "$HCB" || HCB=/system/etc/hdfconfig/hdf_default.hcb
CODEC_HOST_CFG=0
if has "$HCB" && $B grep -a -q codec_host "$R$HCB" 2>/dev/null; then
    CODEC_HOST_CFG=1
fi
say "CODEC_HCB=$HCB"
say "CODEC_HCB_CODEC_HOST=$CODEC_HOST_CFG"

# ------------------------------------------------------- stage: media SAs
set_stage media-sas
inchroot /system/bin/param set marble.debug.start.media true || true
say "CODEC_MEDIA_PARAM_SET=1"
waited=0
while [ "$waited" -lt 15 ]; do
    proc_running av_codec_service && break
    $B sleep 1
    waited=$((waited + 1))
done
SA_CODEC=missing
SA_MEDIA=missing
SA_SESSION=missing
proc_running av_codec_service && SA_CODEC=running
proc_running media_service && SA_MEDIA=running
proc_running av_session && SA_SESSION=running
say "CODEC_SA_WAIT_SECONDS=$waited"
say "CODEC_SA_AVCODEC_3011=$SA_CODEC"
say "CODEC_SA_MEDIA_3002=$SA_MEDIA"
say "CODEC_SA_AVSESSION_3010=$SA_SESSION"
say "CODEC_SA_NOTE=process_running_is_not_decode_capability"

# ------------------------------------------------------- stage: codec HDI host
set_stage hdi-host
HOST_PID=none
if [ "$CODEC_HOST_CFG" = "1" ]; then
    HOST_PID=$(proc_pid_matching hdf_devhost 2>/dev/null || true)
    if [ -n "$HOST_PID" ]; then
        say "CODEC_CODEC_HOST=present pid=$HOST_PID"
    elif has /vendor/bin/hdf_devhost; then
        ( inchroot /vendor/bin/hdf_devhost -n codec_host >"$R$WORK/codec_host.log" 2>&1 & ) || true
        $B sleep 2
        HOST_PID=$(proc_pid_matching hdf_devhost 2>/dev/null || true)
        say "CODEC_CODEC_HOST=started pid=${HOST_PID:-none}"
    else
        say "CODEC_CODEC_HOST=blocked:no-hdf_devhost-binary"
    fi
else
    say "CODEC_CODEC_HOST=blocked:hcb_missing_codec_host"
    say "CODEC_CODEC_HOST_HINT=packaging_must_add_a_codec_node_to_the_marble_hcb"
fi

# ------------------------------------------------------ stage: hardware attempt
set_stage hardware
if [ "$HW" = "1" ]; then
    # Venus firmware is NOT part of the donor OHOS rootfs; the operator stages the
    # marble stock dump files (vpu20_4v.*) under /data/local/tmp/marble-codec/firmware.
    FW_SRC=${CODEC_FW_SRC:-$R$WORK/firmware}
    fw_count=0
    for f in "$FW_SRC"/vpu20_4v.*; do
        [ -f "$f" ] || continue
        fw_count=$((fw_count + 1))
        for dst in /compat/firmware "$R/compat/firmware"; do
            $B mkdir -p "$dst"
            $B cp "$f" "$dst/${f##*/}"
            $B chmod 0644 "$dst/${f##*/}"
        done
    done
    say "CODEC_FW_STAGED=$fw_count"
    [ "$fw_count" -eq 0 ] && say "CODEC_HW_BLOCKER=firmware-vpu20_4v-missing"
    # CRITICAL: no trailing newline in the firmware_class path (docs/MARBLE_HANDOFF.md)
    if [ -w /sys/module/firmware_class/parameters/path ]; then
        printf '/compat/firmware' > /sys/module/firmware_class/parameters/path
    fi
    load_ko() {
        ko=$1
        mod=$(printf '%s' "${ko%.ko}" | $B tr - _)
        $B grep -q "^$mod " /proc/modules && return 0
        for d in "$R/vendor_dlkm/lib/modules" "$R/lib/modules"; do
            [ -f "$d/$ko" ] || continue
            if $B insmod "$d/$ko"; then
                say "CODEC_MOD_LOADED $ko"
                return 0
            fi
            say "CODEC_MOD_WARN $ko"
            return 1
        done
        say "CODEC_MOD_MISSING $ko"
        return 1
    }
    for ko in qcom-scm.ko mdt_loader.ko mem_buf_dev.ko msm-mmrm.ko llcc-qcom.ko \
              videocc-waipio.ko gdsc.ko msm_video.ko; do
        load_ko "$ko" || true
    done
    if $B grep -q '^msm_video ' /proc/modules; then
        say "CODEC_HW_MODULE=msm_video loaded"
    else
        say "CODEC_HW_MODULE=msm_video NOT-loaded"
    fi
    say "CODEC_HW_DEVDIR=$($B ls -d /sys/class/video4linux/* 2>/dev/null | $B tr '\n' ' ' || echo none)"
    say "CODEC_HW_DMESG=$($B dmesg 2>/dev/null | $B grep -i 'msm_vidc\|venus' | $B tail -n 5 | $B tr '\n' '|' || echo none)"
else
    say "CODEC_HW_ATTEMPT=skipped"
fi

# ------------------------------------------------------- stage: probe
if [ "$MODE" = "stage" ]; then
    set_stage complete
    say "CODEC_STAGE_ONLY=1"
    exit 0
fi
set_stage probe
PROBE=$R$WORK/native_codec_probe
if $B test -x "$PROBE"; then
    $B chmod 0755 "$PROBE" 2>/dev/null || true
    rc=0
    inchroot "$WORK/native_codec_probe" >"$LOG" 2>&1 || rc=$?
    say "CODEC_PROBE_LOG=$WORK/probe.log"
    hw=$($B grep -c 'type=hw' "$LOG" 2>/dev/null || true)
    sw=$($B grep -c 'type=sw' "$LOG" 2>/dev/null || true)
    say "CODEC_PROBE_HW=${hw:-0}"
    say "CODEC_PROBE_SW=${sw:-0}"
    say "CODEC_PROBE_EXIT=$rc"
    if [ -n "${CODEC_INSTANCE_MIME:-}" ]; then
        irc=0
        inchroot "$WORK/native_codec_probe" --instance "$CODEC_INSTANCE_MIME" >>"$LOG" 2>&1 || irc=$?
        say "CODEC_INSTANCE_MIME=$CODEC_INSTANCE_MIME rc=$irc"
    fi
    if [ -n "${CODEC_DECODE_FILE:-}" ] && $B test -f "$R$CODEC_DECODE_FILE"; then
        drc=0
        inchroot "$WORK/native_codec_probe" --decode "${CODEC_DECODE_MIME:-video/avc}" \
            "$CODEC_DECODE_FILE" "${CODEC_DECODE_W:-1280}" "${CODEC_DECODE_H:-720}" >>"$LOG" 2>&1 || drc=$?
        say "CODEC_DECODE_RC=$drc"
    fi
else
    say "CODEC_PROBE=blocked:missing-$WORK/native_codec_probe"
fi

set_stage complete
say "CODEC_STAGE_COMPLETE=1"