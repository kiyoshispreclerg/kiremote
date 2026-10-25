/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "schema.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "i18n.h"
#include "packet.h"
#include "pointer_img.h"
#include "v4linfo.h"
#include "util.h"

#define MAX_TEXT 1024
#define MAX_ADDRS 16

static const char *const device_types[] = {"desktop", "laptop", "phone", "tablet", "tv", NULL};
static const char *const players[] = {"", "paplay", "pw-play", "aplay", NULL};
static const char *const tray_choices[] = {"auto", "yes", "no", NULL};
static const char *const mirror_sizes[] = {"", "800", "1024", "1280", "1600", "1920", NULL};
static const char *const mirror_rates[] = {"", "2M", "4M", "8M", "16M", "32M", NULL};
static const char *const mirror_fps[] = {"", "15", "24", "30", "60", NULL};
static const char *const mirror_codecs[] = {"h264", "h265", "av1", NULL};
static const char *const camera_facings[] = {"back", "front", "external", NULL};
static const char *const camera_sizes[] = {"640", "960", "1280", "1920", NULL};
static const char *const pointer_shapes[] = {"circle", "ring", "square", "diamond", "cross", NULL};
static const char *const pointer_sizes[] = {"small", "medium", "large", "huge", NULL};
static const char *const pointer_hides[] = {"0.5", "1", "2", "5", "0", NULL};

/* How a choice is shown (msgids); a choice not listed is shown as it is. */
static const struct {
    const char *value, *label;
} choice_labels[] = {
    {"", N_("(default)")},
    {"auto", N_("Automatic")},
    {"yes", N_("Always")},
    {"no", N_("Never")},
    {"desktop", N_("Desktop")},
    {"laptop", N_("Laptop")},
    {"phone", N_("Phone")},
    {"tablet", N_("Tablet")},
    {"tv", N_("TV")},
    {"back", N_("Back")},
    {"front", N_("Front (selfie)")},
    {"external", N_("External (USB)")},
    {"circle", N_("Circle")},
    {"ring", N_("Ring")},
    {"square", N_("Square")},
    {"diamond", N_("Diamond")},
    {"cross", N_("Cross")},
    {"small", N_("Small")},
    {"medium", N_("Medium")},
    {"large", N_("Large")},
    {"huge", N_("Huge")},
    {"0.5", N_("Half a second")},
    {"1", N_("1 second")},
    {"2", N_("2 seconds")},
    {"5", N_("5 seconds")},
    {"0", N_("Never (until the phone stops)")},
};

static const kr_setting_group groups[] = {
    {"device", N_("This computer")},
    {"network", N_("Network")},
    {"files", N_("Files")},
    {"features", N_("Features")},
    {"clipboard", N_("Clipboard")},
    {"media", N_("Media")},
    {"presenter", N_("Presentation pointer")},
    {"find", N_("Find my phone")},
    {"lock", N_("Screen lock")},
    {"commands", N_("Commands")},
    {"mirror", N_("Mirroring")},
    {"camera", N_("Camera")},
    {"interface", N_("Interface")},
    {"advanced", N_("Advanced")},
};

#define PLUGIN(name_, label_, help_, def_) \
    {"plugins", name_, KR_S_BOOL, def_, NULL, "features", label_, help_, NULL, true}

/* The plugins come in the order of kr_plugins; test_schema checks that
 * every one of them has a line here. */
static const kr_setting settings[] = {
    {"device", "name", KR_S_NAME, "", NULL, "device", N_("Name"),
     N_("How the phone calls this computer. Empty: the host name."), NULL, true},
    {"device", "type", KR_S_ENUM, "desktop", device_types, "device", N_("Type"),
     N_("Only changes the icon on the phone."), NULL, true},

    {"network", "addresses", KR_S_ADDRS, "", NULL, "network", N_("Extra addresses"),
     N_("IPv4 addresses that also get our discovery packet, separated by commas. For networks "
        "that block broadcasts."),
     NULL, true},

    {"share", "dir", KR_S_PATH, "", NULL, "files", N_("Received files"),
     N_("Where files from the phone go. Empty: your downloads folder."), "share", false},
    {"sftp", "sshfs", KR_S_STRING, "", NULL, "advanced", N_("sshfs program"),
     N_("The program that mounts the phone's files. Empty: sshfs."), "sftp", false},

    PLUGIN("ping", N_("Ping"), N_("Send and receive pings."), "true"),
    PLUGIN("share", N_("Share files and text"), N_("Send and receive files, text and links."), "true"),
    PLUGIN("battery", N_("Battery"), N_("Show the phone's battery and tell it ours."), "true"),
    PLUGIN("connectivity_report", N_("Signal"), N_("Show the phone's mobile signal."), "true"),
    PLUGIN("findmyphone", N_("Find my phone"), N_("Make the phone ring, and answer when it looks for this computer."), "true"),
    PLUGIN("telephony", N_("Calls"), N_("Notify about calls and let you mute the ringer."), "true"),
    PLUGIN("pausemusic", N_("Pause media on calls"), N_("Pause the players here while the phone rings."), "true"),
    PLUGIN("mpris", N_("Media players"), N_("Control this computer's players from the phone."), "true"),
    PLUGIN("systemvolume", N_("Volume"), N_("Let the phone change this computer's volume."), "true"),
    PLUGIN("clipboard", N_("Clipboard"), N_("Share the clipboard with the phone."), "true"),
    PLUGIN("notifications", N_("Phone notifications"), N_("Show the phone's notifications here."), "true"),
    PLUGIN("runcommand", N_("Run commands"), N_("Let the phone run the commands listed under Commands."), "true"),
    PLUGIN("mousepad", N_("Remote mouse and keyboard"), N_("Let the phone move the pointer and type here."), "true"),
    PLUGIN("presenter", N_("Presentation pointer"), N_("Let the phone point at the screen during a presentation."), "true"),
    PLUGIN("lockdevice", N_("Lock the screen"), N_("Let the phone lock this computer."), "true"),
    PLUGIN("screensaver_inhibit", N_("Keep the screen awake"), N_("Do not let the screen sleep while a phone is connected."), "false"),
    PLUGIN("sftp", N_("Browse phone files"), N_("Mount the phone's storage as a folder."), "true"),

    {"clipboard", "auto_share", KR_S_BOOL, "true", NULL, "clipboard", N_("Send what I copy"),
     N_("Send what is copied here to the phone as soon as it is copied."), "clipboard", false},
    {"clipboard", "share_passwords", KR_S_BOOL, "false", NULL, "clipboard", N_("Send passwords too"),
     N_("Also send what a password manager marks as secret."), "clipboard", false},

    {"pausemusic", "only_when_talking", KR_S_BOOL, "false", NULL, "media", N_("Pause only when answered"),
     N_("Wait until the call is answered instead of pausing while it rings."), "pausemusic", false},
    {"pausemusic", "resume", KR_S_BOOL, "true", NULL, "media", N_("Resume afterwards"),
     N_("Start the players again when the call ends."), "pausemusic", false},

    {"find", "sound", KR_S_PATH, "", NULL, "find", N_("Sound"),
     N_("The sound played when the phone looks for this computer. Empty: a beep."), "findmyphone", false},
    {"find", "player", KR_S_ENUM, "", players, "find", N_("Player"),
     N_("The program that plays it. Empty: the first one found."), "findmyphone", false},

    /* The look of the presentation pointer (src/plugin_presenter.c). */
    {"pointer", "shape", KR_S_ENUM, "circle", pointer_shapes, "presenter", N_("Shape"),
     N_("What the pointer looks like. An image below replaces it."), "presenter", false},
    {"pointer", "color", KR_S_COLOR, "#ff0000", NULL, "presenter", N_("Color"), N_("The color of the pointer."),
     "presenter", false},
    {"pointer", "outline", KR_S_BOOL, "true", NULL, "presenter", N_("Outline"),
     N_("Draw a thin line around the pointer so it shows on any background."), "presenter", false},
    {"pointer", "outline_color", KR_S_COLOR, "#ffffff", NULL, "presenter", N_("Outline color"),
     N_("The color of that line."), "presenter", false},
    {"pointer", "size", KR_S_ENUM, "medium", pointer_sizes, "presenter", N_("Size"),
     N_("How big the pointer is, compared to the screen."), "presenter", false},
    {"pointer", "image", KR_S_PATH, "", NULL, "presenter", N_("Image"),
     N_("A PNG or GIF (it can be animated) to use as the pointer. Transparent parts are see-through, "
        "with no soft edges. Empty: use the shape."),
     "presenter", false},
    {"pointer", "hide_after", KR_S_ENUM, "0.5", pointer_hides, "presenter", N_("Hide after"),
     N_("How long the pointer stays on the screen after the phone stops moving it."), "presenter", false},

    {"lock", "command", KR_S_STRING, "", NULL, "lock", N_("Lock command"),
     N_("Empty: loginctl lock-session."), "lockdevice", false},
    {"lock", "allow_unlock", KR_S_BOOL, "false", NULL, "lock", N_("Let the phone unlock"),
     N_("Off by default: anyone with the phone could otherwise unlock this computer."), "lockdevice", false},
    {"lock", "unlock_command", KR_S_STRING, "", NULL, "lock", N_("Unlock command"),
     N_("Empty: loginctl unlock-session. Needs the option above."), "lockdevice", false},

    {"ui", "tray", KR_S_ENUM, "auto", tray_choices, "interface", N_("Tray icon"),
     N_("Show an icon in the notification area. Automatic: unless the desktop already has one for Kiremote (KiDesktop)."),
     NULL, false},

    /* Read by kiremote-mirror each time it starts (mirror/conf.c); the
     * command line overrides them. */
    {"mirror", "max_size", KR_S_ENUM, "", mirror_sizes, "mirror", N_("Resolution"),
     N_("The longer side of the picture, in pixels. Default: the phone's own. Less is lighter for the phone."),
     NULL, false},
    {"mirror", "bit_rate", KR_S_ENUM, "", mirror_rates, "mirror", N_("Video quality"),
     N_("Bits per second the phone may spend on the picture. Default: 8M."), NULL, false},
    {"mirror", "max_fps", KR_S_ENUM, "", mirror_fps, "mirror", N_("Frames per second"),
     N_("The most frames the phone sends each second. Default: no limit."), NULL, false},
    {"mirror", "codec", KR_S_ENUM, "h264", mirror_codecs, "mirror", N_("Video codec"),
     N_("H.264 works everywhere. H.265 and AV1 use less data but need a newer phone and a faster computer."),
     NULL, false},
    {"mirror", "audio", KR_S_BOOL, "true", NULL, "mirror", N_("Play the phone's sound"),
     N_("Needs Android 11 or newer."), NULL, false},
    {"mirror", "control", KR_S_BOOL, "true", NULL, "mirror", N_("Mouse and keyboard"),
     N_("Let the mouse and keyboard act on the phone. Off: the screen is only shown."), NULL, false},
    {"mirror", "clipboard", KR_S_BOOL, "true", NULL, "mirror", N_("Share the clipboard"),
     N_("Copy and paste between the phone and this computer while mirroring."), NULL, false},
    {"mirror", "screen_off", KR_S_BOOL, "false", NULL, "mirror", N_("Turn the phone's screen off"),
     N_("Switch the phone's own screen off while mirroring. It stays on in the window."), NULL, false},
    {"mirror", "stay_awake", KR_S_BOOL, "false", NULL, "mirror", N_("Keep the phone awake"),
     N_("Do not let the phone sleep while mirroring (only works while it is charging)."), NULL, false},
    {"mirror", "wifi", KR_S_BOOL, "true", NULL, "mirror", N_("Use Wi-Fi without a cable"),
     N_("When no cable is connected, try the phone over the network (its debugging over Wi-Fi must be on)."),
     NULL, false},

    /* The phone's camera as a webcam (kiremote-mirror --camera). */
    {"camera", "status", KR_S_NOTE, "", NULL, "camera", N_("Video device status"),
     N_("Whether the video device the camera goes to is ready."), NULL, false},
    {"camera", "device", KR_S_PATH, "", NULL, "camera", N_("Video device"),
     N_("The v4l2loopback device the camera goes to, such as /dev/video10. Empty: the only one there is."), NULL,
     false},
    {"camera", "facing", KR_S_ENUM, "back", camera_facings, "camera", N_("Camera"),
     N_("Which of the phone's cameras to use."), NULL, false},
    {"camera", "max_size", KR_S_ENUM, "1280", camera_sizes, "camera", N_("Resolution"),
     N_("The longer side of the picture, in pixels. The phone picks the best size up to this one."), NULL, false},
    {"camera", "mirror", KR_S_BOOL, "false", NULL, "camera", N_("Flip left to right"),
     N_("Like a mirror: the picture other people see is the one you see in the preview."), NULL, false},

    {"runcommand", NULL, KR_S_MAP, "", NULL, "commands", N_("Commands the phone may run"),
     N_("Name and shell command. The phone only sees the names."), "runcommand", false},
};

#define N_SETTINGS ((int)(sizeof settings / sizeof *settings))

const kr_setting *kr_schema_all(int *n)
{
    *n = N_SETTINGS;
    return settings;
}

const kr_setting_group *kr_schema_groups(int *n)
{
    *n = (int)(sizeof groups / sizeof *groups);
    return groups;
}

const kr_setting *kr_schema_find(const char *spec, const char **name)
{
    if (name)
        *name = NULL;
    const char *dot = strchr(spec, '.');
    if (!dot || dot == spec || !dot[1])
        return NULL;
    size_t sl = (size_t)(dot - spec);
    for (int i = 0; i < N_SETTINGS; i++) {
        const kr_setting *s = &settings[i];
        if (strlen(s->section) != sl || strncasecmp(s->section, spec, sl) != 0)
            continue;
        if (s->type == KR_S_MAP) {
            if (name)
                *name = dot + 1;
            return s;
        }
        if (strcasecmp(s->key, dot + 1) == 0)
            return s;
    }
    return NULL;
}

/* ---- checking ------------------------------------------------------------ */

static bool fail(char *err, size_t size, const char *msg)
{
    snprintf(err, size, "%s", msg);
    return false;
}

static bool one_line(const char *v)
{
    for (const unsigned char *p = (const unsigned char *)v; *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return false;
    return true;
}

static bool check_bool(const char *v, char *norm, size_t size)
{
    static const char *const no[] = {"false", "no", "off", "0"};
    static const char *const yes[] = {"true", "yes", "on", "1"};
    for (size_t i = 0; i < 4; i++) {
        if (strcasecmp(v, no[i]) == 0)
            return kr_pathf(norm, size, "false");
        if (strcasecmp(v, yes[i]) == 0)
            return kr_pathf(norm, size, "true");
    }
    return false;
}

static bool check_addrs(const char *v, char *norm, size_t size)
{
    char *copy = strdup(v);
    if (!copy)
        return false;
    size_t len = 0;
    int count = 0;
    bool ok = true;
    norm[0] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(copy, ", \t", &save); tok && ok; tok = strtok_r(NULL, ", \t", &save)) {
        struct in_addr a;
        char text[INET_ADDRSTRLEN];
        ok = count < MAX_ADDRS && inet_pton(AF_INET, tok, &a) == 1 &&
             inet_ntop(AF_INET, &a, text, sizeof text) &&
             len + strlen(text) + 3 < size;
        if (ok) {
            len += (size_t)snprintf(norm + len, size - len, "%s%s", count ? ", " : "", text);
            count++;
        }
    }
    free(copy);
    return ok && count > 0;
}

bool kr_schema_check(const kr_setting *s, const char *name, const char *value, char *norm,
                     size_t norm_size, char *err, size_t err_size)
{
    /* The ini trims values; do it here so what we check is what is kept. */
    while (*value == ' ' || *value == '\t')
        value++;
    size_t len = strlen(value);
    while (len && (value[len - 1] == ' ' || value[len - 1] == '\t'))
        len--;
    char v[MAX_TEXT + 1];
    if (len > MAX_TEXT)
        return fail(err, err_size, _("too long"));
    memcpy(v, value, len);
    v[len] = '\0';
    norm[0] = '\0';

    if (s->type == KR_S_MAP) {
        if (!name || !*name || strlen(name) > 64 || !one_line(name) || strchr(name, '=') ||
            strchr("#;[", name[0]) || name[0] == ' ' || name[strlen(name) - 1] == ' ')
            return fail(err, err_size, _("invalid name"));
    }
    if (!one_line(v))
        return fail(err, err_size, _("must be a single line"));
    if (!v[0])
        return true; /* back to the default */

    switch (s->type) {
    case KR_S_BOOL:
        if (!check_bool(v, norm, norm_size))
            return fail(err, err_size, _("must be true or false"));
        return true;
    case KR_S_ENUM:
        for (const char *const *c = s->choices; c && *c; c++)
            if (strcmp(*c, v) == 0)
                return kr_pathf(norm, norm_size, "%s", v);
        return fail(err, err_size, _("not one of the choices"));
    case KR_S_ADDRS:
        if (!check_addrs(v, norm, norm_size))
            return fail(err, err_size, _("must be IPv4 addresses separated by commas"));
        return true;
    case KR_S_PATH:
        if (v[0] != '/' && strncmp(v, "~/", 2) != 0)
            return fail(err, err_size, _("must be an absolute path"));
        break;
    case KR_S_NAME: {
        char filtered[KR_NAME_SIZE];
        kr_name_filter(v, filtered, sizeof filtered);
        if (!filtered[0])
            return fail(err, err_size, _("not a usable name"));
        break;
    }
    case KR_S_COLOR: {
        uint32_t rgb;
        if (!kr_color_parse(v, &rgb))
            return fail(err, err_size, _("must be a color like #ff0000"));
        snprintf(norm, norm_size, "#%06x", (unsigned)rgb);
        return true;
    }
    case KR_S_NOTE:
        return fail(err, err_size, _("this is not a setting"));
    case KR_S_STRING:
    case KR_S_MAP:
        break;
    }
    return kr_pathf(norm, norm_size, "%s", v) || fail(err, err_size, _("too long"));
}

/* ---- JSON ---------------------------------------------------------------- */

static const char *type_name(kr_setting_type t)
{
    static const char *const names[] = {"bool", "string", "name", "path", "enum", "addresses", "color", "map", "note"};
    return names[t];
}

void kr_schema_to_json(cJSON *reply)
{
    cJSON *gs = cJSON_AddArrayToObject(reply, "groups");
    for (size_t i = 0; gs && i < sizeof groups / sizeof *groups; i++) {
        cJSON *g = cJSON_CreateObject();
        if (!g)
            break;
        cJSON_AddStringToObject(g, "id", groups[i].id);
        cJSON_AddStringToObject(g, "label", _(groups[i].label));
        cJSON_AddItemToArray(gs, g);
    }
    cJSON *ss = cJSON_AddArrayToObject(reply, "settings");
    for (int i = 0; ss && i < N_SETTINGS; i++) {
        const kr_setting *s = &settings[i];
        cJSON *o = cJSON_CreateObject();
        if (!o)
            break;
        char key[96];
        snprintf(key, sizeof key, "%s.%s", s->section, s->key ? s->key : "*");
        cJSON_AddStringToObject(o, "key", key);
        cJSON_AddStringToObject(o, "type", type_name(s->type));
        cJSON_AddStringToObject(o, "group", s->group);
        cJSON_AddStringToObject(o, "label", _(s->label));
        cJSON_AddStringToObject(o, "help", _(s->help));
        cJSON_AddStringToObject(o, "default", s->def);
        cJSON_AddBoolToObject(o, "restart", s->restart);
        if (s->plugin)
            cJSON_AddStringToObject(o, "plugin", s->plugin);
        if (s->choices) {
            cJSON *c = cJSON_AddArrayToObject(o, "choices");
            cJSON *l = cJSON_AddArrayToObject(o, "choice_labels");
            for (const char *const *p = s->choices; c && l && *p; p++) {
                const char *label = *p;
                for (size_t k = 0; k < sizeof choice_labels / sizeof *choice_labels; k++)
                    if (strcmp(choice_labels[k].value, *p) == 0)
                        label = _(choice_labels[k].label);
                cJSON_AddItemToArray(c, cJSON_CreateString(*p));
                cJSON_AddItemToArray(l, cJSON_CreateString(label));
            }
        }
        cJSON_AddItemToArray(ss, o);
    }
}

static void add_value(const kr_config *cfg, const kr_setting *s, cJSON *values, cJSON *set)
{
    char key[96];
    snprintf(key, sizeof key, "%s.%s", s->section, s->key);
    const char *raw = kr_config_get(cfg, s->section, s->key);
    if (s->type == KR_S_NOTE) { /* how things stand, now: the setting's value is a paragraph */
        kr_v4l v4l;
        kr_v4l_scan(&v4l);
        const char *dev = kr_config_get(cfg, "camera", "device");
        char *note = kr_v4l_note(&v4l, dev);
        cJSON_AddStringToObject(values, key, note ? note : "");
        free(note);
        return;
    }
    bool is_set = raw && *raw;
    char text[KR_PATH_MAX];
    const char *shown = is_set ? raw : s->def;

    if (strcmp(s->section, "device") == 0 && strcmp(s->key, "name") == 0) {
        shown = kr_config_device_name(cfg);
    } else if (strcmp(s->section, "device") == 0 && strcmp(s->key, "type") == 0) {
        shown = kr_config_device_type(cfg);
    } else if (strcmp(s->section, "share") == 0 && strcmp(s->key, "dir") == 0 && !is_set) {
        shown = kr_config_share_dir(cfg, text, sizeof text) ? text : "";
    } else if (s->type == KR_S_BOOL) {
        shown = kr_config_get_bool(cfg, s->section, s->key, strcmp(s->def, "true") == 0) ? "true" : "false";
    }
    cJSON_AddStringToObject(values, key, shown);
    if (is_set)
        cJSON_AddItemToArray(set, cJSON_CreateString(key));
}

void kr_schema_values_to_json(const kr_config *cfg, cJSON *reply)
{
    cJSON *values = cJSON_AddObjectToObject(reply, "values");
    cJSON *set = cJSON_AddArrayToObject(reply, "set");
    if (!values || !set)
        return;
    for (int i = 0; i < N_SETTINGS; i++) {
        const kr_setting *s = &settings[i];
        if (s->type != KR_S_MAP) {
            add_value(cfg, s, values, set);
            continue;
        }
        int it = 0;
        const char *k, *v;
        while (kr_config_each(cfg, s->section, &it, &k, &v)) {
            char key[256];
            snprintf(key, sizeof key, "%s.%s", s->section, k);
            cJSON_AddStringToObject(values, key, v);
            cJSON_AddItemToArray(set, cJSON_CreateString(key));
        }
    }
}
