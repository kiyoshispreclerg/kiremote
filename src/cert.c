/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Certificate generation follows kdeconnect-kde's core/sslhelper.cpp and
 * core/kdeconnectconfig.cpp. */
#include "cert.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "i18n.h"
#include "log.h"
#include "packet.h"
#include "util.h"

struct kr_cert {
    EVP_PKEY *key;
    X509 *x509;
    char id[KR_ID_SIZE];
};

static const char *ssl_error(void)
{
    static char buf[256];
    unsigned long e = ERR_get_error();
    ERR_clear_error();
    if (!e)
        return "?";
    ERR_error_string_n(e, buf, sizeof buf);
    return buf;
}

static EVP_PKEY *generate_key(void)
{
    return EVP_EC_gen("P-256");
}

/* A new id: 32 lower-case hex digits, like QUuid::Id128. */
static bool new_device_id(char out[KR_ID_SIZE])
{
    unsigned char raw[16];
    if (RAND_bytes(raw, sizeof raw) != 1)
        return false;
    for (int i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02x", raw[i]);
    return true;
}

static X509 *self_signed(EVP_PKEY *key, const char *id)
{
    X509 *x = X509_new();
    X509_NAME *name = X509_NAME_new();
    BIGNUM *sn = BN_new();
    bool ok = x && name && sn && X509_set_version(x, 2) &&
              BN_rand(sn, 160, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) &&
              BN_to_ASN1_INTEGER(sn, X509_get_serialNumber(x)) &&
              X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)id, -1, -1, 0) &&
              X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"KDE", -1, -1, 0) &&
              X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC,
                                         (const unsigned char *)"KDE Connect", -1, -1, 0) &&
              X509_set_subject_name(x, name) && X509_set_issuer_name(x, name);
    if (ok) {
        long year = 365L * 24 * 60 * 60;
        ok = X509_gmtime_adj(X509_getm_notBefore(x), -year) &&
             X509_gmtime_adj(X509_getm_notAfter(x), 10 * year) && X509_set_pubkey(x, key) &&
             X509_sign(x, key, EVP_sha512()) > 0;
    }
    BN_free(sn);
    X509_NAME_free(name);
    if (!ok) {
        X509_free(x);
        return NULL;
    }
    return x;
}

static kr_cert *make(EVP_PKEY *key)
{
    kr_cert *c = calloc(1, sizeof *c);
    if (!c || !new_device_id(c->id) || !(c->x509 = self_signed(key, c->id))) {
        free(c);
        return NULL;
    }
    c->key = key;
    return c;
}

kr_cert *kr_cert_generate(void)
{
    EVP_PKEY *key = generate_key();
    kr_cert *c = key ? make(key) : NULL;
    if (!c)
        EVP_PKEY_free(key);
    return c;
}

void kr_cert_free(kr_cert *c)
{
    if (!c)
        return;
    X509_free(c->x509);
    EVP_PKEY_free(c->key);
    free(c);
}

const char *kr_cert_device_id(const kr_cert *c)
{
    return c->id;
}

X509 *kr_cert_x509(const kr_cert *c)
{
    return c->x509;
}

EVP_PKEY *kr_cert_key(const kr_cert *c)
{
    return c->key;
}

/* ---- files --------------------------------------------------------------- */

static EVP_PKEY *read_key(const char *path)
{
    size_t len;
    char *pem = kr_read_file_or_backup(path, kr_pem_text_ok, &len);
    BIO *b = pem ? BIO_new_mem_buf(pem, (int)len) : NULL;
    EVP_PKEY *k = b ? PEM_read_bio_PrivateKey(b, NULL, NULL, NULL) : NULL;
    BIO_free(b);
    if (pem)
        OPENSSL_cleanse(pem, len);
    free(pem);
    return k;
}

static X509 *read_x509(const char *path)
{
    size_t len;
    char *pem = kr_read_file_or_backup(path, kr_pem_text_ok, &len);
    BIO *b = pem ? BIO_new_mem_buf(pem, (int)len) : NULL;
    X509 *x = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
    BIO_free(b);
    free(pem);
    return x;
}

/* Writes atomically with mode 0600. */
static bool write_pem(const char *path, EVP_PKEY *key, X509 *x)
{
    char tmp[KR_PATH_MAX];
    if (!kr_pathf(tmp, sizeof tmp, "%s.tmp", path))
        return false;
    kr_make_parent_dirs(path);
    if (kr_backup_file(path, kr_pem_text_ok) != 0)
        kr_log_warn(_("cannot back up %s: %s"), path, strerror(errno));
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return false;
    }
    bool ok = key ? PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL)
                  : PEM_write_X509(f, x);
    ok = ok && kr_flush_sync(f) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

/* Is x usable as our certificate: ours (same key), valid now, CN a valid id. */
static bool cert_usable(X509 *x, EVP_PKEY *key, char id[KR_ID_SIZE], const char *path)
{
    if (X509_check_private_key(x, key) != 1) {
        ERR_clear_error();
        kr_log_warn(_("%s does not match the private key"), path);
        return false;
    }
    if (X509_cmp_current_time(X509_get0_notBefore(x)) >= 0) {
        kr_log_warn(_("%s is not valid yet"), path);
        return false;
    }
    if (X509_cmp_current_time(X509_get0_notAfter(x)) <= 0) {
        kr_log_warn(_("%s has expired"), path);
        return false;
    }
    if (!kr_x509_cn(x, id, KR_ID_SIZE) || !kr_device_id_valid(id)) {
        kr_log_warn(_("%s does not hold a valid device id"), path);
        return false;
    }
    return true;
}

kr_cert *kr_cert_load_or_create(const char *dir, bool *created)
{
    char key_path[KR_PATH_MAX], cert_path[KR_PATH_MAX];
    if (!kr_pathf(key_path, sizeof key_path, "%s/privateKey.pem", dir) ||
        !kr_pathf(cert_path, sizeof cert_path, "%s/certificate.pem", dir)) {
        kr_log_error(_("path too long: %s"), dir);
        return NULL;
    }
    if (created)
        *created = false;

    EVP_PKEY *key = read_key(key_path);
    if (!key) {
        if (errno != ENOENT || kr_file_exists(key_path))
            kr_log_warn(_("cannot read %s, making a new key"), key_path);
        ERR_clear_error();
        key = generate_key();
        if (!key || !write_pem(key_path, key, NULL)) {
            kr_log_error(_("cannot create %s: %s"), key_path, key ? strerror(errno) : ssl_error());
            EVP_PKEY_free(key);
            return NULL;
        }
        kr_log_info(_("created a new private key in %s"), key_path);
    }

    kr_cert *c = calloc(1, sizeof *c);
    if (!c) {
        EVP_PKEY_free(key);
        return NULL;
    }
    c->key = key;
    c->x509 = read_x509(cert_path);
    ERR_clear_error();
    if (c->x509 && cert_usable(c->x509, key, c->id, cert_path))
        return c;

    X509_free(c->x509);
    c->x509 = NULL;
    if (!new_device_id(c->id) || !(c->x509 = self_signed(key, c->id)) ||
        !write_pem(cert_path, NULL, c->x509)) {
        kr_log_error(_("cannot create %s: %s"), cert_path, c->x509 ? strerror(errno) : ssl_error());
        kr_cert_free(c);
        return NULL;
    }
    kr_log_info(_("created a new certificate in %s (device id %s)"), cert_path, c->id);
    if (created)
        *created = true;
    return c;
}

/* ---- helpers ------------------------------------------------------------- */

bool kr_x509_cn(X509 *x, char *out, size_t size)
{
    int n = X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, NULL, 0);
    if (n <= 0 || (size_t)n >= size)
        return false;
    return X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, out, (int)size) == n;
}

char *kr_x509_to_pem(X509 *x)
{
    BIO *b = BIO_new(BIO_s_mem());
    char *out = NULL;
    if (b && PEM_write_bio_X509(b, x)) {
        char *data;
        long n = BIO_get_mem_data(b, &data);
        out = malloc((size_t)n + 1);
        if (out) {
            memcpy(out, data, (size_t)n);
            out[n] = '\0';
        }
    }
    BIO_free(b);
    return out;
}

X509 *kr_x509_from_pem(const char *pem, size_t len)
{
    BIO *b = BIO_new_mem_buf(pem, (int)len);
    X509 *x = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
    BIO_free(b);
    ERR_clear_error();
    return x;
}

bool kr_x509_equal(X509 *a, X509 *b)
{
    return a && b && X509_cmp(a, b) == 0;
}

/* DER SubjectPublicKeyInfo, what Qt's publicKey().toDer() and Java's
 * getPublicKey().getEncoded() give. Caller frees with OPENSSL_free. */
static int pubkey_der(X509 *x, unsigned char **out)
{
    *out = NULL;
    return i2d_X509_PUBKEY(X509_get_X509_PUBKEY(x), out);
}

bool kr_verification_key(X509 *a, X509 *b, int64_t timestamp, char out[9])
{
    unsigned char *da, *db;
    int na = pubkey_der(a, &da), nb = pubkey_der(b, &db);
    bool ok = na > 0 && nb > 0;
    if (ok) {
        /* Byte comparison as QByteArray's operator< and Java's
         * Arrays.compareUnsigned: a shorter prefix is smaller. */
        int cmp = memcmp(da, db, (size_t)(na < nb ? na : nb));
        if (cmp < 0 || (cmp == 0 && na < nb)) {
            unsigned char *t = da;
            da = db;
            db = t;
            int tn = na;
            na = nb;
            nb = tn;
        }
        char ts[32];
        snprintf(ts, sizeof ts, "%" PRId64, timestamp);
        unsigned char md[SHA256_DIGEST_LENGTH];
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        unsigned int mdlen = 0;
        ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) &&
             EVP_DigestUpdate(ctx, da, (size_t)na) && EVP_DigestUpdate(ctx, db, (size_t)nb) &&
             EVP_DigestUpdate(ctx, ts, strlen(ts)) && EVP_DigestFinal_ex(ctx, md, &mdlen);
        EVP_MD_CTX_free(ctx);
        for (int i = 0; ok && i < 4; i++)
            snprintf(out + 2 * i, 3, "%02X", md[i]);
    }
    OPENSSL_free(da);
    OPENSSL_free(db);
    return ok;
}
