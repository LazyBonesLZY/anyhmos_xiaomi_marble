#!/bin/sh
# SPDX-License-Identifier: MIT
# Stage Xiaomi marble Phase 3 HarmonyOS compatibility overlays in RAM (vendor read-only).
set -eu
B=/busybox
R=/hmos-root

$B mkdir -p "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc" \
 "$R/system/etc/shng" "$R/system/etc/hdfconfig" "$R/chip_prod/etc/hdfconfig"

# Install minimal empty-host HDF config so hdf_devmgr reaches ready without Huawei Kirin hosts.
if $B test -f /graphics/hdf_default_minimal.hcb; then
 $B cp /graphics/hdf_default_minimal.hcb "$R/system/etc/hdfconfig/hdf_default.hcb"
 $B cp /graphics/hdf_default_minimal.hcb "$R/chip_prod/etc/hdfconfig/hdf_default.hcb"
 $B chmod 0644 "$R/system/etc/hdfconfig/hdf_default.hcb" "$R/chip_prod/etc/hdfconfig/hdf_default.hcb"
 echo MARBLE_PHASE3_MINIMAL_HCB_STAGED
fi

# Prepend /compat/lib64 and /compat/bin to musl loader search configs in the RAM overlay.
path_cfg="$R/system/etc/ld-musl-aarch64.path"
if $B test -f "$path_cfg" && ! $B grep -q '^/compat/lib64:' "$path_cfg"; then
 cur=$($B cat "$path_cfg")
 printf '/compat/lib64:%s\n' "$cur" > "$path_cfg.new"
 $B chmod 0644 "$path_cfg.new"
 $B mv "$path_cfg.new" "$path_cfg"
fi

for ini in \
 "$R/system/etc/ld-musl-namespace-aarch64.ini" \
 "$R/system/etc/ld-musl-namespace-aarch64-New.ini" \
 "$R/system/etc/ld-musl-namespace-aarch64-flex.ini" \
 "$R/system/etc/ld-musl-namespace-aarch64-test.ini"; do
 if $B test -f "$ini"; then
  $B sed \
   -e '/systemscence[[:space:]]*=[[:space:]]*\/compat\/bin:/! s|^\([[:space:]]*systemscence[[:space:]]*=[[:space:]]*\)|\1/compat/bin:|' \
   -e '/\.lib\.paths[[:space:]]*=[[:space:]]*\/compat\/lib64:/! s|^\([[:space:]]*namespace\.[^=]*\.lib\.paths[[:space:]]*=[[:space:]]*\)|\1/compat/lib64:|' \
   "$ini" > "$ini.new"
  $B chmod 0644 "$ini.new"
  $B mv "$ini.new" "$ini"
 fi
done
echo MARBLE_PHASE3_MUSL_PATHS_STAGED

# Expose ashmem and hmos_dmabuf_sync inside /hmos-root/dev if present in kernel.
if $B test -f /sys/class/misc/ashmem/dev; then
 nums=$($B cat /sys/class/misc/ashmem/dev)
 $B test -e "$R/dev/ashmem" || $B mknod "$R/dev/ashmem" c "${nums%:*}" "${nums#*:}"
 $B chown 0:0 "$R/dev/ashmem"
 $B chmod 0666 "$R/dev/ashmem"
fi
if $B test -f /sys/class/misc/hmos_dmabuf_sync/dev; then
 nums=$($B cat /sys/class/misc/hmos_dmabuf_sync/dev)
 $B test -e "$R/dev/hmos_dmabuf_sync" || $B mknod "$R/dev/hmos_dmabuf_sync" c "${nums%:*}" "${nums#*:}"
 $B chown 0:0 "$R/dev/hmos_dmabuf_sync"
 $B chmod 0660 "$R/dev/hmos_dmabuf_sync"
fi

# Apply guarded donor binary patches (SHA-256 verified input & output) in RAM.
if $B test -f /graphics/apply-donor-patches-phase3.sh; then
 $B sh /graphics/apply-donor-patches-phase3.sh
else
 echo "MARBLE_PHASE3_ERROR: missing /graphics/apply-donor-patches-phase3.sh" >&2
 exit 70
fi

$B test -x "$R/compat/bin/init"
echo MARBLE_PHASE3_RAM_COMPAT_READY
