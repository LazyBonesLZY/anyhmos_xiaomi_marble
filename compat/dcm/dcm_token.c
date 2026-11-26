/* SPDX-License-Identifier: MIT
 * Self-contained SHA-256 / HMAC-SHA256 / base64url and the marble local
 * device-token derivation. No OpenSSL, no libcrypto, no device dependency so
 * the same code runs in the service, in the probe, and in host self-tests.
 */
#include "dcm_token.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ SHA-256 */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    uint8_t buf[64];
    size_t used;
} dcm_sha256_ctx;

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t dcm_ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void dcm_sha256_block(dcm_sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = dcm_ror(w[i - 15], 7) ^ dcm_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = dcm_ror(w[i - 2], 17) ^ dcm_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = dcm_ror(e, 6) ^ dcm_ror(e, 11) ^ dcm_ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        uint32_t s0 = dcm_ror(a, 2) ^ dcm_ror(a, 13) ^ dcm_ror(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void dcm_sha256_init(dcm_sha256_ctx *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u; c->h[2] = 0x3c6ef372u;
    c->h[3] = 0xa54ff53au; c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->bits = 0;
    c->used = 0;
}

static void dcm_sha256_update(dcm_sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->bits += (uint64_t)len * 8u;
    while (len != 0) {
        size_t n = 64u - c->used;
        if (n > len) {
            n = len;
        }
        memcpy(c->buf + c->used, p, n);
        c->used += n;
        p += n;
        len -= n;
        if (c->used == 64u) {
            dcm_sha256_block(c, c->buf);
            c->used = 0;
        }
    }
}

static void dcm_sha256_final(dcm_sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->bits;
    uint8_t pad = 0x80;
    dcm_sha256_update(c, &pad, 1);
    pad = 0;
    while (c->used != 56u) {
        dcm_sha256_update(c, &pad, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) {
        len[7 - i] = (uint8_t)(bits >> (i * 8));
    }
    dcm_sha256_update(c, len, 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

static void dcm_sha256(const void *data, size_t len, uint8_t out[32])
{
    dcm_sha256_ctx c;
    dcm_sha256_init(&c);
    dcm_sha256_update(&c, data, len);
    dcm_sha256_final(&c, out);
}

static void dcm_hmac_key(const uint8_t *key, size_t key_len, const void *data, size_t len,
                         uint8_t out[32])
{
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[32];
    dcm_sha256_ctx c;

    memset(k, 0, sizeof(k));
    if (key_len > sizeof(k)) {
        dcm_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (int i = 0; i < 64; ++i) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36u);
        opad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }
    dcm_sha256_init(&c);
    dcm_sha256_update(&c, ipad, sizeof(ipad));
    dcm_sha256_update(&c, data, len);
    dcm_sha256_final(&c, inner);
    dcm_sha256_init(&c);
    dcm_sha256_update(&c, opad, sizeof(opad));
    dcm_sha256_update(&c, inner, sizeof(inner));
    dcm_sha256_final(&c, out);
}

/* --------------------------------------------------------------- base64url */

static const char kB64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t dcm_b64url_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    size_t need = ((len + 2u) / 3u) * 4u + 1u;
    if (cap < need) {
        return 0;
    }
    size_t o = 0;
    size_t i = 0;
    while (i + 3u <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = kB64Url[(v >> 18) & 0x3fu];
        out[o++] = kB64Url[(v >> 12) & 0x3fu];
        out[o++] = kB64Url[(v >> 6) & 0x3fu];
        out[o++] = kB64Url[v & 0x3fu];
        i += 3u;
    }
    if (len - i == 1u) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = kB64Url[(v >> 18) & 0x3fu];
        out[o++] = kB64Url[(v >> 12) & 0x3fu];
    } else if (len - i == 2u) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = kB64Url[(v >> 18) & 0x3fu];
        out[o++] = kB64Url[(v >> 12) & 0x3fu];
        out[o++] = kB64Url[(v >> 6) & 0x3fu];
    }
    out[o] = '\0';
    return o;
}

static int dcm_b64url_decode(const char *in, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t len = strlen(in);
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4u) {
        uint32_t v = 0;
        size_t n = len - i;
        if (n > 4u) {
            n = 4u;
        }
        for (size_t j = 0; j < 4u; ++j) {
            v <<= 6;
            if (j < n) {
                const char *pos = strchr(kB64Url, in[i + j]);
                if (pos == NULL || in[i + j] == '\0') {
                    return -1;
                }
                v |= (uint32_t)(pos - kB64Url);
            }
        }
        if (o >= cap) {
            return -1;
        }
        out[o++] = (uint8_t)(v >> 16);
        if (n >= 3u) {
            if (o >= cap) {
                return -1;
            }
            out[o++] = (uint8_t)(v >> 8);
        }
        if (n == 4u) {
            if (o >= cap) {
                return -1;
            }
            out[o++] = (uint8_t)v;
        }
    }
    *out_len = o;
    return 0;
}

/* -------------------------------------------------------------------- token */

static size_t dcm_sanitise(const char *in, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    size_t o = 0;
    if (in != NULL) {
        for (; *in != '\0' && o + 1u < cap && o < 127u; ++in) {
            unsigned char c = (unsigned char)*in;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                c == '.' || c == '_' || c == ':' || c == '-') {
                out[o++] = (char)c;
            } else {
                out[o++] = '_';
            }
        }
    }
    if (o == 0) {
        memcpy(out, "unknown", 7);
        o = 7;
    }
    out[o] = '\0';
    return o;
}

static void dcm_hex(const uint8_t *in, size_t len, char *out)
{
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[in[i] >> 4];
        out[i * 2 + 1] = kHex[in[i] & 0x0fu];
    }
    out[len * 2] = '\0';
}

int dcm_build_token(const char *sn, const char *model, const char *udid,
                    uint64_t request_id, int64_t issued_at,
                    char *out, size_t out_cap)
{
    if (out == NULL || out_cap < 256u) {
        return -1;
    }
    char ssn[128];
    char smodel[128];
    char sudid[128];
    dcm_sanitise(sn, ssn, sizeof(ssn));
    dcm_sanitise(model, smodel, sizeof(smodel));
    dcm_sanitise(udid, sudid, sizeof(sudid));

    char payload[512];
    int n = snprintf(payload, sizeof(payload),
                     "{\"v\":%d,\"src\":\"hmos-soft-tee\",\"sn\":\"%s\",\"model\":\"%s\","
                     "\"udid\":\"%s\",\"req\":\"%016llx\",\"iat\":%lld}",
                     DCM_TOKEN_VERSION, ssn, smodel, sudid,
                     (unsigned long long)request_id, (long long)issued_at);
    if (n <= 0 || (size_t)n >= sizeof(payload)) {
        return -2;
    }
    char body[1024];
    size_t body_len = dcm_b64url_encode((const uint8_t *)payload, (size_t)n, body, sizeof(body));
    if (body_len == 0) {
        return -3;
    }

    char key_material[256];
    int kn = snprintf(key_material, sizeof(key_material), "hmos-dcm-soft-tee|%s|%s", ssn, smodel);
    if (kn <= 0 || (size_t)kn >= sizeof(key_material)) {
        return -4;
    }
    uint8_t key[32];
    dcm_sha256(key_material, (size_t)kn, key);
    uint8_t mac[32];
    dcm_hmac_key(key, sizeof(key), body, body_len, mac);
    char mac_hex[65];
    dcm_hex(mac, sizeof(mac), mac_hex);

    int m = snprintf(out, out_cap, DCM_TOKEN_PREFIX "%s.%s", body, mac_hex);
    if (m <= 0 || (size_t)m >= out_cap) {
        return -5;
    }
    return 0;
}

int dcm_token_digest(const char *token, char hex[17])
{
    if (token == NULL || hex == NULL) {
        return -1;
    }
    uint8_t h[32];
    dcm_sha256(token, strlen(token), h);
    char full[65];
    dcm_hex(h, sizeof(h), full);
    memcpy(hex, full, 16);
    hex[16] = '\0';
    return 0;
}

/* ----------------------------------------------------------------- selftest */

static int dcm_expect_hex(const char *what, const uint8_t *got, size_t len, const char *want,
                          int verbose)
{
    char hex[129];
    dcm_hex(got, len, hex);
    if (strcmp(hex, want) != 0) {
        if (verbose) {
            fprintf(stderr, "FAIL %s got=%s want=%s\n", what, hex, want);
        }
        return 1;
    }
    if (verbose) {
        fprintf(stdout, "ok %s\n", what);
    }
    return 0;
}

int dcm_token_selftest(int verbose)
{
    int fails = 0;
    uint8_t h[32];

    dcm_sha256("abc", 3, h);
    fails += dcm_expect_hex("sha256(abc)", h, 32,
                            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                            verbose);
    dcm_sha256("", 0, h);
    fails += dcm_expect_hex("sha256(empty)", h, 32,
                            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                            verbose);

    dcm_hmac_key((const uint8_t *)"key", 3, "The quick brown fox jumps over the lazy dog", 43, h);
    fails += dcm_expect_hex("hmac(key,fox)", h, 32,
                            "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
                            verbose);

    char b64[64];
    size_t n = dcm_b64url_encode((const uint8_t *)"abc", 3, b64, sizeof(b64));
    if (n != 4u || strcmp(b64, "YWJj") != 0) {
        if (verbose) {
            fprintf(stderr, "FAIL b64url abc got=%s\n", b64);
        }
        ++fails;
    } else if (verbose) {
        fprintf(stdout, "ok b64url abc\n");
    }
    uint8_t dec[32];
    size_t dec_len = 0;
    if (dcm_b64url_decode("YWJj", dec, sizeof(dec), &dec_len) != 0 || dec_len != 3u ||
        memcmp(dec, "abc", 3) != 0) {
        if (verbose) {
            fprintf(stderr, "FAIL b64url decode\n");
        }
        ++fails;
    } else if (verbose) {
        fprintf(stdout, "ok b64url decode\n");
    }

    char token[1024];
    if (dcm_build_token("5030e45e", "marble", "", 0x1234u, 1700000000, token, sizeof(token)) != 0) {
        if (verbose) {
            fprintf(stderr, "FAIL build token\n");
        }
        ++fails;
    } else {
        const char *dot1 = strchr(token, '.');
        const char *dot2 = dot1 == NULL ? NULL : strchr(dot1 + 1, '.');
        if (strncmp(token, DCM_TOKEN_PREFIX, strlen(DCM_TOKEN_PREFIX)) != 0 || dot1 == NULL ||
            dot2 == NULL || strlen(dot2 + 1) != 64u || strchr(dot2 + 1, '.') != NULL) {
            if (verbose) {
                fprintf(stderr, "FAIL token shape %s\n", token);
            }
            ++fails;
        } else {
            size_t body_len = (size_t)(dot2 - dot1 - 1);
            char body[1024];
            if (body_len >= sizeof(body)) {
                ++fails;
            } else {
                memcpy(body, dot1 + 1, body_len);
                body[body_len] = '\0';
                uint8_t raw[512];
                size_t raw_len = 0;
                if (dcm_b64url_decode(body, raw, sizeof(raw), &raw_len) != 0 || raw_len == 0 ||
                    memchr(raw, '{', raw_len) == NULL || memchr(raw, '}', raw_len) == NULL) {
                    if (verbose) {
                        fprintf(stderr, "FAIL token payload\n");
                    }
                    ++fails;
                }
            }
        }
        char d1[17];
        char d2[17];
        dcm_token_digest(token, d1);
        dcm_token_digest(token, d2);
        if (strcmp(d1, d2) != 0 || strlen(d1) != 16u) {
            if (verbose) {
                fprintf(stderr, "FAIL token digest\n");
            }
            ++fails;
        } else if (verbose) {
            fprintf(stdout, "ok token shape digest=%s\n", d1);
        }
    }
    if (verbose) {
        fprintf(stdout, "%s tokenselftest\n", fails == 0 ? "PASS" : "FAILED");
    }
    return fails == 0 ? 0 : 1;
}

#ifdef DCM_TOKEN_HOST_TEST
int main(int argc, char **argv)
{
    int verbose = argc < 2 || strcmp(argv[1], "-q") != 0;
    int rc = dcm_token_selftest(verbose);
    if (rc == 0 && verbose) {
        char token[1024];
        if (dcm_build_token("5030e45e", "marble", "", 42u, 1700000000, token, sizeof(token)) == 0) {
            fprintf(stdout, "TOKEN_SAMPLE %s\n", token);
        }
    }
    return rc;
}
#endif
