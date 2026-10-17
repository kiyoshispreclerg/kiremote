/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The pairing state machine, against pairinghandler.cpp's behaviour. */
#include "../harness.h"
#include "pair.h"

#define WALL 1760000000
#define MONO 100.0

TEST(we_ask_they_accept)
{
    kr_pair p;
    kr_pair_init(&p, false);
    kr_pair_out o = kr_pair_request(&p, true, WALL, MONO);
    CHECK_INT_EQ(o.send, KR_PAIR_SEND_REQUEST);
    CHECK_INT_EQ(p.state, KR_PAIR_REQUESTED);
    CHECK_INT_EQ(p.timestamp, WALL);
    CHECK(kr_pair_deadline(&p) == MONO + 30);

    o = kr_pair_receive(&p, true, false, 0, WALL, MONO + 1);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_PAIRED);
    CHECK_INT_EQ(o.send, KR_PAIR_SEND_NOTHING);
    CHECK_INT_EQ(p.state, KR_PAIR_PAIRED);
    CHECK(kr_pair_deadline(&p) == 0);
}

TEST(they_ask_we_accept)
{
    kr_pair p;
    kr_pair_init(&p, false);
    /* Without a timestamp: ignored. */
    kr_pair_out o = kr_pair_receive(&p, true, false, 0, WALL, MONO);
    CHECK_INT_EQ(o.events, 0);
    CHECK_INT_EQ(p.state, KR_PAIR_NONE);

    o = kr_pair_receive(&p, true, true, WALL - 5, WALL, MONO);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_INCOMING);
    CHECK_INT_EQ(p.state, KR_PAIR_REQUESTED_BY_PEER);
    CHECK_INT_EQ(p.timestamp, WALL - 5);

    /* A second request is ignored. */
    o = kr_pair_receive(&p, true, true, WALL, WALL, MONO + 1);
    CHECK_INT_EQ(o.events, 0);
    CHECK_INT_EQ(p.timestamp, WALL - 5);

    /* Asking when they asked = accepting. */
    o = kr_pair_request(&p, true, WALL, MONO + 2);
    CHECK_INT_EQ(o.send, KR_PAIR_SEND_ACCEPT);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_PAIRED);
    CHECK_INT_EQ(p.state, KR_PAIR_PAIRED);
}

TEST(clocks_out_of_sync)
{
    kr_pair p;
    kr_pair_init(&p, false);
    kr_pair_out o = kr_pair_receive(&p, true, true, WALL + 1801, WALL, MONO);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_FAILED);
    CHECK_INT_EQ(o.why, KR_PAIR_WHY_CLOCKS);
    CHECK_INT_EQ(p.state, KR_PAIR_NONE);
    o = kr_pair_receive(&p, true, true, WALL - 1800, WALL, MONO);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_INCOMING);
}

TEST(refusals_and_timeouts)
{
    kr_pair p;
    kr_pair_init(&p, false);
    kr_pair_request(&p, true, WALL, MONO);
    kr_pair_out o = kr_pair_receive(&p, false, false, 0, WALL, MONO + 1);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_FAILED);
    CHECK_INT_EQ(o.why, KR_PAIR_WHY_CANCELED_BY_PEER);

    kr_pair_request(&p, true, WALL, MONO);
    o = kr_pair_tick(&p, MONO + 29.9);
    CHECK_INT_EQ(o.events, 0);
    o = kr_pair_tick(&p, MONO + 30);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_FAILED);
    CHECK_INT_EQ(o.why, KR_PAIR_WHY_TIMED_OUT);
    CHECK_INT_EQ(o.send, KR_PAIR_SEND_REFUSE);

    kr_pair_receive(&p, true, true, WALL, WALL, MONO);
    o = kr_pair_cancel(&p);
    CHECK_INT_EQ(o.why, KR_PAIR_WHY_CANCELED_BY_USER);
    CHECK_INT_EQ(o.send, KR_PAIR_SEND_REFUSE);
    CHECK_INT_EQ(p.state, KR_PAIR_NONE);

    o = kr_pair_request(&p, false, WALL, MONO);
    CHECK_INT_EQ(o.why, KR_PAIR_WHY_UNREACHABLE);
    CHECK_INT_EQ(o.send, KR_PAIR_SEND_NOTHING);
}

TEST(paired_device)
{
    kr_pair p;
    kr_pair_init(&p, true);
    /* An accept (no timestamp) from a trusted device is ignored. */
    kr_pair_out o = kr_pair_receive(&p, true, false, 0, WALL, MONO);
    CHECK_INT_EQ(o.events, 0);
    CHECK_INT_EQ(p.state, KR_PAIR_PAIRED);
    /* A request means it forgot us: unpaired, and a new request. */
    o = kr_pair_receive(&p, true, true, WALL, WALL, MONO);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_UNPAIRED | KR_PAIR_EV_INCOMING);
    CHECK_INT_EQ(p.state, KR_PAIR_REQUESTED_BY_PEER);

    kr_pair_init(&p, true);
    o = kr_pair_receive(&p, false, false, 0, WALL, MONO);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_UNPAIRED);
    CHECK_INT_EQ(p.state, KR_PAIR_NONE);
    /* Already unpaired: nothing. */
    o = kr_pair_receive(&p, false, false, 0, WALL, MONO);
    CHECK_INT_EQ(o.events, 0);

    kr_pair_init(&p, true);
    o = kr_pair_unpair(&p);
    CHECK_INT_EQ(o.events, KR_PAIR_EV_UNPAIRED);
    CHECK_INT_EQ(o.send, KR_PAIR_SEND_REFUSE);
    CHECK_INT_EQ(kr_pair_request(&p, true, WALL, MONO).send, KR_PAIR_SEND_REQUEST);
}

int main(void)
{
    RUN(we_ask_they_accept);
    RUN(they_ask_we_accept);
    RUN(clocks_out_of_sync);
    RUN(refusals_and_timeouts);
    RUN(paired_device);
    return harness_report();
}
