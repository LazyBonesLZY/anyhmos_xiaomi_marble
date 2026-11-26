/* SPDX-License-Identifier: MIT */
/*
 * marble_torch_ctrl.h - Clean-room SM7475 (Xiaomi marble) QPNP flash/torch LED
 * sysfs controller (/sys/class/leds/led:torch_0..3 + led:switch_0).
 *
 * Host-testable by passing a custom sysfs root directory (default "/sys").
 */
#ifndef MARBLE_CAMERA_TORCH_CTRL_H
#define MARBLE_CAMERA_TORCH_CTRL_H

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <unistd.h>

namespace marble_cam {

struct TorchApplyResult {
    bool ok = false;
    bool on = false;
    int32_t torch0Brightness = 0;
    int32_t torch1Brightness = 0;
    int32_t switch0Value = 0;
};

class MarbleTorchCtrl {
public:
    explicit MarbleTorchCtrl(std::string sysRoot = "/sys")
        : sysRoot_(std::move(sysRoot)) {}

    void setSysRoot(std::string sysRoot) {
        std::lock_guard<std::mutex> lk(mu_);
        sysRoot_ = std::move(sysRoot);
    }

    bool isOn() const {
        std::lock_guard<std::mutex> lk(mu_);
        return active_;
    }

    float currentLevel() const {
        std::lock_guard<std::mutex> lk(mu_);
        return level_;
    }

    static int32_t computeBrightness(float level, int32_t maxBrightness) {
        if (maxBrightness <= 0)
            return 0;
        if (std::isnan(level) || level <= 0.0f)
            return 0;
        if (level > 1.0f)
            level = 1.0f;
        long v = std::lround(static_cast<double>(level) * static_cast<double>(maxBrightness));
        if (v < 1)
            v = 1;
        if (v > static_cast<long>(maxBrightness))
            v = static_cast<long>(maxBrightness);
        return static_cast<int32_t>(v);
    }

    TorchApplyResult setLevel(float level) {
        std::lock_guard<std::mutex> lk(mu_);
        TorchApplyResult res {};

        const std::string ledsDir = sysRoot_ + "/class/leds";
        const std::string sw0Path = ledsDir + "/led:switch_0/brightness";
        const std::string t0Bright = ledsDir + "/led:torch_0/brightness";
        const std::string t0Max = ledsDir + "/led:torch_0/max_brightness";
        const std::string t1Bright = ledsDir + "/led:torch_1/brightness";
        const std::string t1Max = ledsDir + "/led:torch_1/max_brightness";

        if (!std::isnan(level) && level > 0.0f) {
            float clamped = (level > 1.0f) ? 1.0f : level;
            int32_t max0 = 500;
            (void)readIntFile(t0Max, max0);
            if (max0 <= 0)
                max0 = 500;
            int32_t b0 = computeBrightness(clamped, max0);

            bool wrote0 = writeIntFile(t0Bright, b0);
            res.torch0Brightness = wrote0 ? b0 : 0;

            /* Balanced dual-tone warm/cold pair: drive led:torch_1 if present. */
            if (fileExists(t1Bright)) {
                int32_t max1 = max0;
                (void)readIntFile(t1Max, max1);
                if (max1 <= 0)
                    max1 = max0;
                int32_t b1 = computeBrightness(clamped, max1);
                if (writeIntFile(t1Bright, b1))
                    res.torch1Brightness = b1;
            }

            /* Enable flash switch AFTER setting torch channel current(s). */
            bool wroteSw = writeIntFile(sw0Path, 1);
            res.switch0Value = wroteSw ? 1 : 0;

            res.ok = wrote0 || wroteSw;
            res.on = res.ok;
            if (res.ok) {
                active_ = true;
                level_ = clamped;
            }
            return res;
        }

        /* Turn OFF: disable switch_0 FIRST, then zero all torch_0..3 channels. */
        bool wroteSw = writeIntFile(sw0Path, 0);
        bool wroteAnyTorch = false;
        for (int idx = 0; idx < 4; ++idx) {
            std::string tp = ledsDir + "/led:torch_" + std::to_string(idx) + "/brightness";
            if (fileExists(tp)) {
                if (writeIntFile(tp, 0))
                    wroteAnyTorch = true;
            }
        }
        res.ok = wroteSw || wroteAnyTorch;
        res.on = false;
        res.torch0Brightness = 0;
        res.torch1Brightness = 0;
        res.switch0Value = 0;
        active_ = false;
        level_ = 0.0f;
        return res;
    }

private:
    static bool fileExists(const std::string &path) {
        return ::access(path.c_str(), F_OK) == 0;
    }

    static bool readIntFile(const std::string &path, int32_t &out) {
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        char buf[64] = {0};
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (n <= 0)
            return false;
        buf[n] = '\0';
        char *endp = nullptr;
        errno = 0;
        long val = std::strtol(buf, &endp, 10);
        if (errno != 0 || endp == buf)
            return false;
        out = static_cast<int32_t>(val);
        return true;
    }

    static bool writeIntFile(const std::string &path, int32_t val) {
        int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        char buf[32];
        int len = std::snprintf(buf, sizeof(buf), "%d\n", val);
        bool ok = false;
        if (len > 0) {
            ssize_t nw = ::write(fd, buf, static_cast<size_t>(len));
            ok = (nw == static_cast<ssize_t>(len));
        }
        ::close(fd);
        return ok;
    }

    mutable std::mutex mu_;
    std::string sysRoot_;
    bool active_ = false;
    float level_ = 0.0f;
};

inline MarbleTorchCtrl &globalTorchCtrl() {
    static MarbleTorchCtrl g_ctrl("/sys");
    return g_ctrl;
}

} // namespace marble_cam

#endif /* MARBLE_CAMERA_TORCH_CTRL_H */
