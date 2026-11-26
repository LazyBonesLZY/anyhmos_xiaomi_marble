#!/busybox sh
# SPDX-License-Identifier: MIT
# marble(SM7475/ukee) theme/wallpaper bring-up + verification.
# Declares NO service: theme_manager_service(65846) and wallpaper_service(3705) already
# exist in devices/marble/runtime/init/init-desktop-marble.cfg (jobs param:marble-debug-*).
set -eu
B=/busybox
R=/hmos-root
L="$R/data/local/tmp/marble-theme-category.txt"
stage=boot
# shellcheck disable=SC2154  # c is assigned inside the trap
 trap 'c=$?; echo "THEME_START_EXIT=$c STAGE=$stage"' EXIT
st(){ stage=$1; echo "THEME_STAGE=$1"; }
st env
for p in /system/profile/theme_manager_service.json /system/profile/wallpaper_service.json \
 /system/lib64/libtheme_manager_server.z.so /system/lib64/libwallpaper_service.z.so \
 /system/etc/wallpaperdefault.jpeg /system/etc/wallpaperlockdefault.jpeg; do
 if $B test -e "$R$p"; then echo "THEME_FILE_PRESENT $p"; else echo "THEME_FILE_MISSING $p"; fi
done

st dirs
for d in "$R/data/service/el1/public/themes" "$R/data/service/el1/public/themes/container" \
 "$R/data/service/el1/public/themes/container/fonts"; do
 $B mkdir -p "$d"
 $B chown 3817:3817 "$d" 2>/dev/null || true
 $B chmod 0775 "$d" 2>/dev/null || true
done
$B mkdir -p "$R/data/service/el1/public/wallpaper"
$B chown 3817:3817 "$R/data/service/el1/public/wallpaper" 2>/dev/null || true
$B chmod 0770 "$R/data/service/el1/public/wallpaper" 2>/dev/null || true
echo "THEME_DIR_THEMES=$($B stat -c '%u:%g %a' "$R/data/service/el1/public/themes" 2>/dev/null || echo unknown)"
echo "THEME_DIR_WALLPAPER=$($B stat -c '%u:%g %a' "$R/data/service/el1/public/wallpaper" 2>/dev/null || echo unknown)"
if [ -f "$R/data/service/el1/public/themes/theme_cfg.json" ]; then echo THEME_CFG_JSON=present; else echo THEME_CFG_JSON=absent; fi

st services
$B chroot "$R" /system/bin/param set marble.debug.start.theme true >/dev/null 2>&1 || true
$B chroot "$R" /system/bin/param set marble.debug.start.wallpaper true >/dev/null 2>&1 || true
wait_sa() {
 i=0
 while [ "$i" -lt "$2" ]; do
  if $B timeout 4 $B chroot "$R" /compat/bin/native_sa_check "$1" >/dev/null 2>&1; then
   echo "THEME_SA_$1=ready"
   return 0
  fi
  i=$((i + 1))
  $B sleep 1
 done
 echo "THEME_SA_$1=missing"
}
wait_sa 65846 20
wait_sa 3705 8

st logs
if [ -f "$R/data/local/tmp/marble-p4-hilog.txt" ]; then
 $B tail -c 2000000 "$R/data/local/tmp/marble-p4-hilog.txt" 2>/dev/null \
  | $B grep -i -E "theme|wallpaper" | $B tail -n 200 > "$L" 2>/dev/null || true
else
 $B timeout 10 $B chroot "$R" /system/bin/hilog -x 2>/dev/null \
  | $B grep -i -E "theme|wallpaper" | $B tail -n 200 > "$L" 2>/dev/null || true
fi
echo "THEME_LOG_BYTES=$($B stat -c %s "$L" 2>/dev/null || echo 0)"
st complete
