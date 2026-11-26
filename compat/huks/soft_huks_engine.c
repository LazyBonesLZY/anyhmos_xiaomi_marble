/*
 * Software HUKS engine for marble.
 *
 * Exports exactly the vtable seam that the HUKS HDI passthrough adapter uses
 * upstream (work/upstream/drivers_peripheral/huks/hdi_service/
 * huks_hdi_passthrough_adapter.c -> dlopen("libhuks_engine_core_standard.z.so")
 * -> dlsym("HuksCreateHdiDevicePtr") -> struct HuksHdi from
 * huks_sa_hdi_struct.h).
 *
 * Crypto: OpenSSL EVP.  Key storage: AES-256-GCM sealed files, see
 * soft_huks_store.c.  Every operation first goes through soft_route(), which
 * logs one "HUKS_BACKEND=..." line.
 *
 * NOT a TEE: no hardware isolation, no attestation, root can read every key.
 */

#include "soft_huks.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#define REC_MAGIC 0x43455253u /* "SREC" */
#define REC_VERSION 1u
#define MAX_SESSIONS 256
#define MAX_BLOB (16u * 1024u * 1024u)

struct OpParams {
    uint32_t alg;
    uint32_t bits;
    uint32_t purpose;
    uint32_t digest;
    uint32_t mode;
    uint32_t padding;
    uint32_t iteration;
    uint32_t deriveBytes;
    uint32_t origin;
    const uint8_t *iv;
    uint32_t ivLen;
    const uint8_t *aad;
    uint32_t aadLen;
    const uint8_t *aeTag;
    uint32_t aeTagLen;
    const uint8_t *salt;
    uint32_t saltLen;
    const uint8_t *info;
    uint32_t infoLen;
};

struct SoftKey {
    uint32_t alg;
    uint32_t bits;
    uint32_t flags;
    uint32_t aliasLen;
    uint8_t *alias;
    uint32_t matLen;
    uint8_t *material;
};

/* ------------------------------------------------------------------ */
/* Param helpers                                                       */
/* ------------------------------------------------------------------ */

static const struct HksParam *find_param(const struct HksParamSet *ps, uint32_t tag)
{
    uint32_t i;
    uint32_t maxCnt;

    if (ps == NULL || ps->paramSetSize < sizeof(struct HksParamSet)) {
        return NULL;
    }
    maxCnt = (ps->paramSetSize - (uint32_t)sizeof(struct HksParamSet)) / (uint32_t)sizeof(struct HksParam);
    if (ps->paramsCnt > maxCnt) {
        return NULL;
    }
    for (i = 0; i < ps->paramsCnt; i++) {
        if (ps->params[i].tag == tag) {
            return &ps->params[i];
        }
    }
    return NULL;
}

static uint32_t param_uint(const struct HksParamSet *ps, uint32_t tag, uint32_t def)
{
    const struct HksParam *p = find_param(ps, tag);
    return (p == NULL) ? def : p->uint32Param;
}

static const uint8_t *resolve_param_blob(const struct HksParamSet *ps, const struct HksParam *p)
{
    uintptr_t raw;

    if (p == NULL || p->blob.data == NULL) {
        return NULL;
    }
    raw = (uintptr_t)p->blob.data;
    if (ps != NULL && raw >= sizeof(struct HksParamSet) && raw < ps->paramSetSize &&
        p->blob.size <= ps->paramSetSize - raw) {
        return ((const uint8_t *)ps) + raw;
    }
    return p->blob.data;
}

static const uint8_t *param_bytes(const struct HksParamSet *ps, uint32_t tag, uint32_t *len)
{
    const struct HksParam *p = find_param(ps, tag);
    const uint8_t *ptr = resolve_param_blob(ps, p);
    if (ptr == NULL) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    if (len != NULL) {
        *len = p->blob.size;
    }
    return ptr;
}

static int32_t blob_put(struct HksBlob *out, const uint8_t *data, uint32_t len);

static int32_t build_key_blob_paramset(const struct HksParamSet *inPs, uint32_t keyFlag,
    const uint8_t token[SOFT_HUKS_TOKEN_LEN], struct HksBlob *keyOut)
{
    struct BuildEntry {
        struct HksParam param;
        const uint8_t *bytesPtr;
    };
    const uint32_t coreTags[5] = {
        HKS_TAG_KEY_VERSION,
        HKS_TAG_OS_VERSION,
        HKS_TAG_OS_PATCHLEVEL,
        HKS_TAG_KEY_FLAG,
        HKS_TAG_KEY,
    };
    struct BuildEntry *entries = NULL;
    uint32_t inMax = 0;
    uint32_t cnt = 0;
    uint64_t total64;
    uint32_t totalSize;
    uint32_t dataOff;
    uint8_t *buf = NULL;
    struct HksParamSet *outPs;
    int32_t ret;

    if (inPs != NULL && inPs->paramSetSize >= sizeof(struct HksParamSet)) {
        uint32_t cap = (inPs->paramSetSize - (uint32_t)sizeof(struct HksParamSet)) / (uint32_t)sizeof(struct HksParam);
        if (inPs->paramsCnt <= cap) {
            inMax = inPs->paramsCnt;
        }
    }
    if (inMax > 512u) {
        return HKS_ERROR_INVALID_ARGUMENT;
    }

    entries = calloc(inMax + 5u, sizeof(*entries));
    if (entries == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }

    for (uint32_t i = 0; i < inMax; i++) {
        const struct HksParam *p = &inPs->params[i];
        uint32_t tag = p->tag;
        bool skip = (tag == HKS_TAG_KEY_OVERRIDE);
        for (uint32_t c = 0; !skip && c < 5u; c++) {
            if (tag == coreTags[c]) {
                skip = true;
            }
        }
        for (uint32_t d = 0; !skip && d < cnt; d++) {
            if (entries[d].param.tag == tag) {
                skip = true;
            }
        }
        if (skip) {
            continue;
        }
        entries[cnt].param = *p;
        if ((tag >> 28) == 5u) {
            const uint8_t *ptr = resolve_param_blob(inPs, p);
            if (ptr == NULL || p->blob.size == 0) {
                continue;
            }
            entries[cnt].bytesPtr = ptr;
        }
        cnt++;
    }

    entries[cnt++].param = (struct HksParam){ .tag = HKS_TAG_KEY_VERSION, .uint32Param = SOFT_HUKS_KEY_VERSION };
    entries[cnt++].param = (struct HksParam){ .tag = HKS_TAG_OS_VERSION, .uint32Param = 1u };
    entries[cnt++].param = (struct HksParam){ .tag = HKS_TAG_OS_PATCHLEVEL, .uint32Param = 1u };
    entries[cnt++].param = (struct HksParam){ .tag = HKS_TAG_KEY_FLAG, .uint32Param = keyFlag };
    entries[cnt].param = (struct HksParam){ .tag = HKS_TAG_KEY, .blob = { .size = SOFT_HUKS_TOKEN_LEN, .data = NULL } };
    entries[cnt].bytesPtr = token;
    cnt++;

    total64 = sizeof(struct HksParamSet) + (uint64_t)cnt * sizeof(struct HksParam);
    for (uint32_t i = 0; i < cnt; i++) {
        if ((entries[i].param.tag >> 28) == 5u) {
            total64 += entries[i].param.blob.size;
        }
    }
    if (total64 > MAX_BLOB) {
        free(entries);
        return HKS_ERROR_INSUFFICIENT_MEMORY;
    }

    totalSize = (uint32_t)total64;
    buf = calloc(1u, totalSize);
    if (buf == NULL) {
        free(entries);
        return HKS_ERROR_MALLOC_FAIL;
    }

    outPs = (struct HksParamSet *)buf;
    outPs->paramSetSize = totalSize;
    outPs->paramsCnt = cnt;
    dataOff = (uint32_t)(sizeof(struct HksParamSet) + cnt * sizeof(struct HksParam));

    for (uint32_t i = 0; i < cnt; i++) {
        outPs->params[i] = entries[i].param;
        if ((entries[i].param.tag >> 28) == 5u) {
            uint32_t blen = entries[i].param.blob.size;
            (void)memcpy(buf + dataOff, entries[i].bytesPtr, blen);
            outPs->params[i].blob.size = blen;
            outPs->params[i].blob.data = (uint8_t *)(uintptr_t)dataOff;
            dataOff += blen;
        }
    }
    free(entries);
    if (dataOff != totalSize) {
        (void)OPENSSL_cleanse(buf, totalSize);
        free(buf);
        return HKS_ERROR_INVALID_ARGUMENT;
    }

    ret = blob_put(keyOut, buf, totalSize);
    (void)OPENSSL_cleanse(buf, totalSize);
    free(buf);
    return ret;
}

static void parse_params(const struct HksParamSet *ps, struct OpParams *op)
{
    (void)memset(op, 0, sizeof(*op));
    op->alg = param_uint(ps, HKS_TAG_ALGORITHM, 0);
    op->bits = param_uint(ps, HKS_TAG_KEY_SIZE, 0);
    op->purpose = param_uint(ps, HKS_TAG_PURPOSE, 0);
    op->digest = param_uint(ps, HKS_TAG_DIGEST, HKS_DIGEST_SHA256);
    op->mode = param_uint(ps, HKS_TAG_BLOCK_MODE, 0);
    op->padding = param_uint(ps, HKS_TAG_PADDING, HKS_PADDING_NONE);
    op->iteration = param_uint(ps, HKS_TAG_ITERATION, 0);
    op->deriveBytes = param_uint(ps, HKS_TAG_DERIVE_KEY_SIZE, 0);
    op->origin = param_uint(ps, SOFT_HUKS_TAG_KEY_ORIGIN, SOFT_KEY_ORIGIN_GENERATED);
    op->iv = param_bytes(ps, HKS_TAG_NONCE, &op->ivLen);
    if (op->iv == NULL) {
        op->iv = param_bytes(ps, HKS_TAG_IV, &op->ivLen);
    }
    op->aad = param_bytes(ps, HKS_TAG_ASSOCIATED_DATA, &op->aadLen);
    op->aeTag = param_bytes(ps, HKS_TAG_AE_TAG, &op->aeTagLen);
    op->salt = param_bytes(ps, HKS_TAG_SALT, &op->saltLen);
    op->info = param_bytes(ps, HKS_TAG_INFO, &op->infoLen);
}

static void merge_finish_params(const struct HksParamSet *ps, struct OpParams *dst, const struct OpParams *base)
{
    const uint8_t *b;
    uint32_t blen = 0;

    *dst = *base;
    if (ps == NULL) {
        return;
    }
    if (dst->alg == 0) {
        dst->alg = param_uint(ps, HKS_TAG_ALGORITHM, 0);
    }
    if (dst->bits == 0) {
        dst->bits = param_uint(ps, HKS_TAG_KEY_SIZE, 0);
    }
    if (dst->purpose == 0) {
        dst->purpose = param_uint(ps, HKS_TAG_PURPOSE, 0);
    }
    if (find_param(ps, HKS_TAG_DIGEST) != NULL) {
        dst->digest = param_uint(ps, HKS_TAG_DIGEST, dst->digest);
    }
    if (find_param(ps, HKS_TAG_BLOCK_MODE) != NULL && dst->mode == 0) {
        dst->mode = param_uint(ps, HKS_TAG_BLOCK_MODE, dst->mode);
    }
    if (find_param(ps, HKS_TAG_PADDING) != NULL && dst->padding == HKS_PADDING_NONE) {
        dst->padding = param_uint(ps, HKS_TAG_PADDING, dst->padding);
    }
    if (find_param(ps, HKS_TAG_ITERATION) != NULL) {
        dst->iteration = param_uint(ps, HKS_TAG_ITERATION, dst->iteration);
    }
    if (find_param(ps, HKS_TAG_DERIVE_KEY_SIZE) != NULL) {
        dst->deriveBytes = param_uint(ps, HKS_TAG_DERIVE_KEY_SIZE, dst->deriveBytes);
    }
    b = param_bytes(ps, HKS_TAG_NONCE, &blen);
    if (b == NULL) {
        b = param_bytes(ps, HKS_TAG_IV, &blen);
    }
    if (b != NULL && blen > 0) {
        dst->iv = b;
        dst->ivLen = blen;
    }
    b = param_bytes(ps, HKS_TAG_ASSOCIATED_DATA, &blen);
    if (b != NULL) {
        dst->aad = b;
        dst->aadLen = blen;
    }
    b = param_bytes(ps, HKS_TAG_AE_TAG, &blen);
    if (b != NULL) {
        dst->aeTag = b;
        dst->aeTagLen = blen;
    }
    b = param_bytes(ps, HKS_TAG_SALT, &blen);
    if (b != NULL) {
        dst->salt = b;
        dst->saltLen = blen;
    }
    b = param_bytes(ps, HKS_TAG_INFO, &blen);
    if (b != NULL) {
        dst->info = b;
        dst->infoLen = blen;
    }
}

static int32_t blob_put(struct HksBlob *out, const uint8_t *data, uint32_t len)
{
    uint8_t *buf;

    if (out == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    if (len == 0) {
        out->size = 0;
        return HKS_SUCCESS;
    }
    if (out->data != NULL) {
        if (out->size < len) {
            return HKS_ERROR_BUFFER_TOO_SMALL;
        }
        (void)memcpy(out->data, data, len);
        out->size = len;
        return HKS_SUCCESS;
    }
    buf = malloc(len);
    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    (void)memcpy(buf, data, len);
    out->data = buf;
    out->size = len;
    return HKS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Record (plaintext inside the sealed file)                           */
/* ------------------------------------------------------------------ */

static uint32_t put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
    return 4;
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t record_serialize(const struct SoftKey *key, uint8_t **out, uint32_t *outLen)
{
    uint32_t len = 28u + key->aliasLen + key->matLen;
    uint8_t *buf = malloc(len);
    uint8_t *p;

    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    p = buf;
    p += put_u32(p, REC_MAGIC);
    p += put_u32(p, REC_VERSION);
    p += put_u32(p, key->alg);
    p += put_u32(p, key->bits);
    p += put_u32(p, key->flags);
    p += put_u32(p, key->aliasLen);
    p += put_u32(p, key->matLen);
    if (key->aliasLen > 0) {
        (void)memcpy(p, key->alias, key->aliasLen);
        p += key->aliasLen;
    }
    if (key->matLen > 0) {
        (void)memcpy(p, key->material, key->matLen);
    }
    *out = buf;
    *outLen = len;
    return HKS_SUCCESS;
}

static void soft_key_free(struct SoftKey *key)
{
    if (key == NULL) {
        return;
    }
    if (key->material != NULL) {
        (void)OPENSSL_cleanse(key->material, key->matLen);
        free(key->material);
    }
    free(key->alias);
    (void)memset(key, 0, sizeof(*key));
}

static int32_t record_parse(const uint8_t *buf, uint32_t len, struct SoftKey *key)
{
    uint32_t aliasLen;
    uint32_t matLen;
    const uint8_t *p = buf;

    (void)memset(key, 0, sizeof(*key));
    if (len < 28u || get_u32(p) != REC_MAGIC || get_u32(p + 4) != REC_VERSION) {
        return HKS_ERROR_INVALID_KEY_FILE;
    }
    key->alg = get_u32((p += 8));
    p += 4;
    key->bits = get_u32(p);
    p += 4;
    key->flags = get_u32(p);
    p += 4;
    aliasLen = get_u32(p);
    p += 4;
    matLen = get_u32(p);
    p += 4;
    if (aliasLen > len || matLen > len || 28u + aliasLen + matLen != len) {
        return HKS_ERROR_INVALID_KEY_FILE;
    }
    if (aliasLen > 0) {
        key->alias = malloc(aliasLen);
        if (key->alias == NULL) {
            return HKS_ERROR_MALLOC_FAIL;
        }
        (void)memcpy(key->alias, p, aliasLen);
        key->aliasLen = aliasLen;
        p += aliasLen;
    }
    if (matLen > 0) {
        key->material = malloc(matLen);
        if (key->material == NULL) {
            soft_key_free(key);
            return HKS_ERROR_MALLOC_FAIL;
        }
        (void)memcpy(key->material, p, matLen);
        key->matLen = matLen;
    }
    return HKS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Crypto primitives                                                   */
/* ------------------------------------------------------------------ */

static const EVP_MD *md_of(uint32_t digest)
{
    switch (digest) {
        case HKS_DIGEST_SHA1:
            return EVP_sha1();
        case HKS_DIGEST_SHA224:
            return EVP_sha224();
        case HKS_DIGEST_SHA384:
            return EVP_sha384();
        case HKS_DIGEST_SHA512:
            return EVP_sha512();
        case HKS_DIGEST_SHA256:
        case HKS_DIGEST_NONE:
        default:
            return EVP_sha256();
    }
}

static int ecc_nid_for_bits(uint32_t bits)
{
    switch (bits) {
        case 224u:
            return NID_secp224r1;
        case 384u:
            return NID_secp384r1;
        case 521u:
            return NID_secp521r1;
        case 256u:
        default:
            return NID_X9_62_prime256v1;
    }
}

static int32_t gen_asym(int32_t type, uint32_t bits, uint8_t **der, uint32_t *derLen)
{
    EVP_PKEY_CTX *ctx = NULL;
    EVP_PKEY *pkey = NULL;
    uint8_t *buf = NULL;
    int len;
    int32_t ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;

    ctx = EVP_PKEY_CTX_new_id(type, NULL);
    if (ctx == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    if (EVP_PKEY_keygen_init(ctx) != 1) {
        goto out;
    }
    if (type == EVP_PKEY_RSA) {
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, (int)bits) != 1) {
            goto out;
        }
    } else {
        if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, ecc_nid_for_bits(bits)) != 1) {
            goto out;
        }
    }
    if (EVP_PKEY_keygen(ctx, &pkey) != 1) {
        goto out;
    }
    len = i2d_PrivateKey(pkey, NULL);
    if (len <= 0) {
        goto out;
    }
    buf = malloc((size_t)len);
    if (buf == NULL) {
        ret = HKS_ERROR_MALLOC_FAIL;
        goto out;
    }
    {
        uint8_t *q = buf;
        if (i2d_PrivateKey(pkey, &q) != len) {
            goto out;
        }
    }
    *der = buf;
    *derLen = (uint32_t)len;
    buf = NULL;
    ret = HKS_SUCCESS;

out:
    if (buf != NULL) {
        free(buf);
    }
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    return ret;
}

static int32_t pkey_from_rsa_material(const uint8_t *buf, uint32_t len, EVP_PKEY **pkey)
{
    struct KeyMaterialRsa km;
    RSA *rsa = NULL;
    BIGNUM *n = NULL;
    BIGNUM *e = NULL;
    BIGNUM *d = NULL;
    EVP_PKEY *k = NULL;
    uint32_t off = (uint32_t)sizeof(km);

    if (len < sizeof(km)) {
        return HKS_ERROR_INVALID_PRIVATE_KEY;
    }
    (void)memcpy(&km, buf, sizeof(km));
    if (km.keyAlg != HKS_ALG_RSA || km.nSize == 0 || km.eSize == 0 ||
        km.nSize > len - off || km.eSize > len - off - km.nSize ||
        km.dSize != len - off - km.nSize - km.eSize) {
        return HKS_ERROR_INVALID_PRIVATE_KEY;
    }
    n = BN_bin2bn(buf + off, (int)km.nSize, NULL);
    off += km.nSize;
    e = BN_bin2bn(buf + off, (int)km.eSize, NULL);
    off += km.eSize;
    if (km.dSize > 0) {
        d = BN_bin2bn(buf + off, (int)km.dSize, NULL);
    }
    rsa = RSA_new();
    k = EVP_PKEY_new();
    if (n == NULL || e == NULL || (km.dSize > 0 && d == NULL) || rsa == NULL || k == NULL ||
        RSA_set0_key(rsa, n, e, d) != 1) {
        BN_free(n);
        BN_free(e);
        BN_free(d);
        RSA_free(rsa);
        EVP_PKEY_free(k);
        return HKS_ERROR_INVALID_PRIVATE_KEY;
    }
    if (EVP_PKEY_assign_RSA(k, rsa) != 1) {
        RSA_free(rsa);
        EVP_PKEY_free(k);
        return HKS_ERROR_INVALID_PRIVATE_KEY;
    }
    *pkey = k;
    return HKS_SUCCESS;
}

static int32_t pkey_from_ecc_material(const uint8_t *buf, uint32_t len, EVP_PKEY **pkey)
{
    struct KeyMaterialEcc km;
    EC_KEY *ec = NULL;
    BIGNUM *x = NULL;
    BIGNUM *y = NULL;
    BIGNUM *z = NULL;
    EVP_PKEY *k = NULL;
    uint32_t off = (uint32_t)sizeof(km);

    if (len < sizeof(km)) {
        return HKS_ERROR_INVALID_PRIVATE_KEY;
    }
    (void)memcpy(&km, buf, sizeof(km));
    if (km.keyAlg != HKS_ALG_ECC || km.xSize == 0 || km.ySize == 0 ||
        km.xSize > len - off || km.ySize > len - off - km.xSize ||
        km.zSize != len - off - km.xSize - km.ySize) {
        return HKS_ERROR_INVALID_PRIVATE_KEY;
    }
    ec = EC_KEY_new_by_curve_name(ecc_nid_for_bits(km.keySize));
    x = BN_bin2bn(buf + off, (int)km.xSize, NULL);
    off += km.xSize;
    y = BN_bin2bn(buf + off, (int)km.ySize, NULL);
    off += km.ySize;
    if (km.zSize > 0) {
        z = BN_bin2bn(buf + off, (int)km.zSize, NULL);
    }
    k = EVP_PKEY_new();
    if (ec == NULL || x == NULL || y == NULL || (km.zSize > 0 && z == NULL) || k == NULL ||
        EC_KEY_set_public_key_affine_coordinates(ec, x, y) != 1 ||
        (z != NULL && EC_KEY_set_private_key(ec, z) != 1)) {
        BN_free(x);
        BN_free(y);
        BN_free(z);
        EC_KEY_free(ec);
        EVP_PKEY_free(k);
        return HKS_ERROR_INVALID_PRIVATE_KEY;
    }
    BN_free(x);
    BN_free(y);
    BN_free(z);
    if (EVP_PKEY_assign_EC_KEY(k, ec) != 1) {
        EC_KEY_free(ec);
        EVP_PKEY_free(k);
        return HKS_ERROR_INVALID_PRIVATE_KEY;
    }
    *pkey = k;
    return HKS_SUCCESS;
}

static int32_t pkey_of(const struct SoftKey *key, EVP_PKEY **pkey)
{
    const uint8_t *p = key->material;
    EVP_PKEY *k;

    if (key->material == NULL || key->matLen == 0) {
        return HKS_ERROR_INVALID_KEY_FILE;
    }
    k = d2i_AutoPrivateKey(NULL, &p, (long)key->matLen);
    if (k != NULL) {
        *pkey = k;
        return HKS_SUCCESS;
    }
    p = key->material;
    k = d2i_PUBKEY(NULL, &p, (long)key->matLen);
    if (k != NULL) {
        *pkey = k;
        return HKS_SUCCESS;
    }
    if (pkey_from_rsa_material(key->material, key->matLen, &k) == HKS_SUCCESS) {
        *pkey = k;
        return HKS_SUCCESS;
    }
    if (pkey_from_ecc_material(key->material, key->matLen, &k) == HKS_SUCCESS) {
        *pkey = k;
        return HKS_SUCCESS;
    }
    return HKS_ERROR_INVALID_PRIVATE_KEY;
}

static int32_t sign_evp(const struct SoftKey *key, const struct OpParams *op, const uint8_t *data,
    uint32_t dataLen, uint8_t **sig, uint32_t *sigLen)
{
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *mdCtx = NULL;
    EVP_PKEY_CTX *pctx = NULL;      /* owned by mdCtx after DigestSignInit */
    EVP_PKEY_CTX *ownedCtx = NULL;  /* created here for the raw path */
    uint8_t *buf = NULL;
    size_t len = 0;
    int32_t ret;

    ret = pkey_of(key, &pkey);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    mdCtx = EVP_MD_CTX_new();
    if (mdCtx == NULL) {
        EVP_PKEY_free(pkey);
        return HKS_ERROR_MALLOC_FAIL;
    }

    if (op->digest == HKS_DIGEST_NONE) {
        /* raw signature (input already digested) */
        ownedCtx = EVP_PKEY_CTX_new(pkey, NULL);
        if (ownedCtx == NULL || EVP_PKEY_sign_init(ownedCtx) != 1 ||
            (key->alg == HKS_ALG_RSA && EVP_PKEY_CTX_set_rsa_padding(ownedCtx, RSA_PKCS1_PADDING) != 1) ||
            EVP_PKEY_sign(ownedCtx, NULL, &len, data, dataLen) != 1) {
            ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
            goto out;
        }
        buf = malloc(len);
        if (buf == NULL) {
            ret = HKS_ERROR_MALLOC_FAIL;
            goto out;
        }
        if (EVP_PKEY_sign(ownedCtx, buf, &len, data, dataLen) != 1) {
            ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
            goto out;
        }
    } else {
        if (EVP_DigestSignInit(mdCtx, &pctx, md_of(op->digest), NULL, pkey) != 1) {
            ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
            goto out;
        }
        if (key->alg == HKS_ALG_RSA && op->padding == HKS_PADDING_PSS) {
            if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
                EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1) {
                ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
                goto out;
            }
        } else if (key->alg == HKS_ALG_RSA) {
            if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) != 1) {
                ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
                goto out;
            }
        }
        if (EVP_DigestSignUpdate(mdCtx, data, dataLen) != 1 ||
            EVP_DigestSignFinal(mdCtx, NULL, &len) != 1) {
            ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
            goto out;
        }
        buf = malloc(len);
        if (buf == NULL) {
            ret = HKS_ERROR_MALLOC_FAIL;
            goto out;
        }
        if (EVP_DigestSignFinal(mdCtx, buf, &len) != 1) {
            ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
            goto out;
        }
    }
    *sig = buf;
    *sigLen = (uint32_t)len;
    buf = NULL;
    ret = HKS_SUCCESS;

out:
    free(buf);
    EVP_PKEY_CTX_free(ownedCtx);
    EVP_MD_CTX_free(mdCtx);
    EVP_PKEY_free(pkey);
    return ret;
}

static int32_t verify_evp(const struct SoftKey *key, const struct OpParams *op, const uint8_t *data,
    uint32_t dataLen, const uint8_t *sig, uint32_t sigLen)
{
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *mdCtx = NULL;
    EVP_PKEY_CTX *pctx = NULL;      /* owned by mdCtx */
    EVP_PKEY_CTX *ownedCtx = NULL;  /* created here for the raw path */
    int ok;
    int32_t ret;

    ret = pkey_of(key, &pkey);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    mdCtx = EVP_MD_CTX_new();
    if (mdCtx == NULL) {
        EVP_PKEY_free(pkey);
        return HKS_ERROR_MALLOC_FAIL;
    }
    if (op->digest == HKS_DIGEST_NONE) {
        ownedCtx = EVP_PKEY_CTX_new(pkey, NULL);
        ok = (ownedCtx != NULL &&
            EVP_PKEY_verify_init(ownedCtx) == 1 &&
            (key->alg != HKS_ALG_RSA || EVP_PKEY_CTX_set_rsa_padding(ownedCtx, RSA_PKCS1_PADDING) == 1) &&
            EVP_PKEY_verify(ownedCtx, sig, sigLen, data, dataLen) == 1);
    } else {
        ok = (EVP_DigestVerifyInit(mdCtx, &pctx, md_of(op->digest), NULL, pkey) == 1);
        if (ok && key->alg == HKS_ALG_RSA && op->padding == HKS_PADDING_PSS) {
            ok = (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
                EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) == 1);
        } else if (ok && key->alg == HKS_ALG_RSA) {
            ok = (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) == 1);
        }
        if (ok) {
            ok = (EVP_DigestVerifyUpdate(mdCtx, data, dataLen) == 1);
        }
        if (ok) {
            ok = (EVP_DigestVerifyFinal(mdCtx, sig, sigLen) == 1);
        }
    }
    EVP_PKEY_CTX_free(ownedCtx);
    EVP_MD_CTX_free(mdCtx);
    EVP_PKEY_free(pkey);
    return ok ? HKS_SUCCESS : HKS_ERROR_VERIFICATION_FAILED;
}

static uint32_t aes_key_bits(const struct SoftKey *key, const struct OpParams *op)
{
    if (key->bits != 0) {
        return key->bits;
    }
    if (op != NULL && op->bits != 0) {
        return op->bits;
    }
    return key->matLen * 8u;
}

static const EVP_CIPHER *aes_cipher(uint32_t bits, uint32_t mode)
{
    if (mode == HKS_MODE_GCM) {
        switch (bits) {
            case 128: return EVP_aes_128_gcm();
            case 192: return EVP_aes_192_gcm();
            case 256: return EVP_aes_256_gcm();
            default: return NULL;
        }
    }
    if (mode == HKS_MODE_CBC) {
        switch (bits) {
            case 128: return EVP_aes_128_cbc();
            case 192: return EVP_aes_192_cbc();
            case 256: return EVP_aes_256_cbc();
            default: return NULL;
        }
    }
    if (mode == HKS_MODE_CTR) {
        switch (bits) {
            case 128: return EVP_aes_128_ctr();
            case 192: return EVP_aes_192_ctr();
            case 256: return EVP_aes_256_ctr();
            default: return NULL;
        }
    }
    if (mode == HKS_MODE_ECB) {
        switch (bits) {
            case 128: return EVP_aes_128_ecb();
            case 192: return EVP_aes_192_ecb();
            case 256: return EVP_aes_256_ecb();
            default: return NULL;
        }
    }
    return NULL;
}

static int32_t aes_gcm(const struct SoftKey *key, const struct OpParams *op, const uint8_t *in,
    uint32_t inLen, bool encrypt, uint8_t **out, uint32_t *outLen)
{
    EVP_CIPHER_CTX *ctx = NULL;
    uint32_t bits = aes_key_bits(key, op);
    const EVP_CIPHER *cipher = aes_cipher(bits, HKS_MODE_GCM);
    uint8_t *buf;
    int len = 0;
    int total = 0;
    int32_t ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;

    if (cipher == NULL || key->matLen != bits / 8u) {
        return HKS_ERROR_INVALID_KEY_SIZE;
    }
    if (op->iv == NULL || op->ivLen != 12u) {
        return HKS_ERROR_INVALID_NONCE;
    }
    if (inLen > MAX_BLOB) {
        return HKS_ERROR_INVALID_ARGUMENT;
    }
    buf = malloc((size_t)inLen + 16u + 1u);
    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        free(buf);
        return HKS_ERROR_MALLOC_FAIL;
    }
    if (encrypt) {
        if (EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)op->ivLen, NULL) != 1 ||
            EVP_EncryptInit_ex(ctx, NULL, NULL, key->material, op->iv) != 1) {
            goto out;
        }
        if (op->aadLen > 0 && op->aad != NULL &&
            EVP_EncryptUpdate(ctx, NULL, &len, op->aad, (int)op->aadLen) != 1) {
            goto out;
        }
        if (inLen > 0 && in != NULL && EVP_EncryptUpdate(ctx, buf, &len, in, (int)inLen) != 1) {
            goto out;
        }
        total = len;
        if (EVP_EncryptFinal_ex(ctx, buf + total, &len) != 1) {
            goto out;
        }
        total += len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, buf + total) != 1) {
            goto out;
        }
        *outLen = (uint32_t)total + 16u;
    } else {
        /* ciphertext || 16-byte tag (HUKS AE_TAG is optional) */
        const uint8_t *ct = in;
        uint32_t ctLen = inLen;
        const uint8_t *tag;
        uint8_t tagBuf[16];
        if (op->aeTag != NULL && op->aeTagLen == 16u) {
            (void)memcpy(tagBuf, op->aeTag, 16);
            tag = tagBuf;
        } else if (inLen >= 16u && in != NULL) {
            tag = in + inLen - 16u;
            ctLen = inLen - 16u;
        } else {
            ret = HKS_ERROR_INVALID_AE_TAG;
            goto out;
        }
        if (EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)op->ivLen, NULL) != 1 ||
            EVP_DecryptInit_ex(ctx, NULL, NULL, key->material, op->iv) != 1) {
            goto out;
        }
        if (op->aadLen > 0 && op->aad != NULL &&
            EVP_DecryptUpdate(ctx, NULL, &len, op->aad, (int)op->aadLen) != 1) {
            goto out;
        }
        if (ctLen > 0 && ct != NULL && EVP_DecryptUpdate(ctx, buf, &len, ct, (int)ctLen) != 1) {
            goto out;
        }
        total = len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) != 1 ||
            EVP_DecryptFinal_ex(ctx, buf + total, &len) != 1) {
            ret = HKS_ERROR_VERIFICATION_FAILED;
            goto out;
        }
        total += len;
        *outLen = (uint32_t)total;
    }
    *out = buf;
    buf = NULL;
    ret = HKS_SUCCESS;

out:
    EVP_CIPHER_CTX_free(ctx);
    free(buf);
    return ret;
}

static int32_t aes_block_cipher(const struct SoftKey *key, const struct OpParams *op, const uint8_t *in,
    uint32_t inLen, bool encrypt, uint8_t **out, uint32_t *outLen)
{
    EVP_CIPHER_CTX *ctx = NULL;
    uint32_t bits = aes_key_bits(key, op);
    const EVP_CIPHER *cipher = aes_cipher(bits, op->mode);
    int pad = (op->mode == HKS_MODE_CTR || op->padding == HKS_PADDING_NONE) ? 0 : 1;
    const uint8_t *iv = (op->mode == HKS_MODE_ECB) ? NULL : op->iv;
    uint8_t *buf;
    int len = 0;
    int total = 0;
    int32_t ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;

    if (cipher == NULL) {
        return (aes_cipher(256u, op->mode) == NULL) ? HKS_ERROR_INVALID_MODE : HKS_ERROR_INVALID_KEY_SIZE;
    }
    if (key->matLen != bits / 8u) {
        return HKS_ERROR_INVALID_KEY_SIZE;
    }
    if (op->mode != HKS_MODE_ECB && (op->iv == NULL || op->ivLen != 16u)) {
        return HKS_ERROR_INVALID_IV;
    }
    if (inLen > MAX_BLOB) {
        return HKS_ERROR_INVALID_ARGUMENT;
    }
    if (!encrypt && op->mode != HKS_MODE_CTR && pad == 0 && (inLen % 16u) != 0) {
        return HKS_ERROR_INVALID_ARGUMENT;
    }
    buf = malloc((size_t)inLen + 32u);
    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        free(buf);
        return HKS_ERROR_MALLOC_FAIL;
    }
    if (encrypt) {
        if (EVP_EncryptInit_ex(ctx, cipher, NULL, key->material, iv) != 1 ||
            EVP_CIPHER_CTX_set_padding(ctx, pad) != 1 ||
            (inLen > 0 && in != NULL && EVP_EncryptUpdate(ctx, buf, &len, in, (int)inLen) != 1)) {
            goto out;
        }
        total = len;
        if (EVP_EncryptFinal_ex(ctx, buf + total, &len) != 1) {
            goto out;
        }
        total += len;
        *outLen = (uint32_t)total;
    } else {
        if (EVP_DecryptInit_ex(ctx, cipher, NULL, key->material, iv) != 1 ||
            EVP_CIPHER_CTX_set_padding(ctx, pad) != 1 ||
            (inLen > 0 && in != NULL && EVP_DecryptUpdate(ctx, buf, &len, in, (int)inLen) != 1)) {
            goto out;
        }
        total = len;
        if (EVP_DecryptFinal_ex(ctx, buf + total, &len) != 1) {
            ret = HKS_ERROR_VERIFICATION_FAILED;
            goto out;
        }
        total += len;
        *outLen = (uint32_t)total;
    }
    *out = buf;
    buf = NULL;
    ret = HKS_SUCCESS;

out:
    EVP_CIPHER_CTX_free(ctx);
    free(buf);
    return ret;
}

static int32_t rsa_cipher(const struct SoftKey *key, const struct OpParams *op, const uint8_t *in,
    uint32_t inLen, bool encrypt, uint8_t **out, uint32_t *outLen)
{
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *ctx = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;
    int rsaPad = RSA_PKCS1_PADDING;
    int32_t ret;

    if (in == NULL && inLen > 0) {
        return HKS_ERROR_NULL_POINTER;
    }
    if (op->padding == HKS_PADDING_OAEP) {
        rsaPad = RSA_PKCS1_OAEP_PADDING;
    } else if (op->padding == HKS_PADDING_PKCS1_V1_5) {
        rsaPad = RSA_PKCS1_PADDING;
    } else if (op->padding == HKS_PADDING_NONE) {
        rsaPad = RSA_NO_PADDING;
    } else {
        return HKS_ERROR_NOT_SUPPORTED;
    }

    ret = pkey_of(key, &pkey);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    ctx = EVP_PKEY_CTX_new(pkey, NULL);
    if (ctx == NULL) {
        EVP_PKEY_free(pkey);
        return HKS_ERROR_MALLOC_FAIL;
    }
    ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
    if ((encrypt ? EVP_PKEY_encrypt_init(ctx) : EVP_PKEY_decrypt_init(ctx)) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(ctx, rsaPad) != 1) {
        goto out;
    }
    if (rsaPad == RSA_PKCS1_OAEP_PADDING && op->digest != HKS_DIGEST_NONE) {
        const EVP_MD *md = md_of(op->digest);
        if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, md) != 1 ||
            EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, md) != 1) {
            goto out;
        }
    }
    if ((encrypt ? EVP_PKEY_encrypt(ctx, NULL, &len, in, inLen) :
                   EVP_PKEY_decrypt(ctx, NULL, &len, in, inLen)) != 1 ||
        len == 0 || len > MAX_BLOB) {
        goto out;
    }
    buf = malloc(len);
    if (buf == NULL) {
        ret = HKS_ERROR_MALLOC_FAIL;
        goto out;
    }
    if ((encrypt ? EVP_PKEY_encrypt(ctx, buf, &len, in, inLen) :
                   EVP_PKEY_decrypt(ctx, buf, &len, in, inLen)) != 1) {
        goto out;
    }
    *out = buf;
    *outLen = (uint32_t)len;
    buf = NULL;
    ret = HKS_SUCCESS;

out:
    if (buf != NULL) {
        (void)OPENSSL_cleanse(buf, len);
        free(buf);
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ret;
}

static int32_t hmac_mac(const struct SoftKey *key, const struct OpParams *op, const uint8_t *data,
    uint32_t dataLen, uint8_t **out, uint32_t *outLen)
{
    const EVP_MD *md = md_of(op != NULL ? op->digest : HKS_DIGEST_SHA256);
    uint8_t *buf = malloc(EVP_MAX_MD_SIZE);
    unsigned int len = 0;
    const uint8_t *msg = (data != NULL) ? data : (const uint8_t *)"";

    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    if (HMAC(md, key->material, (int)key->matLen, msg, dataLen, buf, &len) == NULL) {
        free(buf);
        return HKS_ERROR_CRYPTO_ENGINE_ERROR;
    }
    *out = buf;
    *outLen = len;
    return HKS_SUCCESS;
}

static int32_t derive_key(const struct SoftKey *key, const struct OpParams *op, uint8_t **out, uint32_t *outLen)
{
    uint32_t len = (op->deriveBytes == 0) ? 32u : op->deriveBytes;
    uint32_t kdfAlg = (op->alg != 0) ? op->alg : key->alg;
    uint8_t *buf;
    const EVP_MD *md = md_of(op->digest);
    int32_t ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;

    if (len == 0 || len > 1024u) {
        return HKS_ERROR_INVALID_KEY_SIZE;
    }
    buf = malloc(len);
    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    if (kdfAlg == HKS_ALG_PBKDF2) {
        uint32_t iter = (op->iteration == 0) ? 10000u : op->iteration;
        if (op->salt == NULL || op->saltLen == 0) {
            free(buf);
            return HKS_ERROR_INVALID_SALT;
        }
        if (PKCS5_PBKDF2_HMAC((const char *)key->material, (int)key->matLen, op->salt, (int)op->saltLen,
                (int)iter, md, (int)len, buf) != 1) {
            free(buf);
            return HKS_ERROR_CRYPTO_ENGINE_ERROR;
        }
    } else {
        EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
        size_t outSize = len;
        if (pctx == NULL) {
            free(buf);
            return HKS_ERROR_MALLOC_FAIL;
        }
        if (EVP_PKEY_derive_init(pctx) != 1 ||
            EVP_PKEY_CTX_set_hkdf_md(pctx, md) != 1 ||
            EVP_PKEY_CTX_set1_hkdf_key(pctx, key->material, (int)key->matLen) != 1) {
            EVP_PKEY_CTX_free(pctx);
            free(buf);
            return HKS_ERROR_CRYPTO_ENGINE_ERROR;
        }
        if (op->salt != NULL && op->saltLen > 0 &&
            EVP_PKEY_CTX_set1_hkdf_salt(pctx, op->salt, (int)op->saltLen) != 1) {
            EVP_PKEY_CTX_free(pctx);
            free(buf);
            return HKS_ERROR_CRYPTO_ENGINE_ERROR;
        }
        if (op->info != NULL && op->infoLen > 0 &&
            EVP_PKEY_CTX_add1_hkdf_info(pctx, op->info, (int)op->infoLen) != 1) {
            EVP_PKEY_CTX_free(pctx);
            free(buf);
            return HKS_ERROR_CRYPTO_ENGINE_ERROR;
        }
        if (EVP_PKEY_derive(pctx, buf, &outSize) != 1) {
            EVP_PKEY_CTX_free(pctx);
            free(buf);
            return HKS_ERROR_CRYPTO_ENGINE_ERROR;
        }
        EVP_PKEY_CTX_free(pctx);
        len = (uint32_t)outSize;
    }
    *out = buf;
    *outLen = len;
    ret = HKS_SUCCESS;
    return ret;
}

/* ------------------------------------------------------------------ */
/* Key load / generate                                                 */
/* ------------------------------------------------------------------ */

static int32_t load_key(const struct HksBlob *blob, const struct HksParamSet *ps, struct SoftKey *key)
{
    uint8_t keyId[SOFT_HUKS_KEYID_LEN];
    uint8_t *plain = NULL;
    uint32_t plainLen = 0;
    int32_t ret;

    if (blob == NULL || blob->data == NULL || blob->size == 0) {
        return HKS_ERROR_NULL_POINTER;
    }
    if (soft_token_keyid(blob->data, blob->size, keyId)) {
        ret = soft_store_get(keyId, &plain, &plainLen);
        if (ret != HKS_SUCCESS) {
            return ret;
        }
        ret = record_parse(plain, plainLen, key);
        (void)OPENSSL_cleanse(plain, plainLen);
        free(plain);
        return ret;
    }
    if (blob->size >= sizeof(struct HksParamSet)) {
        const struct HksParamSet *kps = (const struct HksParamSet *)blob->data;
        if (kps->paramSetSize == blob->size) {
            uint32_t tokLen = 0;
            const uint8_t *tok = param_bytes(kps, HKS_TAG_KEY, &tokLen);
            if (tok != NULL && soft_token_keyid(tok, tokLen, keyId)) {
                ret = soft_store_get(keyId, &plain, &plainLen);
                if (ret != HKS_SUCCESS) {
                    return ret;
                }
                ret = record_parse(plain, plainLen, key);
                (void)OPENSSL_cleanse(plain, plainLen);
                free(plain);
                return ret;
            }
        }
    }
    /*
     * Not one of our tokens: accept it as raw key material.  This is what the
     * host self-test uses, and it keeps the engine usable from a caller that
     * stores the material itself.
     */
    (void)memset(key, 0, sizeof(*key));
    key->alg = param_uint(ps, HKS_TAG_ALGORITHM, HKS_ALG_AES);
    key->bits = param_uint(ps, HKS_TAG_KEY_SIZE, blob->size * 8u);
    key->material = malloc(blob->size);
    if (key->material == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    (void)memcpy(key->material, blob->data, blob->size);
    key->matLen = blob->size;
    return HKS_SUCCESS;
}

static int32_t store_key(const struct SoftKey *key, const uint8_t keyId[SOFT_HUKS_KEYID_LEN])
{
    uint8_t *rec = NULL;
    uint32_t recLen = 0;
    int32_t ret;

    ret = record_serialize(key, &rec, &recLen);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    ret = soft_store_put(keyId, rec, recLen);
    (void)OPENSSL_cleanse(rec, recLen);
    free(rec);
    return ret;
}

static int32_t generate_material(uint32_t alg, uint32_t bits, struct SoftKey *key)
{
    uint8_t *der = NULL;
    uint32_t derLen = 0;
    int32_t ret;

    key->alg = alg;
    key->bits = bits;
    switch (alg) {
        case HKS_ALG_RSA:
            if (bits != 2048u && bits != 3072u && bits != 4096u) {
                return HKS_ERROR_INVALID_KEY_SIZE;
            }
            ret = gen_asym(EVP_PKEY_RSA, bits, &der, &derLen);
            break;
        case HKS_ALG_ECC:
            if (bits != 256u) {
                return HKS_ERROR_INVALID_KEY_SIZE;
            }
            ret = gen_asym(EVP_PKEY_EC, bits, &der, &derLen);
            break;
        case HKS_ALG_AES:
            if (bits != 128u && bits != 256u) {
                return HKS_ERROR_INVALID_KEY_SIZE;
            }
            derLen = bits / 8u;
            der = malloc(derLen);
            ret = (der == NULL) ? HKS_ERROR_MALLOC_FAIL : HKS_SUCCESS;
            if (ret == HKS_SUCCESS && RAND_bytes(der, (int)derLen) != 1) {
                free(der);
                der = NULL;
                ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
            }
            break;
        case HKS_ALG_HKDF:
        case HKS_ALG_PBKDF2:
            if (bits == 0) {
                key->bits = 256;
            }
            if (key->bits % 8u != 0 || key->bits < 128u || key->bits > 1024u) {
                return HKS_ERROR_INVALID_KEY_SIZE;
            }
            derLen = key->bits / 8u;
            der = malloc(derLen);
            ret = (der == NULL) ? HKS_ERROR_MALLOC_FAIL : HKS_SUCCESS;
            if (ret == HKS_SUCCESS && RAND_bytes(der, (int)derLen) != 1) {
                free(der);
                der = NULL;
                ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
            }
            break;
        case HKS_ALG_HMAC:
            if (bits == 0) {
                key->bits = 256;
            }
            if (key->bits % 8u != 0 || key->bits < 64u || key->bits > 512u) {
                return HKS_ERROR_INVALID_KEY_SIZE;
            }
            derLen = key->bits / 8u;
            der = malloc(derLen);
            ret = (der == NULL) ? HKS_ERROR_MALLOC_FAIL : HKS_SUCCESS;
            if (ret == HKS_SUCCESS && RAND_bytes(der, (int)derLen) != 1) {
                free(der);
                der = NULL;
                ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;
            }
            break;
        default:
            return HKS_ERROR_NOT_SUPPORTED;
    }
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    key->material = der;
    key->matLen = derLen;
    return HKS_SUCCESS;
}

static bool backend_ok(enum SoftBackend backend, const char *op, int32_t *outRet)
{
    if (backend == SOFT_BACKEND_SOFT) {
        return true;
    }
    if (backend == SOFT_BACKEND_QSEE) {
        *outRet = soft_qsee_execute(op);
    } else {
        *outRet = soft_stub_execute(op);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Sessions (Init/Update/Finish)                                       */
/* ------------------------------------------------------------------ */

struct Session {
    int used;
    uint64_t handleId;
    uint64_t lastSeq;
    struct SoftKey key;
    struct OpParams op;
    uint8_t iv[64];
    uint8_t aad[1024];
    uint8_t aeTag[64];
    uint8_t salt[256];
    uint8_t info[512];
    EVP_CIPHER_CTX *cipherCtx;
    bool streamingActive;
    bool streamEncrypt;
    bool streamGcm;
    uint8_t *buf;
    size_t bufLen;
    size_t bufCap;
};

static struct Session gSessions[MAX_SESSIONS];
static pthread_mutex_t gSessionMutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t gNextHandleId = 1u;
static uint64_t gSessionSeq = 0u;

static struct Session *session_find_id(uint64_t id)
{
    uint32_t i;

    if (id == 0u) {
        return NULL;
    }
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (gSessions[i].used && gSessions[i].handleId == id) {
            return &gSessions[i];
        }
    }
    return NULL;
}

static struct Session *session_find(const uint8_t *handle, uint32_t len)
{
    uint64_t id = 0u;

    if (handle == NULL || len != sizeof(uint64_t)) {
        return NULL;
    }
    (void)memcpy(&id, handle, sizeof(id));
    return session_find_id(id);
}

static void session_reset(struct Session *s)
{
    if (s->cipherCtx != NULL) {
        EVP_CIPHER_CTX_free(s->cipherCtx);
        s->cipherCtx = NULL;
    }
    soft_key_free(&s->key);
    if (s->buf != NULL) {
        (void)OPENSSL_cleanse(s->buf, s->bufLen);
        free(s->buf);
    }
    (void)OPENSSL_cleanse(s->iv, sizeof(s->iv));
    (void)OPENSSL_cleanse(s->aeTag, sizeof(s->aeTag));
    (void)OPENSSL_cleanse(s->salt, sizeof(s->salt));
    (void)OPENSSL_cleanse(s->info, sizeof(s->info));
    (void)memset(s, 0, sizeof(*s));
}

static void op_params_copy_session(struct Session *s, const struct OpParams *src)
{
    s->op = *src;
    if (src->iv != NULL && src->ivLen > 0 && src->ivLen <= sizeof(s->iv)) {
        (void)memcpy(s->iv, src->iv, src->ivLen);
        s->op.iv = s->iv;
    } else {
        s->op.iv = NULL;
        s->op.ivLen = 0;
    }
    if (src->aad != NULL && src->aadLen > 0 && src->aadLen <= sizeof(s->aad)) {
        (void)memcpy(s->aad, src->aad, src->aadLen);
        s->op.aad = s->aad;
    } else {
        s->op.aad = NULL;
        s->op.aadLen = 0;
    }
    if (src->aeTag != NULL && src->aeTagLen > 0 && src->aeTagLen <= sizeof(s->aeTag)) {
        (void)memcpy(s->aeTag, src->aeTag, src->aeTagLen);
        s->op.aeTag = s->aeTag;
    } else {
        s->op.aeTag = NULL;
        s->op.aeTagLen = 0;
    }
    if (src->salt != NULL && src->saltLen > 0 && src->saltLen <= sizeof(s->salt)) {
        (void)memcpy(s->salt, src->salt, src->saltLen);
        s->op.salt = s->salt;
    } else {
        s->op.salt = NULL;
        s->op.saltLen = 0;
    }
    if (src->info != NULL && src->infoLen > 0 && src->infoLen <= sizeof(s->info)) {
        (void)memcpy(s->info, src->info, src->infoLen);
        s->op.info = s->info;
    } else {
        s->op.info = NULL;
        s->op.infoLen = 0;
    }
}

/* ------------------------------------------------------------------ */
/* vtable implementation                                               */
/* ------------------------------------------------------------------ */

static int32_t soft_module_init(void)
{
    int32_t ret = soft_store_ensure_ready();
    soft_huks_log("HUKS_BACKEND=soft op=module_init result=%d root=%s", ret,
        getenv("HUKS_SOFT_ROOT") != NULL ? getenv("HUKS_SOFT_ROOT") : "/data/service/el1/public/huks_soft");
    return ret;
}

static int32_t soft_module_destroy(void)
{
    uint32_t i;

    (void)pthread_mutex_lock(&gSessionMutex);
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (gSessions[i].used) {
            session_reset(&gSessions[i]);
        }
    }
    (void)pthread_mutex_unlock(&gSessionMutex);
    return HKS_SUCCESS;
}

static int32_t soft_refresh(void)
{
    return HKS_SUCCESS;
}

static int32_t soft_generate_key(const struct HksBlob *keyAlias, const struct HksParamSet *paramSet,
    const struct HksBlob *keyIn, struct HksBlob *keyOut)
{
    struct OpParams op;
    struct SoftKey key;
    uint8_t keyId[SOFT_HUKS_KEYID_LEN];
    uint8_t token[SOFT_HUKS_TOKEN_LEN];
    const char *reason = "";
    int32_t ret;

    (void)keyIn;
    parse_params(paramSet, &op);
    if (!backend_ok(soft_route("generate", op.alg, op.origin, &reason), "generate", &ret)) {
        return ret;
    }
    (void)memset(&key, 0, sizeof(key));
    ret = generate_material(op.alg, op.bits, &key);
    if (ret != HKS_SUCCESS) {
        soft_huks_log("HUKS_BACKEND=soft op=generate result=%d reason=unsupported_or_bad_size alg=%u bits=%u",
            ret, op.alg, op.bits);
        return ret;
    }
    if (keyAlias != NULL && keyAlias->data != NULL) {
        key.alias = malloc(keyAlias->size);
        if (key.alias == NULL) {
            soft_key_free(&key);
            return HKS_ERROR_MALLOC_FAIL;
        }
        (void)memcpy(key.alias, keyAlias->data, keyAlias->size);
        key.aliasLen = keyAlias->size;
    }
    key.flags = op.origin;
    if (RAND_bytes(keyId, (int)sizeof(keyId)) != 1) {
        soft_key_free(&key);
        return HKS_ERROR_CRYPTO_ENGINE_ERROR;
    }
    ret = store_key(&key, keyId);
    soft_key_free(&key);
    if (ret != HKS_SUCCESS) {
        soft_huks_log("HUKS_BACKEND=soft op=generate result=%d reason=store_failed", ret);
        return ret;
    }
    soft_make_token(keyId, token);
    return build_key_blob_paramset(paramSet, HKS_KEY_FLAG_GENERATE_KEY, token, keyOut);
}

static int32_t soft_import_key(const struct HksBlob *keyAlias, const struct HksBlob *key,
    const struct HksParamSet *paramSet, struct HksBlob *keyOut)
{
    struct OpParams op;
    struct SoftKey rec;
    uint8_t keyId[SOFT_HUKS_KEYID_LEN];
    uint8_t token[SOFT_HUKS_TOKEN_LEN];
    const char *reason = "";
    int32_t ret;

    parse_params(paramSet, &op);
    if (!backend_ok(soft_route("import", op.alg, op.origin, &reason), "import", &ret)) {
        return ret;
    }
    if (key == NULL || key->data == NULL || key->size == 0 || op.alg == 0) {
        return HKS_ERROR_INVALID_ARGUMENT;
    }
    (void)memset(&rec, 0, sizeof(rec));
    rec.alg = op.alg;
    rec.bits = (op.bits != 0) ? op.bits : key->size * 8u;
    rec.flags = SOFT_KEY_ORIGIN_IMPORTED;
    rec.material = malloc(key->size);
    if (rec.material == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    (void)memcpy(rec.material, key->data, key->size);
    rec.matLen = key->size;
    if (keyAlias != NULL && keyAlias->data != NULL) {
        rec.alias = malloc(keyAlias->size);
        if (rec.alias == NULL) {
            soft_key_free(&rec);
            return HKS_ERROR_MALLOC_FAIL;
        }
        (void)memcpy(rec.alias, keyAlias->data, keyAlias->size);
        rec.aliasLen = keyAlias->size;
    }
    /* asymmetric material must parse before it is sealed */
    if (rec.alg == HKS_ALG_RSA || rec.alg == HKS_ALG_ECC) {
        EVP_PKEY *pkey = NULL;
        if (pkey_of(&rec, &pkey) != HKS_SUCCESS) {
            soft_key_free(&rec);
            return HKS_ERROR_INVALID_PRIVATE_KEY;
        }
        EVP_PKEY_free(pkey);
    }
    if (RAND_bytes(keyId, (int)sizeof(keyId)) != 1) {
        soft_key_free(&rec);
        return HKS_ERROR_CRYPTO_ENGINE_ERROR;
    }
    ret = store_key(&rec, keyId);
    soft_key_free(&rec);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    soft_make_token(keyId, token);
    return build_key_blob_paramset(paramSet, HKS_KEY_FLAG_IMPORT_KEY, token, keyOut);
}

static int32_t soft_import_wrapped_key(const struct HksBlob *wrappingKeyAlias, const struct HksBlob *key,
    const struct HksBlob *wrappedKeyData, const struct HksParamSet *paramSet, struct HksBlob *keyOut)
{
    struct OpParams op;
    const char *reason = "";

    (void)wrappingKeyAlias;
    (void)key;
    (void)wrappedKeyData;
    (void)keyOut;
    parse_params(paramSet, &op);
    (void)soft_route("import_wrapped", op.alg, op.origin, &reason);
    /* Huawei wrap format; never silently half-implement it. */
    soft_huks_log("HUKS_BACKEND=soft op=import_wrapped result=%d reason=wrap_format_not_implemented",
        HKS_ERROR_NOT_SUPPORTED);
    return HKS_ERROR_NOT_SUPPORTED;
}

static int32_t export_rsa_material(EVP_PKEY *pkey, uint32_t bits, uint8_t **outBuf, uint32_t *outLen)
{
    const RSA *rsa = EVP_PKEY_get0_RSA(pkey);
    const BIGNUM *n = NULL;
    const BIGNUM *e = NULL;
    struct KeyMaterialRsa km;
    uint32_t nSize;
    uint32_t eSize;
    uint32_t total;
    uint8_t *buf;

    if (rsa == NULL) {
        return HKS_ERROR_INVALID_USAGE_OF_KEY;
    }
    RSA_get0_key(rsa, &n, &e, NULL);
    if (n == NULL || e == NULL) {
        return HKS_ERROR_CRYPTO_ENGINE_ERROR;
    }
    nSize = (uint32_t)BN_num_bytes(n);
    eSize = (uint32_t)BN_num_bytes(e);
    total = (uint32_t)sizeof(km) + nSize + eSize;
    buf = malloc(total);
    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    km = (struct KeyMaterialRsa){
        .keyAlg = HKS_ALG_RSA,
        .keySize = (bits != 0) ? bits : (uint32_t)BN_num_bits(n),
        .nSize = nSize,
        .eSize = eSize,
        .dSize = 0,
    };
    (void)memcpy(buf, &km, sizeof(km));
    (void)BN_bn2bin(n, buf + sizeof(km));
    (void)BN_bn2bin(e, buf + sizeof(km) + nSize);
    *outBuf = buf;
    *outLen = total;
    return HKS_SUCCESS;
}

static int32_t export_ecc_material(EVP_PKEY *pkey, uint32_t bits, uint8_t **outBuf, uint32_t *outLen)
{
    const EC_KEY *ec = EVP_PKEY_get0_EC_KEY(pkey);
    const EC_GROUP *grp;
    const EC_POINT *pt;
    BIGNUM *x = NULL;
    BIGNUM *y = NULL;
    struct KeyMaterialEcc km;
    uint32_t deg;
    uint32_t coordLen;
    uint32_t total;
    uint8_t *buf = NULL;
    int32_t ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;

    if (ec == NULL) {
        return HKS_ERROR_INVALID_USAGE_OF_KEY;
    }
    grp = EC_KEY_get0_group(ec);
    pt = EC_KEY_get0_public_key(ec);
    if (grp == NULL || pt == NULL) {
        return HKS_ERROR_CRYPTO_ENGINE_ERROR;
    }
    deg = (uint32_t)EC_GROUP_get_degree(grp);
    if (bits == 0) {
        bits = deg;
    }
    coordLen = (deg + 7u) / 8u;
    total = (uint32_t)sizeof(km) + 2u * coordLen;
    x = BN_new();
    y = BN_new();
    buf = malloc(total);
    if (x == NULL || y == NULL || buf == NULL) {
        ret = HKS_ERROR_MALLOC_FAIL;
        goto out;
    }
    if (EC_POINT_get_affine_coordinates(grp, pt, x, y, NULL) != 1 ||
        BN_bn2binpad(x, buf + sizeof(km), (int)coordLen) != (int)coordLen ||
        BN_bn2binpad(y, buf + sizeof(km) + coordLen, (int)coordLen) != (int)coordLen) {
        goto out;
    }
    km = (struct KeyMaterialEcc){
        .keyAlg = HKS_ALG_ECC,
        .keySize = bits,
        .xSize = coordLen,
        .ySize = coordLen,
        .zSize = 0,
    };
    (void)memcpy(buf, &km, sizeof(km));
    *outBuf = buf;
    *outLen = total;
    buf = NULL;
    ret = HKS_SUCCESS;

out:
    free(buf);
    BN_free(x);
    BN_free(y);
    return ret;
}

static int32_t soft_export_public_key(const struct HksBlob *key, const struct HksParamSet *paramSet,
    struct HksBlob *keyOut)
{
    struct SoftKey rec;
    EVP_PKEY *pkey = NULL;
    uint8_t *mat = NULL;
    uint32_t matLen = 0;
    int32_t ret;
    const char *reason = "";

    (void)soft_route("export_public_key", param_uint(paramSet, HKS_TAG_ALGORITHM, 0), 0, &reason);
    ret = load_key(key, paramSet, &rec);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    if (rec.alg != HKS_ALG_RSA && rec.alg != HKS_ALG_ECC) {
        soft_key_free(&rec);
        return HKS_ERROR_INVALID_USAGE_OF_KEY;
    }
    ret = pkey_of(&rec, &pkey);
    if (ret != HKS_SUCCESS) {
        soft_key_free(&rec);
        return ret;
    }
    if (rec.alg == HKS_ALG_RSA) {
        ret = export_rsa_material(pkey, rec.bits, &mat, &matLen);
    } else {
        ret = export_ecc_material(pkey, rec.bits, &mat, &matLen);
    }
    soft_key_free(&rec);
    EVP_PKEY_free(pkey);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    ret = blob_put(keyOut, mat, matLen);
    free(mat);
    return ret;
}

static int32_t session_init_stream_aes(struct Session *s)
{
    const struct SoftKey *key = &s->key;
    const struct OpParams *op = &s->op;
    bool encrypt = (op->purpose == HKS_KEY_PURPOSE_ENCRYPT);
    uint32_t bits = aes_key_bits(key, op);
    const EVP_CIPHER *cipher;
    EVP_CIPHER_CTX *ctx;
    int len = 0;

    if (key->alg != HKS_ALG_AES ||
        (op->purpose != HKS_KEY_PURPOSE_ENCRYPT && op->purpose != HKS_KEY_PURPOSE_DECRYPT)) {
        return HKS_SUCCESS;
    }
    cipher = aes_cipher(bits, op->mode);
    if (cipher == NULL) {
        return (aes_cipher(256u, op->mode) == NULL) ? HKS_ERROR_INVALID_MODE : HKS_ERROR_INVALID_KEY_SIZE;
    }
    if (key->matLen != bits / 8u) {
        return HKS_ERROR_INVALID_KEY_SIZE;
    }
    if (op->mode == HKS_MODE_GCM) {
        if (op->iv == NULL || op->ivLen != 12u) {
            return HKS_ERROR_INVALID_NONCE;
        }
        /* Wait for Finish if GCM decrypt omitted AE_TAG (trailing-tag or Finish-tag caller). */
        if (!encrypt && (op->aeTag == NULL || op->aeTagLen != 16u)) {
            return HKS_SUCCESS;
        }
        ctx = EVP_CIPHER_CTX_new();
        if (ctx == NULL) {
            return HKS_ERROR_MALLOC_FAIL;
        }
        if (encrypt) {
            if (EVP_EncryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1 ||
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)op->ivLen, NULL) != 1 ||
                EVP_EncryptInit_ex(ctx, NULL, NULL, key->material, op->iv) != 1 ||
                (op->aadLen > 0 && op->aad != NULL &&
                 EVP_EncryptUpdate(ctx, NULL, &len, op->aad, (int)op->aadLen) != 1)) {
                EVP_CIPHER_CTX_free(ctx);
                return HKS_ERROR_CRYPTO_ENGINE_ERROR;
            }
        } else {
            if (EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) != 1 ||
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)op->ivLen, NULL) != 1 ||
                EVP_DecryptInit_ex(ctx, NULL, NULL, key->material, op->iv) != 1 ||
                (op->aadLen > 0 && op->aad != NULL &&
                 EVP_DecryptUpdate(ctx, NULL, &len, op->aad, (int)op->aadLen) != 1)) {
                EVP_CIPHER_CTX_free(ctx);
                return HKS_ERROR_CRYPTO_ENGINE_ERROR;
            }
        }
        s->cipherCtx = ctx;
        s->streamingActive = true;
        s->streamEncrypt = encrypt;
        s->streamGcm = true;
        return HKS_SUCCESS;
    }
    if (op->mode == HKS_MODE_CBC || op->mode == HKS_MODE_CTR || op->mode == HKS_MODE_ECB) {
        int pad = (op->mode == HKS_MODE_CTR || op->padding == HKS_PADDING_NONE) ? 0 : 1;
        const uint8_t *iv = (op->mode == HKS_MODE_ECB) ? NULL : op->iv;
        if (op->mode != HKS_MODE_ECB && (op->iv == NULL || op->ivLen != 16u)) {
            return HKS_ERROR_INVALID_IV;
        }
        ctx = EVP_CIPHER_CTX_new();
        if (ctx == NULL) {
            return HKS_ERROR_MALLOC_FAIL;
        }
        if ((encrypt ? EVP_EncryptInit_ex(ctx, cipher, NULL, key->material, iv) :
                       EVP_DecryptInit_ex(ctx, cipher, NULL, key->material, iv)) != 1 ||
            EVP_CIPHER_CTX_set_padding(ctx, pad) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return HKS_ERROR_CRYPTO_ENGINE_ERROR;
        }
        s->cipherCtx = ctx;
        s->streamingActive = true;
        s->streamEncrypt = encrypt;
        s->streamGcm = false;
        return HKS_SUCCESS;
    }
    return HKS_ERROR_INVALID_MODE;
}

static int32_t soft_init(const struct HksBlob *key, const struct HksParamSet *paramSet, struct HksBlob *handle,
    struct HksBlob *token)
{
    struct OpParams op;
    struct SoftKey rec;
    struct Session *slot = NULL;
    uint64_t handleVal = 0u;
    uint32_t i;
    int32_t ret;
    const char *reason = "";

    if (handle == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    if (handle->data != NULL && handle->size < sizeof(uint64_t)) {
        return HKS_ERROR_BUFFER_TOO_SMALL;
    }
    parse_params(paramSet, &op);
    if (!backend_ok(soft_route("init", op.alg, op.origin, &reason), "init", &ret)) {
        return ret;
    }
    ret = load_key(key, paramSet, &rec);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    if (op.alg == 0) {
        op.alg = rec.alg;
    }
    if (op.bits == 0) {
        op.bits = rec.bits;
    }

    (void)pthread_mutex_lock(&gSessionMutex);
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (!gSessions[i].used) {
            slot = &gSessions[i];
            break;
        }
    }
    if (slot == NULL) {
        /* Evict the least-recently-used session so caller churn cannot wedge HUKS with -39. */
        slot = &gSessions[0];
        for (i = 1; i < MAX_SESSIONS; i++) {
            if (gSessions[i].lastSeq < slot->lastSeq) {
                slot = &gSessions[i];
            }
        }
        soft_huks_log("HUKS_BACKEND=soft op=init evict_handle=%llu slots=%d",
            (unsigned long long)slot->handleId, MAX_SESSIONS);
        session_reset(slot);
    }
    do {
        if (gNextHandleId == 0u) {
            gNextHandleId = 1u;
        }
        handleVal = (uint64_t)(gNextHandleId++);
    } while (session_find_id(handleVal) != NULL);

    (void)memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->handleId = handleVal;
    slot->lastSeq = ++gSessionSeq;
    slot->key = rec;
    op_params_copy_session(slot, &op);
    ret = session_init_stream_aes(slot);
    if (ret != HKS_SUCCESS) {
        session_reset(slot);
        (void)pthread_mutex_unlock(&gSessionMutex);
        return ret;
    }
    (void)pthread_mutex_unlock(&gSessionMutex);

    if (token != NULL) {
        token->size = 0;
    }
    ret = blob_put(handle, (const uint8_t *)&handleVal, sizeof(handleVal));
    if (ret != HKS_SUCCESS) {
        (void)pthread_mutex_lock(&gSessionMutex);
        slot = session_find_id(handleVal);
        if (slot != NULL) {
            session_reset(slot);
        }
        (void)pthread_mutex_unlock(&gSessionMutex);
    }
    return ret;
}

static int32_t session_append(struct Session *s, const uint8_t *data, uint32_t len)
{
    size_t need;

    if (len == 0) {
        return HKS_SUCCESS;
    }
    if (data == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    need = s->bufLen + len;
    if (need > MAX_BLOB) {
        return HKS_ERROR_INVALID_ARGUMENT;
    }
    if (need > s->bufCap) {
        size_t cap = (s->bufCap == 0) ? 4096u : s->bufCap;
        uint8_t *nb;
        while (cap < need) {
            cap *= 2u;
        }
        nb = realloc(s->buf, cap);
        if (nb == NULL) {
            return HKS_ERROR_MALLOC_FAIL;
        }
        s->buf = nb;
        s->bufCap = cap;
    }
    (void)memcpy(s->buf + s->bufLen, data, len);
    s->bufLen += len;
    return HKS_SUCCESS;
}

static int32_t soft_update(const struct HksBlob *handle, const struct HksParamSet *paramSet,
    const struct HksBlob *inData, struct HksBlob *outData)
{
    struct Session *s;
    const uint8_t *inPtr = (inData != NULL) ? inData->data : NULL;
    uint32_t inLen = (inData != NULL) ? inData->size : 0u;
    int32_t ret = HKS_SUCCESS;

    (void)paramSet;
    if (handle == NULL || handle->data == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    if (inLen > 0 && inPtr == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    (void)pthread_mutex_lock(&gSessionMutex);
    s = session_find(handle->data, handle->size);
    if (s == NULL) {
        (void)pthread_mutex_unlock(&gSessionMutex);
        return HKS_ERROR_NOT_EXIST;
    }
    s->lastSeq = ++gSessionSeq;
    if (s->streamingActive) {
        if (inLen == 0) {
            if (outData != NULL) {
                outData->size = 0;
            }
        } else {
            uint8_t *tmp = malloc((size_t)inLen + 32u);
            int outLen = 0;
            if (tmp == NULL) {
                (void)pthread_mutex_unlock(&gSessionMutex);
                return HKS_ERROR_MALLOC_FAIL;
            }
            if ((s->streamEncrypt ?
                 EVP_EncryptUpdate(s->cipherCtx, tmp, &outLen, inPtr, (int)inLen) :
                 EVP_DecryptUpdate(s->cipherCtx, tmp, &outLen, inPtr, (int)inLen)) != 1) {
                (void)OPENSSL_cleanse(tmp, (size_t)inLen + 32u);
                free(tmp);
                (void)pthread_mutex_unlock(&gSessionMutex);
                return HKS_ERROR_CRYPTO_ENGINE_ERROR;
            }
            if (outData != NULL) {
                ret = blob_put(outData, tmp, (uint32_t)outLen);
            }
            (void)OPENSSL_cleanse(tmp, (size_t)inLen + 32u);
            free(tmp);
        }
        (void)pthread_mutex_unlock(&gSessionMutex);
        return ret;
    }
    ret = session_append(s, inPtr, inLen);
    (void)pthread_mutex_unlock(&gSessionMutex);
    if (ret == HKS_SUCCESS && outData != NULL) {
        outData->size = 0;
    }
    return ret;
}

static int32_t one_shot(const struct SoftKey *rec, const struct OpParams *op, const uint8_t *in, uint32_t inLen,
    const uint8_t *sig, uint32_t sigLen, struct HksBlob *out)
{
    uint8_t *buf = NULL;
    uint32_t bufLen = 0;
    int32_t ret;

    switch (op->purpose) {
        case HKS_KEY_PURPOSE_SIGN:
            ret = sign_evp(rec, op, in, inLen, &buf, &bufLen);
            break;
        case HKS_KEY_PURPOSE_VERIFY:
            ret = verify_evp(rec, op, in, inLen, sig, sigLen);
            if (ret == HKS_SUCCESS && out != NULL) {
                out->size = 0;
            }
            return ret;
        case HKS_KEY_PURPOSE_ENCRYPT:
            if (rec->alg == HKS_ALG_RSA) {
                ret = rsa_cipher(rec, op, in, inLen, true, &buf, &bufLen);
            } else if (rec->alg == HKS_ALG_AES) {
                ret = (op->mode == HKS_MODE_GCM) ? aes_gcm(rec, op, in, inLen, true, &buf, &bufLen) :
                    aes_block_cipher(rec, op, in, inLen, true, &buf, &bufLen);
            } else {
                return HKS_ERROR_INVALID_USAGE_OF_KEY;
            }
            break;
        case HKS_KEY_PURPOSE_DECRYPT:
            if (rec->alg == HKS_ALG_RSA) {
                ret = rsa_cipher(rec, op, in, inLen, false, &buf, &bufLen);
            } else if (rec->alg == HKS_ALG_AES) {
                ret = (op->mode == HKS_MODE_GCM) ? aes_gcm(rec, op, in, inLen, false, &buf, &bufLen) :
                    aes_block_cipher(rec, op, in, inLen, false, &buf, &bufLen);
            } else {
                return HKS_ERROR_INVALID_USAGE_OF_KEY;
            }
            break;
        case HKS_KEY_PURPOSE_MAC:
            ret = hmac_mac(rec, op, in, inLen, &buf, &bufLen);
            break;
        case HKS_KEY_PURPOSE_DERIVE:
            ret = derive_key(rec, op, &buf, &bufLen);
            break;
        default:
            return HKS_ERROR_INVALID_PURPOSE;
    }
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    ret = blob_put(out, buf, bufLen);
    if (buf != NULL) {
        (void)OPENSSL_cleanse(buf, bufLen);
        free(buf);
    }
    return ret;
}

static bool derive_needs_storage(const struct HksParamSet *ps)
{
    uint32_t sf;
    uint32_t dsf;

    if (ps == NULL) {
        return false;
    }
    sf = param_uint(ps, HKS_TAG_KEY_STORAGE_FLAG, UINT32_MAX);
    dsf = param_uint(ps, HKS_TAG_DERIVE_AGREE_KEY_STORAGE_FLAG, UINT32_MAX);
    return (sf == HKS_STORAGE_PERSISTENT || dsf == HKS_STORAGE_ONLY_USED_IN_HUKS);
}

static int32_t store_derived_key(const struct HksParamSet *finishPs, const uint8_t *raw, uint32_t rawLen,
    uint32_t origin, struct HksBlob *keyOut)
{
    struct SoftKey drec;
    uint8_t keyId[SOFT_HUKS_KEYID_LEN];
    uint8_t token[SOFT_HUKS_TOKEN_LEN];
    uint32_t aliasLen = 0;
    const uint8_t *aliasPtr = param_bytes(finishPs, HKS_TAG_KEY_ALIAS, &aliasLen);
    uint32_t targetAlg = param_uint(finishPs, HKS_TAG_ALGORITHM, HKS_ALG_AES);
    uint32_t targetBits = param_uint(finishPs, HKS_TAG_KEY_SIZE, rawLen * 8u);
    int32_t ret;

    (void)memset(&drec, 0, sizeof(drec));
    drec.alg = (targetAlg != 0) ? targetAlg : HKS_ALG_AES;
    drec.bits = (targetBits != 0) ? targetBits : rawLen * 8u;
    drec.flags = origin;
    drec.matLen = rawLen;
    drec.material = malloc(rawLen);
    if (drec.material == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    (void)memcpy(drec.material, raw, rawLen);
    if (aliasPtr != NULL && aliasLen > 0) {
        drec.alias = malloc(aliasLen);
        if (drec.alias == NULL) {
            soft_key_free(&drec);
            return HKS_ERROR_MALLOC_FAIL;
        }
        (void)memcpy(drec.alias, aliasPtr, aliasLen);
        drec.aliasLen = aliasLen;
    }
    if (RAND_bytes(keyId, (int)sizeof(keyId)) != 1) {
        soft_key_free(&drec);
        return HKS_ERROR_CRYPTO_ENGINE_ERROR;
    }
    ret = store_key(&drec, keyId);
    soft_key_free(&drec);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    soft_make_token(keyId, token);
    return build_key_blob_paramset(finishPs, HKS_KEY_FLAG_DERIVE_KEY, token, keyOut);
}

static int32_t finish_stream_aes(struct Session *s, const struct OpParams *op, const uint8_t *inPtr, uint32_t inLen,
    struct HksBlob *outData)
{
    size_t maxOut = (size_t)inLen + 48u;
    uint8_t *tmp = malloc(maxOut);
    int len = 0;
    int total = 0;
    int32_t ret = HKS_ERROR_CRYPTO_ENGINE_ERROR;

    if (tmp == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    if (s->streamEncrypt) {
        if (inLen > 0 && EVP_EncryptUpdate(s->cipherCtx, tmp, &len, inPtr, (int)inLen) != 1) {
            goto out;
        }
        total = len;
        if (EVP_EncryptFinal_ex(s->cipherCtx, tmp + total, &len) != 1) {
            goto out;
        }
        total += len;
        if (s->streamGcm) {
            if (EVP_CIPHER_CTX_ctrl(s->cipherCtx, EVP_CTRL_GCM_GET_TAG, 16, tmp + total) != 1) {
                goto out;
            }
            total += 16;
        }
    } else {
        if (s->streamGcm) {
            if (op->aeTag == NULL || op->aeTagLen != 16u) {
                ret = HKS_ERROR_INVALID_AE_TAG;
                goto out;
            }
            if (inLen > 0 && EVP_DecryptUpdate(s->cipherCtx, tmp, &len, inPtr, (int)inLen) != 1) {
                goto out;
            }
            total = len;
            if (EVP_CIPHER_CTX_ctrl(s->cipherCtx, EVP_CTRL_GCM_SET_TAG, 16, (void *)op->aeTag) != 1 ||
                EVP_DecryptFinal_ex(s->cipherCtx, tmp + total, &len) != 1) {
                ret = HKS_ERROR_VERIFICATION_FAILED;
                goto out;
            }
            total += len;
        } else {
            if (inLen > 0 && EVP_DecryptUpdate(s->cipherCtx, tmp, &len, inPtr, (int)inLen) != 1) {
                goto out;
            }
            total = len;
            if (EVP_DecryptFinal_ex(s->cipherCtx, tmp + total, &len) != 1) {
                ret = HKS_ERROR_VERIFICATION_FAILED;
                goto out;
            }
            total += len;
        }
    }
    ret = blob_put(outData, tmp, (uint32_t)total);

out:
    (void)OPENSSL_cleanse(tmp, maxOut);
    free(tmp);
    return ret;
}

static int32_t soft_finish(const struct HksBlob *handle, const struct HksParamSet *paramSet,
    const struct HksBlob *inData, struct HksBlob *outData)
{
    struct Session *s;
    struct OpParams op;
    const uint8_t *inPtr = (inData != NULL) ? inData->data : NULL;
    uint32_t inLen = (inData != NULL) ? inData->size : 0u;
    int32_t ret;

    if (handle == NULL || handle->data == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    if (inLen > 0 && inPtr == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    (void)pthread_mutex_lock(&gSessionMutex);
    s = session_find(handle->data, handle->size);
    if (s == NULL) {
        (void)pthread_mutex_unlock(&gSessionMutex);
        return HKS_ERROR_NOT_EXIST;
    }
    merge_finish_params(paramSet, &op, &s->op);
    if (s->streamingActive) {
        ret = finish_stream_aes(s, &op, inPtr, inLen, outData);
    } else if (op.purpose == HKS_KEY_PURPOSE_VERIFY) {
        ret = one_shot(&s->key, &op, s->buf, (uint32_t)s->bufLen, inPtr, inLen, outData);
    } else if (op.purpose == HKS_KEY_PURPOSE_DERIVE) {
        uint8_t *derived = NULL;
        uint32_t derivedLen = 0;
        ret = session_append(s, inPtr, inLen);
        if (ret == HKS_SUCCESS) {
            ret = derive_key(&s->key, &op, &derived, &derivedLen);
        }
        if (ret == HKS_SUCCESS) {
            if (derive_needs_storage(paramSet)) {
                ret = store_derived_key(paramSet, derived, derivedLen, op.origin, outData);
            } else {
                ret = blob_put(outData, derived, derivedLen);
            }
        }
        if (derived != NULL) {
            (void)OPENSSL_cleanse(derived, derivedLen);
            free(derived);
        }
    } else {
        ret = session_append(s, inPtr, inLen);
        if (ret == HKS_SUCCESS) {
            ret = one_shot(&s->key, &op, s->buf, (uint32_t)s->bufLen, NULL, 0, outData);
        }
    }
    session_reset(s);
    (void)pthread_mutex_unlock(&gSessionMutex);
    return ret;
}

static int32_t soft_abort(const struct HksBlob *handle, const struct HksParamSet *paramSet)
{
    struct Session *s;

    (void)paramSet;
    if (handle == NULL || handle->data == NULL) {
        return HKS_ERROR_NULL_POINTER;
    }
    (void)pthread_mutex_lock(&gSessionMutex);
    s = session_find(handle->data, handle->size);
    if (s != NULL) {
        session_reset(s);
    }
    (void)pthread_mutex_unlock(&gSessionMutex);
    return HKS_SUCCESS;
}

static int32_t soft_one_shot_op(const char *opName, const struct HksBlob *key, const struct HksParamSet *paramSet,
    const struct HksBlob *in, const struct HksBlob *sig, struct HksBlob *out)
{
    struct OpParams op;
    struct SoftKey rec;
    const char *reason = "";
    int32_t ret;

    parse_params(paramSet, &op);
    if (!backend_ok(soft_route(opName, op.alg, op.origin, &reason), opName, &ret)) {
        return ret;
    }
    ret = load_key(key, paramSet, &rec);
    if (ret != HKS_SUCCESS) {
        soft_huks_log("HUKS_BACKEND=soft op=%s result=%d reason=key_not_found", opName, ret);
        return ret;
    }
    ret = one_shot(&rec, &op, in != NULL ? in->data : NULL, in != NULL ? in->size : 0,
        sig != NULL ? sig->data : NULL, sig != NULL ? sig->size : 0, out);
    if (ret != HKS_SUCCESS) {
        soft_huks_log("HUKS_BACKEND=soft op=%s result=%d reason=crypto_failed alg=%u purpose=%u", opName, ret,
            rec.alg, op.purpose);
    }
    soft_key_free(&rec);
    return ret;
}

static int32_t soft_sign(const struct HksBlob *key, const struct HksParamSet *paramSet, const struct HksBlob *srcData,
    struct HksBlob *signature)
{
    return soft_one_shot_op("sign", key, paramSet, srcData, NULL, signature);
}

static int32_t soft_verify(const struct HksBlob *key, const struct HksParamSet *paramSet, const struct HksBlob *srcData,
    const struct HksBlob *signature)
{
    struct HksBlob dummy = { 0, NULL };
    return soft_one_shot_op("verify", key, paramSet, srcData, signature, &dummy);
}

static int32_t soft_encrypt(const struct HksBlob *key, const struct HksParamSet *paramSet,
    const struct HksBlob *plainText, struct HksBlob *cipherText)
{
    return soft_one_shot_op("encrypt", key, paramSet, plainText, NULL, cipherText);
}

static int32_t soft_decrypt(const struct HksBlob *key, const struct HksParamSet *paramSet,
    const struct HksBlob *cipherText, struct HksBlob *plainText)
{
    return soft_one_shot_op("decrypt", key, paramSet, cipherText, NULL, plainText);
}

static int32_t soft_mac(const struct HksBlob *key, const struct HksParamSet *paramSet, const struct HksBlob *srcData,
    struct HksBlob *mac)
{
    return soft_one_shot_op("mac", key, paramSet, srcData, NULL, mac);
}

static int32_t soft_derive(const struct HksParamSet *paramSet, const struct HksBlob *kdfKey, struct HksBlob *derivedKey)
{
    struct OpParams op;
    struct SoftKey rec;
    uint8_t *buf = NULL;
    uint32_t bufLen = 0;
    const char *reason = "";
    int32_t ret;

    parse_params(paramSet, &op);
    if (!backend_ok(soft_route("derive", op.alg, op.origin, &reason), "derive", &ret)) {
        return ret;
    }
    ret = load_key(kdfKey, paramSet, &rec);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    ret = derive_key(&rec, &op, &buf, &bufLen);
    soft_key_free(&rec);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    ret = blob_put(derivedKey, buf, bufLen);
    (void)OPENSSL_cleanse(buf, bufLen);
    free(buf);
    return ret;
}

static int32_t soft_agree_key(const struct HksParamSet *paramSet, const struct HksBlob *privateKey,
    const struct HksBlob *peerPublicKey, struct HksBlob *agreedKey)
{
    struct OpParams op;
    const char *reason = "";

    (void)privateKey;
    (void)peerPublicKey;
    (void)agreedKey;
    parse_params(paramSet, &op);
    (void)soft_route("agree", op.alg, op.origin, &reason);
    soft_huks_log("HUKS_BACKEND=soft op=agree result=%d reason=ecdh_not_implemented", HKS_ERROR_NOT_SUPPORTED);
    return HKS_ERROR_NOT_SUPPORTED;
}

static int32_t soft_check_key_validity(const struct HksParamSet *paramSet, const struct HksBlob *key)
{
    struct SoftKey rec;
    int32_t ret;
    const char *reason = "";

    (void)soft_route("check_validity", param_uint(paramSet, HKS_TAG_ALGORITHM, 0), 0, &reason);
    ret = load_key(key, paramSet, &rec);
    if (ret != HKS_SUCCESS) {
        return ret;
    }
    soft_key_free(&rec);
    return HKS_SUCCESS;
}

static int32_t soft_get_key_properties(const struct HksParamSet *paramSet, const struct HksBlob *key)
{
    return soft_check_key_validity(paramSet, key);
}

static int32_t soft_attest_key(const struct HksBlob *key, const struct HksParamSet *paramSet,
    struct HksBlob *certChain)
{
    (void)key;
    (void)paramSet;
    (void)certChain;
    /* No hardware attestation chain exists on this port. Return success and
     * leave certChain untouched: HksServiceAttestKey keeps certChain->size as
     * the caller-provided capacity, then DcmGenerateCertChainInAttestKey()
     * passes that same buffer to DcmAnonymousAttestKey(), which fills it with
     * the local DCM chain. Any backend_ok()/soft_route("attest") gate here
     * would short-circuit to NOT_SUPPORTED before DCM is reached. */
    soft_huks_log("HUKS_BACKEND=stub op=attest result=0 reason=local_attest_deferred_to_dcm");
    return HKS_SUCCESS;
}

static int32_t soft_generate_random(const struct HksParamSet *paramSet, struct HksBlob *random)
{
    uint32_t len;
    uint32_t bits;
    uint8_t *buf;
    const char *reason = "";

    (void)soft_route("random", 0, 0, &reason);
    bits = param_uint(paramSet, HKS_TAG_KEY_SIZE, 0);
    len = (random != NULL && random->size > 0) ? random->size : ((bits > 0) ? bits / 8u : 32u);
    if (len == 0 || len > 1024u) {
        return HKS_ERROR_INVALID_KEY_SIZE;
    }
    buf = malloc(len);
    if (buf == NULL) {
        return HKS_ERROR_MALLOC_FAIL;
    }
    if (RAND_bytes(buf, (int)len) != 1) {
        free(buf);
        return HKS_ERROR_CRYPTO_ENGINE_ERROR;
    }
    int32_t ret = blob_put(random, buf, len);
    free(buf);
    return ret;
}

static int32_t soft_upgrade_key(const struct HksBlob *oldKey, const struct HksParamSet *paramSet,
    struct HksBlob *newKey)
{
    struct OpParams op;
    const char *reason = "";

    parse_params(paramSet, &op);
    (void)soft_route("upgrade", op.alg, op.origin, &reason);
    /* Our record format is versioned; nothing to migrate yet. */
    return blob_put(newKey, oldKey->data, oldKey->size);
}

static int32_t soft_unsupported(const char *op)
{
    soft_huks_log("HUKS_BACKEND=soft op=%s result=%d reason=not_implemented", op, HKS_ERROR_NOT_SUPPORTED);
    return HKS_ERROR_NOT_SUPPORTED;
}

static int32_t soft_get_ability(int32_t funcType)
{
    (void)funcType;
    return HKS_SUCCESS;
}

static int32_t soft_get_hardware_info(void)
{
    return HKS_ERROR_NOT_SUPPORTED;
}

static int32_t soft_calc_mac_header(const struct HksParamSet *paramSet, const struct HksBlob *salt,
    const struct HksBlob *srcData, struct HksBlob *mac)
{
    (void)paramSet;
    (void)salt;
    (void)srcData;
    (void)mac;
    return soft_unsupported("calc_mac_header");
}

static int32_t soft_upgrade_key_info(const struct HksBlob *keyAlias, const struct HksBlob *keyInfo,
    struct HksBlob *keyOut)
{
    (void)keyAlias;
    (void)keyInfo;
    (void)keyOut;
    return soft_unsupported("upgrade_key_info");
}

static int32_t soft_export_chipset_public_key(const struct HksBlob *salt,
    enum HksChipsetPlatformDecryptScene scene, struct HksBlob *publicKey)
{
    (void)salt;
    (void)scene;
    (void)publicKey;
    return soft_unsupported("export_chipset_public_key");
}

static int32_t soft_get_error_info(struct HksBlob *errorInfo)
{
    static const char msg[] = "soft-huks: no hardware attestation on this port";
    if (errorInfo != NULL) {
        errorInfo->size = 0;
    }
    soft_huks_log("%s", msg);
    return HKS_SUCCESS;
}

static int32_t soft_get_stat_info(struct HksBlob *statInfo)
{
    if (statInfo != NULL) {
        statInfo->size = 0;
    }
    return HKS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

static struct HuksHdi gEngine = {
    .HuksHdiModuleInit = soft_module_init,
    .HuksHdiModuleDestroy = soft_module_destroy,
    .HuksHdiRefresh = soft_refresh,
    .HuksHdiGenerateKey = soft_generate_key,
    .HuksHdiImportKey = soft_import_key,
    .HuksHdiImportWrappedKey = soft_import_wrapped_key,
    .HuksHdiExportPublicKey = soft_export_public_key,
    .HuksHdiInit = soft_init,
    .HuksHdiUpdate = soft_update,
    .HuksHdiFinish = soft_finish,
    .HuksHdiAbort = soft_abort,
    .HuksHdiGetKeyProperties = soft_get_key_properties,
    .HuksHdiAttestKey = soft_attest_key,
    .HuksHdiGetAbility = soft_get_ability,
    .HuksHdiGetHardwareInfo = soft_get_hardware_info,
    .HuksHdiCalcMacHeader = soft_calc_mac_header,
    .HuksHdiUpgradeKeyInfo = soft_upgrade_key_info,
    .HuksHdiGenerateRandom = soft_generate_random,
    .HuksHdiSign = soft_sign,
    .HuksHdiVerify = soft_verify,
    .HuksHdiEncrypt = soft_encrypt,
    .HuksHdiDecrypt = soft_decrypt,
    .HuksHdiAgreeKey = soft_agree_key,
    .HuksHdiDeriveKey = soft_derive,
    .HuksHdiMac = soft_mac,
    .HuksHdiUpgradeKey = soft_upgrade_key,
    .HuksHdiExportChipsetPlatformPublicKey = soft_export_chipset_public_key,
    .HuksHdiGetErrorInfo = soft_get_error_info,
    .HuksHdiGetStatInfo = soft_get_stat_info,
};

SOFT_HUKS_EXPORT struct HuksHdi *HuksCreateHdiDevicePtr(void)
{
    soft_huks_log("HUKS_BACKEND=soft op=engine_create result=0 note=software_backend_no_hardware_isolation");
    return &gEngine;
}

SOFT_HUKS_EXPORT void HuksDestoryHdiDevicePtr(struct HuksHdi *hdiDevicePtr)
{
    (void)hdiDevicePtr;
}

_Static_assert(sizeof(struct HuksHdi) == SOFT_HUKS_ENGINE_SLOTS * sizeof(void *),
    "struct HuksHdi must match huks_sa_hdi_struct.h");
