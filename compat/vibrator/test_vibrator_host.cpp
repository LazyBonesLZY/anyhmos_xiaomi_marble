/* SPDX-License-Identifier: MIT */
/*
 * test_vibrator_host.cpp - Strict offline host unit test for Xiaomi marble Vibrator HDI v2.0.
 *
 * Verifies:
 *   1. All donor ABI struct sizes and member offsets (static_assert + runtime checks).
 *   2. Preset effect resolution table (15-120ms tactile presets + 40ms unknown fallback).
 *   3. FF_CONSTANT effect builder (clamping, level/gain scaling, strict rejection of FF_RUMBLE).
 *   4. Device name auto-discovery matcher.
 *   5. Composite / HapticPaket / VibratorPackage timeline builders.
 *   6. VibratorController state machine + background worker thread + preemption & session cancellation
 *      using a strict MockEvdevTransport modeled after SM7475 qcom-hv-haptics.ko.
 */

#include "marble_vibrator_backend.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace OHOS::HDI::Vibrator::V2_0;
using namespace marble_vibrator;

namespace {

struct RecordedEffect {
    uint16_t type;
    int16_t id;
    uint16_t length;
    int16_t level;
};

struct RecordedTrigger {
    int16_t id;
    int32_t value;
};

class MockEvdevTransport : public EvdevTransport {
public:
    bool openDevice() override {
        std::lock_guard<std::mutex> lk(mu_);
        openCount_++;
        if (failOpen_)
            return false;
        open_ = true;
        return true;
    }

    bool isOpen() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return open_;
    }

    void closeDevice() override {
        std::lock_guard<std::mutex> lk(mu_);
        open_ = false;
    }

    int uploadEffect(struct ff_effect &eff) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!open_)
            return -ENODEV;
        if (failNextUploadWith_ != 0) {
            int err = failNextUploadWith_;
            failNextUploadWith_ = 0;
            return err;
        }
        /* Match SM7475 qcom-hv-haptics.ko: strictly reject FF_RUMBLE! */
        if (eff.type != FF_CONSTANT && eff.type != FF_PERIODIC)
            return -EINVAL;
        if (eff.id != -1)
            return -EINVAL;
        int16_t assigned = nextId_++;
        eff.id = assigned;
        liveIds_.insert(assigned);
        uploads_.push_back(RecordedEffect{eff.type, eff.id, eff.replay.length, eff.u.constant.level});
        return 0;
    }

    int writeGain(uint16_t gain) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!open_)
            return -ENODEV;
        gains_.push_back(gain);
        return 0;
    }

    int writeTrigger(int16_t effectId, int32_t value) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!open_)
            return -ENODEV;
        if (liveIds_.find(effectId) == liveIds_.end())
            return -EINVAL;
        triggers_.push_back(RecordedTrigger{effectId, value});
        return 0;
    }

    int removeEffect(int16_t effectId) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!open_)
            return -ENODEV;
        if (liveIds_.erase(effectId) == 0)
            return -EINVAL;
        removedIds_.push_back(effectId);
        return 0;
    }

    size_t uploadCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return uploads_.size();
    }

    RecordedEffect lastUpload() const {
        std::lock_guard<std::mutex> lk(mu_);
        assert(!uploads_.empty());
        return uploads_.back();
    }

    size_t liveIdCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return liveIds_.size();
    }

    std::vector<RecordedEffect> uploads() const {
        std::lock_guard<std::mutex> lk(mu_);
        return uploads_;
    }

    std::vector<uint16_t> gains() const {
        std::lock_guard<std::mutex> lk(mu_);
        return gains_;
    }

    std::vector<RecordedTrigger> triggers() const {
        std::lock_guard<std::mutex> lk(mu_);
        return triggers_;
    }

    void setFailNextUpload(int err) {
        std::lock_guard<std::mutex> lk(mu_);
        failNextUploadWith_ = err;
    }

    void setFailOpen(bool fail) {
        std::lock_guard<std::mutex> lk(mu_);
        failOpen_ = fail;
    }

    int openCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return openCount_;
    }

private:
    mutable std::mutex mu_;
    bool open_ = false;
    bool failOpen_ = false;
    int failNextUploadWith_ = 0;
    int openCount_ = 0;
    int16_t nextId_ = 1;
    std::set<int16_t> liveIds_;
    std::vector<RecordedEffect> uploads_;
    std::vector<uint16_t> gains_;
    std::vector<RecordedTrigger> triggers_;
    std::vector<int16_t> removedIds_;
};

void testStructSizes() {
    assert(sizeof(DeviceVibratorInfo) == 8);
    assert(sizeof(HdfEffectInfo) == 8);
    assert(sizeof(HdfVibratorInfo) == 32);
    assert(offsetof(HdfVibratorInfo, reserved) == 28);
    assert(sizeof(HapticCapacity) == 16);
    assert(sizeof(HdfWaveInformation) == 24);
    assert(sizeof(TimeEffect) == 16);
    assert(sizeof(PrimitiveEffect) == 16);
    assert(sizeof(CompositeEffect) == 16);
    assert(sizeof(CurvePoint) == 16);
    assert(sizeof(HdfCompositeEffect) == 32);
    assert(sizeof(HapticEvent) == 56);
    assert(sizeof(HapticPaket) == 32);
    assert(sizeof(VibratorPackage) == 32);
    std::printf("[PASS] testStructSizes\n");
}

void testFfConstantBuilder() {
    struct ff_effect effLow = buildConstantEffect(1, 0);
    assert(effLow.type == FF_CONSTANT);
    assert(effLow.id == -1);
    assert(effLow.replay.length == 10);
    assert(effLow.u.constant.level >= 1 && effLow.u.constant.level <= 0x7fff);

    struct ff_effect effMid = buildConstantEffect(50, 50);
    assert(effMid.type == FF_CONSTANT);
    assert(effMid.replay.length == 50);
    assert(effMid.u.constant.level == 16384);

    struct ff_effect effMax = buildConstantEffect(99999, 250);
    assert(effMax.type == FF_CONSTANT);
    assert(effMax.replay.length == 10000);
    assert(effMax.u.constant.level == 0x7fff);

    assert(intensityToGainValue(100) == 0xffff);
    assert(intensityToGainValue(50) == 0x8000);
    assert(intensityToGainValue(1) >= 1);

    struct input_event ev = buildFfEvent(FF_GAIN, 0x8000);
    assert(ev.type == EV_FF && ev.code == FF_GAIN && ev.value == 0x8000);

    assert(isHapticsDeviceName("qcom-hv-haptics"));
    assert(isHapticsDeviceName("QCOM_HV_HAPTICS"));
    assert(isHapticsDeviceName("aw8697_vibrator"));
    assert(!isHapticsDeviceName("goodix_ts"));
    assert(!isHapticsDeviceName("gpio-keys"));
    assert(!isHapticsDeviceName(""));
    assert(!isHapticsDeviceName(nullptr));
    std::printf("[PASS] testFfConstantBuilder\n");
}

void testPresetMapping() {
    auto checkKnown = [](const char *name, uint32_t minMs, uint32_t maxMs, size_t minSegs) {
        PresetResolution r = resolvePresetEffect(name);
        assert(r.known);
        assert(!r.isNull);
        assert(r.totalDurationMs >= minMs && r.totalDurationMs <= maxMs);
        assert(r.segments.size() >= minSegs);
    };

    checkKnown("haptic.clock.timer", 15, 120, 1);
    checkKnown("haptic.effect.soft", 15, 120, 1);
    checkKnown("haptic.effect.hard", 15, 120, 1);
    checkKnown("haptic.effect.sharp", 15, 120, 1);
    checkKnown("haptic.common.click", 15, 120, 1);
    checkKnown("haptic.common.double_click", 15, 120, 2);
    checkKnown("haptic.notice.success", 15, 120, 2);
    checkKnown("haptic.notice.warning", 15, 120, 2);
    checkKnown("haptic.notice.fail", 15, 120, 3);
    checkKnown("haptic.notice.custom_ping", 15, 120, 2);
    checkKnown("haptic.slide.light", 15, 120, 1);
    checkKnown("haptic.slide.heavy", 15, 120, 1);
    checkKnown("haptic.long_press.light", 15, 120, 1);
    checkKnown("haptic.long_press.medium", 15, 120, 1);
    checkKnown("haptic.long_press.custom", 15, 120, 1);

    PresetResolution nullPreset = resolvePresetEffect("haptic.null");
    assert(nullPreset.known && nullPreset.isNull && nullPreset.totalDurationMs == 0);

    PresetResolution unknownHaptic = resolvePresetEffect("haptic.vendor.unknown_xyz");
    assert(unknownHaptic.known);
    assert(unknownHaptic.totalDurationMs == 40);
    assert(unknownHaptic.segments.size() == 1 && unknownHaptic.segments[0].durationMs == 40);

    PresetResolution arbitrary = resolvePresetEffect("some_unknown_preset");
    assert(!arbitrary.known);
    assert(arbitrary.totalDurationMs == 40);
    assert(arbitrary.segments.size() == 1 && arbitrary.segments[0].durationMs == 40);
    std::printf("[PASS] testPresetMapping\n");
}

void testCompositeAndPatternBuilders() {
    HdfCompositeEffect comp{};
    comp.type = HDF_EFFECT_TYPE_TIME;
    CompositeEffect ce1{};
    ce1.timeEffect = TimeEffect{10, 30, 60, 0, 0};
    CompositeEffect ce2{};
    ce2.timeEffect = TimeEffect{20, 50, 90, 0, 0};
    comp.compositeEffects.push_back(ce1);
    comp.compositeEffects.push_back(ce2);

    std::vector<VibratorSegment> segs;
    assert(buildSegmentsFromComposite(comp, segs));
    assert(segs.size() == 2);
    assert(segs[0].delayMs == 10 && segs[0].durationMs == 30 && segs[0].intensity == 60);
    assert(segs[1].delayMs == 20 && segs[1].durationMs == 50 && segs[1].intensity == 90);

    HdfCompositeEffect prim{};
    prim.type = HDF_EFFECT_TYPE_PRIMITIVE;
    CompositeEffect pe{};
    pe.primitiveEffect = PrimitiveEffect{5, 2, 75, 0};
    prim.compositeEffects.push_back(pe);
    assert(buildSegmentsFromComposite(prim, segs));
    assert(segs.size() == 1 && segs[0].delayMs == 5 && segs[0].durationMs == 30 && segs[0].intensity == 75);

    HdfCompositeEffect bad{};
    bad.type = HDF_EFFECT_TYPE_BUTT;
    bad.compositeEffects.push_back(pe);
    assert(!buildSegmentsFromComposite(bad, segs));

    /* HapticPaket with out-of-order events and CurvePoint intensity fallback */
    HapticPaket pkt{};
    pkt.time = 10;
    pkt.eventNum = 2;
    HapticEvent evLate{};
    evLate.type = CONTINUOUS;
    evLate.time = 80;
    evLate.duration = 40;
    evLate.intensity = 0;
    evLate.points.push_back(CurvePoint{0, 20, 0, 0});
    evLate.points.push_back(CurvePoint{20, 88, 0, 0});
    HapticEvent evEarly{};
    evEarly.type = TRANSIENT;
    evEarly.time = 0;
    evEarly.duration = 0; /* should default to 20ms */
    evEarly.intensity = 65;
    pkt.events.push_back(evLate);
    pkt.events.push_back(evEarly);

    assert(buildSegmentsFromPaket(pkt, segs));
    assert(segs.size() == 2);
    assert(segs[0].delayMs == 10 && segs[0].durationMs == 20 && segs[0].intensity == 65);
    /* cursor after evEarly is 10 + 20 = 30; evLate absStart = 10 + 80 = 90 => gap = 60 */
    assert(segs[1].delayMs == 60 && segs[1].durationMs == 40 && segs[1].intensity == 88);

    VibratorPackage vpkg{};
    vpkg.patternNum = 2;
    vpkg.packageduration = 200;
    HapticPaket p1{};
    p1.time = 0;
    p1.eventNum = 1;
    p1.events.push_back(evEarly);
    HapticPaket p2{};
    p2.time = 100;
    p2.eventNum = 1;
    p2.events.push_back(evEarly);
    vpkg.patterns.push_back(p1);
    vpkg.patterns.push_back(p2);
    assert(buildSegmentsFromPackage(vpkg, segs));
    assert(segs.size() == 2);
    assert(segs[0].delayMs == 0 && segs[0].durationMs == 20);
    assert(segs[1].delayMs == 80 && segs[1].durationMs == 20);
    std::printf("[PASS] testCompositeAndPatternBuilders\n");
}

void testControllerPlaybackAndPreemption() {
    MockEvdevTransport mock;
    VibratorController ctrl(&mock);

    /* 1. StartOnce(50) -> verify FF_CONSTANT, FF_GAIN, trigger, IsRunning, Stop cleanup */
    assert(ctrl.startOnce(50, 80) == 0);
    assert(ctrl.isRunning());
    assert(mock.uploadCount() == 1);
    assert(mock.lastUpload().type == FF_CONSTANT);
    assert(mock.lastUpload().length == 50);
    assert(mock.lastUpload().level == intensityToConstantLevel(80));
    assert(mock.liveIdCount() == 1);
    assert(!mock.gains().empty() && mock.gains().back() == intensityToGainValue(80));
    assert(!mock.triggers().empty() && mock.triggers().back().value == 1);

    assert(ctrl.stopAll() == 0);
    assert(!ctrl.isRunning());
    assert(mock.liveIdCount() == 0);
    assert(mock.triggers().back().value == 0);

    /* 2. StartOnce(0) cleanly stops without uploading */
    size_t beforeZero = mock.uploadCount();
    assert(ctrl.startOnce(0) == 0);
    assert(mock.uploadCount() == beforeZero);
    assert(!ctrl.isRunning());

    /* 3. Multi-segment pattern runs cleanly to completion on worker thread */
    std::vector<VibratorSegment> twoPulse = {{0, 20, 60}, {15, 25, 90}};
    size_t beforePat = mock.uploadCount();
    assert(ctrl.startSegments(twoPulse, 10u) == 0);
    assert(ctrl.isRunning());

    for (int i = 0; i < 60 && ctrl.isRunning(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!ctrl.isRunning());
    assert(mock.uploadCount() == beforePat + 2);
    assert(mock.liveIdCount() == 0);
    auto ups = mock.uploads();
    assert(ups[beforePat].length == 20 && ups[beforePat].level == intensityToConstantLevel(60));
    assert(ups[beforePat + 1].length == 25 && ups[beforePat + 1].level == intensityToConstantLevel(90));

    /* 4. Session-scoped cancellation & instant preemption */
    std::vector<VibratorSegment> longPattern = {{0, 400, 70}, {100, 400, 80}, {100, 400, 90}};
    assert(ctrl.startSegments(longPattern, 77u) == 0);
    assert(ctrl.isRunning());
    assert(ctrl.activeSessionId() == 77u);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    /* Wrong session ID must NOT stop active session 77 */
    assert(ctrl.stopBySessionId(99u) == 0);
    assert(ctrl.isRunning());
    assert(ctrl.activeSessionId() == 77u);

    /* Matching session ID stops immediately and cleans up FF slot */
    auto t0 = std::chrono::steady_clock::now();
    assert(ctrl.stopBySessionId(77u) == 0);
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
    assert(elapsedMs < 50);
    assert(!ctrl.isRunning());
    assert(ctrl.activeSessionId() == 0u);
    assert(mock.liveIdCount() == 0);

    /* 5. Transient -ENODEV recovery triggers close + reopen + retry */
    int opensBefore = mock.openCount();
    mock.setFailNextUpload(-ENODEV);
    assert(ctrl.startOnce(30, 50) == 0);
    assert(mock.openCount() == opensBefore + 1);
    assert(ctrl.stopAll() == 0);
    assert(mock.liveIdCount() == 0);

    ctrl.shutdown();
    std::printf("[PASS] testControllerPlaybackAndPreemption\n");
}

} // namespace

int main(void) {
    testStructSizes();
    testFfConstantBuilder();
    testPresetMapping();
    testCompositeAndPatternBuilders();
    testControllerPlaybackAndPreemption();
    std::printf("ALL_HOST_TESTS_PASSED=1\n");
    return 0;
}
