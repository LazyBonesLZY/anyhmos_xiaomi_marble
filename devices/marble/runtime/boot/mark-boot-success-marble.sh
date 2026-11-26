#!/busybox sh
# SPDX-License-Identifier: MIT
# Xiaomi marble (SM7475) boot_control marking wrapper.
#
# Invoked by the phase-4 orchestrator only after the desktop has been unlocked
# and the screenshot was captured, with the boot_id of *this* boot as $1:
#
#   mark-boot-success-marble.sh "$(cat /proc/sys/kernel/random/boot_id)"
#
# Every preflight failure prints `MARK_SUCCESS_SKIP reason=<why>` and exits
# non-zero so the caller can log it; nothing here pretends a slot was marked.
# The shipped build of compat/bootcontrol/marble_bootcontrol.c is read-only, so
# in practice this script reports the slot/retry state and prints
# `MARK_SUCCESS_SKIP reason=mark_path_not_compiled`.
set -eu

B=${MARBLE_BUSYBOX:-/busybox}
[ -x "$B" ] || B=
D=${MARBLE_BOOTCONTROL_DIR:-/compat/bootcontrol-marble}
BIONIC=${MARBLE_BIONIC_DIR:-$D/runtime}
BOOT_ID_FILE=${MARBLE_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
BOOTCONFIG=${MARBLE_BOOTCONFIG:-/proc/bootconfig}
BIN=$D/marble_bootcontrol

skip() {
 echo "MARK_SUCCESS_SKIP reason=$1"
 exit "$2"
}

run_binary() {
 LD_LIBRARY_PATH="$D:$BIONIC/lib64" $B timeout 12 \
  "$BIONIC/bin/linker64" "$BIN" "$@"
}

[ "$#" -eq 1 ] || skip argc 31
[ -n "$1" ] || skip empty_boot_id 32

current=$($B cat "$BOOT_ID_FILE" 2>/dev/null | $B tr -d ' \r\n') ||
 skip boot_id_unreadable 33
[ -n "$current" ] || skip boot_id_unreadable 33
[ "$1" = "$current" ] || skip boot_id_mismatch 33

$B grep -qx 'androidboot.hardware.sku = "marble"' "$BOOTCONFIG" ||
 skip bootconfig_sku_mismatch 34
$B grep -qx 'androidboot.hardware = "qcom"' "$BOOTCONFIG" ||
 skip bootconfig_hardware_mismatch 34
$B grep -qx 'androidboot.slot_suffix = "_a"' "$BOOTCONFIG" ||
 skip bootconfig_slot_mismatch 34

[ -x "$BIONIC/bin/linker64" ] || skip bionic_linker_missing 35
[ -f "$BIN" ] || skip probe_binary_missing 36

probe_rc=0
probe_out=$(run_binary probe --hal-audit 2>&1) || probe_rc=$?
echo "$probe_out"
[ "$probe_rc" -eq 0 ] || skip probe_exec_failed 37

case "$probe_out" in
 *GATES_ALL_PASS=1*) gates=1 ;;
 *) gates=0 ;;
esac
case "$probe_out" in
 *MARK_PATH_COMPILED=1*) compiled=1 ;;
 *) compiled=0 ;;
esac

if [ "$compiled" -ne 1 ]; then
 echo "MARBLE_BOOTCONTROL_MODE=read_only"
 skip mark_path_not_compiled 0
fi
[ "$gates" -eq 1 ] || skip gates_failed 38

mark_rc=0
run_binary mark "$1" || mark_rc=$?
[ "$mark_rc" -eq 0 ] || skip mark_failed 39
echo "MARK_SUCCESS_DONE boot_id=$1"
