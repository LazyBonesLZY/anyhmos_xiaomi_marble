/*
 * HDI service shim: `struct IHuks` (v1.1) over the software engine.
 *
 * This is the layer the shipped libhuks_service.z.so talks to through
 * libhuks_proxy_1.1.z.so -> LoadHdiImpl() -> dlopen(impl) ->
 * dlsym("HuksImplGetInstance").  Upstream equivalent:
 * work/upstream/drivers_peripheral/huks/hdi_service/huks_hdi_service.c
 * (adapter + IDL service).
 *
 * The IDL blob layouts come from huks_hdi_template.h:
 *   struct HuksBlob     { uint32_t dataLen; uint8_t *data; }
 *   struct HuksParamSet { uint32_t dataLen; uint8_t *data; }
 * and the paramSet payload is `struct HksParamSet` produced by libhukssdk.
 */

#include "soft_huks.h"

#include <stdlib.h>
#include <string.h>

static struct HuksHdi *soft_engine(void)
{
    static struct HuksHdi *engine = NULL;
    if (engine == NULL) {
        engine = HuksCreateHdiDevicePtr();
    }
    return engine;
}

static struct HksBlob in_blob(const struct HuksBlob *blob)
{
    struct HksBlob out = { 0, NULL };
    if (blob != NULL) {
        out.size = blob->dataLen;
        out.data = blob->data;
    }
    return out;
}

static const struct HksParamSet *in_param_set(const struct HuksParamSet *paramSet)
{
    if (paramSet == NULL || paramSet->data == NULL || paramSet->dataLen < sizeof(struct HksParamSet)) {
        return NULL;
    }
    return (const struct HksParamSet *)paramSet->data;
}

/* Hand an output blob back through the IDL struct, keeping caller buffers. */
static struct HksBlob out_blob_init(const struct HuksBlob *dst)
{
    struct HksBlob out = { 0, NULL };
    if (dst != NULL && dst->data != NULL) {
        out.size = dst->dataLen;
        out.data = dst->data;
    }
    return out;
}

static void out_blob_finish(const struct HksBlob *src, struct HuksBlob *dst)
{
    if (dst == NULL) {
        return;
    }
    dst->data = src->data;
    dst->dataLen = src->size;
}

#define SHIM_BEGIN(name) \
    struct HuksHdi *e = soft_engine(); \
    if (e == NULL) { \
        return HKS_ERROR_NULL_POINTER; \
    }

static int32_t shim_module_init(struct IHuks *self)
{
    (void)self;
    SHIM_BEGIN(ModuleInit)
    return e->HuksHdiModuleInit();
}

static int32_t shim_module_destroy(struct IHuks *self)
{
    (void)self;
    SHIM_BEGIN(ModuleDestroy)
    return e->HuksHdiModuleDestroy();
}

static int32_t shim_generate_key(struct IHuks *self, const struct HuksBlob *keyAlias,
    const struct HuksParamSet *paramSet, const struct HuksBlob *keyIn, struct HuksBlob *encKeyOut)
{
    struct HksBlob alias = in_blob(keyAlias);
    struct HksBlob key = in_blob(keyIn);
    struct HksBlob out = out_blob_init(encKeyOut);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(GenerateKey)
    ret = e->HuksHdiGenerateKey(&alias, in_param_set(paramSet), &key, &out);
    out_blob_finish(&out, encKeyOut);
    return ret;
}

static int32_t shim_import_key(struct IHuks *self, const struct HuksBlob *keyAlias, const struct HuksBlob *key,
    const struct HuksParamSet *paramSet, struct HuksBlob *encKeyOut)
{
    struct HksBlob alias = in_blob(keyAlias);
    struct HksBlob material = in_blob(key);
    struct HksBlob out = out_blob_init(encKeyOut);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(ImportKey)
    ret = e->HuksHdiImportKey(&alias, &material, in_param_set(paramSet), &out);
    out_blob_finish(&out, encKeyOut);
    return ret;
}

static int32_t shim_import_wrapped_key(struct IHuks *self, const struct HuksBlob *wrappingKeyAlias,
    const struct HuksBlob *wrappingEncKey, const struct HuksBlob *wrappedKeyData,
    const struct HuksParamSet *paramSet, struct HuksBlob *encKeyOut)
{
    struct HksBlob alias = in_blob(wrappingKeyAlias);
    struct HksBlob wrapping = in_blob(wrappingEncKey);
    struct HksBlob wrapped = in_blob(wrappedKeyData);
    struct HksBlob out = out_blob_init(encKeyOut);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(ImportWrappedKey)
    ret = e->HuksHdiImportWrappedKey(&alias, &wrapping, &wrapped, in_param_set(paramSet), &out);
    out_blob_finish(&out, encKeyOut);
    return ret;
}

static int32_t shim_export_public_key(struct IHuks *self, const struct HuksBlob *encKey,
    const struct HuksParamSet *paramSet, struct HuksBlob *keyOut)
{
    struct HksBlob key = in_blob(encKey);
    struct HksBlob out = out_blob_init(keyOut);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(ExportPublicKey)
    ret = e->HuksHdiExportPublicKey(&key, in_param_set(paramSet), &out);
    out_blob_finish(&out, keyOut);
    return ret;
}

static int32_t shim_init(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
    struct HuksBlob *handle, struct HuksBlob *token)
{
    struct HksBlob key = in_blob(encKey);
    struct HksBlob h = out_blob_init(handle);
    struct HksBlob t = out_blob_init(token);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(Init)
    ret = e->HuksHdiInit(&key, in_param_set(paramSet), &h, &t);
    out_blob_finish(&h, handle);
    out_blob_finish(&t, token);
    return ret;
}

static int32_t shim_update(struct IHuks *self, const struct HuksBlob *handle, const struct HuksParamSet *paramSet,
    const struct HuksBlob *inData, struct HuksBlob *outData)
{
    struct HksBlob h = in_blob(handle);
    struct HksBlob in = in_blob(inData);
    struct HksBlob out = out_blob_init(outData);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(Update)
    ret = e->HuksHdiUpdate(&h, in_param_set(paramSet), &in, &out);
    out_blob_finish(&out, outData);
    return ret;
}

static int32_t shim_finish(struct IHuks *self, const struct HuksBlob *handle, const struct HuksParamSet *paramSet,
    const struct HuksBlob *inData, struct HuksBlob *outData)
{
    struct HksBlob h = in_blob(handle);
    struct HksBlob in = in_blob(inData);
    struct HksBlob out = out_blob_init(outData);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(Finish)
    ret = e->HuksHdiFinish(&h, in_param_set(paramSet), &in, &out);
    out_blob_finish(&out, outData);
    return ret;
}

static int32_t shim_abort(struct IHuks *self, const struct HuksBlob *handle, const struct HuksParamSet *paramSet)
{
    struct HksBlob h = in_blob(handle);

    (void)self;
    SHIM_BEGIN(Abort)
    return e->HuksHdiAbort(&h, in_param_set(paramSet));
}

static int32_t shim_check_key_validity(struct IHuks *self, const struct HuksParamSet *paramSet,
    const struct HuksBlob *encKey)
{
    struct HksBlob key = in_blob(encKey);

    (void)self;
    SHIM_BEGIN(CheckKeyValidity)
    return e->HuksHdiGetKeyProperties(in_param_set(paramSet), &key);
}

static int32_t shim_attest_key(struct IHuks *self, const struct HuksBlob *encKey,
    const struct HuksParamSet *paramSet, struct HuksBlob *certChain)
{
    struct HksBlob key = in_blob(encKey);
    struct HksBlob out = out_blob_init(certChain);
    uint8_t *synthesized = NULL;
    int32_t ret;

    (void)self;
    SHIM_BEGIN(AttestKey)
    /* HUKS may pass a null certChain to the HDI. DcmAnonymousAttestKey()
     * rejects that locally (DcmCheckBlob -10001) before any IPC, so the local
     * DCM service never sees the request. Hand back a small zeroed
     * HksCertChain carrier for DCM to fill; HUKS owns it after this call. */
    soft_huks_log("HUKS_SHIM op=AttestKey in_data=%d in_len=%u synth=%d",
        out.data != NULL ? 1 : 0, (unsigned)out.size,
        (out.data == NULL || out.size == 0) ? 1 : 0);
    if (out.data == NULL || out.size == 0) {
        synthesized = calloc(1, 64);
        if (synthesized == NULL) {
            return HKS_ERROR_NULL_POINTER;
        }
        out.data = synthesized;
        out.size = 64;
    }
    ret = e->HuksHdiAttestKey(&key, in_param_set(paramSet), &out);
    soft_huks_log("HUKS_SHIM op=AttestKey engine_ret=%d out_data=%d out_len=%u",
        ret, out.data != NULL ? 1 : 0, (unsigned)out.size);
    if (ret == HKS_SUCCESS && synthesized != NULL) {
        if (certChain != NULL) {
            certChain->data = synthesized;
            certChain->dataLen = 64;
        } else {
            free(synthesized);
        }
    } else {
        if (synthesized != NULL) {
            free(synthesized);
        } else {
            out_blob_finish(&out, certChain);
        }
    }
    return ret;
}

static int32_t shim_generate_random(struct IHuks *self, const struct HuksParamSet *paramSet, struct HuksBlob *random)
{
    struct HksBlob out = out_blob_init(random);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(GenerateRandom)
    ret = e->HuksHdiGenerateRandom(in_param_set(paramSet), &out);
    out_blob_finish(&out, random);
    return ret;
}

static int32_t shim_sign(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
    const struct HuksBlob *srcData, struct HuksBlob *signature)
{
    struct HksBlob key = in_blob(encKey);
    struct HksBlob data = in_blob(srcData);
    struct HksBlob out = out_blob_init(signature);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(Sign)
    ret = e->HuksHdiSign(&key, in_param_set(paramSet), &data, &out);
    out_blob_finish(&out, signature);
    return ret;
}

static int32_t shim_verify(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
    const struct HuksBlob *srcData, const struct HuksBlob *signature)
{
    struct HksBlob key = in_blob(encKey);
    struct HksBlob data = in_blob(srcData);
    struct HksBlob sig = in_blob(signature);

    (void)self;
    SHIM_BEGIN(Verify)
    return e->HuksHdiVerify(&key, in_param_set(paramSet), &data, &sig);
}

static int32_t shim_encrypt(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
    const struct HuksBlob *plainText, struct HuksBlob *cipherText)
{
    struct HksBlob key = in_blob(encKey);
    struct HksBlob in = in_blob(plainText);
    struct HksBlob out = out_blob_init(cipherText);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(Encrypt)
    ret = e->HuksHdiEncrypt(&key, in_param_set(paramSet), &in, &out);
    out_blob_finish(&out, cipherText);
    return ret;
}

static int32_t shim_decrypt(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
    const struct HuksBlob *cipherText, struct HuksBlob *plainText)
{
    struct HksBlob key = in_blob(encKey);
    struct HksBlob in = in_blob(cipherText);
    struct HksBlob out = out_blob_init(plainText);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(Decrypt)
    ret = e->HuksHdiDecrypt(&key, in_param_set(paramSet), &in, &out);
    out_blob_finish(&out, plainText);
    return ret;
}

static int32_t shim_agree_key(struct IHuks *self, const struct HuksParamSet *paramSet,
    const struct HuksBlob *encPrivateKey, const struct HuksBlob *peerPublicKey, struct HuksBlob *agreedKey)
{
    struct HksBlob priv = in_blob(encPrivateKey);
    struct HksBlob peer = in_blob(peerPublicKey);
    struct HksBlob out = out_blob_init(agreedKey);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(AgreeKey)
    ret = e->HuksHdiAgreeKey(in_param_set(paramSet), &priv, &peer, &out);
    out_blob_finish(&out, agreedKey);
    return ret;
}

static int32_t shim_derive_key(struct IHuks *self, const struct HuksParamSet *paramSet,
    const struct HuksBlob *encKdfKey, struct HuksBlob *derivedKey)
{
    struct HksBlob key = in_blob(encKdfKey);
    struct HksBlob out = out_blob_init(derivedKey);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(DeriveKey)
    ret = e->HuksHdiDeriveKey(in_param_set(paramSet), &key, &out);
    out_blob_finish(&out, derivedKey);
    return ret;
}

static int32_t shim_mac(struct IHuks *self, const struct HuksBlob *encKey, const struct HuksParamSet *paramSet,
    const struct HuksBlob *srcData, struct HuksBlob *mac)
{
    struct HksBlob key = in_blob(encKey);
    struct HksBlob data = in_blob(srcData);
    struct HksBlob out = out_blob_init(mac);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(Mac)
    ret = e->HuksHdiMac(&key, in_param_set(paramSet), &data, &out);
    out_blob_finish(&out, mac);
    return ret;
}

static int32_t shim_upgrade_key(struct IHuks *self, const struct HuksBlob *encOldKey,
    const struct HuksParamSet *paramSet, struct HuksBlob *encNewKey)
{
    struct HksBlob oldKey = in_blob(encOldKey);
    struct HksBlob out = out_blob_init(encNewKey);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(UpgradeKey)
    ret = e->HuksHdiUpgradeKey(&oldKey, in_param_set(paramSet), &out);
    out_blob_finish(&out, encNewKey);
    return ret;
}

static int32_t shim_export_chipset_public_key(struct IHuks *self, const struct HuksBlob *salt,
    enum HksChipsetPlatformDecryptScene scene, struct HuksBlob *publicKey)
{
    struct HksBlob s = in_blob(salt);
    struct HksBlob out = out_blob_init(publicKey);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(ExportChipsetPlatformPublicKey)
    ret = e->HuksHdiExportChipsetPlatformPublicKey(&s, scene, &out);
    out_blob_finish(&out, publicKey);
    return ret;
}

static int32_t shim_get_error_info(struct IHuks *self, struct HuksBlob *errorInfo)
{
    struct HksBlob out = out_blob_init(errorInfo);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(GetErrorInfo)
    ret = e->HuksHdiGetErrorInfo(&out);
    out_blob_finish(&out, errorInfo);
    return ret;
}

static int32_t shim_get_stat_info(struct IHuks *self, struct HuksBlob *statInfo)
{
    struct HksBlob out = out_blob_init(statInfo);
    int32_t ret;

    (void)self;
    SHIM_BEGIN(GetStatInfo)
    ret = e->HuksHdiGetStatInfo(&out);
    out_blob_finish(&out, statInfo);
    return ret;
}

static int32_t shim_get_version(struct IHuks *self, uint32_t *majorVer, uint32_t *minorVer)
{
    (void)self;
    if (majorVer == NULL || minorVer == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    *majorVer = SOFT_HUKS_IHUKS_VERSION_MAJOR;
    *minorVer = SOFT_HUKS_IHUKS_VERSION_MINOR;
    return HKS_SUCCESS;
}

SOFT_HUKS_EXPORT struct IHuks *HuksImplGetInstance(void)
{
    struct IHuks *instance = malloc(sizeof(*instance));
    if (instance == NULL) {
        return NULL;
    }
    (void)memset(instance, 0, sizeof(*instance));
    instance->ModuleInit = shim_module_init;
    instance->ModuleDestroy = shim_module_destroy;
    instance->GenerateKey = shim_generate_key;
    instance->ImportKey = shim_import_key;
    instance->ImportWrappedKey = shim_import_wrapped_key;
    instance->ExportPublicKey = shim_export_public_key;
    instance->Init = shim_init;
    instance->Update = shim_update;
    instance->Finish = shim_finish;
    instance->Abort = shim_abort;
    instance->CheckKeyValidity = shim_check_key_validity;
    instance->AttestKey = shim_attest_key;
    instance->GenerateRandom = shim_generate_random;
    instance->Sign = shim_sign;
    instance->Verify = shim_verify;
    instance->Encrypt = shim_encrypt;
    instance->Decrypt = shim_decrypt;
    instance->AgreeKey = shim_agree_key;
    instance->DeriveKey = shim_derive_key;
    instance->Mac = shim_mac;
    instance->UpgradeKey = shim_upgrade_key;
    instance->ExportChipsetPlatformPublicKey = shim_export_chipset_public_key;
    instance->GetErrorInfo = shim_get_error_info;
    instance->GetStatInfo = shim_get_stat_info;
    instance->GetVersion = shim_get_version;
    soft_huks_log("HUKS_BACKEND=soft op=hdi_get_instance result=0 abi=IHuks.v1.1 slots=%d",
        (int)SOFT_HUKS_IHUKS_SLOTS);
    return instance;
}

SOFT_HUKS_EXPORT void HuksImplRelease(struct IHuks *instance)
{
    if (instance == NULL) {
        return;
    }
    (void)soft_engine()->HuksHdiModuleDestroy();
    free(instance);
}

_Static_assert(sizeof(struct IHuks) == SOFT_HUKS_IHUKS_SLOTS * sizeof(void *),
    "struct IHuks must match the v1.1 IDL method order");
_Static_assert(sizeof(struct HuksBlob) == 16, "IDL HuksBlob layout");
_Static_assert(offsetof(struct HuksBlob, data) == 0 && offsetof(struct HuksBlob, dataLen) == 8,
    "IDL HuksBlob field offsets");
_Static_assert(sizeof(struct HuksParamSet) == 16, "IDL HuksParamSet layout");
_Static_assert(offsetof(struct HuksParamSet, data) == 0 && offsetof(struct HuksParamSet, dataLen) == 8,
    "IDL HuksParamSet field offsets");
