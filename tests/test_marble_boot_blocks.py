"""Guard against silently losing a boot block or image payload entry.

Twice during this port a large edit of `start-phase4-marble.sh` / `stage-phase4-marble.sh`
silently dropped a whole block (the /vendor/etc overlay, then the Bluetooth and
USB/HDC stages) and nothing failed until a device run.  These assertions pin the
markers that must exist in the scripts *and* in a freshly built image.
"""

import glob
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / 'devices/marble/runtime/boot/stage-phase4-marble.sh'
START = ROOT / 'devices/marble/runtime/boot/start-phase4-marble.sh'

STAGE_MARKERS = (
    'MARBLE_VENDOR_ETC_OVERLAY',
    'acdbdata/Forte',
    'MARBLE_DSP_FS_MOUNTED',
    'MARBLE_AUDIO_DEVS',
    'MARBLE_GPR_NODE',
    'MARBLE_CHROOT_BUSYBOX',
    'MARBLE_HISYSEVENT_SINK_STARTED',
    'MARBLE_BRIGHTNESS_WATCH_STARTED',
    'MARBLE_BT_FIRMWARE_MOUNTED',
    'MARBLE_AUDIO_RUNTIME_STAGED',
)

START_MARKERS = (
    'AGMIPC_STATUS',
    'PAL_SERVER_STATUS',
    'HISYSEVENT_SINK=',
    'BRIGHTNESS_APPLY',
    'BRIGHTNESS_APPLY2',
    'BLUETOOTH_STATUS',
    'USB_START=',
    'HDC_ENDPOINT_READY=',
    'marble.debug.start.extra',
    'marble.debug.start.storage',
    'marble.debug.start.location',
    'marble.debug.start.theme',
)


class ScriptBlockTests(unittest.TestCase):
    def test_stage_keeps_every_block(self):
        text = STAGE.read_text()
        missing = [m for m in STAGE_MARKERS if m not in text]
        self.assertEqual(missing, [], f'stage script lost blocks: {missing}')

    def test_start_keeps_every_block(self):
        text = START.read_text()
        missing = [m for m in START_MARKERS if m not in text]
        self.assertEqual(missing, [], f'start script lost blocks: {missing}')

    def test_scripts_parse(self):
        for script in (STAGE, START):
            subprocess.run(['sh', '-n', str(script)], check=True)

    def test_optional_stages_stay_opt_in(self):
        text = START.read_text()
        # These touch the single USB port / BT firmware, so they must remain
        # behind marker files rather than running on every boot.
        self.assertIn('/data/local/tmp/usb-autostart', text)
        self.assertIn('/data/local/tmp/bt-autostart', text)
        self.assertNotIn('pset marble.debug.start.extra2', text)
        self.assertNotIn('pset marble.debug.start.extra3', text)


class UsbOrchestratorTests(unittest.TestCase):
    def run_usb_block(self, marker, *, skip=False, outcome='ready', helper=True):
        busybox = shutil.which('busybox')
        if not busybox:
            self.skipTest('BusyBox needed for the host fixture')
        text = START.read_text()
        block = text[text.index('# 6f. USB-only HDC'):text.index('# 7. Foundation')]
        with tempfile.TemporaryDirectory(prefix='marble-usb-start-') as tmp:
            root = Path(tmp)
            request = root / 'data/local/tmp/usb-autostart'
            request.parent.mkdir(parents=True)
            (root / 'dev').mkdir()
            exec_path = root / 'compat/bin/start-usb-marble.sh'
            exec_path.parent.mkdir(parents=True)
            if marker == 'file':
                request.touch()
            elif marker == 'symlink':
                unrelated = root / 'unrelated'
                unrelated.touch()
                request.symlink_to(unrelated)
            if helper:
                exec_path.write_text('''#!/bin/sh
printf 'HELPER_MODE=%s\\n' "$MODE"
case "$OUTCOME" in
  ready) echo 'USB_FFS_READY=1 hdcd=running'; echo 'USB_UDC_BOUND fixture' ;;
  stale) echo 'USB_FFS_READY=1 hdcd=running' ;;
  failed) echo 'USB_FFS_READY=1 hdcd=running'; echo 'USB_UDC_BOUND fixture'; exit 49 ;;
esac
''')
                exec_path.chmod(0o755)
            driver = '''set -eu
B=$1; R=$2; skip=$3; OUTCOME=$4; export OUTCOME
skipped() { [ "$1" = usb ] && [ "$skip" = 1 ]; }
log() { printf '%s\\n' "$*"; }
'''
            proc = subprocess.run(['sh', '-c', driver + block, 'fixture', busybox,
                                   tmp, str(int(skip)), outcome],
                                  capture_output=True, text=True, timeout=5)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            traces = [p.read_text() for p in (root / 'dev').glob('marble-usb.*')]
            return proc.stdout, traces

    def test_marker_and_skip_control_usb(self):
        for marker, kwargs, reason in (
            ('missing', {}, 'not-requested'), ('symlink', {}, 'not-requested'),
            ('file', {'skip': True}, 'requested'), ('file', {'helper': False}, 'helper-missing'),
        ):
            with self.subTest(marker=marker, **kwargs):
                out, traces = self.run_usb_block(marker, **kwargs)
                self.assertIn(f'USB_SKIP={reason}', out)
                self.assertFalse(traces)

    def test_endpoint_marker_requires_success_and_bound_udc(self):
        for outcome, expected in (('ready', True), ('stale', False), ('failed', False)):
            with self.subTest(outcome=outcome):
                out, traces = self.run_usb_block('file', outcome=outcome)
                self.assertEqual(len(traces), 1)
                self.assertIn('HELPER_MODE=hdc', traces[0])
                self.assertIn('host_untested=1', out)
                self.assertEqual('HDC_ENDPOINT_READY=1' in out, expected)

    def test_wrapper_keeps_native_service_identity_and_policy(self):
        text = START.read_text()
        block = text[text.index('# 6f. USB-only HDC'):text.index('# 7. Foundation')]
        self.assertNotIn('chroot', block)
        self.assertNotIn('/system/bin/hdcd', block)
        self.assertNotIn('const.security.developermode.state', block)
        self.assertNotIn('persist.hdc.', block)


class BuiltImageTests(unittest.TestCase):
    """Only runs when a built image is present in the workspace."""

    def _entries(self):
        images = sorted(glob.glob(str(ROOT / 'work/marble-phase4-verify.*/images/vendor_boot_phase4.img')))
        if not images:
            self.skipTest('no built vendor_boot image in work/')
        sys.path.insert(0, str(ROOT / 'tools/packaging'))
        from build_marble_phase2_images import (  # noqa: PLC0415
            decompress_ramdisk,
            extract_full_single_vendor_ramdisk,
            parse_cpio_newc,
        )
        out = ROOT / 'work/marble-img-inspect-test'
        out.mkdir(exist_ok=True)
        rd, cpio = out / 'rd.gz', out / 'rd.cpio'
        extract_full_single_vendor_ramdisk(Path(images[-1]), rd)
        decompress_ramdisk(rd, cpio)
        return parse_cpio_newc(cpio.read_bytes())

    def test_image_carries_the_expected_artifacts(self):
        files = self._entries()
        names = {n.rsplit('/', 1)[-1] for n in files}
        for want in ('marble_verify_all.sh', 'hmos_brightness_persist', 'hmos_hisysevent_sink',
                     'foundation-app-core.json', 'init-platform-marble.cfg', 'stage-phase4-marble.sh',
                     'start-phase4-marble.sh', 'libhmos_agmipc_shim.so', 'libaudioroute.so'):
            self.assertIn(want, names, f'{want} missing from the built image')

    def test_image_foundation_profile_publishes_ans(self):
        files = self._entries()
        raw = next(v[1] for k, v in files.items() if k.endswith('foundation-app-core.json'))
        sas = {str(s['name']) for s in json.loads(raw.decode())['systemability']}
        self.assertIn('3203', sas)
        self.assertIn('3204', sas)

    def test_image_platform_cfg_has_storage_and_installer(self):
        files = self._entries()
        raw = next(v[1] for k, v in files.items() if k.endswith('init-platform-marble.cfg'))
        services = {s['name'] for s in json.loads(raw.decode())['services']}
        for name in ('disk_manager', 'StorageSpaceMgr', 'device_storage_manager_service', 'sys_installer_sa'):
            self.assertIn(name, services)


class OrderingTests(unittest.TestCase):
    """The order of these steps is load-bearing; swapping two of them silently
    breaks audio, brightness or the DSP module registry."""

    def test_dsp_partition_is_mounted_before_the_adsp_boots(self):
        text = STAGE.read_text()
        self.assertLess(text.index('MARBLE_DSP_FS_MOUNTED'), text.index('boot_adsp/boot'),
                        'dsp_a must be visible before ADSP starts')

    def test_adsp_boot_is_requested_only_once(self):
        text = STAGE.read_text()
        self.assertEqual(text.count('echo 1 > /sys/kernel/boot_adsp/boot 2>&1'), 1)

    def test_brightness_is_applied_after_the_display_power_manager(self):
        text = START.read_text()
        self.assertLess(text.index('marble.debug.display.wake'), text.index('BRIGHTNESS_APPLY'))

    def test_hisysevent_sink_is_supervised_before_audio(self):
        text = START.read_text()
        self.assertLess(text.index('HISYSEVENT_SINK='), text.index('stage=audio_pal'))

    def test_agmipc_starts_before_pal(self):
        text = START.read_text()
        self.assertLess(text.index('AGMIPC_STATUS'), text.index('PAL_SERVER_STATUS'))

    def test_acdb_override_is_created_after_the_vendor_overlay(self):
        text = STAGE.read_text()
        overlay = text.index('mount -t overlay overlay -o "lowerdir=$R/vendor/etc')
        forte = text.index('acdbdata/Forte')
        self.assertLess(overlay, forte, 'Forte/ must be created inside the overlay')


class ChrootLaunchTests(unittest.TestCase):
    """musl PIEs must be started inside the chroot: launching them by host path
    fails with rc=127 because the interpreter is not at the host root, and that
    is how the hisysevent sink silently stopped running."""

    def test_sink_and_brightness_are_launched_through_chroot(self):
        stage, start = STAGE.read_text(), START.read_text()
        for script in (stage, start):
            self.assertIn('chroot "$R" /compat/bin/hmos_hisysevent_sink -q', script)
        self.assertIn('chroot "$R" /compat/bin/hmos_brightness_persist watch', stage)
        for marker in ('BRIGHTNESS_APPLY=', 'BRIGHTNESS_APPLY2='):
            lines = [line for line in start.splitlines() if marker in line]
            self.assertEqual(len(lines), 1, marker)
            self.assertIn('chroot "$R" /compat/bin/hmos_brightness_persist apply', lines[0])

    def test_no_host_path_launch_of_the_sink(self):
        for path in (STAGE, START):
            text = path.read_text()
            self.assertNotIn('"$R/compat/bin/hmos_hisysevent_sink" -q', text,
                             f'{path.name} still launches the sink by host path')


class ImageBinaryFreshnessTests(unittest.TestCase):
    """The image must carry the binaries we actually rebuilt.

    During this port a stale `hmos_bt_bridge_marble` (pre-retry build) was
    shipped for several images while the source had already changed, and the
    only symptom was a device log that looked like the patch "did nothing".
    """

    def _image_entry(self, name):
        import glob as _glob
        import hashlib
        images = sorted(_glob.glob(str(ROOT / 'work/marble-phase4-sinkfix.*/images/vendor_boot_phase4.img')))
        if not images:
            self.skipTest('no built vendor_boot image in work/')
        sys.path.insert(0, str(ROOT / 'tools/packaging'))
        from build_marble_phase2_images import (  # noqa: PLC0415
            decompress_ramdisk,
            extract_full_single_vendor_ramdisk,
            parse_cpio_newc,
        )
        out = ROOT / 'work/marble-img-fresh'
        out.mkdir(exist_ok=True)
        extract_full_single_vendor_ramdisk(Path(images[-1]), out / 'rd.gz')
        decompress_ramdisk(out / 'rd.gz', out / 'rd.cpio')
        files = parse_cpio_newc((out / 'rd.cpio').read_bytes())
        for key, val in files.items():
            if key.rsplit('/', 1)[-1] == name:
                return hashlib.sha256(val[1]).hexdigest()
        return None

    def test_bluetooth_bridge_in_image_matches_the_built_binary(self):
        import hashlib
        local = ROOT / 'work/marble-bluetooth-build/hmos_bt_bridge_marble'
        if not local.is_file():
            self.skipTest('bridge build artifact missing')
        want = hashlib.sha256(local.read_bytes()).hexdigest()
        self.assertEqual(self._image_entry('hmos_bt_bridge_marble'), want,
                         'image ships a stale hmos_bt_bridge_marble')

    def test_hisysevent_sink_in_image_matches_the_built_binary(self):
        import hashlib
        local = ROOT / 'work/marble-phase4-connection.bl5xLy/build/hmos_hisysevent_sink'
        if not local.is_file():
            self.skipTest('sink build artifact missing')
        want = hashlib.sha256(local.read_bytes()).hexdigest()
        self.assertEqual(self._image_entry('hmos_hisysevent_sink'), want)


if __name__ == '__main__':
    unittest.main()
