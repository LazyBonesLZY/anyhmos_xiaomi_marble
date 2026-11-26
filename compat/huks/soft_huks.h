/*
 * Software HUKS backend for Xiaomi marble (SM7475, no usable TEE TA).
 *
 * This header re-declares the HUKS / HUKS-HDI ABI that the shipped
 * libhuks_service.z.so and the HDI passthrough adapter use.  The layouts are
 * copied from:
 *   work/upstream/security_huks/interfaces/inner_api/huks_standard/main/include/hks_type.h
 *   work/upstream/security_huks/interfaces/inner_api/huks_standard/main/include/hks_tag.h
 *   work/upstream/security_huks/interfaces/inner_api/huks_standard/main/include/hks_type_enum.h
 *   work/upstream/drivers_peripheral/huks/hdi_service/huks_sa_hdi_struct.h  (struct HuksHdi)
 *   work/upstream/drivers_interface/huks/v1_1/IHuks.idl                    (struct IHuks, v1.1)
 *
 * It is a *software* key store.  It does not provide hardware isolation.
 */

#ifndef SOFT_HUKS_H
#define SOFT_HUKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOFT_HUKS_EXPORT __attribute__((visibility("default")))

/* ------------------------------------------------------------------ */
/* HUKS core types (ABI: hks_type.h)                                   */
/* ------------------------------------------------------------------ */

struct HksBlob {
    uint32_t size;
    uint8_t *data;
};

struct HksParam {
    uint32_t tag;
    union {
        bool boolParam;
        int32_t int32Param;
        uint32_t uint32Param;
        uint64_t uint64Param;
        struct HksBlob blob;
    };
};

struct HksParamSet {
    uint32_t paramSetSize;
    uint32_t paramsCnt;
    struct HksParam params[];
};

enum HksChipsetPlatformDecryptScene {
    HKS_CHIPSET_PLATFORM_DECRYPT_SCENE_TA_TO_TA = 1,
};

/* ------------------------------------------------------------------ */
/* Error codes (subset of hks_error_code.h; same numeric values)       */
/* ------------------------------------------------------------------ */

enum SoftHksErrorCode {
    HKS_SUCCESS = 0,
    HKS_FAILURE = -1,
    HKS_ERROR_BAD_STATE = -2,
    HKS_ERROR_INVALID_ARGUMENT = -3,
    HKS_ERROR_NOT_SUPPORTED = -4,
    HKS_ERROR_NO_PERMISSION = -5,
    HKS_ERROR_INSUFFICIENT_DATA = -6,
    HKS_ERROR_BUFFER_TOO_SMALL = -7,
    HKS_ERROR_INSUFFICIENT_MEMORY = -8,
    HKS_ERROR_STORAGE_FAILURE = -10,
    HKS_ERROR_ALREADY_EXISTS = -12,
    HKS_ERROR_NOT_EXIST = -13,
    HKS_ERROR_NULL_POINTER = -14,
    HKS_ERROR_READ_FILE_FAIL = -16,
    HKS_ERROR_INVALID_ARGUMENT_KEY = -19, /* == HKS_ERROR_INVALID_KEY_INFO */
    HKS_ERROR_INVALID_PRIVATE_KEY = -18,
    HKS_ERROR_SESSION_REACHED_LIMIT = -39,
    HKS_ERROR_MALLOC_FAIL = -21,
    HKS_ERROR_WRITE_FILE_FAIL = -22,
    HKS_ERROR_OPEN_FILE_FAIL = -24,
    HKS_ERROR_MAKE_DIR_FAIL = -26,
    HKS_ERROR_INVALID_KEY_FILE = -27,
    HKS_ERROR_PARAM_NOT_EXIST = -30,
    HKS_ERROR_CRYPTO_ENGINE_ERROR = -31,
    HKS_ERROR_VERIFICATION_FAILED = -38,
    HKS_ERROR_API_NOT_SUPPORTED = -45,
    HKS_ERROR_INVALID_ALGORITHM = -112,
    HKS_ERROR_INVALID_KEY_SIZE = -113,
    HKS_ERROR_INVALID_PURPOSE = -115,
    HKS_ERROR_INVALID_MODE = -116,
    HKS_ERROR_INVALID_DIGEST = -117,
    HKS_ERROR_INVALID_IV = -119,
    HKS_ERROR_INVALID_NONCE = -121,
    HKS_ERROR_INVALID_AE_TAG = -122,
    HKS_ERROR_INVALID_SALT = -123,
    HKS_ERROR_INVALID_OPERATION = -125,
    HKS_ERROR_INVALID_USAGE_OF_KEY = -127,
};

/* ------------------------------------------------------------------ */
/* Tags (subset of hks_tag.h; exact numeric values)                    */
/* ------------------------------------------------------------------ */

#define HKS_TAG_TYPE_INT   (1u << 28)
#define HKS_TAG_TYPE_UINT  (2u << 28)
#define HKS_TAG_TYPE_BOOL  (4u << 28)
#define HKS_TAG_TYPE_BYTES (5u << 28)

enum SoftHksTag {
    HKS_TAG_ALGORITHM = HKS_TAG_TYPE_UINT | 1,
    HKS_TAG_PURPOSE = HKS_TAG_TYPE_UINT | 2,
    HKS_TAG_KEY_SIZE = HKS_TAG_TYPE_UINT | 3,
    HKS_TAG_DIGEST = HKS_TAG_TYPE_UINT | 4,
    HKS_TAG_PADDING = HKS_TAG_TYPE_UINT | 5,
    HKS_TAG_BLOCK_MODE = HKS_TAG_TYPE_UINT | 6,
    HKS_TAG_ASSOCIATED_DATA = HKS_TAG_TYPE_BYTES | 8,
    HKS_TAG_NONCE = HKS_TAG_TYPE_BYTES | 9,
    HKS_TAG_IV = HKS_TAG_TYPE_BYTES | 10,
    HKS_TAG_INFO = HKS_TAG_TYPE_BYTES | 11,
    HKS_TAG_SALT = HKS_TAG_TYPE_BYTES | 12,
    HKS_TAG_ITERATION = HKS_TAG_TYPE_UINT | 14,
    HKS_TAG_KEY_ALIAS = HKS_TAG_TYPE_BYTES | 23,
    HKS_TAG_DERIVE_KEY_SIZE = HKS_TAG_TYPE_UINT | 24,
    HKS_TAG_DERIVE_AGREE_KEY_STORAGE_FLAG = HKS_TAG_TYPE_UINT | 29,
    HKS_TAG_IS_KEY_ALIAS = HKS_TAG_TYPE_BOOL | 1001,
    HKS_TAG_KEY_STORAGE_FLAG = HKS_TAG_TYPE_UINT | 1002,
    HKS_TAG_KEY_OVERRIDE = HKS_TAG_TYPE_BOOL | 520,
    HKS_TAG_KEY_FLAG = HKS_TAG_TYPE_UINT | 1007,
    HKS_TAG_KEY = HKS_TAG_TYPE_BYTES | 10006,
    HKS_TAG_KEY_VERSION = HKS_TAG_TYPE_UINT | 10007,
    HKS_TAG_OS_VERSION = HKS_TAG_TYPE_UINT | 10101,
    HKS_TAG_OS_PATCHLEVEL = HKS_TAG_TYPE_UINT | 10102,
    HKS_TAG_AE_TAG = HKS_TAG_TYPE_BYTES | 10009,
};

#define SOFT_HUKS_KEY_VERSION 3u

enum SoftHksKeyFlag {
    HKS_KEY_FLAG_IMPORT_KEY = 1,
    HKS_KEY_FLAG_GENERATE_KEY = 2,
    HKS_KEY_FLAG_AGREE_KEY = 3,
    HKS_KEY_FLAG_DERIVE_KEY = 4,
};

enum SoftHksKeyStorageType {
    HKS_STORAGE_TEMP = 0,
    HKS_STORAGE_PERSISTENT = 1,
    HKS_STORAGE_ONLY_USED_IN_HUKS = 2,
    HKS_STORAGE_ALLOW_KEY_EXPORTED = 3,
};

struct KeyMaterialRsa {
    uint32_t keyAlg;
    uint32_t keySize;
    uint32_t nSize;
    uint32_t eSize;
    uint32_t dSize;
};

struct KeyMaterialEcc {
    uint32_t keyAlg;
    uint32_t keySize;
    uint32_t xSize;
    uint32_t ySize;
    uint32_t zSize;
};

_Static_assert(sizeof(struct KeyMaterialRsa) == 20, "KeyMaterialRsa ABI");
_Static_assert(sizeof(struct KeyMaterialEcc) == 20, "KeyMaterialEcc ABI");

/*
 * Local routing extension, NOT an upstream HUKS tag.  Real HUKS clients never
 * emit it; it exists so that a caller (or a future kernel-side bridge) can pin
 * a request to a backend without guessing.  Values: enum SoftKeyOrigin.
 */
#define SOFT_HUKS_TAG_KEY_ORIGIN (HKS_TAG_TYPE_UINT | 0xFF01u)

enum SoftKeyOrigin {
    SOFT_KEY_ORIGIN_GENERATED = 0,
    SOFT_KEY_ORIGIN_IMPORTED = 1,
    SOFT_KEY_ORIGIN_HUAWEI_EXT = 2, /* HMS / vendor-proprietary semantics */
};

/* ------------------------------------------------------------------ */
/* Algorithms, purposes, digests, modes, paddings                      */
/* ------------------------------------------------------------------ */

enum SoftHksAlg {
    HKS_ALG_RSA = 1,
    HKS_ALG_ECC = 2,
    HKS_ALG_AES = 20,
    HKS_ALG_HMAC = 50,
    HKS_ALG_HKDF = 51,
    HKS_ALG_PBKDF2 = 52,
    HKS_ALG_SM2 = 150,
    HKS_ALG_SM4 = 152,
};

enum SoftHksPurpose {
    HKS_KEY_PURPOSE_ENCRYPT = 1,
    HKS_KEY_PURPOSE_DECRYPT = 2,
    HKS_KEY_PURPOSE_SIGN = 4,
    HKS_KEY_PURPOSE_VERIFY = 8,
    HKS_KEY_PURPOSE_DERIVE = 16,
    HKS_KEY_PURPOSE_WRAP = 32,
    HKS_KEY_PURPOSE_UNWRAP = 64,
    HKS_KEY_PURPOSE_MAC = 128,
    HKS_KEY_PURPOSE_AGREE = 256,
};

enum SoftHksDigest {
    HKS_DIGEST_NONE = 0,
    HKS_DIGEST_SHA1 = 10,
    HKS_DIGEST_SHA224 = 11,
    HKS_DIGEST_SHA256 = 12,
    HKS_DIGEST_SHA384 = 13,
    HKS_DIGEST_SHA512 = 14,
};

enum SoftHksMode {
    HKS_MODE_ECB = 1,
    HKS_MODE_CBC = 2,
    HKS_MODE_CTR = 3,
    HKS_MODE_GCM = 32,
};

enum SoftHksPadding {
    HKS_PADDING_NONE = 0,
    HKS_PADDING_OAEP = 1,
    HKS_PADDING_PSS = 2,
    HKS_PADDING_PKCS1_V1_5 = 3,
    HKS_PADDING_PKCS5 = 4,
    HKS_PADDING_PKCS7 = 5,
};

/* ------------------------------------------------------------------ */
/* struct HuksHdi: the engine vtable (huks_sa_hdi_struct.h)            */
/*                                                                     */
/* The HDI passthrough adapter (drivers_peripheral/huks/hdi_service)   */
/* takes this table from libhuks_engine_core_standard.z.so via         */
/* HuksCreateHdiDevicePtr().  Field order is ABI.                      */
/* ------------------------------------------------------------------ */

struct HuksHdi {
    int32_t (*HuksHdiModuleInit)(void);
    int32_t (*HuksHdiModuleDestroy)(void);
    int32_t (*HuksHdiRefresh)(void);
    int32_t (*HuksHdiGenerateKey)(const struct HksBlob *keyAlias, const struct HksParamSet *paramSet,
        const struct HksBlob *keyIn, struct HksBlob *keyOut);
    int32_t (*HuksHdiImportKey)(const struct HksBlob *keyAlias, const struct HksBlob *key,
        const struct HksParamSet *paramSet, struct HksBlob *keyOut);
    int32_t (*HuksHdiImportWrappedKey)(const struct HksBlob *wrappingKeyAlias, const struct HksBlob *key,
        const struct HksBlob *wrappedKeyData, const struct HksParamSet *paramSet, struct HksBlob *keyOut);
    int32_t (*HuksHdiExportPublicKey)(const struct HksBlob *key, const struct HksParamSet *paramSet,
        struct HksBlob *keyOut);
    int32_t (*HuksHdiInit)(const struct HksBlob *key, const struct HksParamSet *paramSet, struct HksBlob *handle,
        struct HksBlob *token);
    int32_t (*HuksHdiUpdate)(const struct HksBlob *handle, const struct HksParamSet *paramSet,
        const struct HksBlob *inData, struct HksBlob *outData);
    int32_t (*HuksHdiFinish)(const struct HksBlob *handle, const struct HksParamSet *paramSet,
        const struct HksBlob *inData, struct HksBlob *outData);
    int32_t (*HuksHdiAbort)(const struct HksBlob *handle, const struct HksParamSet *paramSet);
    int32_t (*HuksHdiGetKeyProperties)(const struct HksParamSet *paramSet, const struct HksBlob *key);
    int32_t (*HuksHdiAttestKey)(const struct HksBlob *key, const struct HksParamSet *paramSet,
        struct HksBlob *certChain);
    int32_t (*HuksHdiGetAbility)(int32_t funcType);
    int32_t (*HuksHdiGetHardwareInfo)(void);
    int32_t (*HuksHdiCalcMacHeader)(const struct HksParamSet *paramSet, const struct HksBlob *salt,
        const struct HksBlob *srcData, struct HksBlob *mac);
    int32_t (*HuksHdiUpgradeKeyInfo)(const struct HksBlob *keyAlias, const struct HksBlob *keyInfo,
        struct HksBlob *keyOut);
    int32_t (*HuksHdiGenerateRandom)(const struct HksParamSet *paramSet, struct HksBlob *random);
    int32_t (*HuksHdiSign)(const struct HksBlob *key, const struct HksParamSet *paramSet,
        const struct HksBlob *srcData, struct HksBlob *signature);
    int32_t (*HuksHdiVerify)(const struct HksBlob *key, const struct HksParamSet *paramSet,
        const struct HksBlob *srcData, const struct HksBlob *signature);
    int32_t (*HuksHdiEncrypt)(const struct HksBlob *key, const struct HksParamSet *paramSet,
        const struct HksBlob *plainText, struct HksBlob *cipherText);
    int32_t (*HuksHdiDecrypt)(const struct HksBlob *key, const struct HksParamSet *paramSet,
        const struct HksBlob *cipherText, struct HksBlob *plainText);
    int32_t (*HuksHdiAgreeKey)(const struct HksParamSet *paramSet, const struct HksBlob *privateKey,
        const struct HksBlob *peerPublicKey, struct HksBlob *agreedKey);
    int32_t (*HuksHdiDeriveKey)(const struct HksParamSet *paramSet, const struct HksBlob *kdfKey,
        struct HksBlob *derivedKey);
    int32_t (*HuksHdiMac)(const struct HksBlob *key, const struct HksParamSet *paramSet,
        const struct HksBlob *srcData, struct HksBlob *mac);
    int32_t (*HuksHdiUpgradeKey)(const struct HksBlob *oldKey, const struct HksParamSet *paramSet,
        struct HksBlob *newKey);
    int32_t (*HuksHdiExportChipsetPlatformPublicKey)(const struct HksBlob *salt,
        enum HksChipsetPlatformDecryptScene scene, struct HksBlob *publicKey);
    int32_t (*HuksHdiGetErrorInfo)(struct HksBlob *errorInfo);
    int32_t (*HuksHdiGetStatInfo)(struct HksBlob *statInfo);
};

#define SOFT_HUKS_ENGINE_SLOTS 29

/* ------------------------------------------------------------------ */
/* HUKS HDI IDL types and the struct IHuks vtable (v1.1)               */
/*                                                                     */
/* Generated by the IDL compiler from drivers_interface/huks/v1_0+1_1. */
/* The shipped libhuks_service.z.so calls these slots directly (see     */
/* security_huks/.../idl/passthrough/wrapper/wrapper_v1_1.c), so the    */
/* order below is ABI.                                                  */
/* ------------------------------------------------------------------ */

struct HuksBlob {
    uint8_t *data;
    uint32_t dataLen;
};

struct HuksParamSet {
    uint8_t *data;
    uint32_t dataLen;
};

struct IHuks {
    int32_t (*ModuleInit)(struct IHuks *self);
    int32_t (*ModuleDestroy)(struct IHuks *self);
    int32_t (*GenerateKey)(struct IHuks *self, const struct HuksBlob *keyAlias,
        const struct HuksParamSet *paramSet, const struct HuksBlob *keyIn, struct HuksBlob *encKeyOut);
    int32_t (*ImportKey)(struct IHuks *self, const struct HuksBlob *keyAlias, const struct HuksBlob *key,
        const struct HuksParamSet *paramSet, struct HuksBlob *encKeyOut);
    int32_t (*ImportWrappedKey)(struct IHuks *self, const struct HuksBlob *wrappingKeyAlias,
        const struct HuksBlob *wrappingEncKey, const struct HuksBlob *wrappedKeyData,
        const struct HuksParamSet *paramSet, struct HuksBlob *encKeyOut);
    int32_t (*ExportPublicKey)(struct IHuks *self, const struct HuksBlob *encKey,
        const struct HuksParamSet *paramSet, struct HuksBlob *keyOut);
    int32_t (*Init)(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
        struct HuksBlob *handle, struct HuksBlob *token);
    int32_t (*Update)(struct IHuks *self, const struct HuksBlob *handle, const struct HuksParamSet *paramSet,
        const struct HuksBlob *inData, struct HuksBlob *outData);
    int32_t (*Finish)(struct IHuks *self, const struct HuksBlob *handle, const struct HuksParamSet *paramSet,
        const struct HuksBlob *inData, struct HuksBlob *outData);
    int32_t (*Abort)(struct IHuks *self, const struct HuksBlob *handle, const struct HuksParamSet *paramSet);
    int32_t (*CheckKeyValidity)(struct IHuks *self, const struct HuksParamSet *paramSet,
        const struct HuksBlob *encKey);
    int32_t (*AttestKey)(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
        struct HuksBlob *certChain);
    int32_t (*GenerateRandom)(struct IHuks *self, const struct HuksParamSet *paramSet, struct HuksBlob *random);
    int32_t (*Sign)(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
        const struct HuksBlob *srcData, struct HuksBlob *signature);
    int32_t (*Verify)(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
        const struct HuksBlob *srcData, const struct HuksBlob *signature);
    int32_t (*Encrypt)(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
        const struct HuksBlob *plainText, struct HuksBlob *cipherText);
    int32_t (*Decrypt)(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
        const struct HuksBlob *cipherText, struct HuksBlob *plainText);
    int32_t (*AgreeKey)(struct IHuks *self, const struct HuksParamSet *paramSet, const struct HuksBlob *encPrivateKey,
        const struct HuksBlob *peerPublicKey, struct HuksBlob *agreedKey);
    int32_t (*DeriveKey)(struct IHuks *self, const struct HuksParamSet *paramSet, const struct HuksBlob *encKdfKey,
        struct HuksBlob *derivedKey);
    int32_t (*Mac)(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
        const struct HuksBlob *srcData, struct HuksBlob *mac);
    int32_t (*UpgradeKey)(struct IHuks *self, const struct HuksBlob *encOldKey, const struct HuksParamSet *paramSet,
        struct HuksBlob *encNewKey);
    int32_t (*ExportChipsetPlatformPublicKey)(struct IHuks *self, const struct HuksBlob *salt,
        enum HksChipsetPlatformDecryptScene scene, struct HuksBlob *publicKey);
    int32_t (*GetErrorInfo)(struct IHuks *self, struct HuksBlob *errorInfo);
    int32_t (*GetStatInfo)(struct IHuks *self, struct HuksBlob *statInfo);
    int32_t (*GetVersion)(struct IHuks *self, uint32_t *majorVer, uint32_t *minorVer);
};

#define SOFT_HUKS_IHUKS_SLOTS 25
#define SOFT_HUKS_IHUKS_VERSION_MAJOR 1u
#define SOFT_HUKS_IHUKS_VERSION_MINOR 1u

/* ------------------------------------------------------------------ */
/* Backend routing                                                     */
/* ------------------------------------------------------------------ */

enum SoftBackend {
    SOFT_BACKEND_SOFT = 0, /* OpenSSL + sealed file store (always available) */
    SOFT_BACKEND_QSEE = 1, /* Qualcomm TEE via /dev/qseecom + GPTEE (skeleton) */
    SOFT_BACKEND_STUB = 2, /* explicit "not provided by this port" */
};

const char *soft_backend_name(enum SoftBackend backend);

/* Probe result of the Qualcomm TEE path; never crashes, never throws. */
enum SoftBackend soft_probe_qsee(char *detail, size_t detailLen);

/* One routing decision + one "HUKS_BACKEND=..." log line. */
enum SoftBackend soft_route(const char *op, uint32_t alg, uint32_t origin, const char **reason);

/* Fail-closed executors for the non-soft tiers. */
int32_t soft_qsee_execute(const char *op);
int32_t soft_stub_execute(const char *op);

/* Logging (stderr + libhilog when present). */
void soft_huks_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ------------------------------------------------------------------ */
/* Exported entry points                                               */
/* ------------------------------------------------------------------ */

/* libhuks_engine_core_standard.z.so */
SOFT_HUKS_EXPORT struct HuksHdi *HuksCreateHdiDevicePtr(void);
SOFT_HUKS_EXPORT void HuksDestoryHdiDevicePtr(struct HuksHdi *hdiDevicePtr);

/* libhuks_hdi_service_1.1.z.so */
SOFT_HUKS_EXPORT struct IHuks *HuksImplGetInstance(void);
SOFT_HUKS_EXPORT void HuksImplRelease(struct IHuks *instance);

/* Store helpers shared by engine and tests. */
#define SOFT_HUKS_TOKEN_LEN 20
#define SOFT_HUKS_KEYID_LEN 16

int32_t soft_store_put(const uint8_t keyId[SOFT_HUKS_KEYID_LEN], const uint8_t *plain, uint32_t plainLen);
int32_t soft_store_get(const uint8_t keyId[SOFT_HUKS_KEYID_LEN], uint8_t **plain, uint32_t *plainLen);
int32_t soft_store_exists(const uint8_t keyId[SOFT_HUKS_KEYID_LEN]);
int32_t soft_store_ensure_ready(void);
void soft_make_token(const uint8_t keyId[SOFT_HUKS_KEYID_LEN], uint8_t token[SOFT_HUKS_TOKEN_LEN]);
bool soft_token_keyid(const uint8_t *data, uint32_t len, uint8_t keyId[SOFT_HUKS_KEYID_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* SOFT_HUKS_H */
