/* SPDX-License-Identifier: MIT */
/*
 * ssc_motion_wire.c - Self-contained QRTR/QMI SSC reader for marble Motion HDI.
 *
 * Reuses compat/audio/pd-mapper/qrtr.c (libqrtr.h) and compat/sensors/ssc_wire.h
 * strictly read-only, adding support for SLPI "tilt_to_wake" (MSGID 514 / event 775)
 * and 25 Hz "accel" (MSGID 513 / event 1025) fallback without modifying compat/sensors/.
 */

#include "ssc_motion_wire.h"
#include "libqrtr.h"
#include "ssc_wire.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    SSC_QRTR_SERVICE = 400,
    SSC_QRTR_VERSION = 1,
    SSC_MAX_ENDPOINTS = 16,
    SNS_MSGID_ATTR_EVENT = 128,
    SNS_MSGID_ERROR_EVENT = 130,
    SNS_MSGID_SUID_EVENT = 768,
    SNS_MSGID_TILT_TO_WAKE_EVENT = 775,
    SNS_MSGID_STD_SENSOR_EVENT = 1025,
    SNS_MSGID_SCALAR_GESTURE_EVENT = 1028,
};

static int64_t motion_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int ssc_motion_open_socket(void)
{
    return socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

void ssc_motion_close_socket(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

static int motion_recv_packet(int fd, int wake_fd, int64_t deadline_ms,
                              uint8_t *buf, size_t cap,
                              struct sockaddr_qrtr *src, size_t *out_size)
{
    while (1) {
        const int64_t now = motion_now_ms();
        if (now < 0) {
            return SSC_MOTION_ERROR;
        }
        if (now >= deadline_ms) {
            return SSC_MOTION_TIMEOUT;
        }
        struct pollfd fds[2];
        nfds_t nfds = 1;
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        if (wake_fd >= 0) {
            fds[1].fd = wake_fd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            nfds = 2;
        }
        int wait_ms = (int)(deadline_ms - now);
        if (wait_ms < 1) {
            wait_ms = 1;
        }
        int ready = poll(fds, nfds, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SSC_MOTION_ERROR;
        }
        if (ready == 0) {
            return SSC_MOTION_TIMEOUT;
        }
        if (nfds == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            return SSC_MOTION_WOKE;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return SSC_MOTION_ERROR;
        }
        if (!(fds[0].revents & POLLIN)) {
            continue;
        }
        socklen_t slen = sizeof(*src);
        ssize_t n = recvfrom(fd, buf, cap, MSG_TRUNC, (struct sockaddr *)src, &slen);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return SSC_MOTION_ERROR;
        }
        if (slen != sizeof(*src) || src->sq_family != AF_QIPCRTR || (size_t)n > cap) {
            return SSC_MOTION_ERROR;
        }
        *out_size = (size_t)n;
        return SSC_MOTION_OK;
    }
}

int ssc_motion_find_endpoint(int fd, int wake_fd, struct SscMotionEndpoint *out_ep, int timeout_ms)
{
    if (fd < 0 || out_ep == NULL || timeout_ms <= 0) {
        return SSC_MOTION_ERROR;
    }
    if (qrtr_new_lookup(fd, SSC_QRTR_SERVICE, SSC_QRTR_VERSION, 0) != 0) {
        return SSC_MOTION_ERROR;
    }

    struct {
        uint32_t node;
        uint32_t port;
        bool active;
    } entries[SSC_MAX_ENDPOINTS];
    memset(entries, 0, sizeof(entries));

    const int64_t start = motion_now_ms();
    if (start < 0) {
        (void)qrtr_remove_lookup(fd, SSC_QRTR_SERVICE, SSC_QRTR_VERSION, 0);
        return SSC_MOTION_ERROR;
    }
    const int64_t deadline = start + timeout_ms;
    int rc = SSC_MOTION_OK;
    bool eof = false;

    while (!eof) {
        uint8_t buf[256];
        size_t size = 0;
        struct sockaddr_qrtr src;
        memset(&src, 0, sizeof(src));
        rc = motion_recv_packet(fd, wake_fd, deadline, buf, sizeof(buf), &src, &size);
        if (rc != SSC_MOTION_OK) {
            break;
        }
        struct qrtr_packet pkt;
        memset(&pkt, 0, sizeof(pkt));
        if (src.sq_port != QRTR_PORT_CTRL || qrtr_decode(&pkt, buf, size, &src) != 0) {
            continue;
        }
        if (pkt.type == QRTR_TYPE_NEW_SERVER &&
            pkt.service == 0 && pkt.node == 0 && pkt.port == 0 &&
            pkt.version == 0 && pkt.instance == 0) {
            eof = true;
            break;
        }
        if ((pkt.type != QRTR_TYPE_NEW_SERVER && pkt.type != QRTR_TYPE_DEL_SERVER) ||
            pkt.service != SSC_QRTR_SERVICE || pkt.version != SSC_QRTR_VERSION || pkt.port == 0) {
            continue;
        }
        int empty = -1;
        int found = -1;
        for (int i = 0; i < SSC_MAX_ENDPOINTS; ++i) {
            if (!entries[i].active) {
                if (empty < 0) {
                    empty = i;
                }
            } else if (entries[i].node == pkt.node && entries[i].port == pkt.port) {
                found = i;
            }
        }
        if (pkt.type == QRTR_TYPE_DEL_SERVER) {
            if (found >= 0) {
                entries[found].active = false;
            }
        } else if (found < 0 && empty >= 0) {
            entries[empty].node = pkt.node;
            entries[empty].port = pkt.port;
            entries[empty].active = true;
        }
    }

    (void)qrtr_remove_lookup(fd, SSC_QRTR_SERVICE, SSC_QRTR_VERSION, 0);
    if (rc != SSC_MOTION_OK) {
        return rc;
    }

    unsigned int active = 0;
    for (int i = 0; i < SSC_MAX_ENDPOINTS; ++i) {
        if (entries[i].active) {
            out_ep->node = entries[i].node;
            out_ep->port = entries[i].port;
            ++active;
        }
    }
    return (active >= 1) ? SSC_MOTION_OK : SSC_MOTION_NOT_FOUND;
}

static int motion_accept_ack(struct View body, struct SscMotionSession *sess)
{
    struct Ack ack;
    if (!ParseAck(body, &ack)) {
        return SSC_MOTION_ERROR;
    }
    if (ack.result != 0 || ack.error != 0 || (ack.has_sns_result && ack.sns_result != 0)) {
        return SSC_MOTION_ERROR;
    }
    if (ack.has_client) {
        if (sess->client_seen && sess->client_id != ack.client) {
            return SSC_MOTION_ERROR;
        }
        sess->client_seen = true;
        sess->client_id = ack.client;
    }
    return SSC_MOTION_OK;
}

static int motion_query_phase(struct SscMotionSession *sess, int wake_fd,
                              struct Sensor *sensor, bool query_attrs, int timeout_ms)
{
    const struct Suid suid_lookup = {UINT64_C(0xabababababababab), UINT64_C(0xabababababababab)};
    const uint16_t txn = query_attrs ? 2 : 1;
    uint8_t req[1012];
    const size_t req_size = BuildRequest(req, txn,
                                         query_attrs ? sensor->suid : suid_lookup,
                                         query_attrs ? NULL : sensor->type);
    const int64_t start = motion_now_ms();
    if (req_size == 0 || start < 0) {
        return SSC_MOTION_ERROR;
    }
    if (qrtr_sendto(sess->fd, sess->ep.node, sess->ep.port, req, (unsigned int)req_size) != 0) {
        return SSC_MOTION_ERROR;
    }

    const int64_t deadline = start + timeout_ms;
    bool ack_seen = false;
    while (1) {
        uint8_t data[2048];
        size_t size = 0;
        struct sockaddr_qrtr src;
        memset(&src, 0, sizeof(src));
        int rc = motion_recv_packet(sess->fd, wake_fd, deadline, data, sizeof(data), &src, &size);
        if (rc != SSC_MOTION_OK) {
            return rc;
        }
        if (src.sq_node != sess->ep.node || src.sq_port != sess->ep.port) {
            continue;
        }
        if (size < 7 || Read16(data + 5) != size - 7) {
            return SSC_MOTION_ERROR;
        }
        struct View body = {data + 7, size - 7};
        if (data[0] == QMI_RESPONSE && Read16(data + 1) == txn && Read16(data + 3) == 0x20) {
            if (motion_accept_ack(body, sess) != SSC_MOTION_OK) {
                return SSC_MOTION_ERROR;
            }
            ack_seen = true;
        } else if (data[0] == QMI_INDICATION && Read16(data + 3) == 0x21) {
            struct Report report;
            if (!ParseReport(body, &report)) {
                return SSC_MOTION_ERROR;
            }
            if (sess->client_seen && sess->client_id != report.client) {
                return SSC_MOTION_ERROR;
            }
            sess->client_seen = true;
            sess->client_id = report.client;
            if (!ParseEvents(report.payload, query_attrs ? 1u : 0u, sensor)) {
                return SSC_MOTION_ERROR;
            }
            if (sensor->error_seen && sensor->sns_error != 0) {
                return SSC_MOTION_NOT_FOUND;
            }
        }
        if (ack_seen && (query_attrs ? sensor->attr_seen : sensor->suid_seen)) {
            return SSC_MOTION_OK;
        }
    }
}

int ssc_motion_lookup_sensor(struct SscMotionSession *sess, int wake_fd,
                             const char *sensor_type, int timeout_ms)
{
    if (sess == NULL || sess->fd < 0 || sensor_type == NULL || timeout_ms <= 0) {
        return SSC_MOTION_ERROR;
    }
    struct Sensor sensor;
    memset(&sensor, 0, sizeof(sensor));
    sensor.type = sensor_type;

    int rc = motion_query_phase(sess, wake_fd, &sensor, false, timeout_ms);
    if (rc != SSC_MOTION_OK) {
        return rc;
    }
    if (!sensor.suid_seen || sensor.suid_count == 0) {
        return SSC_MOTION_NOT_FOUND;
    }

    rc = motion_query_phase(sess, wake_fd, &sensor, true, timeout_ms);
    if (rc != SSC_MOTION_OK) {
        return rc;
    }
    if (!sensor.available_seen || !sensor.available) {
        return SSC_MOTION_NOT_FOUND;
    }

    sess->suid.low = sensor.suid.low;
    sess->suid.high = sensor.suid.high;
    return SSC_MOTION_OK;
}

int ssc_motion_start_stream(struct SscMotionSession *sess, int wake_fd,
                            bool on_change, float rate_hz, int timeout_ms)
{
    if (sess == NULL || sess->fd < 0 || timeout_ms <= 0) {
        return SSC_MOTION_ERROR;
    }
    struct Suid wire_suid = {sess->suid.low, sess->suid.high};
    uint8_t req[1012];
    const size_t req_size = BuildStreamRequestMode(req, wire_suid, false, on_change, rate_hz);
    const int64_t start = motion_now_ms();
    if (req_size == 0 || start < 0) {
        return SSC_MOTION_ERROR;
    }
    if (qrtr_sendto(sess->fd, sess->ep.node, sess->ep.port, req, (unsigned int)req_size) != 0) {
        return SSC_MOTION_ERROR;
    }

    const int64_t deadline = start + timeout_ms;
    while (1) {
        uint8_t data[2048];
        size_t size = 0;
        struct sockaddr_qrtr src;
        memset(&src, 0, sizeof(src));
        int rc = motion_recv_packet(sess->fd, wake_fd, deadline, data, sizeof(data), &src, &size);
        if (rc != SSC_MOTION_OK) {
            return rc;
        }
        if (src.sq_node != sess->ep.node || src.sq_port != sess->ep.port) {
            continue;
        }
        if (size < 7 || Read16(data + 5) != size - 7) {
            return SSC_MOTION_ERROR;
        }
        struct View body = {data + 7, size - 7};
        if (data[0] == QMI_RESPONSE && Read16(data + 1) == 3 && Read16(data + 3) == 0x20) {
            if (motion_accept_ack(body, sess) != SSC_MOTION_OK) {
                return SSC_MOTION_ERROR;
            }
            sess->stream_active = true;
            sess->is_tilt_to_wake = on_change;
            return SSC_MOTION_OK;
        }
    }
}

int ssc_motion_stop_stream(struct SscMotionSession *sess, int timeout_ms)
{
    if (sess == NULL || sess->fd < 0 || !sess->stream_active) {
        return SSC_MOTION_OK;
    }
    struct Suid wire_suid = {sess->suid.low, sess->suid.high};
    uint8_t req[1012];
    const size_t req_size = BuildStreamRequestMode(req, wire_suid, true, false, 0.0f);
    sess->stream_active = false;
    if (req_size == 0) {
        return SSC_MOTION_ERROR;
    }
    if (qrtr_sendto(sess->fd, sess->ep.node, sess->ep.port, req, (unsigned int)req_size) != 0) {
        return SSC_MOTION_ERROR;
    }
    const int64_t start = motion_now_ms();
    if (start < 0 || timeout_ms <= 0) {
        return SSC_MOTION_OK;
    }
    const int64_t deadline = start + timeout_ms;
    while (1) {
        uint8_t data[2048];
        size_t size = 0;
        struct sockaddr_qrtr src;
        memset(&src, 0, sizeof(src));
        int rc = motion_recv_packet(sess->fd, -1, deadline, data, sizeof(data), &src, &size);
        if (rc != SSC_MOTION_OK) {
            return rc;
        }
        if (src.sq_node != sess->ep.node || src.sq_port != sess->ep.port) {
            continue;
        }
        if (size < 7 || Read16(data + 5) != size - 7) {
            return SSC_MOTION_ERROR;
        }
        if (data[0] == QMI_RESPONSE && Read16(data + 1) == 4 && Read16(data + 3) == 0x20) {
            return motion_accept_ack((struct View){data + 7, size - 7}, sess);
        }
    }
}

static bool motion_parse_error_payload(struct View payload, uint64_t *out_err)
{
    struct Field field;
    int next;
    *out_err = 0;
    while ((next = NextField(&payload, &field)) > 0) {
        if (field.id == 1 && field.wire == 0) {
            *out_err = field.value;
            return true;
        }
    }
    return next == 0;
}

static bool motion_decode_indication_events(struct View view,
                                            const struct SscMotionSession *sess,
                                            struct SscMotionEventBatch *out_batch)
{
    struct View scan = view;
    struct Field field;
    struct Suid uid = {0, 0};
    bool uid_seen = false;
    int next;
    while ((next = NextField(&scan, &field)) > 0) {
        if (field.id == 1) {
            if (uid_seen || field.wire != 2 || !ParseSuid(field.bytes, &uid)) {
                return false;
            }
            uid_seen = true;
        }
    }
    if (next < 0 || !uid_seen) {
        return false;
    }
    struct Suid expected = {sess->suid.low, sess->suid.high};
    if (!SameSuid(uid, expected)) {
        return true;
    }

    struct Sensor scratch;
    memset(&scratch, 0, sizeof(scratch));

    while ((next = NextField(&view, &field)) > 0) {
        if (field.id != 2) {
            continue;
        }
        if (field.wire != 2) {
            return false;
        }
        struct View ev = field.bytes;
        struct View payload = {NULL, 0};
        unsigned int seen = 0;
        uint32_t msg_id = 0;
        uint64_t timestamp = 0;
        struct Field part;
        int inner;
        while ((inner = NextField(&ev, &part)) > 0) {
            if (part.id < 1 || part.id > 3) {
                continue;
            }
            unsigned int bit = 1u << (part.id - 1);
            if (seen & bit) {
                return false;
            }
            seen |= bit;
            if (part.id == 1) {
                if (part.wire != 5) {
                    return false;
                }
                msg_id = (uint32_t)part.value;
            } else if (part.id == 2) {
                if (part.wire != 1) {
                    return false;
                }
                timestamp = part.value;
            } else if (part.id == 3) {
                if (part.wire != 2) {
                    return false;
                }
                payload = part.bytes;
            }
        }
        /* Allow field 3 (payload) to be omitted when an event carries an empty protobuf body (e.g. MSGID 775). */
        if (inner < 0 || (seen & 3u) != 3u) {
            return false;
        }

        if (msg_id == SNS_MSGID_ERROR_EVENT) {
            uint64_t sns_err = 0;
            if (motion_parse_error_payload(payload, &sns_err) && sns_err != 0) {
                out_batch->sensor_error = true;
                out_batch->sns_error = sns_err;
            }
            continue;
        }

        if (sess->is_tilt_to_wake) {
            if (msg_id == SNS_MSGID_TILT_TO_WAKE_EVENT || msg_id == SNS_MSGID_SCALAR_GESTURE_EVENT) {
                out_batch->tilt_triggered = true;
            } else if (msg_id == SNS_MSGID_STD_SENSOR_EVENT) {
                scratch.stored_samples = 0;
                if (payload.size == 0) {
                    out_batch->tilt_triggered = true;
                } else if (ParseSample(payload, timestamp, &scratch) &&
                           scratch.stored_samples > 0 &&
                           scratch.samples[0].data[0] > 0.5f) {
                    out_batch->tilt_triggered = true;
                }
            }
        } else {
            if (msg_id == SNS_MSGID_STD_SENSOR_EVENT) {
                scratch.stored_samples = 0;
                if (ParseSample(payload, timestamp, &scratch)) {
                    for (unsigned int i = 0; i < scratch.stored_samples; ++i) {
                        if (scratch.samples[i].value_count >= 3 && out_batch->accel_count < 32) {
                            struct SscMotionAccelSample *dst = &out_batch->accel[out_batch->accel_count++];
                            dst->x = scratch.samples[i].data[0];
                            dst->y = scratch.samples[i].data[1];
                            dst->z = scratch.samples[i].data[2];
                            dst->timestamp_ticks = scratch.samples[i].timestamp;
                        }
                    }
                }
            }
        }
    }
    return next == 0;
}

int ssc_motion_poll_events(struct SscMotionSession *sess, int wake_fd,
                           int timeout_ms, struct SscMotionEventBatch *out_batch)
{
    if (sess == NULL || sess->fd < 0 || out_batch == NULL || timeout_ms <= 0) {
        return SSC_MOTION_ERROR;
    }
    memset(out_batch, 0, sizeof(*out_batch));

    const int64_t start = motion_now_ms();
    if (start < 0) {
        return SSC_MOTION_ERROR;
    }
    const int64_t deadline = start + timeout_ms;

    while (1) {
        uint8_t data[2048];
        size_t size = 0;
        struct sockaddr_qrtr src;
        memset(&src, 0, sizeof(src));
        int rc = motion_recv_packet(sess->fd, wake_fd, deadline, data, sizeof(data), &src, &size);
        if (rc != SSC_MOTION_OK) {
            return rc;
        }
        if (src.sq_node != sess->ep.node || src.sq_port != sess->ep.port) {
            continue;
        }
        if (size < 7 || Read16(data + 5) != size - 7) {
            return SSC_MOTION_ERROR;
        }
        if (data[0] != QMI_INDICATION || Read16(data + 3) != 0x21) {
            continue;
        }
        struct Report report;
        if (!ParseReport((struct View){data + 7, size - 7}, &report)) {
            return SSC_MOTION_ERROR;
        }
        if (sess->client_seen && sess->client_id != report.client) {
            return SSC_MOTION_ERROR;
        }
        sess->client_seen = true;
        sess->client_id = report.client;
        if (!motion_decode_indication_events(report.payload, sess, out_batch)) {
            return SSC_MOTION_ERROR;
        }
        return SSC_MOTION_OK;
    }
}
