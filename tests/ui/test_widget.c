/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The xispanel widget module, loaded the way xispanel loads it: dlopen() the
 * .so and give it the xpw_*() functions it expects (here, recording ones).
 *
 *   test_widget build/kidesktop/widgets/kiremote.so
 *
 * The command the widget runs for a pairing question is executed for real,
 * with a fake `xisserve` and `kiremote-cli` first in PATH, to see what the
 * shell makes of a phone's name. */
#define _DEFAULT_SOURCE /* mkdtemp */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../harness.h"
#include "xispanel_widget.h"

/* ---- what the module may call (xpw_*), recorded --------------------------------- */

struct XpwWidget {
    void *priv;
    char icon_option[64];
    int w, h;
    uint64_t tick_at;
};

static uint64_t g_now = 1000;
static char *g_runs[32];
static int g_n_runs;
static char *g_texts[32];
static int g_n_texts;
static char g_opened[64];
static int g_icon_draws, g_fallback_draws;
static char g_icon_loaded[64];

static void forget(void)
{
    for (int i = 0; i < g_n_runs; i++)
        free(g_runs[i]);
    for (int i = 0; i < g_n_texts; i++)
        free(g_texts[i]);
    g_n_runs = g_n_texts = 0;
    g_opened[0] = g_icon_loaded[0] = '\0';
    g_icon_draws = g_fallback_draws = 0;
}

void *xpw_priv(XpwWidget *w)
{
    return w->priv;
}

int xpw_option(XpwWidget *w, const char *key, char *out, size_t outsz)
{
    if (strcmp(key, "icon") != 0 || !w->icon_option[0])
        return 0;
    snprintf(out, outsz, "%s", w->icon_option);
    return 1;
}

int xpw_option_int(XpwWidget *w, const char *key, int defval)
{
    (void)w;
    (void)key;
    return defval;
}

void xpw_geometry(XpwWidget *w, int *x, int *y, int *width, int *height)
{
    *x = *y = 0;
    *width = w->w;
    *height = w->h;
}

int xpw_is_vertical(XpwWidget *w)
{
    (void)w;
    return 0;
}

void xpw_colors(XpwWidget *w, double fg[4], double bg[4])
{
    (void)w;
    fg[0] = fg[1] = fg[2] = fg[3] = 1;
    bg[0] = bg[1] = bg[2] = 0;
    bg[3] = 1;
}

void xpw_tick_at(XpwWidget *w, uint64_t at_ms)
{
    w->tick_at = at_ms;
}

void xpw_redraw(XpwWidget *w)
{
    (void)w;
}

uint64_t xpw_now_ms(void)
{
    return g_now;
}

void xpw_paint_hover_bg(XpwWidget *w, cairo_t *cr)
{
    (void)w;
    (void)cr;
}

double xpw_text_size(XpwWidget *w)
{
    (void)w;
    return 12;
}

double xpw_text_width(XpwWidget *w, const char *text)
{
    (void)w;
    return 7.0 * (double)strlen(text);
}

void xpw_draw_text(XpwWidget *w, cairo_t *cr, double x, double top_y, double box_h, const char *text)
{
    (void)w;
    (void)cr;
    (void)x;
    (void)top_y;
    (void)box_h;
    if (g_n_texts < 32)
        g_texts[g_n_texts++] = strdup(text);
}

cairo_surface_t *xpw_load_icon(const char *name_or_path, int px)
{
    snprintf(g_icon_loaded, sizeof g_icon_loaded, "%s", name_or_path);
    if (strcmp(name_or_path, "no-such-icon") == 0)
        return NULL;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_destroy(cr);
    return s;
}

void xpw_draw_icon(cairo_t *cr, cairo_surface_t *icon, double x, double y, double size)
{
    (void)size;
    g_icon_draws++;
    cairo_set_source_surface(cr, icon, x, y);
    cairo_paint(cr);
}

void xpw_draw_fallback_icon(XpwWidget *w, cairo_t *cr, double x, double y, double size, const char *title)
{
    (void)w;
    (void)cr;
    (void)x;
    (void)y;
    (void)size;
    (void)title;
    g_fallback_draws++;
}

void xpw_run(const char *cmd)
{
    if (g_n_runs < 32)
        g_runs[g_n_runs++] = strdup(cmd);
}

void xpw_open_xisserve(XpwWidget *w, const char *cmd_name, const char *extra_args)
{
    (void)w;
    (void)cmd_name;
    snprintf(g_opened, sizeof g_opened, "%s", extra_args ? extra_args : "");
}

uint64_t xpw_async(const char *cmd, unsigned refresh_ms, const char **out_text)
{
    (void)cmd;
    (void)refresh_ms;
    (void)out_text;
    return 0;
}

/* ---- the rig ----------------------------------------------------------------------- */

static const char *g_so;
static char g_tmp[64], g_status[128], g_bin[128];
static const XispanelWidgetModule *g_mod;

static const XispanelWidgetModule *load(void)
{
    void *h = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return NULL;
    }
    XispanelWidgetEntryFn entry;
    *(void **)(&entry) = dlsym(h, XISPANEL_WIDGET_ENTRY);
    return entry ? entry() : NULL;
}

static XpwWidget *new_widget(const char *icon)
{
    XpwWidget *w = calloc(1, sizeof *w);
    w->priv = calloc(1, g_mod->priv_size);
    w->w = w->h = 32;
    if (icon)
        snprintf(w->icon_option, sizeof w->icon_option, "%s", icon);
    if (g_mod->init(w) != 0) {
        free(w->priv);
        free(w);
        return NULL;
    }
    return w;
}

static void free_widget(XpwWidget *w)
{
    g_mod->destroy(w);
    free(w->priv);
    free(w);
}

static void write_status(const char *text)
{
    FILE *f = fopen(g_status, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void status_with(int pid, const char *devices)
{
    char buf[4096];
    snprintf(buf, sizeof buf, "{\"pid\":%d,\"version\":\"t\",\"id\":\"me\",\"name\":\"Mesa\",\"type\":\"desktop\",\"devices\":%s}", pid,
             devices);
    write_status(buf);
}

#define CONNECTED                                                                                      \
    "[{\"id\":\"b1\",\"name\":\"moto g\",\"type\":\"phone\",\"reachable\":true,\"paired\":true,"       \
    "\"state\":\"paired\",\"battery\":{\"charge\":80,\"charging\":true}}]"

static int tick(XpwWidget *w)
{
    g_now += 2000;
    return g_mod->on_tick(w, g_now);
}

static void paint(XpwWidget *w)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w->w, w->h);
    cairo_t *cr = cairo_create(s);
    g_mod->paint(w, cr);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return strdup("");
    char *buf = calloc(1, 8192);
    size_t n = fread(buf, 1, 8191, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void fresh(void)
{
    forget();
    unlink(g_status);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -f %s/kiremote-asked-* %s/cli.log %s/xisserve.log %s/X", g_tmp, g_tmp, g_tmp, g_tmp);
    (void)!system(cmd);
}

/* ---- the tests ----------------------------------------------------------------------- */

TEST(the_module_speaks_the_current_abi)
{
    CHECK(g_mod);
    CHECK_INT_EQ(g_mod->abi, XISPANEL_WIDGET_ABI);
    CHECK(g_mod->embeddable);
    CHECK(g_mod->priv_size > 0);
    CHECK(g_mod->paint && g_mod->on_tick && g_mod->on_button && g_mod->get_tooltip && g_mod->is_urgent);
}

TEST(without_a_daemon_it_says_so)
{
    fresh();
    XpwWidget *w = new_widget(NULL);
    CHECK(w);
    CHECK(w->tick_at > 0); /* it asks to be ticked */
    CHECK_INT_EQ(tick(w), 0);
    char tip[200];
    CHECK(g_mod->get_tooltip(w, tip, sizeof tip));
    CHECK_STR_EQ(tip, "Kiremote is not running");
    CHECK(!g_mod->is_urgent(w));
    paint(w);
    CHECK_INT_EQ(g_n_texts, 0); /* no badge */
    CHECK_STR_EQ(g_icon_loaded, "phone");
    CHECK_INT_EQ(g_icon_draws, 1);
    free_widget(w);
}

TEST(the_icon_option_and_a_missing_icon)
{
    fresh();
    XpwWidget *w = new_widget("no-such-icon");
    paint(w);
    CHECK_STR_EQ(g_icon_loaded, "no-such-icon");
    CHECK_INT_EQ(g_fallback_draws, 1); /* a letter instead of a hole */
    free_widget(w);
}

TEST(connected_devices_show_a_count_and_the_batteries)
{
    fresh();
    XpwWidget *w = new_widget(NULL);
    status_with((int)getpid(), CONNECTED);
    CHECK_INT_EQ(tick(w), 1);
    char tip[200];
    g_mod->get_tooltip(w, tip, sizeof tip);
    CHECK_STR_EQ(tip, "moto g - 80% (charging)");
    paint(w);
    CHECK_INT_EQ(g_n_texts, 1);
    CHECK_STR_EQ(g_texts[0], "1");
    CHECK(!g_mod->is_urgent(w));
    CHECK_INT_EQ(tick(w), 0); /* nothing changed: no repaint of the bar */
    CHECK_INT_EQ((int)(w->tick_at - g_now), 2000);
    free_widget(w);
}

TEST(a_crashed_daemons_file_is_not_believed)
{
    fresh();
    XpwWidget *w = new_widget(NULL);
    status_with((int)getpid(), CONNECTED);
    CHECK_INT_EQ(tick(w), 1);
    pid_t child = fork();
    if (child == 0)
        _exit(0);
    waitpid(child, NULL, 0);
    status_with((int)child, CONNECTED);
    CHECK_INT_EQ(tick(w), 1); /* it is down now */
    char tip[200];
    g_mod->get_tooltip(w, tip, sizeof tip);
    CHECK_STR_EQ(tip, "Kiremote is not running");
    unlink(g_status);
    free_widget(w);
}

TEST(clicks)
{
    fresh();
    XpwWidget *w = new_widget(NULL);
    CHECK(g_mod->on_button(w, 1, 5, 5));
    CHECK_STR_EQ(g_opened, "--kiremote");
    CHECK(g_mod->on_button(w, 2, 5, 5));
    CHECK_INT_EQ(g_n_runs, 1);
    CHECK_STR_EQ(g_runs[0], "kiconf --tab kiremote");
    CHECK(!g_mod->on_button(w, 3, 5, 5)); /* the panel's own menu */
    CHECK(!g_mod->on_button(w, 4, 5, 5));
    int len = 0, min = 0;
    g_mod->measure(w, 40, &len, &min);
    CHECK_INT_EQ(len, 40);
    CHECK(min <= len && min > 0);
    free_widget(w);
}

/* ---- pairing questions ---------------------------------------------------------------- */

static void fake_programs(void)
{
    char path[320];
    snprintf(path, sizeof path, "%s/xisserve", g_bin);
    FILE *f = fopen(path, "w");
    fprintf(f, "#!/bin/sh\nfor a in \"$@\"; do printf '%%s\\n' \"$a\"; done >> '%s/xisserve.log'\n"
               "echo -- >> '%s/xisserve.log'\n"
               "[ -n \"$FAKE_ANSWER\" ] && { echo \"$FAKE_ANSWER\"; exit 0; }\nexit 1\n", g_tmp, g_tmp);
    fclose(f);
    chmod(path, 0755);
    snprintf(path, sizeof path, "%s/kiremote-cli", g_bin);
    f = fopen(path, "w");
    fprintf(f, "#!/bin/sh\necho \"$*\" >> '%s/cli.log'\n", g_tmp);
    fclose(f);
    chmod(path, 0755);
}

#define EVIL "Zé's \\\"phone\\\"; touch X; $(touch X) `touch X`"
#define ASKING(id, key, name)                                                                         \
    "[{\"id\":\"" id "\",\"name\":\"" name "\",\"type\":\"phone\",\"reachable\":true,\"paired\":false," \
    "\"state\":\"requested_by_peer\",\"key\":\"" key "\"}]"

static void run_last_command(const char *answer)
{
    char cmd[3000];
    snprintf(cmd, sizeof cmd, "cd '%s' && PATH='%s':\"$PATH\" FAKE_ANSWER='%s' sh -c \"$KR_CMD\"", g_tmp, g_bin, answer);
    setenv("KR_CMD", g_runs[g_n_runs - 1], 1);
    CHECK_INT_EQ(system(cmd), 0);
}

TEST(a_pairing_request_is_asked_once)
{
    fresh();
    XpwWidget *w = new_widget(NULL);
    status_with((int)getpid(), ASKING("abc-1", "AABBCCDD", "Pedinte"));
    CHECK_INT_EQ(tick(w), 1);
    CHECK_INT_EQ(g_n_runs, 1);
    CHECK_STR_HAS(g_runs[0], "xisserve --question");
    CHECK(g_mod->is_urgent(w));
    char tip[200];
    g_mod->get_tooltip(w, tip, sizeof tip);
    CHECK_STR_EQ(tip, "A device wants to pair");
    paint(w);
    CHECK_STR_EQ(g_texts[0], "!");

    CHECK_INT_EQ(tick(w), 0);
    CHECK_INT_EQ(g_n_runs, 1); /* not again on the next tick */

    /* Another bar showing the same widget (another monitor), or a restarted panel. */
    XpwWidget *w2 = new_widget(NULL);
    tick(w2);
    CHECK_INT_EQ(g_n_runs, 1);

    /* Answered: the request is over, and a new one (a new key) is asked. */
    status_with((int)getpid(), CONNECTED);
    tick(w);
    tick(w2);
    status_with((int)getpid(), ASKING("abc-1", "11223344", "Pedinte"));
    tick(w);
    CHECK_INT_EQ(g_n_runs, 2);
    free_widget(w);
    free_widget(w2);
}

TEST(the_question_is_answered_through_the_cli)
{
    fresh();
    fake_programs();
    XpwWidget *w = new_widget(NULL);
    status_with((int)getpid(), ASKING("abc-1", "AABBCCDD", "Pedinte"));
    tick(w);
    CHECK_INT_EQ(g_n_runs, 1);

    run_last_command("1"); /* Accept */
    char *cli = slurp("/dev/null");
    free(cli);
    char path[320];
    snprintf(path, sizeof path, "%s/cli.log", g_tmp);
    cli = slurp(path);
    CHECK_STR_EQ(cli, "-d abc-1 --pair\n");
    free(cli);

    unlink(path);
    run_last_command("0"); /* Refuse */
    cli = slurp(path);
    CHECK_STR_EQ(cli, "-d abc-1 --unpair\n");
    free(cli);

    unlink(path);
    run_last_command(""); /* the popup was dismissed: no answer, nothing done */
    cli = slurp(path);
    CHECK_STR_EQ(cli, "");
    free(cli);

    /* What it asked: the name, the key, and the two buttons. */
    snprintf(path, sizeof path, "%s/xisserve.log", g_tmp);
    char *xl = slurp(path);
    CHECK_STR_HAS(xl, "--question\n");
    CHECK_STR_HAS(xl, "--text=Pedinte wants to pair");
    CHECK_STR_HAS(xl, "Verification key: AABBCCDD");
    CHECK_STR_HAS(xl, "--button=Refuse:0\n");
    CHECK_STR_HAS(xl, "--button=Accept:1\n");
    free(xl);
    free_widget(w);
}

/* The name is whatever the phone says (cleaned by the daemon, but no more than
 * the protocol asks): the shell must see it as one word of text. */
TEST(a_hostile_name_stays_text)
{
    fresh();
    fake_programs();
    XpwWidget *w = new_widget(NULL);
    status_with((int)getpid(), ASKING("abc-1", "AABBCCDD", EVIL));
    tick(w);
    CHECK_INT_EQ(g_n_runs, 1);
    run_last_command("1");
    char path[320];
    snprintf(path, sizeof path, "%s/X", g_tmp);
    CHECK(access(path, F_OK) != 0); /* nothing was executed from the name */
    snprintf(path, sizeof path, "%s/xisserve.log", g_tmp);
    char *xl = slurp(path);
    CHECK_STR_HAS(xl, "--text=Z\xc3\xa9's \"phone\"; touch X; $(touch X) `touch X` wants to pair");
    free(xl);
    snprintf(path, sizeof path, "%s/cli.log", g_tmp);
    char *cli = slurp(path);
    CHECK_STR_EQ(cli, "-d abc-1 --pair\n");
    free(cli);
    free_widget(w);
}

/* The id and key become part of a file name and a command line. */
TEST(odd_ids_and_keys_are_not_asked)
{
    fresh();
    XpwWidget *w = new_widget(NULL);
    status_with((int)getpid(), ASKING("../etc", "AABBCCDD", "x"));
    tick(w);
    status_with((int)getpid(), ASKING("a b", "AABBCCDD", "x"));
    tick(w);
    status_with((int)getpid(), ASKING("abc", "AA/BB", "x"));
    tick(w);
    status_with((int)getpid(), ASKING("abc", "", "x"));
    tick(w);
    CHECK_INT_EQ(g_n_runs, 0);
    free_widget(w);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s widget.so\n", argv[0]);
        return 2;
    }
    g_so = argv[1];
    snprintf(g_tmp, sizeof g_tmp, "/tmp/krwidget.XXXXXX");
    if (!mkdtemp(g_tmp))
        return 1;
    snprintf(g_bin, sizeof g_bin, "%s/bin", g_tmp);
    mkdir(g_bin, 0700);
    snprintf(g_status, sizeof g_status, "%s/kiremote-status.json", g_tmp);
    setenv("XDG_RUNTIME_DIR", g_tmp, 1);
    setenv("LANGUAGE", "en", 1);
    setenv("LC_ALL", "C", 1);

    g_mod = load();
    RUN(the_module_speaks_the_current_abi);
    if (g_mod) {
        RUN(without_a_daemon_it_says_so);
        RUN(the_icon_option_and_a_missing_icon);
        RUN(connected_devices_show_a_count_and_the_batteries);
        RUN(a_crashed_daemons_file_is_not_believed);
        RUN(clicks);
        RUN(a_pairing_request_is_asked_once);
        RUN(the_question_is_answered_through_the_cli);
        RUN(a_hostile_name_stays_text);
        RUN(odd_ids_and_keys_are_not_asked);
    }
    int rc = harness_report();
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_tmp);
    (void)!system(cmd);
    forget();
    return rc;
}
