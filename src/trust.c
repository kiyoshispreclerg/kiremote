/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "trust.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cert.h"
#include "i18n.h"
#include "ini.h"
#include "log.h"
#include "packet.h"
#include "util.h"

typedef struct {
    char id[KR_ID_SIZE];
    X509 *cert;
    char *name, *type;
    int protocol;
} entry;

struct kr_trust {
    char dir[KR_PATH_MAX];
    entry *v;
    int n, cap;
};

static entry *find(const kr_trust *t, const char *id)
{
    for (int i = 0; id && i < t->n; i++)
        if (strcmp(t->v[i].id, id) == 0)
            return &t->v[i];
    return NULL;
}

static void entry_clear(entry *e)
{
    X509_free(e->cert);
    free(e->name);
    free(e->type);
}

static bool path_for(const kr_trust *t, const char *id, const char *ext, char *out, size_t size)
{
    return kr_pathf(out, size, "%s/%s.%s", t->dir, id, ext);
}

static void load_one(kr_trust *t, const char *id)
{
    char pem_path[KR_PATH_MAX], ini_path[KR_PATH_MAX];
    if (!path_for(t, id, "pem", pem_path, sizeof pem_path) ||
        !path_for(t, id, "ini", ini_path, sizeof ini_path))
        return;
    size_t len;
    char *pem = kr_read_file(pem_path, &len);
    X509 *cert = pem ? kr_x509_from_pem(pem, len) : NULL;
    free(pem);
    char cn[KR_ID_SIZE];
    if (!cert || !kr_x509_cn(cert, cn, sizeof cn) || strcmp(cn, id) != 0) {
        kr_log_warn(_("ignoring %s: not a certificate for that device"), pem_path);
        X509_free(cert);
        return;
    }

    char *text = kr_read_file(ini_path, NULL);
    kr_ini *ini = kr_ini_parse(text ? text : "", NULL, NULL);
    free(text);
    const char *name = ini ? kr_ini_get(ini, "device", "name") : NULL;
    const char *type = ini ? kr_ini_get(ini, "device", "type") : NULL;
    const char *proto = ini ? kr_ini_get(ini, "device", "protocol_version") : NULL;

    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 8;
        entry *v = realloc(t->v, (size_t)cap * sizeof *v);
        if (!v) {
            X509_free(cert);
            kr_ini_free(ini);
            return;
        }
        t->v = v;
        t->cap = cap;
    }
    entry *e = &t->v[t->n++];
    *e = (entry){.cert = cert,
                 .name = strdup(name && *name ? name : id),
                 .type = strdup(type && *type ? type : "phone"),
                 .protocol = proto ? atoi(proto) : 0};
    snprintf(e->id, sizeof e->id, "%s", id);
    kr_ini_free(ini);
}

kr_trust *kr_trust_open(const char *dir)
{
    kr_trust *t = calloc(1, sizeof *t);
    if (!t || !kr_pathf(t->dir, sizeof t->dir, "%s", dir)) {
        free(t);
        return NULL;
    }
    DIR *d = opendir(dir);
    if (!d)
        return t;
    struct dirent *de;
    while ((de = readdir(d))) {
        char id[KR_ID_SIZE];
        size_t n = strlen(de->d_name);
        if (n < 5 || strcmp(de->d_name + n - 4, ".pem") != 0 || n - 4 >= sizeof id)
            continue;
        memcpy(id, de->d_name, n - 4);
        id[n - 4] = '\0';
        if (kr_device_id_valid(id) && !find(t, id))
            load_one(t, id);
    }
    closedir(d);
    return t;
}

void kr_trust_free(kr_trust *t)
{
    if (!t)
        return;
    for (int i = 0; i < t->n; i++)
        entry_clear(&t->v[i]);
    free(t->v);
    free(t);
}

int kr_trust_count(const kr_trust *t)
{
    return t->n;
}

const char *kr_trust_id(const kr_trust *t, int i)
{
    return i >= 0 && i < t->n ? t->v[i].id : NULL;
}

bool kr_trust_has(const kr_trust *t, const char *id)
{
    return find(t, id) != NULL;
}

X509 *kr_trust_cert(const kr_trust *t, const char *id)
{
    entry *e = find(t, id);
    return e ? e->cert : NULL;
}

const char *kr_trust_name(const kr_trust *t, const char *id)
{
    entry *e = find(t, id);
    return e ? e->name : NULL;
}

const char *kr_trust_type(const kr_trust *t, const char *id)
{
    entry *e = find(t, id);
    return e ? e->type : NULL;
}

int kr_trust_protocol(const kr_trust *t, const char *id)
{
    entry *e = find(t, id);
    return e ? e->protocol : 0;
}

static bool save_info(const kr_trust *t, const entry *e)
{
    char path[KR_PATH_MAX], proto[16];
    if (!path_for(t, e->id, "ini", path, sizeof path))
        return false;
    kr_ini *ini = kr_ini_new();
    snprintf(proto, sizeof proto, "%d", e->protocol);
    bool ok = ini && kr_ini_set(ini, "device", "name", e->name) == 0 &&
              kr_ini_set(ini, "device", "type", e->type) == 0 &&
              kr_ini_set(ini, "device", "protocol_version", proto) == 0 &&
              kr_ini_save(ini, path) == 0;
    kr_ini_free(ini);
    if (!ok)
        kr_log_warn(_("cannot write %s: %s"), path, strerror(errno));
    return ok;
}

static bool save_cert(const kr_trust *t, const entry *e)
{
    char path[KR_PATH_MAX], tmp[KR_PATH_MAX];
    char *pem = kr_x509_to_pem(e->cert);
    bool ok = pem && path_for(t, e->id, "pem", path, sizeof path) &&
              kr_pathf(tmp, sizeof tmp, "%s.tmp", path);
    if (ok) {
        kr_make_parent_dirs(path);
        FILE *f = fopen(tmp, "w");
        ok = f && fputs(pem, f) >= 0;
        if (f)
            ok = (fclose(f) == 0) && ok;
        ok = ok && rename(tmp, path) == 0;
        if (!ok) {
            kr_log_warn(_("cannot write %s: %s"), path, strerror(errno));
            unlink(tmp);
        }
    }
    free(pem);
    return ok;
}

bool kr_trust_add(kr_trust *t, const char *id, X509 *cert, const char *name,
                  const char *type, int protocol)
{
    kr_trust_remove(t, id);
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 8;
        entry *v = realloc(t->v, (size_t)cap * sizeof *v);
        if (!v)
            return false;
        t->v = v;
        t->cap = cap;
    }
    entry e = {.name = strdup(name), .type = strdup(type), .protocol = protocol};
    snprintf(e.id, sizeof e.id, "%s", id);
    if (!e.name || !e.type || X509_up_ref(cert) != 1) {
        free(e.name);
        free(e.type);
        return false;
    }
    e.cert = cert;
    if (!save_cert(t, &e) || !save_info(t, &e)) {
        char path[KR_PATH_MAX];
        if (path_for(t, id, "pem", path, sizeof path))
            unlink(path);
        entry_clear(&e);
        return false;
    }
    t->v[t->n++] = e;
    return true;
}

void kr_trust_update(kr_trust *t, const char *id, const char *name, const char *type,
                     int protocol)
{
    entry *e = find(t, id);
    if (!e || (strcmp(e->name, name) == 0 && strcmp(e->type, type) == 0 && e->protocol == protocol))
        return;
    char *n = strdup(name), *ty = strdup(type);
    if (!n || !ty) {
        free(n);
        free(ty);
        return;
    }
    free(e->name);
    free(e->type);
    e->name = n;
    e->type = ty;
    e->protocol = protocol;
    save_info(t, e);
}

void kr_trust_remove(kr_trust *t, const char *id)
{
    entry *e = find(t, id);
    if (!e)
        return;
    char path[KR_PATH_MAX];
    if (path_for(t, id, "pem", path, sizeof path))
        unlink(path);
    if (path_for(t, id, "ini", path, sizeof path))
        unlink(path);
    entry_clear(e);
    *e = t->v[--t->n];
}

void kr_trust_clear(kr_trust *t)
{
    while (t->n)
        kr_trust_remove(t, t->v[0].id);
}
