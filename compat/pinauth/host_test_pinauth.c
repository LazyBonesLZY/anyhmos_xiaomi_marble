/*
 * Clean-room offline host unit test for the pure-userspace OHOS pin_auth v3.0 C core.
 * Exercises real upstream OpenSSL PBKDF2/HMAC/Ed25519 + PinIndexDb persistence inside a
 * temporary directory without touching /data/.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "all_in_one_func.h"
#include "collector_func.h"
#include "file_operator.h"
#include "hilog/log_c.h"
#include "pin_db.h"
#include "pin_db_ops_base.h"
#include "verifier_func.h"

/* Host mock for HiLogPrint used by upstream adaptor_log.h / iam_logger.h */
int HiLogPrint(LogType type, LogLevel level, unsigned int domain,
               const char *tag, const char *fmt, ...)
{
    (void)type; (void)level; (void)domain; (void)tag; (void)fmt;
    return 0;
}

static char g_tmp_root[PATH_MAX];

static void map_path(const char *orig, char *out, size_t out_sz)
{
    const char *prefix = DEFAULT_FILE_HEAD;
    size_t plen = strlen(prefix);
    if (orig != NULL && strncmp(orig, prefix, plen) == 0) {
        int n = snprintf(out, out_sz, "%s/%s", g_tmp_root, orig + plen);
        assert(n > 0 && (size_t)n < out_sz);
    } else {
        int n = snprintf(out, out_sz, "%s", orig ? orig : "");
        assert(n >= 0 && (size_t)n < out_sz);
    }
}

static bool test_is_file_exist(const char *fileName)
{
    if (!fileName) return false;
    char mapped[PATH_MAX];
    map_path(fileName, mapped, sizeof(mapped));
    FILE *fp = fopen(mapped, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

static int32_t test_get_file_len(const char *fileName, uint32_t *len)
{
    if (!fileName || !len) return RESULT_BAD_PARAM;
    char mapped[PATH_MAX];
    map_path(fileName, mapped, sizeof(mapped));
    FILE *fp = fopen(mapped, "rb");
    if (!fp) return RESULT_BAD_PARAM;
    if (fseek(fp, 0L, SEEK_END) != 0) { fclose(fp); return RESULT_GENERAL_ERROR; }
    long sz = ftell(fp);
    fclose(fp);
    if (sz < 0 || sz > UINT32_MAX) return RESULT_GENERAL_ERROR;
    *len = (uint32_t)sz;
    return RESULT_SUCCESS;
}

static int32_t test_read_file(const char *fileName, uint8_t *buf, uint32_t len)
{
    if (!fileName || !buf || len == 0) return RESULT_BAD_PARAM;
    char mapped[PATH_MAX];
    map_path(fileName, mapped, sizeof(mapped));
    FILE *fp = fopen(mapped, "rb");
    if (!fp) return RESULT_BAD_PARAM;
    size_t n = fread(buf, 1, len, fp);
    fclose(fp);
    return (n == len) ? RESULT_SUCCESS : RESULT_BAD_READ;
}

static int32_t test_write_file(const char *fileName, const uint8_t *buf, uint32_t len)
{
    if (!fileName || !buf || len == 0) return RESULT_BAD_PARAM;
    char mapped[PATH_MAX];
    map_path(fileName, mapped, sizeof(mapped));
    FILE *fp = fopen(mapped, "wb");
    if (!fp) return RESULT_BAD_PARAM;
    size_t n = fwrite(buf, 1, len, fp);
    fflush(fp);
    fclose(fp);
    return (n == len) ? RESULT_SUCCESS : RESULT_BAD_WRITE;
}

static int32_t test_delete_file(const char *fileName)
{
    if (!fileName) return RESULT_BAD_PARAM;
    char mapped[PATH_MAX];
    map_path(fileName, mapped, sizeof(mapped));
    return (remove(mapped) == 0) ? RESULT_SUCCESS : RESULT_GENERAL_ERROR;
}

/* Override upstream GetDefaultFileOperator so DB persists into g_tmp_root */
FileOperator *GetDefaultFileOperator(void)
{
    static FileOperator op = {
        .isFileExist = test_is_file_exist,
        .getFileLen = test_get_file_len,
        .readFile = test_read_file,
        .writeFile = test_write_file,
        .deleteFile = test_delete_file,
    };
    return &op;
}

int main(void)
{
    char tmpl[] = "/tmp/marble_pinauth_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir != NULL);
    snprintf(g_tmp_root, sizeof(g_tmp_root), "%s", dir);

    /* 1. Initialize empty PinDb and Ed25519 executor keypairs */
    assert(LoadPinDb());
    assert(GenerateAllInOneKeyPair() == RESULT_SUCCESS);
    assert(GenerateCollectorKeyPair() == RESULT_SUCCESS);
    assert(GenerateVerifierKeyPair() == RESULT_SUCCESS);

    /* 2. Verify executor info returns non-zero Ed25519 public key */
    PinExecutorInfo einfo;
    memset(&einfo, 0, sizeof(einfo));
    assert(DoGetAllInOneExecutorInfo(&einfo) == RESULT_SUCCESS);
    bool non_zero_pk = false;
    for (size_t i = 0; i < sizeof(einfo.pubKey); i++) {
        if (einfo.pubKey[i] != 0) non_zero_pk = true;
    }
    assert(non_zero_pk);

    /* 3. Generate salt + algoVersion using a 65-byte UDID */
    uint8_t salt[CONST_SALT_LEN] = {0};
    uint32_t salt_len = CONST_SALT_LEN;
    uint32_t algo_ver = 999;
    uint8_t fake_udid[65] = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    assert(DoGenerateAlgoParameter(salt, &salt_len, &algo_ver, fake_udid, sizeof(fake_udid)) == RESULT_SUCCESS);
    assert(salt_len == CONST_SALT_LEN);

    /* 4. Enroll PIN template directly via PinDb & verify persistence */
    PinEnrollParam enroll;
    memset(&enroll, 0, sizeof(enroll));
    enroll.scheduleId = 0x11223344ULL;
    enroll.subType = 10000ULL; /* PIN_SIX */
    memcpy(enroll.salt, salt, CONST_SALT_LEN);
    memset(enroll.pinData, 0x36, CONST_PIN_DATA_LEN);
    enroll.pinLength = 6;

    uint64_t template_id = INVALID_TEMPLATE_ID;
    Buffer *root_secret = CreateBufferBySize(32);
    assert(root_secret != NULL);
    assert(AddPin(&enroll, &template_id, root_secret) == RESULT_SUCCESS);
    assert(template_id != INVALID_TEMPLATE_ID);
    assert(root_secret->contentSize == 32);

    /* Check PinIndexDb file was persisted to temp dir */
    assert(test_is_file_exist(PIN_INDEX_NAME));

    /* 5. Reload DB from disk to verify cross-reboot persistence */
    DestroyPinDb();
    assert(LoadPinDb());

    PinCredentialInfos info;
    memset(&info, 0, sizeof(info));
    assert(DoQueryPinInfo(template_id, &info) == RESULT_SUCCESS);
    assert(info.subType == 10000ULL);
    assert(info.remainTimes == 5u);
    assert(info.credentialLength == 6u);

    /* 6. Authenticate with WRONG PIN -> RESULT_COMPARE_FAIL and remainTimes decrements */
    uint8_t wrong_pin[CONST_PIN_DATA_LEN];
    memset(wrong_pin, 0x99, sizeof(wrong_pin));
    Buffer *wrong_buf = CreateBufferByData(wrong_pin, sizeof(wrong_pin));
    Buffer *auth_secret = CreateBufferBySize(32);
    ResultCode cmp_ret = RESULT_GENERAL_ERROR;
    assert(AuthPinById(wrong_buf, template_id, 6, auth_secret, &cmp_ret) == RESULT_SUCCESS);
    assert(cmp_ret == RESULT_COMPARE_FAIL);

    memset(&info, 0, sizeof(info));
    assert(DoQueryPinInfo(template_id, &info) == RESULT_SUCCESS);
    assert(info.remainTimes == 4u);

    /* 7. Authenticate with CORRECT PIN -> RESULT_SUCCESS and remainTimes resets to 5 */
    Buffer *good_buf = CreateBufferByData(enroll.pinData, sizeof(enroll.pinData));
    cmp_ret = RESULT_GENERAL_ERROR;
    assert(AuthPinById(good_buf, template_id, 6, auth_secret, &cmp_ret) == RESULT_SUCCESS);
    assert(cmp_ret == RESULT_SUCCESS);
    assert(auth_secret->contentSize == root_secret->contentSize);
    assert(memcmp(auth_secret->buf, root_secret->buf, root_secret->contentSize) == 0);

    memset(&info, 0, sizeof(info));
    assert(DoQueryPinInfo(template_id, &info) == RESULT_SUCCESS);
    assert(info.remainTimes == 5u);

    /* 8. Enroll + signed TLV via DoEnrollPin */
    Buffer *tlv = CreateBufferBySize(RESULT_TLV_LEN);
    assert(tlv != NULL);
    enroll.scheduleId = 0x55667788ULL;
    assert(DoEnrollPin(&enroll, tlv) == RESULT_SUCCESS);
    assert(tlv->contentSize > 32);

    /* 9. Delete template and clean up */
    assert(DoDeleteTemplate(template_id) == RESULT_SUCCESS);
    DestroyBuffer(tlv);
    DestroyBuffer(good_buf);
    DestroyBuffer(wrong_buf);
    DestroyBuffer(auth_secret);
    DestroyBuffer(root_secret);
    DestroyAllInOneKeyPair();
    DestroyCollectorKeyPair();
    DestroyVerifierKeyPair();
    DestroyPinDb();

    /* Clean up temp directory */
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp_root);
    assert(system(cmd) == 0);

    printf("PIN_AUTH_HOST_TEST_OK (keypair + salt + enroll + reload + wrong-PIN + right-PIN + TLV + delete verified)\n");
    return 0;
}
