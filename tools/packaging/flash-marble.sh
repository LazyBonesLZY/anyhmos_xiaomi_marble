#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./flash-marble.sh [verify|inspect|upgrade|full] [options]

Modes (default: verify):
  verify    Offline check of PACKAGE.json file sizes and SHA-256 checksums.
  inspect   Verify package + read-only fastboot preflight checks on Xiaomi marble.
  upgrade   Verify + preflight + flash boot_a & vendor_boot_a + set_active a + reboot
            (preserves super, userdata, and all stock firmware).
  full      Verify + preflight + flash vbmeta_a/vbmeta_system_a/boot_a/vendor_boot_a/super
            + set_active a + reboot (requires a full package).

Options:
  --serial <id>             Target Fastboot device serial.
  --format-userdata-ext4    (full mode only) Also erase metadata and format userdata as ext4.
  --fastboot <path>         Override fastboot binary path.
  -h, --help                Show this help message.
EOF
}

MODE="verify"
SERIAL=""
FORMAT_USERDATA=0
FASTBOOT_BIN=""

if [[ $# -gt 0 ]]; then
    case "$1" in
        verify|inspect|upgrade|full)
            MODE="$1"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            ;;
        *)
            echo "ERROR: unknown mode '$1'" >&2
            usage >&2
            exit 2
            ;;
    esac
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --serial)
            [[ $# -ge 2 ]] || { echo "ERROR: --serial requires a value" >&2; exit 2; }
            SERIAL="$2"
            shift 2
            ;;
        --format-userdata-ext4)
            FORMAT_USERDATA=1
            shift
            ;;
        --fastboot)
            [[ $# -ge 2 ]] || { echo "ERROR: --fastboot requires a value" >&2; exit 2; }
            FASTBOOT_BIN="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown option '$1'" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$FORMAT_USERDATA" -eq 1 && "$MODE" != "full" ]]; then
    echo "ERROR: --format-userdata-ext4 is only valid with 'full' mode." >&2
    exit 2
fi

if [[ -n "$SERIAL" && ! "$SERIAL" =~ ^[a-zA-Z0-9_-]+$ ]]; then
    echo "ERROR: invalid serial format '$SERIAL'." >&2
    exit 2
fi

PKG_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="$PKG_ROOT/PACKAGE.json"

if [[ ! -f "$MANIFEST" ]]; then
    echo "ERROR: PACKAGE.json is missing in $PKG_ROOT" >&2
    exit 1
fi

command -v python3 >/dev/null 2>&1 || {
    echo "ERROR: python3 is required to verify PACKAGE.json." >&2
    exit 1
}

# Offline manifest, size, and SHA-256 verification
META_OUT="$(python3 - "$PKG_ROOT" "$MODE" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

pkg_root = Path(sys.argv[1]).resolve()
mode = sys.argv[2]
manifest = json.loads((pkg_root / "PACKAGE.json").read_text(encoding="utf-8"))

if (
    manifest.get("kind") != "marble-anyhmos-fastboot"
    or manifest.get("device") != "marble"
    or manifest.get("target_slot") != "a"
):
    raise SystemExit("ERROR: Unexpected package identity in PACKAGE.json.")

pkg_type = manifest.get("package_type")
if pkg_type not in ("upgrade", "full"):
    raise SystemExit(f"ERROR: Unsupported package_type: {pkg_type}")

if not isinstance(manifest.get("device_boot_tested"), bool):
    raise SystemExit("ERROR: Invalid device_boot_tested flag in PACKAGE.json.")

if mode == "full" and pkg_type != "full":
    raise SystemExit(
        "ERROR: This is an upgrade-only package (boot + vendor_boot). "
        "Use 'upgrade' mode or use the full package for 'full' mode."
    )

files = manifest.get("files")
if not isinstance(files, list) or not files:
    raise SystemExit("ERROR: PACKAGE.json has empty files list.")

required = ["images/boot.img", "images/vendor_boot.img"]
if pkg_type == "full":
    required.extend(["images/vbmeta.img", "images/vbmeta_system.img", "images/super.img"])

by_path = {}
for rec in files:
    rel = rec.get("path", "")
    if not rel or rel.startswith(("/", "\\")) or ":" in rel or ".." in Path(rel).parts:
        raise SystemExit(f"ERROR: Invalid manifest path: {rel}")
    if rel in by_path:
        raise SystemExit(f"ERROR: Duplicate manifest entry: {rel}")
    by_path[rel] = rec

for req in required:
    if req not in by_path:
        raise SystemExit(f"ERROR: Missing required artifact in manifest: {req}")

print("============================================================", file=sys.stderr)
print(f"AnyHMOS Xiaomi marble Fastboot Tool ({manifest.get('batch')} / {pkg_type})", file=sys.stderr)
print(f"Donor:  {manifest.get('donor')}", file=sys.stderr)
print(f"Kernel: {manifest.get('kernel_release')}", file=sys.stderr)
print("============================================================", file=sys.stderr)
print("Verifying package file sizes and SHA-256 checksums...", file=sys.stderr)

for rec in files:
    rel = rec["path"]
    fpath = (pkg_root / rel).resolve()
    try:
        fpath.relative_to(pkg_root)
    except ValueError:
        raise SystemExit(f"ERROR: Path escapes package root: {rel}")
    if not fpath.is_file():
        raise SystemExit(f"ERROR: Missing file: {rel}")
    actual_size = fpath.stat().st_size
    if actual_size != int(rec["bytes"]):
        raise SystemExit(f"ERROR: Size mismatch for {rel}: expected {rec['bytes']}, got {actual_size}")
    h = hashlib.sha256()
    with fpath.open("rb") as fh:
        while True:
            chunk = fh.read(4 * 1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    digest = h.hexdigest()
    if digest != str(rec["sha256"]).lower():
        raise SystemExit(f"ERROR: SHA-256 mismatch for {rel}")
    print(f"  [OK] {rel} ({actual_size} bytes)", file=sys.stderr)

if manifest["device_boot_tested"]:
    print(f"Device boot validation: PASSED (boot_id: {manifest.get('verified_boot_id')})", file=sys.stderr)
else:
    print("Device boot validation: NOT marked tested in this package.", file=sys.stderr)

print(
    "\t".join(
        [
            pkg_type,
            str(int(manifest.get("super_bytes") or 0)),
            str(int(by_path["images/boot.img"]["bytes"])),
            str(int(by_path["images/vendor_boot.img"]["bytes"])),
            str(int(by_path["images/vbmeta.img"]["bytes"])) if "images/vbmeta.img" in by_path else "0",
            str(int(by_path["images/vbmeta_system.img"]["bytes"])) if "images/vbmeta_system.img" in by_path else "0",
        ]
    )
)
PY
)"

IFS=$'\t' read -r PKG_TYPE SUPER_BYTES BOOT_BYTES VENDOR_BOOT_BYTES VBMETA_BYTES VBMETA_SYS_BYTES <<<"$META_OUT"

if [[ "$MODE" == "verify" ]]; then
    echo "Offline verification passed. No device command was sent."
    exit 0
fi

if [[ -z "$FASTBOOT_BIN" ]]; then
    if command -v fastboot >/dev/null 2>&1; then
        FASTBOOT_BIN="$(command -v fastboot)"
    elif [[ -x "$PKG_ROOT/bin/fastboot" ]]; then
        FASTBOOT_BIN="$PKG_ROOT/bin/fastboot"
    else
        echo "ERROR: fastboot not found in PATH. Install Android platform-tools or pass --fastboot." >&2
        exit 1
    fi
fi

LOG_DIR="$PKG_ROOT/logs"
mkdir -p "$LOG_DIR"
LOG_PATH="$LOG_DIR/fastboot-$(date +%Y%m%d-%H%M%S).txt"

run_fb() {
    local out rc
    set +e
    out="$("$FASTBOOT_BIN" "$@" 2>&1 < /dev/null)"
    rc=$?
    set -e
    {
        printf 'fastboot'
        printf ' %q' "$@"
        printf '\n%s\n\n' "$out"
    } >>"$LOG_PATH"
    if [[ "$rc" -ne 0 ]]; then
        echo "ERROR: fastboot failed (exit $rc):" >&2
        echo "$out" >&2
        exit "$rc"
    fi
    printf '%s\n' "$out"
}

DEVICES_OUT="$(run_fb devices)"
FOUND_SERIALS=()
while IFS= read -r line; do
    if [[ "$line" =~ ^([a-zA-Z0-9_-]+)[[:space:]]+fastboot[[:space:]]*$ ]]; then
        FOUND_SERIALS+=("${BASH_REMATCH[1]}")
    fi
done <<<"$DEVICES_OUT"

if [[ -n "$SERIAL" ]]; then
    MATCHED=0
    for s in "${FOUND_SERIALS[@]}"; do
        if [[ "$s" == "$SERIAL" ]]; then
            MATCHED=1
            break
        fi
    done
    if [[ "$MATCHED" -ne 1 ]]; then
        echo "ERROR: Requested Fastboot serial '$SERIAL' is not connected." >&2
        exit 1
    fi
else
    if [[ "${#FOUND_SERIALS[@]}" -ne 1 ]]; then
        echo "ERROR: Connect exactly one Xiaomi marble in bootloader Fastboot mode (found ${#FOUND_SERIALS[@]}), or pass --serial." >&2
        exit 1
    fi
    SERIAL="${FOUND_SERIALS[0]}"
fi

getvar() {
    local name="$1"
    local out val
    out="$(run_fb -s "$SERIAL" getvar "$name")"
    val="$(printf '%s\n' "$out" | sed -nE "s/^(\(bootloader\)[[:space:]]*)?${name}:[[:space:]]*([^[:space:]]+)[[:space:]]*$/\2/p" | head -n 1)"
    if [[ -z "$val" ]]; then
        echo "ERROR: Cannot read Fastboot variable: $name" >&2
        exit 1
    fi
    printf '%s\n' "$val"
}

hex_to_dec() {
    local raw="$1"
    if [[ ! "$raw" =~ ^0x[0-9a-fA-F]+$ ]]; then
        echo "ERROR: Invalid hex partition size: $raw" >&2
        exit 1
    fi
    printf '%d\n' "$((raw))"
}

PRODUCT="$(getvar product)"
if [[ "$PRODUCT" != "marble" ]]; then
    echo "ERROR: Unsupported device product '$PRODUCT'. Only Xiaomi marble (Redmi Note 12 Turbo / POCO F5) is supported." >&2
    exit 1
fi
if [[ "$(getvar unlocked)" != "yes" ]]; then
    echo "ERROR: Bootloader must already be unlocked." >&2
    exit 1
fi
if [[ "$(getvar is-userspace)" != "no" ]]; then
    echo "ERROR: Device is in fastbootd. Reboot into bootloader Fastboot (Volume Down + Power) first." >&2
    exit 1
fi
CURRENT_SLOT="$(getvar current-slot)"
if [[ "$CURRENT_SLOT" != "a" && "$CURRENT_SLOT" != "b" ]]; then
    echo "ERROR: Unexpected current-slot value: $CURRENT_SLOT" >&2
    exit 1
fi

BOOT_CAP="$(hex_to_dec "$(getvar partition-size:boot_a)")"
VB_CAP="$(hex_to_dec "$(getvar partition-size:vendor_boot_a)")"
(( BOOT_CAP >= BOOT_BYTES )) || { echo "ERROR: boot_a too small ($BOOT_CAP < $BOOT_BYTES)" >&2; exit 1; }
(( VB_CAP >= VENDOR_BOOT_BYTES )) || { echo "ERROR: vendor_boot_a too small ($VB_CAP < $VENDOR_BOOT_BYTES)" >&2; exit 1; }

if [[ "$PKG_TYPE" == "full" ]]; then
    SUPER_CAP="$(hex_to_dec "$(getvar partition-size:super)")"
    if [[ "$SUPER_CAP" -ne "$SUPER_BYTES" ]]; then
        echo "ERROR: Super partition size mismatch: device has $SUPER_CAP bytes, expected $SUPER_BYTES bytes." >&2
        exit 1
    fi
    VBM_CAP="$(hex_to_dec "$(getvar partition-size:vbmeta_a)")"
    VBMS_CAP="$(hex_to_dec "$(getvar partition-size:vbmeta_system_a)")"
    (( VBM_CAP >= VBMETA_BYTES )) || { echo "ERROR: vbmeta_a too small ($VBM_CAP < $VBMETA_BYTES)" >&2; exit 1; }
    (( VBMS_CAP >= VBMETA_SYS_BYTES )) || { echo "ERROR: vbmeta_system_a too small ($VBMS_CAP < $VBMETA_SYS_BYTES)" >&2; exit 1; }
fi

echo "Read-only preflight passed for Xiaomi marble ($SERIAL, current-slot=$CURRENT_SLOT). Target slot: a."
if [[ "$MODE" == "inspect" ]]; then
    echo "No partition was changed. Log: $LOG_PATH"
    exit 0
fi

IMG_DIR="$PKG_ROOT/images"

if [[ "$MODE" == "upgrade" ]]; then
    cat <<'EOF'

UPGRADE MODE (preserves super, userdata, and all stock firmware):
  - Will flash: boot_a, vendor_boot_a
  - Will run:   set_active a, reboot
  - Untouched:  super, userdata, vbmeta*, abl, xbl, modem*, dsp*, bluetooth*, persist, recovery*, dtbo*

EOF
    read -r -p "Type UPGRADE MARBLE to continue: " CONFIRM
    if [[ "$CONFIRM" != "UPGRADE MARBLE" ]]; then
        echo "Confirmation did not match. No partition was written." >&2
        exit 1
    fi
    echo "Flashing boot_a..."
    run_fb -s "$SERIAL" flash boot_a "$IMG_DIR/boot.img"
    echo "Flashing vendor_boot_a..."
    run_fb -s "$SERIAL" flash vendor_boot_a "$IMG_DIR/vendor_boot.img"
    echo "Setting active slot to a..."
    run_fb -s "$SERIAL" set_active a
    echo "Upgrade succeeded; rebooting device..."
    run_fb -s "$SERIAL" reboot
    echo "Flash log: $LOG_PATH"
    exit 0
fi

cat <<'EOF'

FULL INSTALL MODE (first-time AnyHMOS installation on Xiaomi marble):
  - Will flash: vbmeta_a (--disable-verity --disable-verification)
  - Will flash: vbmeta_system_a (--disable-verity --disable-verification)
  - Will flash: boot_a, vendor_boot_a, super (9 GiB sparse; takes several minutes)
  - Will run:   set_active a, reboot
  - Untouched:  abl, xbl, modem*, dsp*, bluetooth*, persist, recovery*, dtbo*
EOF

if [[ "$FORMAT_USERDATA" -eq 1 ]]; then
    echo "  - WARNING:    --format-userdata-ext4 ENABLED -> WILL ERASE metadata AND FORMAT userdata AS EXT4 (DELETES ALL USER DATA)!"
else
    echo "  - Userdata:   NOT touched by this script. If migrating from stock encrypted Android/MIUI/HyperOS,"
    echo "                you MUST format userdata as ext4 (in Recovery, or re-run with --format-userdata-ext4) before first boot."
fi
echo ""

read -r -p "Type FLASH MARBLE FULL to write system partitions: " CONFIRM_FULL
if [[ "$CONFIRM_FULL" != "FLASH MARBLE FULL" ]]; then
    echo "Confirmation did not match. No partition was written." >&2
    exit 1
fi

if [[ "$FORMAT_USERDATA" -eq 1 ]]; then
    read -r -p "Type FORMAT MARBLE USERDATA EXT4 to confirm DELETING ALL USER DATA: " CONFIRM_WIPE
    if [[ "$CONFIRM_WIPE" != "FORMAT MARBLE USERDATA EXT4" ]]; then
        echo "Userdata format confirmation did not match. No partition was written." >&2
        exit 1
    fi
fi

echo "Flashing vbmeta_a (verity & verification disabled)..."
run_fb -s "$SERIAL" flash --disable-verity --disable-verification vbmeta_a "$IMG_DIR/vbmeta.img"
echo "Flashing vbmeta_system_a (verity & verification disabled)..."
run_fb -s "$SERIAL" flash --disable-verity --disable-verification vbmeta_system_a "$IMG_DIR/vbmeta_system.img"
echo "Flashing boot_a..."
run_fb -s "$SERIAL" flash boot_a "$IMG_DIR/boot.img"
echo "Flashing vendor_boot_a..."
run_fb -s "$SERIAL" flash vendor_boot_a "$IMG_DIR/vendor_boot.img"
echo "Flashing super (7+ GiB sparse image; please keep USB connected and wait)..."
run_fb -s "$SERIAL" flash super "$IMG_DIR/super.img"

if [[ "$FORMAT_USERDATA" -eq 1 ]]; then
    echo "Erasing Android encryption metadata..."
    run_fb -s "$SERIAL" erase metadata
    echo "Formatting userdata as ext4..."
    run_fb -s "$SERIAL" format:ext4 userdata
fi

echo "Setting active slot to a..."
run_fb -s "$SERIAL" set_active a
echo "All writes succeeded; rebooting device..."
run_fb -s "$SERIAL" reboot
echo "Flash log: $LOG_PATH"
