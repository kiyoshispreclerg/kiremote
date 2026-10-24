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

TEST(the_phone_address_comes_from_its_wifi_route)
{
    char ip[32];
    /* A phone with mobile data and Wi-Fi: only the wlan line counts. */
    CHECK(kr_adb_parse_device_ip("10.12.0.0/16 dev rmnet_data1 proto kernel scope link src 10.12.4.9\r\n"
                                 "192.168.1.0/24 dev wlan0  proto kernel  scope link  src 192.168.1.42\r\n",
                                 ip, sizeof ip));
    CHECK_STR_EQ(ip, "192.168.1.42");
    CHECK(!kr_adb_parse_device_ip("10.12.0.0/16 dev rmnet_data1 proto kernel scope link src 10.12.4.9\n", ip, sizeof ip));
    CHECK(!kr_adb_parse_device_ip("192.168.1.0/24 dev wlan0 proto kernel scope link src not-an-ip\n", ip, sizeof ip));
    CHECK(!kr_adb_parse_device_ip("192.168.1.0/24 dev wlan0 proto kernel scope link\n", ip, sizeof ip));
    CHECK(!kr_adb_parse_device_ip("", ip, sizeof ip));
    char tiny[8];
    CHECK(!kr_adb_parse_device_ip("192.168.1.0/24 dev wlan0 src 192.168.1.42\n", tiny, sizeof tiny));
}

TEST(addresses_and_connect_replies)
{
    char hp[32];
    CHECK(kr_adb_host_port("192.168.1.42", hp, sizeof hp));
    CHECK_STR_EQ(hp, "192.168.1.42:5555");
    CHECK(kr_adb_host_port("192.168.1.42:37005", hp, sizeof hp));
    CHECK_STR_EQ(hp, "192.168.1.42:37005");
    CHECK(!kr_adb_host_port("", hp, sizeof hp));
    CHECK(!kr_adb_host_port(NULL, hp, sizeof hp));
    CHECK(!kr_adb_host_port("192.168.100.200", hp, 10));
    CHECK(kr_adb_is_tcpip("192.168.1.42:5555"));
    CHECK(!kr_adb_is_tcpip("5a0d7fcd"));
    CHECK(!kr_adb_is_tcpip("adb-R5CT-x._adb-tls-connect._tcp"));

    CHECK(kr_adb_connect_ok("connected to 192.168.1.42:5555\n"));
    CHECK(kr_adb_connect_ok("already connected to 192.168.1.42:5555\n"));
    CHECK(!kr_adb_connect_ok("failed to connect to '192.168.1.42:5555': Connection refused\n"));
    CHECK(!kr_adb_connect_ok("cannot connect to 192.168.1.42:5555: Connection refused (10061)\n"));
    CHECK(!kr_adb_connect_ok(""));
}

int main(void)
{
    RUN(device_lines_parse_from_the_end);
    RUN(garbage_is_not_a_device);
    RUN(picking_the_phone);
    RUN(the_phone_address_comes_from_its_wifi_route);
    RUN(addresses_and_connect_replies);
    return harness_report();
}
