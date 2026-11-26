#!/usr/bin/env python3
"""Generate and verify small, synthetic 6-second media controls on the host."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import subprocess
import wave
from fractions import Fraction
from pathlib import Path

WIDTH, HEIGHT, FPS = 320, 240, 30
RATE, SECONDS = 48_000, 6
FRAME_BYTES = WIDTH * HEIGHT * 3 // 2
SAMPLE_COUNT = RATE * SECONDS
FRAME_COUNT = FPS * SECONDS
TONE_WINDOW = RATE // 5
# Studio-range BT.601 (I420). Samples at (16,120) distinguish UV order.
SCENES = (
    ('red', (81, 90, 240), 400),
    ('green', (145, 54, 34), 600),
    ('blue', (41, 240, 110), 800),
)
SEEK_MS = (4200, 1200, 3200)
MEDIA = ('tones.wav', 'tones.m4a', 'av_seek.mp4')
REPORT = 'media-report.json'
MAX_FILE_BYTES = 4 * 1024 * 1024
FFMPEG = ('ffmpeg', '-hide_banner', '-nostdin', '-v', 'error', '-xerror')
LOCAL_INPUT = ('-protocol_whitelist', 'file,pipe')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def command(args, *, data=None):
    return subprocess.run(args, input=data, capture_output=True, timeout=30, check=True).stdout


def scene_at(frame):
    return SCENES[frame // (2 * FPS)]


def reference_pcm():
    samples = bytearray()
    fade = RATE // 100
    scene_samples = 2 * RATE
    for _, _, frequency in SCENES:
        period = RATE // frequency
        assert RATE % frequency == 0
        cycle = [round(8191 * math.sin(math.tau * i / period)) for i in range(period)]
        for i in range(scene_samples):
            gain = min(i, scene_samples - 1 - i, fade)
            samples += struct.pack('<h', round(cycle[i % period] * gain / fade))
    return bytes(samples)


def reference_frame(index):
    _, (y0, u0, v0), _ = scene_at(index)
    y = bytearray([y0]) * (WIDTH * HEIGHT)
    u = bytearray([u0]) * (WIDTH * HEIGHT // 4)
    v = bytearray([v0]) * (WIDTH * HEIGHT // 4)

    def rectangle(x, top, width, height, value):
        # Rectangle coordinates and extents are even for 4:2:0 subsampling.
        for plane, stride, scale, level in ((y, WIDTH, 1, value), (u, WIDTH // 2, 2, 128),
                                          (v, WIDTH // 2, 2, 128)):
            row = bytes([level]) * (width // scale)
            for r in range(top // scale, (top + height) // scale):
                off = r * stride + x // scale
                plane[off:off + len(row)] = row

    for bit in range(8):
        rectangle(24 + bit * 34, 24, 24, 20, 235 if index & (1 << (7 - bit)) else 16)
    for marker in range(index // (2 * FPS) + 1):
        rectangle(24 + marker * 44, 78, 24, 24, 235)
    rectangle(24 + 4 * (index % (2 * FPS)), 182, 24, 32, 235)
    return bytes(y + u + v)


def encode(out):
    with wave.open(str(out / MEDIA[0]), 'wb') as audio:
        audio.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
        audio.writeframes(reference_pcm())
    command([*FFMPEG, '-n', *LOCAL_INPUT, '-i', str(out / MEDIA[0]), '-map', '0:a:0',
             '-map_metadata', '-1', '-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '64k',
             '-threads:a', '1', '-movflags', '+faststart', str(out / MEDIA[1])])
    command([*FFMPEG, '-n', *LOCAL_INPUT, '-f', 'rawvideo', '-pixel_format', 'yuv420p',
             '-video_size', f'{WIDTH}x{HEIGHT}', '-framerate', str(FPS), '-i', 'pipe:0',
             *LOCAL_INPUT, '-i', str(out / MEDIA[1]), '-map', '0:v:0', '-map', '1:a:0',
             '-map_metadata', '-1', '-c:v', 'libx264', '-threads:v', '1', '-profile:v', 'baseline',
             '-pix_fmt', 'yuv420p', '-preset', 'fast', '-crf', '18', '-g', str(FPS), '-bf', '0',
             '-sc_threshold', '0', '-colorspace', 'smpte170m', '-color_primaries', 'smpte170m',
             '-color_trc', 'smpte170m', '-color_range', 'tv', '-video_track_timescale', '90000',
             '-c:a', 'copy', '-movflags', '+faststart', str(out / MEDIA[2])],
            data=b''.join(reference_frame(i) for i in range(FRAME_COUNT)))


def demuxer(path):
    return 'wav' if path.suffix == '.wav' else 'mov'


def probe(path, *fields):
    return json.loads(command(['ffprobe', '-v', 'error', *LOCAL_INPUT, '-f', demuxer(path),
                               '-of', 'json', *fields, str(path)]))


def decode_audio(path, seek=None):
    opts = [] if seek is None else ['-ss', f'{seek / 1000:.3f}']
    return command([*FFMPEG, *LOCAL_INPUT, '-f', demuxer(path), *opts, '-threads', '1', '-i', str(path),
                    '-map', '0:a:0', '-t', '6.05' if seek is None else '0.200',
                    '-f', 's16le', '-c:a', 'pcm_s16le', 'pipe:1'])


def decode_video(path, seek=None):
    opts = [] if seek is None else ['-ss', f'{seek / 1000:.3f}']
    count = FRAME_COUNT + 1 if seek is None else 1
    return command([*FFMPEG, *LOCAL_INPUT, '-f', 'mov', *opts, '-threads', '1', '-i', str(path),
                    '-map', '0:v:0', '-frames:v', str(count), '-fps_mode', 'passthrough',
                    '-f', 'rawvideo', '-pix_fmt', 'yuv420p', 'pipe:1'])


def check_color_and_counter(frame, index):
    require(len(frame) == FRAME_BYTES, f'frame {index}: truncated I420')
    expected = reference_frame(index)
    luma = WIDTH * HEIGHT
    xy = 120 * WIDTH + 16
    chroma = 60 * (WIDTH // 2) + 8
    points = [xy, luma + chroma, luma + luma // 4 + chroma]
    points += [34 * WIDTH + 36 + bit * 34 for bit in range(8)]
    points += [90 * WIDTH + 36 + marker * 44 for marker in range(len(SCENES))]
    marker_x = 36 + 4 * (index % (2 * FPS))
    background_x = 272 if marker_x < 154 else 36
    points += [194 * WIDTH + marker_x, 194 * WIDTH + background_x]
    for point in points:
        require(abs(frame[point] - expected[point]) <= 12,
                f'frame {index}: color, counter or motion marker differs at byte {point}')
    return [frame[point] for point in points[:3]]


def check_tone(pcm, begin, frequency):
    start = begin * 2
    window = pcm[start:start + TONE_WINDOW * 2]
    require(len(window) == TONE_WINDOW * 2, 'incomplete audio analysis window')
    samples = struct.unpack(f'<{TONE_WINDOW}h', window)
    rms = math.sqrt(sum(x * x for x in samples) / len(samples))
    require(3500 <= rms <= 8500 and max(map(abs, samples)) < 30_000, 'unexpected tone amplitude or clipping')
    amplitudes = {}
    for _, _, freq in SCENES:
        period = RATE // freq
        folded = [0] * period
        for i, value in enumerate(samples):
            folded[i % period] += value
        real = sum(x * math.cos(math.tau * i / period) for i, x in enumerate(folded))
        imag = sum(x * math.sin(math.tau * i / period) for i, x in enumerate(folded))
        amplitudes[freq] = 2 * math.hypot(real, imag) / len(samples)
    peak = amplitudes[frequency]
    require(peak >= rms and all(amp < peak * 0.15 for f, amp in amplitudes.items() if f != frequency),
            f'expected dominant {frequency} Hz, saw {amplitudes}')
    return {'frequency_hz': frequency, 'rms_s16': round(rms, 2)}


def file_identity(path):
    require(not path.is_symlink() and path.is_file(), f'unexpected file type: {path.name}')
    with path.open('rb') as stream:
        data = stream.read(MAX_FILE_BYTES)
    require(0 < len(data) < MAX_FILE_BYTES, f'unexpected file size: {path.name}')
    return {'bytes': len(data), 'sha256': sha(data)}


def read_report(directory):
    path = directory / REPORT
    require(not path.is_symlink() and path.is_file(), 'missing regular generation report')
    with path.open('rb') as stream:
        data = stream.read(65536)
    require(0 < len(data) < 65536, 'unexpected generation report size')
    report = json.loads(data)
    require(isinstance(report, dict) and isinstance(report.get('files'), dict) and
            report['files'].keys() == set(MEDIA), 'unexpected generation report file set')
    return report['files']


def verify(directory, expected=None):
    identities = {name: file_identity(directory / name) for name in MEDIA}
    if expected is not None:
        for name, identity in identities.items():
            record = expected.get(name)
            require(isinstance(record, dict) and all(record.get(k) == v for k, v in identity.items()),
                    f'{name}: supplied bytes do not match generation report')
    result = {'host_only': True, 'video_frames': FRAME_COUNT, 'files': {}, 'seek': []}
    audio = {}
    streams = {}
    for name in MEDIA:
        path = directory / name
        meta = probe(path, '-show_streams')['streams']
        streams[name] = meta
        types = ['video', 'audio'] if name.endswith('.mp4') else ['audio']
        require([entry['codec_type'] for entry in meta] == types, f'{name}: unexpected streams')
        a = meta[-1]
        require(a['sample_rate'] == str(RATE) and a['channels'] == 1 and
                int(a['duration_ts']) * Fraction(a['time_base']) == SECONDS and int(a.get('start_pts', 0)) == 0,
                f'{name}: unexpected rate, channels or presentation duration')
        if name.endswith('.wav'):
            require(a['codec_name'] == 'pcm_s16le', 'expected PCM WAV')
        else:
            require(a['codec_name'] == 'aac' and a['profile'] == 'LC', 'expected AAC-LC')
        pcm = decode_audio(path)
        require(len(pcm) % 2 == 0 and SAMPLE_COUNT <= len(pcm) // 2 <= SAMPLE_COUNT + 1023,
                f'{name}: empty, short or excessive decoded audio')
        if name.endswith('.wav'):
            require(pcm == reference_pcm(), 'WAV PCM differs from the reference')
        audio[name] = pcm
        tones = [check_tone(pcm, (2 * i + 1) * RATE, freq) for i, (_, _, freq) in enumerate(SCENES)]
        result['files'][name] = {**identities[name],
                                 'presentation_ms': SECONDS * 1000, 'decoded_samples': len(pcm) // 2,
                                 'decoded_pcm_sha256': sha(pcm), 'tones': tones}
    require(audio[MEDIA[1]] == audio[MEDIA[2]], 'MP4 audio differs from the standalone AAC track')

    path = directory / MEDIA[2]
    v = streams[path.name][0]
    require(v['codec_name'] == 'h264' and 'Baseline' in v['profile'] and v['pix_fmt'] == 'yuv420p' and
            (v['width'], v['height']) == (WIDTH, HEIGHT) and Fraction(v['avg_frame_rate']) == FPS and
            int(v['duration_ts']) * Fraction(v['time_base']) == SECONDS and int(v['start_pts']) == 0 and
            v['color_range'] == 'tv' and v['color_space'] == 'smpte170m', 'unexpected H.264 geometry, timing or color metadata')
    frames = probe(path, '-select_streams', 'v:0', '-show_frames',
                   '-show_entries', 'frame=best_effort_timestamp,key_frame')['frames']
    time_base = Fraction(v['time_base'])
    require(len(frames) == FRAME_COUNT and all(int(f['best_effort_timestamp']) * time_base == Fraction(i, FPS)
                                             for i, f in enumerate(frames)), 'non-contiguous presentation timestamps')
    key_indices = [i for i, f in enumerate(frames) if f['key_frame']]
    require(key_indices == list(range(0, FRAME_COUNT, FPS)), 'unexpected seek keyframe positions')
    raw = decode_video(path)
    require(len(raw) == FRAME_COUNT * FRAME_BYTES, 'did not decode exactly 180 frames')
    for i in range(FRAME_COUNT):
        check_color_and_counter(raw[i * FRAME_BYTES:(i + 1) * FRAME_BYTES], i)
    result['keyframe_ms'] = [i * 1000 // FPS for i in key_indices]
    result['decoded_i420_sha256'] = sha(raw)
    for ms in SEEK_MS:
        index = ms * FPS // 1000
        color, _, tone = scene_at(index)
        # Separate host demux/decode opens, not a single player's seek lifecycle.
        frame = decode_video(path, ms)
        result['seek'].append({
            'requested_ms': ms, 'decoded_frame_index': index, 'expected_color': color,
            'sample_yuv': check_color_and_counter(frame, index), 'i420_sha256': sha(frame),
            'tones': {name: check_tone(decode_audio(directory / name, ms), 0, tone) for name in MEDIA},
        })
    return result


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    mode = cli.add_mutually_exclusive_group(required=True)
    mode.add_argument('--out', type=Path, help='Generate in a new directory; never overwrite')
    mode.add_argument('--verify', type=Path, help='Verify the known fixture files in an existing directory')
    args = cli.parse_args()
    directory = (args.out or args.verify).resolve()
    expected = None
    if args.out is not None:
        directory.mkdir()
        encode(directory)
    else:
        expected = read_report(directory)
    result = verify(directory, expected)
    if args.out is not None:
        result['ffmpeg'] = command(['ffmpeg', '-version']).decode().splitlines()[0]
        with (directory / REPORT).open('x', encoding='utf-8') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
    print(f'HOST_FIXTURE_OK {directory}: {FRAME_COUNT} I420 frames, WAV/AAC and {len(result["seek"])} seek controls')


if __name__ == '__main__':
    main()
