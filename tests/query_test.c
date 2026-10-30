#include <stdio.h>
#include <string.h>
#include "chmlink/query.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

/* The wire contract is frozen at device version 3. These helpers use the
 * literal offsets of that contract so any drift in query.h is caught. */
static uint32_t ref32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ref64(const uint8_t *p)
{
    return (uint64_t)ref32(p) | ((uint64_t)ref32(p + 4) << 32);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Reference validation, transcribed from the pre-refactor SDK. Returns
 * 0 valid, 1 revision mismatch, 2 invalid data. */
static int reference_check(const uint8_t *bytes, size_t received)
{
    if (received != 128 || ref32(bytes) != CHMLINK_DEVICE_VERSION ||
        ref32(bytes + 4) != 128) return 1;
    if ((ref32(bytes + 8) & CHMLINK_CAP_BATCH) == 0 ||
        (ref32(bytes + 16) == CHMLINK_MODE_ETHERNET ?
            (!(ref32(bytes + 8) & CHMLINK_CAP_ETHERNET) || (ref32(bytes + 8) & CHMLINK_CAP_IP) ||
             ref32(bytes + 28) < 14 ||
             ref32(bytes + 24) > ref32(bytes + 28) - 14) :
         ref32(bytes + 16) == CHMLINK_MODE_IP ?
            (!(ref32(bytes + 8) & CHMLINK_CAP_IP) || (ref32(bytes + 8) & CHMLINK_CAP_ETHERNET) ||
             ref32(bytes + 28) < 1280 || ref32(bytes + 24) < 1280 ||
             bytes[48] || bytes[49] || bytes[50] || bytes[51] || bytes[52] || bytes[53] ||
             ref32(bytes + 24) != ref32(bytes + 28)) : 1) ||
        ref32(bytes + 28) > CHMLINK_PACKET_MAX_SIZE || ref32(bytes + 24) == 0 ||
        ref32(bytes + 32) == 0 ||
        ref32(bytes + 36) > ref32(bytes + 32)) return 2;
    return 0;
}

static chmlink_query_info ethernet_info(void)
{
    chmlink_query_info info;
    unsigned int i;
    memset(&info, 0, sizeof(info));
    info.version = CHMLINK_DEVICE_VERSION;
    info.capabilities = CHMLINK_CAP_ETHERNET | CHMLINK_CAP_BATCH |
                        CHMLINK_CAP_STATISTICS | CHMLINK_CAP_WAIT;
    info.state = CHMLINK_STATE_RUNNING | CHMLINK_STATE_SESSION;
    info.mode = CHMLINK_MODE_ETHERNET;
    info.interface_index = 0x11223344u; info.mtu = 1500; info.packet_limit = 1522;
    info.queue_capacity = 256; info.queued_packets = 7; info.packet_filter = 0x0badf00du;
    for (i = 0; i < 6; ++i) info.mac[i] = (uint8_t)(0x02 + i * 0x11);
    info.luid = 0x0102030405060708ull;
    info.tx_packets = 0x1111111111111101ull; info.tx_bytes = 0x2222222222222202ull;
    info.tx_errors = 0x3333333333333303ull; info.rx_packets = 0x4444444444444404ull;
    info.rx_bytes = 0x5555555555555505ull; info.rx_discards = 0x6666666666666606ull;
    info.tx_queue_drops = 0x7777777777777707ull;
    return info;
}

/* Field-wise: struct padding is not part of the value. */
static int same_info(const chmlink_query_info *a, const chmlink_query_info *b)
{
    return a->version == b->version && a->capabilities == b->capabilities &&
        a->state == b->state && a->mode == b->mode &&
        a->interface_index == b->interface_index && a->mtu == b->mtu &&
        a->packet_limit == b->packet_limit && a->queue_capacity == b->queue_capacity &&
        a->queued_packets == b->queued_packets && a->packet_filter == b->packet_filter &&
        memcmp(a->mac, b->mac, sizeof(a->mac)) == 0 && a->luid == b->luid &&
        a->tx_packets == b->tx_packets && a->tx_bytes == b->tx_bytes &&
        a->tx_errors == b->tx_errors && a->rx_packets == b->rx_packets &&
        a->rx_bytes == b->rx_bytes && a->rx_discards == b->rx_discards &&
        a->tx_queue_drops == b->tx_queue_drops;
}

static uint32_t rng = 12345;
static uint32_t next_random(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

int main(void)
{
    uint8_t bytes[CHMLINK_INFO_SIZE + 8], copy[CHMLINK_INFO_SIZE];
    chmlink_query_info in = ethernet_info(), out, sentinel;
    unsigned int n;
    size_t len;

    /* Golden layout: every field lands at its frozen offset. */
    memset(bytes, 0xcc, sizeof(bytes));
    chmlink_query_encode(bytes, &in);
    CHECK(bytes[CHMLINK_INFO_SIZE] == 0xcc);
    CHECK(ref32(bytes + 0) == CHMLINK_DEVICE_VERSION && ref32(bytes + 4) == 128);
    CHECK(ref32(bytes + 8) == in.capabilities && ref32(bytes + 12) == in.state);
    CHECK(ref32(bytes + 16) == in.mode && ref32(bytes + 20) == in.interface_index);
    CHECK(ref32(bytes + 24) == in.mtu && ref32(bytes + 28) == in.packet_limit);
    CHECK(ref32(bytes + 32) == in.queue_capacity && ref32(bytes + 36) == in.queued_packets);
    CHECK(ref32(bytes + 40) == in.packet_filter && ref32(bytes + 44) == 0);
    CHECK(memcmp(bytes + 48, in.mac, 6) == 0 && bytes[54] == 0 && bytes[55] == 0);
    CHECK(ref64(bytes + 56) == in.luid && ref64(bytes + 64) == 0);
    CHECK(ref64(bytes + 72) == in.tx_packets && ref64(bytes + 80) == in.tx_bytes);
    CHECK(ref64(bytes + 88) == in.tx_errors && ref64(bytes + 96) == in.rx_packets);
    CHECK(ref64(bytes + 104) == in.rx_bytes && ref64(bytes + 112) == in.rx_discards);
    CHECK(ref64(bytes + 120) == in.tx_queue_drops);
    CHECK(bytes[0] == 3 && bytes[1] == 0 && bytes[56] == 0x08 && bytes[63] == 0x01);

    /* Roundtrip. */
    CHECK(chmlink_query_decode(&out, bytes, CHMLINK_INFO_SIZE) == CHMLINK_OK);
    CHECK(same_info(&in, &out));

    /* IP roundtrip, and an IP response may not carry a MAC. */
    in.mode = CHMLINK_MODE_IP; in.mtu = in.packet_limit = 1500;
    in.capabilities = (in.capabilities & ~CHMLINK_CAP_ETHERNET) | CHMLINK_CAP_IP;
    memset(in.mac, 0, sizeof(in.mac));
    chmlink_query_encode(bytes, &in);
    CHECK(chmlink_query_decode(&out, bytes, CHMLINK_INFO_SIZE) == CHMLINK_OK);
    CHECK(same_info(&in, &out));
    bytes[53] = 1;
    CHECK(chmlink_query_decode(&out, bytes, CHMLINK_INFO_SIZE) == CHMLINK_INVALID_QUERY);

    /* Failures leave the output untouched and classify version problems. */
    memset(&sentinel, 0x5a, sizeof(sentinel)); out = sentinel;
    chmlink_query_encode(bytes, &in);
    CHECK(chmlink_query_decode(&out, bytes, CHMLINK_INFO_SIZE - 1) == CHMLINK_UNSUPPORTED_VERSION);
    CHECK(chmlink_query_decode(&out, bytes, CHMLINK_INFO_SIZE + 1) == CHMLINK_UNSUPPORTED_VERSION);
    put32(bytes, 2);
    CHECK(chmlink_query_decode(&out, bytes, CHMLINK_INFO_SIZE) == CHMLINK_UNSUPPORTED_VERSION);
    put32(bytes, CHMLINK_DEVICE_VERSION); put32(bytes + 4, 127);
    CHECK(chmlink_query_decode(&out, bytes, CHMLINK_INFO_SIZE) == CHMLINK_UNSUPPORTED_VERSION);
    put32(bytes + 4, 128); put32(bytes + 16, 3);
    CHECK(chmlink_query_decode(&out, bytes, CHMLINK_INFO_SIZE) == CHMLINK_INVALID_QUERY);
    CHECK(same_info(&out, &sentinel));
    CHECK(chmlink_query_decode(NULL, bytes, CHMLINK_INFO_SIZE) == CHMLINK_INVALID_ARGUMENT);
    CHECK(chmlink_query_decode(&out, NULL, CHMLINK_INFO_SIZE) == CHMLINK_INVALID_ARGUMENT);

    /* Differential check against the pre-refactor validation on mutations of
     * valid Ethernet and IP responses, focusing on validated fields. */
    {
        static const unsigned int fields[] = {0, 4, 8, 16, 24, 28, 32, 36, 48, 52};
        static const uint32_t values[] = {0, 1, 2, 3, 13, 14, 15, 255, 256, 257, 1279,
            1280, 1281, 1500, 1508, 1522, 1523, 65557, 65558, 0xffffffffu, 0x1fu, 0x12u, 0x11u};
        unsigned long accepted = 0, rejected = 0;
        for (n = 0; n < 400000; ++n) {
            chmlink_query_info base = ethernet_info();
            int expected, mutations = 1 + (int)(next_random() % 3);
            if (next_random() & 1) {
                base.mode = CHMLINK_MODE_IP; base.mtu = base.packet_limit = 1500;
                base.capabilities = (base.capabilities & ~CHMLINK_CAP_ETHERNET) | CHMLINK_CAP_IP;
                memset(base.mac, 0, sizeof(base.mac));
            }
            chmlink_query_encode(copy, &base);
            while (mutations--) {
                unsigned int f = fields[next_random() % (sizeof(fields) / sizeof(fields[0]))];
                uint32_t v = (next_random() & 3) ? values[next_random() % (sizeof(values) / sizeof(values[0]))]
                                                 : next_random();
                if (f == 48 || f == 52) copy[f + (next_random() % 2)] = (uint8_t)v;
                else put32(copy + f, v);
            }
            len = (next_random() % 64) == 0 ? (size_t)(next_random() % 140) : CHMLINK_INFO_SIZE;
            expected = reference_check(copy, len);
            switch (chmlink_query_decode(&out, copy, len)) {
            case CHMLINK_OK: CHECK(expected == 0); accepted++; break;
            case CHMLINK_UNSUPPORTED_VERSION: CHECK(expected == 1); rejected++; break;
            case CHMLINK_INVALID_QUERY: CHECK(expected == 2); rejected++; break;
            default: CHECK(0);
            }
        }
        CHECK(accepted > 1000 && rejected > 1000);
    }
    return 0;
}
