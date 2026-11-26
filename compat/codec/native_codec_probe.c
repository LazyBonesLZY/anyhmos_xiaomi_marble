// SPDX-License-Identifier: MIT
/*
 * marble codec capability probe (OHOS AVCodec native API, aarch64-linux-ohos).
 *
 * Purpose: separate "the SA process is running" from "a codec really exists".
 *   - default      : enumerate capabilities only, never creates a codec instance.
 *   - --instance   : create/configure/prepare/start/stop/destroy a decoder; this
 *                    actually reaches av_codec_service (SA 3011) and, for every
 *                    HCodec/video capability, the codec HDI 4.0 service
 *                    (codec_hdi_omx) inside the HDF codec_host.
 *   - --decode     : buffer-mode H.264/H.265 decode of one caller-framed Annex-B
 *                    access unit (with any needed parameter sets). The unit
 *                    must fit one input buffer; this is not a demuxer or AU
 *                    parser. Requests I420, no surface. Success requires one
 *                    readable picture, input/output EOS and clean teardown.
 *                    The hash covers the first picture's full buffer payload
 *                    (including any padding), not a normalized image.
 *
 * Output lines are stable KEY=VALUE for parsing:
 *   CODEC_PROBE name=<codec> mime=<mime> type=sw|hw kind=decode|encode ...
 *   CODEC_PROBE_SUMMARY ...
 *   CODEC_INSTANCE mime=... create=0 configure=0 prepare=0 start=0 ...
 *   CODEC_DECODE mime=... frames=... errors=...
 *   CODEC_ERROR code=<int>
 *
 * Exit codes: 0 = at least one capability found (or requested action succeeded),
 *             2 = no capability found / action failed, 3 = usage error.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "multimedia/player_framework/native_avbuffer.h"
#include "multimedia/player_framework/native_avcapability.h"
#include "multimedia/player_framework/native_avcodec_base.h"
#include "multimedia/player_framework/native_avcodec_videodecoder.h"
#include "multimedia/player_framework/native_avformat.h"
#include "multimedia/player_framework/native_averrors.h"

#ifndef PROBE_DECODE_TIMEOUT_MS
#define PROBE_DECODE_TIMEOUT_MS 10000
#endif
#define PROBE_HASH_SEED UINT64_C(1469598103934665603)
#define PROBE_HASH_PRIME UINT64_C(1099511628211)
#define PROBE_INPUT_BUFFERS 2 /* One access unit followed by an empty EOS. */

typedef struct {
    const char *mime;
    int isEncoder;
    const char *kind;
} ProbeMime;

/* Kept deliberately static: OH_AVCodec_GetCapabilityList is newer than some SDK
 * headers, and a fixed table keeps the output stable across API levels. */
static const ProbeMime PROBE_MIMES[] = {
    {"video/avc", 0, "decode"},
    {"video/hevc", 0, "decode"},
    {"video/x-vnd.on2.vp8", 0, "decode"},
    {"video/x-vnd.on2.vp9", 0, "decode"},
    {"video/av01", 0, "decode"},
    {"video/mp4v-es", 0, "decode"},
    {"video/3gpp", 0, "decode"},
    {"video/mpeg2", 0, "decode"},
    {"audio/mp4a-latm", 0, "decode"},
    {"audio/mpeg", 0, "decode"},
    {"audio/flac", 0, "decode"},
    {"audio/vorbis", 0, "decode"},
    {"audio/opus", 0, "decode"},
    {"audio/raw", 0, "decode"},
    {"video/avc", 1, "encode"},
    {"video/hevc", 1, "encode"},
    {"video/mp4v-es", 1, "encode"},
    {"audio/mp4a-latm", 1, "encode"},
    {"audio/opus", 1, "encode"},
};

static int probe_report(const ProbeMime *entry, OH_AVCodecCategory category, int *found)
{
    OH_AVCapability *cap = OH_AVCodec_GetCapabilityByCategory(entry->mime, entry->isEncoder != 0, category);
    if (cap == NULL) {
        return 0;
    }
    const char *name = OH_AVCapability_GetName(cap);
    const char *type = OH_AVCapability_IsHardware(cap) ? "hw" : "sw";
    int32_t instances = OH_AVCapability_GetMaxSupportedInstances(cap);
    printf("CODEC_PROBE name=%s mime=%s type=%s kind=%s instances=%d category=%s\n",
        (name != NULL) ? name : "?", entry->mime, type, entry->kind, instances,
        (category == HARDWARE) ? "hw" : "sw");
    *found += 1;
    return 1;
}

static int probe_enumerate(void)
{
    int total = 0;
    int hw = 0;
    int sw = 0;
    int decode = 0;
    int encode = 0;
    size_t count = sizeof(PROBE_MIMES) / sizeof(PROBE_MIMES[0]);
    for (size_t i = 0; i < count; i++) {
        int before = total;
        probe_report(&PROBE_MIMES[i], HARDWARE, &total);
        probe_report(&PROBE_MIMES[i], SOFTWARE, &total);
        if (total == before) {
            OH_AVCapability *cap = OH_AVCodec_GetCapability(PROBE_MIMES[i].mime, PROBE_MIMES[i].isEncoder != 0);
            if (cap != NULL) {
                printf("CODEC_PROBE name=%s mime=%s type=%s kind=%s instances=%d category=default\n",
                    (OH_AVCapability_GetName(cap) != NULL) ? OH_AVCapability_GetName(cap) : "?",
                    PROBE_MIMES[i].mime,
                    OH_AVCapability_IsHardware(cap) ? "hw" : "sw",
                    PROBE_MIMES[i].kind,
                    OH_AVCapability_GetMaxSupportedInstances(cap));
                total += 1;
            }
        }
    }
    /* Recount by category from the printed table for the summary. */
    for (size_t i = 0; i < count; i++) {
        if (OH_AVCodec_GetCapabilityByCategory(PROBE_MIMES[i].mime, PROBE_MIMES[i].isEncoder != 0, HARDWARE) != NULL) {
            hw += 1;
        }
        if (OH_AVCodec_GetCapabilityByCategory(PROBE_MIMES[i].mime, PROBE_MIMES[i].isEncoder != 0, SOFTWARE) != NULL) {
            sw += 1;
        }
        if (PROBE_MIMES[i].isEncoder == 0 && OH_AVCodec_GetCapability(PROBE_MIMES[i].mime, false) != NULL) {
            decode += 1;
        }
        if (PROBE_MIMES[i].isEncoder != 0 && OH_AVCodec_GetCapability(PROBE_MIMES[i].mime, true) != NULL) {
            encode += 1;
        }
    }
    printf("CODEC_PROBE_SUMMARY entries=%d hw=%d sw=%d decode=%d encode=%d\n", total, hw, sw, decode, encode);
    return (total > 0) ? 0 : 2;
}

typedef struct {
    uint32_t index;
    OH_AVBuffer *buffer;
} ProbeInput;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    ProbeInput input[PROBE_INPUT_BUFFERS];
    unsigned int inputCount;
    unsigned int activeOutputs;
    bool payloadSubmitting;
    bool eosSubmitting;
    bool outputEos;
    bool closing;
    bool finished;
    int32_t lastError;
    const char *failureStage;
    uint64_t frames;
    uint64_t bytes;
    uint64_t hash;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t pixelFormat;
} ProbeDecode;

/* One CLI decode per process. Keep callback userData and its synchronization
 * objects alive even on failed Stop/Destroy. Once registered, mutable state is
 * protected by mutex; no decoder call is made with it held. */
static ProbeDecode decodeCtx = {.mutex = PTHREAD_MUTEX_INITIALIZER};

static void probe_decode_error_locked(ProbeDecode *ctx, const char *stage, int32_t code)
{
    if (!ctx->finished && ctx->lastError == 0) {
        ctx->lastError = code != 0 ? code : -1;
        ctx->failureStage = stage;
        pthread_cond_signal(&ctx->changed);
    }
}

static void probe_decode_error(ProbeDecode *ctx, const char *stage, int32_t code)
{
    pthread_mutex_lock(&ctx->mutex);
    probe_decode_error_locked(ctx, stage, code);
    pthread_mutex_unlock(&ctx->mutex);
}

static void probe_on_error(OH_AVCodec *codec, int32_t errorCode, void *userData)
{
    (void)codec;
    probe_decode_error(userData, "codec_error", errorCode);
    printf("CODEC_ERROR code=%d\n", errorCode);
    fflush(stdout);
}

static void probe_on_stream_changed(OH_AVCodec *codec, OH_AVFormat *format, void *userData)
{
    (void)codec;
    ProbeDecode *ctx = userData;
    pthread_mutex_lock(&ctx->mutex);
    if (ctx->closing) {
        pthread_mutex_unlock(&ctx->mutex);
        return;
    }
    /* Snapshot metadata from the callback-owned format; retain no pointer.
     * Width/height fall back to the configuration if not reported. */
    int32_t width = 0, height = 0, stride = 0, pixelFormat = -1;
    if (format == NULL || !OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &width)) width = ctx->width;
    if (format == NULL || !OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &height)) height = ctx->height;
    bool haveStride = format != NULL && OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_STRIDE, &stride);
    bool havePixelFormat = format != NULL && OH_AVFormat_GetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, &pixelFormat);
    if (!haveStride) stride = 0;
    if (!havePixelFormat) pixelFormat = -1;
    ctx->width = width;
    ctx->height = height;
    ctx->stride = stride;
    ctx->pixelFormat = pixelFormat;
    if (format == NULL || width <= 0 || height <= 0 || (haveStride && stride < width)) {
        probe_decode_error_locked(ctx, "output_geometry", -1);
    } else if (havePixelFormat && pixelFormat != AV_PIXEL_FORMAT_YUVI420) {
        probe_decode_error_locked(ctx, "output_pixel_format", -1);
    }
    pthread_mutex_unlock(&ctx->mutex);
    printf("CODEC_STREAM width=%d height=%d stride=%d pixel_format=%d\n", width, height, stride, pixelFormat);
    fflush(stdout);
}

static void probe_on_need_input(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
{
    (void)codec;
    ProbeDecode *ctx = userData;
    pthread_mutex_lock(&ctx->mutex);
    /* Keep up to two offers, regardless of whether callbacks are serial or
     * overlap. Only the CLI thread will push them, in payload then EOS order. */
    if (!ctx->closing && ctx->lastError == 0 && ctx->inputCount < PROBE_INPUT_BUFFERS) {
        if (buffer == NULL) {
            probe_decode_error_locked(ctx, "input_buffer", -1);
        } else {
            ctx->input[ctx->inputCount++] = (ProbeInput){.index = index, .buffer = buffer};
            pthread_cond_signal(&ctx->changed);
        }
    }
    pthread_mutex_unlock(&ctx->mutex);
}

static uint64_t probe_i420_min_bytes(int32_t width, int32_t height, int32_t stride)
{
    /* Use the reported Y stride, if any. The SDK does not promise tight
     * chroma strides or a tight slice height, so this is only a lower bound. */
    uint64_t w = (uint64_t)width;
    uint64_t h = (uint64_t)height;
    uint64_t yStride = (uint64_t)(stride > width ? stride : width);
    return yStride * h + 2 * ((w + 1) / 2) * ((h + 1) / 2);
}

static void probe_on_new_output(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
{
    ProbeDecode *ctx = userData;
    pthread_mutex_lock(&ctx->mutex);
    if (ctx->closing) {
        /* Stop/Destroy owns reclamation once shutdown starts. */
        if (ctx->outputEos) probe_decode_error_locked(ctx, "after_output_eos", -1);
        pthread_mutex_unlock(&ctx->mutex);
        return;
    }
    ctx->activeOutputs++;
    bool payloadExpected = ctx->payloadSubmitting;
    bool eosExpected = ctx->eosSubmitting;
    bool afterEos = ctx->outputEos;
    pthread_mutex_unlock(&ctx->mutex);

    OH_AVCodecBufferAttr attr = {0};
    const char *stage = NULL;
    int32_t error = -1;
    bool picture = false;
    uint64_t hash = PROBE_HASH_SEED;
    if (buffer == NULL) {
        stage = "output_buffer";
    } else if ((error = OH_AVBuffer_GetBufferAttr(buffer, &attr)) != AV_ERR_OK) {
        stage = "output_attr";
    } else if (attr.size < 0 || attr.offset < 0 ||
        (attr.size == 0 && attr.offset > 0 && attr.offset > OH_AVBuffer_GetCapacity(buffer))) {
        stage = "output_bounds";
    } else if (afterEos) {
        stage = "after_output_eos";
    } else if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) && !eosExpected) {
        stage = "early_output_eos";
    } else if (attr.size > 0 && !payloadExpected) {
        stage = "early_output";
    } else if (attr.flags & AVCODEC_BUFFER_FLAGS_INCOMPLETE_FRAME) {
        stage = "output_incomplete";
    } else if (attr.size > 0) {
        const uint8_t *addr = OH_AVBuffer_GetAddr(buffer);
        int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
        if (addr == NULL || capacity < attr.offset || attr.size > capacity - attr.offset) {
            stage = "output_bounds";
        } else if (!(attr.flags & (AVCODEC_BUFFER_FLAGS_CODEC_DATA | AVCODEC_BUFFER_FLAGS_DISCARD))) {
            addr += attr.offset;
            for (int32_t i = 0; i < attr.size; i++) {
                hash = (hash * PROBE_HASH_PRIME) ^ addr[i];
            }
            picture = true;
        }
    }
    /* EOS may carry the final picture or be empty and unmapped. Release every
     * admitted index, including malformed and EOS buffers, before publishing. */
    int releaseRc = OH_VideoDecoder_FreeOutputBuffer(codec, index);
    pthread_mutex_lock(&ctx->mutex);
    if (stage != NULL) {
        probe_decode_error_locked(ctx, stage, error);
    } else if (releaseRc != AV_ERR_OK) {
        probe_decode_error_locked(ctx, "output_free", releaseRc);
    } else if (picture && ctx->lastError == 0 &&
        (uint64_t)attr.size < probe_i420_min_bytes(ctx->width, ctx->height, ctx->stride)) {
        probe_decode_error_locked(ctx, "output_size", -1);
    } else if (ctx->lastError == 0) {
        if (picture) {
            if (ctx->frames == 0) ctx->hash = hash;
            ctx->frames++;
            ctx->bytes += (uint64_t)attr.size;
        }
        if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
            if (ctx->outputEos) {
                probe_decode_error_locked(ctx, "duplicate_output_eos", -1);
            }
            ctx->outputEos = true;
        }
    }
    ctx->activeOutputs--;
    pthread_cond_signal(&ctx->changed);
    pthread_mutex_unlock(&ctx->mutex);
}

static int probe_init_decode_wait(ProbeDecode *ctx)
{
    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    if (rc != 0) return rc;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) rc = pthread_cond_init(&ctx->changed, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

static void probe_decode_deadline_error(ProbeDecode *ctx, int rc)
{
    probe_decode_error_locked(ctx, rc == ETIMEDOUT ? "timeout" : "wait", -rc);
}

static bool probe_send_input(OH_AVCodec *codec, ProbeDecode *ctx, unsigned int slot,
    const uint8_t *data, size_t size, const struct timespec *deadline)
{
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->lastError == 0 && ctx->inputCount <= slot) {
        int rc = pthread_cond_timedwait(&ctx->changed, &ctx->mutex, deadline);
        if (rc != 0 && ctx->inputCount <= slot) probe_decode_deadline_error(ctx, rc);
    }
    if (ctx->lastError != 0) {
        pthread_mutex_unlock(&ctx->mutex);
        return false;
    }
    ProbeInput input = ctx->input[slot];
    pthread_mutex_unlock(&ctx->mutex);

    /* One pre-framed AU at PTS 0; capacity is a limit, never a packet boundary. */
    OH_AVCodecBufferAttr attr = {0};
    if (size != 0) {
        int32_t capacity = OH_AVBuffer_GetCapacity(input.buffer);
        if (capacity <= 0 || size > (size_t)capacity) {
            probe_decode_error(ctx, "input_capacity", -1);
            return false;
        }
        uint8_t *addr = OH_AVBuffer_GetAddr(input.buffer);
        if (addr == NULL) {
            probe_decode_error(ctx, "input_address", -1);
            return false;
        }
        memcpy(addr, data, size);
        attr.size = (int32_t)size;
    } else {
        attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
    }
    int rc = OH_AVBuffer_SetBufferAttr(input.buffer, &attr);
    if (rc != AV_ERR_OK) {
        probe_decode_error(ctx, "input_attr", rc);
        return false;
    }
    pthread_mutex_lock(&ctx->mutex);
    bool ready = ctx->lastError == 0;
    if (ready) {
        if (size == 0) {
            ctx->eosSubmitting = true;
        } else {
            ctx->payloadSubmitting = true;
        }
    }
    pthread_mutex_unlock(&ctx->mutex);
    if (!ready) return false;

    rc = OH_VideoDecoder_PushInputBuffer(codec, input.index);
    if (rc != AV_ERR_OK) {
        probe_decode_error(ctx, size == 0 ? "input_eos" : "input_push", rc);
        return false;
    }
    return true;
}

static void probe_finish_decode(ProbeDecode *ctx, bool inputEos, const struct timespec *deadline)
{
    pthread_mutex_lock(&ctx->mutex);
    while (inputEos && ctx->lastError == 0 && (!ctx->outputEos || ctx->activeOutputs != 0)) {
        int rc = pthread_cond_timedwait(&ctx->changed, &ctx->mutex, deadline);
        if (rc != 0 && (!ctx->outputEos || ctx->activeOutputs != 0)) probe_decode_deadline_error(ctx, rc);
    }
    ctx->closing = true;
    /* Do not invalidate an output index while a callback is reading/freeing it.
     * A stuck NDK call needs an external process timeout; the deadline above
     * only bounds waiting for offers/EOS, not SDK calls or teardown. */
    while (ctx->activeOutputs != 0) pthread_cond_wait(&ctx->changed, &ctx->mutex);
    pthread_mutex_unlock(&ctx->mutex);
}

static uint8_t *probe_read_file(const char *path, size_t *sizeOut)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long length = ftell(fp);
    if (length <= 0 || length > INT32_MAX) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)length);
    if (data == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, fp) != (size_t)length) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *sizeOut = (size_t)length;
    return data;
}

static int probe_instance(const char *mime, int width, int height)
{
    OH_AVCodec *codec = OH_VideoDecoder_CreateByMime(mime);
    int createRc = (codec != NULL) ? 0 : -1;
    int configureRc = -1;
    int prepareRc = -1;
    int startRc = -1;
    int stopRc = -1;
    int destroyRc = -1;
    if (codec != NULL) {
        OH_AVFormat *format = OH_AVFormat_CreateVideoFormat(mime, width, height);
        if (format != NULL) {
            OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_YUVI420);
            configureRc = (int)OH_VideoDecoder_Configure(codec, format);
            OH_AVFormat_Destroy(format);
            if (configureRc == (int)AV_ERR_OK) {
                prepareRc = (int)OH_VideoDecoder_Prepare(codec);
            }
            if (prepareRc == (int)AV_ERR_OK) {
                startRc = (int)OH_VideoDecoder_Start(codec);
            }
            if (startRc == (int)AV_ERR_OK) {
                stopRc = (int)OH_VideoDecoder_Stop(codec);
            }
        }
        destroyRc = (int)OH_VideoDecoder_Destroy(codec);
    }
    printf("CODEC_INSTANCE mime=%s width=%d height=%d create=%d configure=%d prepare=%d start=%d stop=%d destroy=%d\n",
        mime, width, height, createRc, configureRc, prepareRc, startRc, stopRc, destroyRc);
    return (createRc == 0 && configureRc == (int)AV_ERR_OK && prepareRc == (int)AV_ERR_OK &&
        startRc == (int)AV_ERR_OK && stopRc == (int)AV_ERR_OK && destroyRc == (int)AV_ERR_OK) ? 0 : 2;
}

static bool probe_has_annex_b_prefix(const uint8_t *data, size_t size)
{
    size_t i = 0;
    while (i < size && data[i] == 0) i++;
    return i >= 2 && i + 1 < size && data[i] == 1;
}

static int probe_decode(const char *mime, const char *path, int width, int height)
{
    if ((strcmp(mime, "video/avc") != 0 && strcmp(mime, "video/hevc") != 0) || width <= 0 || height <= 0) {
        fprintf(stderr, "probe: --decode requires video/avc or video/hevc and positive dimensions\n");
        return 3;
    }
    size_t size = 0;
    uint8_t *data = probe_read_file(path, &size);
    if (data == NULL) {
        fprintf(stderr, "probe: cannot read a non-empty access unit (up to %d bytes) from %s\n", INT32_MAX, path);
        return 3;
    }
    if (!probe_has_annex_b_prefix(data, size)) {
        fprintf(stderr, "probe: expected one caller-framed Annex-B access unit, not a container or byte chunks\n");
        free(data);
        return 3;
    }
    ProbeDecode *ctx = &decodeCtx;
    int rc = probe_init_decode_wait(ctx);
    if (rc != 0) {
        printf("CODEC_DECODE mime=%s frames=0 errors=%d stage=sync_init\n", mime, -rc);
        free(data);
        return 2;
    }
    ctx->width = width;
    ctx->height = height;
    ctx->pixelFormat = -1; /* Unknown unless reported by the decoder. */
    ctx->hash = PROBE_HASH_SEED;
    const OH_AVCodecCallback callback = {
        .onError = probe_on_error,
        .onStreamChanged = probe_on_stream_changed,
        .onNeedInputBuffer = probe_on_need_input,
        .onNewOutputBuffer = probe_on_new_output,
    };

    bool started = false, inputEos = false;
    int stopRc = -1, destroyRc = -1;
    struct timespec deadline = {0};
    OH_AVCodec *codec = OH_VideoDecoder_CreateByMime(mime);
    if (codec == NULL) {
        probe_decode_error(ctx, "create", -1);
        goto cleanup;
    }
    OH_AVFormat *format = OH_AVFormat_CreateVideoFormat(mime, width, height);
    if (format == NULL) {
        probe_decode_error(ctx, "format", -1);
        goto cleanup;
    }
    if (!OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_YUVI420)) {
        OH_AVFormat_Destroy(format);
        probe_decode_error(ctx, "pixel_format_request", -1);
        goto cleanup;
    }
    rc = OH_VideoDecoder_Configure(codec, format);
    OH_AVFormat_Destroy(format);
    if (rc != AV_ERR_OK) {
        probe_decode_error(ctx, "configure", rc);
        goto cleanup;
    }
    rc = OH_VideoDecoder_RegisterCallback(codec, callback, ctx);
    if (rc != AV_ERR_OK) {
        probe_decode_error(ctx, "callback", rc);
        goto cleanup;
    }
    rc = OH_VideoDecoder_Prepare(codec);
    if (rc != AV_ERR_OK) {
        probe_decode_error(ctx, "prepare", rc);
        goto cleanup;
    }
    rc = OH_VideoDecoder_Start(codec);
    if (rc != AV_ERR_OK) {
        probe_decode_error(ctx, "start", rc);
        goto cleanup;
    }
    started = true;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        probe_decode_error(ctx, "clock", -errno);
        goto cleanup;
    }
    deadline.tv_sec += PROBE_DECODE_TIMEOUT_MS / 1000;
    deadline.tv_nsec += (PROBE_DECODE_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    inputEos = probe_send_input(codec, ctx, 0, data, size, &deadline) &&
        probe_send_input(codec, ctx, 1, NULL, 0, &deadline);

cleanup:
    probe_finish_decode(ctx, inputEos, &deadline);
    free(data);
    if (started) {
        stopRc = OH_VideoDecoder_Stop(codec);
        if (stopRc != AV_ERR_OK) probe_decode_error(ctx, "stop", stopRc);
    }
    if (codec != NULL) {
        destroyRc = OH_VideoDecoder_Destroy(codec);
        if (destroyRc != AV_ERR_OK) probe_decode_error(ctx, "destroy", destroyRc);
    }
    pthread_mutex_lock(&ctx->mutex);
    if (!inputEos || !ctx->outputEos) probe_decode_error_locked(ctx, "eos", -1);
    if (ctx->frames != 1) {
        probe_decode_error_locked(ctx, ctx->frames == 0 ? "no_picture" : "unexpected_frames", -1);
    } else if (ctx->lastError == 0 && ctx->bytes < probe_i420_min_bytes(ctx->width, ctx->height, ctx->stride)) {
        /* A stream change during an output callback may update the layout. */
        probe_decode_error_locked(ctx, "output_size", -1);
    }
    ctx->finished = true;
    int result = ctx->lastError == 0 ? 0 : 2;
    printf("CODEC_DECODE mime=%s frames=%" PRIu64 " bytes=%" PRIu64 " hash=%016" PRIx64
        " errors=%d size=%zu width=%d height=%d pixel_format=%d input_eos=%d stop=%d destroy=%d stage=%s\n",
        mime, ctx->frames, ctx->bytes, ctx->hash, ctx->lastError, size, ctx->width, ctx->height,
        ctx->pixelFormat, inputEos, stopRc, destroyRc, ctx->failureStage != NULL ? ctx->failureStage : "ok");
    printf("CODEC_OUTPUT_EOS=%d\n", ctx->outputEos);
    pthread_mutex_unlock(&ctx->mutex);
    return result;
}

static void probe_usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--instance <mime> [width height]] [--decode <mime> <file> [width height]]\n"
        "       --decode takes one Annex-B access unit with any needed parameter sets (must fit one buffer)\n"
        "       no arguments = enumerate capabilities only (never creates a codec)\n",
        argv0);
}

int main(int argc, char **argv)
{
    printf("CODEC_PROBE_BEGIN pid=%d\n", (int)getpid());
    fflush(stdout);
    int enumerated = probe_enumerate();
    if (argc == 1) {
        return enumerated;
    }
    if (strcmp(argv[1], "--instance") == 0 && argc >= 3) {
        int width = (argc >= 5) ? atoi(argv[3]) : 1280;
        int height = (argc >= 5) ? atoi(argv[4]) : 720;
        return probe_instance(argv[2], width, height);
    }
    if (strcmp(argv[1], "--decode") == 0 && argc >= 4) {
        int width = (argc >= 6) ? atoi(argv[4]) : 1280;
        int height = (argc >= 6) ? atoi(argv[5]) : 720;
        return probe_decode(argv[2], argv[3], width, height);
    }
    probe_usage(argv[0]);
    return 3;
}
