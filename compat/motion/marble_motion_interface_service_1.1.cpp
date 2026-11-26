/* SPDX-License-Identifier: MIT */
/*
 * marble_motion_interface_service_1.1.cpp - Clean-room SM7475 (Xiaomi marble) OHOS Motion HDI v1.1.
 *
 * SONAME: libmotion_interface_service_1.1.z.so (bind-mounted over donor vendor service).
 * Loaded by unmodified donor libmotion_driver.z.so + libmotion_stub_1.1.z.so + libmotion_stub_1.0.z.so
 * inside HDF host-18 (motion_host) via MotionInterfaceImplGetInstance().
 *
 * Features:
 *   - Supports HDF_MOTION_TYPE_PICKUP (0) raise-to-wake (抬起亮屏) backed by Qualcomm SLPI:
 *       1) Tries SLPI SUID "tilt_to_wake" (MSGID 514, on_change=true) first.
 *       2) Falls back to SLPI "accel" at 25 Hz (MSGID 513) feeding PickupDetector.
 *   - Zero SLPI/CPU draw when pickup is disabled or when screen backlight > 0.
 *   - On pickup trigger while screen is off, delivers HdfMotionEvent{0, 0, 0, 0, {}} to all
 *     registered IMotionCallback instances (SA 2901 -> PowerMgrClient::WakeupDevice(25)).
 *   - Non-pickup motion types (!= 0) return HDF_SUCCESS cleanly (no-op).
 */

#include "v1_1/imotion_interface.h"
#include "ssc_motion_client.h"

#include <iproxy_broker.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace OHOS {
namespace HDI {
namespace Motion {
namespace V1_1 {

static_assert(sizeof(OHOS::PeerHolder) == 24, "OHOS::PeerHolder ABI size mismatch with donor MotionCallbackProxy");

namespace {

constexpr const char *kMotionLogPath = "/data/local/tmp/marble-motion.log";

static void appendMotionLog(const char *msg) {
    if (msg == nullptr) {
        return;
    }
    struct timespec ts {};
    (void)::clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[256];
    int len = std::snprintf(line, sizeof(line), "[%lld.%03ld] %s\n",
                            static_cast<long long>(ts.tv_sec),
                            ts.tv_nsec / 1000000L, msg);
    if (len <= 0) {
        return;
    }
    const size_t n = (static_cast<size_t>(len) < sizeof(line)) ? static_cast<size_t>(len) : (sizeof(line) - 1);
    int fd = ::open(kMotionLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0664);
    if (fd >= 0) {
        (void)::write(fd, line, n);
        ::close(fd);
    }
}

static const void *callbackIdentityKey(const sptr<V1_0::IMotionCallback> &cb) {
    if (cb == nullptr) {
        return nullptr;
    }
    if (cb->IsProxy()) {
        sptr<IRemoteObject> remote = OHOS::HDI::hdi_objcast<V1_0::IMotionCallback>(cb);
        if (remote != nullptr) {
            return remote.GetRefPtr();
        }
    }
    return cb.GetRefPtr();
}

} // namespace

class MotionIfService : public IMotionInterface {
public:
    MotionIfService()
        : ssc_client_(
              [this](const char *src, float pitch_deg, float roll_deg) {
                  this->onPickupDetected(src, pitch_deg, roll_deg);
              },
              [](const char *msg) { appendMotionLog(msg); }) {
        appendMotionLog("SERVICE_INIT motion_interface_service v1.1 ready");
    }

    ~MotionIfService() override {
        ssc_client_.stop();
        appendMotionLog("SERVICE_FINI motion_interface_service stopped");
    }

    int32_t EnableMotion(int32_t motionType) override {
        if (motionType != V1_0::HDF_MOTION_TYPE_PICKUP) {
            return HDF_SUCCESS;
        }
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (pickup_refcount_ > 0) {
                ++pickup_refcount_;
                return HDF_SUCCESS;
            }
            pickup_refcount_ = 1;
        }
        if (!ssc_client_.start()) {
            std::lock_guard<std::mutex> lk(state_mu_);
            pickup_refcount_ = 0;
            appendMotionLog("ENABLE motion=0 failed=worker_start");
            return HDF_FAILURE;
        }
        appendMotionLog("ENABLE motion=0 ok");
        return HDF_SUCCESS;
    }

    int32_t DisableMotion(int32_t motionType) override {
        if (motionType != V1_0::HDF_MOTION_TYPE_PICKUP) {
            return HDF_SUCCESS;
        }
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (pickup_refcount_ == 0) {
                return HDF_SUCCESS;
            }
            --pickup_refcount_;
            if (pickup_refcount_ > 0) {
                return HDF_SUCCESS;
            }
        }
        ssc_client_.stop();
        appendMotionLog("DISABLE motion=0 ok");
        return HDF_SUCCESS;
    }

    int32_t Register(const sptr<V1_0::IMotionCallback> &callbackObj) override {
        if (callbackObj == nullptr) {
            return HDF_ERR_INVALID_PARAM;
        }
        const void *key = callbackIdentityKey(callbackObj);
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            bool updated = false;
            for (auto &entry : callbacks_) {
                if (entry.key == key || entry.cb == callbackObj) {
                    entry.cb = callbackObj;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                callbacks_.push_back(CallbackEntry{key, callbackObj});
            }
            count = callbacks_.size();
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg), "REGISTER cbs=%zu", count);
        appendMotionLog(msg);
        return HDF_SUCCESS;
    }

    int32_t Unregister(const sptr<V1_0::IMotionCallback> &callbackObj) override {
        if (callbackObj == nullptr) {
            return HDF_ERR_INVALID_PARAM;
        }
        const void *key = callbackIdentityKey(callbackObj);
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            for (auto it = callbacks_.begin(); it != callbacks_.end(); ) {
                if (it->key == key || it->cb == callbackObj) {
                    it = callbacks_.erase(it);
                } else {
                    ++it;
                }
            }
            count = callbacks_.size();
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg), "UNREGISTER cbs=%zu", count);
        appendMotionLog(msg);
        return HDF_SUCCESS;
    }

    int32_t SetMotionConfig(int32_t motionType, const std::vector<uint8_t> &data) override {
        (void)motionType;
        (void)data;
        return HDF_SUCCESS;
    }

private:
    struct CallbackEntry {
        const void *key = nullptr;
        sptr<V1_0::IMotionCallback> cb;
    };

    void onPickupDetected(const char *src, float pitch_deg, float roll_deg) {
        std::vector<sptr<V1_0::IMotionCallback>> snapshot;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            snapshot.reserve(callbacks_.size());
            for (const auto &entry : callbacks_) {
                if (entry.cb != nullptr) {
                    snapshot.push_back(entry.cb);
                }
            }
        }

        V1_0::HdfMotionEvent ev{};
        ev.motion = V1_0::HDF_MOTION_TYPE_PICKUP;
        ev.result = 0;
        ev.status = 0;
        ev.datalen = 0;
        ev.data.clear();

        size_t delivered = 0;
        int32_t last_rc = 0;
        for (const auto &cb : snapshot) {
            const int32_t rc = cb->OnDataEvent(ev);
            last_rc = rc;
            if (rc == HDF_SUCCESS) {
                ++delivered;
            }
        }

        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "PICKUP src=%s pitch=%.1f roll=%.1f cbs=%zu ok=%zu last_rc=%d",
                      (src != nullptr) ? src : "unknown",
                      static_cast<double>(pitch_deg),
                      static_cast<double>(roll_deg),
                      snapshot.size(), delivered, last_rc);
        appendMotionLog(msg);
    }

    std::mutex state_mu_;
    uint32_t pickup_refcount_ = 0;
    marble_motion::SscMotionClient ssc_client_;

    std::mutex cb_mu_;
    std::vector<CallbackEntry> callbacks_;
};

extern "C" IMotionInterface *MotionInterfaceImplGetInstance(void) {
    return new (std::nothrow) MotionIfService();
}

} // namespace V1_1
} // namespace Motion
} // namespace HDI
} // namespace OHOS
