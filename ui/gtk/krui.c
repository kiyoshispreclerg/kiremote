/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "krui.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "krui_private.h"
#include "util.h"

/* ---- the client in GLib's main loop --------------------------------------- */

typedef struct {
    GSource base;
    kr_client *client;
    GPollFD pfd;
    int fd; /* the one added to the source, -1 for none */
} client_source;

static gushort to_gio(short ev)
{
    gushort g = G_IO_ERR | G_IO_HUP;
    if (ev & POLLIN)
        g |= G_IO_IN;
    if (ev & POLLOUT)
        g |= G_IO_OUT;
    return g;
}

/* The client's descriptor changes when it reconnects, and it asks for
 * POLLOUT only while it has output waiting. */
static void sync_poll(client_source *s)
{
    int fd = kr_client_fd(s->client);
    if (fd != s->fd) {
        if (s->fd >= 0)
            g_source_remove_poll(&s->base, &s->pfd);
        s->fd = fd;
        if (fd >= 0) {
            s->pfd.fd = fd;
            s->pfd.revents = 0;
            g_source_add_poll(&s->base, &s->pfd);
        }
    }
    if (fd >= 0)
        s->pfd.events = to_gio(kr_client_events(s->client));
}

static gboolean src_prepare(GSource *src, gint *timeout)
{
    client_source *s = (client_source *)src;
    sync_poll(s);
    double d = kr_client_deadline(s->client);
    if (d <= 0) {
        *timeout = -1;
        return FALSE;
    }
    double ms = (d - kr_now()) * 1000;
    *timeout = ms <= 0 ? 0 : (gint)ms + 1;
    return ms <= 0;
}

static gboolean src_check(GSource *src)
{
    client_source *s = (client_source *)src;
    double d = kr_client_deadline(s->client);
    return (s->fd >= 0 && s->pfd.revents) || (d > 0 && kr_now() >= d);
}

static gboolean src_dispatch(GSource *src, GSourceFunc cb, gpointer ud)
{
    client_source *s = (client_source *)src;
    (void)cb;
    (void)ud;
    if (s->fd >= 0 && s->pfd.revents) {
        short ev = 0;
        if (s->pfd.revents & (G_IO_IN | G_IO_HUP | G_IO_ERR))
            ev |= POLLIN;
        if (s->pfd.revents & G_IO_OUT)
            ev |= POLLOUT;
        s->pfd.revents = 0;
        kr_client_dispatch(s->client, ev);
    }
    kr_client_tick(s->client, kr_now());
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs source_funcs = {src_prepare, src_check, src_dispatch, NULL, NULL, NULL};

/* ---- the context ------------------------------------------------------------ */

typedef struct {
    guint id;
    kr_ui_listener fn;
    void *ud;
} listener;

struct kr_ui {
    kr_client *client;
    client_source *source;
    GArray *listeners; /* listener */
    guint next_id;
    kr_ui_hooks hooks;
    GList *pair_dialogs; /* krui_private.h */
    gboolean closing;
    GSList *owned; /* freed with the kr_ui */
};

static void on_change(void *ud, kr_change what, const char *id)
{
    kr_ui *ui = ud;
    /* A listener may add or remove listeners: walk a copy, and skip the ones
     * removed meanwhile. */
    GArray *copy = g_array_sized_new(FALSE, FALSE, sizeof(listener), ui->listeners->len);
    g_array_append_vals(copy, ui->listeners->data, ui->listeners->len);
    for (guint i = 0; i < copy->len; i++) {
        listener l = g_array_index(copy, listener, i);
        for (guint k = 0; k < ui->listeners->len; k++)
            if (g_array_index(ui->listeners, listener, k).id == l.id) {
                l.fn(l.ud, what, id);
                break;
            }
    }
    g_array_free(copy, TRUE);
}

kr_ui *kr_ui_new(const char *socket_path)
{
    kr_ui *ui = g_new0(kr_ui, 1);
    ui->listeners = g_array_new(FALSE, FALSE, sizeof(listener));
    ui->next_id = 1;
    kr_ui_default_hooks(&ui->hooks);

    static const kr_client_ops ops = {on_change};
    ui->client = kr_client_new(socket_path, &ops, ui);
    if (!ui->client) {
        g_array_free(ui->listeners, TRUE);
        g_free(ui);
        return NULL;
    }
    ui->source = (client_source *)g_source_new(&source_funcs, sizeof(client_source));
    ui->source->client = ui->client;
    ui->source->fd = -1;
    g_source_attach(&ui->source->base, NULL);
    return ui;
}

void kr_ui_free(kr_ui *ui)
{
    if (!ui)
        return;
    ui->closing = TRUE; /* requests still waiting are answered with NULL: say nothing */
    g_source_destroy(&ui->source->base);
    g_source_unref(&ui->source->base);
    kr_client_free(ui->client);
    g_array_free(ui->listeners, TRUE);
    g_list_free(ui->pair_dialogs);
    g_slist_free_full(ui->owned, g_free);
    g_free(ui);
}

kr_client *kr_ui_client(kr_ui *ui)
{
    return ui->client;
}

const kr_model *kr_ui_model(kr_ui *ui)
{
    return kr_client_model(ui->client);
}

guint kr_ui_listen(kr_ui *ui, kr_ui_listener fn, void *ud)
{
    listener l = {ui->next_id++, fn, ud};
    g_array_append_val(ui->listeners, l);
    return l.id;
}

void kr_ui_unlisten(kr_ui *ui, guint id)
{
    for (guint i = 0; i < ui->listeners->len; i++)
        if (g_array_index(ui->listeners, listener, i).id == id) {
            g_array_remove_index(ui->listeners, i);
            return;
        }
}

void kr_ui_set_hooks(kr_ui *ui, const kr_ui_hooks *hooks)
{
    kr_ui_default_hooks(&ui->hooks);
    if (hooks) {
        if (hooks->confirm)
            ui->hooks.confirm = hooks->confirm;
        if (hooks->choose_files)
            ui->hooks.choose_files = hooks->choose_files;
        if (hooks->ask_text)
            ui->hooks.ask_text = hooks->ask_text;
        if (hooks->show_error)
            ui->hooks.show_error = hooks->show_error;
    }
}

const kr_ui_hooks *kr_ui_get_hooks(kr_ui *ui)
{
    return &ui->hooks;
}

GList **kr_ui_pair_dialogs(kr_ui *ui)
{
    return &ui->pair_dialogs;
}

gboolean kr_ui_closing(kr_ui *ui)
{
    return ui->closing;
}

void kr_ui_own(kr_ui *ui, gpointer mem)
{
    ui->owned = g_slist_prepend(ui->owned, mem);
}
