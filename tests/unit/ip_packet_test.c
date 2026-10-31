#include "chmlink/packet.h"
#include "chmlink/protocol.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    uint8_t p[128] = {0};
    size_t i;
    CHECK(!chmlink_ip_packet_valid(NULL, 20, 128));
    p[0] = 0x45; p[3] = 20;
    for (i = 0; i < 20; ++i) CHECK(!chmlink_ip_packet_valid(p, i, 128));
    CHECK(chmlink_ip_packet_valid(p, 20, 20));
    CHECK(!chmlink_ip_packet_valid(p, 20, 19));
    CHECK(!chmlink_ip_packet_valid(p, 21, 128));
    p[0] = 0x44; CHECK(!chmlink_ip_packet_valid(p, 20, 128));
    p[0] = 0x46; CHECK(!chmlink_ip_packet_valid(p, 20, 128));
    p[3] = 24; CHECK(chmlink_ip_packet_valid(p, 24, 128));
    p[3] = 23; CHECK(!chmlink_ip_packet_valid(p, 24, 128));
    memset(p, 0, sizeof(p)); p[0] = 0x60;
    for (i = 0; i < 40; ++i) CHECK(!chmlink_ip_packet_valid(p, i, 128));
    CHECK(chmlink_ip_packet_valid(p, 40, 128));
    CHECK(!chmlink_ip_packet_valid(p, 41, 128));
    p[5] = 1; CHECK(chmlink_ip_packet_valid(p, 41, 128));
    CHECK(!chmlink_ip_packet_valid(p, 40, 128));
    p[4] = 0xff; p[5] = 0xff; CHECK(!chmlink_ip_packet_valid(p, 128, 128));
    p[0] = 0x70; CHECK(!chmlink_ip_packet_valid(p, 40, 128));
    {
        uint8_t batch[256], saved[256], ipv4[20] = {0x45}, ipv6[40] = {0x60};
        chmlink_batch_writer writer;
        uint32_t count = 9;
        size_t saved_length;
        ipv4[3] = 20;
        CHECK(chmlink_batch_begin(&writer, batch, sizeof(batch), CHMLINK_MODE_IP, 128) == CHMLINK_OK);
        CHECK(chmlink_batch_append(&writer, ipv4, sizeof(ipv4)) == CHMLINK_OK);
        CHECK(chmlink_batch_append(&writer, ipv6, sizeof(ipv6)) == CHMLINK_OK);
        CHECK(chmlink_batch_validate(batch, writer.length, CHMLINK_MODE_IP, 128, &count) == CHMLINK_OK && count == 2);
        saved_length = writer.length; memcpy(saved, batch, saved_length);
        ipv6[5] = 1;
        CHECK(chmlink_batch_append(&writer, ipv6, sizeof(ipv6)) == CHMLINK_INVALID_PACKET);
        CHECK(writer.count == 2 && writer.length == saved_length && !memcmp(saved, batch, saved_length));
        batch[28 + 20 + 4 + 5] = 1; /* Inconsistent IPv6 payload length in second packet. */
        count = 9;
        CHECK(chmlink_batch_validate(batch, writer.length, CHMLINK_MODE_IP, 128, &count) == CHMLINK_INVALID_PACKET && count == 0);
        CHECK(chmlink_batch_validate(batch, writer.length - 1, CHMLINK_MODE_IP, 128, &count) != CHMLINK_OK && count == 0);
    }
    return 0;
}
