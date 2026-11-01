#include "chmlink/packet.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static size_t ipv4(uint8_t *p, uint8_t protocol, unsigned sport, unsigned dport, uint8_t id, uint8_t fragment)
{
    memset(p, 0, 60);
    p[0] = 0x45; p[3] = 48; p[4] = id; p[6] = fragment; p[8] = 64; p[9] = protocol;
    p[12] = 10; p[15] = 1; p[16] = 10; p[19] = 2;
    p[20] = (uint8_t)(sport >> 8); p[21] = (uint8_t)sport;
    p[22] = (uint8_t)(dport >> 8); p[23] = (uint8_t)dport;
    p[28] = id;   /* payload */
    return 48;
}

static size_t ipv6(uint8_t *p, uint8_t protocol, unsigned sport, unsigned dport)
{
    memset(p, 0, 80);
    p[0] = 0x60; p[5] = 8; p[6] = protocol; p[7] = 64;
    p[8] = 0x20; p[9] = 0x01; p[23] = 1; p[24] = 0x20; p[25] = 0x01; p[39] = 2;
    p[40] = (uint8_t)(sport >> 8); p[41] = (uint8_t)sport;
    p[42] = (uint8_t)(dport >> 8); p[43] = (uint8_t)dport;
    return 48;
}

/* An IPv6 TCP/UDP packet behind the given extension headers: each entry is
 * a header type (0, 43, 60, 51 or 44); for 44, fragment says offset/M bits. */
static size_t ipv6_ext(uint8_t *p, const uint8_t *chain, unsigned count, uint8_t protocol, unsigned sport,
                       unsigned dport, uint16_t fragment)
{
    size_t at = 40, i;
    uint8_t *next = p + 6;
    memset(p, 0, 160);
    p[0] = 0x60; p[7] = 64; p[8] = 0x20; p[9] = 0x01; p[23] = 1; p[24] = 0x20; p[25] = 0x01; p[39] = 2;
    for (i = 0; i < count; ++i) {
        *next = chain[i];
        next = p + at;
        if (chain[i] == 44) {
            p[at + 2] = (uint8_t)(fragment >> 8); p[at + 3] = (uint8_t)fragment; p[at + 7] = 9;   /* id */
            at += 8;
        } else if (chain[i] == 51) {
            p[at + 1] = 2; at += 16;        /* (2 + 2) * 4 bytes */
        } else {
            p[at + 1] = 1; at += 16;        /* (1 + 1) * 8 bytes */
        }
    }
    *next = protocol;
    p[at] = (uint8_t)(sport >> 8); p[at + 1] = (uint8_t)sport;
    p[at + 2] = (uint8_t)(dport >> 8); p[at + 3] = (uint8_t)dport;
    at += 20;
    p[4] = (uint8_t)((at - 40) >> 8); p[5] = (uint8_t)(at - 40);
    return at;
}

int main(void)
{
    uint8_t a[160], b[160], frame[100];
    size_t n;
    unsigned int bucket[4] = {0, 0, 0, 0}, i;
    uint32_t h;

    /* One flow hashes alike whatever the rest of the packet holds. */
    n = ipv4(a, 6, 40000, 443, 1, 0x40);
    ipv4(b, 6, 40000, 443, 99, 0x40); b[8] = 3;
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) == chmlink_flow_hash(CHMLINK_MODE_IP, b, n));
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) != 0);
    /* Ports, protocol and addresses tell flows apart. */
    ipv4(b, 6, 40001, 443, 1, 0x40);
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) != chmlink_flow_hash(CHMLINK_MODE_IP, b, n));
    ipv4(b, 17, 40000, 443, 1, 0x40);
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) != chmlink_flow_hash(CHMLINK_MODE_IP, b, n));
    ipv4(b, 6, 40000, 443, 1, 0x40); b[19] = 3;
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) != chmlink_flow_hash(CHMLINK_MODE_IP, b, n));
    /* Both directions of a connection hash alike. */
    n = ipv4(a, 6, 40000, 443, 1, 0x40);
    ipv4(b, 6, 443, 40000, 1, 0x40); b[15] = 2; b[19] = 1;
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) == chmlink_flow_hash(CHMLINK_MODE_IP, b, n));
    ipv4(b, 6, 40000, 443, 1, 0x40); b[15] = 2; b[19] = 1;   /* swapped addresses, same ports: another flow */
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) != chmlink_flow_hash(CHMLINK_MODE_IP, b, n));
    /* Fragments hash without ports: the first and a later one agree. */
    ipv4(a, 17, 40000, 443, 7, 0x20);         /* MF, offset 0 */
    ipv4(b, 17, 1, 2, 7, 0x00); b[7] = 185;   /* offset 1480, bytes that are not ports */
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) == chmlink_flow_hash(CHMLINK_MODE_IP, b, n));
    /* Ethernet, with and without a VLAN tag, hashes the IP packet inside. */
    n = ipv4(a, 6, 40000, 443, 1, 0x40);
    memset(frame, 0, sizeof(frame)); frame[12] = 0x08; memcpy(frame + 14, a, n);
    CHECK(chmlink_flow_hash(CHMLINK_MODE_ETHERNET, frame, 14 + n) == chmlink_flow_hash(CHMLINK_MODE_IP, a, n));
    frame[12] = 0x81; frame[13] = 0x00; frame[16] = 0x08; frame[17] = 0x00; memcpy(frame + 18, a, n);
    CHECK(chmlink_flow_hash(CHMLINK_MODE_ETHERNET, frame, 18 + n) == chmlink_flow_hash(CHMLINK_MODE_IP, a, n));
    /* IPv6 ports count too. */
    n = ipv6(a, 6, 40000, 443); ipv6(b, 6, 40000, 444);
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) != chmlink_flow_hash(CHMLINK_MODE_IP, b, n));
    ipv6(b, 6, 443, 40000); b[23] = 2; b[39] = 1;
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) == chmlink_flow_hash(CHMLINK_MODE_IP, b, n));
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, n) != 0);
    /* IPv6 extension headers: the same connection hashes alike behind
     * Hop-by-Hop, Routing, Destination Options and Authentication headers and
     * an atomic fragment; the ports still count. */
    {
        static const uint8_t hbh[] = {0}, several[] = {0, 43, 60}, ah[] = {51}, frag[] = {44}, frag_dst[] = {44, 60};
        uint8_t c[160], d[160];
        uint32_t plain;
        size_t m;
        n = ipv6_ext(a, NULL, 0, 6, 40000, 443, 0);
        plain = chmlink_flow_hash(CHMLINK_MODE_IP, a, n);
        m = ipv6_ext(b, hbh, 1, 6, 40000, 443, 0);
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, b, m) == plain);
        m = ipv6_ext(b, several, 3, 6, 40000, 443, 0);
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, b, m) == plain);
        m = ipv6_ext(b, ah, 1, 6, 40000, 443, 0);
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, b, m) == plain);
        m = ipv6_ext(b, frag, 1, 6, 40000, 443, 0);            /* atomic fragment */
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, b, m) == plain);
        m = ipv6_ext(b, several, 3, 6, 40001, 443, 0);
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, b, m) != plain);
        /* Fragments: the first (ports present, more fragments) and a later one
         * (other bytes where the ports were) hash alike, also with a
         * Destination Options header in the fragmentable part. */
        m = ipv6_ext(c, frag, 1, 17, 5000, 53, 0x0001);
        n = ipv6_ext(d, frag, 1, 17, 1, 2, 0x00b8);
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, c, m) == chmlink_flow_hash(CHMLINK_MODE_IP, d, n));
        m = ipv6_ext(c, frag_dst, 2, 17, 5000, 53, 0x0001);
        n = ipv6_ext(d, frag_dst, 2, 17, 1, 2, 0x00b8);
        d[56] = 0x77;                                            /* later fragments carry no options */
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, c, m) == chmlink_flow_hash(CHMLINK_MODE_IP, d, n));
        /* A chain cut short hashes stably and reads nothing beyond the packet. */
        m = ipv6_ext(b, several, 3, 6, 40000, 443, 0);
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, b, 60) == chmlink_flow_hash(CHMLINK_MODE_IP, b, 60));
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, b, 60) != 0);
        CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, b, 41) != 0);
        (void)m;
    }
    /* Nothing to hash: 0. */
    memset(frame, 0, sizeof(frame)); frame[12] = 0x08; frame[13] = 0x06;   /* ARP */
    CHECK(chmlink_flow_hash(CHMLINK_MODE_ETHERNET, frame, 60) == 0);
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, 19) == 0);
    CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, NULL, 40) == 0);
    a[0] = 0x55; CHECK(chmlink_flow_hash(CHMLINK_MODE_IP, a, 40) == 0);
    /* Flows that differ in the source port only spread evenly over 4 queues
     * the way the driver picks them. */
    for (i = 0; i < 4096; ++i) {
        n = ipv4(a, 6, 1024 + i, 443, 1, 0x40);
        h = chmlink_flow_hash(CHMLINK_MODE_IP, a, n);
        bucket[(uint32_t)(((uint64_t)h * 4) >> 32)]++;
    }
    for (i = 0; i < 4; ++i) CHECK(bucket[i] > 900 && bucket[i] < 1150);
    puts("flow hash ok");
    return 0;
}
