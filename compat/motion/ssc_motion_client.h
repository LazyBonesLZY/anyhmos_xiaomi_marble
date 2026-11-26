/* SPDX-License-Identifier: MIT */
#ifndef MARBLE_MOTION_SSC_MOTION_CLIENT_H
#define MARBLE_MOTION_SSC_MOTION_CLIENT_H

#include "pickup_detector.h"
#include "ssc_motion_wire.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <poll.h>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace marble_motion {

/*
 * Self-contained SLPI QRTR motion worker for Xiaomi marble (SM7475).
 *
 * Lifecycle & power guarantees:
 *   - 0 background threads / 0 QRTR sockets when pickup motion is disabled.
 *   - Screen power state is determined from DRM DPMS (/sys/class/drm/card0-DSI-{1,0}/dpms),
 *     falling back to DRM enabled (/sys/class/drm/card0-DSI-{1,0}/enabled), and finally
 *     backlight brightness == 0 only when DRM sysfs nodes are absent.
 *   - When enabled and screen is ON:
 *       closes any active SSC stream and idles via interruptible 500 ms poll (0 SLPI draw).
 *   - When enabled and screen is OFF:
 *       1) First attempts SLPI SUID "tilt_to_wake" (on_change=true, MSGID 514).
 *       2) If unavailable on SLPI, falls back to "accel" at 25 Hz (MSGID 513) feeding PickupDetector.
 *   - Re-checks screen state every 500 ms during active streaming so waking the screen
 *     immediately pauses SSC streaming.
 */
inline bool matchSysfsTokenIgnoreCase(const char *text, const char *lit) {
    if (text == nullptr || lit == nullptr) {
        return false;
    }
    size_t i = 0;
    while (lit[i] != '\0') {
        unsigned char a = static_cast<unsigned char>(text[i]);
        unsigned char b = static_cast<unsigned char>(lit[i]);
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<unsigned char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<unsigned char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
        ++i;
    }
    const char tail = text[i];
    return tail == '\0' || tail == '\n' || tail == '\r' || tail == ' ' || tail == '\t';
}

/*
 * Parse DRM connector dpms sysfs text:
 *   "Off", "Suspend", "Standby" -> 1 (screen is OFF)
 *   "On"                        -> 0 (screen is ON)
 *   anything else / null        -> -1 (unrecognized)
 */
inline int parseDrmDpmsIsOff(const char *text) {
    if (text == nullptr) {
        return -1;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    if (*text == '\0') {
        return -1;
    }
    if (matchSysfsTokenIgnoreCase(text, "Off") ||
        matchSysfsTokenIgnoreCase(text, "Suspend") ||
        matchSysfsTokenIgnoreCase(text, "Standby")) {
        return 1;
    }
    if (matchSysfsTokenIgnoreCase(text, "On")) {
        return 0;
    }
    return -1;
}

/*
 * Parse DRM connector enabled sysfs text:
 *   "disabled"           -> 1 (screen is OFF)
 *   "enabled"            -> 0 (screen is ON)
 *   anything else / null -> -1 (unrecognized)
 */
inline int parseDrmEnabledIsOff(const char *text) {
    if (text == nullptr) {
        return -1;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    if (*text == '\0') {
        return -1;
    }
    if (matchSysfsTokenIgnoreCase(text, "disabled")) {
        return 1;
    }
    if (matchSysfsTokenIgnoreCase(text, "enabled")) {
        return 0;
    }
    return -1;
}

inline ssize_t readSmallSysfsFile(const char *path, char *buf, size_t cap) {
    if (path == nullptr || buf == nullptr || cap < 2) {
        return -1;
    }
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n;
    do {
        n = ::read(fd, buf, cap - 1);
    } while (n < 0 && errno == EINTR);
    ::close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    return n;
}

inline int readBacklightBrightnessFromPath(const char *path) {
    char buf[32];
    if (readSmallSysfsFile(path, buf, sizeof(buf)) <= 0) {
        return -1;
    }
    char *end = nullptr;
    long val = std::strtol(buf, &end, 10);
    if (end == buf) {
        return -1;
    }
    return static_cast<int>(val);
}

/*
 * Query screen-off state with strict DRM priority:
 *   1) DRM DPMS paths ("Off"/"Suspend"/"Standby" => 1, "On" => 0)
 *   2) DRM enabled paths ("disabled" => 1, "enabled" => 0)
 *   3) Backlight brightness fallback ONLY when DRM sysfs nodes do not exist/parse (0 => 1, >0 => 0)
 * Returns: 1 (screen OFF), 0 (screen ON), -1 (all sysfs unreadable).
 */
inline int queryScreenOffFromPaths(const char *const *dpms_paths, size_t dpms_count,
                                   const char *const *enabled_paths, size_t enabled_count,
                                   const char *backlight_path) {
    char buf[64];
    for (size_t i = 0; i < dpms_count; ++i) {
        if (dpms_paths != nullptr && dpms_paths[i] != nullptr &&
            readSmallSysfsFile(dpms_paths[i], buf, sizeof(buf)) > 0) {
            const int state = parseDrmDpmsIsOff(buf);
            if (state >= 0) {
                return state;
            }
        }
    }
    for (size_t i = 0; i < enabled_count; ++i) {
        if (enabled_paths != nullptr && enabled_paths[i] != nullptr &&
            readSmallSysfsFile(enabled_paths[i], buf, sizeof(buf)) > 0) {
            const int state = parseDrmEnabledIsOff(buf);
            if (state >= 0) {
                return state;
            }
        }
    }
    if (backlight_path != nullptr) {
        const int br = readBacklightBrightnessFromPath(backlight_path);
        if (br >= 0) {
            return (br == 0) ? 1 : 0;
        }
    }
    return -1;
}

class SscMotionClient {
public:
    using PickupCallback = std::function<void(const char *source, float pitch_deg, float roll_deg)>;
    using LogCallback = std::function<void(const char *msg)>;

    static constexpr const char *kDrmDpmsPathPrimary =
        "/sys/class/drm/card0-DSI-1/dpms";
    static constexpr const char *kDrmDpmsPathSecondary =
        "/sys/class/drm/card0-DSI-0/dpms";
    static constexpr const char *kDrmEnabledPathPrimary =
        "/sys/class/drm/card0-DSI-1/enabled";
    static constexpr const char *kDrmEnabledPathSecondary =
        "/sys/class/drm/card0-DSI-0/enabled";
    static constexpr const char *kBacklightPath =
        "/sys/class/backlight/panel0-backlight/brightness";
    static constexpr float kAccelFallbackRateHz = 25.0f;
    static constexpr int kScreenPollMs = 500;
    static constexpr int kStreamPollSliceMs = 250;
    static constexpr int kSscRpcTimeoutMs = 2000;
    static constexpr int kRetryBackoffMs = 1500;
    static constexpr int64_t kTiltCooldownMs = 2000;

    SscMotionClient(PickupCallback on_pickup, LogCallback on_log = nullptr)
        : on_pickup_(std::move(on_pickup)),
          on_log_(std::move(on_log)),
          detector_(PickupConfig{}, []() { return isScreenOffSysfs(); }) {
        wake_pipe_[0] = -1;
        wake_pipe_[1] = -1;
    }

    ~SscMotionClient() {
        stop();
    }

    SscMotionClient(const SscMotionClient &) = delete;
    SscMotionClient &operator=(const SscMotionClient &) = delete;

    static int readBacklightBrightness(const char *path = kBacklightPath) {
        return readBacklightBrightnessFromPath(path);
    }

    static int queryScreenOffSysfs() {
        static constexpr const char *kDpmsPaths[] = {
            kDrmDpmsPathPrimary,
            kDrmDpmsPathSecondary,
        };
        static constexpr const char *kEnabledPaths[] = {
            kDrmEnabledPathPrimary,
            kDrmEnabledPathSecondary,
        };
        return queryScreenOffFromPaths(kDpmsPaths, sizeof(kDpmsPaths) / sizeof(kDpmsPaths[0]),
                                       kEnabledPaths, sizeof(kEnabledPaths) / sizeof(kEnabledPaths[0]),
                                       kBacklightPath);
    }

    static bool isScreenOffSysfs() {
        return queryScreenOffSysfs() == 1;
    }

    bool sleepInterruptibleMs(int ms) {
        if (stop_requested_.load(std::memory_order_acquire)) {
            return false;
        }
        if (wake_pipe_[0] < 0) {
            return false;
        }
        struct pollfd pfd {};
        pfd.fd = wake_pipe_[0];
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, ms);
        if (rc > 0) {
            drainWakePipe();
        }
        return !stop_requested_.load(std::memory_order_acquire);
    }

    bool start() {
        std::lock_guard<std::mutex> lk(op_mu_);
        if (running_) {
            return true;
        }
        if (::pipe2(wake_pipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
            wake_pipe_[0] = -1;
            wake_pipe_[1] = -1;
            return false;
        }
        stop_requested_.store(false, std::memory_order_release);
        running_ = true;
        worker_ = std::thread(&SscMotionClient::workerLoop, this);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(op_mu_);
        if (!running_) {
            return;
        }
        stop_requested_.store(true, std::memory_order_release);
        signalWake();
        if (worker_.joinable()) {
            worker_.join();
        }
        closeWakePipe();
        running_ = false;
    }

    bool isRunning() const {
        std::lock_guard<std::mutex> lk(op_mu_);
        return running_;
    }

private:
    static int64_t monotonicMs() {
        struct timespec ts {};
        if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            return 0;
        }
        return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }

    void logLine(const char *msg) {
        if (on_log_ && msg != nullptr) {
            on_log_(msg);
        }
    }

    void signalWake() {
        if (wake_pipe_[1] >= 0) {
            const uint8_t b = 1;
            ssize_t r;
            do {
                r = ::write(wake_pipe_[1], &b, 1);
            } while (r < 0 && errno == EINTR);
        }
    }

    void drainWakePipe() {
        if (wake_pipe_[0] < 0) {
            return;
        }
        uint8_t buf[32];
        while (::read(wake_pipe_[0], buf, sizeof(buf)) > 0) {}
    }

    void closeWakePipe() {
        if (wake_pipe_[0] >= 0) {
            ::close(wake_pipe_[0]);
            wake_pipe_[0] = -1;
        }
        if (wake_pipe_[1] >= 0) {
            ::close(wake_pipe_[1]);
            wake_pipe_[1] = -1;
        }
    }

    void closeSession(SscMotionSession &sess) {
        if (sess.fd >= 0) {
            if (sess.stream_active) {
                (void)ssc_motion_stop_stream(&sess, 400);
            }
            ssc_motion_close_socket(sess.fd);
            sess.fd = -1;
        }
        sess.client_seen = false;
        sess.client_id = 0;
        sess.stream_active = false;
        sess.is_tilt_to_wake = false;
    }

    bool openPreferredStream(SscMotionSession &sess) {
        std::memset(&sess, 0, sizeof(sess));
        sess.fd = ssc_motion_open_socket();
        if (sess.fd < 0) {
            logLine("SSC socket(AF_QIPCRTR) failed");
            return false;
        }

        int rc = ssc_motion_find_endpoint(sess.fd, wake_pipe_[0], &sess.ep, kSscRpcTimeoutMs);
        if (rc != SSC_MOTION_OK) {
            closeSession(sess);
            return false;
        }

        /* 1) First try SLPI hardware/DSP "tilt_to_wake" (MSGID 514, on_change=true). */
        if (!tilt_suid_confirmed_missing_) {
            rc = ssc_motion_lookup_sensor(&sess, wake_pipe_[0], "tilt_to_wake", kSscRpcTimeoutMs);
            if (rc == SSC_MOTION_WOKE) {
                closeSession(sess);
                return false;
            }
            if (rc == SSC_MOTION_OK) {
                rc = ssc_motion_start_stream(&sess, wake_pipe_[0], true, 0.0f, kSscRpcTimeoutMs);
                if (rc == SSC_MOTION_OK) {
                    logLine("STREAM_START mode=tilt_to_wake msgid=514");
                    return true;
                }
                if (rc == SSC_MOTION_WOKE) {
                    closeSession(sess);
                    return false;
                }
            } else if (rc == SSC_MOTION_NOT_FOUND) {
                tilt_suid_confirmed_missing_ = true;
                logLine("SUID tilt_to_wake unavailable on SLPI; falling back to accel 25Hz");
            }
            /* Reopen a clean QRTR socket for accel fallback so QMI client/txn state is fresh. */
            closeSession(sess);
            sess.fd = ssc_motion_open_socket();
            if (sess.fd < 0) {
                return false;
            }
            if (ssc_motion_find_endpoint(sess.fd, wake_pipe_[0], &sess.ep, kSscRpcTimeoutMs) != SSC_MOTION_OK) {
                closeSession(sess);
                return false;
            }
        }

        /* 2) Fallback: SLPI "accel" at 25 Hz (MSGID 513) feeding PickupDetector. */
        rc = ssc_motion_lookup_sensor(&sess, wake_pipe_[0], "accel", kSscRpcTimeoutMs);
        if (rc != SSC_MOTION_OK) {
            closeSession(sess);
            return false;
        }
        rc = ssc_motion_start_stream(&sess, wake_pipe_[0], false, kAccelFallbackRateHz, kSscRpcTimeoutMs);
        if (rc != SSC_MOTION_OK) {
            closeSession(sess);
            return false;
        }

        detector_.resetGesture();
        logLine("STREAM_START mode=accel_25hz msgid=513");
        return true;
    }

    void workerLoop() {
        SscMotionSession sess {};
        sess.fd = -1;
        bool was_screen_on_logged = false;
        bool has_tilt_trigger = false;
        int64_t last_tilt_trigger_ms = 0;

        logLine("WORKER_START pickup=enabled");

        while (!stop_requested_.load(std::memory_order_acquire)) {
            const int screen_off = queryScreenOffSysfs();
            if (screen_off == 0) {
                /* Screen is ON: pause SSC stream completely for zero SLPI power draw. */
                if (sess.fd >= 0) {
                    closeSession(sess);
                    detector_.resetGesture();
                    logLine("STREAM_PAUSE reason=screen_on");
                }
                if (!was_screen_on_logged) {
                    was_screen_on_logged = true;
                }
                sleepInterruptibleMs(kScreenPollMs);
                continue;
            }
            if (screen_off < 0) {
                /* Display/backlight sysfs temporarily unavailable: do not spin aggressively. */
                if (sess.fd >= 0) {
                    closeSession(sess);
                }
                sleepInterruptibleMs(kScreenPollMs);
                continue;
            }
            was_screen_on_logged = false;

            /* Screen is OFF (DRM DPMS Off/Suspend/Standby, disabled, or fallback brightness == 0): ensure SLPI stream is open. */
            if (sess.fd < 0) {
                if (!openPreferredStream(sess)) {
                    sleepInterruptibleMs(kRetryBackoffMs);
                    continue;
                }
            }

            /* Active streaming loop while screen stays OFF. */
            int64_t next_screen_check_ms = monotonicMs() + kScreenPollMs;
            while (!stop_requested_.load(std::memory_order_acquire)) {
                const int64_t now_before = monotonicMs();
                if (now_before >= next_screen_check_ms) {
                    if (queryScreenOffSysfs() != 1) {
                        closeSession(sess);
                        detector_.resetGesture();
                        logLine("STREAM_PAUSE reason=screen_on");
                        break;
                    }
                    next_screen_check_ms = now_before + kScreenPollMs;
                }

                SscMotionEventBatch batch {};
                const int rc = ssc_motion_poll_events(&sess, wake_pipe_[0], kStreamPollSliceMs, &batch);
                if (rc == SSC_MOTION_WOKE) {
                    break;
                }
                if (rc == SSC_MOTION_TIMEOUT) {
                    continue;
                }
                if (rc != SSC_MOTION_OK || batch.sensor_error) {
                    if (sess.is_tilt_to_wake) {
                        tilt_suid_confirmed_missing_ = true;
                        logLine("STREAM_ERR mode=tilt_to_wake switching_to=accel_25hz");
                    } else {
                        logLine("STREAM_ERR mode=accel_25hz reconnecting");
                    }
                    closeSession(sess);
                    sleepInterruptibleMs(kRetryBackoffMs);
                    break;
                }

                const int64_t now_ms = monotonicMs();
                if (sess.is_tilt_to_wake) {
                    if (batch.tilt_triggered) {
                        const bool cooldown_ok =
                            (!has_tilt_trigger || (now_ms - last_tilt_trigger_ms) >= kTiltCooldownMs);
                        if (cooldown_ok && isScreenOffSysfs()) {
                            has_tilt_trigger = true;
                            last_tilt_trigger_ms = now_ms;
                            if (on_pickup_) {
                                on_pickup_("tilt_to_wake", 0.0f, 0.0f);
                            }
                        }
                    }
                } else {
                    for (uint32_t i = 0; i < batch.accel_count; ++i) {
                        const SscMotionAccelSample &s = batch.accel[i];
                        if (detector_.feed(s.x, s.y, s.z, now_ms)) {
                            if (on_pickup_) {
                                on_pickup_("accel_25hz",
                                           detector_.lastTriggerPitchDeg(),
                                           detector_.lastTriggerRollDeg());
                            }
                        }
                    }
                }
            }
        }

        closeSession(sess);
        logLine("WORKER_STOP pickup=disabled");
    }

    PickupCallback on_pickup_;
    LogCallback on_log_;
    PickupDetector detector_;

    mutable std::mutex op_mu_;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    bool running_ = false;
    bool tilt_suid_confirmed_missing_ = false;
    int wake_pipe_[2];
};

} // namespace marble_motion

#endif // MARBLE_MOTION_SSC_MOTION_CLIENT_H
