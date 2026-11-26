/* SPDX-License-Identifier: MIT */
#ifndef MARBLE_MOTION_SSC_MOTION_WIRE_H
#define MARBLE_MOTION_SSC_MOTION_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum SscMotionRc {
    SSC_MOTION_OK = 0,
    SSC_MOTION_TIMEOUT = 1,
    SSC_MOTION_WOKE = 2,
    SSC_MOTION_NOT_FOUND = 3,
    SSC_MOTION_ERROR = -1,
};

struct SscMotionEndpoint {
    uint32_t node;
    uint32_t port;
};

struct SscMotionSuid {
    uint64_t low;
    uint64_t high;
};

struct SscMotionAccelSample {
    float x;
    float y;
    float z;
    uint64_t timestamp_ticks;
};

struct SscMotionEventBatch {
    bool tilt_triggered;
    bool sensor_error;
    uint64_t sns_error;
    uint32_t accel_count;
    struct SscMotionAccelSample accel[32];
};

struct SscMotionSession {
    int fd;
    struct SscMotionEndpoint ep;
    struct SscMotionSuid suid;
    bool client_seen;
    uint64_t client_id;
    bool stream_active;
    bool is_tilt_to_wake;
};

int ssc_motion_open_socket(void);
void ssc_motion_close_socket(int fd);

int ssc_motion_find_endpoint(int fd, int wake_fd, struct SscMotionEndpoint *out_ep, int timeout_ms);

/* Look up SUID by name ("tilt_to_wake" or "accel") and verify availability via attribute query. */
int ssc_motion_lookup_sensor(struct SscMotionSession *sess, int wake_fd,
                             const char *sensor_type, int timeout_ms);

/* Start streaming: on_change=true -> MSGID 514; on_change=false -> MSGID 513 at rate_hz. */
int ssc_motion_start_stream(struct SscMotionSession *sess, int wake_fd,
                            bool on_change, float rate_hz, int timeout_ms);

/* Send MSGID 10 disable request for active stream. */
int ssc_motion_stop_stream(struct SscMotionSession *sess, int timeout_ms);

/* Poll for next QMI indication packet and decode tilt_to_wake / accel events. */
int ssc_motion_poll_events(struct SscMotionSession *sess, int wake_fd,
                           int timeout_ms, struct SscMotionEventBatch *out_batch);

#ifdef __cplusplus
}
#endif

#endif // MARBLE_MOTION_SSC_MOTION_WIRE_H
