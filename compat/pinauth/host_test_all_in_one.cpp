/* SPDX-License-Identifier: MIT */
/*
 * Clean-room C++ offline host unit test for compat/pinauth/all_in_one_impl.cpp.
 *
 * Exercises the ACTUAL AllInOneImpl::Enroll / Authenticate / SetData code paths
 * against real upstream PinAuth + OpenSSL PBKDF2/Ed25519 + PinDb persistence:
 *   1. Standard 64-byte enroll + 64-byte auth (SUCCESS)
 *   2. 64-byte enroll + 96-byte HarmonyOS lockscreen unlock (Scrypt || SHA256 -> SUCCESS)
 *   3. 96-byte unlock with wrong 64-byte prefix + matching 32-byte tail (-> FAIL, not error 8)
 *   4. 96-byte enroll (algoVersion>=2 compat) + both 64-byte and 96-byte auth (-> SUCCESS)
 *   5. Invalid lengths (0, 32, 63, 65, 95, 97, 128) rejected with INVALID_PARAMETERS (8)
 */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <memory>
#include <string>
#include <vector>

#include "all_in_one_impl.h"
#include "adaptor_algorithm.h"
#include "attribute.h"
#include "defines.h"
#include "executor_func_common.h"
#include "file_operator.h"
#include "hilog/log_c.h"
#include "parameter.h"
#include "pin_db.h"
#include "pin_db_ops_base.h"
#include "sysparam_errno.h"
#include "thread_pool.h"

/* Host mocks for OHOS runtime facilities */
extern "C" int HiLogPrint(LogType type, LogLevel level, unsigned int domain,
                          const char *tag, const char *fmt, ...)
{
    (void)type; (void)level; (void)domain; (void)tag; (void)fmt;
    return 0;
}

extern "C" int GetDevUdid(char *udid, int size)
{
    static const char kTestUdid[] =
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    if (udid == nullptr || size < 65) {
        return EC_FAILURE;
    }
    std::memcpy(udid, kTestUdid, 65);
    return EC_SUCCESS;
}

namespace OHOS {
ThreadPool::ThreadPool(const std::string &name) : myName_(name), maxTaskNum_(0), running_(false) {}
ThreadPool::~ThreadPool() { Stop(); }
uint32_t ThreadPool::Start(int threadsNum) { (void)threadsNum; running_ = true; return 0; }
void ThreadPool::Stop() { running_ = false; }
void ThreadPool::AddTask(const Task &f) { if (f) { f(); } }
size_t ThreadPool::GetCurTaskNum() { return 0; }
} // namespace OHOS

namespace {

char g_tmp_root[PATH_MAX];

void mapPath(const char *orig, char *out, size_t out_sz)
{
    const char *prefix = DEFAULT_FILE_HEAD;
    size_t plen = std::strlen(prefix);
    if (orig != nullptr && std::strncmp(orig, prefix, plen) == 0) {
        int n = std::snprintf(out, out_sz, "%s/%s", g_tmp_root, orig + plen);
        assert(n > 0 && static_cast<size_t>(n) < out_sz);
    } else {
        int n = std::snprintf(out, out_sz, "%s", orig ? orig : "");
        assert(n >= 0 && static_cast<size_t>(n) < out_sz);
    }
}

bool testIsFileExist(const char *fileName)
{
    if (!fileName) return false;
    char mapped[PATH_MAX];
    mapPath(fileName, mapped, sizeof(mapped));
    FILE *fp = std::fopen(mapped, "rb");
    if (!fp) return false;
    std::fclose(fp);
    return true;
}

int32_t testGetFileLen(const char *fileName, uint32_t *len)
{
    if (!fileName || !len) return RESULT_BAD_PARAM;
    char mapped[PATH_MAX];
    mapPath(fileName, mapped, sizeof(mapped));
    FILE *fp = std::fopen(mapped, "rb");
    if (!fp) return RESULT_BAD_PARAM;
    if (std::fseek(fp, 0L, SEEK_END) != 0) { std::fclose(fp); return RESULT_GENERAL_ERROR; }
    long sz = std::ftell(fp);
    std::fclose(fp);
    if (sz < 0 || sz > UINT32_MAX) return RESULT_GENERAL_ERROR;
    *len = static_cast<uint32_t>(sz);
    return RESULT_SUCCESS;
}

int32_t testReadFile(const char *fileName, uint8_t *buf, uint32_t len)
{
    if (!fileName || !buf || len == 0) return RESULT_BAD_PARAM;
    char mapped[PATH_MAX];
    mapPath(fileName, mapped, sizeof(mapped));
    FILE *fp = std::fopen(mapped, "rb");
    if (!fp) return RESULT_BAD_PARAM;
    size_t n = std::fread(buf, 1, len, fp);
    std::fclose(fp);
    return (n == len) ? RESULT_SUCCESS : RESULT_BAD_READ;
}

int32_t testWriteFile(const char *fileName, const uint8_t *buf, uint32_t len)
{
    if (!fileName || !buf || len == 0) return RESULT_BAD_PARAM;
    char mapped[PATH_MAX];
    mapPath(fileName, mapped, sizeof(mapped));
    FILE *fp = std::fopen(mapped, "wb");
    if (!fp) return RESULT_BAD_PARAM;
    size_t n = std::fwrite(buf, 1, len, fp);
    std::fflush(fp);
    std::fclose(fp);
    return (n == len) ? RESULT_SUCCESS : RESULT_BAD_WRITE;
}

int32_t testDeleteFile(const char *fileName)
{
    if (!fileName) return RESULT_BAD_PARAM;
    char mapped[PATH_MAX];
    mapPath(fileName, mapped, sizeof(mapped));
    return (std::remove(mapped) == 0) ? RESULT_SUCCESS : RESULT_GENERAL_ERROR;
}

} // namespace

extern "C" FileOperator *GetDefaultFileOperator(void)
{
    static FileOperator op = {
        .isFileExist = testIsFileExist,
        .getFileLen = testGetFileLen,
        .readFile = testReadFile,
        .writeFile = testWriteFile,
        .deleteFile = testDeleteFile,
    };
    return &op;
}

namespace {

using namespace OHOS::HDI::PinAuth;

class RecordingCallback : public HdiIExecutorCallback {
public:
    int32_t OnResult(int32_t result, const std::vector<uint8_t> &extraInfo) override
    {
        onResultCalled = true;
        lastResult = result;
        lastResultTlv = extraInfo;
        return SUCCESS;
    }

    int32_t OnGetData(const std::vector<uint8_t> &algoParameter, uint64_t authSubType,
                      uint32_t algoVersion, const std::vector<uint8_t> &challenge,
                      const std::string &complexityReg) override
    {
        onGetDataCalled = true;
        lastAlgoParam = algoParameter;
        lastSubType = authSubType;
        lastAlgoVersion = algoVersion;
        lastChallenge = challenge;
        lastComplexityReg = complexityReg;
        return SUCCESS;
    }

    int32_t OnTip(int32_t tip, const std::vector<uint8_t> &extraInfo) override
    {
        (void)tip; (void)extraInfo;
        return SUCCESS;
    }

    int32_t OnMessage(int32_t destRole, const std::vector<uint8_t> &msg) override
    {
        (void)destRole; (void)msg;
        return SUCCESS;
    }

    void reset()
    {
        onResultCalled = false;
        onGetDataCalled = false;
        lastResult = -1;
        lastResultTlv.clear();
        lastAlgoParam.clear();
        lastSubType = 0;
        lastAlgoVersion = 0;
        lastChallenge.clear();
        lastComplexityReg.clear();
    }

    bool onResultCalled = false;
    bool onGetDataCalled = false;
    int32_t lastResult = -1;
    std::vector<uint8_t> lastResultTlv;
    std::vector<uint8_t> lastAlgoParam;
    uint64_t lastSubType = 0;
    uint32_t lastAlgoVersion = 0;
    std::vector<uint8_t> lastChallenge;
    std::string lastComplexityReg;
};

std::vector<uint8_t> buildSignedFwkExtraInfo(uint64_t scheduleId, int32_t authIntent, const KeyPair *fwkKey)
{
    Attribute *attr = GetAttributeDataBase(scheduleId, REMOTE_PIN_MSG_NONE);
    assert(attr != nullptr);
    assert(SetAttributeInt32(attr, ATTR_AUTH_PURPOSE, authIntent) == RESULT_SUCCESS);
    uint8_t buf[RESULT_TLV_LEN];
    uint32_t sz = sizeof(buf);
    assert(FormatTlvMsg(attr, fwkKey, buf, &sz) == RESULT_SUCCESS);
    FreeAttribute(&attr);
    return std::vector<uint8_t>(buf, buf + sz);
}

uint64_t extractTemplateIdFromTlv(uint64_t scheduleId, const std::vector<uint8_t> &execPubKey,
                                  const std::vector<uint8_t> &tlv, int32_t *tlvResultOut)
{
    Buffer *pubBuf = CreateBufferByData(execPubKey.data(), static_cast<uint32_t>(execPubKey.size()));
    assert(pubBuf != nullptr);
    Attribute *data = nullptr;
    assert(VerifyAndGetDataAttribute(scheduleId, &data, pubBuf, tlv.data(),
                                     static_cast<uint32_t>(tlv.size())) == RESULT_SUCCESS);
    assert(data != nullptr);
    uint64_t templateId = INVALID_TEMPLATE_ID;
    int32_t resCode = -1;
    assert(GetAttributeUint64(data, ATTR_TEMPLATE_ID, &templateId) == RESULT_SUCCESS);
    assert(GetAttributeInt32(data, ATTR_RESULT_CODE, &resCode) == RESULT_SUCCESS);
    if (tlvResultOut != nullptr) {
        *tlvResultOut = resCode;
    }
    FreeAttribute(&data);
    DestroyBuffer(pubBuf);
    return templateId;
}

std::vector<uint8_t> makeBytes(size_t len, uint8_t fill)
{
    std::vector<uint8_t> v(len, fill);
    for (size_t i = 0; i < len; ++i) {
        v[i] = static_cast<uint8_t>(fill ^ (i * 13u + 7u));
    }
    return v;
}

std::vector<uint8_t> appendSha256Tail(const std::vector<uint8_t> &scrypt64, uint8_t tailSeed)
{
    assert(scrypt64.size() == CONST_PIN_DATA_LEN);
    std::vector<uint8_t> out = scrypt64;
    std::vector<uint8_t> tail = makeBytes(32, tailSeed);
    out.insert(out.end(), tail.begin(), tail.end());
    assert(out.size() == CONST_PIN_DATA_LEN + 32);
    return out;
}

} // namespace

int main(void)
{
    char tmpl[] = "/tmp/marble_pinauth_cpp_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir != nullptr);
    std::snprintf(g_tmp_root, sizeof(g_tmp_root), "%s", dir);

    auto pinHdi = std::make_shared<OHOS::UserIam::PinAuth::PinAuth>();
    assert(pinHdi->Init() == RESULT_SUCCESS);

    AllInOneImpl impl(pinHdi);

    /* Query executor info to capture executor Ed25519 public key for TLV verification */
    HdiExecutorInfo execInfo{};
    assert(impl.GetExecutorInfo(execInfo) == HDF_SUCCESS);
    assert(execInfo.publicKey.size() == ED25519_FIX_PUBKEY_BUFFER_SIZE);

    /* Generate framework Ed25519 keypair and register it with AllInOneImpl */
    KeyPair *fwkKey = GenerateEd25519KeyPair();
    assert(fwkKey != nullptr);
    std::vector<uint8_t> fwkPub(fwkKey->pubKey->buf, fwkKey->pubKey->buf + fwkKey->pubKey->contentSize);
    assert(impl.OnRegisterFinish({}, fwkPub, {}) == HDF_SUCCESS);

    OHOS::sptr<RecordingCallback> cb = new RecordingCallback();

    /* 1. Standard 64-byte enroll */
    const uint64_t schedEnroll64 = 0x1001ULL;
    const std::vector<uint8_t> pin64 = makeBytes(CONST_PIN_DATA_LEN, 0x42);
    cb->reset();
    assert(impl.Enroll(schedEnroll64, {}, cb) == HDF_SUCCESS);
    assert(cb->onGetDataCalled && cb->lastAlgoParam.size() == CONST_SALT_LEN);
    assert(impl.SetData(schedEnroll64, PIN_SIX, pin64, 6, SUCCESS) == HDF_SUCCESS);
    assert(cb->onResultCalled && cb->lastResult == SUCCESS);
    int32_t tlvRes = -1;
    const uint64_t tid64 = extractTemplateIdFromTlv(schedEnroll64, execInfo.publicKey, cb->lastResultTlv, &tlvRes);
    assert(tid64 != INVALID_TEMPLATE_ID && tlvRes == SUCCESS);

    /* 2. Authenticate 64-byte enrolled template with 64-byte pinData -> SUCCESS */
    const uint64_t schedAuth64 = 0x1002ULL;
    auto extraAuth64 = buildSignedFwkExtraInfo(schedAuth64, 1, fwkKey);
    cb->reset();
    assert(impl.Authenticate(schedAuth64, {tid64}, extraAuth64, cb) == HDF_SUCCESS);
    assert(cb->onGetDataCalled);
    assert(impl.SetData(schedAuth64, PIN_SIX, pin64, 6, SUCCESS) == HDF_SUCCESS);
    assert(cb->onResultCalled && cb->lastResult == SUCCESS);
    (void)extractTemplateIdFromTlv(schedAuth64, execInfo.publicKey, cb->lastResultTlv, &tlvRes);
    assert(tlvRes == SUCCESS);

    /* 3. Authenticate 64-byte enrolled template with 96-byte Scrypt||SHA256 (lockscreen unlock!) -> SUCCESS */
    const uint64_t schedAuth96 = 0x1003ULL;
    const std::vector<uint8_t> pin96 = appendSha256Tail(pin64, 0xA5);
    auto extraAuth96 = buildSignedFwkExtraInfo(schedAuth96, 1, fwkKey);
    cb->reset();
    assert(impl.Authenticate(schedAuth96, {tid64}, extraAuth96, cb) == HDF_SUCCESS);
    assert(cb->onGetDataCalled);
    assert(impl.SetData(schedAuth96, PIN_SIX, pin96, 6, SUCCESS) == HDF_SUCCESS);
    assert(cb->onResultCalled && cb->lastResult == SUCCESS);
    (void)extractTemplateIdFromTlv(schedAuth96, execInfo.publicKey, cb->lastResultTlv, &tlvRes);
    assert(tlvRes == SUCCESS);

    /* 4. Authenticate with 96-byte payload where leading 64 bytes are WRONG -> FAIL (not INVALID_PARAMETERS=8!) */
    const uint64_t schedAuth96Wrong = 0x1004ULL;
    const std::vector<uint8_t> wrongPin96 = appendSha256Tail(makeBytes(CONST_PIN_DATA_LEN, 0x99), 0xA5);
    auto extraWrong96 = buildSignedFwkExtraInfo(schedAuth96Wrong, 1, fwkKey);
    cb->reset();
    assert(impl.Authenticate(schedAuth96Wrong, {tid64}, extraWrong96, cb) == HDF_SUCCESS);
    assert(impl.SetData(schedAuth96Wrong, PIN_SIX, wrongPin96, 6, SUCCESS) == HDF_SUCCESS);
    assert(cb->onResultCalled && cb->lastResult == FAIL);
    (void)extractTemplateIdFromTlv(schedAuth96Wrong, execInfo.publicKey, cb->lastResultTlv, &tlvRes);
    assert(tlvRes == FAIL);

    /* 5. Enroll with 96-byte Scrypt||SHA256 (algoVersion>=2 compat), then auth with BOTH 64-byte and 96-byte */
    const uint64_t schedEnroll96 = 0x2001ULL;
    const std::vector<uint8_t> pin2_64 = makeBytes(CONST_PIN_DATA_LEN, 0x77);
    const std::vector<uint8_t> pin2_96_enroll = appendSha256Tail(pin2_64, 0x11);
    const std::vector<uint8_t> pin2_96_unlock = appendSha256Tail(pin2_64, 0x22);
    cb->reset();
    assert(impl.Enroll(schedEnroll96, {}, cb) == HDF_SUCCESS);
    assert(impl.SetData(schedEnroll96, PIN_SIX, pin2_96_enroll, 6, SUCCESS) == HDF_SUCCESS);
    assert(cb->onResultCalled && cb->lastResult == SUCCESS);
    const uint64_t tid96 = extractTemplateIdFromTlv(schedEnroll96, execInfo.publicKey, cb->lastResultTlv, &tlvRes);
    assert(tid96 != INVALID_TEMPLATE_ID && tlvRes == SUCCESS);

    const uint64_t schedAuth96From64 = 0x2002ULL;
    auto extra2a = buildSignedFwkExtraInfo(schedAuth96From64, 1, fwkKey);
    cb->reset();
    assert(impl.Authenticate(schedAuth96From64, {tid96}, extra2a, cb) == HDF_SUCCESS);
    assert(impl.SetData(schedAuth96From64, PIN_SIX, pin2_64, 6, SUCCESS) == HDF_SUCCESS);
    assert(cb->onResultCalled && cb->lastResult == SUCCESS);

    const uint64_t schedAuth96From96 = 0x2003ULL;
    auto extra2b = buildSignedFwkExtraInfo(schedAuth96From96, 10005, fwkKey);
    cb->reset();
    assert(impl.Authenticate(schedAuth96From96, {tid96}, extra2b, cb) == HDF_SUCCESS);
    assert(impl.SetData(schedAuth96From96, PIN_SIX, pin2_96_unlock, 6, SUCCESS) == HDF_SUCCESS);
    assert(cb->onResultCalled && cb->lastResult == SUCCESS);

    /* 6. Reject invalid pinData lengths for BOTH enroll and auth with INVALID_PARAMETERS (8) */
    const size_t badLens[] = {0, 32, 63, 65, 95, 97, 128};
    uint64_t badSched = 0x3000ULL;
    for (size_t badLen : badLens) {
        std::vector<uint8_t> badData(badLen, 0x55);

        ++badSched;
        cb->reset();
        assert(impl.Enroll(badSched, {}, cb) == HDF_SUCCESS);
        assert(impl.SetData(badSched, PIN_SIX, badData, 6, SUCCESS) == HDF_SUCCESS);
        assert(cb->onResultCalled && cb->lastResult == INVALID_PARAMETERS);

        ++badSched;
        auto extraBad = buildSignedFwkExtraInfo(badSched, 1, fwkKey);
        cb->reset();
        assert(impl.Authenticate(badSched, {tid64}, extraBad, cb) == HDF_SUCCESS);
        assert(impl.SetData(badSched, PIN_SIX, badData, 6, SUCCESS) == HDF_SUCCESS);
        assert(cb->onResultCalled && cb->lastResult == INVALID_PARAMETERS);
    }

    assert(impl.Delete(tid64) == HDF_SUCCESS);
    assert(impl.Delete(tid96) == HDF_SUCCESS);
    DestroyKeyPair(fwkKey);
    assert(pinHdi->Close() == RESULT_SUCCESS);

    char cmd[PATH_MAX + 32];
    std::snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp_root);
    assert(std::system(cmd) == 0);

    std::printf("PIN_AUTH_ALL_IN_ONE_CPP_TEST_OK (64B/96B enroll+auth, wrong-96B FAIL, bad-len INVALID_PARAMETERS verified)\n");
    return 0;
}
