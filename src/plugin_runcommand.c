/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The phone runs commands on the desktop (plugins/runcommand upstream;
 * and/plugins/runcommand for the client).
 *
 *   in  kdeconnect.runcommand.request
 *         {"requestCommandList": true}          send the list
 *         {"key": "<name>"}                     run that command
 *         {"stop": ...}                         end the ones it started
 *         {"setup": ...}                        (a config dialog: ignored)
 *   out kdeconnect.runcommand
 *         {"commandList": "<JSON text>", "canAddCommand": false}
 *           the list is a JSON object *inside a string*:
 *           {"<key>": {"name": "...", "command": "..."}, ...}
 *   out kdeconnect.runcommand.output
 *         {"commandStarted": true, "command", "id"}
 *         {"commandOutput": true, "stdout": [lines], "stderr": [], "id"}
 *         {"commandFinished": true, "success", "exitCode", "id"}
 *
 * The commands are the phone-visible ones the user wrote in the config,
 * and only those can run: the phone names a key, never a command line.
 *
 *   [runcommand]
 *   Lock screen = loginctl lock-session
 *   Play/pause  = playerctl play-pause
 *
 * The line's name is also its key. Commands run through sh -c, keep the
 * user's environment, and their standard error is not sent (it joins the
 * output). Output is sent in batches of five lines, at most 200 lines. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"
#include "util.h"

#define REQUEST "kdeconnect.runcommand.request"
#define LIST "kdeconnect.runcommand"
#define OUTPUT "kdeconnect.runcommand.output"
#define MAX_RUNNING 16
#define BATCH 5
#define MAX_LINES 200

static const char *const incoming[] = {REQUEST, NULL};
static const char *const outgoing[] = {LIST, OUTPUT, NULL};

typedef struct {
    kr_daemon *d;
    kr_proc *proc;
    unsigned id;
    char dev_id[64];
    char batch[BATCH][512];
    int n_batch;
    int sent_lines;
} running;

static running *runs[MAX_RUNNING];
static unsigned next_id;

static void send_packet(running *r, kr_packet *p)
{
    kr_device *dev = kr_daemon_find_device(r->d, r->dev_id);
    if (dev)
        kr_daemon_send(r->d, dev, p);
    kr_packet_free(p);
}

static void flush_batch(running *r)
{
    if (!r->n_batch)
        return;
    kr_packet *p = kr_packet_new(OUTPUT);
    cJSON *out = p ? cJSON_AddArrayToObject(p->body, "stdout") : NULL;
    if (!out) {
        kr_packet_free(p);
        r->n_batch = 0;
        return;
    }
    for (int i = 0; i < r->n_batch; i++)
        cJSON_AddItemToArray(out, cJSON_CreateString(r->batch[i]));
    cJSON_AddBoolToObject(p->body, "commandOutput", true);
    cJSON_AddItemToObject(p->body, "stderr", cJSON_CreateArray());
    cJSON_AddNumberToObject(p->body, "id", r->id);
    r->n_batch = 0;
    send_packet(r, p);
}

static void on_line(void *ud, const char *line)
{
    running *r = ud;
    if (r->sent_lines >= MAX_LINES)
        return;
    r->sent_lines++;
    /* The phone parses JSON: only valid UTF-8. */
    if (!kr_utf8_valid(line, strlen(line)))
        line = "?";
    snprintf(r->batch[r->n_batch++], sizeof r->batch[0], "%s", line);
    if (r->n_batch == BATCH)
        flush_batch(r);
}

static void on_exit_cb(void *ud, int status, const char *out, size_t len)
{
    (void)out;
    (void)len;
    running *r = ud;
    flush_batch(r);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    kr_packet *p = kr_packet_new(OUTPUT);
    if (p) {
        cJSON_AddBoolToObject(p->body, "commandFinished", true);
        cJSON_AddBoolToObject(p->body, "success", code == 0);
        cJSON_AddNumberToObject(p->body, "exitCode", code);
        cJSON_AddNumberToObject(p->body, "id", r->id);
        send_packet(r, p);
    }
    for (int i = 0; i < MAX_RUNNING; i++)
        if (runs[i] == r)
            runs[i] = NULL;
    free(r);
}

/* The configured command named key, or NULL. */
static const char *command_for(kr_daemon *d, const char *key)
{
    int i = 0;
    const char *k, *v;
    while (kr_config_each(kr_daemon_config(d), "runcommand", &i, &k, &v))
        if (strcmp(k, key) == 0 && v[0])
            return v;
    return NULL;
}

static void send_list(kr_daemon *d, kr_device *dev)
{
    cJSON *all = cJSON_CreateObject();
    int i = 0;
    const char *k, *v;
    while (all && kr_config_each(kr_daemon_config(d), "runcommand", &i, &k, &v)) {
        cJSON *c = cJSON_AddObjectToObject(all, k);
        if (c) {
            cJSON_AddStringToObject(c, "name", k);
            cJSON_AddStringToObject(c, "command", v);
        }
    }
    char *text = all ? cJSON_PrintUnformatted(all) : NULL;
    kr_packet *p = text ? kr_packet_new(LIST) : NULL;
    if (p) {
        cJSON_AddStringToObject(p->body, "commandList", text);
        cJSON_AddBoolToObject(p->body, "canAddCommand", false);
        kr_daemon_send(d, dev, p);
    }
    kr_packet_free(p);
    cJSON_free(text);
    cJSON_Delete(all);
}

static void start(kr_daemon *d, kr_device *dev, const char *key)
{
    const char *command = command_for(d, key);
    if (!command) {
        kr_log_warn(_("%s asked for a command that is not configured: %s"), kr_device_name(dev), key);
        return;
    }
    int slot = -1;
    for (int i = 0; i < MAX_RUNNING && slot < 0; i++)
        if (!runs[i])
            slot = i;
    running *r = slot >= 0 ? calloc(1, sizeof *r) : NULL;
    if (!r)
        return;
    r->d = d;
    r->id = next_id++;
    snprintf(r->dev_id, sizeof r->dev_id, "%s", kr_device_id(dev));
    const char *argv[] = {"sh", "-c", command, NULL};
    kr_log_info(_("running for %s: %s"), kr_device_name(dev), command);
    r->proc = kr_proc_spawn_ex(kr_daemon_procs(d), argv, KR_PROC_USER | KR_PROC_STDERR,
                               &(kr_proc_cb){.on_line = on_line, .on_exit = on_exit_cb}, r);
    if (!r->proc) {
        kr_log_warn(_("cannot run %s: %s"), command, strerror(errno));
        free(r);
        return;
    }
    runs[slot] = r;
    kr_packet *p = kr_packet_new(OUTPUT);
    if (p) {
        cJSON_AddBoolToObject(p->body, "commandStarted", true);
        cJSON_AddStringToObject(p->body, "command", command);
        cJSON_AddNumberToObject(p->body, "id", r->id);
        send_packet(r, p);
    }
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    if (kr_packet_bool(p, "requestCommandList", false)) {
        send_list(d, dev);
        return;
    }
    const char *key = kr_packet_str(p, "key");
    if (key) {
        start(d, dev, key);
    } else if (cJSON_GetObjectItemCaseSensitive(p->body, "stop")) {
        for (int i = 0; i < MAX_RUNNING; i++)
            if (runs[i] && strcmp(runs[i]->dev_id, kr_device_id(dev)) == 0)
                kr_proc_kill(runs[i]->proc);
    }
}

static void on_disconnect(kr_daemon *d, kr_device *dev)
{
    (void)d;
    /* Nobody is listening for the output; the commands themselves go on. */
    for (int i = 0; i < MAX_RUNNING; i++)
        if (runs[i] && strcmp(runs[i]->dev_id, kr_device_id(dev)) == 0)
            runs[i]->dev_id[0] = '\0';
}

static void shutdown_plugin(kr_daemon *d)
{
    (void)d;
    for (int i = 0; i < MAX_RUNNING; i++)
        if (runs[i]) {
            kr_proc_detach(runs[i]->proc);
            free(runs[i]);
            runs[i] = NULL;
        }
}

const kr_plugin kr_plugin_runcommand = {
    .name = "runcommand",
    .incoming = incoming,
    .outgoing = outgoing,
    .shutdown = shutdown_plugin,
    .on_disconnect = on_disconnect,
    .on_packet = on_packet,
};
