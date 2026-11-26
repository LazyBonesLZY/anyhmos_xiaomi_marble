/*
 * Host self-test for the marble software HUKS backend.
 *
 * Exercises the two seams the device actually loads:
 *   1. engine vtable   HuksCreateHdiDevicePtr() -> struct HuksHdi
 *   2. HDI impl        HuksImplGetInstance()    -> struct IHuks (v1.1)
 *
 * Run:  HUKS_SOFT_ROOT=$(mktemp -d) soft_huks_selftest <engine.so> <hdi.so>
 */

#include "soft_huks.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int gFail = 0;
static int gPass = 0;

#define CHECK(cond, name) do { \
    if (cond) { gPass++; (void)printf("SELFTEST ok %s\n", name); } \
    else { gFail++; (void)printf("SELFTEST FAIL %s (line %d)\n", name, __LINE__); } \
} while (0)

struct PsBuf {
    uint8_t raw[sizeof(struct HksParamSet) + 24 * sizeof(struct HksParam)];
};

static struct HksParamSet *ps_begin(struct PsBuf *b)
{
    struct HksParamSet *ps = (struct HksParamSet *)b->raw;
    (void)memset(b->raw, 0, sizeof(b->raw));
    ps->paramSetSize = (uint32_t)sizeof(struct HksParamSet);
    ps->paramsCnt = 0;
    return ps;
}

static void ps_add_uint(struct HksParamSet *ps, uint32_t tag, uint32_t value)
{
    struct HksParam *p = &ps->params[ps->paramsCnt++];
    p->tag = tag;
    p->uint32Param = value;
    ps->paramSetSize += (uint32_t)sizeof(struct HksParam);
}

static void ps_add_blob(struct HksParamSet *ps, uint32_t tag, const uint8_t *data, uint32_t len)
{
    struct HksParam *p = &ps->params[ps->paramsCnt++];
    p->tag = tag;
    p->blob.size = len;
    p->blob.data = (uint8_t *)data;
    ps->paramSetSize += (uint32_t)sizeof(struct HksParam);
}

static struct HksBlob blob_of(const void *data, uint32_t len)
{
    struct HksBlob b;
    b.data = (uint8_t *)data;
    b.size = len;
    return b;
}

static struct HksBlob blob_copy(const struct HksBlob *src)
{
    struct HksBlob out;
    out.size = src->size;
    out.data = malloc(src->size > 0 ? src->size : 1u);
    if (out.data != NULL && src->size > 0) {
        (void)memcpy(out.data, src->data, src->size);
    }
    return out;
}

static void blob_free(struct HksBlob *b)
{
    free(b->data);
    b->data = NULL;
    b->size = 0;
}

static void check_key_blob_paramset(const uint8_t *buf, uint32_t len, uint32_t expectedFlag, const char *name)
{
    const struct HksParamSet *kps = (const struct HksParamSet *)buf;
    uint32_t hdrBytes;
    uint32_t off;
    int seenVer = 0;
    int seenFlag = 0;
    int seenKey = 0;
    int ok = 1;

    if (buf == NULL || len < sizeof(struct HksParamSet)) {
        CHECK(0, name);
        return;
    }
    if (kps->paramSetSize != len ||
        kps->paramsCnt > ((len - (uint32_t)sizeof(struct HksParamSet)) / (uint32_t)sizeof(struct HksParam))) {
        CHECK(0, name);
        return;
    }
    hdrBytes = (uint32_t)sizeof(struct HksParamSet) + kps->paramsCnt * (uint32_t)sizeof(struct HksParam);
    off = hdrBytes;
    for (uint32_t i = 0; i < kps->paramsCnt; i++) {
        const struct HksParam *p = &kps->params[i];
        if ((p->tag >> 28) == 5u) {
            if ((uintptr_t)p->blob.data != off || p->blob.size > len - off) {
                ok = 0;
                break;
            }
            off += p->blob.size;
        }
        if (p->tag == HKS_TAG_KEY_VERSION && p->uint32Param == SOFT_HUKS_KEY_VERSION) {
            seenVer = 1;
        } else if (p->tag == HKS_TAG_KEY_FLAG && p->uint32Param == expectedFlag) {
            seenFlag = 1;
        } else if (p->tag == HKS_TAG_KEY && p->blob.size == SOFT_HUKS_TOKEN_LEN &&
                   memcmp(buf + (uintptr_t)p->blob.data, "SHK1", 4) == 0) {
            seenKey = 1;
        }
    }
    CHECK(ok && off == len && seenVer && seenFlag && seenKey, name);
}

/* ------------------------------------------------------------------ */
/* Engine seam                                                         */
/* ------------------------------------------------------------------ */

static struct HuksHdi *load_engine(const char *path, void **handleOut)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    struct HuksHdi *(*create)(void);

    if (h == NULL) {
        (void)printf("SELFTEST dlopen_failed %s %s\n", path, dlerror());
        return NULL;
    }
    *(void **)(&create) = dlsym(h, "HuksCreateHdiDevicePtr");
    if (create == NULL) {
        (void)printf("SELFTEST dlsym_failed HuksCreateHdiDevicePtr\n");
        return NULL;
    }
    *handleOut = h;
    return create();
}

static void test_engine(struct HuksHdi *e)
{
    struct PsBuf psb;
    struct HksParamSet *ps;
    struct HksBlob alias = blob_of("rsa-test-alias", 14);
    struct HksBlob keyOut = { 0, NULL };
    struct HksBlob key;
    struct HksBlob data = blob_of("the quick brown fox", 19);
    struct HksBlob sig = { 0, NULL };
    struct HksBlob recovered = { 0, NULL };
    int32_t ret;

    CHECK(e != NULL, "engine.vtable");
    CHECK(e->HuksHdiModuleInit() == HKS_SUCCESS, "engine.module_init");

    /* ---- RSA-2048 sign/verify ---- */
    ps = ps_begin(&psb);
    ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
    ps_add_uint(ps, HKS_TAG_KEY_SIZE, 2048);
    ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN | HKS_KEY_PURPOSE_VERIFY);
    ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
    ps_add_uint(ps, HKS_TAG_PADDING, HKS_PADDING_PKCS1_V1_5);
    ret = e->HuksHdiGenerateKey(&alias, ps, NULL, &keyOut);
    CHECK(ret == HKS_SUCCESS && keyOut.size > sizeof(struct HksParamSet), "engine.rsa.generates_token");
    check_key_blob_paramset(keyOut.data, keyOut.size, HKS_KEY_FLAG_GENERATE_KEY, "engine.rsa.paramset_blob");
    key = blob_copy(&keyOut);
    blob_free(&keyOut);

    ps = ps_begin(&psb);
    ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
    ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN);
    ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
    ps_add_uint(ps, HKS_TAG_PADDING, HKS_PADDING_PKCS1_V1_5);
    ret = e->HuksHdiSign(&key, ps, &data, &sig);
    CHECK(ret == HKS_SUCCESS && sig.size == 256, "engine.rsa.sign");

    ps = ps_begin(&psb);
    ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
    ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_VERIFY);
    ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
    ps_add_uint(ps, HKS_TAG_PADDING, HKS_PADDING_PKCS1_V1_5);
    ret = e->HuksHdiVerify(&key, ps, &data, &sig);
    CHECK(ret == HKS_SUCCESS, "engine.rsa.verify");

    ((uint8_t *)sig.data)[0] ^= 0x5a;
    ret = e->HuksHdiVerify(&key, ps, &data, &sig);
    CHECK(ret == HKS_ERROR_VERIFICATION_FAILED, "engine.rsa.verify_rejects_tamper");
    ((uint8_t *)sig.data)[0] ^= 0x5a;

    /* public key export (KeyMaterialRsa) + import verify roundtrip */
    ps = ps_begin(&psb);
    ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
    ret = e->HuksHdiExportPublicKey(&key, ps, &recovered);
    if (ret == HKS_SUCCESS && recovered.size > sizeof(struct KeyMaterialRsa)) {
        struct KeyMaterialRsa km;
        (void)memcpy(&km, recovered.data, sizeof(km));
        CHECK(km.keyAlg == HKS_ALG_RSA && km.keySize == 2048u && km.dSize == 0u &&
            recovered.size == sizeof(km) + km.nSize + km.eSize, "engine.rsa.export_public_key_material");
    } else {
        CHECK(0, "engine.rsa.export_public_key_material");
    }
    {
        struct HksBlob rsaPubAlias = blob_of("rsa-pub-import", 14);
        struct HksBlob rsaPubOut = { 0, NULL };
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 2048);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_VERIFY);
        ret = e->HuksHdiImportKey(&rsaPubAlias, &recovered, ps, &rsaPubOut);
        CHECK(ret == HKS_SUCCESS, "engine.rsa.import_public_key_material");
        check_key_blob_paramset(rsaPubOut.data, rsaPubOut.size, HKS_KEY_FLAG_IMPORT_KEY, "engine.rsa.import_paramset");
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_VERIFY);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ps_add_uint(ps, HKS_TAG_PADDING, HKS_PADDING_PKCS1_V1_5);
        ret = e->HuksHdiVerify(&rsaPubOut, ps, &data, &sig);
        CHECK(ret == HKS_SUCCESS, "engine.rsa.imported_pub_verifies_sig");
        blob_free(&rsaPubOut);
    }
    blob_free(&recovered);
    blob_free(&sig);

    /* ---- ECC P-256 sign/verify ---- */
    {
        struct HksBlob eccAlias = blob_of("ecc-test-alias", 14);
        struct HksBlob eccOut = { 0, NULL };
        struct HksBlob eccKey;
        struct HksBlob eccSig = { 0, NULL };

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN | HKS_KEY_PURPOSE_VERIFY);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ret = e->HuksHdiGenerateKey(&eccAlias, ps, NULL, &eccOut);
        CHECK(ret == HKS_SUCCESS, "engine.ecc.generate");
        check_key_blob_paramset(eccOut.data, eccOut.size, HKS_KEY_FLAG_GENERATE_KEY, "engine.ecc.paramset_blob");
        eccKey = blob_copy(&eccOut);
        blob_free(&eccOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ret = e->HuksHdiSign(&eccKey, ps, &data, &eccSig);
        CHECK(ret == HKS_SUCCESS && eccSig.size > 0 && eccSig.data[0] == 0x30, "engine.ecc.sign_der");

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_VERIFY);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ret = e->HuksHdiVerify(&eccKey, ps, &data, &eccSig);
        CHECK(ret == HKS_SUCCESS, "engine.ecc.verify");

        {
            struct HksBlob eccPub = { 0, NULL };
            struct HksBlob eccPubAlias = blob_of("ecc-pub-import", 14);
            struct HksBlob eccPubOut = { 0, NULL };
            ps = ps_begin(&psb);
            ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
            ret = e->HuksHdiExportPublicKey(&eccKey, ps, &eccPub);
            if (ret == HKS_SUCCESS && eccPub.size == sizeof(struct KeyMaterialEcc) + 64u) {
                struct KeyMaterialEcc km;
                (void)memcpy(&km, eccPub.data, sizeof(km));
                CHECK(km.keyAlg == HKS_ALG_ECC && km.keySize == 256u &&
                    km.xSize == 32u && km.ySize == 32u && km.zSize == 0u,
                    "engine.ecc.export_public_key_material");
            } else {
                CHECK(0, "engine.ecc.export_public_key_material");
            }
            ps = ps_begin(&psb);
            ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
            ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
            ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_VERIFY);
            ret = e->HuksHdiImportKey(&eccPubAlias, &eccPub, ps, &eccPubOut);
            CHECK(ret == HKS_SUCCESS, "engine.ecc.import_public_key_material");
            ps = ps_begin(&psb);
            ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
            ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_VERIFY);
            ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
            ret = e->HuksHdiVerify(&eccPubOut, ps, &data, &eccSig);
            CHECK(ret == HKS_SUCCESS, "engine.ecc.imported_pub_verifies_sig");
            blob_free(&eccPubOut);
            blob_free(&eccPub);
        }
        blob_free(&eccSig);
        blob_free(&eccKey);
    }

    /* ---- AES-256-GCM ---- */
    {
        static uint8_t nonce[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        static uint8_t aad[5] = { 'a', 'a', 'd', '!', '!' };
        struct HksBlob aesAlias = blob_of("aes-gcm-alias", 13);
        struct HksBlob aesOut = { 0, NULL };
        struct HksBlob aesKey;
        struct HksBlob ct = { 0, NULL };
        struct HksBlob pt = { 0, NULL };

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT | HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_GCM);
        ret = e->HuksHdiGenerateKey(&aesAlias, ps, NULL, &aesOut);
        CHECK(ret == HKS_SUCCESS && aesOut.size > sizeof(struct HksParamSet), "engine.aes.generate");
        check_key_blob_paramset(aesOut.data, aesOut.size, HKS_KEY_FLAG_GENERATE_KEY, "engine.aes.paramset_blob");
        aesKey = blob_copy(&aesOut);
        blob_free(&aesOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_GCM);
        ps_add_blob(ps, HKS_TAG_NONCE, nonce, sizeof(nonce));
        ps_add_blob(ps, HKS_TAG_ASSOCIATED_DATA, aad, sizeof(aad));
        ret = e->HuksHdiEncrypt(&aesKey, ps, &data, &ct);
        CHECK(ret == HKS_SUCCESS && ct.size == data.size + 16u, "engine.aes_gcm.encrypt");

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_GCM);
        ps_add_blob(ps, HKS_TAG_NONCE, nonce, sizeof(nonce));
        ps_add_blob(ps, HKS_TAG_ASSOCIATED_DATA, aad, sizeof(aad));
        ret = e->HuksHdiDecrypt(&aesKey, ps, &ct, &pt);
        CHECK(ret == HKS_SUCCESS && pt.size == data.size &&
            memcmp(pt.data, data.data, data.size) == 0, "engine.aes_gcm.roundtrip");
        blob_free(&pt);

        ((uint8_t *)ct.data)[0] ^= 0xff;
        pt.data = NULL;
        pt.size = 0;
        ret = e->HuksHdiDecrypt(&aesKey, ps, &ct, &pt);
        CHECK(ret == HKS_ERROR_VERIFICATION_FAILED, "engine.aes_gcm.rejects_tamper");
        ((uint8_t *)ct.data)[0] ^= 0xff;
        blob_free(&ct);
        blob_free(&aesKey);
    }

    /* ---- AES-128-CBC ---- */
    {
        static uint8_t iv[16] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6 };
        struct HksBlob cbcAlias = blob_of("aes-cbc-alias", 13);
        struct HksBlob cbcOut = { 0, NULL };
        struct HksBlob cbcKey;
        struct HksBlob ct = { 0, NULL };
        struct HksBlob pt = { 0, NULL };

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 128);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT | HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_CBC);
        ret = e->HuksHdiGenerateKey(&cbcAlias, ps, NULL, &cbcOut);
        CHECK(ret == HKS_SUCCESS, "engine.aes128.generate");
        cbcKey = blob_copy(&cbcOut);
        blob_free(&cbcOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 128);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_CBC);
        ps_add_uint(ps, HKS_TAG_PADDING, HKS_PADDING_PKCS7);
        ps_add_blob(ps, HKS_TAG_IV, iv, sizeof(iv));
        ret = e->HuksHdiEncrypt(&cbcKey, ps, &data, &ct);
        CHECK(ret == HKS_SUCCESS && ct.size % 16u == 0, "engine.aes_cbc.encrypt");

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 128);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_CBC);
        ps_add_uint(ps, HKS_TAG_PADDING, HKS_PADDING_PKCS7);
        ps_add_blob(ps, HKS_TAG_IV, iv, sizeof(iv));
        ret = e->HuksHdiDecrypt(&cbcKey, ps, &ct, &pt);
        CHECK(ret == HKS_SUCCESS && pt.size == data.size &&
            memcmp(pt.data, data.data, data.size) == 0, "engine.aes_cbc.roundtrip");
        blob_free(&pt);
        blob_free(&ct);
        blob_free(&cbcKey);
    }

    /* ---- HMAC-SHA256 ---- */
    {
        struct HksBlob macAlias = blob_of("hmac-alias", 10);
        struct HksBlob macOut = { 0, NULL };
        struct HksBlob macKey;
        struct HksBlob mac = { 0, NULL };

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_HMAC);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_MAC);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ret = e->HuksHdiGenerateKey(&macAlias, ps, NULL, &macOut);
        CHECK(ret == HKS_SUCCESS, "engine.hmac.generate");
        macKey = blob_copy(&macOut);
        blob_free(&macOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_HMAC);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_MAC);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ret = e->HuksHdiMac(&macKey, ps, &data, &mac);
        CHECK(ret == HKS_SUCCESS && mac.size == 32, "engine.hmac.mac");
        blob_free(&mac);
        blob_free(&macKey);
    }

    /* ---- HKDF + PBKDF2 ---- */
    {
        static uint8_t salt[16] = { 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8 };
        static uint8_t info[8] = { 'c', 'o', 'n', 't', 'e', 'x', 't', '!' };
        struct HksBlob hkdfAlias = blob_of("hkdf-alias", 10);
        struct HksBlob hkdfOut = { 0, NULL };
        struct HksBlob hkdfKey;
        struct HksBlob derived = { 0, NULL };
        struct HksBlob pbkdfAlias = blob_of("pbkdf-alias", 11);
        struct HksBlob pbkdfOut = { 0, NULL };
        struct HksBlob pbkdfKey;

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_HKDF);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DERIVE);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ret = e->HuksHdiGenerateKey(&hkdfAlias, ps, NULL, &hkdfOut);
        CHECK(ret == HKS_SUCCESS, "engine.hkdf.generate");
        hkdfKey = blob_copy(&hkdfOut);
        blob_free(&hkdfOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_HKDF);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DERIVE);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ps_add_uint(ps, HKS_TAG_DERIVE_KEY_SIZE, 32);
        ps_add_blob(ps, HKS_TAG_SALT, salt, sizeof(salt));
        ps_add_blob(ps, HKS_TAG_INFO, info, sizeof(info));
        ret = e->HuksHdiDeriveKey(ps, &hkdfKey, &derived);
        CHECK(ret == HKS_SUCCESS && derived.size == 32, "engine.hkdf.derive");
        blob_free(&derived);
        blob_free(&hkdfKey);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_PBKDF2);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 128);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DERIVE);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ret = e->HuksHdiGenerateKey(&pbkdfAlias, ps, NULL, &pbkdfOut);
        CHECK(ret == HKS_SUCCESS, "engine.pbkdf2.generate");
        pbkdfKey = blob_copy(&pbkdfOut);
        blob_free(&pbkdfOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_PBKDF2);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DERIVE);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ps_add_uint(ps, HKS_TAG_ITERATION, 1000);
        ps_add_uint(ps, HKS_TAG_DERIVE_KEY_SIZE, 16);
        ps_add_blob(ps, HKS_TAG_SALT, salt, sizeof(salt));
        derived.data = NULL;
        derived.size = 0;
        ret = e->HuksHdiDeriveKey(ps, &pbkdfKey, &derived);
        CHECK(ret == HKS_SUCCESS && derived.size == 16, "engine.pbkdf2.derive");
        blob_free(&derived);
        blob_free(&pbkdfKey);
    }

    /* ---- GenerateRandom ---- */
    {
        uint8_t buf[32];
        struct HksBlob rnd = blob_of(buf, sizeof(buf));
        ps = ps_begin(&psb);
        ret = e->HuksHdiGenerateRandom(ps, &rnd);
        CHECK(ret == HKS_SUCCESS && rnd.size == sizeof(buf), "engine.random");
    }

    /* ---- unsupported algorithm must return a HKS_* code, not crash ---- */
    {
        struct HksBlob smAlias = blob_of("sm4-alias", 9);
        struct HksBlob smOut = { 0, NULL };
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_SM4);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 128);
        ret = e->HuksHdiGenerateKey(&smAlias, ps, NULL, &smOut);
        CHECK(ret == HKS_ERROR_NOT_SUPPORTED, "engine.unsupported_alg_error_code");
    }

    /* ---- Three-stage ECC sign/verify (8-byte handle ABI) ---- */
    {
        struct HksBlob eccAlias = blob_of("ecc-3s-alias", 12);
        struct HksBlob eccOut = { 0, NULL };
        struct HksBlob eccKey;
        uint8_t handleBuf[8];
        uint8_t smallHandleBuf[4];
        struct HksBlob hBlob = blob_of(handleBuf, sizeof(handleBuf));
        struct HksBlob smallH = blob_of(smallHandleBuf, sizeof(smallHandleBuf));
        struct HksBlob tok = { 0, NULL };
        struct HksBlob empty = { 0, NULL };
        struct HksBlob sig3 = { 0, NULL };
        struct HksBlob part1 = blob_of("hello ", 6);
        struct HksBlob part2 = blob_of("three-stage!", 12);
        struct HksBlob updOut = { 0, NULL };

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN | HKS_KEY_PURPOSE_VERIFY);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ret = e->HuksHdiGenerateKey(&eccAlias, ps, NULL, &eccOut);
        CHECK(ret == HKS_SUCCESS, "engine.ecc_3s.generate");
        eccKey = blob_copy(&eccOut);
        blob_free(&eccOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ret = e->HuksHdiInit(&eccKey, ps, &smallH, &tok);
        CHECK(ret == HKS_ERROR_BUFFER_TOO_SMALL, "engine.3s.rejects_small_handle");

        hBlob = blob_of(handleBuf, sizeof(handleBuf));
        ret = e->HuksHdiInit(&eccKey, ps, &hBlob, &tok);
        CHECK(ret == HKS_SUCCESS && hBlob.size == 8u && hBlob.data == handleBuf && tok.size == 0u,
            "engine.3s.init_8b_handle");
        ret = e->HuksHdiUpdate(&hBlob, ps, &part1, &updOut);
        CHECK(ret == HKS_SUCCESS, "engine.ecc_3s.sign_update");
        ret = e->HuksHdiFinish(&hBlob, ps, &part2, &sig3);
        CHECK(ret == HKS_SUCCESS && sig3.size > 0 && sig3.data[0] == 0x30, "engine.ecc_3s.sign_finish");

        /* Verify: chunks in Update, signature in Finish inData */
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_VERIFY);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        hBlob = blob_of(handleBuf, sizeof(handleBuf));
        ret = e->HuksHdiInit(&eccKey, ps, &hBlob, &tok);
        CHECK(ret == HKS_SUCCESS && hBlob.size == 8u, "engine.ecc_3s.verify_init");
        CHECK(e->HuksHdiUpdate(&hBlob, ps, &part1, &updOut) == HKS_SUCCESS, "engine.ecc_3s.verify_update1");
        CHECK(e->HuksHdiUpdate(&hBlob, ps, &part2, &updOut) == HKS_SUCCESS, "engine.ecc_3s.verify_update2");
        ret = e->HuksHdiFinish(&hBlob, ps, &sig3, &empty);
        CHECK(ret == HKS_SUCCESS, "engine.ecc_3s.verify_finish");

        /* Tampered signature rejected */
        ((uint8_t *)sig3.data)[sig3.size - 1] ^= 0xff;
        hBlob = blob_of(handleBuf, sizeof(handleBuf));
        CHECK(e->HuksHdiInit(&eccKey, ps, &hBlob, &tok) == HKS_SUCCESS, "engine.ecc_3s.tamper_init");
        CHECK(e->HuksHdiUpdate(&hBlob, ps, &part1, &updOut) == HKS_SUCCESS, "engine.ecc_3s.tamper_u1");
        CHECK(e->HuksHdiUpdate(&hBlob, ps, &part2, &updOut) == HKS_SUCCESS, "engine.ecc_3s.tamper_u2");
        ret = e->HuksHdiFinish(&hBlob, ps, &sig3, &empty);
        CHECK(ret == HKS_ERROR_VERIFICATION_FAILED, "engine.ecc_3s.verify_rejects_tamper");

        blob_free(&sig3);
        blob_free(&eccKey);
    }

    /* ---- Three-stage AES-256-GCM streaming + AES-128-CBC + AES-256-CTR ---- */
    {
        static uint8_t nonce[12] = { 7, 7, 7, 7, 1, 2, 3, 4, 5, 6, 7, 8 };
        static uint8_t aad[4] = { 'h', 'm', 'o', 's' };
        static uint8_t iv16[16] = { 1, 3, 5, 7, 9, 11, 13, 15, 2, 4, 6, 8, 10, 12, 14, 16 };
        struct HksBlob aesAlias = blob_of("aes-3s-gcm", 10);
        struct HksBlob aesOut = { 0, NULL };
        struct HksBlob aesKey;
        uint8_t hBuf[8];
        struct HksBlob hBlob = blob_of(hBuf, sizeof(hBuf));
        struct HksBlob tok = { 0, NULL };
        struct HksBlob p1 = blob_of("0123456789abcdef0123", 20);
        struct HksBlob p2 = blob_of("456789abcdef", 12);
        struct HksBlob c1 = { 0, NULL };
        struct HksBlob c2 = { 0, NULL };
        uint8_t tag[16];
        struct HksBlob d1 = { 0, NULL };
        struct HksBlob d2 = { 0, NULL };
        uint8_t combined[64];
        struct HksBlob ctrAlias = blob_of("aes-3s-ctr", 10);
        struct HksBlob ctrOut = { 0, NULL };
        struct HksBlob ctrKey;

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT | HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_GCM);
        ret = e->HuksHdiGenerateKey(&aesAlias, ps, NULL, &aesOut);
        CHECK(ret == HKS_SUCCESS, "engine.aes_3s.generate");
        aesKey = blob_copy(&aesOut);
        blob_free(&aesOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_GCM);
        ps_add_blob(ps, HKS_TAG_NONCE, nonce, sizeof(nonce));
        ps_add_blob(ps, HKS_TAG_ASSOCIATED_DATA, aad, sizeof(aad));
        ret = e->HuksHdiInit(&aesKey, ps, &hBlob, &tok);
        CHECK(ret == HKS_SUCCESS && hBlob.size == 8u, "engine.aes_gcm_3s.enc_init");
        ret = e->HuksHdiUpdate(&hBlob, ps, &p1, &c1);
        CHECK(ret == HKS_SUCCESS && c1.size == p1.size, "engine.aes_gcm_3s.enc_update");
        ret = e->HuksHdiFinish(&hBlob, ps, &p2, &c2);
        CHECK(ret == HKS_SUCCESS && c2.size == p2.size + 16u, "engine.aes_gcm_3s.enc_finish");

        (void)memcpy(combined, c1.data, c1.size);
        (void)memcpy(combined + c1.size, c2.data, c2.size - 16u);
        (void)memcpy(tag, c2.data + (c2.size - 16u), 16u);
        {
            struct HksBlob dc1 = blob_of(combined, c1.size);
            struct HksBlob dc2 = blob_of(combined + c1.size, c2.size - 16u);
            ps = ps_begin(&psb);
            ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
            ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
            ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DECRYPT);
            ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_GCM);
            ps_add_blob(ps, HKS_TAG_NONCE, nonce, sizeof(nonce));
            ps_add_blob(ps, HKS_TAG_ASSOCIATED_DATA, aad, sizeof(aad));
            ps_add_blob(ps, HKS_TAG_AE_TAG, tag, sizeof(tag));
            hBlob = blob_of(hBuf, sizeof(hBuf));
            CHECK(e->HuksHdiInit(&aesKey, ps, &hBlob, &tok) == HKS_SUCCESS, "engine.aes_gcm_3s.dec_init");
            CHECK(e->HuksHdiUpdate(&hBlob, ps, &dc1, &d1) == HKS_SUCCESS && d1.size == dc1.size,
                "engine.aes_gcm_3s.dec_update");
            CHECK(e->HuksHdiFinish(&hBlob, ps, &dc2, &d2) == HKS_SUCCESS && d2.size == dc2.size &&
                memcmp(d1.data, p1.data, p1.size) == 0 && memcmp(d2.data, p2.data, p2.size) == 0,
                "engine.aes_gcm_3s.dec_finish_roundtrip");
            blob_free(&d1);
            blob_free(&d2);
        }
        blob_free(&c1);
        blob_free(&c2);
        blob_free(&aesKey);

        /* AES-256-CTR streaming + Abort check */
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT | HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_CTR);
        CHECK(e->HuksHdiGenerateKey(&ctrAlias, ps, NULL, &ctrOut) == HKS_SUCCESS, "engine.aes_ctr.generate");
        ctrKey = blob_copy(&ctrOut);
        blob_free(&ctrOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_CTR);
        ps_add_blob(ps, HKS_TAG_IV, iv16, sizeof(iv16));
        hBlob = blob_of(hBuf, sizeof(hBuf));
        CHECK(e->HuksHdiInit(&ctrKey, ps, &hBlob, &tok) == HKS_SUCCESS, "engine.aes_ctr.abort_init");
        CHECK(e->HuksHdiAbort(&hBlob, ps) == HKS_SUCCESS, "engine.aes_ctr.abort");
        CHECK(e->HuksHdiUpdate(&hBlob, ps, &p1, &c1) == HKS_ERROR_NOT_EXIST, "engine.aes_ctr.aborted_not_exist");

        hBlob = blob_of(hBuf, sizeof(hBuf));
        CHECK(e->HuksHdiInit(&ctrKey, ps, &hBlob, &tok) == HKS_SUCCESS, "engine.aes_ctr.enc_init");
        CHECK(e->HuksHdiUpdate(&hBlob, ps, &p1, &c1) == HKS_SUCCESS, "engine.aes_ctr.enc_u");
        CHECK(e->HuksHdiFinish(&hBlob, ps, &p2, &c2) == HKS_SUCCESS, "engine.aes_ctr.enc_f");
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_CTR);
        ps_add_blob(ps, HKS_TAG_IV, iv16, sizeof(iv16));
        hBlob = blob_of(hBuf, sizeof(hBuf));
        CHECK(e->HuksHdiInit(&ctrKey, ps, &hBlob, &tok) == HKS_SUCCESS, "engine.aes_ctr.dec_init");
        CHECK(e->HuksHdiUpdate(&hBlob, ps, &c1, &d1) == HKS_SUCCESS, "engine.aes_ctr.dec_u");
        CHECK(e->HuksHdiFinish(&hBlob, ps, &c2, &d2) == HKS_SUCCESS &&
            d1.size == p1.size && d2.size == p2.size &&
            memcmp(d1.data, p1.data, p1.size) == 0 && memcmp(d2.data, p2.data, p2.size) == 0,
            "engine.aes_ctr.dec_roundtrip");
        blob_free(&c1);
        blob_free(&c2);
        blob_free(&d1);
        blob_free(&d2);
        blob_free(&ctrKey);
    }

    /* ---- Three-stage HKDF Derive (TEMP raw + PERSISTENT stored keyblob) & HMAC-SHA512 ---- */
    {
        static uint8_t dSalt[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
        static uint8_t dInfo[6] = { 'r', 'd', 'b', 'k', 'e', 'y' };
        static uint8_t gcmNonce[12] = { 3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8 };
        struct HksBlob kAlias = blob_of("hkdf-3s-base", 12);
        struct HksBlob kOut = { 0, NULL };
        struct HksBlob baseKey;
        uint8_t hBuf[8];
        struct HksBlob hBlob = blob_of(hBuf, sizeof(hBuf));
        struct HksBlob tok = { 0, NULL };
        struct HksBlob updOut = { 0, NULL };
        struct HksBlob empty = { 0, NULL };
        struct HksBlob rawDerived = { 0, NULL };
        struct HksBlob storedDerived = { 0, NULL };
        struct HksBlob ct = { 0, NULL };
        struct HksBlob pt = { 0, NULL };
        struct HksBlob macAlias = blob_of("hmac-512-3s", 11);
        struct HksBlob macKey = { 0, NULL };
        struct HksBlob macVal = { 0, NULL };

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DERIVE);
        ret = e->HuksHdiGenerateKey(&kAlias, ps, NULL, &kOut);
        CHECK(ret == HKS_SUCCESS, "engine.derive_3s.gen_base");
        baseKey = blob_copy(&kOut);
        blob_free(&kOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_HKDF);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DERIVE);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ps_add_uint(ps, HKS_TAG_DERIVE_KEY_SIZE, 32);
        ps_add_blob(ps, HKS_TAG_SALT, dSalt, sizeof(dSalt));
        ps_add_blob(ps, HKS_TAG_INFO, dInfo, sizeof(dInfo));
        hBlob = blob_of(hBuf, sizeof(hBuf));
        CHECK(e->HuksHdiInit(&baseKey, ps, &hBlob, &tok) == HKS_SUCCESS, "engine.derive_3s.temp_init");
        CHECK(e->HuksHdiUpdate(&hBlob, ps, &empty, &updOut) == HKS_SUCCESS, "engine.derive_3s.temp_update");
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_KEY_STORAGE_FLAG, HKS_STORAGE_TEMP);
        CHECK(e->HuksHdiFinish(&hBlob, ps, &empty, &rawDerived) == HKS_SUCCESS && rawDerived.size == 32u,
            "engine.derive_3s.temp_finish");
        blob_free(&rawDerived);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_HKDF);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DERIVE);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ps_add_uint(ps, HKS_TAG_DERIVE_KEY_SIZE, 32);
        ps_add_blob(ps, HKS_TAG_SALT, dSalt, sizeof(dSalt));
        ps_add_blob(ps, HKS_TAG_INFO, dInfo, sizeof(dInfo));
        hBlob = blob_of(hBuf, sizeof(hBuf));
        CHECK(e->HuksHdiInit(&baseKey, ps, &hBlob, &tok) == HKS_SUCCESS, "engine.derive_3s.stored_init");
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT | HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_GCM);
        ps_add_uint(ps, HKS_TAG_KEY_STORAGE_FLAG, HKS_STORAGE_PERSISTENT);
        ps_add_blob(ps, HKS_TAG_KEY_ALIAS, (const uint8_t *)"derived-aes", 11);
        CHECK(e->HuksHdiFinish(&hBlob, ps, &empty, &storedDerived) == HKS_SUCCESS, "engine.derive_3s.stored_finish");
        check_key_blob_paramset(storedDerived.data, storedDerived.size, HKS_KEY_FLAG_DERIVE_KEY,
            "engine.derive_3s.stored_paramset");

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_GCM);
        ps_add_blob(ps, HKS_TAG_NONCE, gcmNonce, sizeof(gcmNonce));
        CHECK(e->HuksHdiEncrypt(&storedDerived, ps, &data, &ct) == HKS_SUCCESS, "engine.derive_3s.stored_enc");
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_BLOCK_MODE, HKS_MODE_GCM);
        ps_add_blob(ps, HKS_TAG_NONCE, gcmNonce, sizeof(gcmNonce));
        CHECK(e->HuksHdiDecrypt(&storedDerived, ps, &ct, &pt) == HKS_SUCCESS &&
            pt.size == data.size && memcmp(pt.data, data.data, data.size) == 0,
            "engine.derive_3s.stored_dec");
        blob_free(&ct);
        blob_free(&pt);
        blob_free(&storedDerived);
        blob_free(&baseKey);

        /* HMAC-SHA512 3-stage */
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_HMAC);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_MAC);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA512);
        CHECK(e->HuksHdiGenerateKey(&macAlias, ps, NULL, &macKey) == HKS_SUCCESS, "engine.hmac512.generate");
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_HMAC);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_MAC);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA512);
        hBlob = blob_of(hBuf, sizeof(hBuf));
        CHECK(e->HuksHdiInit(&macKey, ps, &hBlob, &tok) == HKS_SUCCESS, "engine.hmac512_3s.init");
        CHECK(e->HuksHdiUpdate(&hBlob, ps, &data, &updOut) == HKS_SUCCESS, "engine.hmac512_3s.update");
        CHECK(e->HuksHdiFinish(&hBlob, ps, &empty, &macVal) == HKS_SUCCESS && macVal.size == 64u,
            "engine.hmac512_3s.finish");
        blob_free(&macVal);
        blob_free(&macKey);
    }

    /* ---- RSA-2048 encrypt/decrypt (OAEP + PKCS1_V1_5) + LRU eviction (>256 sessions) ---- */
    {
        struct HksBlob rsaAlias = blob_of("rsa-enc-alias", 13);
        struct HksBlob rsaOut = { 0, NULL };
        struct HksBlob rsaKey;
        struct HksBlob ct = { 0, NULL };
        struct HksBlob pt = { 0, NULL };
        uint8_t lastHBuf[8];
        uint8_t firstHBuf[8];
        struct HksBlob tok = { 0, NULL };
        struct HksBlob sigOut = { 0, NULL };

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
        ps_add_uint(ps, HKS_TAG_KEY_SIZE, 2048);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT | HKS_KEY_PURPOSE_DECRYPT | HKS_KEY_PURPOSE_SIGN);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        CHECK(e->HuksHdiGenerateKey(&rsaAlias, ps, NULL, &rsaOut) == HKS_SUCCESS, "engine.rsa_enc.generate");
        rsaKey = blob_copy(&rsaOut);
        blob_free(&rsaOut);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_ENCRYPT);
        ps_add_uint(ps, HKS_TAG_PADDING, HKS_PADDING_OAEP);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        CHECK(e->HuksHdiEncrypt(&rsaKey, ps, &data, &ct) == HKS_SUCCESS && ct.size == 256u, "engine.rsa_oaep.enc");
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_DECRYPT);
        ps_add_uint(ps, HKS_TAG_PADDING, HKS_PADDING_OAEP);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        CHECK(e->HuksHdiDecrypt(&rsaKey, ps, &ct, &pt) == HKS_SUCCESS &&
            pt.size == data.size && memcmp(pt.data, data.data, data.size) == 0, "engine.rsa_oaep.dec");
        blob_free(&ct);
        blob_free(&pt);

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        ps_add_uint(ps, HKS_TAG_PADDING, HKS_PADDING_PKCS1_V1_5);
        int lruOk = 1;
        for (int s = 0; s < 270; s++) {
            uint8_t hBuf[8];
            struct HksBlob hb = blob_of(hBuf, sizeof(hBuf));
            if (e->HuksHdiInit(&rsaKey, ps, &hb, &tok) != HKS_SUCCESS || hb.size != 8u) {
                lruOk = 0;
                break;
            }
            if (s == 0) {
                (void)memcpy(firstHBuf, hBuf, 8u);
            }
            if (s == 269) {
                (void)memcpy(lastHBuf, hBuf, 8u);
            }
        }
        CHECK(lruOk, "engine.sessions.lru_evicts_over_256");
        {
            struct HksBlob stale = blob_of(firstHBuf, 8u);
            struct HksBlob fresh = blob_of(lastHBuf, 8u);
            CHECK(e->HuksHdiAbort(&stale, ps) == HKS_SUCCESS, "engine.sessions.evicted_abort_safe");
            CHECK(e->HuksHdiFinish(&fresh, ps, &data, &sigOut) == HKS_SUCCESS && sigOut.size == 256u,
                "engine.sessions.latest_survives_finish");
            blob_free(&sigOut);
        }
        blob_free(&rsaKey);
    }

    /* ---- attestation is deferred to DCM, never faked here ---- */
    {
        struct HksBlob cert = { 0, NULL };
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        ret = e->HuksHdiAttestKey(&key, ps, &cert);
        CHECK(ret == HKS_SUCCESS && cert.data == NULL && cert.size == 0u,
            "engine.attest_defers_to_dcm");
    }

    /* ---- missing key returns NOT_EXIST ---- */
    {
        uint8_t fake[SOFT_HUKS_TOKEN_LEN] = { 'S', 'H', 'K', '1', 0xde, 0xad, 0xbe, 0xef };
        struct HksBlob fakeKey = blob_of(fake, sizeof(fake));
        struct HksBlob out = { 0, NULL };
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_RSA);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN);
        ret = e->HuksHdiSign(&fakeKey, ps, &data, &out);
        CHECK(ret == HKS_ERROR_NOT_EXIST || ret == HKS_ERROR_VERIFICATION_FAILED,
            "engine.unknown_key_not_exist");
    }

    blob_free(&key);
    (void)e->HuksHdiModuleDestroy();
}

/* ------------------------------------------------------------------ */
/* HDI seam (struct IHuks v1.1)                                        */
/* ------------------------------------------------------------------ */

static void test_hdi(const char *path)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    struct IHuks *(*getInstance)(void);
    struct IHuks *iface;
    uint32_t major = 0;
    uint32_t minor = 0;
    struct PsBuf psb;
    struct HksParamSet *ps;
    struct HuksBlob aliasBlob;
    struct HuksBlob keyOut = { .data = NULL, .dataLen = 0 };
    struct HuksBlob key;
    struct HuksBlob dataBlob;
    struct HuksBlob sigBlob = { .data = NULL, .dataLen = 0 };
    struct HuksParamSet idlPs;
    int32_t ret;

    CHECK(h != NULL, "hdi.dlopen");
    if (h == NULL) {
        (void)printf("SELFTEST dlopen_failed %s %s\n", path, dlerror());
        return;
    }
    *(void **)(&getInstance) = dlsym(h, "HuksImplGetInstance");
    CHECK(getInstance != NULL, "hdi.dlsym_HuksImplGetInstance");
    if (getInstance == NULL) {
        return;
    }
    iface = getInstance();
    CHECK(iface != NULL, "hdi.instance");

    (void)printf("ABI IHuks.size=%zu slots=%d\n", sizeof(struct IHuks), (int)SOFT_HUKS_IHUKS_SLOTS);
    (void)printf("ABI offset.GenerateKey=%zu\n", offsetof(struct IHuks, GenerateKey));
    (void)printf("ABI offset.Sign=%zu\n", offsetof(struct IHuks, Sign));
    (void)printf("ABI offset.Verify=%zu\n", offsetof(struct IHuks, Verify));
    (void)printf("ABI offset.GetVersion=%zu\n", offsetof(struct IHuks, GetVersion));
    (void)printf("ABI HuksBlob.size=%zu HuksParamSet.size=%zu\n", sizeof(struct HuksBlob),
        sizeof(struct HuksParamSet));
    (void)printf("ABI offset.HuksBlob.data=%zu offset.HuksBlob.dataLen=%zu offset.HuksParamSet.data=%zu offset.HuksParamSet.dataLen=%zu\n",
        offsetof(struct HuksBlob, data), offsetof(struct HuksBlob, dataLen),
        offsetof(struct HuksParamSet, data), offsetof(struct HuksParamSet, dataLen));

    ret = iface->GetVersion(iface, &major, &minor);
    (void)printf("ABI version=%u.%u\n", major, minor);
    CHECK(ret == HKS_SUCCESS && major == 1 && minor == 1, "hdi.get_version_1_1");
    CHECK(iface->ModuleInit(iface) == HKS_SUCCESS, "hdi.module_init");
    CHECK(iface->GetVersion(iface, &major, &minor) == HKS_SUCCESS, "hdi.version_slot_callable");

    /* generate through the IDL struct */
    ps = ps_begin(&psb);
    ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
    ps_add_uint(ps, HKS_TAG_KEY_SIZE, 256);
    ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN | HKS_KEY_PURPOSE_VERIFY);
    ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
    idlPs.data = (uint8_t *)ps;
    idlPs.dataLen = ps->paramSetSize;
    aliasBlob.data = (uint8_t *)"hdi-ecc-alias";
    aliasBlob.dataLen = 13;
    ret = iface->GenerateKey(iface, &aliasBlob, &idlPs, NULL, &keyOut);
    CHECK(ret == HKS_SUCCESS && keyOut.dataLen > sizeof(struct HksParamSet), "hdi.generate_key");
    check_key_blob_paramset(keyOut.data, keyOut.dataLen, HKS_KEY_FLAG_GENERATE_KEY, "hdi.paramset_blob");
    key.data = keyOut.data;
    key.dataLen = keyOut.dataLen;

    dataBlob.data = (uint8_t *)"payload";
    dataBlob.dataLen = 7;
    ps = ps_begin(&psb);
    ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
    ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN);
    ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
    idlPs.data = (uint8_t *)ps;
    idlPs.dataLen = ps->paramSetSize;
    ret = iface->Sign(iface, &key, &idlPs, &dataBlob, &sigBlob);
    CHECK(ret == HKS_SUCCESS && sigBlob.dataLen > 0, "hdi.sign");

    ps = ps_begin(&psb);
    ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
    ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_VERIFY);
    ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
    idlPs.data = (uint8_t *)ps;
    idlPs.dataLen = ps->paramSetSize;
    ret = iface->Verify(iface, &key, &idlPs, &dataBlob, &sigBlob);
    CHECK(ret == HKS_SUCCESS, "hdi.verify");

    /* HDI v1.1 3-stage Init (8-byte handle) -> Update -> Finish -> Verify */
    {
        uint8_t hData[8] = { 0 };
        uint8_t tData[32] = { 0 };
        struct HuksBlob hBlob = { .data = hData, .dataLen = sizeof(hData) };
        struct HuksBlob tBlob = { .data = tData, .dataLen = sizeof(tData) };
        struct HuksBlob p1Blob = { .data = (uint8_t *)"hdi-", .dataLen = 4 };
        struct HuksBlob p2Blob = { .data = (uint8_t *)"3stage", .dataLen = 6 };
        struct HuksBlob fullBlob = { .data = (uint8_t *)"hdi-3stage", .dataLen = 10 };
        struct HuksBlob uOut = { .data = NULL, .dataLen = 0 };
        struct HuksBlob fSig = { .data = NULL, .dataLen = 0 };

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_SIGN);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        idlPs.data = (uint8_t *)ps;
        idlPs.dataLen = ps->paramSetSize;
        ret = iface->Init(iface, &key, &idlPs, &hBlob, &tBlob);
        CHECK(ret == HKS_SUCCESS && hBlob.dataLen == 8u && hBlob.data == hData && tBlob.dataLen == 0u,
            "hdi.3s.init_8b_handle");
        CHECK(iface->Update(iface, &hBlob, &idlPs, &p1Blob, &uOut) == HKS_SUCCESS, "hdi.3s.update");
        CHECK(iface->Finish(iface, &hBlob, &idlPs, &p2Blob, &fSig) == HKS_SUCCESS && fSig.dataLen > 0,
            "hdi.3s.finish");

        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        ps_add_uint(ps, HKS_TAG_PURPOSE, HKS_KEY_PURPOSE_VERIFY);
        ps_add_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
        idlPs.data = (uint8_t *)ps;
        idlPs.dataLen = ps->paramSetSize;
        CHECK(iface->Verify(iface, &key, &idlPs, &fullBlob, &fSig) == HKS_SUCCESS, "hdi.3s.oneshot_verify");
        free(fSig.data);
    }

    /* AttestKey: local chain is deferred to DCM, so the HDI must succeed and
     * hand back a carrier blob the caller owns (64 zeroed bytes). */
    {
        struct HuksBlob certBlob = { .data = NULL, .dataLen = 0 };
        struct HuksBlob tokenBlob = { .data = keyOut.data, .dataLen = keyOut.dataLen };
        ps = ps_begin(&psb);
        ps_add_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_ECC);
        idlPs.data = (uint8_t *)ps;
        idlPs.dataLen = ps->paramSetSize;
        ret = iface->AttestKey(iface, &tokenBlob, &idlPs, &certBlob);
        CHECK(ret == HKS_SUCCESS && certBlob.data != NULL && certBlob.dataLen == 64u,
            "hdi.attest_defers_to_dcm");
        free(certBlob.data);
    }

    free(keyOut.data);
    free(sigBlob.data);
    iface->ModuleDestroy(iface);
    (void)dlclose(h);
}

int main(int argc, char **argv)
{
    void *handle = NULL;
    struct HuksHdi *engine;

    if (argc < 3) {
        (void)fprintf(stderr, "usage: %s <engine.so> <hdi.so>\n", argv[0]);
        return 2;
    }
    if (getenv("HUKS_SOFT_ROOT") == NULL) {
        (void)fprintf(stderr, "HUKS_SOFT_ROOT must be set (use a temp dir)\n");
        return 2;
    }

    engine = load_engine(argv[1], &handle);
    if (engine == NULL) {
        return 3;
    }
    test_engine(engine);
    test_hdi(argv[2]);

    (void)printf("SELFTEST SUMMARY pass=%d fail=%d\n", gPass, gFail);
    return (gFail == 0) ? 0 : 1;
}
