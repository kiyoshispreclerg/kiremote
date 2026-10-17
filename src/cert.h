/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_CERT_H
#define KR_CERT_H

/* This computer's identity: a private key and a self-signed certificate
 * whose CN is the device id, made the way kdeconnect-kde makes them
 * (EC P-256, CN=<id>, O=KDE, OU=KDE Connect, valid from a year ago to ten
 * years ahead, SHA-512). Also the certificate helpers the protocol needs:
 * the CN, the public key, and the pairing verification key.
 *
 * kr_cert owns its key and certificate; the X509/EVP_PKEY pointers it
 * hands out are borrowed. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <openssl/evp.h>
#include <openssl/x509.h>

typedef struct kr_cert kr_cert;

/* Loads dir/privateKey.pem and dir/certificate.pem, making new ones when
 * missing, unreadable, expired or not ours. *created (may be NULL) says a
 * new certificate (and so a new device id) was made: the old pairings are
 * worthless then. NULL on failure (logged). */
kr_cert *kr_cert_load_or_create(const char *dir, bool *created);
/* A fresh identity in memory, for tests. */
kr_cert *kr_cert_generate(void);
void kr_cert_free(kr_cert *c);

const char *kr_cert_device_id(const kr_cert *c);
X509 *kr_cert_x509(const kr_cert *c);
EVP_PKEY *kr_cert_key(const kr_cert *c);

/* The certificate's CN into out; false if absent or too long. */
bool kr_x509_cn(X509 *x, char *out, size_t size);
/* PEM text of a certificate (caller frees), and back (caller X509_frees). */
char *kr_x509_to_pem(X509 *x);
X509 *kr_x509_from_pem(const char *pem, size_t len);
/* Same certificate (byte for byte)? */
bool kr_x509_equal(X509 *a, X509 *b);

/* The code both sides show while pairing (8 upper-case hex digits plus
 * NUL): SHA-256 of the two public keys (DER SubjectPublicKeyInfo, the
 * larger first) followed by the request's timestamp in decimal. */
bool kr_verification_key(X509 *a, X509 *b, int64_t timestamp, char out[9]);

#endif
