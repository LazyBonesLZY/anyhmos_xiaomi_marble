#!/busybox sh
# SPDX-License-Identifier: MIT
# Run as root outside the chroot, after /data/service/el1/public exists and
# before starting wifi_manager_service/wpa_host. WIFI_PAYLOAD points to the
# parent of etc/{wpa,p2p}_supplicant.conf, not to a replacement library.
# This seeds absent files only. It does not start or validate WLAN, and the
# live SELinux labels and access in the service domains are separate checks.
set -eu

B=/busybox
R=${MARBLE_ROOT:-/hmos-root}
PAYLOAD=${WIFI_PAYLOAD:-/compat/wifi}
PUBLIC=$R/data/service/el1/public
WIFI=$PUBLIC/wifi
CONF_DIR=$WIFI/wpa_supplicant
SOCKS=$WIFI/sockets
SOCK_DIR=$SOCKS/wpa
VENDOR_LIB=$R/vendor/lib64/libwpa.z.so
# Verified donor ADY-AL10 207.0.0.111 SP10: wpa_main@@1.0 and ap_main@@1.0.
# This is a byte-identity guard, not a check of what a process has mapped.
DONOR_WPA_SHA256=89289dfa1200e4c44fdc41e7e88a650ca0c18a85b4c23326b06066bd483653b8
# Donor /system/etc/{passwd,group}: wifi UID 1010, wifi_group GID 1011.
OWNER=1010:1011
CTRL_CONFIG='ctrl_interface=/data/service/el1/public/wifi/sockets/wpa'
stage=precheck
scratch=
clean_seed() {
    "$B" rm -f -- "$scratch/seed" && "$B" rmdir "$scratch"
}
finish() {
    code=$?
    if [ -n "$scratch" ] && ! clean_seed; then
        code=32
        echo 'WIFI_STAGE=config FAIL could not remove private staging directory' >&2
    fi
    echo "WIFI_STAGE_EXIT=$code STAGE=$stage"
    exit "$code"
}
trap finish EXIT
trap 'exit 32' HUP INT TERM

fail() {
    rc=$1
    shift
    printf 'WIFI_STAGE=%s FAIL %s\n' "$stage" "$*" >&2
    exit "$rc"
}

canonical_dir() {
    case "$1" in /*) ;; *) return 1 ;; esac
    [ -d "$1" ] && [ "$("$B" readlink -f -- "$1")" = "$1" ]
}

if [ "$R" = / ] || ! canonical_dir "$R"; then
    fail 21 'MARBLE_ROOT must be an existing canonical directory other than /'
fi
[ "$("$B" id -u)" = 0 ] || fail 21 'run as root outside the chroot'
canonical_dir "$PUBLIC" || fail 21 'expected init-created /data/service/el1/public on mounted userdata'
canonical_dir "$PAYLOAD/etc" || fail 22 'expected canonical WIFI_PAYLOAD/etc directory'
for name in wpa_supplicant.conf p2p_supplicant.conf; do
    seed=$PAYLOAD/etc/$name
    if [ -L "$seed" ] || [ ! -f "$seed" ] ||
        ! printf '%s\n' "$CTRL_CONFIG" | "$B" cmp -s "$seed" -; then
        fail 22 "missing or unexpected blank seed: $name"
    fi
done
if ! canonical_dir "$R/vendor/lib64" || [ -L "$VENDOR_LIB" ] || [ ! -f "$VENDOR_LIB" ]; then
    fail 23 'required donor /vendor/lib64/libwpa.z.so is absent or not a direct file'
fi
actual=$("$B" sha256sum "$VENDOR_LIB") || fail 23 'could not hash existing donor libwpa'
[ "${actual%% *}" = "$DONOR_WPA_SHA256" ] || fail 23 'unexpected vendor libwpa digest; left unchanged'
echo 'WIFI_STAGE=precheck donor_lib=matched'

# Treat existing directories as inputs, including metadata. This stage may
# create them, but does not claim ownership of an unexpected or linked tree.
check_dir() {
    [ ! -L "$1" ] && [ -d "$1" ] || fail 31 "not a direct directory: $1"
    meta=$("$B" stat -c '%u:%g:%a' "$1") || fail 31 "cannot inspect directory: $1"
    [ "$meta" = "$OWNER:770" ] || fail 31 "unexpected owner/mode for $1: $meta (expected $OWNER:770)"
}

check_saved() {
    [ ! -L "$1" ] && [ -f "$1" ] || fail 31 "not a direct regular config: $1"
}

stage=config
# Check all pre-existing destinations before creating any of the missing ones.
for path in "$WIFI" "$CONF_DIR" "$SOCKS" "$SOCK_DIR"; do
    if [ -e "$path" ] || [ -L "$path" ]; then
        check_dir "$path"
    fi
done
for name in wpa_supplicant.conf p2p_supplicant.conf; do
    dst=$CONF_DIR/$name
    if [ -e "$dst" ] || [ -L "$dst" ]; then
        check_saved "$dst"
    fi
done
umask 077
for path in "$WIFI" "$CONF_DIR" "$SOCKS" "$SOCK_DIR"; do
    if "$B" mkdir -m 0700 "$path" 2>/dev/null; then
        if ! "$B" chown "$OWNER" "$path" || ! "$B" chmod 0770 "$path"; then
            "$B" rmdir "$path" 2>/dev/null || :
            fail 32 "failed to set new directory permissions: $path"
        fi
    fi
    check_dir "$path"
done

for name in wpa_supplicant.conf p2p_supplicant.conf; do
    dst=$CONF_DIR/$name
    if [ -e "$dst" ] || [ -L "$dst" ]; then
        check_saved "$dst"
        echo "WIFI_STAGE=config keep $name"
        continue
    fi
    scratch=$("$B" mktemp -d "$CONF_DIR/.marble-wpa.XXXXXX") || fail 32 "cannot create private staging directory for $name"
    # Write the allowlisted blank contents rather than re-reading mutable payload bytes.
    tmp=$scratch/seed
    printf '%s\n' "$CTRL_CONFIG" > "$tmp" || fail 32 "cannot write blank seed: $name"
    if ! "$B" chmod 0660 "$tmp" || ! "$B" chown "$OWNER" "$tmp"; then
        fail 32 "cannot set seed permissions: $name"
    fi
    # A same-filesystem link publishes a complete file without replacing any
    # name, even if another writer created it after the precheck. -T forbids
    # interpreting a racing destination directory as an output directory.
    if "$B" ln -T "$tmp" "$dst" 2>/dev/null; then
        echo "WIFI_STAGE=config seed $name"
    elif [ -e "$dst" ] || [ -L "$dst" ]; then
        check_saved "$dst"
        echo "WIFI_STAGE=config keep $name"
    else
        fail 32 "cannot publish blank seed: $name"
    fi
    clean_seed || fail 32 "cannot clean private staging directory: $name"
    scratch=
done

stage=complete
echo 'WIFI_STAGE=complete config_only=1 wlan_verified=0'
