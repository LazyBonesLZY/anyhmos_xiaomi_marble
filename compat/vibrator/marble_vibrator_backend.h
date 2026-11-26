/* SPDX-License-Identifier: MIT */
#ifndef MARBLE_VIBRATOR_BACKEND_H
#define MARBLE_VIBRATOR_BACKEND_H

#include "v2_0/vibrator_types.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace marble_vibrator {

constexpr uint32_t kMinDurationMs = 10;
constexpr uint32_t kMaxDurationMs = 10000;
constexpr uint16_t kMinIntensity = 1;
constexpr uint16_t kMaxIntensity = 100;
constexpr uint16_t kDefaultOnceIntensity = 80;
constexpr uint16_t kDefaultPresetIntensity = 70;
constexpr uint32_t kFallbackPresetDurationMs = 40;
constexpr uint32_t kTransientDefaultDurationMs = 20;
constexpr size_t kMaxPatternSegments = 256;

struct VibratorSegment {
    uint32_t delayMs;
    uint32_t durationMs;
    uint16_t intensity;
};

struct PresetResolution {
    bool known;
    bool isNull;
    uint32_t totalDurationMs;
    std::vector<VibratorSegment> segments;
};

inline uint32_t clampDurationMs(uint32_t durationMs) {
    if (durationMs < kMinDurationMs)
        return kMinDurationMs;
    if (durationMs > kMaxDurationMs)
        return kMaxDurationMs;
    return durationMs;
}

inline uint16_t clampIntensityPercent(int32_t intensity) {
    if (intensity < static_cast<int32_t>(kMinIntensity))
        return kMinIntensity;
    if (intensity > static_cast<int32_t>(kMaxIntensity))
        return kMaxIntensity;
    return static_cast<uint16_t>(intensity);
}

inline int16_t intensityToConstantLevel(uint16_t intensityPercent) {
    uint32_t pct = clampIntensityPercent(intensityPercent);
    uint32_t level = (pct * 0x7fffu + 50u) / 100u;
    if (level < 1u)
        level = 1u;
    if (level > 0x7fffu)
        level = 0x7fffu;
    return static_cast<int16_t>(level);
}

inline uint16_t intensityToGainValue(uint16_t intensityPercent) {
    uint32_t pct = clampIntensityPercent(intensityPercent);
    uint32_t gain = (pct * 0xffffu + 50u) / 100u;
    if (gain < 1u)
        gain = 1u;
    if (gain > 0xffffu)
        gain = 0xffffu;
    return static_cast<uint16_t>(gain);
}

/*
 * SM7475 qcom-hv-haptics.ko ONLY accepts FF_CONSTANT (0x52) and FF_PERIODIC (0x51).
 * FF_RUMBLE (0x50) is rejected with -EINVAL.
 */
inline struct ff_effect buildConstantEffect(uint32_t durationMs, uint16_t intensityPercent) {
    struct ff_effect eff;
    std::memset(&eff, 0, sizeof(eff));
    eff.type = FF_CONSTANT;
    eff.id = -1;
    eff.direction = 0;
    eff.replay.length = static_cast<uint16_t>(clampDurationMs(durationMs));
    eff.replay.delay = 0;
    eff.u.constant.level = intensityToConstantLevel(intensityPercent);
    return eff;
}

inline struct input_event buildFfEvent(uint16_t code, int32_t value) {
    struct input_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = EV_FF;
    ev.code = code;
    ev.value = value;
    return ev;
}

inline std::string toLowerAscii(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

inline bool isHapticsDeviceName(const char *name) {
    if (name == nullptr || name[0] == '\0')
        return false;
    std::string lower = toLowerAscii(name);
    return lower.find("haptics") != std::string::npos ||
           lower.find("vibrator") != std::string::npos;
}

inline bool startsWith(const std::string &s, const char *prefix) {
    size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

inline PresetResolution makeSinglePreset(bool known, uint32_t durationMs, uint16_t intensity) {
    PresetResolution res{};
    res.known = known;
    res.isNull = false;
    res.totalDurationMs = durationMs;
    res.segments.push_back(VibratorSegment{0, durationMs, clampIntensityPercent(intensity)});
    return res;
}

inline PresetResolution makeMultiPreset(bool known, std::initializer_list<VibratorSegment> segs) {
    PresetResolution res{};
    res.known = known;
    res.isNull = false;
    uint32_t total = 0;
    for (const auto &s : segs) {
        VibratorSegment clamped{s.delayMs, clampDurationMs(s.durationMs), clampIntensityPercent(s.intensity)};
        res.segments.push_back(clamped);
        total += clamped.delayMs + clamped.durationMs;
    }
    res.totalDurationMs = total;
    return res;
}

inline PresetResolution resolvePresetEffect(const std::string &effectType) {
    if (effectType == "haptic.null") {
        PresetResolution res{};
        res.known = true;
        res.isNull = true;
        res.totalDurationMs = 0;
        return res;
    }

    /* Core crisp tactile presets (15-120ms) */
    if (effectType == "haptic.clock.timer")
        return makeSinglePreset(true, 20, 60);
    if (effectType == "haptic.effect.soft")
        return makeSinglePreset(true, 20, 45);
    if (effectType == "haptic.effect.hard")
        return makeSinglePreset(true, 50, 95);
    if (effectType == "haptic.effect.sharp")
        return makeSinglePreset(true, 25, 85);

    if (effectType == "haptic.common.click")
        return makeSinglePreset(true, 15, 65);
    if (effectType == "haptic.common.click_up")
        return makeSinglePreset(true, 15, 50);
    if (effectType == "haptic.common.tick")
        return makeSinglePreset(true, 15, 55);
    if (effectType == "haptic.common.double_click")
        return makeMultiPreset(true, {{0, 20, 70}, {75, 20, 70}});
    if (effectType == "haptic.common.long_press" ||
        effectType == "haptic.common.long_press1" ||
        effectType == "haptic.common.long_press2")
        return makeSinglePreset(true, 35, 80);

    if (effectType == "haptic.long_press.light" || effectType == "haptic.long_press_light")
        return makeSinglePreset(true, 25, 55);
    if (effectType == "haptic.long_press.medium" || effectType == "haptic.long_press_medium")
        return makeSinglePreset(true, 40, 75);
    if (effectType == "haptic.long_press.heavy" || effectType == "haptic.long_press_heavy")
        return makeSinglePreset(true, 55, 90);

    if (effectType == "haptic.slide" || effectType == "haptic.slide.light" ||
        effectType == "haptic.upglide" || effectType == "haptic.drag")
        return makeSinglePreset(true, 18, 50);

    if (effectType == "haptic.volume.max" || effectType == "haptic.volume.min")
        return makeSinglePreset(true, 25, 70);
    if (effectType == "haptic.camera.click" || effectType == "haptic.camera.shutter.action_down")
        return makeSinglePreset(true, 20, 75);
    if (effectType == "haptic.camera.focus" || effectType == "haptic.camera.gear_slip")
        return makeSinglePreset(true, 15, 55);
    if (effectType == "haptic.dialler.click")
        return makeSinglePreset(true, 18, 60);

    if (effectType == "haptic.notice.success")
        return makeMultiPreset(true, {{0, 25, 75}, {30, 25, 75}});
    if (effectType == "haptic.notice.warning")
        return makeMultiPreset(true, {{0, 35, 85}, {30, 35, 85}});
    if (effectType == "haptic.notice.fail" || effectType == "haptic.fail")
        return makeMultiPreset(true, {{0, 25, 90}, {22, 25, 90}, {23, 25, 90}});

    if (effectType == "haptic.charging")
        return makeSinglePreset(true, 60, 70);
    if (effectType == "haptic.threshold")
        return makeSinglePreset(true, 30, 80);
    if (effectType == "haptic.default.effect")
        return makeSinglePreset(true, 40, 70);

    if (effectType == "haptic.grade.strength1" || effectType == "haptic.grade.normal.strength1")
        return makeSinglePreset(true, 15, 30);
    if (effectType == "haptic.grade.strength2" || effectType == "haptic.grade.normal.strength2")
        return makeSinglePreset(true, 20, 50);
    if (effectType == "haptic.grade.strength3" || effectType == "haptic.grade.normal.strength3")
        return makeSinglePreset(true, 25, 70);
    if (effectType == "haptic.grade.strength4" || effectType == "haptic.grade.normal.strength4")
        return makeSinglePreset(true, 35, 85);
    if (effectType == "haptic.grade.strength5" || effectType == "haptic.grade.normal.strength5")
        return makeSinglePreset(true, 45, 100);

    /* Prefix families */
    if (startsWith(effectType, "haptic.notice."))
        return makeMultiPreset(true, {{0, 30, 80}, {30, 30, 80}});
    if (startsWith(effectType, "haptic.slide."))
        return makeSinglePreset(true, 18, 50);
    if (startsWith(effectType, "haptic.long_press.") || startsWith(effectType, "haptic.long_press_"))
        return makeSinglePreset(true, 35, 75);
    if (startsWith(effectType, "haptic.clock."))
        return makeSinglePreset(true, 20, 60);
    if (startsWith(effectType, "haptic.effect."))
        return makeSinglePreset(true, 30, 70);
    if (startsWith(effectType, "haptic.common."))
        return makeSinglePreset(true, 20, 65);
    if (startsWith(effectType, "haptic.camera."))
        return makeSinglePreset(true, 20, 65);
    if (startsWith(effectType, "haptic.control.") || startsWith(effectType, "haptic.contacts."))
        return makeSinglePreset(true, 18, 55);
    if (startsWith(effectType, "haptic.dialler."))
        return makeSinglePreset(true, 18, 60);
    if (startsWith(effectType, "haptic.pattern."))
        return makeMultiPreset(true, {{0, 45, 85}, {30, 45, 85}});
    if (startsWith(effectType, "haptic."))
        return makeSinglePreset(true, kFallbackPresetDurationMs, kDefaultPresetIntensity);

    /* Unknown non-haptic preset: fallback 40ms for playback, known=false for GetEffectInfo */
    return makeSinglePreset(false, kFallbackPresetDurationMs, kDefaultPresetIntensity);
}

inline uint32_t primitiveEffectDurationMs(int32_t effectId) {
    switch (effectId) {
    case 0:
        return 15;
    case 1:
        return 20;
    case 2:
        return 30;
    case 3:
        return 45;
    case 4:
        return 60;
    default:
        return 25;
    }
}

inline bool buildSegmentsFromComposite(const OHOS::HDI::Vibrator::V2_0::HdfCompositeEffect &effect,
                                       std::vector<VibratorSegment> &out) {
    using namespace OHOS::HDI::Vibrator::V2_0;
    out.clear();
    if (effect.compositeEffects.empty())
        return false;
    if (effect.type != HDF_EFFECT_TYPE_TIME && effect.type != HDF_EFFECT_TYPE_PRIMITIVE)
        return false;

    const size_t count = std::min(effect.compositeEffects.size(), kMaxPatternSegments);
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (effect.type == HDF_EFFECT_TYPE_TIME) {
            const TimeEffect &te = effect.compositeEffects[i].timeEffect;
            if (te.time <= 0)
                continue;
            uint32_t delayMs = te.delay > 0 ? static_cast<uint32_t>(te.delay) : 0u;
            uint32_t durMs = clampDurationMs(static_cast<uint32_t>(te.time));
            uint16_t inten = te.intensity > 0 ? clampIntensityPercent(te.intensity) : kDefaultOnceIntensity;
            out.push_back(VibratorSegment{delayMs, durMs, inten});
        } else {
            const PrimitiveEffect &pe = effect.compositeEffects[i].primitiveEffect;
            uint32_t delayMs = pe.delay > 0 ? static_cast<uint32_t>(pe.delay) : 0u;
            uint32_t durMs = clampDurationMs(primitiveEffectDurationMs(pe.effectId));
            uint16_t inten = pe.intensity > 0 ? clampIntensityPercent(pe.intensity) : kDefaultPresetIntensity;
            out.push_back(VibratorSegment{delayMs, durMs, inten});
        }
    }
    return !out.empty();
}

struct TimedEventItem {
    uint64_t absStartMs;
    uint32_t durationMs;
    uint16_t intensity;
};

inline void collectPaketEvents(const OHOS::HDI::Vibrator::V2_0::HapticPaket &pkg,
                               uint64_t baseOffsetMs,
                               std::vector<TimedEventItem> &items) {
    using namespace OHOS::HDI::Vibrator::V2_0;
    const uint64_t pkgStart = baseOffsetMs + (pkg.time > 0 ? static_cast<uint64_t>(pkg.time) : 0ULL);
    for (const auto &ev : pkg.events) {
        if (items.size() >= kMaxPatternSegments)
            break;
        uint64_t absStart = pkgStart + (ev.time > 0 ? static_cast<uint64_t>(ev.time) : 0ULL);
        uint32_t rawDur = 0;
        if (ev.duration > 0) {
            rawDur = static_cast<uint32_t>(ev.duration);
        } else if (ev.type == TRANSIENT) {
            rawDur = kTransientDefaultDurationMs;
        } else {
            rawDur = kTransientDefaultDurationMs;
        }
        int32_t rawIntensity = ev.intensity;
        if (rawIntensity <= 0 && !ev.points.empty()) {
            int32_t peak = 0;
            for (const auto &pt : ev.points) {
                if (pt.intensity > peak)
                    peak = pt.intensity;
            }
            rawIntensity = peak;
        }
        uint16_t inten = rawIntensity > 0 ? clampIntensityPercent(rawIntensity) : kDefaultPresetIntensity;
        items.push_back(TimedEventItem{absStart, clampDurationMs(rawDur), inten});
    }
}

inline bool finalizeTimedEvents(std::vector<TimedEventItem> &items, std::vector<VibratorSegment> &out) {
    out.clear();
    if (items.empty())
        return false;
    std::stable_sort(items.begin(), items.end(), [](const TimedEventItem &a, const TimedEventItem &b) {
        return a.absStartMs < b.absStartMs;
    });
    out.reserve(items.size());
    uint64_t cursorMs = 0;
    for (const auto &it : items) {
        uint64_t gap = (it.absStartMs > cursorMs) ? (it.absStartMs - cursorMs) : 0ULL;
        if (gap > 60000ULL)
            gap = 60000ULL;
        out.push_back(VibratorSegment{static_cast<uint32_t>(gap), it.durationMs, it.intensity});
        uint64_t segStart = (it.absStartMs > cursorMs) ? it.absStartMs : cursorMs;
        cursorMs = segStart + it.durationMs;
    }
    return !out.empty();
}

inline bool buildSegmentsFromPaket(const OHOS::HDI::Vibrator::V2_0::HapticPaket &pkg,
                                   std::vector<VibratorSegment> &out) {
    std::vector<TimedEventItem> items;
    items.reserve(std::min(pkg.events.size(), kMaxPatternSegments));
    collectPaketEvents(pkg, 0ULL, items);
    return finalizeTimedEvents(items, out);
}

inline bool buildSegmentsFromPackage(const OHOS::HDI::Vibrator::V2_0::VibratorPackage &pkg,
                                     std::vector<VibratorSegment> &out) {
    std::vector<TimedEventItem> items;
    for (const auto &pat : pkg.patterns) {
        if (items.size() >= kMaxPatternSegments)
            break;
        collectPaketEvents(pat, 0ULL, items);
    }
    return finalizeTimedEvents(items, out);
}

class EvdevTransport {
public:
    virtual ~EvdevTransport() = default;
    virtual bool openDevice() = 0;
    virtual bool isOpen() const = 0;
    virtual void closeDevice() = 0;
    virtual int uploadEffect(struct ff_effect &eff) = 0;
    virtual int writeGain(uint16_t gain) = 0;
    virtual int writeTrigger(int16_t effectId, int32_t value) = 0;
    virtual int removeEffect(int16_t effectId) = 0;
};

class LinuxEvdevTransport : public EvdevTransport {
public:
    LinuxEvdevTransport() = default;
    ~LinuxEvdevTransport() override {
        closeDevice();
    }

    bool openDevice() override {
        if (fd_ >= 0)
            return true;

        std::vector<int> indices;
        DIR *dir = ::opendir("/dev/input");
        if (dir != nullptr) {
            struct dirent *ent = nullptr;
            while ((ent = ::readdir(dir)) != nullptr) {
                if (std::strncmp(ent->d_name, "event", 5) == 0 && ent->d_name[5] >= '0' && ent->d_name[5] <= '9') {
                    char *endp = nullptr;
                    long idx = std::strtol(ent->d_name + 5, &endp, 10);
                    if (endp != nullptr && *endp == '\0' && idx >= 0 && idx < 256)
                        indices.push_back(static_cast<int>(idx));
                }
            }
            ::closedir(dir);
            std::sort(indices.begin(), indices.end());
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        } else {
            for (int i = 0; i < 32; ++i)
                indices.push_back(i);
        }

        for (int idx : indices) {
            char path[64];
            std::snprintf(path, sizeof(path), "/dev/input/event%d", idx);
            int fd = ::open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0)
                continue;

            char name[128];
            std::memset(name, 0, sizeof(name));
            if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && isHapticsDeviceName(name)) {
                unsigned long evBits[(EV_MAX / (8 * sizeof(unsigned long))) + 1];
                std::memset(evBits, 0, sizeof(evBits));
                bool hasFf = true;
                if (::ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits) >= 0) {
                    const size_t word = EV_FF / (8 * sizeof(unsigned long));
                    const size_t bit = EV_FF % (8 * sizeof(unsigned long));
                    hasFf = ((evBits[word] & (1UL << bit)) != 0);
                }
                if (hasFf) {
                    fd_ = fd;
                    devicePath_ = path;
                    deviceName_ = name;
                    return true;
                }
            }
            ::close(fd);
        }
        return false;
    }

    bool isOpen() const override {
        return fd_ >= 0;
    }

    void closeDevice() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        devicePath_.clear();
        deviceName_.clear();
    }

    int uploadEffect(struct ff_effect &eff) override {
        if (fd_ < 0)
            return -ENODEV;
        if (::ioctl(fd_, EVIOCSFF, &eff) < 0)
            return -errno;
        return 0;
    }

    int writeGain(uint16_t gain) override {
        if (fd_ < 0)
            return -ENODEV;
        struct input_event ev = buildFfEvent(FF_GAIN, static_cast<int32_t>(gain));
        ssize_t n = ::write(fd_, &ev, sizeof(ev));
        if (n != static_cast<ssize_t>(sizeof(ev)))
            return (n < 0) ? -errno : -EIO;
        return 0;
    }

    int writeTrigger(int16_t effectId, int32_t value) override {
        if (fd_ < 0)
            return -ENODEV;
        struct input_event ev = buildFfEvent(static_cast<uint16_t>(effectId), value);
        ssize_t n = ::write(fd_, &ev, sizeof(ev));
        if (n != static_cast<ssize_t>(sizeof(ev)))
            return (n < 0) ? -errno : -EIO;
        return 0;
    }

    int removeEffect(int16_t effectId) override {
        if (fd_ < 0)
            return -ENODEV;
        if (::ioctl(fd_, EVIOCRMFF, static_cast<int>(effectId)) < 0)
            return -errno;
        return 0;
    }

    const std::string &devicePath() const { return devicePath_; }
    const std::string &deviceName() const { return deviceName_; }

private:
    int fd_ = -1;
    std::string devicePath_;
    std::string deviceName_;
};

class VibratorController {
public:
    explicit VibratorController(EvdevTransport *transport) : transport_(transport) {}

    ~VibratorController() {
        shutdown();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            shutdown_ = true;
            generation_++;
            workerActive_ = false;
            hasPendingWork_ = false;
            pendingSegments_.clear();
            currentSessionId_ = 0;
            stopHardwareLocked();
        }
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
        std::lock_guard<std::mutex> lk(mu_);
        if (transport_ != nullptr)
            transport_->closeDevice();
    }

    bool probeDevice() {
        std::lock_guard<std::mutex> lk(mu_);
        return ensureOpenLocked();
    }

    int startOnce(uint32_t durationMs, uint16_t intensityPercent = kDefaultOnceIntensity) {
        std::lock_guard<std::mutex> lk(mu_);
        if (durationMs == 0) {
            cancelAllLocked();
            return 0;
        }
        cancelAllLocked();
        return playHardwareLocked(durationMs, intensityPercent);
    }

    int startSegments(const std::vector<VibratorSegment> &segments, uint32_t sessionId = 0) {
        if (segments.empty())
            return -EINVAL;

        std::lock_guard<std::mutex> lk(mu_);
        if (!ensureOpenLocked())
            return -ENODEV;

        /* Fast synchronous path for single immediate segment without session tracking */
        if (segments.size() == 1 && segments[0].delayMs == 0 && sessionId == 0) {
            cancelAllLocked();
            return playHardwareLocked(segments[0].durationMs, segments[0].intensity);
        }

        generation_++;
        stopHardwareLocked();
        currentSessionId_ = sessionId;
        pendingSegments_ = segments;
        hasPendingWork_ = true;
        workerActive_ = true;

        if (!workerStarted_) {
            workerStarted_ = true;
            worker_ = std::thread(&VibratorController::workerLoop, this);
        }
        cv_.notify_all();
        return 0;
    }

    int stopAll() {
        std::lock_guard<std::mutex> lk(mu_);
        cancelAllLocked();
        return 0;
    }

    int stopBySessionId(uint32_t sessionId) {
        std::lock_guard<std::mutex> lk(mu_);
        if (sessionId == 0 || currentSessionId_ == sessionId) {
            cancelAllLocked();
        }
        return 0;
    }

    bool isRunning() {
        std::lock_guard<std::mutex> lk(mu_);
        if (workerActive_)
            return true;
        if (activeEffectId_ >= 0 && std::chrono::steady_clock::now() < activeEndTime_)
            return true;
        return false;
    }

    uint32_t activeSessionId() {
        std::lock_guard<std::mutex> lk(mu_);
        return currentSessionId_;
    }

private:
    bool ensureOpenLocked() {
        if (transport_ == nullptr)
            return false;
        if (transport_->isOpen())
            return true;
        return transport_->openDevice();
    }

    void stopHardwareLocked() {
        if (transport_ != nullptr && transport_->isOpen() && activeEffectId_ >= 0) {
            (void)transport_->writeTrigger(activeEffectId_, 0);
            (void)transport_->removeEffect(activeEffectId_);
        }
        activeEffectId_ = -1;
        activeEndTime_ = std::chrono::steady_clock::time_point{};
    }

    void cancelAllLocked() {
        generation_++;
        hasPendingWork_ = false;
        workerActive_ = false;
        pendingSegments_.clear();
        currentSessionId_ = 0;
        stopHardwareLocked();
        cv_.notify_all();
    }

    int playHardwareLocked(uint32_t durationMs, uint16_t intensityPercent) {
        if (!ensureOpenLocked())
            return -ENODEV;

        stopHardwareLocked();

        struct ff_effect eff = buildConstantEffect(durationMs, intensityPercent);
        int rc = transport_->uploadEffect(eff);
        if (rc == -ENODEV || rc == -EBADF || rc == -EIO) {
            transport_->closeDevice();
            if (!transport_->openDevice())
                return -ENODEV;
            eff = buildConstantEffect(durationMs, intensityPercent);
            rc = transport_->uploadEffect(eff);
        }
        if (rc != 0)
            return rc;

        activeEffectId_ = eff.id;
        /* Optional gain hint; ignore driver rejection if unsupported. */
        (void)transport_->writeGain(intensityToGainValue(intensityPercent));

        rc = transport_->writeTrigger(eff.id, 1);
        if (rc != 0) {
            (void)transport_->removeEffect(eff.id);
            activeEffectId_ = -1;
            activeEndTime_ = std::chrono::steady_clock::time_point{};
            return rc;
        }

        activeEndTime_ = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(eff.replay.length);
        return 0;
    }

    void workerLoop() {
        std::unique_lock<std::mutex> lk(mu_);
        while (!shutdown_) {
            cv_.wait(lk, [this]() { return shutdown_ || hasPendingWork_; });
            if (shutdown_)
                break;

            uint64_t myGen = generation_;
            std::vector<VibratorSegment> segs = std::move(pendingSegments_);
            pendingSegments_.clear();
            hasPendingWork_ = false;

            for (const auto &seg : segs) {
                if (seg.delayMs > 0) {
                    if (cv_.wait_for(lk, std::chrono::milliseconds(seg.delayMs),
                                     [this, myGen]() { return shutdown_ || generation_ != myGen; })) {
                        break;
                    }
                }
                if (shutdown_ || generation_ != myGen)
                    break;

                int rc = playHardwareLocked(seg.durationMs, seg.intensity);
                if (rc != 0)
                    break;

                uint32_t waitMs = clampDurationMs(seg.durationMs);
                if (cv_.wait_for(lk, std::chrono::milliseconds(waitMs),
                                 [this, myGen]() { return shutdown_ || generation_ != myGen; })) {
                    break;
                }
            }

            if (!shutdown_ && generation_ == myGen) {
                stopHardwareLocked();
                workerActive_ = false;
                currentSessionId_ = 0;
            }
        }
    }

    EvdevTransport *transport_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
    bool workerStarted_ = false;
    bool shutdown_ = false;
    bool hasPendingWork_ = false;
    bool workerActive_ = false;
    uint64_t generation_ = 0;
    uint32_t currentSessionId_ = 0;
    int16_t activeEffectId_ = -1;
    std::chrono::steady_clock::time_point activeEndTime_{};
    std::vector<VibratorSegment> pendingSegments_;
};

} // namespace marble_vibrator

#endif // MARBLE_VIBRATOR_BACKEND_H
