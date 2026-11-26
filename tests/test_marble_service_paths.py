"""Every path our init configs reference must actually exist somewhere.

`sa_main /system/profile/x.json` that points at a missing library exits silently
(this is exactly how `disk_manager` looked "dead" for two rounds), so a typo or a
sheng-ism in a profile is worth catching at build time.

Two sources are checked:
  * the donor ROM extract for `/system` and `/vendor` paths;
  * the built vendor_boot payload for `/compat` paths.

Libraries that the donor extract lacks but the device resolves anyway are listed
in DEVICE_ONLY with the reason, so the test stays honest instead of quietly
skipping them.
"""

import glob
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs'
OUR_CFGS = sorted(glob.glob(str(ROOT / 'devices/marble/runtime/init/*.cfg')))
FOUNDATION = ROOT / 'devices/marble/runtime/init/foundation-marble.json'

# SA 4606 (window manager) is READY on the device (native_sa_check 4606) even
# though libwms.z.so is absent from both the donor extract and /system/lib64, so
# the library is merged into another unit on this ROM.  Verified on hardware.
DEVICE_ONLY_LIBS = {'libwms.z.so'}

# Verified on the device; kept here so the check documents the expectation.
RUNTIME_OUTPUTS = {'/data/local/tmp/hmos-phase4-capture/display0.jpeg'}

LIB_DIRS = ('.', 'platformsdk', 'chipset-sdk', 'chipset-sdk-sp', 'module')


def _donor_path(path: str) -> Path:
    if path.startswith('/system/'):
        return DONOR / 'system' / 'system' / path[len('/system/'):]
    if path.startswith('/vendor/'):
        return DONOR / 'vendor' / path[len('/vendor/'):]
    return DONOR / path.lstrip('/')


def _service_paths():
    for cfg_path in OUR_CFGS:
        try:
            cfg = json.loads(Path(cfg_path).read_text())
        except Exception:
            continue
        for svc in cfg.get('services') or []:
            for entry in svc.get('path') or []:
                if isinstance(entry, str) and entry.startswith('/'):
                    yield svc['name'], entry


class ServicePathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not DONOR.is_dir():
            raise unittest.SkipTest('donor ROM extract not present in work/')

    def test_system_and_vendor_paths_exist_in_the_donor_extract(self):
        missing = []
        for name, entry in _service_paths():
            if entry.startswith('/compat/') or entry in RUNTIME_OUTPUTS:
                continue
            if not _donor_path(entry).exists():
                missing.append((name, entry))
        self.assertEqual(missing, [], f'unresolvable service paths: {missing}')

    def test_foundation_libraries_are_available(self):
        profile = json.loads(FOUNDATION.read_text())
        missing = []
        for sa in profile['systemability']:
            lib = sa.get('libpath')
            if not lib or lib in DEVICE_ONLY_LIBS:
                continue
            found = any((DONOR / 'system' / 'system' / 'lib64' / d / lib).exists() for d in LIB_DIRS)
            if not found:
                missing.append((sa['name'], lib))
        self.assertEqual(missing, [], f'foundation libs missing: {missing}')

    def test_compat_paths_ship_in_the_built_image(self):
        images = sorted(glob.glob(str(ROOT / 'work/marble-phase4-parity.*/images/vendor_boot_phase4.img')))
        if not images:
            self.skipTest('no built vendor_boot image in work/')
        sys.path.insert(0, str(ROOT / 'tools/packaging'))
        from build_marble_phase2_images import (  # noqa: PLC0415
            decompress_ramdisk,
            extract_full_single_vendor_ramdisk,
            parse_cpio_newc,
        )
        out = ROOT / 'work/marble-img-pathcheck'
        out.mkdir(exist_ok=True)
        extract_full_single_vendor_ramdisk(Path(images[-1]), out / 'rd.gz')
        decompress_ramdisk(out / 'rd.gz', out / 'rd.cpio')
        names = {n.rsplit('/', 1)[-1] for n in parse_cpio_newc((out / 'rd.cpio').read_bytes())}
        missing = [entry for _, entry in _service_paths()
                   if entry.startswith('/compat/') and entry.rsplit('/', 1)[-1] not in names]
        self.assertEqual(missing, [], f'/compat paths missing from the image: {missing}')


if __name__ == '__main__':
    unittest.main()
