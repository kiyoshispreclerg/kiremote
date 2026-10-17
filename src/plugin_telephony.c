/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kdeconnect.telephony (plugins/telephony upstream; and/plugins/telephony
 * for the sender):
 *
 *   {"event": "ringing"|"talking"|"missedCall"|"sms",
 *    "phoneNumber": "...", "contactName": "...",   (name or number)
 *    "phoneThumbnail": base64 JPEG,                (not shown here)
 *    "isCancel": "true"}                           (a string, from Android)
 *
 * ringing → a notification that stays until the call is answered, ends, or
 * is cancelled, with a "Mute Call" button when the server has buttons;
 * missedCall → a notification; talking and isCancel close the ringing one;
 * the old-style "sms" event is ignored (messages come with the SMS plugin).
 * Muting sends kdeconnect.telephony.request_mute, also reachable as the
 * "mute-call" request on the control socket. */
#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "log.h"
#include "plugin.h"

#define TELEPHONY "kdeconnect.telephony"
#define MUTE "kdeconnect.telephony.request_mute"

static const char *const incoming[] = {TELEPHONY, NULL};
static const char *const outgoing[] = {MUTE, NULL};

typedef struct {
    kr_daemon *d;
    char id[64];
    kr_note *note; /* the call's notification, while it is shown */
} call_state;

static bool send_mute(kr_daemon *d, kr_device *dev)
{
    kr_packet *p = kr_packet_new(MUTE);
    bool ok = p && kr_daemon_send(d, dev, p);
    kr_packet_free(p);
    return ok;
}

static void note_action(void *ud, const char *key)
{
    call_state *st = ud;
    kr_device *dev = kr_daemon_find_device(st->d, st->id);
    if (dev && strcmp(key, "mute") == 0)
        send_mute(st->d, dev);
}

static void note_closed(void *ud)
{
    ((call_state *)ud)->note = NULL;
}

static void close_note(call_state *st)
{
    if (st->note) {
        kr_note_close(st->note);
        kr_note_detach(st->note);
        st->note = NULL;
    }
}

static void on_connect(kr_daemon *d, kr_device *dev)
{
    call_state *st = kr_device_state(dev, &kr_plugin_telephony);
    st->d = d;
    snprintf(st->id, sizeof st->id, "%s", kr_device_id(dev));
}

static void on_disconnect(kr_daemon *d, kr_device *dev)
{
    (void)d;
    close_note(kr_device_state(dev, &kr_plugin_telephony));
}

static void on_packet(kr_daemon *d, kr_device *dev, const kr_packet *p)
{
    call_state *st = kr_device_state(dev, &kr_plugin_telephony);
    if (kr_packet_bool(p, "isCancel", false)) {
        close_note(st);
        return;
    }
    const char *event = kr_packet_str(p, "event");
    if (!event || strcmp(event, "sms") == 0)
        return;
    if (strcmp(event, "talking") == 0) {
        close_note(st);
        return;
    }
    bool ringing = strcmp(event, "ringing") == 0;
    if (!ringing && strcmp(event, "missedCall") != 0)
        return;

    const char *number = kr_packet_str(p, "phoneNumber");
    const char *who = kr_packet_str(p, "contactName");
    if (!who)
        who = number ? number : _("unknown number");
    char body[400];
    snprintf(body, sizeof body, ringing ? _("Incoming call from %s") : _("Missed call from %s"), who);

    close_note(st);
    const char *actions[3] = {"mute", _("Mute Call"), NULL};
    st->note = kr_notify_show(kr_daemon_notify(d),
                              &(kr_note_spec){.summary = kr_device_name(dev),
                                              .body = body,
                                              .icon = "call-start",
                                              .actions = ringing ? actions : NULL,
                                              .timeout_ms = ringing ? 0 : -1,
                                              .urgent = ringing},
                              &(kr_note_cb){.on_action = note_action, .on_closed = note_closed}, st);
}

static bool request(kr_daemon *d, const char *type, const cJSON *req, cJSON *reply)
{
    if (strcmp(type, "mute-call") != 0)
        return false;
    kr_device *dev = kr_daemon_request_device(d, req, reply);
    if (dev && !send_mute(d, dev))
        kr_control_fail(reply, _("the device is not reachable or does not take this"));
    return true;
}

const kr_plugin kr_plugin_telephony = {
    .name = "telephony",
    .incoming = incoming,
    .outgoing = outgoing,
    .state_size = sizeof(call_state),
    .on_connect = on_connect,
    .on_disconnect = on_disconnect,
    .on_packet = on_packet,
    .request = request,
};
