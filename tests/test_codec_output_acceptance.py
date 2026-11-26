"""Run the real probe entry point with a synthetic NDK callback implementation."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PROBE = ROOT / 'compat' / 'codec' / 'native_codec_probe.c'
SDK_MULTIMEDIA = ROOT / 'work' / 'ohos-sdk' / 'installed' / 'native' / 'sysroot' / 'usr' / 'include' / 'multimedia'
# A fake Annex-B payload used only to check input transport (the mock never decodes it).
ACCESS_UNIT = b'\x00\x00\x00\x01\x67\x11\x00\x00\x01\x65\x22'

MOCK = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "multimedia/player_framework/native_avcapability.h"
#include "multimedia/player_framework/native_avcodec_videodecoder.h"

const char *OH_MD_KEY_WIDTH = "width";
const char *OH_MD_KEY_HEIGHT = "height";
const char *OH_MD_KEY_PIXEL_FORMAT = "pixel-format";
const char *OH_MD_KEY_VIDEO_STRIDE = "stride";

struct OH_AVCapability { int unused; };
struct OH_AVCodec { int unused; };
struct OH_AVFormat { int32_t width, height, stride, pixel; };
struct OH_AVBuffer {
    OH_AVCodecBufferAttr attr;
    uint8_t *addr;
    int32_t capacity;
    bool badAttr;
};

static OH_AVCapability cap;
static OH_AVCodec decoder;
static OH_AVCodecCallback cb;
static void *user;
static const uint8_t au[] = {0, 0, 0, 1, 0x67, 0x11, 0, 0, 1, 0x65, 0x22};
static uint8_t in[2][sizeof(au)];
static OH_AVBuffer inputs[2];
static uint8_t pixels[7 + 96 + 5];
static OH_AVBuffer frame = {.attr = {.offset = 7, .size = 96}, .addr = pixels, .capacity = sizeof(pixels)};
static OH_AVBuffer eos = {.attr = {.flags = AVCODEC_BUFFER_FLAGS_EOS}, .capacity = -1};
static unsigned int pushes;
static atomic_uint releases;
static pthread_t outputThread;
static bool outputThreadStarted;

static bool scenario(const char *name)
{
    const char *selected = getenv("CODEC_MOCK");
    return selected != NULL && strcmp(selected, name) == 0;
}

OH_AVCapability *OH_AVCodec_GetCapability(const char *mime, bool encoder)
{
    return !encoder && strcmp(mime, "video/avc") == 0 ? &cap : NULL;
}

OH_AVCapability *OH_AVCodec_GetCapabilityByCategory(const char *mime, bool encoder, OH_AVCodecCategory category)
{
    return category == SOFTWARE ? OH_AVCodec_GetCapability(mime, encoder) : NULL;
}

const char *OH_AVCapability_GetName(OH_AVCapability *capability)
{
    assert(capability == &cap);
    return "mock.avc";
}

bool OH_AVCapability_IsHardware(OH_AVCapability *capability)
{
    assert(capability == &cap);
    return false;
}

int32_t OH_AVCapability_GetMaxSupportedInstances(OH_AVCapability *capability)
{
    assert(capability == &cap);
    return 1;
}

OH_AVCodec *OH_VideoDecoder_CreateByMime(const char *mime)
{
    assert(strcmp(mime, "video/avc") == 0);
    puts("MOCK_CREATE");
    return &decoder;
}

OH_AVFormat *OH_AVFormat_CreateVideoFormat(const char *mime, int32_t width, int32_t height)
{
    assert(strcmp(mime, "video/avc") == 0);
    OH_AVFormat *format = malloc(sizeof(*format));
    assert(format != NULL);
    *format = (OH_AVFormat){.width = width, .height = height, .stride = width, .pixel = -1};
    return format;
}

void OH_AVFormat_Destroy(OH_AVFormat *format) { free(format); }

bool OH_AVFormat_SetIntValue(OH_AVFormat *format, const char *key, int32_t value)
{
    assert(strcmp(key, OH_MD_KEY_PIXEL_FORMAT) == 0 && value == AV_PIXEL_FORMAT_YUVI420);
    format->pixel = value;
    return !scenario("request_fail");
}

bool OH_AVFormat_GetIntValue(OH_AVFormat *format, const char *key, int32_t *result)
{
    if (strcmp(key, OH_MD_KEY_WIDTH) == 0) *result = format->width;
    else if (strcmp(key, OH_MD_KEY_HEIGHT) == 0) *result = format->height;
    else if (strcmp(key, OH_MD_KEY_VIDEO_STRIDE) == 0) *result = format->stride;
    else if (strcmp(key, OH_MD_KEY_PIXEL_FORMAT) == 0 && format->pixel >= 0) *result = format->pixel;
    else return false;
    return true;
}

OH_AVErrCode OH_VideoDecoder_Configure(OH_AVCodec *codec, OH_AVFormat *format)
{
    assert(codec == &decoder && format->pixel == AV_PIXEL_FORMAT_YUVI420);
    assert(!scenario("request_fail"));
    return AV_ERR_OK;
}

OH_AVErrCode OH_VideoDecoder_RegisterCallback(OH_AVCodec *codec, OH_AVCodecCallback callback, void *data)
{
    assert(codec == &decoder);
    cb = callback;
    user = data;
    return AV_ERR_OK;
}

OH_AVErrCode OH_VideoDecoder_Prepare(OH_AVCodec *codec)
{
    assert(codec == &decoder);
    return scenario("prepare_fail") ? AV_ERR_INVALID_STATE : AV_ERR_OK;
}

static void *offer_second(void *codec)
{
    cb.onNeedInputBuffer(codec, 1, &inputs[1], user);
    return NULL;
}

OH_AVErrCode OH_VideoDecoder_Start(OH_AVCodec *codec)
{
    assert(codec == &decoder);
    puts("MOCK_START");
    if (scenario("start_fail")) return AV_ERR_INVALID_STATE;
    if (cb.onNeedInputBuffer == NULL) return AV_ERR_OK;  // --instance has no callbacks.
    const bool burst = scenario("async_burst");
    for (unsigned int i = 0; i < 2; i++) {
        inputs[i] = (OH_AVBuffer){.addr = in[i], .capacity = sizeof(au)};
    }
    if (scenario("input_small")) inputs[0].capacity--;
    if (scenario("input_unmapped")) inputs[0].addr = NULL;
    for (unsigned int i = 0; i < 96; i++) pixels[i + 7] = (uint8_t)(i * 3 + 1);
    if (scenario("change_tail")) pixels[7 + 95] ^= 1;
    OH_AVFormat stream = {.width = 8, .height = 8, .stride = 8, .pixel = AV_PIXEL_FORMAT_YUVI420};
    if (scenario("bad_geometry")) stream.stride = 4;
    if (scenario("stride_short")) stream.stride = 10;
    if (scenario("wrong_pixel_format")) stream.pixel = AV_PIXEL_FORMAT_NV12;
    if (scenario("unknown_pixel_format")) stream.pixel = -1;
    cb.onStreamChanged(codec, &stream, user);
    if (scenario("no_input_offer")) return AV_ERR_OK;
    pthread_t offerThread;
    if (burst) assert(pthread_create(&offerThread, NULL, offer_second, codec) == 0);
    cb.onNeedInputBuffer(codec, 0, scenario("null_input") ? NULL : &inputs[0], user);
    if (burst) assert(pthread_join(offerThread, NULL) == 0);
    return AV_ERR_OK;
}

int32_t OH_AVBuffer_GetCapacity(OH_AVBuffer *b) { return b->capacity; }
uint8_t *OH_AVBuffer_GetAddr(OH_AVBuffer *b) { return b->addr; }

OH_AVErrCode OH_AVBuffer_SetBufferAttr(OH_AVBuffer *b, const OH_AVCodecBufferAttr *attr)
{
    assert(attr->offset == 0 && attr->pts == 0);
    if (scenario("input_attr_fail")) return AV_ERR_INVALID_VAL;
    b->attr = *attr;
    return AV_ERR_OK;
}

OH_AVErrCode OH_AVBuffer_GetBufferAttr(OH_AVBuffer *b, OH_AVCodecBufferAttr *attr)
{
    if (b->badAttr) return AV_ERR_INVALID_VAL;
    *attr = b->attr;
    return AV_ERR_OK;
}

static void output_picture(OH_AVCodec *codec)
{
    if (scenario("bad_attr")) frame.badAttr = true;
    if (scenario("negative_size")) frame.attr.size = -1;
    if (scenario("negative_offset")) frame.attr.offset = -1;
    if (scenario("out_of_bounds")) frame.attr.offset = 13;
    if (scenario("negative_capacity")) frame.capacity = -1;
    if (scenario("unmapped")) frame.addr = NULL;
    if (scenario("incomplete")) frame.attr.flags |= AVCODEC_BUFFER_FLAGS_INCOMPLETE_FRAME;
    if (scenario("codec_data")) frame.attr.flags |= AVCODEC_BUFFER_FLAGS_CODEC_DATA;
    if (scenario("discard")) frame.attr.flags |= AVCODEC_BUFFER_FLAGS_DISCARD;
    if (scenario("short")) frame.attr.size--;
    cb.onNewOutputBuffer(codec, 10, scenario("null_output") ? NULL : &frame, user);
    if (scenario("extra_picture")) cb.onNewOutputBuffer(codec, 12, &frame, user);
}

static void *output_after_eos(void *codec)
{
    if (scenario("eos_with_picture")) {
        frame.attr.flags |= AVCODEC_BUFFER_FLAGS_EOS;
        output_picture(codec);
    } else {
        if (scenario("async_burst")) output_picture(codec);
        if (scenario("empty_eos_bad_offset")) eos.attr.offset = 1;
        cb.onNewOutputBuffer(codec, 11, &eos, user);
        if (scenario("post_eos_output")) cb.onNewOutputBuffer(codec, 12, &frame, user);
    }
    return NULL;
}

OH_AVErrCode OH_VideoDecoder_PushInputBuffer(OH_AVCodec *codec, uint32_t index)
{
    assert(codec == &decoder && index < 2 && pushes < 2);
    const OH_AVBuffer *b = &inputs[index];
    pushes++;
    if (pushes == 1) {
        assert(b->attr.size == (int32_t)sizeof(au) && b->attr.flags == 0);
        assert(memcmp(b->addr, au, sizeof(au)) == 0);
        if (scenario("input_push_fail")) return AV_ERR_INVALID_STATE;
        if (!scenario("async_burst") && !scenario("no_picture") && !scenario("eos_with_picture")) {
            output_picture(codec);
        }
        if (scenario("early_eos")) cb.onNewOutputBuffer(codec, 11, &eos, user);
        /* Exercise reentrant callbacks and reuse of one index after Push. */
        if (!scenario("async_burst")) cb.onNeedInputBuffer(codec, index, &inputs[index], user);
    } else {
        assert(b->attr.size == 0 && b->attr.flags == AVCODEC_BUFFER_FLAGS_EOS);
        if (scenario("async_burst")) {
            assert(pthread_create(&outputThread, NULL, output_after_eos, codec) == 0);
            outputThreadStarted = true;
        } else if (!scenario("missing_eos")) {
            output_after_eos(codec);
        }
        if (scenario("eos_push_fail")) return AV_ERR_INVALID_STATE;
    }
    return AV_ERR_OK;
}

OH_AVErrCode OH_VideoDecoder_FreeOutputBuffer(OH_AVCodec *codec, uint32_t index)
{
    assert(codec == &decoder && index >= 10 && index <= 12);
    if (scenario("async_burst") && index == 11) {
        // The main thread must not call Stop merely because EOS attributes were read.
        struct timespec pause = {.tv_nsec = 40000000L};
        nanosleep(&pause, NULL);
    }
    atomic_fetch_add(&releases, 1);
    if (scenario("callback_error")) cb.onError(codec, AV_ERR_SERVICE_DIED, user);
    if (scenario("late_geometry")) {
        OH_AVFormat stream = {.width = 8, .height = 8, .stride = 10, .pixel = AV_PIXEL_FORMAT_YUVI420};
        cb.onStreamChanged(codec, &stream, user);
    }
    if (scenario("free_fail") || (index == 11 && scenario("eos_free_fail"))) return AV_ERR_INVALID_STATE;
    return AV_ERR_OK;
}

OH_AVErrCode OH_VideoDecoder_Stop(OH_AVCodec *codec)
{
    assert(codec == &decoder);
    if (scenario("async_burst")) assert(atomic_load(&releases) == 2);
    printf("MOCK_STOP pushes=%u frees=%u\n", pushes, atomic_load(&releases));
    return scenario("stop_fail") ? AV_ERR_SERVICE_DIED : AV_ERR_OK;
}

OH_AVErrCode OH_VideoDecoder_Destroy(OH_AVCodec *codec)
{
    assert(codec == &decoder);
    if (outputThreadStarted) assert(pthread_join(outputThread, NULL) == 0);
    if (scenario("late_error")) cb.onError(codec, AV_ERR_SERVICE_DIED, user);
    puts("MOCK_DESTROY");
    return scenario("destroy_fail") ? AV_ERR_SERVICE_DIED : AV_ERR_OK;
}
'''


def payload_hash(payload):
    value = 1469598103934665603
    for byte in payload:
        value = ((value * 1099511628211) ^ byte) & ((1 << 64) - 1)
    return f'{value:016x}'


class CodecOutputAcceptanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc')
        if not cc or not (SDK_MULTIMEDIA / 'player_framework' / 'native_avbuffer.h').is_file():
            raise unittest.SkipTest('host C compiler and local OHOS SDK headers required')
        scratch = tempfile.TemporaryDirectory()
        cls.addClassCleanup(scratch.cleanup)
        tmp = pathlib.Path(scratch.name)
        # Only use SDK multimedia headers; the executable uses host libc/pthreads.
        (tmp / 'multimedia').symlink_to(SDK_MULTIMEDIA, target_is_directory=True)
        mock = tmp / 'mock.c'
        mock.write_text(MOCK, encoding='utf-8')
        cls.sample = tmp / 'fixture.h264'
        cls.sample.write_bytes(ACCESS_UNIT)
        cls.container = tmp / 'fixture.mp4'
        cls.container.write_bytes(b'\x00\x00\x00\x18ftypisom' + bytes(12))
        cls.binary = tmp / 'probe'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DPROBE_DECODE_TIMEOUT_MS=500',
                        '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(tmp),
                        str(PROBE), str(mock), '-o', str(cls.binary)], check=True, timeout=30)

    def run_probe(self, scenario, *args):
        return subprocess.run([str(self.binary), *args], env={**os.environ, 'CODEC_MOCK': scenario},
                              capture_output=True, text=True, timeout=5)

    def decode(self, scenario, status=0):
        result = self.run_probe(scenario, '--decode', 'video/avc', str(self.sample), '8', '8')
        self.assertEqual(result.returncode, status, result.stdout + result.stderr)
        lines = [line for line in result.stdout.splitlines() if line.startswith('CODEC_DECODE ')]
        self.assertEqual(len(lines), 1, result.stdout)
        return dict(field.split('=', 1) for field in lines[0].split()[1:]), result.stdout

    def test_single_access_unit_full_payload_hash_and_eos(self):
        pixels = bytes((i * 3 + 1) % 256 for i in range(96))
        for scenario in ('ok', 'async_burst', 'eos_with_picture', 'unknown_pixel_format', 'change_tail'):
            with self.subTest(scenario=scenario):
                fields, output = self.decode(scenario)
                expected = pixels if scenario != 'change_tail' else pixels[:-1] + bytes([pixels[-1] ^ 1])
                self.assertEqual(fields['hash'], payload_hash(expected))
                self.assertEqual(fields['frames'], '1')
                self.assertEqual(fields['bytes'], '96')
                self.assertEqual(fields['size'], str(len(ACCESS_UNIT)))
                self.assertEqual(fields['input_eos'], '1')
                self.assertEqual(fields['stage'], 'ok')
                self.assertEqual(fields['errors'], '0')
                self.assertEqual(fields['pixel_format'], '-1' if scenario == 'unknown_pixel_format' else '1')
                self.assertIn('CODEC_OUTPUT_EOS=1', output)
                self.assertIn(f'MOCK_STOP pushes=2 frees={1 if scenario == "eos_with_picture" else 2}', output)
                self.assertIn('MOCK_DESTROY', output)

    def test_invalid_output_buffers_are_released_without_counting_pixels(self):
        failures = {
            'null_output': 'output_buffer', 'bad_attr': 'output_attr', 'unmapped': 'output_bounds',
            'negative_size': 'output_bounds', 'negative_offset': 'output_bounds',
            'negative_capacity': 'output_bounds', 'out_of_bounds': 'output_bounds',
            'short': 'output_size', 'stride_short': 'output_size', 'late_geometry': 'output_size',
            'incomplete': 'output_incomplete', 'free_fail': 'output_free', 'callback_error': 'codec_error',
        }
        for scenario, stage in failures.items():
            with self.subTest(scenario=scenario):
                fields, output = self.decode(scenario, 2)
                self.assertEqual((fields['stage'], fields['frames'], fields['bytes']), (stage, '0', '0'))
                self.assertNotEqual(fields['errors'], '0')
                self.assertIn('MOCK_STOP pushes=1 frees=1', output)
                self.assertIn('MOCK_DESTROY', output)

    def test_format_and_picture_acceptance(self):
        for scenario, stage, frames in (
            ('bad_geometry', 'output_geometry', '0'), ('wrong_pixel_format', 'output_pixel_format', '0'),
            ('no_picture', 'no_picture', '0'), ('codec_data', 'no_picture', '0'),
            ('discard', 'no_picture', '0'), ('extra_picture', 'unexpected_frames', '2'),
        ):
            with self.subTest(scenario=scenario):
                fields, output = self.decode(scenario, 2)
                self.assertEqual((fields['stage'], fields['frames']), (stage, frames))
                self.assertIn('MOCK_DESTROY', output)

    def test_capacity_is_a_limit_and_input_failures_do_not_submit_eos(self):
        for scenario, stage, pushes in (
            ('input_small', 'input_capacity', 0), ('input_unmapped', 'input_address', 0),
            ('input_attr_fail', 'input_attr', 0), ('null_input', 'input_buffer', 0),
            ('input_push_fail', 'input_push', 1), ('no_input_offer', 'timeout', 0),
        ):
            with self.subTest(scenario=scenario):
                fields, output = self.decode(scenario, 2)
                self.assertEqual((fields['stage'], fields['input_eos']), (stage, '0'))
                self.assertIn(f'MOCK_STOP pushes={pushes} frees=0', output)
                self.assertIn('CODEC_OUTPUT_EOS=0', output)

    def test_both_eos_and_teardown_are_required_even_with_valid_pixels(self):
        for scenario, stage, input_eos, output_eos in (
            ('early_eos', 'early_output_eos', '0', 0), ('missing_eos', 'timeout', '1', 0),
            ('eos_push_fail', 'input_eos', '0', 1), ('eos_free_fail', 'output_free', '1', 0),
            ('empty_eos_bad_offset', 'output_bounds', '1', 0),
            ('post_eos_output', 'after_output_eos', '1', 1),
            ('stop_fail', 'stop', '1', 1), ('destroy_fail', 'destroy', '1', 1),
            ('late_error', 'codec_error', '1', 1),
        ):
            with self.subTest(scenario=scenario):
                fields, output = self.decode(scenario, 2)
                self.assertEqual((fields['stage'], fields['frames'], fields['input_eos']), (stage, '1', input_eos))
                self.assertNotEqual(fields['errors'], '0')
                self.assertIn(f'CODEC_OUTPUT_EOS={output_eos}', output)
                self.assertEqual(fields['stop'] == '0', scenario != 'stop_fail')
                self.assertEqual(fields['destroy'] == '0', scenario != 'destroy_fail')
                self.assertIn('MOCK_DESTROY', output)

    def test_early_failures_destroy_without_stopping_an_unstarted_decoder(self):
        for scenario, stage in (('request_fail', 'pixel_format_request'), ('prepare_fail', 'prepare'),
                                ('start_fail', 'start')):
            with self.subTest(scenario=scenario):
                fields, output = self.decode(scenario, 2)
                self.assertEqual((fields['stage'], fields['stop'], fields['destroy']), (stage, '-1', '0'))
                self.assertNotIn('MOCK_STOP', output)
                self.assertIn('MOCK_DESTROY', output)

    def test_console_modes_preserve_enumeration_and_instance_behavior(self):
        enum = self.run_probe('ok')
        self.assertEqual(enum.returncode, 0, enum.stderr)
        self.assertIn('CODEC_PROBE name=mock.avc mime=video/avc type=sw', enum.stdout)
        self.assertNotIn('MOCK_CREATE', enum.stdout)
        for scenario, status in (('ok', 0), ('start_fail', 2), ('stop_fail', 2), ('destroy_fail', 2)):
            with self.subTest(scenario=scenario):
                inst = self.run_probe(scenario, '--instance', 'video/avc', '8', '8')
                self.assertEqual(inst.returncode, status, inst.stdout + inst.stderr)
                self.assertIn('CODEC_INSTANCE mime=video/avc', inst.stdout)
                self.assertIn('MOCK_DESTROY', inst.stdout)

    def test_obvious_container_is_not_submitted_to_decoder(self):
        result = self.run_probe('ok', '--decode', 'video/avc', str(self.container), '8', '8')
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn('caller-framed Annex-B access unit', result.stderr)
        self.assertNotIn('MOCK_CREATE', result.stdout)
