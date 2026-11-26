/* SPDX-License-Identifier: MIT */
/*
 * native_nfc_hdi_probe.cpp - End-to-end OHOS Binder/HDI probe for SM7475 NFC.
 *
 * Connects to nfc_host's "nfc_interface_service" via unmodified donor
 * libnfc_proxy_1.1.z.so (INfcInterface::Get(false)), registers a real
 * INfcCallback over Binder, and executes a complete live NCI 2.0 session
 * against the physical NXP SN110T silicon chip (CORE_RESET + CORE_INIT +
 * CORE_GET_CONFIG + CoreInitialized + Prediscover + Close).
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include <pthread.h>
#include <ipc_skeleton.h>
#include <iremote_object.h>
#include <message_option.h>
#include <message_parcel.h>
#include "v1_1/infc_interface.h"

using OHOS::IPCSkeleton;
using OHOS::IRemoteObject;
using OHOS::MessageOption;
using OHOS::MessageParcel;
using OHOS::sptr;
using OHOS::HDI::Nfc::V1_0::INfcCallback;
using OHOS::HDI::Nfc::V1_0::NfcEvent;
using OHOS::HDI::Nfc::V1_0::NfcStatus;
using OHOS::HDI::Nfc::V1_1::INfcInterface;
using OHOS::HDI::Nfc::V1_1::NfcVendorConfig;

namespace {

static std::string toHex(const std::vector<uint8_t> &v) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(v.size() * 2u);
    for (uint8_t b : v) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0Fu]);
    }
    return out;
}

class ProbeNfcCallback : public INfcCallback {
public:
    int32_t OnData(const std::vector<uint8_t> &data) override {
        std::lock_guard<std::mutex> lk(mu_);
        rxPkts_.push_back(data);
        cv_.notify_all();
        return HDF_SUCCESS;
    }

    int32_t OnEvent(NfcEvent event, NfcStatus status) override {
        std::lock_guard<std::mutex> lk(mu_);
        rxEvents_.push_back({event, status});
        cv_.notify_all();
        return HDF_SUCCESS;
    }

    bool waitEvent(NfcEvent want, int timeoutMs, NfcStatus *outStatus = nullptr) {
        std::unique_lock<std::mutex> lk(mu_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            for (auto it = rxEvents_.begin(); it != rxEvents_.end(); ++it) {
                if (it->first == want) {
                    if (outStatus) *outStatus = it->second;
                    rxEvents_.erase(it);
                    return true;
                }
            }
            if (cv_.wait_until(lk, deadline) == std::cv_status::timeout)
                return false;
        }
    }

    bool waitPacket(uint8_t b0, uint8_t b1, int timeoutMs, std::vector<uint8_t> &outPkt) {
        std::unique_lock<std::mutex> lk(mu_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            for (auto it = rxPkts_.begin(); it != rxPkts_.end(); ++it) {
                if (it->size() >= 3u && (*it)[0] == b0 && (*it)[1] == b1) {
                    outPkt = *it;
                    rxPkts_.erase(it);
                    return true;
                }
            }
            if (cv_.wait_until(lk, deadline) == std::cv_status::timeout)
                return false;
        }
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> rxPkts_;
    std::deque<std::pair<NfcEvent, NfcStatus>> rxEvents_;
};

static bool checkSaRegistered(int32_t saId) {
    sptr<IRemoteObject> samgr = IPCSkeleton::GetContextObject();
    if (samgr == nullptr)
        return false;
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(u"ohos.samgr.accessToken"))
        return false;
    if (!data.WriteInt32(saId))
        return false;
    int rc = samgr->SendRequest(2 /* CHECK_SYSTEM_ABILITY_TRANSACTION */, data, reply, opt);
    if (rc != 0)
        return false;
    return reply.ReadRemoteObject() != nullptr;
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            std::vector<uint8_t> v = { 0x40, 0x00, 0x01, 0x00 };
            bool ok = (toHex(v) == "40000100");
            std::printf("MARBLE_NFC_HDI_PROBE_SELF_TEST_OK=%d\n", ok ? 1 : 0);
            return ok ? 0 : 1;
        }
    }

    (void)IPCSkeleton::SetMaxWorkThreadNum(4);
    pthread_t ipcTid;
    if (pthread_create(&ipcTid, nullptr, [](void *) -> void * {
            IPCSkeleton::JoinWorkThread();
            return nullptr;
        }, nullptr) == 0) {
        pthread_detach(ipcTid);
    }

    bool sa1140 = checkSaRegistered(1140);
    std::printf("SA1140_REGISTERED=%d\n", sa1140 ? 1 : 0);

    sptr<INfcInterface> nfc = INfcInterface::Get(false);
    if (nfc == nullptr) {
        std::printf("NFC_HDI_CONNECT_ERR=null\n");
        return 2;
    }
    std::printf("NFC_HDI_SERVICE=connected\n");

    uint32_t maj = 0, min = 0;
    int32_t vrc = nfc->GetVersion(maj, min);
    std::printf("NFC_HDI_VERSION rc=%d ver=%u.%u\n", vrc, maj, min);

    NfcVendorConfig cfg {};
    NfcStatus st = NfcStatus::FAILED;
    int32_t crc = nfc->GetVendorConfig(cfg, st);
    bool cfgOk = (crc == HDF_SUCCESS && st == NfcStatus::OK &&
                  cfg.isoDepExtApduLength == 65535u && cfg.vendorProtocalDiscoveryCfgSize == 9u);
    std::printf("NFC_VENDOR_CONFIG rc=%d st=%d apdu=%u prop_cfg=%u algo=%u ok=%d\n",
                crc, static_cast<int>(st), cfg.isoDepExtApduLength,
                static_cast<unsigned>(cfg.vendorProtocalDiscoveryCfgSize),
                static_cast<unsigned>(cfg.checkAlgorithm), cfgOk ? 1 : 0);

    sptr<ProbeNfcCallback> cb = new ProbeNfcCallback();
    st = NfcStatus::FAILED;
    int32_t orc = nfc->Open(cb, st);
    NfcStatus evSt = NfcStatus::FAILED;
    bool openEvt = (orc == HDF_SUCCESS && st == NfcStatus::OK) &&
                   cb->waitEvent(NfcEvent::OPEN_CPLT, 2000, &evSt) && evSt == NfcStatus::OK;
    std::printf("NFC_OPEN rc=%d st=%d evt_ok=%d\n", orc, static_cast<int>(st), openEvt ? 1 : 0);
    if (!openEvt) {
        std::fflush(stdout);
        _exit(3);
    }

    /* 1. NCI 2.0 CORE_RESET_CMD */
    bool resetOk = false, resetNtfOk = false;
    uint8_t nciVer = 0, mfgId = 0, chipType = 0, fwMaj = 0, fwMin = 0;
    std::vector<uint8_t> pkt;
    st = NfcStatus::FAILED;
    if (nfc->Write({0x20, 0x00, 0x01, 0x00}, st) == HDF_SUCCESS && st == NfcStatus::OK) {
        if (cb->waitPacket(0x40, 0x00, 2000, pkt)) {
            std::printf("BINDER_NCI_RESET_RSP len=%zu hex=%s\n", pkt.size(), toHex(pkt).c_str());
            resetOk = (pkt.size() >= 4u && pkt[3] == 0x00);
        }
        if (resetOk && cb->waitPacket(0x60, 0x00, 2000, pkt)) {
            std::printf("BINDER_NCI_RESET_NTF len=%zu hex=%s\n", pkt.size(), toHex(pkt).c_str());
            if (pkt.size() >= 13u) {
                resetNtfOk = true;
                nciVer = pkt[5];
                mfgId = pkt[6];
                chipType = pkt[9];
                fwMaj = pkt[11];
                fwMin = pkt[12];
            }
        }
    }

    /* 2. NCI 2.0 CORE_INIT_CMD */
    bool initOk = false;
    st = NfcStatus::FAILED;
    if (resetNtfOk && nfc->Write({0x20, 0x01, 0x02, 0x00, 0x00}, st) == HDF_SUCCESS && st == NfcStatus::OK) {
        if (cb->waitPacket(0x40, 0x01, 2000, pkt)) {
            std::printf("BINDER_NCI_INIT_RSP len=%zu hex=%s\n", pkt.size(), toHex(pkt).c_str());
            initOk = (pkt.size() >= 16u && pkt[3] == 0x00);
        }
    }

    /* 3. CoreInitialized & Prediscover callbacks */
    bool postInitOk = false, preDiscOk = false;
    st = NfcStatus::FAILED;
    if (initOk && nfc->CoreInitialized({0x01}, st) == HDF_SUCCESS && st == NfcStatus::OK) {
        postInitOk = cb->waitEvent(NfcEvent::POST_INIT_CPLT, 1500, &evSt) && evSt == NfcStatus::OK;
    }
    st = NfcStatus::FAILED;
    if (postInitOk && nfc->Prediscover(st) == HDF_SUCCESS && st == NfcStatus::OK) {
        preDiscOk = cb->waitEvent(NfcEvent::PRE_DISCOVER_CPLT, 1500, &evSt) && evSt == NfcStatus::OK;
    }

    /* 4. NCI 2.0 CORE_GET_CONFIG_CMD (TOTAL_DURATION) */
    bool getCfgOk = false;
    st = NfcStatus::FAILED;
    if (initOk && nfc->Write({0x20, 0x03, 0x02, 0x01, 0x00}, st) == HDF_SUCCESS && st == NfcStatus::OK) {
        if (cb->waitPacket(0x40, 0x03, 2000, pkt)) {
            std::printf("BINDER_NCI_GET_CFG_RSP len=%zu hex=%s\n", pkt.size(), toHex(pkt).c_str());
            getCfgOk = (pkt.size() >= 5u && pkt[3] == 0x00);
        }
    }

    /* 5. Close cleanly */
    st = NfcStatus::FAILED;
    int32_t clrc = nfc->Close(st);
    bool closeOk = (clrc == HDF_SUCCESS && st == NfcStatus::OK) &&
                   cb->waitEvent(NfcEvent::CLOSE_CPLT, 2000, &evSt) && evSt == NfcStatus::OK;
    std::printf("NFC_CLOSE rc=%d st=%d evt_ok=%d\n", clrc, static_cast<int>(st), closeOk ? 1 : 0);

    bool allGood = (vrc == HDF_SUCCESS && maj == 1u && min == 1u) && cfgOk && openEvt &&
                   resetOk && resetNtfOk && initOk && postInitOk && preDiscOk && getCfgOk && closeOk;
    std::printf("MARBLE_NFC_HDI_PROBE_SUMMARY sa1140=%d hdi_ver=%u.%u cfg_ok=%d open=%d "
                "nci_reset=%d nci_ntf=%d nci_init=%d post_init=%d pre_disc=%d get_cfg=%d "
                "close=%d nci_ver=0x%02x mfg=0x%02x chip=0x%02x fw=%u.%u all_ok=%d\n",
                sa1140 ? 1 : 0, maj, min, cfgOk ? 1 : 0, openEvt ? 1 : 0,
                resetOk ? 1 : 0, resetNtfOk ? 1 : 0, initOk ? 1 : 0,
                postInitOk ? 1 : 0, preDiscOk ? 1 : 0, getCfgOk ? 1 : 0, closeOk ? 1 : 0,
                nciVer, mfgId, chipType, fwMaj, fwMin, allGood ? 1 : 0);
    std::fflush(stdout);
    _exit(allGood ? 0 : 10);
}
