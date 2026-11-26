/* SPDX-License-Identifier: MIT
 * Silent verification probe for donor A2DP Audio HDI + IBluetoothAudioSession V2.0 SMQ.
 * Exercises a2dp_host on-demand load, SHM queue round-trip, and cycle-2 reload without touching speaker PAL.
 */
#include "bluetooth/a2dp/v2_0/bluetooth_audio_types.h"
#include "bluetooth/a2dp/v2_0/ibluetooth_audio_callback.h"
#include "bluetooth/a2dp/v2_0/ibluetooth_audio_session.h"
#include "audio_proxy_manager.h"
#include "devmgr_hdi.h"
#include "hdf_remote_service.h"
#include "ipc_skeleton.h"
#include "servmgr_hdi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <queue>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace A2dp = OHOS::HDI::Bluetooth::A2dp::V2_0;
namespace BtAudio = OHOS::HDI::Audio_Bluetooth;

namespace {

constexpr char kSessionService[] = "bluetooth_audio_session_service";
constexpr char kAudioService[] = "audio_bluetooth_hdi_service";
constexpr char kProxyLib[] = "/vendor/lib64/passthrough/libaudio_bluetooth_hdi_proxy_server.z.so";
constexpr char kHdfIpcLib[] = "libhdf_ipc_adapter.z.so";

struct PendingOp {
    A2dp::Operation op;
};

class ProbeAudioCallback : public A2dp::IBluetoothAudioCallback {
public:
    ProbeAudioCallback() = default;
    ~ProbeAudioCallback() override
    {
        StopWorker();
    }

    void AttachSession(const OHOS::sptr<A2dp::IBluetoothAudioSession> &session)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = session;
        if (!workerStarted_) {
            stopWorker_ = false;
            worker_ = std::thread(&ProbeAudioCallback::WorkerLoop, this);
            workerStarted_ = true;
        }
    }

    void StopWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!workerStarted_) {
                session_.clear();
                return;
            }
            stopWorker_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        workerStarted_ = false;
        session_.clear();
    }

    int32_t StartRender() override
    {
        int count = ++startCalls_;
        std::printf("A2DP_PROBE_CB StartRender count=%d\n", count);
        Enqueue(A2dp::START_RENDER);
        return 0;
    }

    int32_t SuspendRender() override
    {
        int count = ++suspendCalls_;
        std::printf("A2DP_PROBE_CB SuspendRender count=%d\n", count);
        Enqueue(A2dp::SUSPEND_RENDER);
        return 0;
    }

    int32_t StopRender() override
    {
        int count = ++stopCalls_;
        std::printf("A2DP_PROBE_CB StopRender count=%d\n", count);
        return 0;
    }

    int32_t ChangeCodec(const A2dp::CodecParam &codecParam) override
    {
        std::printf("A2DP_PROBE_CB ChangeCodec rate=%u bits=%u ch=%u\n",
                    codecParam.sampleRate, codecParam.bitsPerSample, codecParam.channelMode);
        return 0;
    }

    int32_t GetRenderPosition(uint64_t &latencyNs, uint64_t &frames, A2dp::BluetoothTimeStamp &time) override
    {
        latencyNs = 20000000ULL;
        frames = 0;
        time.tvSec = 0;
        time.tvNSec = 0;
        return 0;
    }

    int StartCalls() const { return startCalls_.load(); }
    int LastAckRc() const { return lastAckRc_.load(); }

private:
    void Enqueue(A2dp::Operation op)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ops_.push({op});
        }
        cv_.notify_one();
    }

    void WorkerLoop()
    {
        while (true) {
            PendingOp item{};
            OHOS::sptr<A2dp::IBluetoothAudioSession> session;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopWorker_ || !ops_.empty(); });
                if (stopWorker_ && ops_.empty()) {
                    return;
                }
                item = ops_.front();
                ops_.pop();
                session = session_;
            }
            if (session != nullptr) {
                int32_t rc = session->RenderOperationResult(item.op, A2dp::SUCCESS);
                lastAckRc_.store(rc);
                std::printf("A2DP_PROBE_ACK op=%d rc=%d\n", static_cast<int>(item.op), rc);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<PendingOp> ops_;
    OHOS::sptr<A2dp::IBluetoothAudioSession> session_;
    std::thread worker_;
    bool workerStarted_ = false;
    bool stopWorker_ = false;
    std::atomic<int> startCalls_{0};
    std::atomic<int> suspendCalls_{0};
    std::atomic<int> stopCalls_{0};
    std::atomic<int> lastAckRc_{0};
};

using RecycleFn = void (*)(HdfRemoteService *);

bool CheckServicesReady(HDIServiceManager *servmgr, RecycleFn recycle, int cycle, const char *phaseTag)
{
    HdfRemoteService *sSession = servmgr->GetService(servmgr, kSessionService);
    HdfRemoteService *sAudio = servmgr->GetService(servmgr, kAudioService);
    bool okSession = sSession != nullptr;
    bool okAudio = sAudio != nullptr;
    std::printf("%s cycle=%d svc_session=%d svc_audio=%d\n", phaseTag, cycle, okSession ? 1 : 0, okAudio ? 1 : 0);
    if (sSession != nullptr && recycle != nullptr) {
        recycle(sSession);
    }
    if (sAudio != nullptr && recycle != nullptr) {
        recycle(sAudio);
    }
    return okSession && okAudio;
}

bool LoadA2dpPair(HDIDeviceManager *devmgr, HDIServiceManager *servmgr, RecycleFn recycle, int cycle)
{
    int32_t rc1 = devmgr->LoadDevice(devmgr, kSessionService);
    int32_t rc2 = devmgr->LoadDevice(devmgr, kAudioService);
    std::printf("A2DP_PROBE_LOAD cycle=%d load_session=%d load_audio=%d\n", cycle, rc1, rc2);
    if (rc1 != 0 || rc2 != 0) {
        return false;
    }
    for (int i = 0; i < 20; ++i) {
        if (CheckServicesReady(servmgr, recycle, cycle, "A2DP_PROBE_SERVICES")) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

void UnloadA2dpPair(HDIDeviceManager *devmgr, int cycle)
{
    int32_t rcAudio = devmgr->UnloadDevice(devmgr, kAudioService);
    int32_t rcSession = devmgr->UnloadDevice(devmgr, kSessionService);
    std::printf("A2DP_PROBE_UNLOAD cycle=%d unload_audio=%d unload_session=%d\n", cycle, rcAudio, rcSession);
}

} // namespace

int main()
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    alarm(45);

    OHOS::IPCSkeleton::SetMaxWorkThreadNum(4);
    std::thread binderThread([]() { OHOS::IPCSkeleton::JoinWorkThread(); });
    binderThread.detach();

    void *ipcLib = dlopen(kHdfIpcLib, RTLD_NOW | RTLD_LOCAL);
    RecycleFn recycleRemote = ipcLib ? reinterpret_cast<RecycleFn>(dlsym(ipcLib, "HdfRemoteServiceRecycle")) : nullptr;

    HDIDeviceManager *devmgr = HDIDeviceManagerGet();
    HDIServiceManager *servmgr = HDIServiceManagerGet();
    if (devmgr == nullptr || servmgr == nullptr) {
        std::printf("A2DP_PROBE_ERR devmgr=%p servmgr=%p\n", static_cast<void *>(devmgr), static_cast<void *>(servmgr));
        return 2;
    }

    int exitCode = 0;
    bool needUnload = false;
    do {
        if (!LoadA2dpPair(devmgr, servmgr, recycleRemote, 1)) {
            exitCode = 3;
            break;
        }
        needUnload = true;

        OHOS::sptr<A2dp::IBluetoothAudioSession> session = A2dp::IBluetoothAudioSession::Get(kSessionService, false);
        if (session == nullptr) {
            std::printf("A2DP_PROBE_ERR session_get=null\n");
            exitCode = 4;
            break;
        }

        OHOS::sptr<ProbeAudioCallback> cb = new ProbeAudioCallback();
        cb->AttachSession(session);

        A2dp::StartSessionParams params{};
        params.sessionType = A2dp::SOFTWARE_ENCODING;
        params.codecParam.sampleRate = 48000;
        params.codecParam.bitsPerSample = 16;
        params.codecParam.channelMode = 2;

        std::shared_ptr<OHOS::HDI::Base::SharedMemQueue<uint8_t>> smq;
        int32_t startSessRc = session->StartSessionWithParams(params, cb, smq);
        bool smqGood = smq != nullptr && smq->IsGood();
        size_t smqSize = smqGood ? smq->GetSize() : 0;
        std::printf("A2DP_PROBE_PHASE2 start_session_rc=%d smq_good=%d smq_size=%zu\n",
                    startSessRc, smqGood ? 1 : 0, smqSize);
        if (startSessRc != 0 || !smqGood) {
            cb->StopWorker();
            exitCode = 5;
            break;
        }

        void *proxyLib = dlopen(kProxyLib, RTLD_NOW | RTLD_LOCAL);
        if (proxyLib == nullptr) {
            std::printf("A2DP_PROBE_ERR dlopen_proxy=%s\n", dlerror());
            (void)session->StopSession(A2dp::SOFTWARE_ENCODING);
            cb->StopWorker();
            exitCode = 6;
            break;
        }
        using GetProxyFuncsFn = BtAudio::AudioProxyManager *(*)();
        auto getFuncs = reinterpret_cast<GetProxyFuncsFn>(dlsym(proxyLib, "GetAudioProxyManagerFuncs"));
        BtAudio::AudioProxyManager *mgr = getFuncs ? getFuncs() : nullptr;
        if (mgr == nullptr || mgr->GetAllAdapters == nullptr || mgr->LoadAdapter == nullptr) {
            std::printf("A2DP_PROBE_ERR proxy_mgr=null\n");
            (void)session->StopSession(A2dp::SOFTWARE_ENCODING);
            cb->StopWorker();
            exitCode = 7;
            break;
        }

        BtAudio::AudioAdapterDescriptor *descs = nullptr;
        int32_t adapterCount = 0;
        int32_t allRc = mgr->GetAllAdapters(mgr, &descs, &adapterCount);
        const BtAudio::AudioAdapterDescriptor *btA2dpDesc = nullptr;
        bool hasFastA2dp = false;
        std::printf("A2DP_PROBE_ADAPTERS rc=%d count=%d\n", allRc, adapterCount);
        for (int32_t i = 0; descs != nullptr && i < adapterCount; ++i) {
            const char *aname = descs[i].adapterName ? descs[i].adapterName : "(null)";
            std::printf("A2DP_PROBE_ADAPTER idx=%d name=%s ports=%u\n", i, aname, descs[i].portNum);
            if (std::strcmp(aname, "bt_a2dp") == 0) {
                btA2dpDesc = &descs[i];
            } else if (std::strcmp(aname, "bt_a2dp_fast") == 0) {
                hasFastA2dp = true;
            }
        }
        if (allRc != 0 || btA2dpDesc == nullptr || !hasFastA2dp) {
            std::printf("A2DP_PROBE_ERR missing_a2dp_adapters has_fast=%d\n", hasFastA2dp ? 1 : 0);
            if (mgr->remote && recycleRemote) recycleRemote(mgr->remote);
            (void)session->StopSession(A2dp::SOFTWARE_ENCODING);
            cb->StopWorker();
            exitCode = 8;
            break;
        }

        BtAudio::AudioAdapter *adapter = nullptr;
        int32_t loadAdtRc = mgr->LoadAdapter(mgr, btA2dpDesc, &adapter);
        int32_t initPortsRc = (loadAdtRc == 0 && adapter != nullptr) ? adapter->InitAllPorts(adapter) : -1;
        std::printf("A2DP_PROBE_LOAD_ADAPTER rc=%d init_ports_rc=%d\n", loadAdtRc, initPortsRc);
        if (loadAdtRc != 0 || adapter == nullptr || initPortsRc != 0) {
            if (adapter != nullptr) mgr->UnloadAdapter(mgr, adapter);
            if (mgr->remote && recycleRemote) recycleRemote(mgr->remote);
            (void)session->StopSession(A2dp::SOFTWARE_ENCODING);
            cb->StopWorker();
            exitCode = 9;
            break;
        }

        BtAudio::AudioDeviceDescriptor devDesc{};
        devDesc.portId = 0;
        devDesc.pins = BtAudio::PIN_OUT_SPEAKER;
        devDesc.desc = const_cast<char *>("");

        BtAudio::AudioSampleAttributes attrs{};
        attrs.type = BtAudio::AUDIO_IN_MEDIA;
        attrs.interleaved = false;
        attrs.format = BtAudio::AUDIO_FORMAT_TYPE_PCM_16_BIT;
        attrs.sampleRate = 48000;
        attrs.channelCount = 2;
        attrs.period = 0;
        attrs.frameSize = 4;
        attrs.isBigEndian = false;
        attrs.isSignedData = true;
        attrs.startThreshold = 0;
        attrs.stopThreshold = 0;
        attrs.silenceThreshold = 0;

        BtAudio::AudioRender *render = nullptr;
        int32_t createRc = adapter->CreateRender(adapter, &devDesc, &attrs, &render);
        std::printf("A2DP_PROBE_CREATE_RENDER rc=%d render=%d\n", createRc, render != nullptr ? 1 : 0);
        if (createRc != 0 || render == nullptr) {
            mgr->UnloadAdapter(mgr, adapter);
            if (mgr->remote && recycleRemote) recycleRemote(mgr->remote);
            (void)session->StopSession(A2dp::SOFTWARE_ENCODING);
            cb->StopWorker();
            exitCode = 10;
            break;
        }

        (void)smq->Reset();
        int32_t startRc = render->control.Start(render);
        uint32_t latencyMs = 0;
        int32_t latRc = render->GetLatency(render, &latencyMs);
        std::printf("A2DP_PROBE_RENDER_START rc=%d cb_start=%d ack_rc=%d lat_rc=%d latency_ms=%u\n",
                    startRc, cb->StartCalls(), cb->LastAckRc(), latRc, latencyMs);

        constexpr size_t kFrameBytes = 3840;
        std::vector<uint8_t> txBuf(kFrameBytes);
        for (size_t i = 0; i < kFrameBytes; ++i) {
            txBuf[i] = static_cast<uint8_t>((i * 37 + 13) & 0xff);
        }
        uint64_t replyBytes = 0;
        int32_t writeRc = render->RenderFrame(render, txBuf.data(), txBuf.size(), &replyBytes);

        std::vector<uint8_t> rxBuf;
        rxBuf.reserve(kFrameBytes);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (rxBuf.size() < kFrameBytes && std::chrono::steady_clock::now() < deadline) {
            size_t avail = smq->GetAvalidReadSize();
            if (avail > 0) {
                size_t want = kFrameBytes - rxBuf.size();
                if (avail > want) {
                    avail = want;
                }
                size_t oldSz = rxBuf.size();
                rxBuf.resize(oldSz + avail);
                int rrc = smq->ReadNonBlocking(rxBuf.data() + oldSz, avail);
                if (rrc != 0) {
                    rxBuf.resize(oldSz);
                    break;
                }
            } else {
                usleep(5000);
            }
        }
        bool exactMatch = (rxBuf.size() == txBuf.size()) && (std::memcmp(rxBuf.data(), txBuf.data(), kFrameBytes) == 0);
        std::printf("A2DP_PROBE_PHASE3 write_rc=%d reply_bytes=%llu rx_bytes=%zu exact_match=%d\n",
                    writeRc, static_cast<unsigned long long>(replyBytes), rxBuf.size(), exactMatch ? 1 : 0);

        int32_t stopRc = render->control.Stop(render);
        int32_t destroyRc = adapter->DestroyRender(adapter, render);
        mgr->UnloadAdapter(mgr, adapter);
        if (mgr->remote && recycleRemote) {
            recycleRemote(mgr->remote);
            mgr->remote = nullptr;
        }
        dlclose(proxyLib);

        int32_t stopSessRc = session->StopSession(A2dp::SOFTWARE_ENCODING);
        cb->StopWorker();
        const int cbStartCalls = cb->StartCalls();
        session.clear();
        cb.clear();
        smq.reset();
        std::printf("A2DP_PROBE_CLEANUP stop_rc=%d destroy_rc=%d stop_sess_rc=%d\n", stopRc, destroyRc, stopSessRc);

        if (startRc != 0 || cbStartCalls < 1 || writeRc != 0 || replyBytes != kFrameBytes || !exactMatch ||
            stopRc != 0 || destroyRc != 0 || stopSessRc != 0) {
            exitCode = 11;
            break;
        }

        // Phase 4: Unload and verify clean cycle-2 reload (tests ondemand + once lifecycle).
        usleep(200000);
        UnloadA2dpPair(devmgr, 1);
        needUnload = false;
        usleep(500000);

        bool reloadOk = LoadA2dpPair(devmgr, servmgr, recycleRemote, 2);
        std::printf("A2DP_PROBE_PHASE4 reload_ok=%d\n", reloadOk ? 1 : 0);
        if (!reloadOk) {
            exitCode = 12;
            break;
        }
        UnloadA2dpPair(devmgr, 2);
    } while (false);

    if (needUnload) {
        UnloadA2dpPair(devmgr, 1);
    }
    HDIServiceManagerRelease(servmgr);
    HDIDeviceManagerRelease(devmgr);
    std::printf("A2DP_PROBE_EXIT=%d\n", exitCode);
    return exitCode;
}
