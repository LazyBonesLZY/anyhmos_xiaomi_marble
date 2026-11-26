import json
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT / 'tools/packaging'))
import build_marble_h20_d_sensors as pkg  # noqa: E402


HOST_C_HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ssc_backend.h"
#include "ssc_wire.h"

static size_t EncodeTag(uint8_t *out, uint32_t field, uint32_t wire)
{
    uint64_t v = ((uint64_t)field << 3) | wire;
    size_t n = 0;
    do {
        out[n++] = (uint8_t)((v & 0x7f) | (v >= 0x80 ? 0x80 : 0));
        v >>= 7;
    } while (v);
    return n;
}

static size_t EncodeVarint(uint8_t *out, uint64_t v)
{
    size_t n = 0;
    do {
        out[n++] = (uint8_t)((v & 0x7f) | (v >= 0x80 ? 0x80 : 0));
        v >>= 7;
    } while (v);
    return n;
}

static size_t EncodeBytes(uint8_t *out, uint32_t field, const void *data, size_t len)
{
    size_t n = EncodeTag(out, field, 2);
    n += EncodeVarint(out + n, len);
    if (len) memcpy(out + n, data, len);
    return n + len;
}

static size_t WrapQmiEvent(uint8_t *out, struct Suid suid, uint32_t msg, const void *payload, size_t plen)
{
    uint8_t sb[32];
    size_t sn = EncodeTag(sb, 1, 1); memcpy(sb + sn, &suid.low, 8); sn += 8;
    sn += EncodeTag(sb + sn, 2, 1); memcpy(sb + sn, &suid.high, 8); sn += 8;

    uint8_t ev[256];
    size_t en = EncodeTag(ev, 1, 5); memcpy(ev + en, &msg, 4); en += 4;
    uint64_t ts = 987654321ULL;
    en += EncodeTag(ev + en, 2, 1); memcpy(ev + en, &ts, 8); en += 8;
    en += EncodeBytes(ev + en, 3, payload, plen);

    size_t n = EncodeBytes(out, 1, sb, sn);
    n += EncodeBytes(out + n, 2, ev, en);
    return n;
}

int main(void)
{
    struct Suid suid = {0x1122334455667788ULL, 0x99aabbccddeeff00ULL};
    uint8_t pkt[1012];

    /* 1. Request modes: 513 vs 514 vs 10 */
    assert(BuildStreamRequestMode(pkt, suid, false, true, 5.0f) > 0);
    assert(BuildStreamRequestMode(pkt, suid, false, false, 25.0f) > 0);
    assert(BuildStreamRequestMode(pkt, suid, true, true, 5.0f) > 0);

    /* 2. Type class recognition across marble sensors */
    const char *names[] = {
        "accel", "gyro", "ambient_light", "mag", "proximity", "ori", "gravity",
        "akm_rotv", "game_rv", "sig_motion", "pedometer", "oem_step_counter",
        "geomag_rv", "device_orient"
    };
    const unsigned int expected_kinds[] = {
        1, 2, 5, 6, 12, 256, 257, 259, 262, 264, 265, 266, 277, 279
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        struct View v = {(const uint8_t *)names[i], strlen(names[i])};
        assert(SscTypeClassFromName(v) == expected_kinds[i]);
    }

    /* 3. ATTR id=16 stream_type sfixed64=1 */
    struct SscSensorInfo info = {0};
    uint8_t attr_val[32], item[16];
    uint64_t st_val = 1;
    size_t in = EncodeTag(item, 4, 1); memcpy(item + in, &st_val, 8); in += 8;
    size_t an = EncodeBytes(attr_val, 1, item, in);
    assert(ParseInfoAttribute(16, (struct View){attr_val, an}, &info));
    assert((info.present & SSC_INFO_STREAM_TYPE) && info.stream_type == 1);

    /* 4. ALS 4-float sample + Gravity 6-float sample + msg 768/1022/1031 skips + Prox 769 + Ped 775 */
    struct Sensor s = {.type = "gravity", .suid = suid};
    float grav6[6] = {0.1f, -0.2f, 9.75f, 0.0f, 0.0f, 1.0f};
    uint8_t spayload[64];
    size_t spn = EncodeBytes(spayload, 1, grav6, sizeof(grav6));
    spn += EncodeTag(spayload + spn, 2, 0);
    spayload[spn++] = 3;
    uint8_t msg[512];
    size_t mn = WrapQmiEvent(msg, suid, 1025, spayload, spn);
    assert(ParseEvents((struct View){msg, mn}, 2, &s));
    assert(s.stored_samples == 1 && s.samples[0].value_count == 4);
    assert(fabsf(s.samples[0].data[2] - 9.75f) < 1e-5f);

    /* Ignored metadata / calibration / recurrent events */
    uint8_t dummy[8] = {0x08, 0x01};
    const uint32_t skip_ids[] = {768, 775, 1022, 1026, 1027, 1031};
    for (size_t i = 0; i < sizeof(skip_ids) / sizeof(skip_ids[0]); ++i) {
        mn = WrapQmiEvent(msg, suid, skip_ids[i], dummy, 2);
        assert(ParseEvents((struct View){msg, mn}, 2, &s));
        assert(s.stored_samples == 1);
    }

    /* Proximity NEAR (f1=1 -> 0.0f) and FAR (f1=0 -> 5.0f) */
    s.stored_samples = 0;
    uint8_t prox_near[] = {0x08, 0x01, 0x18, 0x03};
    mn = WrapQmiEvent(msg, suid, 769, prox_near, sizeof(prox_near));
    assert(ParseEvents((struct View){msg, mn}, 2, &s));
    assert(s.stored_samples == 1 && s.samples[0].data[0] == 0.0f && s.samples[0].status == 3);

    uint8_t prox_far[] = {0x08, 0x00, 0x18, 0x03};
    mn = WrapQmiEvent(msg, suid, 769, prox_far, sizeof(prox_far));
    assert(ParseEvents((struct View){msg, mn}, 2, &s));
    assert(s.stored_samples == 2 && s.samples[1].data[0] == 5.0f);

    /* Pedometer step event msg 1028 (775 config event is ignored above) */
    mn = WrapQmiEvent(msg, suid, 1028, dummy, 2);
    assert(ParseEvents((struct View){msg, mn}, 2, &s));
    assert(s.stored_samples == 3 && s.samples[2].data[0] == 1.0f);

    puts("HOST_C_HARNESS_OK");
    return 0;
}
'''


class MarbleBatchDSensorsTests(unittest.TestCase):
    def test_ssc_wire_host_c_harness(self):
        scratch = '/mnt/e/marble-tmp' if Path('/mnt/e/marble-tmp').is_dir() else None
        with tempfile.TemporaryDirectory(dir=scratch) as td:
            src = Path(td) / 'harness.c'
            exe = Path(td) / 'harness'
            src.write_text(HOST_C_HARNESS, encoding='utf-8')
            subprocess.run(
                ['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                 '-I', str(ROOT / 'compat/sensors'), str(src), '-lm', '-o', str(exe)],
                check=True,
            )
            res = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('HOST_C_HARNESS_OK', res.stdout)

    def test_libsensor_utils_parcel_patch(self):
        donor = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system/lib64/platformsdk/libsensor_utils.z.so'
        patched = ROOT / 'work/marble-d-build/sensors-out/libsensor_utils.z.so'
        orig_bytes = donor.read_bytes()
        expected = pkg.patch_sensor_parcel(orig_bytes)
        self.assertEqual(patched.read_bytes(), expected)
        diffs = [i for i, (a, b) in enumerate(zip(orig_bytes, expected)) if a != b]
        self.assertEqual({d & ~3 for d in diffs}, {0x10c68, 0x10c90, 0x10cb8})

    def test_optional_sa_and_brightness_config(self):
        b2_list = set(pkg.OPTIONAL_SA_B2.split())
        d_list = set(pkg.OPTIONAL_SA_D.split())
        self.assertEqual(b2_list - d_list, {'2802', '3601'})
        self.assertEqual(d_list - b2_list, set())
        bright = json.loads((pkg.D_SCRIPTS / 'brightness_multiple_sensor_config-marble.json').read_text())
        self.assertIs(bright['isSupportBackSensor'], False)

    def test_package_roundtrip_on_b2_base(self):
        if not pkg.BASE_DEFAULT.is_file():
            self.skipTest(f'Missing base image: {pkg.BASE_DEFAULT}')
        scratch = '/mnt/e/marble-tmp' if Path('/mnt/e/marble-tmp').is_dir() else None
        with tempfile.TemporaryDirectory(dir=scratch) as td:
            out_dir = Path(td)
            out_img = pkg.build(pkg.BASE_DEFAULT, out_dir)
            self.assertTrue(out_img.is_file())
            report = json.loads((out_dir / 'd-build-report.json').read_text())
            self.assertEqual(report['entries_before'], 507)
            self.assertEqual(report['entries_after'], 507 + len(pkg.ADDED_FILES))
            self.assertEqual(report['unchanged_entries'], 507 - 4)
            stage_text = pkg.D_SCRIPTS / 'start-sensors-marble.sh'
            st = stage_text.read_text()
            self.assertLess(st.index('frpc-adsprpc.ko'), st.index('/sys/kernel/boot_slpi/boot'))
            self.assertLess(st.index('recovery'), st.index('/sys/kernel/boot_slpi/boot'))
            self.assertIn("pgrep -f '/vendor/bin/sscrpcd'", st)
            self.assertNotIn('pgrep -x sscrpcd', st)
            fastrpc = (pkg.D_SCRIPTS / 'stage-fastrpc-nodes.sh').read_text()
            self.assertIn('0664', fastrpc)
            self.assertIn('0644', fastrpc)
            prep = (pkg.D_SCRIPTS / 'prepare-private-persist.sh').read_text()
            self.assertIn('ro,noload', prep)
            self.assertIn('grep -q persist /proc/mounts', prep)

            import importlib.util
            spec = importlib.util.spec_from_file_location('arc', pkg.ORACLE)
            arc = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(arc)
            r = arc.load_tools()
            img_bytes = out_img.read_bytes()
            self.assertEqual(len(img_bytes), 100663296)
            entries, _, _, _, _ = r.extract_entries(out_img, img_bytes, r.layout(img_bytes), out_dir / 'verify_extract')
            packed_stage = entries[pkg.STAGE_MEMBER][1].decode('utf-8')
            for asset in (
                'native_sensorspd_launcher',
                'native_sensor_hdi_publisher',
                'native_sensor_client_probe',
                'libsensor_interface_service_3.1.z.so',
                'libsensor_utils.z.so',
                'sensors-marble.json',
                'brightness_multiple_sensor_config-marble.json',
            ):
                self.assertIn(asset, packed_stage)
            self.assertIn(pkg.OPTIONAL_SA_D, packed_stage)
            packed_seed = json.loads(entries[pkg.SEED_MEMBER][1])
            self.assertIn('/compat/etc/init-d-sensors-marble.cfg', packed_seed['import'])


if __name__ == '__main__':
    unittest.main()
