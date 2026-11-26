/* SPDX-License-Identifier: MIT */
#ifndef HMOS_PAL_PROTOCOL_H
#define HMOS_PAL_PROTOCOL_H
#include <stdint.h>
#define HMOS_PAL_MAGIC 0x53415031u
#define HMOS_PAL_SOCKET "hmos.pal.pcm.v1"
#define HMOS_PAL_MAX_DATA 61440u
enum hmos_pal_op {
    HMOS_PAL_OPEN=1, HMOS_PAL_START, HMOS_PAL_WRITE, HMOS_PAL_VOLUME,
    HMOS_PAL_STOP, HMOS_PAL_CLOSE, HMOS_PAL_PAUSE, HMOS_PAL_RESUME,
    HMOS_PAL_STATUS,
    HMOS_PAL_CAPTURE_OPEN, HMOS_PAL_CAPTURE_READ, HMOS_PAL_CAPTURE_START,
    HMOS_PAL_CAPTURE_STOP, HMOS_PAL_CAPTURE_CLOSE, HMOS_PAL_CAPTURE_MUTE,
    HMOS_PAL_CAPTURE_GET_MUTE, HMOS_PAL_CAPTURE_STATUS,
    HMOS_PAL_MIC_MUTE, HMOS_PAL_MIC_GET_MUTE,
    /* Batch F11: playback output switching + PAL_STREAM_VOICE_CALL control */
    HMOS_PAL_SET_DEVICE,
    HMOS_PAL_VOICE_OPEN,
    HMOS_PAL_VOICE_START,
    HMOS_PAL_VOICE_STOP,
    HMOS_PAL_VOICE_CLOSE,
    HMOS_PAL_VOICE_SET_DEVICE,
    HMOS_PAL_VOICE_VOLUME,
    HMOS_PAL_VOICE_MUTE,
    HMOS_PAL_VOICE_STATUS
};
/* Stable PAL device & VSID constants shared between OHOS VDI and Bionic PAL server */
#define HMOS_PAL_DEV_OUT_HANDSET        2u
#define HMOS_PAL_DEV_OUT_SPEAKER        3u
#define HMOS_PAL_DEV_OUT_WIRED_HEADSET  4u
#define HMOS_PAL_DEV_OUT_WIRED_HEADPHONE 5u
#define HMOS_PAL_DEV_OUT_BLUETOOTH_SCO  7u
#define HMOS_PAL_DEV_OUT_USB_HEADSET    12u
#define HMOS_PAL_DEV_IN_HANDSET_MIC     23u
#define HMOS_PAL_DEV_IN_SPEAKER_MIC     24u
#define HMOS_PAL_DEV_IN_BT_SCO_HEADSET  25u
#define HMOS_PAL_DEV_IN_WIRED_HEADSET   26u
#define HMOS_PAL_DEV_IN_USB_HEADSET     31u
#define HMOS_PAL_VSID_MMODE1            0x11C05000u
#define HMOS_PAL_VSID_MMODE2            0x11DC5000u
#define HMOS_PAL_CAPTURE_CHUNK 960u
struct hmos_pal_request {
    uint32_t magic, op, bytes, sequence, arg0, arg1;
};
struct hmos_pal_reply {
    uint32_t magic, sequence;
    int32_t rc;
    uint32_t bytes;
    uint64_t written_frames;
};
_Static_assert(sizeof(struct hmos_pal_request)==24, "fixed wire header");
_Static_assert(sizeof(struct hmos_pal_reply)==24, "fixed wire reply");
#endif
