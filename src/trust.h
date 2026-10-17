/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_TRUST_H
#define KR_TRUST_H

/* The paired devices, kept in the config folder:
 *
 *   trusted/<id>.pem   the device's certificate, pinned at pairing time
 *   trusted/<id>.ini   [device] name, type, protocol_version
 *
 * The certificate is what matters: a device id is only trusted with the
 * same certificate it had when paired. Everything is loaded at start and
 * written through on every change.
 *
 * kr_trust owns the certificates and strings it hands out; they live until
 * the entry is removed or kr_trust_free. */

#include <stdbool.h>

#include <openssl/x509.h>

typedef struct kr_trust kr_trust;

/* Loads dir (created when first written). Broken entries are logged and
 * skipped. NULL only without memory. */
kr_trust *kr_trust_open(const char *dir);
void kr_trust_free(kr_trust *t);

int kr_trust_count(const kr_trust *t);
/* The i-th id (0 <= i < count), in no particular order. */
const char *kr_trust_id(const kr_trust *t, int i);

bool kr_trust_has(const kr_trust *t, const char *id);
/* NULL if not trusted. */
X509 *kr_trust_cert(const kr_trust *t, const char *id);
const char *kr_trust_name(const kr_trust *t, const char *id);
const char *kr_trust_type(const kr_trust *t, const char *id);
/* The last protocol version seen (0 if unknown), against downgrades. */
int kr_trust_protocol(const kr_trust *t, const char *id);

/* Adds or replaces a device. False if it could not be saved (logged). */
bool kr_trust_add(kr_trust *t, const char *id, X509 *cert, const char *name,
                  const char *type, int protocol);
/* Updates the name, type and version of a trusted device (no-op if not
 * trusted or unchanged). */
void kr_trust_update(kr_trust *t, const char *id, const char *name, const char *type,
                     int protocol);
void kr_trust_remove(kr_trust *t, const char *id);
void kr_trust_clear(kr_trust *t);

#endif
