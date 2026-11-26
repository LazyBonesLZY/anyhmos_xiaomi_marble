/* SPDX-License-Identifier: MIT */
#ifndef MARBLE_MOTION_PICKUP_DETECTOR_H
#define MARBLE_MOTION_PICKUP_DETECTOR_H

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace marble_motion {

/*
 * Pure C++17 raise-to-wake (pickup) detector aligned with Xiaomi marble SM7475
 * Qualcomm sns_tilt_to_wake.json parameters:
 *   - min_delta_pitch_threshold_deg = 45.0
 *   - min_pitch_threshold_deg       = 40.0
 *   - max_roll_threshold_deg        = 25.0
 *   - max_accel_spread_threshold    = 4.8 m/s^2
 *   - viewing pose: pitch in [40, 82] deg, |roll| <= 25 deg
 *   - gravity norm gate: [6.5, 13.5] m/s^2 (near 9.81 m/s^2)
 *   - stability hold: >= 120 ms continuous viewing pose without jolt
 *   - cooldown: 2000 ms between triggers
 *   - screen-off gate via injectable callback (backlight_brightness == 0)
 *
 * Coordinate convention (Qualcomm SEE / Android / OHOS device frame):
 *   +X = right edge, +Y = top edge (earpiece), +Z = out of screen (toward user).
 *   Flat face-up on table  : (0, 0, +9.81)         -> pitch =   0 deg, roll = 0 deg
 *   Viewing pose 55 deg    : (0, +8.03, +5.62)     -> pitch = +55 deg, roll = 0 deg
 *   Pocket vertical (down) : (0, -9.81, 0)         -> pitch = -90 deg
 *   Face-down tilted       : (0, -8.03, +5.62) or (0, +8.03, -5.62) -> pitch = -55 or +125 deg
 */
struct PickupConfig {
    float min_delta_pitch_deg = 45.0f;
    float min_pitch_deg = 40.0f;
    float max_pitch_deg = 82.0f;
    float max_roll_deg = 25.0f;
    float flat_rest_max_pitch_deg = 25.0f;
    float pocket_upside_down_deg = -60.0f;
    float pocket_upright_away_deg = 88.0f;
    float min_gravity_norm = 6.5f;
    float max_gravity_norm = 13.5f;
    float max_accel_spread = 4.8f;
    int64_t stability_hold_ms = 120;
    int64_t cooldown_ms = 2000;
    int64_t baseline_window_ms = 1500;
    int64_t max_gesture_window_ms = 4000;
};

struct AccelAngles {
    float norm = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    bool valid = false;
};

class PickupDetector {
public:
    using ScreenOffFn = std::function<bool()>;

    explicit PickupDetector(PickupConfig cfg = PickupConfig{},
                            ScreenOffFn is_screen_off = nullptr)
        : cfg_(cfg), is_screen_off_(std::move(is_screen_off)) {}

    void setScreenOffCallback(ScreenOffFn fn) {
        is_screen_off_ = std::move(fn);
    }

    const PickupConfig &config() const { return cfg_; }

    void reset() {
        rest_count_ = 0;
        rest_head_ = 0;
        holding_ = false;
        hold_start_ms_ = 0;
        has_triggered_ = false;
        last_trigger_ms_ = 0;
        last_trigger_pitch_deg_ = 0.0f;
        last_trigger_roll_deg_ = 0.0f;
    }

    /* Reset gesture/hold state while preserving trigger cooldown. */
    void resetGesture() {
        rest_count_ = 0;
        rest_head_ = 0;
        holding_ = false;
        hold_start_ms_ = 0;
    }

    static AccelAngles computeAngles(float ax, float ay, float az) {
        AccelAngles out{};
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) {
            return out;
        }
        const float norm = std::sqrt(ax * ax + ay * ay + az * az);
        if (!std::isfinite(norm) || norm < 1e-3f) {
            return out;
        }
        constexpr float kRadToDeg = 57.29577951308232f;
        const float yz = std::sqrt(ay * ay + az * az);
        out.norm = norm;
        out.pitch_deg = std::atan2(ay, az) * kRadToDeg;
        out.roll_deg = std::atan2(ax, yz) * kRadToDeg;
        out.valid = true;
        return out;
    }

    /*
     * Feed one accelerometer sample in m/s^2 at monotonic timestamp now_ms.
     * Returns true ONLY on the exact rising edge of a validated pickup trigger.
     */
    bool feed(float ax, float ay, float az, int64_t now_ms) {
        const AccelAngles ang = computeAngles(ax, ay, az);
        if (!ang.valid) {
            holding_ = false;
            return false;
        }

        const bool norm_ok = (ang.norm >= cfg_.min_gravity_norm &&
                              ang.norm <= cfg_.max_gravity_norm);
        if (!norm_ok) {
            /* Free-fall, impact, or strong shake: invalidate any active hold. */
            holding_ = false;
            return false;
        }

        const bool is_flat_rest =
            (az > 0.0f &&
             ang.pitch_deg > -cfg_.flat_rest_max_pitch_deg &&
             ang.pitch_deg < cfg_.flat_rest_max_pitch_deg);
        const bool is_pocket_or_facedown_rest =
            (ang.pitch_deg <= cfg_.pocket_upside_down_deg ||
             ang.pitch_deg >= cfg_.pocket_upright_away_deg);

        if (is_flat_rest || is_pocket_or_facedown_rest) {
            holding_ = false;
            recordRestSample(now_ms, ang.pitch_deg, is_flat_rest || ang.pitch_deg <= cfg_.pocket_upside_down_deg);
            return false;
        }

        const bool in_viewing_pose =
            (ay > 0.0f &&
             az > 0.0f &&
             ang.pitch_deg >= cfg_.min_pitch_deg &&
             ang.pitch_deg <= cfg_.max_pitch_deg &&
             std::fabs(ang.roll_deg) <= cfg_.max_roll_deg);

        if (!in_viewing_pose) {
            holding_ = false;
            return false;
        }

        /* Screen-on gate: suppress and disarm if user already raised while screen is on. */
        if (!isScreenOff()) {
            resetGesture();
            return false;
        }

        /* Must have a qualifying recent resting baseline with pitch delta >= min_delta_pitch_deg. */
        if (!hasQualifyingBaseline(now_ms, ang.pitch_deg)) {
            holding_ = false;
            return false;
        }

        /* Enforce stability hold (>= 120 ms) and bounded accel spread during hold. */
        if (!holding_) {
            startHold(now_ms, ax, ay, az);
            return false;
        }

        if (now_ms < hold_start_ms_) {
            startHold(now_ms, ax, ay, az);
            return false;
        }

        const float nmin_x = (ax < hold_min_[0]) ? ax : hold_min_[0];
        const float nmax_x = (ax > hold_max_[0]) ? ax : hold_max_[0];
        const float nmin_y = (ay < hold_min_[1]) ? ay : hold_min_[1];
        const float nmax_y = (ay > hold_max_[1]) ? ay : hold_max_[1];
        const float nmin_z = (az < hold_min_[2]) ? az : hold_min_[2];
        const float nmax_z = (az > hold_max_[2]) ? az : hold_max_[2];

        if ((nmax_x - nmin_x) > cfg_.max_accel_spread ||
            (nmax_y - nmin_y) > cfg_.max_accel_spread ||
            (nmax_z - nmin_z) > cfg_.max_accel_spread) {
            /* Excessive motion spread during hold: restart stability window from current sample. */
            startHold(now_ms, ax, ay, az);
            return false;
        }

        hold_min_[0] = nmin_x; hold_max_[0] = nmax_x;
        hold_min_[1] = nmin_y; hold_max_[1] = nmax_y;
        hold_min_[2] = nmin_z; hold_max_[2] = nmax_z;

        if ((now_ms - hold_start_ms_) < cfg_.stability_hold_ms) {
            return false;
        }

        /* Enforce 2000 ms cooldown after last trigger. */
        if (has_triggered_ && (now_ms - last_trigger_ms_) < cfg_.cooldown_ms) {
            return false;
        }

        /* Validated raise-to-wake trigger: fire once and disarm until phone returns to rest. */
        has_triggered_ = true;
        last_trigger_ms_ = now_ms;
        last_trigger_pitch_deg_ = ang.pitch_deg;
        last_trigger_roll_deg_ = ang.roll_deg;
        resetGesture();
        return true;
    }

    float lastTriggerPitchDeg() const { return last_trigger_pitch_deg_; }
    float lastTriggerRollDeg() const { return last_trigger_roll_deg_; }
    bool hasTriggered() const { return has_triggered_; }
    int64_t lastTriggerMs() const { return last_trigger_ms_; }

private:
    struct RestEntry {
        int64_t time_ms = 0;
        float pitch_deg = 0.0f;
        bool signed_upward = true;
    };

    static constexpr std::size_t kRestCap = 64;

    bool isScreenOff() const {
        if (!is_screen_off_) {
            return true;
        }
        return is_screen_off_();
    }

    void startHold(int64_t now_ms, float ax, float ay, float az) {
        holding_ = true;
        hold_start_ms_ = now_ms;
        hold_min_ = {ax, ay, az};
        hold_max_ = {ax, ay, az};
    }

    void recordRestSample(int64_t now_ms, float pitch_deg, bool signed_upward) {
        /* If timestamps move backward, clear stale history. */
        if (rest_count_ > 0) {
            const std::size_t newest = (rest_head_ + rest_count_ - 1) % kRestCap;
            if (now_ms < rest_buf_[newest].time_ms) {
                rest_count_ = 0;
                rest_head_ = 0;
            }
        }

        if (rest_count_ == kRestCap) {
            rest_head_ = (rest_head_ + 1) % kRestCap;
            --rest_count_;
        }
        const std::size_t idx = (rest_head_ + rest_count_) % kRestCap;
        rest_buf_[idx] = RestEntry{now_ms, pitch_deg, signed_upward};
        ++rest_count_;

        /* Prune resting entries older than baseline_window_ms relative to newest resting sample,
         * always keeping at least the newest resting sample. */
        while (rest_count_ > 1) {
            if ((now_ms - rest_buf_[rest_head_].time_ms) > cfg_.baseline_window_ms) {
                rest_head_ = (rest_head_ + 1) % kRestCap;
                --rest_count_;
            } else {
                break;
            }
        }
    }

    bool hasQualifyingBaseline(int64_t now_ms, float current_pitch_deg) const {
        if (rest_count_ == 0) {
            return false;
        }
        const std::size_t newest = (rest_head_ + rest_count_ - 1) % kRestCap;
        const int64_t latest_rest_ms = rest_buf_[newest].time_ms;
        if (now_ms < latest_rest_ms || (now_ms - latest_rest_ms) > cfg_.max_gesture_window_ms) {
            return false;
        }

        for (std::size_t i = 0; i < rest_count_; ++i) {
            const RestEntry &e = rest_buf_[(rest_head_ + i) % kRestCap];
            if ((now_ms - e.time_ms) > cfg_.max_gesture_window_ms) {
                continue;
            }
            float delta = 0.0f;
            if (e.signed_upward) {
                delta = current_pitch_deg - e.pitch_deg;
            } else {
                delta = std::fabs(current_pitch_deg - e.pitch_deg);
                if (delta > 180.0f) {
                    delta = 360.0f - delta;
                }
            }
            if (delta >= cfg_.min_delta_pitch_deg) {
                return true;
            }
        }
        return false;
    }

    PickupConfig cfg_{};
    ScreenOffFn is_screen_off_{};

    std::array<RestEntry, kRestCap> rest_buf_{};
    std::size_t rest_head_ = 0;
    std::size_t rest_count_ = 0;

    bool holding_ = false;
    int64_t hold_start_ms_ = 0;
    std::array<float, 3> hold_min_{0.0f, 0.0f, 0.0f};
    std::array<float, 3> hold_max_{0.0f, 0.0f, 0.0f};

    bool has_triggered_ = false;
    int64_t last_trigger_ms_ = 0;
    float last_trigger_pitch_deg_ = 0.0f;
    float last_trigger_roll_deg_ = 0.0f;
};

} // namespace marble_motion

#endif // MARBLE_MOTION_PICKUP_DETECTOR_H
