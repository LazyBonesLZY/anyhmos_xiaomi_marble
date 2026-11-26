"""Exercise the actual probe's pixel oracle on a host; no Vulkan or devices."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


SERVICE_DIR = Path(__file__).resolve().parents[1] / 'compat' / 'services'
# Match the probe dimensions, but deliberately choose a different padded pitch
# from the Marble allocator. Supply only enough bytes for the last visible pixel.
W, H, PITCH = 37, 23, 160
CYAN = bytes((0, 255, 255, 255))
RED = bytes((255, 0, 0, 255))
OPAQUE_BLACK = bytes((0, 0, 0, 255))
FIXTURE = r'''
#define MARBLE_VULKAN_PIXEL_TEST
#include "marble_vulkan_native_clear.c"
int main(int argc, char **argv)
{
    if (argc != 2)
        return 99;
    unsigned char plane[4000];
    const size_t n = fread(plane, 1, sizeof(plane), stdin);
    const uint8_t expected[] = {0, 255, 255, 255};
    int rc = check_pixels(plane, !strcmp(argv[1], "short-pitch") ? 147 :
                          !strcmp(argv[1], "huge-pitch") ? UINT64_MAX : 160, n, expected, 2);
    return rc < 0 ? 3 : rc ? 2 : 0;
}
'''


def padded_plane(color):
    row = color * W
    return (row + b'\xa5' * (PITCH - len(row))) * (H - 1) + row


class MarbleVulkanProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temp.cleanup)
        cls.executable = Path(temp.name) / 'check_pixels'
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-O2',
                        '-Wall', '-Wextra', '-Werror', '-I', str(SERVICE_DIR), '-x', 'c', '-',
                        '-o', str(cls.executable)], input=FIXTURE, text=True, check=True)

    def sample(self, data, layout='valid'):
        return subprocess.run([str(self.executable), layout], input=data, capture_output=True)

    def test_uses_pitch_and_samples_visible_last_pixel(self):
        result = self.sample(padded_plane(CYAN))
        self.assertEqual(result.returncode, 0)
        self.assertIn(b'x=36 y=22 rgba=00ffffff', result.stdout)
        self.assertIn(b'matches=5/5 rgb_zero=0/5', result.stdout)

    def test_black_rgb_with_nonzero_alpha_and_padding_is_a_mismatch(self):
        result = self.sample(padded_plane(OPAQUE_BLACK))
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'matches=0/5 rgb_zero=5/5', result.stdout)

    def test_stale_clear_and_one_uncleared_corner_are_detected(self):
        for data, matches in ((padded_plane(RED), b'0/5'), (padded_plane(CYAN)[:-4] + RED, b'4/5')):
            with self.subTest(matches=matches):
                result = self.sample(data)
                self.assertEqual(result.returncode, 2)
                self.assertIn(b'matches=' + matches + b' rgb_zero=0/5', result.stdout)

    def test_truncated_or_invalid_pitch_is_rejected_before_sampling(self):
        good = padded_plane(CYAN)
        for data, layout in ((good[:-1], 'valid'), (good, 'short-pitch'), (good, 'huge-pitch')):
            with self.subTest(layout=layout):
                result = self.sample(data, layout)
                self.assertEqual(result.returncode, 3)
                self.assertEqual(result.stdout, b'')


if __name__ == '__main__':
    unittest.main()
