/* SPDX-License-Identifier: GPL-3.0-or-later */
/* `adb devices -l` as adb prints it, and picking the phone to mirror. */
#include "../harness.h"
#include "adb.h"

/* What this machine's adb (1.0.41) printed with the test phone, after the
 * server came up, plus the cases scrcpy handles (a serial with spaces). */
static const char OUT[] = "* daemon not running; starting now at tcp:5037\n"
                          "* daemon started successfully\n"
                          "List of devices attached\n"
                          "5a0d7fcd               device usb:3-2 product:spes_global model:2201117TL device:spes "
                          "transport_id:1\n"
                          "192.168.0.7:5555       offline transport_id:2\r\n"
                          "adb-R5CT 2 _adb-tls-connect._tcp unauthorized transport_id:3\n"
                          "\n";

TEST(device_lines_parse_from_the_end)
{
    kr_adb_dev d[8];
    int n = kr_adb_parse_devices(OUT, d, 8);
    CHECK_INT_EQ(n, 3);
    CHECK_STR_EQ(d[0].serial, "5a0d7fcd");
    CHECK_STR_EQ(d[0].state, "device");
    CHECK_STR_EQ(d[0].model, "2201117TL");
    CHECK_STR_EQ(d[1].serial, "192.168.0.7:5555");
    CHECK_STR_EQ(d[1].state, "offline");
    CHECK_STR_EQ(d[1].model, "");
    CHECK_STR_EQ(d[2].serial, "adb-R5CT 2 _adb-tls-connect._tcp");
    CHECK_STR_EQ(d[2].state, "unauthorized");
}

TEST(garbage_is_not_a_device)
{
    kr_adb_dev d[4];
    CHECK_INT_EQ(kr_adb_parse_devices("5a0d7fcd device\n", d, 4), 0); /* no header */
    CHECK_INT_EQ(kr_adb_parse_devices("List of devices attached\nadb server version (41) doesn't match\n"
                                      "nostate here\n   device\n",
                                      d, 4),
                 0);
    CHECK_INT_EQ(kr_adb_parse_devices("", d, 4), 0);
    /* At most max. */
    CHECK_INT_EQ(kr_adb_parse_devices("List of devices attached\na device\nb device\nc device\n", d, 2), 2);
}

TEST(picking_the_phone)
{
    kr_adb_dev d[8];
    int n = kr_adb_parse_devices(OUT, d, 8);
    const char *why = NULL;
    CHECK_INT_EQ(kr_adb_pick(d, n, NULL, &why), 0);
    CHECK_INT_EQ(kr_adb_pick(d, n, "5a0d7fcd", &why), 0);
    CHECK_INT_EQ(kr_adb_pick(d, n, "192.168.0.7:5555", &why), -1);
    CHECK_STR_HAS(why, "offline");
    CHECK_INT_EQ(kr_adb_pick(d, n, "adb-R5CT 2 _adb-tls-connect._tcp", &why), -1);
    CHECK_STR_HAS(why, "USB debugging prompt");
    CHECK_INT_EQ(kr_adb_pick(d, n, "nope", &why), -1);

    /* Only the unauthorized one: say so. */
    CHECK_INT_EQ(kr_adb_pick(d + 2, 1, NULL, &why), -1);
    CHECK_STR_HAS(why, "USB debugging prompt");
    CHECK_INT_EQ(kr_adb_pick(d, 0, NULL, &why), -1);
    CHECK_STR_HAS(why, "No phone");

    /* Two ready ones: the user picks. */
    n = kr_adb_parse_devices("List of devices attached\na device\nb device\n", d, 8);
    CHECK_INT_EQ(kr_adb_pick(d, n, NULL, &why), -1);
    CHECK_STR_HAS(why, "--serial");
    CHECK_INT_EQ(kr_adb_pick(d, n, "b", &why), 1);
}

int main(void)
{
    RUN(device_lines_parse_from_the_end);
    RUN(garbage_is_not_a_device);
    RUN(picking_the_phone);
    return harness_report();
}
