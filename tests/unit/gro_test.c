/* Software GRO: merged packets carry the bytes of their segments with correct
 * headers; cutting them again (chmlink_ip_split, chmlink_gso_segment) gives
 * the original segments back. Checksums are checked with a separate sum. */
#include "chmlink/gro.h"
#include "chmlink/offload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } \
} while (0)

#define MAX_OUT 64
typedef struct output {
    uint8_t *packet[MAX_OUT];
    size_t length[MAX_OUT];
    chmlink_offload offload[MAX_OUT];
    int has_offload[MAX_OUT];
    int count;
} output;

static void collect(const uint8_t *packet, size_t length, const chmlink_offload *offload, void *context)
{
    output *o = context;
    CHECK(o->count < MAX_OUT);
    o->packet[o->count] = malloc(length);
    memcpy(o->packet[o->count], packet, length);
    o->length[o->count] = length;
    o->has_offload[o->count] = offload != NULL;
    if (offload) o->offload[o->count] = *offload;
    o->count++;
}

static void reset(output *o)
{
    int i;
    for (i = 0; i < o->count; ++i) free(o->packet[i]);
    o->count = 0;
}

/* Independent RFC 1071 sum. */
static uint32_t sum_bytes(const uint8_t *p, size_t n, uint32_t sum)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) sum += (uint32_t)p[n - 1] << 8;
    return sum;
}
static uint16_t finish(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Valid checksums for an IP packet at p (IPv4 or IPv6, TCP or UDP). */
static void fix(uint8_t *ip, size_t length)
{
    size_t l4, n;
    uint32_t sum;
    uint8_t *field;
    if (ip[0] >> 4 == 4) {
        ip[10] = ip[11] = 0;
        sum = finish(sum_bytes(ip, 20, 0));
        ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;
        l4 = 20; n = length - l4;
        sum = sum_bytes(ip + 12, 8, 0) + ip[9] + (uint32_t)n;
    } else {
        l4 = 40; n = length - l4;
        sum = sum_bytes(ip + 8, 32, 0) + ip[6] + (uint32_t)n;
    }
    field = ip + l4 + (ip[(ip[0] >> 4 == 4) ? 9 : 6] == 6 ? 16 : 6);
    field[0] = field[1] = 0;
    sum = finish(sum_bytes(ip + l4, n, sum));
    field[0] = (uint8_t)(sum >> 8); field[1] = (uint8_t)sum;
}

static int checksums_ok(const uint8_t *ip, size_t length)
{
    size_t l4 = ip[0] >> 4 == 4 ? 20 : 40, n = length - l4;
    uint32_t sum;
    if (ip[0] >> 4 == 4) {
        if (finish(sum_bytes(ip, 20, 0)) != 0) return 0;
        if ((size_t)(ip[2] << 8 | ip[3]) != length) return 0;
        sum = sum_bytes(ip + 12, 8, 0) + 6 + (uint32_t)n;
    } else {
        if ((size_t)(ip[4] << 8 | ip[5]) != length - 40) return 0;
        sum = sum_bytes(ip + 8, 32, 0) + 6 + (uint32_t)n;
    }
    return finish(sum_bytes(ip + l4, n, sum)) == 0;
}

/* A TCP segment of flow `port` with `payload` bytes starting at seq; IPv4 ID
 * and payload bytes follow the sequence so pieces of a stream are distinct. */
static size_t tcp_segment(uint8_t *ip, int v6, unsigned port, uint32_t seq, size_t payload, uint8_t flags,
                          uint16_t id, int timestamps)
{
    size_t l4 = v6 ? 40 : 20, th = timestamps ? 32 : 20, i, length = l4 + th + payload;
    uint8_t *t = ip + l4;
    memset(ip, 0, l4 + th);
    if (v6) {
        ip[0] = 0x60; ip[4] = (uint8_t)((th + payload) >> 8); ip[5] = (uint8_t)(th + payload);
        ip[6] = 6; ip[7] = 64; ip[8] = 0xfd; ip[23] = 1; ip[24] = 0xfd; ip[39] = 2;
    } else {
        ip[0] = 0x45; ip[2] = (uint8_t)(length >> 8); ip[3] = (uint8_t)length;
        ip[4] = (uint8_t)(id >> 8); ip[5] = (uint8_t)id; ip[6] = 0x40; ip[8] = 64; ip[9] = 6;
        ip[12] = 10; ip[15] = 1; ip[16] = 10; ip[19] = 2;
    }
    t[0] = (uint8_t)(port >> 8); t[1] = (uint8_t)port; t[2] = 0x01; t[3] = 0xbb;
    t[4] = (uint8_t)(seq >> 24); t[5] = (uint8_t)(seq >> 16); t[6] = (uint8_t)(seq >> 8); t[7] = (uint8_t)seq;
    t[8] = 0x11; t[9] = 0x22; t[10] = 0x33; t[11] = 0x44;
    t[12] = (uint8_t)((th / 4) << 4); t[13] = flags; t[14] = 0x20; t[15] = 0x00;
    if (timestamps) { t[20] = 1; t[21] = 1; t[22] = 8; t[23] = 10; t[24] = 0xaa; t[31] = 0x55; }
    for (i = 0; i < payload; ++i) ip[l4 + th + i] = (uint8_t)(seq + i);
    fix(ip, length);
    return length;
}

static size_t split_count;
static uint8_t split_seen[64][2048];
static size_t split_length[64];
static int keep(const uint8_t *segment, size_t length, void *context)
{
    (void)context;
    CHECK(split_count < 64 && length <= 2048);
    memcpy(split_seen[split_count], segment, length);
    split_length[split_count++] = length;
    return 1;
}

int main(void)
{
    static uint8_t memory[CHMLINK_GRO_MEMORY(65535)], scratch[70000], seg[64][2048], frame[70000];
    size_t len[64], i, total;
    chmlink_gro g;
    output o = {0};
    int v6;

    /* 1. IP mode, IPv4 and IPv6, timestamps: ten 1300-byte segments become one
     *    packet with exact headers, and splitting it gives them back. */
    for (v6 = 0; v6 < 2; ++v6) {
        CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 65535, 0, memory, sizeof(memory), collect, &o) == CHMLINK_OK);
        for (i = 0; i < 10; ++i) {
            len[i] = tcp_segment(seg[i], v6, 40000, 1000 + 1300 * (uint32_t)i, 1300, i == 9 ? 0x18 : 0x10,
                                 (uint16_t)(7 + i), 1);
            CHECK(chmlink_gro_add(&g, seg[i], len[i]) == CHMLINK_OK);
        }
        /* PSH ended the merge without a flush. */
        CHECK(o.count == 1 && !o.has_offload[0]);
        CHECK(o.length[0] == (v6 ? 40 : 20) + 32 + 13000 && checksums_ok(o.packet[0], o.length[0]));
        CHECK((o.packet[0][(v6 ? 40 : 20) + 13] & 0x08) != 0);
        split_count = 0;
        CHECK(chmlink_ip_split(o.packet[0], o.length[0], len[0], 0, scratch, sizeof(scratch), keep, NULL) == CHMLINK_OK);
        CHECK(split_count == 10);
        for (i = 0; i < 10; ++i) CHECK(split_length[i] == len[i] && memcmp(split_seen[i], seg[i], len[i]) == 0);
        reset(&o);
    }

    /* 2. A shorter segment ends the merge; a gap or a changed acknowledgement
     *    starts a new packet; nothing is held after a flush. */
    CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 65535, 0, memory, sizeof(memory), collect, &o) == CHMLINK_OK);
    len[0] = tcp_segment(seg[0], 0, 40000, 0, 1200, 0x10, 1, 0);
    len[1] = tcp_segment(seg[1], 0, 40000, 1200, 1200, 0x10, 2, 0);
    len[2] = tcp_segment(seg[2], 0, 40000, 4800, 1200, 0x10, 3, 0);        /* gap */
    len[3] = tcp_segment(seg[3], 0, 40000, 6000, 1200, 0x10, 4, 0);
    seg[3][20 + 11] ^= 1; fix(seg[3], len[3]);                             /* other ack */
    len[4] = tcp_segment(seg[4], 0, 40000, 7200, 500, 0x10, 5, 0);         /* short: last */
    seg[4][20 + 11] ^= 1; fix(seg[4], len[4]);
    for (i = 0; i < 5; ++i) CHECK(chmlink_gro_add(&g, seg[i], len[i]) == CHMLINK_OK);
    CHECK(o.count == 3);                     /* 0+1, 2, then 3+4 ended by the short one */
    CHECK(o.length[0] == 20 + 20 + 2400 && checksums_ok(o.packet[0], o.length[0]));
    CHECK(o.length[1] == len[2] && memcmp(o.packet[1], seg[2], len[2]) == 0);
    CHECK(o.length[2] == 20 + 20 + 1700 && checksums_ok(o.packet[2], o.length[2]));
    chmlink_gro_flush(&g);
    CHECK(o.count == 3);
    reset(&o);

    /* 3. IP mode reads no payload: a corrupt segment merges, and the merged
     *    checksum fails as the segment's did. Pure acknowledgements pass;
     *    two flows merge independently. */
    CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 65535, 0, memory, sizeof(memory), collect, &o) == CHMLINK_OK);
    len[0] = tcp_segment(seg[0], 0, 40000, 0, 1000, 0x10, 1, 0);
    len[1] = tcp_segment(seg[1], 0, 40001, 0, 1000, 0x10, 1, 0);
    len[2] = tcp_segment(seg[2], 0, 40000, 1000, 1000, 0x10, 2, 0);
    len[3] = tcp_segment(seg[3], 0, 40001, 1000, 1000, 0x10, 2, 0);
    len[4] = tcp_segment(seg[4], 0, 40000, 2000, 1000, 0x10, 3, 0); seg[4][100] ^= 0xff;   /* corrupt */
    len[5] = tcp_segment(seg[5], 0, 40002, 0, 0, 0x10, 1, 0);              /* pure ACK */
    for (i = 0; i < 6; ++i) CHECK(chmlink_gro_add(&g, seg[i], len[i]) == CHMLINK_OK);
    CHECK(o.count == 1 && o.length[0] == len[5]);
    chmlink_gro_flush(&g);
    CHECK(o.count == 3);
    CHECK(o.length[1] == 20 + 20 + 3000 && o.packet[1][21] == 0x40 && !checksums_ok(o.packet[1], o.length[1]));
    CHECK(o.length[2] == 20 + 20 + 2000 && o.packet[2][21] == 0x41 && checksums_ok(o.packet[2], o.length[2]));
    reset(&o);

    /* 3b. Ethernet (RSC: Windows trusts the result) verifies every segment and
     *     passes a corrupt one on untouched, after what its flow holds; with
     *     CHMLINK_GRO_TRUSTED it reads no payload and merges it. */
    {
        int trusted;
        for (trusted = 0; trusted < 2; ++trusted) {
            CHECK(chmlink_gro_init(&g, CHMLINK_MODE_ETHERNET, 65535, trusted ? CHMLINK_GRO_TRUSTED : 0, memory,
                                   sizeof(memory), collect, &o) == CHMLINK_OK);
            for (i = 0; i < 3; ++i) {
                size_t n = tcp_segment(frame + 14, 0, 40000, 1000 * (uint32_t)i, 1000, 0x10, (uint16_t)i, 0);
                memset(frame, 0, 14); frame[12] = 8;
                if (i == 2) frame[14 + 200] ^= 0xff;
                memcpy(seg[i], frame, 14 + n); len[i] = 14 + n;
                CHECK(chmlink_gro_add(&g, seg[i], len[i]) == CHMLINK_OK);
            }
            chmlink_gro_flush(&g);
            if (!trusted) {
                CHECK(o.count == 2 && o.has_offload[0] && o.length[0] == 14 + 40 + 2000);
                CHECK(!o.has_offload[1] && memcmp(o.packet[1], seg[2], len[2]) == 0);
            } else {
                CHECK(o.count == 1 && o.has_offload[0] && o.length[0] == 14 + 40 + 3000);
            }
            reset(&o);
        }
    }

    /* 4. The limit caps the merged packet. */
    CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 3000, 0, memory, sizeof(memory), collect, &o) == CHMLINK_OK);
    for (i = 0; i < 4; ++i) {
        len[i] = tcp_segment(seg[i], 0, 40000, 1000 * (uint32_t)i, 1000, 0x10, (uint16_t)i, 0);
        CHECK(chmlink_gro_add(&g, seg[i], len[i]) == CHMLINK_OK);
    }
    chmlink_gro_flush(&g);
    CHECK(o.count == 2 && o.length[0] == 40 + 2000 && o.length[1] == 40 + 2000);
    reset(&o);

    /* 5. Odd segment sizes keep the checksum right. */
    CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 65535, 0, memory, sizeof(memory), collect, &o) == CHMLINK_OK);
    for (i = 0; i < 5; ++i) {
        len[i] = tcp_segment(seg[i], 0, 40000, 999 * (uint32_t)i, 999, 0x10, (uint16_t)i, 0);
        CHECK(chmlink_gro_add(&g, seg[i], len[i]) == CHMLINK_OK);
    }
    chmlink_gro_flush(&g);
    CHECK(o.count == 1 && o.length[0] == 40 + 4995 && checksums_ok(o.packet[0], o.length[0]));
    reset(&o);

    /* 6. Ethernet: a GSO frame for RSC that chmlink_offload_valid accepts and
     *    chmlink_gso_segment turns back into the original frames. */
    CHECK(chmlink_gro_init(&g, CHMLINK_MODE_ETHERNET, 65535, 0, memory, sizeof(memory), collect, &o) == CHMLINK_OK);
    for (i = 0; i < 8; ++i) {
        size_t n = tcp_segment(frame + 14, 0, 40000, 1448 * (uint32_t)i, 1448, 0x10, (uint16_t)(100 + i), 1);
        memset(frame, 0, 14); frame[0] = 2; frame[6] = 4; frame[12] = 8;
        memcpy(seg[i], frame, 14 + n); len[i] = 14 + n;
        CHECK(chmlink_gro_add(&g, seg[i], len[i]) == CHMLINK_OK);
    }
    chmlink_gro_flush(&g);
    CHECK(o.count == 1 && o.has_offload[0]);
    total = o.length[0];
    CHECK(total == 14 + 20 + 32 + 8 * 1448);
    CHECK(o.offload[0].gso_type == CHMLINK_GSO_TCPV4 && o.offload[0].gso_size == 1448 &&
          o.offload[0].hdr_len == 14 + 52 && (o.offload[0].flags & CHMLINK_OFFLOAD_DATA_VALID));
    CHECK(chmlink_offload_valid(CHMLINK_MODE_ETHERNET, o.packet[0], total, 1514, &o.offload[0]));
    CHECK(checksums_ok(o.packet[0] + 14, total - 14));
    split_count = 0;
    CHECK(chmlink_gso_segment(CHMLINK_MODE_ETHERNET, o.packet[0], total, &o.offload[0], scratch, sizeof(scratch),
                              keep, NULL) == CHMLINK_OK);
    CHECK(split_count == 8);
    for (i = 0; i < 8; ++i) CHECK(split_length[i] == len[i] && memcmp(split_seen[i], seg[i], len[i]) == 0);
    reset(&o);

    /* 7. More flows than slots: the oldest is let out first; nothing is lost. */
    CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 65535, 0, memory, sizeof(memory), collect, &o) == CHMLINK_OK);
    for (i = 0; i < CHMLINK_GRO_FLOWS + 2; ++i) {
        len[i] = tcp_segment(seg[i], 0, 30000 + (unsigned)i, 0, 100, 0x10, 1, 0);
        CHECK(chmlink_gro_add(&g, seg[i], len[i]) == CHMLINK_OK);
    }
    CHECK(o.count == 2 && memcmp(o.packet[0], seg[0], len[0]) == 0 && memcmp(o.packet[1], seg[1], len[1]) == 0);
    chmlink_gro_flush(&g);
    CHECK(o.count == (int)CHMLINK_GRO_FLOWS + 2 && g.packets_in == g.packets_out);
    reset(&o);

    /* 8. A segment larger than the first does not merge (segments of a GSO
     *    packet are all the first one's size). */
    CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 65535, 0, memory, sizeof(memory), collect, &o) == CHMLINK_OK);
    len[0] = tcp_segment(seg[0], 0, 40000, 0, 1000, 0x10, 1, 0);
    len[1] = tcp_segment(seg[1], 0, 40000, 1000, 1200, 0x10, 2, 0);
    CHECK(chmlink_gro_add(&g, seg[0], len[0]) == CHMLINK_OK && chmlink_gro_add(&g, seg[1], len[1]) == CHMLINK_OK);
    chmlink_gro_flush(&g);
    CHECK(o.count == 2 && o.length[0] == len[0] && o.length[1] == len[1]);
    reset(&o);

    /* 9. TCP that does not show its ports here (behind an IPv6 extension
     *    header, or an IPv4 fragment) may belong to a held flow: what is held
     *    between those hosts goes out first, then the packet, unchanged. */
    {
        int v6;
        for (v6 = 0; v6 < 2; ++v6) {
            uint8_t odd[2048];
            size_t n;
            CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 65535, 0, memory, sizeof(memory), collect, &o) == CHMLINK_OK);
            len[0] = tcp_segment(seg[0], v6, 40000, 0, 1000, 0x10, 1, 0);
            len[1] = tcp_segment(seg[1], v6, 40000, 1000, 1000, 0x10, 2, 0);
            CHECK(chmlink_gro_add(&g, seg[0], len[0]) == CHMLINK_OK && chmlink_gro_add(&g, seg[1], len[1]) == CHMLINK_OK);
            n = tcp_segment(odd, v6, 40000, 2000, 1000, 0x10, 3, 0);
            if (v6) {
                /* insert an 8-byte Hop-by-Hop header after the IPv6 header */
                memmove(odd + 48, odd + 40, n - 40);
                memset(odd + 40, 0, 8); odd[40] = 6; odd[6] = 0;
                n += 8; odd[4] = (uint8_t)((n - 40) >> 8); odd[5] = (uint8_t)(n - 40);
            } else {
                odd[6] = 0x20;   /* more fragments */
            }
            CHECK(chmlink_gro_add(&g, odd, n) == CHMLINK_OK);
            CHECK(o.count == 2 && o.length[0] == (v6 ? 40u : 20u) + 20 + 2000 && o.length[1] == n &&
                  memcmp(o.packet[1], odd, n) == 0);
            chmlink_gro_flush(&g);
            CHECK(o.count == 2);
            reset(&o);
        }
    }

    CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 65535, 0, memory, 100, collect, &o) == CHMLINK_BUFFER_TOO_SMALL);
    CHECK(chmlink_gro_init(&g, CHMLINK_MODE_IP, 65535, 2, memory, sizeof(memory), collect, &o) == CHMLINK_INVALID_ARGUMENT);
    CHECK(chmlink_gro_add(&g, NULL, 10) == CHMLINK_INVALID_ARGUMENT);
    puts("gro ok");
    return 0;
}
