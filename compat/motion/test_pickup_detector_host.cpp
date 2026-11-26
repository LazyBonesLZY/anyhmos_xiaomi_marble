/* SPDX-License-Identifier: MIT */
/*
 * test_pickup_detector_host.cpp - Strict offline host unit test for marble PickupDetector.
 *
 * Verifies:
 *   1. Flat-on-table (0 deg) -> lift to 55 deg triggers ONCE (and does not repeat while held).
 *   2. 2000 ms cooldown blocks rapid repeat; trigger succeeds after >= 2000 ms.
 *   3. Screen-on suppresses firing (and disarms so turning screen off while already held won't false-wake).
 *   4. Excessive roll (> 35 deg, threshold 25 deg) suppresses firing.
 *   5. Brief drop / shake (< 120 ms hold or out-of-range gravity norm or high spread) suppresses firing.
 *   6. Face-down (-55 deg, +125 deg, -125 deg) suppresses firing.
 *   7. Smooth ramp lift (0 -> 10 -> 22 -> 38 -> 55 deg) triggers cleanly.
 *   8. Pocket vertical upside-down (-90 deg) -> raised to 55 deg triggers cleanly.
 *   9. Insufficient pitch delta (resting at 20 deg -> raised to 55 deg, delta 35 < 45) suppresses firing.
 */

#include "pickup_detector.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr float kG = 9.81f;
constexpr float kDegToRad = 0.017453292519943295f;

struct Vec3 {
    float x;
    float y;
    float z;
};

/* Construct gravity vector (m/s^2) with exact pitch_deg in YZ plane and lateral roll_deg. */
static Vec3 accelForPose(float pitch_deg, float roll_deg, float g = kG) {
    const float pr = pitch_deg * kDegToRad;
    const float rr = roll_deg * kDegToRad;
    const float ax = g * std::sin(rr);
    const float yz = g * std::cos(rr);
    const float ay = yz * std::sin(pr);
    const float az = yz * std::cos(pr);
    return Vec3{ax, ay, az};
}

static int g_failures = 0;

#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

static int feedPoseSequence(marble_motion::PickupDetector &det, Vec3 v,
                            int64_t start_ms, int count, int64_t step_ms) {
    int triggers = 0;
    for (int i = 0; i < count; ++i) {
        if (det.feed(v.x, v.y, v.z, start_ms + i * step_ms)) {
            ++triggers;
        }
    }
    return triggers;
}

static void testFlatTo55TriggersOnce() {
    bool screen_off = true;
    marble_motion::PickupDetector det(marble_motion::PickupConfig{}, [&]() { return screen_off; });

    /* Rest flat on table at 0 deg for 400 ms (25 Hz = 40 ms step). */
    int t0 = feedPoseSequence(det, accelForPose(0.0f, 0.0f), 0, 10, 40);
    CHECK(t0 == 0, "resting flat must not trigger");

    /* Lift to 55 deg and hold for 800 ms (20 samples @ 40 ms). Must trigger ONCE at >= 120 ms hold. */
    int triggers = 0;
    int64_t trigger_time = -1;
    const Vec3 pose55 = accelForPose(55.0f, 0.0f);
    for (int i = 0; i < 20; ++i) {
        const int64_t ts = 400 + i * 40;
        if (det.feed(pose55.x, pose55.y, pose55.z, ts)) {
            ++triggers;
            trigger_time = ts;
        }
    }
    CHECK(triggers == 1, "flat -> 55 deg lift must trigger exactly ONCE");
    CHECK(trigger_time == 520, "trigger must fire at 120 ms stability hold (400 + 120 = 520)");
    CHECK(std::fabs(det.lastTriggerPitchDeg() - 55.0f) < 0.5f, "recorded trigger pitch ~55 deg");
    CHECK(std::fabs(det.lastTriggerRollDeg()) < 0.5f, "recorded trigger roll ~0 deg");
    std::printf("PASS testFlatTo55TriggersOnce (trigger_ts=%lld ms)\n", static_cast<long long>(trigger_time));
}

static void testCooldownBlocksRepeat() {
    bool screen_off = true;
    marble_motion::PickupDetector det(marble_motion::PickupConfig{}, [&]() { return screen_off; });

    /* First lift at t=0..600 -> triggers at t=520. */
    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 0, 10, 40);
    int first = feedPoseSequence(det, accelForPose(55.0f, 0.0f), 400, 10, 40);
    CHECK(first == 1, "first lift must trigger");
    CHECK(det.lastTriggerMs() == 520, "first trigger timestamp == 520");

    /* Quickly lower back flat (t=900..1100) and lift again (t=1200..1800): within 2000 ms cooldown! */
    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 900, 6, 40);
    int during_cooldown = feedPoseSequence(det, accelForPose(55.0f, 0.0f), 1200, 15, 40);
    CHECK(during_cooldown == 0, "repeat lift within 2000 ms cooldown must be blocked");

    /* Now lower flat at t=2400..2600 and lift at t=2600..3000 (> 520 + 2000 = 2520): must trigger! */
    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 2400, 5, 40);
    int after_cooldown = feedPoseSequence(det, accelForPose(55.0f, 0.0f), 2600, 10, 40);
    CHECK(after_cooldown == 1, "lift after 2000 ms cooldown must trigger");
    std::printf("PASS testCooldownBlocksRepeat\n");
}

static void testScreenOnSuppresses() {
    bool screen_off = false; /* Screen is ON! */
    marble_motion::PickupDetector det(marble_motion::PickupConfig{}, [&]() { return screen_off; });

    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 0, 10, 40);
    int while_on = feedPoseSequence(det, accelForPose(55.0f, 0.0f), 400, 15, 40);
    CHECK(while_on == 0, "lift while screen is ON must be suppressed");

    /* Turning screen OFF while STILL holding at 55 deg must NOT spontaneously trigger. */
    screen_off = true;
    int still_held = feedPoseSequence(det, accelForPose(55.0f, 0.0f), 1000, 15, 40);
    CHECK(still_held == 0, "turning screen off while already holding at 55 deg must not false-trigger");

    /* Lowering flat then lifting while screen is OFF must trigger normally. */
    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 1700, 5, 40);
    int valid_after = feedPoseSequence(det, accelForPose(55.0f, 0.0f), 2000, 10, 40);
    CHECK(valid_after == 1, "fresh lift after screen turns off must trigger");
    std::printf("PASS testScreenOnSuppresses\n");
}

static void testExcessiveRollSuppresses() {
    bool screen_off = true;
    marble_motion::PickupDetector det(marble_motion::PickupConfig{}, [&]() { return screen_off; });

    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 0, 10, 40);
    int roll35 = feedPoseSequence(det, accelForPose(55.0f, 35.0f), 400, 15, 40);
    CHECK(roll35 == 0, "roll +35 deg (> 25 deg max) must be suppressed");

    int roll_neg40 = feedPoseSequence(det, accelForPose(55.0f, -40.0f), 1100, 15, 40);
    CHECK(roll_neg40 == 0, "roll -40 deg (> 25 deg max) must be suppressed");

    /* Moderate roll within limit (+15 deg) from flat must trigger. */
    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 1800, 5, 40);
    int roll15 = feedPoseSequence(det, accelForPose(55.0f, 15.0f), 2100, 10, 40);
    CHECK(roll15 == 1, "roll +15 deg (<= 25 deg max) must trigger");
    std::printf("PASS testExcessiveRollSuppresses\n");
}

static void testBriefDropAndShakeSuppress() {
    bool screen_off = true;
    marble_motion::PickupDetector det(marble_motion::PickupConfig{}, [&]() { return screen_off; });

    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 0, 10, 40);

    /* Brief 80 ms flash at 55 deg (< 120 ms stability requirement), then return flat. */
    int brief = feedPoseSequence(det, accelForPose(55.0f, 0.0f), 400, 3, 40); /* 400, 440, 480 => 80 ms span */
    CHECK(brief == 0, "80 ms viewing pose (< 120 ms) must not trigger");
    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 520, 5, 40);

    /* Drop / free-fall (norm = 1.5 m/s^2) and impact spike (norm = 18.0 m/s^2) during 55 deg pose. */
    int drop_trig = 0;
    const Vec3 pose55 = accelForPose(55.0f, 0.0f);
    const Vec3 freefall = accelForPose(55.0f, 0.0f, 1.5f);
    const Vec3 impact = accelForPose(55.0f, 0.0f, 18.0f);
    if (det.feed(pose55.x, pose55.y, pose55.z, 800)) ++drop_trig;
    if (det.feed(pose55.x, pose55.y, pose55.z, 840)) ++drop_trig;
    if (det.feed(freefall.x, freefall.y, freefall.z, 880)) ++drop_trig;
    if (det.feed(pose55.x, pose55.y, pose55.z, 920)) ++drop_trig;
    if (det.feed(impact.x, impact.y, impact.z, 960)) ++drop_trig;
    if (det.feed(pose55.x, pose55.y, pose55.z, 1000)) ++drop_trig;
    CHECK(drop_trig == 0, "free-fall / impact spikes breaking 120 ms hold must not trigger");

    /* Rapid violent shake oscillating accel spread > 4.8 m/s^2 inside [40, 82] deg. */
    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 1200, 5, 40);
    int shake_trig = 0;
    const Vec3 hi = accelForPose(78.0f, 20.0f, 13.0f);
    const Vec3 lo = accelForPose(42.0f, -20.0f, 7.0f);
    for (int i = 0; i < 12; ++i) {
        const Vec3 v = (i % 2 == 0) ? hi : lo;
        if (det.feed(v.x, v.y, v.z, 1500 + i * 40)) {
            ++shake_trig;
        }
    }
    CHECK(shake_trig == 0, "violent shake with spread > 4.8 m/s^2 must not trigger");
    std::printf("PASS testBriefDropAndShakeSuppress\n");
}

static void testFaceDownSuppresses() {
    bool screen_off = true;
    marble_motion::PickupDetector det(marble_motion::PickupConfig{}, [&]() { return screen_off; });

    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 0, 10, 40);

    /* Upside-down / negative pitch -55 deg (ay < 0, az > 0). */
    int neg55 = feedPoseSequence(det, accelForPose(-55.0f, 0.0f), 400, 15, 40);
    CHECK(neg55 == 0, "pitch -55 deg must be suppressed");

    /* Screen facing floor tilted (+125 deg: ay > 0, az < 0). */
    int pos125 = feedPoseSequence(det, accelForPose(125.0f, 0.0f), 1100, 15, 40);
    CHECK(pos125 == 0, "face-down tilted +125 deg (az < 0) must be suppressed");

    /* Screen facing floor upside-down (-125 deg: ay < 0, az < 0). */
    int neg125 = feedPoseSequence(det, accelForPose(-125.0f, 0.0f), 1800, 15, 40);
    CHECK(neg125 == 0, "face-down upside-down -125 deg must be suppressed");

    /* Pure face-down flat (180 deg: az = -9.81). */
    int flat_down = feedPoseSequence(det, accelForPose(180.0f, 0.0f), 2500, 15, 40);
    CHECK(flat_down == 0, "flat face-down 180 deg must be suppressed");
    std::printf("PASS testFaceDownSuppresses\n");
}

static void testSmoothRampAndPocketAndDeltaGate() {
    bool screen_off = true;
    marble_motion::PickupDetector det(marble_motion::PickupConfig{}, [&]() { return screen_off; });

    /* 1) Smooth human lift from table: 0 -> 10 -> 20 -> 24 -> 36 -> 48 -> 55 deg. */
    (void)feedPoseSequence(det, accelForPose(0.0f, 0.0f), 0, 10, 40);
    const float ramp[] = {8.0f, 16.0f, 24.0f, 36.0f, 48.0f, 55.0f, 55.0f, 55.0f, 55.0f, 55.0f};
    int ramp_trig = 0;
    for (int i = 0; i < 10; ++i) {
        const Vec3 v = accelForPose(ramp[i], 3.0f);
        if (det.feed(v.x, v.y, v.z, 400 + i * 40)) {
            ++ramp_trig;
        }
    }
    CHECK(ramp_trig == 1, "smooth human ramp lift from 0 -> 55 deg must trigger ONCE");

    /* 2) Pocket vertical upside-down (-90 deg) -> raised to 55 deg after cooldown. */
    (void)feedPoseSequence(det, accelForPose(-90.0f, 0.0f), 3000, 10, 40);
    int pocket_trig = feedPoseSequence(det, accelForPose(55.0f, 5.0f), 3500, 10, 40);
    CHECK(pocket_trig == 1, "pocket upside-down (-90 deg) -> 55 deg lift must trigger ONCE");

    /* 3) Resting on angled stand at 20 deg for > 2s -> lifted to 55 deg (delta = 35 < 45): suppress! */
    (void)feedPoseSequence(det, accelForPose(20.0f, 0.0f), 6000, 60, 40); /* 6000..8360 ms */
    int small_delta = feedPoseSequence(det, accelForPose(55.0f, 0.0f), 8500, 10, 40);
    CHECK(small_delta == 0, "delta_pitch 35 deg (< 45 deg min) must be suppressed");
    std::printf("PASS testSmoothRampAndPocketAndDeltaGate\n");
}

} // namespace

int main(void) {
    testFlatTo55TriggersOnce();
    testCooldownBlocksRepeat();
    testScreenOnSuppresses();
    testExcessiveRollSuppresses();
    testBriefDropAndShakeSuppress();
    testFaceDownSuppresses();
    testSmoothRampAndPocketAndDeltaGate();

    if (g_failures != 0) {
        std::fprintf(stderr, "HOST_PICKUP_DETECTOR_TEST_FAILED failures=%d\n", g_failures);
        return 1;
    }
    std::printf("HOST_PICKUP_DETECTOR_TEST_PASSED tests=7 failures=0\n");
    return 0;
}
