/* SPDX-License-Identifier: MIT
 *
 * Minimal <log/log.h> stand-in for building AOSP audio_route.c against the
 * Android NDK, which only ships <android/log.h>.
 *
 * ALOG* keeps the same contract as system/logging/liblog/include/log/log.h.
 * LOG_ALWAYS_FATAL intentionally does NOT abort here: the only use in
 * audio_route.c is an unsupported-mixer-ctl-type branch that already falls
 * through to "return 0" (sizeof_ctl_type).  Aborting would take down the whole
 * PAL process on a single unexpected control type, which is never what we want
 * on bring-up hardware.
 */
#pragma once

#include <android/log.h>
#include <stdint.h>

#ifndef ALOG_TAG
#define ALOG_TAG "audio_route"
#endif

#ifndef ALOG_UNUSED
#define ALOG_UNUSED(x) ((void)(x))
#endif

#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, ALOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, ALOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, ALOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, ALOG_TAG, __VA_ARGS__)
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, ALOG_TAG, __VA_ARGS__)

#define LOG_ALWAYS_FATAL(...) ALOGE(__VA_ARGS__)
