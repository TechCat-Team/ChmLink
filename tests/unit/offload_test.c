#include "chmlink/offload.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static uint8_t original[70000], work[70000], scratch[70000], joined[70000];

/* Independent reference: plain 32-bit accumulation, folded at the end. */
static uint16_t reference_sum(const uint8_t *p, size_t n)
{
    unsigned long sum = 0;
    size_t i;
    for (i = 0; i < n; ++i) sum += (i & 1) ? p[i] : (unsigned long)p[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum;
}

static int tcp_ok(const uint8_t *ip, size_t length, int v6)
{
    uint8_t pseudo[40];
    size_t l4 = v6 ? 40 : (size_t)(ip[0] & 15) * 4, n;
    unsigned long sum;
    memset(pseudo, 0, sizeof(pseudo));
    if (v6) { memcpy(pseudo, ip + 8, 32); n = 40;
        pseudo[34] = (uint8_t)((length - l4) >> 8); pseudo[35] = (uint8_t)(length - l4); pseudo[39] = ip[6]; }
    else { memcpy(pseudo, ip + 12, 8); n = 12; pseudo[9] = ip[9];
        pseudo[10] = (uint8_t)((length - l4) >> 8); pseudo[11] = (uint8_t)(length - l4); }
    sum = (unsigned long)reference_sum(pseudo, n) + reference_sum(ip + l4, length - l4);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return sum == 0xffff;
}

typedef struct collect { size_t count, payload, mss; int v6; size_t l3; uint32_t seq0; int failed; } collect;

static int on_segment(const uint8_t *segment, size_t length, void *context)
{
    collect *c = context;
    const uint8_t *ip = segment + c->l3;
    size_t ip_len = length - c->l3, l4 = c->v6 ? 40 : 20, header = c->l3 + l4 + 20, chunk = length - header;
    uint32_t seq = ((uint32_t)ip[l4 + 4] << 24) | ((uint32_t)ip[l4 + 5] << 16) | ((uint32_t)ip[l4 + 6] << 8) | ip[l4 + 7];
    if (chunk == 0 || chunk > c->mss) c->failed = 1;
    if (seq != c->seq0 + (uint32_t)c->payload) c->failed = 2;
    if (!c->v6) {
        if ((size_t)((ip[2] << 8) | ip[3]) != ip_len) c->failed = 3;
        if (reference_sum(ip, 20) != 0xffff) c->failed = 4;
        if ((size_t)(((ip[4] << 8) | ip[5]) - 0x10) != c->count) c->failed = 5;
    } else if ((size_t)((ip[4] << 8) | ip[5]) != ip_len - 40) c->failed = 6;
    if (!tcp_ok(ip, ip_len, c->v6)) c->failed = 7;
    memcpy(joined + c->payload, segment + header, chunk);
    c->payload += chunk;
    c->count++;
    return 1;
}

static size_t build_tcp(uint8_t *p, size_t l3, size_t total_ip, int v6, uint8_t flags)
{
    uint8_t *ip = p + l3;
    size_t l4 = v6 ? 40 : 20, i;
    memset(p, 0, l3 + total_ip);
    if (l3) { p[12] = v6 ? 0x86 : 0x08; p[13] = v6 ? 0xdd : 0x00; }
    if (v6) { ip[0] = 0x60; ip[4] = (uint8_t)((total_ip - 40) >> 8); ip[5] = (uint8_t)(total_ip - 40); ip[6] = 6; ip[7] = 64;
        for (i = 0; i < 32; ++i) ip[8 + i] = (uint8_t)(i * 7 + 1); }
    else { ip[0] = 0x45; ip[2] = (uint8_t)(total_ip >> 8); ip[3] = (uint8_t)total_ip; ip[4] = 0; ip[5] = 0x10; ip[8] = 64; ip[9] = 6;
        ip[12] = 10; ip[15] = 1; ip[16] = 10; ip[19] = 2; }
    ip[l4 + 1] = 80; ip[l4 + 3] = 81;
    ip[l4 + 4] = 0xff; ip[l4 + 5] = 0xff; ip[l4 + 6] = 0xf0; ip[l4 + 7] = 0x00;   /* sequence wraps */
    ip[l4 + 12] = 5 << 4; ip[l4 + 13] = flags;
    for (i = l4 + 20; i < total_ip; ++i) ip[i] = (uint8_t)(i * 13);
    return l3 + total_ip;
}

static int segment_case(chmlink_mode mode, int v6, size_t total_ip, uint16_t mss)
{
    size_t l3 = mode == CHMLINK_MODE_ETHERNET ? 14 : 0, length = build_tcp(original, l3, total_ip, v6, 0x80 | 0x08 | 0x01 | 0x10);
    chmlink_offload o = {CHMLINK_OFFLOAD_NEEDS_CSUM, v6 ? CHMLINK_GSO_TCPV6 : CHMLINK_GSO_TCPV4, 0, mss, 0, 16};
    collect c;
    size_t payload = total_ip - (v6 ? 60 : 40), expected = (payload + mss - 1) / mss;
    o.csum_start = (uint16_t)(l3 + (v6 ? 40 : 20)); o.hdr_len = (uint16_t)(o.csum_start + 20);
    memcpy(work, original, length);
    memset(&c, 0, sizeof(c)); c.mss = mss; c.v6 = v6; c.l3 = l3; c.seq0 = 0xfffff000u;
    CHECK(chmlink_gso_segment(mode, work, length, &o, scratch, sizeof(scratch), on_segment, &c) == CHMLINK_OK);
    CHECK(c.failed == 0 && c.count == expected && c.payload == payload);
    CHECK(memcmp(joined, original + o.hdr_len, payload) == 0);
    CHECK(chmlink_gso_segment(mode, work, length, &o, scratch, (size_t)o.hdr_len + mss - 1, on_segment, &c) == CHMLINK_BUFFER_TOO_SMALL);
    return 0;
}

/* Flags: CWR only first, FIN/PSH only last. */
static int flags_seen[64], flags_count;
static int on_flags(const uint8_t *segment, size_t length, void *context)
{
    (void)length; (void)context;
    flags_seen[flags_count++] = segment[20 + 13];
    return 1;
}

int main(void)
{
    static const uint8_t rfc1071[8] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    chmlink_offload csum = {CHMLINK_OFFLOAD_NEEDS_CSUM, 0, 0, 0, 20, 16}, gso = {CHMLINK_OFFLOAD_NEEDS_CSUM, CHMLINK_GSO_TCPV4, 40, 1000, 20, 16};
    size_t length;
    int i;
    CHECK(chmlink_checksum_add(rfc1071, 8, 0) == 0xddf2);       /* RFC 1071 section 3 */
    CHECK(chmlink_checksum_add(rfc1071, 7, 0) == reference_sum(rfc1071, 7));

    CHECK(segment_case(CHMLINK_MODE_IP, 0, 30000, 1460) == 0);
    CHECK(segment_case(CHMLINK_MODE_IP, 0, 64000, 1448) == 0);
    CHECK(segment_case(CHMLINK_MODE_IP, 1, 20000, 1440) == 0);
    CHECK(segment_case(CHMLINK_MODE_ETHERNET, 0, 9000, 1460) == 0);
    CHECK(segment_case(CHMLINK_MODE_ETHERNET, 1, 9000, 1000) == 0);
    CHECK(segment_case(CHMLINK_MODE_IP, 0, 41 + 20 - 20, 1) == 0);        /* one payload byte */

    length = build_tcp(work, 0, 3040, 0, 0x80 | 0x08 | 0x01 | 0x10);
    flags_count = 0;
    CHECK(chmlink_gso_segment(CHMLINK_MODE_IP, work, length, &gso, scratch, sizeof(scratch), on_flags, NULL) == CHMLINK_OK);
    CHECK(flags_count == 3);
    CHECK(flags_seen[0] == 0x90 && flags_seen[1] == 0x10 && flags_seen[2] == 0x19);

    /* Checksum completion without segmentation, TCP and UDP. */
    length = build_tcp(work, 0, 1500, 0, 0x10);
    work[20 + 16] = 0x12; work[20 + 17] = 0x34;   /* garbage, as a stack leaves it */
    CHECK(chmlink_offload_complete(CHMLINK_MODE_IP, work, length, &csum) == CHMLINK_OK);
    CHECK(tcp_ok(work, length, 0));
    work[9] = 17; csum.csum_offset = 6;
    CHECK(chmlink_offload_complete(CHMLINK_MODE_IP, work, length, &csum) == CHMLINK_OK);
    CHECK(work[26] != 0 || work[27] != 0);
    csum.csum_start = 1499;
    CHECK(chmlink_offload_complete(CHMLINK_MODE_IP, work, length, &csum) == CHMLINK_INVALID_PACKET);
    /* A malformed GSO descriptor is refused, not followed. */
    gso.hdr_len = 1600;
    CHECK(chmlink_gso_segment(CHMLINK_MODE_IP, work, length, &gso, scratch, sizeof(scratch), on_flags, NULL) == CHMLINK_INVALID_PACKET);
    for (i = 0; i < 1; ++i) (void)i;
    puts("offload ok");
    return 0;
}
