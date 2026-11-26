"""Local AVPlayer probe tests: pthread host double and SDK cross-link only.

The host double drives NDK callbacks and a monotonic timeline, not a decoder.
The AArch64 build is inspected as an ELF file and never executed by these tests.
"""
import array
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import wave


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'compat/codec/native_avplayer_probe.c'
BUILD = ROOT / 'compat/codec/build-native-avplayer-probe.sh'
SDK = Path(os.environ.get('HMOS_SDK') or ROOT / 'work/ohos-sdk/installed/native')
INCLUDE = SDK / 'sysroot/usr/include'
DONOR_LIB = ROOT / 'work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system/lib64'
PLAYER_EXPORTS = {
    'OH_AVPlayer_Create', 'OH_AVPlayer_SetOnInfoCallback', 'OH_AVPlayer_SetOnErrorCallback',
    'OH_AVPlayer_SetFDSource', 'OH_AVPlayer_GetDuration', 'OH_AVPlayer_GetCurrentTime',
    'OH_AVPlayer_Prepare', 'OH_AVPlayer_Play', 'OH_AVPlayer_Pause', 'OH_AVPlayer_Seek',
    'OH_AVPlayer_ReleaseSync', 'OH_PLAYER_STATE', 'OH_PLAYER_SEEK_POSITION',
}

# Fallback for CI without a local SDK. When present, compile the same test
# double against the actual AVPlayer headers (but not target libc/SDK .so files).
MOCK_HEADER = r'''
#include <stdbool.h>
#include <stdint.h>
typedef struct OH_AVPlayer OH_AVPlayer;
typedef struct OH_AVFormat OH_AVFormat;
typedef enum { AV_ERR_OK = 0, AV_ERR_INVALID_VAL = 3 } OH_AVErrCode;
typedef enum { AV_IDLE, AV_INITIALIZED, AV_PREPARED, AV_PLAYING, AV_PAUSED,
              AV_STOPPED, AV_COMPLETED, AV_RELEASED, AV_ERROR } AVPlayerState;
typedef enum { AV_SEEK_NEXT_SYNC, AV_SEEK_PREVIOUS_SYNC, AV_SEEK_CLOSEST } AVPlayerSeekMode;
typedef enum { AV_INFO_TYPE_SEEKDONE = 0, AV_INFO_TYPE_EOS = 3,
              AV_INFO_TYPE_STATE_CHANGE = 4 } AVPlayerOnInfoType;
typedef void (*OH_AVPlayerOnInfoCallback)(OH_AVPlayer *, AVPlayerOnInfoType, OH_AVFormat *, void *);
typedef void (*OH_AVPlayerOnErrorCallback)(OH_AVPlayer *, int32_t, const char *, void *);
extern const char *OH_PLAYER_STATE;
extern const char *OH_PLAYER_SEEK_POSITION;
bool OH_AVFormat_GetIntValue(OH_AVFormat *, const char *, int32_t *);
OH_AVPlayer *OH_AVPlayer_Create(void);
OH_AVErrCode OH_AVPlayer_SetOnInfoCallback(OH_AVPlayer *, OH_AVPlayerOnInfoCallback, void *);
OH_AVErrCode OH_AVPlayer_SetOnErrorCallback(OH_AVPlayer *, OH_AVPlayerOnErrorCallback, void *);
OH_AVErrCode OH_AVPlayer_SetFDSource(OH_AVPlayer *, int32_t, int64_t, int64_t);
OH_AVErrCode OH_AVPlayer_GetDuration(OH_AVPlayer *, int32_t *);
OH_AVErrCode OH_AVPlayer_GetCurrentTime(OH_AVPlayer *, int32_t *);
OH_AVErrCode OH_AVPlayer_Prepare(OH_AVPlayer *);
OH_AVErrCode OH_AVPlayer_Play(OH_AVPlayer *);
OH_AVErrCode OH_AVPlayer_Pause(OH_AVPlayer *);
OH_AVErrCode OH_AVPlayer_Seek(OH_AVPlayer *, int32_t, AVPlayerSeekMode);
OH_AVErrCode OH_AVPlayer_ReleaseSync(OH_AVPlayer *);
'''

MOCK_C = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <multimedia/player_framework/avplayer.h>

const char *OH_PLAYER_STATE = "state";
const char *OH_PLAYER_SEEK_POSITION = "seek_position";
struct OH_AVFormat { const char *key; int32_t value; };
bool OH_AVFormat_GetIntValue(OH_AVFormat *body, const char *key, int32_t *out)
{
    if (body->key != key) return false;
    *out = body->value;
    return true;
}

struct OH_AVPlayer {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    bool closing;
    OH_AVPlayerOnInfoCallback info;
    OH_AVPlayerOnErrorCallback error;
    void *info_user, *error_user;
    int fd;
    AVPlayerState state;
    AVPlayerState next_state;
    int32_t pending_seek, base;
    int seeks;
    int64_t anchor, due, eos_at;
};

static bool mode(const char *name)
{
    const char *env = getenv("AVPLAYER_MOCK");
    return env != NULL && strcmp(env, name) == 0;
}

static int64_t now_ms(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void event(OH_AVPlayer *p, AVPlayerOnInfoType type, const char *key, int32_t value)
{
    OH_AVFormat data = {key, value};
    p->info(p, type, key == NULL ? NULL : &data, p->info_user);
}

static void state(OH_AVPlayer *p, AVPlayerState value)
{
    p->state = value;
    event(p, AV_INFO_TYPE_STATE_CHANGE, OH_PLAYER_STATE, value);
}

static int32_t position(OH_AVPlayer *p)
{
    int64_t position = p->base;
    if (p->state == AV_PLAYING && p->seeks == 0 && mode("single_jump")) {
        if (now_ms() - p->anchor >= 240) position += 300;
    } else if ((p->state == AV_PLAYING && !mode(p->seeks == 0 ? "stalled_play" : "stalled_resume")) ||
               (p->state == AV_PAUSED && mode("pause_drift"))) {
        position += now_ms() - p->anchor;
    }
    return position < 6000 ? (int32_t)position : 6000;
}

static void dispatch(OH_AVPlayer *p)
{
    p->due = 0;
    if (p->pending_seek != 0) {
        int32_t target = p->pending_seek;
        p->pending_seek = 0;
        if (!mode("seek_position") || p->seeks == 1) p->base = target;
        if (p->seeks == 2 && mode("missing_seekdone")) return;
        if (p->seeks == 2 && mode("stale_seekdone")) target = 4200;
        event(p, AV_INFO_TYPE_SEEKDONE, mode("malformed_seek") ? NULL : OH_PLAYER_SEEK_POSITION, target);
        if (mode("duplicate_seekdone")) event(p, AV_INFO_TYPE_SEEKDONE, OH_PLAYER_SEEK_POSITION, target);
        return;
    }
    AVPlayerState next = p->next_state;
    if (next == AV_PREPARED && mode("callback_error")) {
        p->error(p, 0, "synthetic error", p->error_user);
        return;
    }
    if (next == AV_PREPARED && mode("noisy_prepare")) {
        state(p, AV_INITIALIZED);
        p->due = now_ms() + 20;
        return;
    }
    if (next == AV_PAUSED) p->base = position(p);
    if (next == AV_PLAYING && p->seeks == 2 && mode("resume_restarts")) p->base = 0;
    p->anchor = now_ms();
    if (next == AV_PLAYING && p->seeks == 2) {
        p->eos_at = p->anchor + (mode("early_eos") ? 1300 : 6000 - p->base);
    }
    state(p, next);
}

static void finish(OH_AVPlayer *p)
{
    p->eos_at = 0;
    p->base = mode("end_position") ? 2000 : 6000;
    p->anchor = now_ms();
    if (mode("completed_first")) state(p, AV_COMPLETED);
    if (!mode("missing_eos")) event(p, AV_INFO_TYPE_EOS, NULL, 0);
    if (!mode("completed_first") && !mode("missing_completed")) state(p, AV_COMPLETED);
}

static void *worker(void *data)
{
    OH_AVPlayer *p = data;
    pthread_mutex_lock(&p->lock);
    while (!p->closing) {
        if (p->due != 0 && now_ms() >= p->due) dispatch(p);
        if (p->eos_at != 0 && now_ms() >= p->eos_at) finish(p);
        int64_t next = p->due != 0 ? p->due : p->eos_at;
        if (next == 0) {
            assert(pthread_cond_wait(&p->wake, &p->lock) == 0);
        } else {
            struct timespec t = {(time_t)(next / 1000), (long)(next % 1000) * 1000000};
            int rc = pthread_cond_timedwait(&p->wake, &p->lock, &t);
            assert(rc == 0 || rc == ETIMEDOUT);
        }
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

OH_AVPlayer *OH_AVPlayer_Create(void)
{
    OH_AVPlayer *p = calloc(1, sizeof(*p));
    assert(p);
    p->fd = -1;
    p->state = AV_IDLE;
    pthread_condattr_t attr;
    assert(pthread_condattr_init(&attr) == 0);
    assert(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0);
    assert(pthread_cond_init(&p->wake, &attr) == 0);
    assert(pthread_condattr_destroy(&attr) == 0);
    assert(pthread_mutex_init(&p->lock, NULL) == 0);
    assert(pthread_create(&p->thread, NULL, worker, p) == 0);
    puts("MOCK_CREATE");
    return p;
}

OH_AVErrCode OH_AVPlayer_SetOnInfoCallback(OH_AVPlayer *p, OH_AVPlayerOnInfoCallback cb, void *user)
{
    p->info = cb;
    p->info_user = user;
    return AV_ERR_OK;
}

OH_AVErrCode OH_AVPlayer_SetOnErrorCallback(OH_AVPlayer *p, OH_AVPlayerOnErrorCallback cb, void *user)
{
    p->error = cb;
    p->error_user = user;
    return AV_ERR_OK;
}

OH_AVErrCode OH_AVPlayer_SetFDSource(OH_AVPlayer *p, int32_t fd, int64_t offset, int64_t size)
{
    struct stat st;
    char riff[12];
    assert(p->info && p->info_user && p->error && p->error_user);
    assert(fstat(fd, &st) == 0 && offset == 0 && size == st.st_size);
    assert((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    assert(pread(fd, riff, sizeof(riff), 0) == (ssize_t)sizeof(riff));
    assert(memcmp(riff, "RIFF", 4) == 0 && memcmp(riff + 8, "WAVE", 4) == 0);
    p->fd = fd;
    if (mode("source_rc")) return AV_ERR_INVALID_VAL;
    state(p, AV_INITIALIZED);  /* Also exercise inline callbacks. */
    if (mode("preseek")) event(p, AV_INFO_TYPE_SEEKDONE, OH_PLAYER_SEEK_POSITION, 4200);
    return AV_ERR_OK;
}

static void schedule(OH_AVPlayer *p)
{
    assert(p->due == 0);
    p->due = now_ms() + 15;
    pthread_cond_signal(&p->wake);
}

static OH_AVErrCode request(OH_AVPlayer *p, AVPlayerState from, AVPlayerState to)
{
    pthread_mutex_lock(&p->lock);
    assert(p->state == from || (from == AV_PREPARED && p->state == AV_PAUSED));
    p->next_state = to;
    schedule(p);
    pthread_mutex_unlock(&p->lock);
    return AV_ERR_OK;
}

OH_AVErrCode OH_AVPlayer_Prepare(OH_AVPlayer *p) { return request(p, AV_INITIALIZED, AV_PREPARED); }
OH_AVErrCode OH_AVPlayer_Play(OH_AVPlayer *p) { return request(p, AV_PREPARED, AV_PLAYING); }
OH_AVErrCode OH_AVPlayer_Pause(OH_AVPlayer *p) { return request(p, AV_PLAYING, AV_PAUSED); }

OH_AVErrCode OH_AVPlayer_Seek(OH_AVPlayer *p, int32_t ms, AVPlayerSeekMode seek_mode)
{
    pthread_mutex_lock(&p->lock);
    assert(p->state == AV_PAUSED && p->seeks < 2 && seek_mode == AV_SEEK_CLOSEST);
    assert(ms == (p->seeks == 0 ? 4200 : 1200));
    ++p->seeks;
    p->pending_seek = ms;
    schedule(p);
    pthread_mutex_unlock(&p->lock);
    return AV_ERR_OK;
}

OH_AVErrCode OH_AVPlayer_GetDuration(OH_AVPlayer *p, int32_t *ms)
{
    pthread_mutex_lock(&p->lock);
    assert(p->state == AV_PREPARED && fcntl(p->fd, F_GETFD) >= 0);
    *ms = mode("bad_duration") || mode("blocked_release") ? 5800 : 6000;
    pthread_mutex_unlock(&p->lock);
    return AV_ERR_OK;
}

OH_AVErrCode OH_AVPlayer_GetCurrentTime(OH_AVPlayer *p, int32_t *ms)
{
    pthread_mutex_lock(&p->lock);
    assert(fcntl(p->fd, F_GETFD) >= 0);
    *ms = position(p);
    pthread_mutex_unlock(&p->lock);
    return AV_ERR_OK;
}

OH_AVErrCode OH_AVPlayer_ReleaseSync(OH_AVPlayer *p)
{
    assert(fcntl(p->fd, F_GETFD) >= 0);
    if (mode("blocked_release")) for (;;) pause();
    pthread_mutex_lock(&p->lock);
    p->closing = true;
    pthread_cond_signal(&p->wake);
    pthread_mutex_unlock(&p->lock);
    assert(pthread_join(p->thread, NULL) == 0);
    state(p, AV_RELEASED);  /* Callback state and fd must survive through ReleaseSync. */
    if (mode("release_error")) p->error(p, 7, "synthetic release error", p->error_user);
    if (mode("release_eos")) event(p, AV_INFO_TYPE_EOS, NULL, 0);
    if (mode("release_seekdone")) event(p, AV_INFO_TYPE_SEEKDONE, OH_PLAYER_SEEK_POSITION, 1200);
    assert(pthread_mutex_destroy(&p->lock) == 0);
    assert(pthread_cond_destroy(&p->wake) == 0);
    free(p);
    puts("MOCK_RELEASE fd_alive=1");
    return mode("release_rc") ? AV_ERR_INVALID_VAL : AV_ERR_OK;
}
'''


def isolated_headers(dst):
    player = 'multimedia/player_framework'
    headers = (
        *(f'{player}/{name}.h' for name in ('avplayer', 'avplayer_base', 'native_avformat', 'native_averrors')),
        'native_window/external_window.h', 'native_window/buffer_handle.h',
        'native_buffer/buffer_common.h', 'ohaudio/native_audiostream_base.h',
        'multimedia/native_audio_channel_layout.h',
    )
    for name in headers:
        (dst / name).parent.mkdir(parents=True, exist_ok=True)
    if (INCLUDE / player / 'avplayer.h').exists():
        for name in headers:
            shutil.copyfile(INCLUDE / name, dst / name)
    else:
        (dst / player / 'avplayer.h').write_text(MOCK_HEADER, encoding='utf-8')


def make_wav(path):
    # 6 s / 48 kHz / mono, with the same 400/600/800 Hz segments as the fixture.
    with wave.open(str(path), 'wb') as audio:
        audio.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        for hz in (400, 600, 800):
            samples = array.array('h', (round(9000 * math.sin(2 * math.pi * hz * i / 48000))
                                        for i in range(96000)))
            if sys.byteorder != 'little':
                samples.byteswap()
            audio.writeframes(samples.tobytes())


@unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
class ProbeHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(tmp.cleanup)
        cls.tmp = tmp_path = Path(tmp.name)
        include = tmp_path / 'include'
        isolated_headers(include)
        mock = tmp_path / 'mock.c'
        mock.write_text(MOCK_C, encoding='utf-8')
        cls.probe = tmp_path / 'native_avplayer_probe_host'
        result = subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                                 '-I', str(include), str(SOURCE), str(mock), '-o', str(cls.probe)],
                                text=True, capture_output=True, timeout=30)
        if result.returncode != 0:
            raise AssertionError(result.stderr)
        cls.wav = tmp_path / 'tones.wav'
        make_wav(cls.wav)

    def run_probe(self, mode='normal', args=None, timeout=12):
        return subprocess.run([self.probe, *(args if args is not None else [self.wav])],
                              env={**os.environ, 'AVPLAYER_MOCK': mode},
                              text=True, capture_output=True, timeout=timeout)

    def assert_failure(self, mode, stage, detail):
        result = self.run_probe(mode)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(f'AVPLAYER_FAIL stage={stage} detail={detail} ', result.stderr)
        self.assertIn('MOCK_RELEASE fd_alive=1', result.stdout)
        self.assertNotIn('AVPLAYER_PROBE_PASS', result.stdout)

    def test_passes_only_after_both_windows_seeks_eos_and_release(self):
        for mode in ('normal', 'completed_first'):
            with self.subTest(mode=mode):
                result = self.run_probe(mode)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('actual_ms=6000', result.stdout)
                self.assertRegex(result.stdout, r'stage=seek_forward from_ms=\d+ target_ms=4200 event_ms=4200 actual_ms=4200 ')
                self.assertIn('stage=seek_backward from_ms=4200 target_ms=1200 event_ms=1200 actual_ms=1200 ', result.stdout)
                for stage in ('play_window', 'pause_window', 'resume_window'):
                    match = re.search(rf'stage={stage} from_ms=(\d+) to_ms=(\d+) elapsed_ms=(\d+)', result.stdout)
                    self.assertIsNotNone(match, result.stdout)
                    start, end, elapsed = map(int, match.groups())
                    if stage == 'pause_window':
                        self.assertGreater(start, 400)
                        self.assertLessEqual(abs(end - start), 80)
                        self.assertGreaterEqual(elapsed, 500)
                    else:
                        self.assertGreaterEqual(end - start, 250)
                        self.assertGreaterEqual(elapsed, 800)
                        if stage == 'resume_window':
                            self.assertGreaterEqual(start, 1200)
                self.assertIn('AVPLAYER_EOS callback=1 completed=1 position_ms=6000', result.stdout)
                self.assertRegex(result.stdout, r'MOCK_RELEASE fd_alive=1\nAVPLAYER_PROBE_PASS evidence=timeline_only\n$')

    def test_demands_fresh_prepared_state_and_valid_duration(self):
        for mode, stage, detail in (
            ('source_rc', 'fd_source', 'sdk'),
            ('noisy_prepare', 'prepare', 'state_timeout'),
            ('callback_error', 'prepare', 'callback_error'),
            ('bad_duration', 'duration', 'outside_fixture_window'),
        ):
            with self.subTest(mode=mode):
                self.assert_failure(mode, stage, detail)

    def test_rejects_frozen_progress_pause_drift_and_restart_from_zero(self):
        for mode, stage, detail in (
            ('stalled_play', 'play_window', 'no_advance'),
            ('single_jump', 'play_window', 'no_advance'),
            ('pause_drift', 'pause_window', 'pause_drift'),
            ('stalled_resume', 'resume_window', 'no_advance'),
            ('resume_restarts', 'resume_window', 'window_origin'),
        ):
            with self.subTest(mode=mode):
                self.assert_failure(mode, stage, detail)

    def test_demands_one_new_seekdone_with_matching_payload_and_actual_time(self):
        for mode, stage, detail in (
            ('preseek', 'prepare', 'unexpected_seekdone'),
            ('malformed_seek', 'seek_forward', 'callback_payload'),
            ('duplicate_seekdone', 'seek_forward', 'unexpected_seekdone'),
            ('missing_seekdone', 'seek_backward', 'seekdone_timeout'),
            ('stale_seekdone', 'seek_backward', 'seekdone_position'),
            ('seek_position', 'seek_backward', 'seek_position'),
        ):
            with self.subTest(mode=mode):
                result = self.run_probe(mode)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                # The first valid completion can race with the duplicate. It must
                # fail at the first/next seek, and may not reach resumed playback.
                stage_re = r'seek_forward|seek_backward' if mode == 'duplicate_seekdone' else stage
                self.assertRegex(result.stderr, rf'AVPLAYER_FAIL stage=(?:{stage_re}) detail={detail} ')
                self.assertIn('MOCK_RELEASE fd_alive=1', result.stdout)
                self.assertNotIn('AVPLAYER_PROBE_PASS', result.stdout)

    def test_demands_real_eos_at_the_end_and_clean_release(self):
        for mode, stage, detail in (
            ('early_eos', 'eos', 'early_eos'),
            ('missing_eos', 'eos', 'eos_timeout'),
            ('missing_completed', 'eos', 'eos_timeout'),
            ('end_position', 'eos', 'eos_position'),
            ('release_rc', 'release', 'sdk'),
            ('release_error', 'release', 'callback_error'),
        ):
            with self.subTest(mode=mode):
                self.assert_failure(mode, stage, detail)

    def test_rejects_duplicate_completion_delivered_during_release(self):
        for mode in ('release_eos', 'release_seekdone'):
            with self.subTest(mode=mode):
                self.assert_failure(mode, 'release', 'extra_completion')

    def test_invalid_paths_fail_before_sdk_and_do_not_block(self):
        empty = self.tmp / 'empty.wav'
        empty.touch()
        directory = self.tmp / 'a.wav'
        directory.mkdir()
        fifo = self.tmp / 'fifo.wav'
        os.mkfifo(fifo)
        for paths in ([], [self.wav, self.wav], ['https://invalid.example/foo.wav'],
                      [self.tmp / 'sample.mp4'], [empty], [directory], [fifo]):
            with self.subTest(paths=paths):
                result = self.run_probe(args=paths, timeout=2)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertNotIn('MOCK_CREATE', result.stdout)

    def test_external_process_timeout_covers_blocked_sdk_teardown(self):
        with self.assertRaises(subprocess.TimeoutExpired) as failure:
            self.run_probe('blocked_release', timeout=2)
        self.assertIn(b'AVPLAYER_STEP stage=release', failure.exception.stdout)
        self.assertNotIn(b'AVPLAYER_PROBE_PASS', failure.exception.stdout)


class LinkOnlyTests(unittest.TestCase):
    @staticmethod
    def readelf(flag, path):
        return subprocess.run(['readelf', '-W', flag, str(path)],
                              env={**os.environ, 'LC_ALL': 'C'}, text=True,
                              capture_output=True, timeout=10, check=True).stdout

    def test_standalone_ndk_cross_link(self):
        subprocess.run(['sh', '-n', BUILD], check=True, timeout=5)
        if not (SDK / 'llvm/bin/clang').is_file() or not shutil.which('readelf'):
            self.skipTest('local OHOS compiler or readelf not present')
        with tempfile.TemporaryDirectory() as out:
            result = subprocess.run(['sh', BUILD],
                                    env={**os.environ, 'HMOS_SDK': str(SDK), 'AVPLAYER_PROBE_OUT': out},
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            target = Path(out) / 'native_avplayer_probe'
            self.assertIn(f'AVPLAYER_PROBE_BUILD_OK out={target}', result.stdout)
            self.assertRegex(self.readelf('-h', target), r'Machine:\s+AArch64')
            dynamic = self.readelf('-d', target)
            for soname in ('libavplayer.so', 'libnative_media_core.so'):
                self.assertIn(f'[{soname}]', dynamic)

    @unittest.skipUnless(DONOR_LIB.is_dir() and shutil.which('readelf'),
                         'local donor libraries or readelf not present')
    def test_donor_exports_used_by_probe(self):
        for name, expected in (('libavplayer.so', PLAYER_EXPORTS),
                               ('libnative_media_core.so', {'OH_AVFormat_GetIntValue'})):
            with self.subTest(name=name):
                exports = set()
                for line in self.readelf('--dyn-syms', DONOR_LIB / name).splitlines():
                    fields = line.split()
                    if len(fields) >= 8 and fields[0][:-1].isdigit() and fields[6] != 'UND':
                        exports.add(fields[7].split('@')[0])
                self.assertFalse(expected - exports, f'missing exports: {expected - exports}')


if __name__ == '__main__':
    unittest.main()
