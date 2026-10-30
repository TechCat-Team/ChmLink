#include "chmlink/packet.h"
#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void)
{
    uint8_t packet[64] = {0};

    CHECK(!chmlink_packet_valid(CHMLINK_MODE_ETHERNET, NULL, 14, 64));
    CHECK(!chmlink_packet_valid(CHMLINK_MODE_ETHERNET, packet, 13, 64));
    CHECK(chmlink_packet_valid(CHMLINK_MODE_ETHERNET, packet, 14, 64));
    /* The Ethernet path accepts arbitrary EtherTypes, including VLAN tags. */
    packet[12] = 0x81;
    packet[13] = 0x00;
    CHECK(chmlink_packet_valid(CHMLINK_MODE_ETHERNET, packet, 18, 64));
    CHECK(!chmlink_packet_valid(CHMLINK_MODE_ETHERNET, packet, 64, 63));
    CHECK(!chmlink_packet_valid((chmlink_mode)99, packet, 64, 64));
    packet[0] = 0x45;
    packet[3] = 20;
    CHECK(!chmlink_packet_valid(CHMLINK_MODE_IP, packet, 19, 64));
    CHECK(chmlink_packet_valid(CHMLINK_MODE_IP, packet, 20, 64));
    packet[0] = 0x60;
    CHECK(!chmlink_packet_valid(CHMLINK_MODE_IP, packet, 39, 64));
    CHECK(chmlink_packet_valid(CHMLINK_MODE_IP, packet, 40, 64));
    packet[0] = 0x70;
    CHECK(!chmlink_packet_valid(CHMLINK_MODE_IP, packet, 64, 64));
    CHECK(!chmlink_packet_valid(CHMLINK_MODE_IP, packet, 0, 64));
    return 0;
}
