/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Follows kdeconnect-kde's core/backends/pairinghandler.cpp
 * (SPDX-FileCopyrightText: 2015 Albert Vaca Cintora <albertvaka@gmail.com>,
 * GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL). */
#include "pair.h"

#include <stdlib.h>

void kr_pair_init(kr_pair *p, bool paired)
{
    *p = (kr_pair){.state = paired ? KR_PAIR_PAIRED : KR_PAIR_NONE};
}

static bool in_progress(const kr_pair *p)
{
    return p->state == KR_PAIR_REQUESTED || p->state == KR_PAIR_REQUESTED_BY_PEER;
}

static kr_pair_out fail(kr_pair *p, kr_pair_why why, kr_pair_send send)
{
    p->state = KR_PAIR_NONE;
    p->deadline = 0;
    return (kr_pair_out){.events = KR_PAIR_EV_FAILED, .why = why, .send = send};
}

kr_pair_out kr_pair_receive(kr_pair *p, bool pair, bool has_ts, int64_t ts,
                            int64_t wall, double mono)
{
    kr_pair_out out = {0};
    if (!pair) {
        switch (p->state) {
        case KR_PAIR_NONE:
            break;
        case KR_PAIR_REQUESTED:
        case KR_PAIR_REQUESTED_BY_PEER:
            return fail(p, KR_PAIR_WHY_CANCELED_BY_PEER, KR_PAIR_SEND_NOTHING);
        case KR_PAIR_PAIRED:
            p->state = KR_PAIR_NONE;
            out.events = KR_PAIR_EV_UNPAIRED;
            break;
        }
        return out;
    }

    switch (p->state) {
    case KR_PAIR_REQUESTED:
        p->state = KR_PAIR_PAIRED;
        p->deadline = 0;
        out.events = KR_PAIR_EV_PAIRED;
        return out;
    case KR_PAIR_REQUESTED_BY_PEER:
        return out; /* a second request before the first timed out */
    case KR_PAIR_PAIRED:
        /* Requests carry a timestamp, accepts don't: a request from a
         * device we trust means it forgot us. */
        if (!has_ts)
            return out;
        p->state = KR_PAIR_NONE;
        out.events = KR_PAIR_EV_UNPAIRED;
        break;
    case KR_PAIR_NONE:
        break;
    }

    if (!has_ts)
        return out;
    if (llabs(ts - wall) > KR_PAIR_MAX_CLOCK_SKEW_S) {
        kr_pair_out f = fail(p, KR_PAIR_WHY_CLOCKS, KR_PAIR_SEND_NOTHING);
        f.events |= out.events;
        return f;
    }
    p->state = KR_PAIR_REQUESTED_BY_PEER;
    p->timestamp = ts;
    p->deadline = mono + KR_PAIR_TIMEOUT_S;
    out.events |= KR_PAIR_EV_INCOMING;
    return out;
}

kr_pair_out kr_pair_request(kr_pair *p, bool reachable, int64_t wall, double mono)
{
    kr_pair_out out = {0};
    switch (p->state) {
    case KR_PAIR_PAIRED:
    case KR_PAIR_REQUESTED:
        return out;
    case KR_PAIR_REQUESTED_BY_PEER:
        return kr_pair_accept(p);
    case KR_PAIR_NONE:
        break;
    }
    if (!reachable)
        return fail(p, KR_PAIR_WHY_UNREACHABLE, KR_PAIR_SEND_NOTHING);
    p->state = KR_PAIR_REQUESTED;
    p->timestamp = wall;
    p->deadline = mono + KR_PAIR_TIMEOUT_S;
    out.send = KR_PAIR_SEND_REQUEST;
    return out;
}

kr_pair_out kr_pair_accept(kr_pair *p)
{
    kr_pair_out out = {0};
    if (p->state != KR_PAIR_REQUESTED_BY_PEER)
        return out;
    p->state = KR_PAIR_PAIRED;
    p->deadline = 0;
    out.events = KR_PAIR_EV_PAIRED;
    out.send = KR_PAIR_SEND_ACCEPT;
    return out;
}

kr_pair_out kr_pair_cancel(kr_pair *p)
{
    if (!in_progress(p))
        return (kr_pair_out){0};
    return fail(p, KR_PAIR_WHY_CANCELED_BY_USER, KR_PAIR_SEND_REFUSE);
}

kr_pair_out kr_pair_unpair(kr_pair *p)
{
    if (in_progress(p))
        return kr_pair_cancel(p);
    kr_pair_out out = {.send = KR_PAIR_SEND_REFUSE};
    if (p->state == KR_PAIR_PAIRED)
        out.events = KR_PAIR_EV_UNPAIRED;
    p->state = KR_PAIR_NONE;
    return out;
}

kr_pair_out kr_pair_tick(kr_pair *p, double mono)
{
    if (!in_progress(p) || mono < p->deadline)
        return (kr_pair_out){0};
    return fail(p, KR_PAIR_WHY_TIMED_OUT, KR_PAIR_SEND_REFUSE);
}

double kr_pair_deadline(const kr_pair *p)
{
    return in_progress(p) ? p->deadline : 0;
}
