/*
 * Sealed key store for the marble software HUKS backend.
 *
 *   /data/service/el1/public/huks_soft/.kek          device KEK, 0600 root
 *   /data/service/el1/public/huks_soft/<uid>/<id>.key one sealed key per file
 *
 * Each key file is AES-256-GCM sealed under the device KEK.  The KEK is a
 * plain file: this is software key storage, NOT a hardware root of trust.
 * Anyone who can read .kek (root, or the huks service uid) can unwrap every
 * key in the store.  That is stated again in work/marble-huks-report.md.
 *
 * Overridable for tests / alternative deployments:
 *   HUKS_SOFT_ROOT   default /data/service/el1/public/huks_soft
 *   HUKS_SOFT_UID    default geteuid()
 */

#include "soft_huks.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#define DEFAULT_ROOT "/data/service/el1/public/huks_soft"
#define KEK_FILE ".kek"
#define KEK_LEN 32u
#define GCM_IV_LEN 12u
#define GCM_TAG_LEN 16u
#define SEAL_MAGIC "SHS1"
#define SEAL_HDR_LEN 24u
#define PATH_MAX_LEN 512

struct StoreState {
    int ready;
    int32_t err;
    char root[PATH_MAX_LEN];
    char dir[PATH_MAX_LEN];
    uint8_t kek[KEK_LEN];
};

static struct StoreState gStore;
static pthread_mutex_t gStoreMutex = PTHREAD_MUTEX_INITIALIZER;

static int32_t make_dirs(const char *path)
{
    char tmp[PATH_MAX_LEN];
    size_t len;
    size_t i;

    len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return HKS_ERROR_INVALID_ARGUMENT;
    }
    (void)memcpy(tmp, path, len + 1);

    for (i = 1; i < len; i++) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
            return HKS_ERROR_MAKE_DIR_FAIL;
        }
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        return HKS_ERROR_MAKE_DIR_FAIL;
    }
    return HKS_SUCCESS;
}

static int32_t read_full(int fd, uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return HKS_ERROR_READ_FILE_FAIL;
        }
        if (n == 0) {
            return HKS_ERROR_READ_FILE_FAIL;
        }
        done += (size_t)n;
    }
    return HKS_SUCCESS;
}

static int32_t write_full(int fd, const uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return HKS_ERROR_WRITE_FILE_FAIL;
        }
        done += (size_t)n;
    }
    return HKS_SUCCESS;
}

static int32_t load_or_create_kek(char *kekPath, size_t kekPathLen)
{
    uint8_t buf[KEK_LEN];
    int fd;
    int32_t ret;

    (void)snprintf(kekPath, kekPathLen, "%s/%s", gStore.root, KEK_FILE);

    fd = open(kekPath, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ret = read_full(fd, buf, sizeof(buf));
        (void)close(fd);
        if (ret != HKS_SUCCESS) {
            return HKS_ERROR_INVALID_KEY_FILE;
        }
        (void)memcpy(gStore.kek, buf, sizeof(buf));
        return HKS_SUCCESS;
    }
    if (errno != ENOENT) {
        return HKS_ERROR_OPEN_FILE_FAIL;
    }

    fd = open(kekPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return HKS_ERROR_OPEN_FILE_FAIL;
    }
    if (RAND_bytes(buf, (int)sizeof(buf)) != 1) {
        (void)close(fd);
        return HKS_ERROR_CRYPTO_ENGINE_ERROR;
    }
    ret = write_full(fd, buf, sizeof(buf));
    if (ret == HKS_SUCCESS) {
        (void)fsync(fd);
    }
    (void)close(fd);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    (void)memcpy(gStore.kek, buf, sizeof(buf));
    return HKS_SUCCESS;
}

int32_t soft_store_ensure_ready(void)
{
    int32_t ret;
    const char *rootEnv = getenv("HUKS_SOFT_ROOT");
    const char *uidEnv = getenv("HUKS_SOFT_UID");
    char kekPath[PATH_MAX_LEN];

    (void)pthread_mutex_lock(&gStoreMutex);
    if (gStore.ready) {
        ret = gStore.err;
        (void)pthread_mutex_unlock(&gStoreMutex);
        return ret;
    }

    (void)snprintf(gStore.root, sizeof(gStore.root), "%s",
        (rootEnv != NULL && rootEnv[0] != '\0') ? rootEnv : DEFAULT_ROOT);

    unsigned int uid = (unsigned int)geteuid();
    if (uidEnv != NULL && uidEnv[0] != '\0') {
        uid = (unsigned int)strtoul(uidEnv, NULL, 10);
    }
    (void)snprintf(gStore.dir, sizeof(gStore.dir), "%s/%u", gStore.root, uid);

    ret = make_dirs(gStore.root);
    if (ret == HKS_SUCCESS) {
        ret = make_dirs(gStore.dir);
    }
    if (ret == HKS_SUCCESS) {
        ret = load_or_create_kek(kekPath, sizeof(kekPath));
    }

    gStore.ready = 1;
    gStore.err = ret;
    (void)pthread_mutex_unlock(&gStoreMutex);
    return ret;
}

static void seal_header(uint8_t hdr[SEAL_HDR_LEN], uint32_t plainLen)
{
    (void)memcpy(hdr, SEAL_MAGIC, 4);
    hdr[4] = 1; /* version */
    hdr[5] = 0;
    hdr[6] = 0;
    hdr[7] = 0;
    hdr[8] = (uint8_t)(plainLen & 0xffu);
    hdr[9] = (uint8_t)((plainLen >> 8) & 0xffu);
    hdr[10] = (uint8_t)((plainLen >> 16) & 0xffu);
    hdr[11] = (uint8_t)((plainLen >> 24) & 0xffu);
    /* hdr[12..23] = GCM IV, filled by caller */
}

static int32_t seal_blob(const uint8_t *plain, uint32_t plainLen, uint8_t **out, uint32_t *outLen)
{
    uint8_t hdr[SEAL_HDR_LEN];
    uint8_t *buf = NULL;
    EVP_CIPHER_CTX *ctx = NULL;
    int len = 0;
    int total = 0;
    int32_t ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;

    if (plainLen > (uint32_t)(64u * 1024u * 1024u)) {
        return HKS_ERROR_INVALID_ARGUMENT;
    }
    buf = malloc(SEAL_HDR_LEN + plainLen + GCM_TAG_LEN);
    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    seal_header(hdr, plainLen);
    if (RAND_bytes(hdr + 12, (int)GCM_IV_LEN) != 1) {
        goto out;
    }

    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        ret = HKS_ERROR_MALLOC_FAIL;
        goto out;
    }
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, gStore.kek, hdr + 12) != 1) {
        goto out;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)GCM_IV_LEN, NULL) != 1) {
        goto out;
    }
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, NULL, NULL) != 1) {
        goto out;
    }
    if (EVP_EncryptUpdate(ctx, NULL, &len, hdr, (int)SEAL_HDR_LEN) != 1) {
        goto out;
    }
    if (EVP_EncryptUpdate(ctx, buf + SEAL_HDR_LEN, &len, plain, (int)plainLen) != 1) {
        goto out;
    }
    total = len;
    if (EVP_EncryptFinal_ex(ctx, buf + SEAL_HDR_LEN + total, &len) != 1) {
        goto out;
    }
    total += len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)GCM_TAG_LEN,
            buf + SEAL_HDR_LEN + total) != 1) {
        goto out;
    }
    (void)memcpy(buf, hdr, SEAL_HDR_LEN);
    *out = buf;
    *outLen = SEAL_HDR_LEN + (uint32_t)total + GCM_TAG_LEN;
    buf = NULL;
    ret = HKS_SUCCESS;

out:
    EVP_CIPHER_CTX_free(ctx);
    if (buf != NULL) {
        (void)OPENSSL_cleanse(buf, SEAL_HDR_LEN + plainLen + GCM_TAG_LEN);
        free(buf);
    }
    return ret;
}

static int32_t open_blob(const uint8_t *sealed, uint32_t sealedLen, uint8_t **out, uint32_t *outLen)
{
    const uint8_t *hdr = sealed;
    const uint8_t *ct;
    const uint8_t *tag;
    uint32_t plainLen;
    uint8_t *buf = NULL;
    EVP_CIPHER_CTX *ctx = NULL;
    int len = 0;
    int32_t ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;

    if (sealedLen < SEAL_HDR_LEN + GCM_TAG_LEN || memcmp(hdr, SEAL_MAGIC, 4) != 0 || hdr[4] != 1) {
        return HKS_ERROR_INVALID_KEY_FILE;
    }
    plainLen = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) |
        ((uint32_t)hdr[11] << 24);
    if (plainLen > sealedLen) {
        return HKS_ERROR_INVALID_KEY_FILE;
    }
    ct = sealed + SEAL_HDR_LEN;
    tag = sealed + sealedLen - GCM_TAG_LEN;

    buf = malloc(plainLen > 0 ? plainLen : 1u);
    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        ret = HKS_ERROR_MALLOC_FAIL;
        goto out;
    }
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, gStore.kek, hdr + 12) != 1) {
        goto out;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)GCM_IV_LEN, NULL) != 1) {
        goto out;
    }
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, NULL, NULL) != 1) {
        goto out;
    }
    if (EVP_DecryptUpdate(ctx, NULL, &len, hdr, (int)SEAL_HDR_LEN) != 1) {
        goto out;
    }
    if (EVP_DecryptUpdate(ctx, buf, &len, ct, (int)(sealedLen - SEAL_HDR_LEN - GCM_TAG_LEN)) != 1) {
        goto out;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)GCM_TAG_LEN, (void *)tag) != 1) {
        goto out;
    }
    if (EVP_DecryptFinal_ex(ctx, buf + len, &len) != 1) {
        /* tag mismatch: wrong KEK or tampered file */
        ret = HKS_ERROR_VERIFICATION_FAILED;
        goto out;
    }
    *out = buf;
    *outLen = plainLen;
    buf = NULL;
    ret = HKS_SUCCESS;

out:
    EVP_CIPHER_CTX_free(ctx);
    if (buf != NULL) {
        (void)OPENSSL_cleanse(buf, plainLen);
        free(buf);
    }
    return ret;
}

static void key_path(const uint8_t keyId[SOFT_HUKS_KEYID_LEN], char *path, size_t pathLen)
{
    char hex[SOFT_HUKS_KEYID_LEN * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < SOFT_HUKS_KEYID_LEN; i++) {
        hex[i * 2] = digits[(keyId[i] >> 4) & 0xfu];
        hex[i * 2 + 1] = digits[keyId[i] & 0xfu];
    }
    hex[SOFT_HUKS_KEYID_LEN * 2] = '\0';
    (void)snprintf(path, pathLen, "%s/%s.key", gStore.dir, hex);
}

int32_t soft_store_put(const uint8_t keyId[SOFT_HUKS_KEYID_LEN], const uint8_t *plain, uint32_t plainLen)
{
    int32_t ret;
    uint8_t *sealed = NULL;
    uint32_t sealedLen = 0;
    char path[PATH_MAX_LEN];
    char tmpPath[PATH_MAX_LEN];
    int fd;

    ret = soft_store_ensure_ready();
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    ret = seal_blob(plain, plainLen, &sealed, &sealedLen);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    key_path(keyId, path, sizeof(path));
    (void)snprintf(tmpPath, sizeof(tmpPath), "%s.tmp.%d", path, (int)getpid());

    fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        free(sealed);
        return HKS_ERROR_OPEN_FILE_FAIL;
    }
    ret = write_full(fd, sealed, sealedLen);
    if (ret == HKS_SUCCESS) {
        (void)fsync(fd);
    }
    (void)close(fd);
    free(sealed);
    if (ret != HKS_SUCCESS) {
        (void)unlink(tmpPath);
        return ret;
    }
    if (rename(tmpPath, path) != 0) {
        (void)unlink(tmpPath);
        return HKS_ERROR_WRITE_FILE_FAIL;
    }
    return HKS_SUCCESS;
}

int32_t soft_store_exists(const uint8_t keyId[SOFT_HUKS_KEYID_LEN])
{
    char path[PATH_MAX_LEN];

    if (soft_store_ensure_ready() != HKS_SUCCESS) {
        return HKS_ERROR_NOT_EXIST;
    }
    key_path(keyId, path, sizeof(path));
    return (access(path, F_OK) == 0) ? HKS_SUCCESS : HKS_ERROR_NOT_EXIST;
}

int32_t soft_store_get(const uint8_t keyId[SOFT_HUKS_KEYID_LEN], uint8_t **plain, uint32_t *plainLen)
{
    char path[PATH_MAX_LEN];
    struct stat st;
    uint8_t *sealed = NULL;
    int fd;
    int32_t ret;

    if (plain == NULL || plainLen == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    ret = soft_store_ensure_ready();
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    key_path(keyId, path, sizeof(path));
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return (errno == ENOENT) ? HKS_ERROR_NOT_EXIST : HKS_ERROR_OPEN_FILE_FAIL;
    }
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 * 1024 * 1024)) {
        (void)close(fd);
        return HKS_ERROR_INVALID_KEY_FILE;
    }
    sealed = malloc((size_t)st.st_size);
    if (sealed == NULL) {
        (void)close(fd);
        return HKS_ERROR_MALLOC_FAIL;
    }
    ret = read_full(fd, sealed, (size_t)st.st_size);
    (void)close(fd);
    if (ret != HKS_SUCCESS) {
        free(sealed);
        return ret;
    }
    ret = open_blob(sealed, (uint32_t)st.st_size, plain, plainLen);
    (void)OPENSSL_cleanse(sealed, (size_t)st.st_size);
    free(sealed);
    return ret;
}

void soft_make_token(const uint8_t keyId[SOFT_HUKS_KEYID_LEN], uint8_t token[SOFT_HUKS_TOKEN_LEN])
{
    (void)memcpy(token, "SHK1", 4);
    (void)memcpy(token + 4, keyId, SOFT_HUKS_KEYID_LEN);
}

bool soft_token_keyid(const uint8_t *data, uint32_t len, uint8_t keyId[SOFT_HUKS_KEYID_LEN])
{
    if (data == NULL || len != SOFT_HUKS_TOKEN_LEN || memcmp(data, "SHK1", 4) != 0) {
        return false;
    }
    (void)memcpy(keyId, data + 4, SOFT_HUKS_KEYID_LEN);
    return true;
}
