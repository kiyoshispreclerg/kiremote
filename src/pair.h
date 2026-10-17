/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KR_PAIR_H
#define KR_PAIR_H

/* The pairing state machine of kdeconnect-kde's pairinghandler.cpp, with
 * no I/O: each call says what to send and what happened, and the device
 * code does it. Times: wall is seconds since the epoch (the request's
 * timestamp), mono is the monotonic clock (the 30 s timeout). */

#include <stdbool.h>
#include <stdint.h>

#define KR_PAIR_TIMEOUT_S 30.0
#define KR_PAIR_MAX_CLOCK_SKEW_S 1800

typedef enum {
    KR_PAIR_NONE,
    KR_PAIR_REQUESTED,         /* we asked */
    KR_PAIR_REQUESTED_BY_PEER, /* they asked; the user decides */
    KR_PAIR_PAIRED,
} kr_pair_state;

typedef enum {
    KR_PAIR_WHY_NONE,
    KR_PAIR_WHY_CLOCKS,          /* request timestamp too far from our clock */
    KR_PAIR_WHY_CANCELED_BY_PEER,
    KR_PAIR_WHY_CANCELED_BY_USER,
    KR_PAIR_WHY_TIMED_OUT,
    KR_PAIR_WHY_UNREACHABLE,
} kr_pair_why;

/* What happened (bits of kr_pair_out.events). */
enum {
    KR_PAIR_EV_INCOMING = 1 << 0, /* the peer asks to pair */
    KR_PAIR_EV_PAIRED = 1 << 1,
    KR_PAIR_EV_FAILED = 1 << 2,   /* a pairing in progress ended (why) */
    KR_PAIR_EV_UNPAIRED = 1 << 3, /* a pairing that existed is gone */
};

/* What to send. */
typedef enum {
    KR_PAIR_SEND_NOTHING,
    KR_PAIR_SEND_REQUEST, /* {"pair":true,"timestamp":<timestamp>} */
    KR_PAIR_SEND_ACCEPT,  /* {"pair":true} */
    KR_PAIR_SEND_REFUSE,  /* {"pair":false} */
} kr_pair_send;

typedef struct {
    unsigned events;
    kr_pair_why why;
    kr_pair_send send;
} kr_pair_out;

typedef struct {
    kr_pair_state state;
    int64_t timestamp; /* of the request in progress (ours or theirs) */
    double deadline;   /* mono time the request expires, while one runs */
} kr_pair;

/* Starts in PAIRED for trusted devices, NONE otherwise. */
void kr_pair_init(kr_pair *p, bool paired);

/* A kdeconnect.pair packet arrived. has_ts says whether "timestamp" was
 * there. */
kr_pair_out kr_pair_receive(kr_pair *p, bool pair, bool has_ts, int64_t ts,
                            int64_t wall, double mono);
/* The user asks to pair (accepts, if the peer asked first). */
kr_pair_out kr_pair_request(kr_pair *p, bool reachable, int64_t wall, double mono);
/* The user accepts the peer's request. */
kr_pair_out kr_pair_accept(kr_pair *p);
/* The user refuses or gives up a pairing in progress. */
kr_pair_out kr_pair_cancel(kr_pair *p);
/* The user unpairs a paired device (also cancels one in progress). */
kr_pair_out kr_pair_unpair(kr_pair *p);
/* Call when the deadline passes. */
kr_pair_out kr_pair_tick(kr_pair *p, double mono);

/* While a request runs: the deadline; 0 otherwise. */
double kr_pair_deadline(const kr_pair *p);

#endif
