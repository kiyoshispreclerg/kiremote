/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Our certificate, the trust store and the pairing verification key. */
#define _DEFAULT_SOURCE /* mkdtemp */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/x509.h>

#include "../harness.h"
#include "cert.h"
#include "packet.h"
#include "trust.h"
#include "util.h"

static char tmpdir[64];

static void make_tmpdir(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/kiremote-test-XXXXXX");
    if (!mkdtemp(tmpdir))
        abort();
}

static void rm_rf(const char *dir)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
}

static X509 *fixture(const char *name)
{
    char path[KR_PATH_MAX];
    kr_pathf(path, sizeof path, "%s/%s", KR_FIXTURES_DIR, name);
    size_t len;
    char *pem = kr_read_file(path, &len);
    X509 *x = pem ? kr_x509_from_pem(pem, len) : NULL;
    free(pem);
    return x;
}

TEST(generated_like_upstream)
{
    kr_cert *c = kr_cert_generate();
    CHECK(c);
    const char *id = kr_cert_device_id(c);
    CHECK(kr_device_id_valid(id));
    CHECK_INT_EQ(strlen(id), 32);
    char cn[KR_ID_SIZE];
    CHECK(kr_x509_cn(kr_cert_x509(c), cn, sizeof cn));
    CHECK_STR_EQ(cn, id);
    X509 *x = kr_cert_x509(c);
    CHECK_INT_EQ(X509_get_version(x), 2);
    CHECK(X509_check_private_key(x, kr_cert_key(c)) == 1);
    CHECK(X509_cmp_current_time(X509_get0_notBefore(x)) < 0);
    CHECK(X509_cmp_current_time(X509_get0_notAfter(x)) > 0);
    char subject[256];
    X509_NAME_oneline(X509_get_subject_name(x), subject, sizeof subject);
    CHECK_STR_HAS(subject, "/O=KDE/OU=KDE Connect");
    kr_cert_free(c);
}

TEST(load_or_create_keeps_the_id)
{
    make_tmpdir();
    bool created = false;
    kr_cert *a = kr_cert_load_or_create(tmpdir, &created);
    CHECK(a && created);
    char key[KR_PATH_MAX];
    kr_pathf(key, sizeof key, "%s/privateKey.pem", tmpdir);
    struct stat st;
    CHECK(stat(key, &st) == 0 && (st.st_mode & 0777) == 0600);

    kr_cert *b = kr_cert_load_or_create(tmpdir, &created);
    CHECK(b && !created);
    CHECK_STR_EQ(kr_cert_device_id(a), kr_cert_device_id(b));
    CHECK(kr_x509_equal(kr_cert_x509(a), kr_cert_x509(b)));
    kr_cert_free(a);
    kr_cert_free(b);
    rm_rf(tmpdir);
}

TEST(verification_key_matches_the_vector)
{
    /* tests/fixtures/pair-*.pem and pair-key.txt come from
     * tools/verification-vector.sh, which uses only the openssl CLI. */
    X509 *a = fixture("pair-a.pem"), *b = fixture("pair-b.pem");
    CHECK(a && b);
    char path[KR_PATH_MAX];
    kr_pathf(path, sizeof path, "%s/pair-key.txt", KR_FIXTURES_DIR);
    char *txt = kr_read_file(path, NULL);
    CHECK(txt);
    long long ts = 0;
    char want[16] = "";
    CHECK(sscanf(txt, "%lld %15s", &ts, want) == 2);
    free(txt);

    char got[9];
    CHECK(kr_verification_key(a, b, ts, got));
    CHECK_STR_EQ(got, want);
    /* Same on both sides. */
    CHECK(kr_verification_key(b, a, ts, got));
    CHECK_STR_EQ(got, want);
    /* And it depends on the timestamp. */
    CHECK(kr_verification_key(a, b, ts + 1, got));
    CHECK(strcmp(got, want) != 0);
    X509_free(a);
    X509_free(b);
}

TEST(trust_store_round_trip)
{
    make_tmpdir();
    char dir[KR_PATH_MAX];
    kr_pathf(dir, sizeof dir, "%s/trusted", tmpdir);
    X509 *a = fixture("pair-a.pem"), *b = fixture("pair-b.pem");
    CHECK(a && b);
    const char *ida = "0123456789abcdef0123456789abcdef";

    kr_trust *t = kr_trust_open(dir);
    CHECK(t && kr_trust_count(t) == 0);
    CHECK(kr_trust_add(t, ida, a, "Celular", "phone", 8));
    /* The certificate must be the device's own. */
    kr_trust_update(t, ida, "Celular novo", "phone", 8);
    kr_trust_free(t);

    t = kr_trust_open(dir);
    CHECK(t && kr_trust_count(t) == 1);
    CHECK(kr_trust_has(t, ida));
    CHECK_STR_EQ(kr_trust_name(t, ida), "Celular novo");
    CHECK_STR_EQ(kr_trust_type(t, ida), "phone");
    CHECK_INT_EQ(kr_trust_protocol(t, ida), 8);
    CHECK(kr_x509_equal(kr_trust_cert(t, ida), a));
    CHECK(!kr_x509_equal(kr_trust_cert(t, ida), b));
    kr_trust_remove(t, ida);
    CHECK(!kr_trust_has(t, ida));
    kr_trust_free(t);

    t = kr_trust_open(dir);
    CHECK(t && kr_trust_count(t) == 0);
    kr_trust_free(t);

    /* A certificate saved under the wrong id is ignored. */
    char path[KR_PATH_MAX];
    kr_pathf(path, sizeof path, "%s/fedcba9876543210fedcba9876543211.pem", dir);
    char *pem = kr_x509_to_pem(b);
    FILE *f = fopen(path, "w");
    CHECK(f && pem);
    fputs(pem, f);
    fclose(f);
    free(pem);
    t = kr_trust_open(dir);
    CHECK(t && kr_trust_count(t) == 0);
    kr_trust_free(t);

    X509_free(a);
    X509_free(b);
    rm_rf(tmpdir);
}

int main(void)
{
    RUN(generated_like_upstream);
    RUN(load_or_create_keeps_the_id);
    RUN(verification_key_matches_the_vector);
    RUN(trust_store_round_trip);
    return harness_report();
}
