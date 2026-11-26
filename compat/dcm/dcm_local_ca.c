/* SPDX-License-Identifier: MIT
 * marble local DCM certificate authority (OpenSSL 3).
 *
 * Chain layout matches the donor's DcmClientUnpackCertChainFromService:
 *   uint32 certCount
 *   { uint32 derLen; uint8 der[derLen]; uint8 pad[0..3]; } * certCount
 *
 * All material is local-only. The root subject says "NOT Huawei" on purpose.
 */
#include "dcm_local_ca.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifndef DCM_CA_DIR
#define DCM_CA_DIR "/data/service/el1/public/device_cert_manager_server/local-ca"
#endif

#define ROOT_CERT DCM_CA_DIR "/root.pem"
#define ROOT_KEY DCM_CA_DIR "/root-key.pem"
#define DEV_CERT DCM_CA_DIR "/device.pem"
#define DEV_KEY DCM_CA_DIR "/device-key.pem"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;
static X509 *g_root;
static X509 *g_dev;
static EVP_PKEY *g_root_key;
static EVP_PKEY *g_dev_key;

static int ensure_dir(void)
{
    if (mkdir(DCM_CA_DIR, 0700) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int write_all(const char *path, const unsigned char *data, size_t len, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    chmod(path, mode);
    return 0;
}

static int write_pem_cert(const char *path, X509 *cert)
{
    BIO *bio = BIO_new(BIO_s_mem());
    if (bio == NULL || PEM_write_bio_X509(bio, cert) != 1) {
        BIO_free(bio);
        return -1;
    }
    char *pem = NULL;
    long pem_len = BIO_get_mem_data(bio, &pem);
    int rc = (pem_len > 0) ? write_all(path, (const unsigned char *)pem, (size_t)pem_len, 0600)
                           : -1;
    BIO_free(bio);
    return rc;
}

static int write_pem_key(const char *path, EVP_PKEY *key)
{
    BIO *bio = BIO_new(BIO_s_mem());
    if (bio == NULL || PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL) != 1) {
        BIO_free(bio);
        return -1;
    }
    char *pem = NULL;
    long pem_len = BIO_get_mem_data(bio, &pem);
    int rc = (pem_len > 0) ? write_all(path, (const unsigned char *)pem, (size_t)pem_len, 0600)
                           : -1;
    BIO_free(bio);
    return rc;
}

static X509 *read_pem_cert(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    X509 *cert = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    return cert;
}

static EVP_PKEY *read_pem_key(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    EVP_PKEY *key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    return key;
}

static EVP_PKEY *gen_ec_key(void)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (ctx == NULL) {
        return NULL;
    }
    EVP_PKEY *key = NULL;
    if (EVP_PKEY_keygen_init(ctx) <= 0 ||
        EVP_PKEY_CTX_set_group_name(ctx, "prime256v1") <= 0 ||
        EVP_PKEY_keygen(ctx, &key) <= 0) {
        key = NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    return key;
}

static void set_name(X509 *cert, const char *cn)
{
    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1, 0);
}

static void set_serial(X509 *cert)
{
    unsigned char rnd[8] = {0};
    ASN1_INTEGER *serial = X509_get_serialNumber(cert);
    if (RAND_bytes(rnd, sizeof(rnd)) != 1) {
        ASN1_INTEGER_set(serial, 1);
        return;
    }
    rnd[0] &= 0x7f;
    BIGNUM *bn = BN_bin2bn(rnd, sizeof(rnd), NULL);
    if (bn == NULL || BN_to_ASN1_INTEGER(bn, serial) == NULL ||
        ASN1_INTEGER_get(serial) == 0) {
        ASN1_INTEGER_set(serial, 1);
    }
    BN_free(bn);
}

static int add_ext_cfg(X509 *cert, X509 *issuer, int nid, const char *value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, issuer, cert, NULL, NULL, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
    if (ext == NULL) {
        return -1;
    }
    int rc = X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
    return rc == 1 ? 0 : -1;
}

static void read_sn_hash(char out[17])
{
    memset(out, '0', 16);
    out[16] = '\0';
    FILE *f = fopen("/compat/etc/dcm-device.json", "r");
    if (f == NULL) {
        return;
    }
    char buf[1024] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        return;
    }
    const char *key = "\"sn\":\"";
    char *p = strstr(buf, key);
    if (p == NULL) {
        return;
    }
    p += strlen(key);
    char *end = strchr(p, '"');
    if (end == NULL || end == p) {
        return;
    }
    unsigned char digest[32];
    unsigned int digest_len = 0;
    if (EVP_Digest(p, (size_t)(end - p), digest, &digest_len, EVP_sha256(), NULL) != 1 ||
        digest_len != 32) {
        return;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
}

static X509 *make_root(EVP_PKEY *key)
{
    X509 *cert = X509_new();
    if (cert == NULL) {
        return NULL;
    }
    X509_set_version(cert, 2);
    set_serial(cert);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 10L * 365 * 24 * 3600);
    set_name(cert, "marble local DCM root (NOT Huawei)");
    X509_set_issuer_name(cert, X509_get_subject_name(cert));
    X509_set_pubkey(cert, key);
    if (add_ext_cfg(cert, cert, NID_basic_constraints, "critical,CA:TRUE") != 0 ||
        add_ext_cfg(cert, cert, NID_key_usage, "critical,keyCertSign,cRLSign") != 0 ||
        X509_sign(cert, key, EVP_sha256()) == 0) {
        X509_free(cert);
        return NULL;
    }
    return cert;
}

static X509 *make_device(EVP_PKEY *key, EVP_PKEY *root_key, X509 *root, const char *sn_hash,
                          const char *model)
{
    X509 *cert = X509_new();
    if (cert == NULL) {
        return NULL;
    }
    X509_set_version(cert, 2);
    set_serial(cert);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 365L * 24 * 3600);
    char cn[64];
    snprintf(cn, sizeof(cn), "marble-local-%s", sn_hash);
    set_name(cert, cn);
    X509_set_issuer_name(cert, X509_get_subject_name(root));
    X509_set_pubkey(cert, key);
    if (add_ext_cfg(cert, root, NID_key_usage, "critical,digitalSignature") != 0) {
        X509_free(cert);
        return NULL;
    }
    char marker[160];
    snprintf(marker, sizeof(marker), "local-dcm;model=%s;sn_sha256=%s", model, sn_hash);
    ASN1_OBJECT *obj = OBJ_txt2obj("1.3.6.1.4.1.99999.1", 1);
    ASN1_OCTET_STRING *octet = ASN1_OCTET_STRING_new();
    if (obj == NULL || octet == NULL ||
        ASN1_OCTET_STRING_set(octet, (const unsigned char *)marker, (int)strlen(marker)) != 1) {
        ASN1_OBJECT_free(obj);
        ASN1_OCTET_STRING_free(octet);
        X509_free(cert);
        return NULL;
    }
    X509_EXTENSION *ext = X509_EXTENSION_create_by_OBJ(NULL, obj, 0, octet);
    ASN1_OBJECT_free(obj);
    ASN1_OCTET_STRING_free(octet);
    if (ext == NULL || X509_add_ext(cert, ext, -1) != 1) {
        X509_EXTENSION_free(ext);
        X509_free(cert);
        return NULL;
    }
    X509_EXTENSION_free(ext);
    if (X509_sign(cert, root_key, EVP_sha256()) == 0) {
        X509_free(cert);
        return NULL;
    }
    return cert;
}

static int device_valid(X509 *dev, X509 *root)
{
    if (dev == NULL || root == NULL) {
        return 0;
    }
    if (X509_cmp_current_time(X509_get0_notAfter(dev)) <= 0) {
        return 0;
    }
    return X509_check_issued(root, dev) == X509_V_OK;
}

static int load_or_create(void)
{
    if (ensure_dir() != 0) {
        return -1;
    }
    g_root_key = read_pem_key(ROOT_KEY);
    g_root = read_pem_cert(ROOT_CERT);
    if (g_root_key == NULL || g_root == NULL || X509_check_private_key(g_root, g_root_key) != 1) {
        EVP_PKEY_free(g_root_key);
        X509_free(g_root);
        g_root_key = gen_ec_key();
        g_root = g_root_key == NULL ? NULL : make_root(g_root_key);
        if (g_root_key == NULL || g_root == NULL) {
            return -2;
        }
        if (write_pem_key(ROOT_KEY, g_root_key) != 0 || write_pem_cert(ROOT_CERT, g_root) != 0) {
            return -3;
        }
    }
    g_dev_key = read_pem_key(DEV_KEY);
    g_dev = read_pem_cert(DEV_CERT);
    char sn_hash[17];
    read_sn_hash(sn_hash);
    const char *model = "marble";
    if (g_dev_key == NULL || g_dev == NULL || !device_valid(g_dev, g_root) ||
        X509_check_private_key(g_dev, g_dev_key) != 1) {
        EVP_PKEY_free(g_dev_key);
        X509_free(g_dev);
        g_dev_key = gen_ec_key();
        g_dev = g_dev_key == NULL ? NULL : make_device(g_dev_key, g_root_key, g_root, sn_hash, model);
        if (g_dev_key == NULL || g_dev == NULL) {
            return -4;
        }
        if (write_pem_key(DEV_KEY, g_dev_key) != 0 || write_pem_cert(DEV_CERT, g_dev) != 0) {
            return -5;
        }
    }
    return 0;
}

int dcm_ca_ensure(void)
{
    pthread_mutex_lock(&g_lock);
    int rc = 0;
    if (!g_ready) {
        rc = load_or_create();
        if (rc == 0) {
            g_ready = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int dcm_ca_state(void)
{
    return dcm_ca_ensure() == 0 && g_ready ? 1 : 0;
}

static int cert_der(X509 *cert, unsigned char **out, int *out_len)
{
    int len = i2d_X509(cert, NULL);
    if (len <= 0) {
        return -1;
    }
    unsigned char *der = malloc((size_t)len);
    if (der == NULL) {
        return -1;
    }
    unsigned char *p = der;
    if (i2d_X509(cert, &p) != len) {
        free(der);
        return -1;
    }
    *out = der;
    *out_len = len;
    return 0;
}

int dcm_ca_cert_der(int index, unsigned char **out, size_t *out_len)
{
    if (out == NULL || out_len == NULL) {
        return -1;
    }
    if (dcm_ca_ensure() != 0) {
        return -2;
    }
    X509 *cert = index == 0 ? g_dev : (index == 1 ? g_root : NULL);
    if (cert == NULL) {
        return -3;
    }
    int len = 0;
    unsigned char *der = NULL;
    if (cert_der(cert, &der, &len) != 0) {
        return -4;
    }
    *out = der;
    *out_len = (size_t)len;
    return 0;
}

int dcm_ca_cert_chain_blob(unsigned char **out, size_t *out_len)
{
    if (out == NULL || out_len == NULL) {
        return -1;
    }
    if (dcm_ca_ensure() != 0) {
        return -2;
    }
    unsigned char *dev_der = NULL;
    unsigned char *root_der = NULL;
    int dev_len = 0;
    int root_len = 0;
    if (cert_der(g_dev, &dev_der, &dev_len) != 0 || cert_der(g_root, &root_der, &root_len) != 0) {
        free(dev_der);
        free(root_der);
        return -3;
    }
    size_t dev_pad = ((size_t)dev_len + 3u) & ~3u;
    size_t root_pad = ((size_t)root_len + 3u) & ~3u;
    size_t total = 4u + 4u + dev_pad + 4u + root_pad;
    unsigned char *blob = calloc(1, total);
    if (blob == NULL) {
        free(dev_der);
        free(root_der);
        return -4;
    }
    size_t off = 0;
    uint32_t count = 2;
    memcpy(blob + off, &count, 4);
    off += 4;
    uint32_t len = (uint32_t)dev_len;
    memcpy(blob + off, &len, 4);
    off += 4;
    memcpy(blob + off, dev_der, (size_t)dev_len);
    off += dev_pad;
    len = (uint32_t)root_len;
    memcpy(blob + off, &len, 4);
    off += 4;
    memcpy(blob + off, root_der, (size_t)root_len);
    off += root_pad;
    free(dev_der);
    free(root_der);
    *out = blob;
    *out_len = off;
    return 0;
}

void dcm_ca_free(void *ptr)
{
    free(ptr);
}

#if defined(DCM_LOCAL_CA_HOST_TEST)
int dcm_ca_selftest(int verbose)
{
    unsigned char *blob = NULL;
    size_t blob_len = 0;
    int fails = 0;
    if (dcm_ca_state() != 1) {
        if (verbose) {
            fprintf(stderr, "FAIL state\n");
        }
        return 1;
    }
    if (dcm_ca_cert_chain_blob(&blob, &blob_len) != 0 || blob_len < 8) {
        if (verbose) {
            fprintf(stderr, "FAIL blob\n");
        }
        return 1;
    }
    uint32_t count = 0;
    memcpy(&count, blob, 4);
    if (count != 2) {
        if (verbose) {
            fprintf(stderr, "FAIL count=%u\n", count);
        }
        ++fails;
    }
    size_t off = 4;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = 0;
        if (off + 4 > blob_len) {
            ++fails;
            break;
        }
        memcpy(&len, blob + off, 4);
        off += 4;
        size_t padded = ((size_t)len + 3u) & ~3u;
        if (len == 0 || off + padded > blob_len) {
            ++fails;
            break;
        }
        const unsigned char *p = blob + off;
        X509 *cert = d2i_X509(NULL, &p, (long)len);
        if (cert == NULL) {
            ++fails;
        } else {
            X509_free(cert);
        }
        off += padded;
    }
    dcm_ca_free(blob);
    if (dcm_ca_ensure() != 0) {
        ++fails;
    }
    if (verbose) {
        fprintf(stdout, "%s dcm_ca_selftest count=%u len=%zu\n", fails == 0 ? "PASS" : "FAILED",
                count, blob_len);
    }
    return fails == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    int verbose = argc < 2 || strcmp(argv[1], "-q") != 0;
    int rc = dcm_ca_ensure();
    if (rc != 0) {
        fprintf(stderr, "dcm_ca_ensure rc=%d errno=%d dir=%s\n", rc, errno, DCM_CA_DIR);
        return 1;
    }
    return dcm_ca_selftest(verbose);
}
#endif
