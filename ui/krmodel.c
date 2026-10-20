/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "krmodel.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "i18n.h"
#include "util.h"

struct kr_model {
    kr_dev *dev;
    int n, cap;
    bool up;
    char name[128], version[32];
    char id_out[96]; /* what apply_event hands out */
    char *last_file; /* the status file as last read */
};

kr_model *kr_model_new(void)
{
    return calloc(1, sizeof(kr_model));
}

void kr_model_free(kr_model *m)
{
    if (!m)
        return;
    free(m->dev);
    free(m->last_file);
    free(m);
}

bool kr_model_daemon_up(const kr_model *m)
{
    return m->up;
}

const char *kr_model_name(const kr_model *m)
{
    return m->name;
}

const char *kr_model_version(const kr_model *m)
{
    return m->version;
}

bool kr_model_reset(kr_model *m)
{
    bool changed = m->up || m->n;
    m->up = false;
    m->n = 0;
    m->name[0] = m->version[0] = '\0';
    return changed;
}

/* ---- parsing ------------------------------------------------------------- */

static void copy(char *out, size_t size, const cJSON *v)
{
    snprintf(out, size, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static const cJSON *item(const cJSON *o, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(o, key);
}

static void dev_from_json(kr_dev *d, const cJSON *o)
{
    memset(d, 0, sizeof *d);
    copy(d->id, sizeof d->id, item(o, "id"));
    copy(d->name, sizeof d->name, item(o, "name"));
    copy(d->type, sizeof d->type, item(o, "type"));
    copy(d->state, sizeof d->state, item(o, "state"));
    copy(d->key, sizeof d->key, item(o, "key"));
    copy(d->address, sizeof d->address, item(o, "address"));
    d->reachable = cJSON_IsTrue(item(o, "reachable"));
    d->paired = cJSON_IsTrue(item(o, "paired"));
    d->battery = -1;
    d->signal = -1;

    const cJSON *b = item(o, "battery");
    if (cJSON_IsObject(b)) {
        const cJSON *ch = item(b, "charge");
        if (cJSON_IsNumber(ch) && ch->valuedouble >= 0 && ch->valuedouble <= 100)
            d->battery = (int)ch->valuedouble;
        d->charging = cJSON_IsTrue(item(b, "charging"));
    }
    const cJSON *s = item(o, "signal");
    if (cJSON_IsObject(s)) {
        d->has_signal = true;
        copy(d->net_type, sizeof d->net_type, item(s, "type"));
        const cJSON *lv = item(s, "strength");
        d->signal = cJSON_IsNumber(lv) && lv->valuedouble >= 0 && lv->valuedouble <= 4 ? (int)lv->valuedouble : -1;
    }
    const cJSON *lk = item(o, "locked");
    if (cJSON_IsBool(lk)) {
        d->locked_known = true;
        d->locked = cJSON_IsTrue(lk);
    }
    const cJSON *pl = item(o, "plugins"), *p;
    cJSON_ArrayForEach(p, pl)
    {
        if (cJSON_IsString(p) && d->n_plugins < KR_MODEL_PLUGINS)
            copy(d->plugins[d->n_plugins++], sizeof d->plugins[0], p);
    }
}

/* ---- ordering ------------------------------------------------------------ */

static int group(const kr_dev *d)
{
    if (strcmp(d->state, "requested_by_peer") == 0)
        return 0;
    if (d->paired && d->reachable)
        return 1;
    if (d->paired)
        return 2;
    return 3;
}

static int cmp(const void *a, const void *b)
{
    const kr_dev *x = a, *y = b;
    int g = group(x) - group(y);
    if (g)
        return g;
    int n = strcasecmp(x->name, y->name);
    return n ? n : strcmp(x->id, y->id);
}

static void sort(kr_model *m)
{
    qsort(m->dev, (size_t)m->n, sizeof *m->dev, cmp);
}

static bool reserve(kr_model *m, int need)
{
    if (need <= m->cap)
        return true;
    int cap = m->cap ? m->cap * 2 : 8;
    while (cap < need)
        cap *= 2;
    kr_dev *nd = realloc(m->dev, (size_t)cap * sizeof *nd);
    if (!nd)
        return false;
    m->dev = nd;
    m->cap = cap;
    return true;
}

static kr_dev *upsert(kr_model *m, const cJSON *o)
{
    kr_dev tmp;
    dev_from_json(&tmp, o);
    if (!tmp.id[0])
        return NULL;
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->dev[i].id, tmp.id) == 0) {
            m->dev[i] = tmp;
            return &m->dev[i];
        }
    if (!reserve(m, m->n + 1))
        return NULL;
    m->dev[m->n] = tmp;
    return &m->dev[m->n++];
}

/* ---- feeding it ------------------------------------------------------------ */

bool kr_model_apply_status(kr_model *m, const cJSON *st)
{
    const cJSON *devs = item(st, "devices");
    if (!cJSON_IsObject(st) || !cJSON_IsArray(devs))
        return false;
    m->n = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, devs) upsert(m, o);
    sort(m);
    m->up = true;
    copy(m->name, sizeof m->name, item(st, "name"));
    copy(m->version, sizeof m->version, item(st, "version"));
    return true;
}

kr_event_kind kr_model_apply_event(kr_model *m, const cJSON *ev, const char **id)
{
    if (id)
        *id = NULL;
    const cJSON *kind = item(ev, "event");
    if (!cJSON_IsObject(ev) || !cJSON_IsString(kind))
        return KR_EV_NONE;
    const char *k = kind->valuestring;

    if (strcmp(k, "device") == 0 || strcmp(k, "pair-request") == 0) {
        kr_dev *d = cJSON_IsObject(item(ev, "device")) ? upsert(m, item(ev, "device")) : NULL;
        if (!d)
            return KR_EV_NONE;
        snprintf(m->id_out, sizeof m->id_out, "%s", d->id);
        sort(m);
        if (id)
            *id = m->id_out;
        return k[0] == 'p' ? KR_EV_PAIR_REQUEST : KR_EV_DEVICES;
    }
    if (strcmp(k, "removed") == 0) {
        const cJSON *i = item(ev, "id");
        if (!cJSON_IsString(i))
            return KR_EV_NONE;
        for (int j = 0; j < m->n; j++)
            if (strcmp(m->dev[j].id, i->valuestring) == 0) {
                snprintf(m->id_out, sizeof m->id_out, "%s", m->dev[j].id);
                memmove(&m->dev[j], &m->dev[j + 1], (size_t)(m->n - j - 1) * sizeof *m->dev);
                m->n--;
                if (id)
                    *id = m->id_out;
                return KR_EV_DEVICES;
            }
        return KR_EV_NONE;
    }
    if (strcmp(k, "config") == 0) {
        const cJSON *key = item(ev, "key");
        if (!cJSON_IsString(key))
            return KR_EV_NONE;
        snprintf(m->id_out, sizeof m->id_out, "%s", key->valuestring);
        if (id)
            *id = m->id_out;
        return KR_EV_CONFIG;
    }
    return KR_EV_NONE;
}

bool kr_model_status_path(char *out, size_t size)
{
    return kr_runtime_path(out, size, "kiremote-status.json");
}

bool kr_model_load_file(kr_model *m, const char *path)
{
    char *text = kr_read_file(path, NULL);
    cJSON *st = text ? cJSON_Parse(text) : NULL;
    const cJSON *pid = item(st, "pid");
    /* Left behind by a daemon that crashed? */
    bool alive = cJSON_IsNumber(pid) && pid->valuedouble > 0 && (kill((pid_t)pid->valuedouble, 0) == 0 || errno == EPERM);
    bool changed;
    if (!alive) {
        free(text);
        cJSON_Delete(st);
        free(m->last_file);
        m->last_file = NULL;
        return kr_model_reset(m);
    }
    if (m->last_file && strcmp(m->last_file, text) == 0 && m->up) {
        changed = false;
        free(text);
    } else {
        changed = kr_model_apply_status(m, st);
        if (!changed)
            changed = kr_model_reset(m);
        free(m->last_file);
        m->last_file = text;
    }
    cJSON_Delete(st);
    return changed;
}

/* ---- the list ---------------------------------------------------------------- */

int kr_model_count(const kr_model *m)
{
    return m->n;
}

const kr_dev *kr_model_at(const kr_model *m, int i)
{
    return i >= 0 && i < m->n ? &m->dev[i] : NULL;
}

const kr_dev *kr_model_find(const kr_model *m, const char *id)
{
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->dev[i].id, id) == 0)
            return &m->dev[i];
    return NULL;
}

int kr_model_connected(const kr_model *m)
{
    int n = 0;
    for (int i = 0; i < m->n; i++)
        n += m->dev[i].paired && m->dev[i].reachable;
    return n;
}

bool kr_model_pair_pending(const kr_model *m)
{
    for (int i = 0; i < m->n; i++)
        if (strcmp(m->dev[i].state, "requested_by_peer") == 0)
            return true;
    return false;
}

/* ---- texts ------------------------------------------------------------------- */

const char *kr_dev_state_text(const kr_dev *d)
{
    if (strcmp(d->state, "requested_by_peer") == 0)
        return _("Wants to pair");
    if (strcmp(d->state, "requested") == 0)
        return _("Waiting for it to accept");
    if (d->paired)
        return d->reachable ? _("Connected") : _("Not reachable");
    return _("Available");
}

const char *kr_dev_battery_text(const kr_dev *d, char *buf, size_t size)
{
    if (d->battery < 0)
        snprintf(buf, size, "%s", "");
    else
        snprintf(buf, size, d->charging ? _("%d%% (charging)") : _("%d%%"), d->battery);
    return buf;
}

const char *kr_dev_signal_text(const kr_dev *d, char *buf, size_t size)
{
    if (!d->has_signal)
        snprintf(buf, size, "%s", "");
    else if (d->signal < 0)
        snprintf(buf, size, "%s", _("No signal"));
    else if (d->net_type[0])
        snprintf(buf, size, "%s %d/4", d->net_type, d->signal);
    else
        snprintf(buf, size, "%d/4", d->signal);
    return buf;
}

const char *kr_dev_icon(const kr_dev *d)
{
    if (strcmp(d->type, "tablet") == 0)
        return "tablet";
    if (strcmp(d->type, "laptop") == 0)
        return "computer-laptop";
    if (strcmp(d->type, "desktop") == 0)
        return "computer";
    if (strcmp(d->type, "tv") == 0)
        return "video-television";
    return "phone";
}

void kr_model_tooltip(const kr_model *m, char *buf, size_t size)
{
    if (!size)
        return;
    buf[0] = '\0';
    if (!m->up) {
        snprintf(buf, size, "%s", _("Kiremote is not running"));
        return;
    }
    size_t len = 0;
    for (int i = 0; i < m->n; i++) {
        const kr_dev *d = &m->dev[i];
        if (!d->paired || !d->reachable)
            continue;
        char bat[48];
        kr_dev_battery_text(d, bat, sizeof bat);
        int w = snprintf(buf + len, size - len, "%s%s%s%s", len ? "\n" : "", d->name, bat[0] ? " - " : "", bat);
        if (w < 0 || (size_t)w >= size - len)
            break;
        len += (size_t)w;
    }
    if (!len)
        snprintf(buf, size, "%s", kr_model_pair_pending(m) ? _("A device wants to pair") : _("No device connected"));
}

/* ---- actions ----------------------------------------------------------------- */

static bool has_plugin(const kr_dev *d, const char *name)
{
    for (int i = 0; i < d->n_plugins; i++)
        if (strcmp(d->plugins[i], name) == 0)
            return true;
    return false;
}

#define BIT(a) (1u << (a))

unsigned kr_dev_actions(const kr_dev *d)
{
    unsigned a = 0;
    if (strcmp(d->state, "requested_by_peer") == 0)
        return BIT(KR_ACT_ACCEPT) | BIT(KR_ACT_REFUSE);
    if (strcmp(d->state, "requested") == 0)
        return BIT(KR_ACT_CANCEL);
    if (!d->paired)
        return d->reachable ? BIT(KR_ACT_PAIR) : 0;
    if (d->reachable) {
        if (has_plugin(d, "ping"))
            a |= BIT(KR_ACT_PING);
        if (has_plugin(d, "findmyphone"))
            a |= BIT(KR_ACT_FIND);
        if (has_plugin(d, "share"))
            a |= BIT(KR_ACT_SEND_FILES) | BIT(KR_ACT_SEND_TEXT);
        if (has_plugin(d, "sftp"))
            a |= BIT(KR_ACT_BROWSE);
    }
    return a | BIT(KR_ACT_UNPAIR);
}

const char *kr_action_label(kr_action a)
{
    switch (a) {
    case KR_ACT_PING:
        return _("Send a ping");
    case KR_ACT_FIND:
        return _("Ring my phone");
    case KR_ACT_SEND_FILES:
        return _("Send files...");
    case KR_ACT_SEND_TEXT:
        return _("Send text...");
    case KR_ACT_BROWSE:
        return _("Browse files");
    case KR_ACT_PAIR:
        return _("Pair");
    case KR_ACT_ACCEPT:
        return _("Accept");
    case KR_ACT_REFUSE:
        return _("Refuse");
    case KR_ACT_CANCEL:
        return _("Cancel pairing");
    case KR_ACT_UNPAIR:
        return _("Unpair");
    case KR_ACT_REFRESH:
        return _("Look for devices");
    case KR_ACT_COUNT:
        break;
    }
    return "";
}

bool kr_action_needs_input(kr_action a)
{
    return a == KR_ACT_SEND_FILES || a == KR_ACT_SEND_TEXT;
}

bool kr_action_destructive(kr_action a)
{
    return a == KR_ACT_UNPAIR || a == KR_ACT_REFUSE;
}

static cJSON *request(const char *type, const char *device_id)
{
    cJSON *r = cJSON_CreateObject();
    if (!r)
        return NULL;
    cJSON_AddStringToObject(r, "type", type);
    if (device_id)
        cJSON_AddStringToObject(r, "device", device_id);
    return r;
}

cJSON *kr_action_request(kr_action a, const char *device_id, const char *arg)
{
    cJSON *r;
    switch (a) {
    case KR_ACT_PING:
        return request("ping", device_id);
    case KR_ACT_FIND:
        return request("find-phone", device_id);
    case KR_ACT_BROWSE:
        return request("browse-files", device_id);
    case KR_ACT_PAIR:
    case KR_ACT_ACCEPT:
        return request("pair", device_id);
    case KR_ACT_REFUSE:
    case KR_ACT_CANCEL:
    case KR_ACT_UNPAIR:
        return request("unpair", device_id);
    case KR_ACT_REFRESH:
        return request("refresh", NULL);
    case KR_ACT_SEND_TEXT:
        if (!arg || !*arg)
            return NULL;
        r = request("share-text", device_id);
        if (r)
            cJSON_AddStringToObject(r, "text", arg);
        return r;
    case KR_ACT_SEND_FILES:
    case KR_ACT_COUNT:
        break;
    }
    return NULL;
}

cJSON *kr_action_files(const char *device_id, const char *const *paths, int n)
{
    cJSON *r = n > 0 ? request("share", device_id) : NULL;
    cJSON *a = r ? cJSON_AddArrayToObject(r, "paths") : NULL;
    if (!a) {
        cJSON_Delete(r);
        return NULL;
    }
    for (int i = 0; i < n; i++)
        cJSON_AddItemToArray(a, cJSON_CreateString(paths[i]));
    return r;
}
