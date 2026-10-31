#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chmlink/offload.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

/* Independent of the library: RFC 1071 and the reassembly checks below. */
static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (be16(p) << 16) | be16(p + 2); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v >> 16); wr16(p + 2, v); }
static uint32_t sum(const uint8_t *p, size_t n, uint32_t s)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) s += be16(p + i);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    return s;
}
static uint16_t fold(uint32_t s) { while (s >> 16) s = (s & 0xffff) + (s >> 16); return (uint16_t)s; }

static const uint8_t src4[4] = {10, 0, 0, 1}, dst4[4] = {10, 0, 0, 2};
static const uint8_t src6[16] = {0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
static const uint8_t dst6[16] = {0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

static void fill(uint8_t *p, size_t n, unsigned seed) { size_t i; for (i = 0; i < n; ++i) p[i] = (uint8_t)(i * 7 + seed); }

static void l4_checksum(uint8_t *ip, int v6, size_t l4, size_t total, unsigned protocol, size_t field)
{
    uint8_t pseudo[40];
    size_t plen;
    uint32_t len = (uint32_t)(total - l4);
    if (!v6) { memcpy(pseudo, ip + 12, 8); pseudo[8] = 0; pseudo[9] = (uint8_t)protocol; wr16(pseudo + 10, len); plen = 12; }
    else { memcpy(pseudo, ip + 8, 32); wr32(pseudo + 32, len); pseudo[36] = pseudo[37] = pseudo[38] = 0; pseudo[39] = (uint8_t)protocol; plen = 40; }
    ip[l4 + field] = ip[l4 + field + 1] = 0;
    wr16(ip + l4 + field, (uint16_t)~fold(sum(ip + l4, total - l4, sum(pseudo, plen, 0))));
}

/* IPv4 packet: `options` bytes of IP options, protocol 6 (TCP) or 17 (UDP). */
static size_t ipv4(uint8_t *p, size_t options, unsigned protocol, size_t payload, int df, unsigned seed)
{
    size_t header = 20 + options, l4len = protocol == 6 ? 32 : 8, total = header + l4len + payload;
    memset(p, 0, total);
    p[0] = (uint8_t)(0x40 | (header / 4)); wr16(p + 2, (uint32_t)total); wr16(p + 4, 0x1234);
    wr16(p + 6, df ? 0x4000 : 0); p[8] = 64; p[9] = (uint8_t)protocol;
    memcpy(p + 12, src4, 4); memcpy(p + 16, dst4, 4);
    if (options) {
        /* Security (130, copied), then record route (7, not copied), then NOP padding. */
        uint8_t opts[40] = {130, 11, 1, 2, 3, 4, 5, 6, 7, 8, 9, 7, 7, 4, 0, 0, 0, 0, 1};
        memcpy(p + 20, opts, options);
    }
    if (protocol == 6) {
        wr16(p + header, 40000); wr16(p + header + 2, 443); wr32(p + header + 4, 0xfffffff0u);
        p[header + 12] = (uint8_t)(8 << 4); p[header + 13] = 0x18 | 0x01;   /* PSH, ACK, FIN */
        p[header + 20] = 1; p[header + 21] = 1; p[header + 22] = 8; p[header + 23] = 10;   /* timestamps */
    } else {
        wr16(p + header, 5353); wr16(p + header + 2, 53); wr16(p + header + 4, (uint32_t)(l4len + payload));
    }
    fill(p + header + l4len, payload, seed);
    l4_checksum(p, 0, header, total, protocol, protocol == 6 ? 16 : 6);
    wr16(p + 10, (uint16_t)~fold(sum(p, header, 0)));
    return total;
}

/* IPv6 packet, optionally with a hop-by-hop header (8 bytes) first. */
static size_t ipv6(uint8_t *p, int hop, unsigned protocol, size_t payload, unsigned seed)
{
    size_t l4 = 40 + (hop ? 8 : 0), l4len = protocol == 6 ? 20 : 8, total = l4 + l4len + payload;
    memset(p, 0, total);
    p[0] = 0x60; wr16(p + 4, (uint32_t)(total - 40)); p[7] = 64;
    memcpy(p + 8, src6, 16); memcpy(p + 24, dst6, 16);
    if (hop) { p[6] = 0; p[40] = (uint8_t)protocol; p[41] = 0; p[42] = 1; p[43] = 4; }   /* PadN */
    else p[6] = (uint8_t)protocol;
    if (protocol == 6) {
        wr16(p + l4, 40000); wr16(p + l4 + 2, 443); wr32(p + l4 + 4, 7);
        p[l4 + 12] = (uint8_t)(5 << 4); p[l4 + 13] = 0x10;
    } else {
        wr16(p + l4, 5353); wr16(p + l4 + 2, 53); wr16(p + l4 + 4, (uint32_t)(l4len + payload));
    }
    fill(p + l4 + l4len, payload, seed);
    l4_checksum(p, 1, l4, total, protocol, protocol == 6 ? 16 : 6);
    return total;
}

typedef struct collected {
    uint8_t *data[2048];
    size_t size[2048];
    size_t count;
} collected;

static int collect(const uint8_t *segment, size_t length, void *context)
{
    collected *c = context;
    CHECK(c->count < 2048);
    c->data[c->count] = malloc(length); CHECK(c->data[c->count] != NULL);
    memcpy(c->data[c->count], segment, length);
    c->size[c->count++] = length;
    return 1;
}

static void release(collected *c) { size_t i; for (i = 0; i < c->count; ++i) free(c->data[i]); c->count = 0; }

static uint8_t original[70000], scratch[70000], joined[70000];

/* TCP segments: valid checksums, consecutive sequence numbers, the original
 * data, FIN/PSH on the last segment only. */
static void check_tcp(const uint8_t *in, size_t length, int v6, size_t mtu, collected *c)
{
    size_t l4 = v6 ? 40 : (size_t)(in[0] & 15) * 4, doff = (size_t)(in[l4 + 12] >> 4) * 4, at = 0, i;
    uint32_t seq = be32(in + l4 + 4);
    CHECK(c->count >= 2);
    for (i = 0; i < c->count; ++i) {
        const uint8_t *s = c->data[i];
        size_t n = c->size[i], data;
        uint8_t pseudo[40];
        size_t plen;
        CHECK(n <= mtu);
        if (!v6) {
            CHECK(be16(s + 2) == n && fold(sum(s, l4, 0)) == 0xffff);
            CHECK((be16(s + 6) & 0x3fff) == 0);
            memcpy(pseudo, s + 12, 8); pseudo[8] = 0; pseudo[9] = 6; wr16(pseudo + 10, (uint32_t)(n - l4)); plen = 12;
        } else {
            CHECK(be16(s + 4) + 40 == n);
            memcpy(pseudo, s + 8, 32); wr32(pseudo + 32, (uint32_t)(n - l4));
            pseudo[36] = pseudo[37] = pseudo[38] = 0; pseudo[39] = 6; plen = 40;
        }
        CHECK(fold(sum(s + l4, n - l4, sum(pseudo, plen, 0))) == 0xffff);
        CHECK(be32(s + l4 + 4) == seq + (uint32_t)at);
        CHECK(memcmp(s + l4 + 20, in + l4 + 20, doff - 20) == 0);   /* TCP options kept */
        if (i + 1 < c->count) CHECK((s[l4 + 13] & 0x09) == 0);
        else CHECK((s[l4 + 13] & 0x09) == (in[l4 + 13] & 0x09));
        data = n - l4 - doff;
        memcpy(joined + at, s + l4 + doff, data);
        at += data;
    }
    CHECK(at == length - l4 - doff && memcmp(joined, in + l4 + doff, at) == 0);
}

/* IPv4 fragments: offsets, MF, header checksums, options, and the payload. */
static void check_ipv4_fragments(const uint8_t *in, size_t length, size_t mtu, collected *c)
{
    size_t header = (size_t)(in[0] & 15) * 4, at = 0, i;
    CHECK(c->count >= 2);
    for (i = 0; i < c->count; ++i) {
        const uint8_t *f = c->data[i];
        size_t n = c->size[i], hl = (size_t)(f[0] & 15) * 4;
        uint32_t field = be16(f + 6);
        CHECK(n <= mtu && be16(f + 2) == n && fold(sum(f, hl, 0)) == 0xffff);
        CHECK(be16(f + 4) == be16(in + 4) && memcmp(f + 12, in + 12, 8) == 0 && f[9] == in[9]);
        CHECK((field & 0x1fff) * 8 == at);
        CHECK(((field & 0x2000) != 0) == (i + 1 < c->count));
        if (i == 0) CHECK(hl == header && memcmp(f + 20, in + 20, header - 20) == 0);
        else if (header > 20) CHECK(hl == 32 && f[20] == 130 && f[21] == 11 && f[31] == 0);   /* copied option only */
        else CHECK(hl == 20);
        if (i + 1 < c->count) CHECK((n - hl) % 8 == 0);
        memcpy(joined + at, f + hl, n - hl);
        at += n - hl;
    }
    CHECK(at == length - header && memcmp(joined, in + header, at) == 0);
}

static void check_ipv6_fragments(const uint8_t *in, size_t length, int hop, size_t mtu, uint32_t id, collected *c)
{
    size_t unfrag = 40 + (hop ? 8 : 0), at = 0, i;
    uint8_t first_next = hop ? in[40] : in[6];
    CHECK(c->count >= 2);
    for (i = 0; i < c->count; ++i) {
        const uint8_t *f = c->data[i];
        size_t n = c->size[i];
        uint32_t field;
        CHECK(n <= mtu && be16(f + 4) + 40 == n);
        CHECK(memcmp(f + 8, in + 8, 32) == 0);
        if (hop) CHECK(f[6] == 0 && f[40] == 44 && memcmp(f + 41, in + 41, 7) == 0);
        else CHECK(f[6] == 44);
        CHECK(f[unfrag] == first_next && f[unfrag + 1] == 0 && be32(f + unfrag + 4) == id);
        field = be16(f + unfrag + 2);
        CHECK((field & ~7u) == at && (field & 6u) == 0);
        CHECK(((field & 1u) != 0) == (i + 1 < c->count));
        if (i + 1 < c->count) CHECK((n - unfrag - 8) % 8 == 0);
        memcpy(joined + at, f + unfrag + 8, n - unfrag - 8);
        at += n - unfrag - 8;
    }
    CHECK(at == length - unfrag && memcmp(joined, in + unfrag, at) == 0);
}

int main(void)
{
    collected c;
    size_t n, mtu, payload;
    unsigned round = 0;
    memset(&c, 0, sizeof(c));

    /* Fits: delivered unchanged, no scratch needed. */
    n = ipv4(original, 0, 17, 1000, 0, 1);
    CHECK(chmlink_ip_split(original, n, 1500, 0, NULL, 0, collect, &c) == CHMLINK_OK);
    CHECK(c.count == 1 && c.size[0] == n && memcmp(c.data[0], original, n) == 0); release(&c);

    /* Arguments and malformed packets. */
    n = ipv4(original, 0, 17, 3000, 0, 1);
    CHECK(chmlink_ip_split(original, n, 575, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_INVALID_ARGUMENT);
    CHECK(chmlink_ip_split(original, n, 1500, 0, scratch, 1499, collect, &c) == CHMLINK_BUFFER_TOO_SMALL);
    CHECK(chmlink_ip_split(original, n - 1, 1500, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_INVALID_PACKET);
    n = ipv6(original, 0, 17, 3000, 1);
    CHECK(chmlink_ip_split(original, n, 1279, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_INVALID_ARGUMENT);
    original[0] = 0x50;
    CHECK(chmlink_ip_split(original, n, 1500, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_INVALID_PACKET);
    CHECK(c.count == 0);

    /* UDP with DF cannot be fragmented. */
    n = ipv4(original, 0, 17, 3000, 1, 2);
    CHECK(chmlink_ip_split(original, n, 1500, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_TOO_BIG && c.count == 0);

    /* Sizes from just over the MTU to the largest IP packet, several MTUs. */
    {
    static const size_t mtus[] = { 1280, 1420, 1500, 9000 };
    size_t m;
    for (m = 0; m < sizeof(mtus) / sizeof(mtus[0]); ++m) {
        mtu = mtus[m];
        for (payload = mtu - 20; payload < 65400; payload = payload * 3 / 2 + 13, ++round) {
            n = ipv4(original, 0, 6, payload, 1, round);   /* TCP always has DF */
            if (n > 65535) break;
            if (n > mtu) {
                CHECK(chmlink_ip_split(original, n, mtu, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_OK);
                check_tcp(original, n, 0, mtu, &c); release(&c);
            }
            n = ipv4(original, 24, 17, payload, 0, round);
            if (n > mtu) {
                CHECK(chmlink_ip_split(original, n, mtu, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_OK);
                check_ipv4_fragments(original, n, mtu, &c); release(&c);
            }
            n = ipv4(original, 0, 17, payload, 0, round);
            if (n > mtu) {
                CHECK(chmlink_ip_split(original, n, mtu, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_OK);
                check_ipv4_fragments(original, n, mtu, &c); release(&c);
            }
            if (40 + 8 + 20 + payload > 65535) continue;   /* the longest IPv6 case below */
            n = ipv6(original, 0, 6, payload, round);
            if (n > mtu) {
                CHECK(chmlink_ip_split(original, n, mtu, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_OK);
                check_tcp(original, n, 1, mtu, &c); release(&c);
            }
            n = ipv6(original, 0, 17, payload, round);
            if (n > mtu) {
                CHECK(chmlink_ip_split(original, n, mtu, 0xabcd0000u + round, scratch, sizeof(scratch), collect, &c) == CHMLINK_OK);
                check_ipv6_fragments(original, n, 0, mtu, 0xabcd0000u + round, &c); release(&c);
            }
            n = ipv6(original, 1, 17, payload, round);
            if (n > mtu) {
                CHECK(chmlink_ip_split(original, n, mtu, 77, scratch, sizeof(scratch), collect, &c) == CHMLINK_OK);
                check_ipv6_fragments(original, n, 1, mtu, 77, &c); release(&c);
            }
            /* TCP behind an extension header is fragmented, not segmented. */
            n = ipv6(original, 1, 6, payload, round);
            if (n > mtu) {
                CHECK(chmlink_ip_split(original, n, mtu, 78, scratch, sizeof(scratch), collect, &c) == CHMLINK_OK);
                check_ipv6_fragments(original, n, 1, mtu, 78, &c); release(&c);
            }
        }
    }
    }
    /* The largest IP packet at the smallest IPv6 MTU. */
    n = ipv4(original, 0, 6, 65535 - 20 - 32, 1, 9);
    CHECK(n == 65535);
    CHECK(chmlink_ip_split(original, n, 1280, 0, scratch, sizeof(scratch), collect, &c) == CHMLINK_OK);
    check_tcp(original, n, 0, 1280, &c); release(&c);
    puts("split ok");
    return 0;
}
