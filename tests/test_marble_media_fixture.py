"""Offline checks for the fixed media fixture; nothing runs on a target device."""
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / 'tests/fixtures/marble-media/generate.py'
SPEC = importlib.util.spec_from_file_location('marble_media_fixture', GENERATOR)
fixture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixture)


class MarbleMediaFixtureTests(unittest.TestCase):
    def test_frame_oracle_rejects_stale_frame_wrong_uv_and_truncation(self):
        index = 126  # 4.2 s, blue phase
        frame = fixture.reference_frame(index)
        self.assertEqual(fixture.check_color_and_counter(frame, index), [41, 240, 110])
        y = 320 * 240
        v = y + y // 4
        negatives = (
            (fixture.reference_frame(index - 1), 'differs'),  # same color, wrong frame
            (frame[:y] + frame[v:] + frame[y:v], 'differs'),    # U and V reversed
            (bytes(len(frame)), 'differs'),
            (frame[:-1], 'truncated'),
        )
        for bad, message in negatives:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                fixture.check_color_and_counter(bad, index)

    def test_tone_oracle_distinguishes_frequency_silence_and_short_reads(self):
        pcm = fixture.reference_pcm()
        self.assertEqual(len(pcm), 288000 * 2)
        for second, hz in ((1, 400), (3, 600), (5, 800)):
            self.assertEqual(fixture.check_tone(pcm, second * 48000, hz)['frequency_hz'], hz)
        with self.assertRaisesRegex(ValueError, 'expected dominant'):
            fixture.check_tone(pcm, 48000, 800)
        with self.assertRaisesRegex(ValueError, 'amplitude'):
            fixture.check_tone(bytes(fixture.TONE_WINDOW * 2), 0, 400)
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            fixture.check_tone(pcm, 288000 - 1, 800)

    def test_file_hash_preflight_rejects_replacement_before_decoding(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in fixture.MEDIA:
                (root / name).write_bytes(b'fixed')
            records = {name: fixture.file_identity(root / name) for name in fixture.MEDIA}
            (root / fixture.REPORT).write_text(json.dumps({'files': records}), encoding='utf-8')
            (root / 'av_seek.mp4').write_bytes(b'other')  # same length, different bytes
            with patch.object(fixture, 'probe') as probe:
                with self.assertRaisesRegex(ValueError, 'av_seek.mp4: supplied bytes do not match'):
                    fixture.verify(root, fixture.read_report(root))
                probe.assert_not_called()

    def test_generation_refuses_an_existing_output_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            marker = root / 'tones.wav'
            marker.write_bytes(b'preserve me')
            ran = subprocess.run([sys.executable, '-B', str(GENERATOR), '--out', str(root)],
                                 capture_output=True, timeout=5)
            self.assertNotEqual(ran.returncode, 0)
            self.assertEqual(marker.read_bytes(), b'preserve me')
            self.assertFalse((root / fixture.REPORT).exists())

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'FFmpeg tools absent')
    def test_generated_media_and_seek_frames_decode_on_the_host(self):
        encoders = fixture.command(['ffmpeg', '-hide_banner', '-encoders']).decode()
        names = {line.split()[1] for line in encoders.splitlines() if len(line.split()) >= 2}
        if not {'libx264', 'aac'} <= names:
            self.skipTest('FFmpeg libx264 or AAC encoder absent')
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'samples'
            subprocess.run([sys.executable, '-B', str(GENERATOR), '--out', str(out)],
                           check=True, capture_output=True, timeout=45)
            report = json.loads((out / fixture.REPORT).read_bytes())
            self.assertTrue(report['host_only'])
            self.assertEqual(report['video_frames'], 180)
            self.assertEqual(report['keyframe_ms'], [0, 1000, 2000, 3000, 4000, 5000])
            self.assertEqual(set(report['files']), {'tones.wav', 'tones.m4a', 'av_seek.mp4'})
            for record in report['files'].values():
                self.assertEqual(record['presentation_ms'], 6000)
            self.assertEqual(report['files']['tones.m4a']['decoded_pcm_sha256'],
                             report['files']['av_seek.mp4']['decoded_pcm_sha256'])
            self.assertEqual([(s['requested_ms'], s['decoded_frame_index'], s['expected_color'])
                              for s in report['seek']],
                             [(4200, 126, 'blue'), (1200, 36, 'red'), (3200, 96, 'green')])
            for seek, hz in zip(report['seek'], (800, 400, 600), strict=True):
                self.assertEqual({t['frequency_hz'] for t in seek['tones'].values()}, {hz})
