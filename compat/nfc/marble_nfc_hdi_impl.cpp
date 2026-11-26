/* SPDX-License-Identifier: MIT */
/*
 * marble_nfc_hdi_impl.cpp - Clean-room SM7475 (Xiaomi marble) OHOS NFC HDI v1.1
 * implementation directly driving NXP SN110T/SN220 NCI 2.0 over /dev/nq-nci.
 *
 * Loaded by unmodified donor libnfc_hdi_driver.z.so inside HDF nfc_host via
 * NfcInterfaceImplGetInstance().
 */
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef MARBLE_NFC_HDI_HOST_TEST
#include <hdf_base.h>
#include "v1_1/infc_interface.h"
#endif

#define NFC_MAGIC           0xE9
#define NFC_SET_PWR         _IOW(NFC_MAGIC, 0x01, unsigned int)
#define NFC_GET_IRQ_STATE   _IO(NFC_MAGIC, 0x06)
#define NFCC_GET_INFO       _IOW(NFC_MAGIC, 0x09, unsigned int)

#define NFC_POWER_OFF       0u
#define NFC_POWER_ON        1u
#define NFC_MAX_NCI_PKT     258u

namespace marble_nfc {

static bool isValidNciFrame(const uint8_t *buf, size_t len) {
    if (!buf || len < 3u || len > NFC_MAX_NCI_PKT)
        return false;
    return (3u + static_cast<size_t>(buf[2])) == len;
}

static void fillMarbleVendorConfigFields(uint32_t &apduLen, uint8_t &offHost,
                                         uint8_t &felica, uint8_t &sysRoute,
                                         uint8_t &sysPwr, uint8_t &unconf,
                                         uint8_t &esePipe, uint8_t &simPipe,
                                         bool &bailOut, uint8_t &chkAlgo,
                                         std::vector<uint8_t> &propCfg,
                                         uint8_t &propSize,
                                         std::vector<uint8_t> &hostWl,
                                         std::vector<uint8_t> &uiccRoutes,
                                         std::vector<uint8_t> &eseRoutes,
                                         uint8_t &isoDepRoute) {
    apduLen = 65535u;
    offHost = 0x00u;
    felica = 0x00u;
    sysRoute = 0x00u;
    sysPwr = 0x19u;
    unconf = 0x00u;
    esePipe = 0x00u;
    simPipe = 0x00u;
    bailOut = false;
    chkAlgo = 1u; /* I_BLOCK */
    /* NXP SN110T standard NCI 2.0 proprietary protocol/discovery map (9 bytes) */
    propCfg = { 0x05u, 0x80u, 0x06u, 0x81u, 0x70u, 0x80u, 0x8au, 0x77u, 0x00u };
    propSize = static_cast<uint8_t>(propCfg.size());
    hostWl.clear();
    uiccRoutes.clear();
    eseRoutes.clear();
    isoDepRoute = 0x00u;
}

} // namespace marble_nfc

#ifdef MARBLE_NFC_HDI_HOST_TEST

int main(void) {
    const uint8_t resetCmd[] = { 0x20, 0x00, 0x01, 0x00 };
    const uint8_t badCmd[]   = { 0x20, 0x00, 0x05, 0x00 };
    if (!marble_nfc::isValidNciFrame(resetCmd, sizeof(resetCmd))) return 1;
    if (marble_nfc::isValidNciFrame(badCmd, sizeof(badCmd))) return 2;

    uint32_t apdu = 0;
    uint8_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0, psz = 0, iso = 0;
    bool bail = true;
    std::vector<uint8_t> pcfg, wl, uicc, ese;
    marble_nfc::fillMarbleVendorConfigFields(apdu, a, b, c, d, e, f, g, bail, h,
                                             pcfg, psz, wl, uicc, ese, iso);
    if (apdu != 65535u || psz != 9u || pcfg.size() != 9u || h != 1u || bail)
        return 3;
    std::printf("MARBLE_NFC_HDI_HOST_TEST_OK=1 apdu=%u prop_cfg=%u nfc_set_pwr=0x%08lx\n",
                apdu, psz, static_cast<unsigned long>(NFC_SET_PWR));
    return 0;
}

#else /* !MARBLE_NFC_HDI_HOST_TEST */

namespace OHOS {
namespace HDI {
namespace Nfc {
namespace V1_1 {

using OHOS::HDI::Nfc::V1_0::INfcCallback;
using OHOS::HDI::Nfc::V1_0::NfcCommand;
using OHOS::HDI::Nfc::V1_0::NfcEvent;
using OHOS::HDI::Nfc::V1_0::NfcStatus;

class NfcImpl : public INfcInterface {
public:
    NfcImpl() = default;

    ~NfcImpl() override {
        NfcStatus st = NfcStatus::OK;
        (void)Close(st);
    }

    int32_t Open(const sptr<INfcCallback> &callbackObj, NfcStatus &status) override {
        if (callbackObj == nullptr) {
            status = NfcStatus::FAILED;
            return HDF_ERR_INVALID_PARAM;
        }
        stopReaderAndCloseLocked();

        std::lock_guard<std::mutex> lk(mu_);
        callback_ = callbackObj;

        const char *devPath = "/dev/nq-nci";
        int fd = ::open(devPath, O_RDWR);
        if (fd < 0) {
            std::fprintf(stderr, "marble_nfc_hdi: open(%s) failed errno=%d\n", devPath, errno);
            callback_ = nullptr;
            status = NfcStatus::ERR_TRANSPORT;
            return HDF_FAILURE;
        }

        unsigned int info = static_cast<unsigned int>(::ioctl(fd, NFCC_GET_INFO, 0));
        chipInfo_ = info;

        if (::ioctl(fd, NFC_SET_PWR, NFC_POWER_ON) < 0) {
            std::fprintf(stderr, "marble_nfc_hdi: NFC_POWER_ON failed errno=%d\n", errno);
            ::close(fd);
            callback_ = nullptr;
            status = NfcStatus::ERR_TRANSPORT;
            return HDF_FAILURE;
        }
        ::usleep(30000);

        /* Drain any stale boot notification before starting reader thread */
        uint8_t drain[NFC_MAX_NCI_PKT];
        for (int i = 0; i < 4 && ::ioctl(fd, NFC_GET_IRQ_STATE, 0) > 0; ++i) {
            ssize_t h = ::read(fd, drain, 3);
            if (h == 3 && drain[2] > 0 && (3u + drain[2]) <= sizeof(drain))
                (void)::read(fd, drain + 3, drain[2]);
            else
                break;
        }

        devFd_ = fd;
        running_.store(true);
        readerThread_ = std::thread(&NfcImpl::readerLoop, this);

        status = NfcStatus::OK;
        sptr<INfcCallback> cb = callback_;
        if (cb != nullptr)
            (void)cb->OnEvent(NfcEvent::OPEN_CPLT, NfcStatus::OK);
        return HDF_SUCCESS;
    }

    int32_t CoreInitialized(const std::vector<uint8_t> &data, NfcStatus &status) override {
        (void)data;
        sptr<INfcCallback> cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (devFd_ < 0) {
                status = NfcStatus::FAILED;
                return HDF_FAILURE;
            }
            cb = callback_;
        }
        status = NfcStatus::OK;
        if (cb != nullptr)
            (void)cb->OnEvent(NfcEvent::POST_INIT_CPLT, NfcStatus::OK);
        return HDF_SUCCESS;
    }

    int32_t Prediscover(NfcStatus &status) override {
        sptr<INfcCallback> cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (devFd_ < 0) {
                status = NfcStatus::FAILED;
                return HDF_FAILURE;
            }
            cb = callback_;
        }
        status = NfcStatus::OK;
        if (cb != nullptr)
            (void)cb->OnEvent(NfcEvent::PRE_DISCOVER_CPLT, NfcStatus::OK);
        return HDF_SUCCESS;
    }

    int32_t Write(const std::vector<uint8_t> &data, NfcStatus &status) override {
        if (data.empty() || data.size() > NFC_MAX_NCI_PKT) {
            status = NfcStatus::FAILED;
            return HDF_ERR_INVALID_PARAM;
        }
        std::lock_guard<std::mutex> lk(writeMu_);
        int fd = devFd_.load();
        if (fd < 0) {
            status = NfcStatus::ERR_TRANSPORT;
            return HDF_FAILURE;
        }
        /* NXP SN110T wakes from I2C standby on first write (-ENOTCONN); retry cleanly. */
        bool ok = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            ssize_t nw = ::write(fd, data.data(), data.size());
            if (nw == static_cast<ssize_t>(data.size())) {
                ok = true;
                break;
            }
            ::usleep(10000);
        }
        if (!ok) {
            status = NfcStatus::ERR_TRANSPORT;
            return HDF_FAILURE;
        }
        status = NfcStatus::OK;
        return HDF_SUCCESS;
    }

    int32_t ControlGranted(NfcStatus &status) override {
        sptr<INfcCallback> cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb = callback_;
        }
        status = NfcStatus::OK;
        if (cb != nullptr)
            (void)cb->OnEvent(NfcEvent::RELEASE_CONTROL, NfcStatus::OK);
        return HDF_SUCCESS;
    }

    int32_t PowerCycle(NfcStatus &status) override {
        sptr<INfcCallback> cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            int fd = devFd_.load();
            if (fd < 0) {
                status = NfcStatus::ERR_TRANSPORT;
                return HDF_FAILURE;
            }
            (void)::ioctl(fd, NFC_SET_PWR, NFC_POWER_OFF);
            ::usleep(20000);
            if (::ioctl(fd, NFC_SET_PWR, NFC_POWER_ON) < 0) {
                status = NfcStatus::ERR_TRANSPORT;
                return HDF_FAILURE;
            }
            ::usleep(30000);
            cb = callback_;
        }
        status = NfcStatus::OK;
        if (cb != nullptr)
            (void)cb->OnEvent(NfcEvent::OPEN_CPLT, NfcStatus::OK);
        return HDF_SUCCESS;
    }

    int32_t Close(NfcStatus &status) override {
        sptr<INfcCallback> cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb = callback_;
        }
        stopReaderAndCloseLocked();
        status = NfcStatus::OK;
        if (cb != nullptr)
            (void)cb->OnEvent(NfcEvent::CLOSE_CPLT, NfcStatus::OK);
        {
            std::lock_guard<std::mutex> lk(mu_);
            callback_ = nullptr;
        }
        return HDF_SUCCESS;
    }

    int32_t Ioctl(NfcCommand cmd, const std::vector<uint8_t> &data, NfcStatus &status) override {
        (void)cmd;
        (void)data;
        status = NfcStatus::OK;
        return HDF_SUCCESS;
    }

    int32_t GetVendorConfig(NfcVendorConfig &config, NfcStatus &status) override {
        marble_nfc::fillMarbleVendorConfigFields(
            config.isoDepExtApduLength,
            config.defaultOffHostRoute,
            config.defaultOffHostRouteFelica,
            config.defaultSysCodeRoute,
            config.defaultSysCodePwrState,
            config.defaultUnconfiguredRoute,
            config.esePipeId,
            config.simPipeId,
            config.pollBailOutMode,
            config.checkAlgorithm,
            config.vendorProtocalDiscoveryCfg,
            config.vendorProtocalDiscoveryCfgSize,
            config.hostWhitelist,
            config.offHostRouteUicc,
            config.offHostRouteEse,
            config.defaultIsoDepRoute);
        status = NfcStatus::OK;
        return HDF_SUCCESS;
    }

    int32_t DoFactoryReset(NfcStatus &status) override {
        status = NfcStatus::OK;
        return HDF_SUCCESS;
    }

    int32_t Shutdown(NfcStatus &status) override {
        return Close(status);
    }

    int32_t IoctlWithResponse(NfcCommand cmd, const std::vector<uint8_t> &data,
                              std::vector<uint8_t> &response, NfcStatus &status) override {
        (void)cmd;
        (void)data;
        response.clear();
        uint32_t info = chipInfo_.load();
        response.push_back(static_cast<uint8_t>(info & 0xFFu));
        response.push_back(static_cast<uint8_t>((info >> 8) & 0xFFu));
        response.push_back(static_cast<uint8_t>((info >> 16) & 0xFFu));
        response.push_back(static_cast<uint8_t>((info >> 24) & 0xFFu));
        status = NfcStatus::OK;
        return HDF_SUCCESS;
    }

private:
    void stopReaderAndCloseLocked() {
        running_.store(false);
        int fd = devFd_.exchange(-1);
        if (fd >= 0) {
            /* NFC_POWER_OFF + close() triggers nfc_dev_flush() to release blocked read() cleanly. */
            (void)::ioctl(fd, NFC_SET_PWR, NFC_POWER_OFF);
            ::close(fd);
        }
        if (readerThread_.joinable())
            readerThread_.join();
    }

    void readerLoop() {
        uint8_t buf[NFC_MAX_NCI_PKT];
        while (running_.load()) {
            int fd = devFd_.load();
            if (fd < 0)
                break;
            /* Poll IRQ gently with 5ms intervals so shutdown never blocks indefinitely */
            int irq = ::ioctl(fd, NFC_GET_IRQ_STATE, 0);
            if (irq <= 0) {
                ::usleep(5000);
                continue;
            }
            ssize_t h = ::read(fd, buf, 3);
            if (h != 3) {
                if (!running_.load())
                    break;
                ::usleep(5000);
                continue;
            }
            uint8_t plen = buf[2];
            if (plen > 0) {
                if ((3u + static_cast<size_t>(plen)) > sizeof(buf))
                    continue;
                ssize_t p = ::read(fd, buf + 3, plen);
                if (p != plen) {
                    if (!running_.load())
                        break;
                    continue;
                }
            }
            size_t total = 3u + static_cast<size_t>(plen);
            if (!marble_nfc::isValidNciFrame(buf, total))
                continue;

            sptr<INfcCallback> cb;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb = callback_;
            }
            if (cb != nullptr && running_.load()) {
                std::vector<uint8_t> pkt(buf, buf + total);
                (void)cb->OnData(pkt);
            }
        }
    }

    std::mutex mu_;
    std::mutex writeMu_;
    sptr<INfcCallback> callback_ = nullptr;
    std::atomic<int> devFd_{-1};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> chipInfo_{0};
    std::thread readerThread_;
};

extern "C" INfcInterface *NfcInterfaceImplGetInstance(void) {
    return new (std::nothrow) NfcImpl();
}

} // namespace V1_1
} // namespace Nfc
} // namespace HDI
} // namespace OHOS

#endif /* !MARBLE_NFC_HDI_HOST_TEST */
