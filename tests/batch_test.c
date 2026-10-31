#include "chmlink/protocol.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int round_trip(void)
{
    uint8_t storage[256], packet[64] = {0}, saved[256];
    chmlink_batch_writer writer;
    uint32_t count;
    size_t length;

    /* Unaligned storage must work on every target architecture. */
    CHECK(chmlink_batch_begin(&writer, storage + 1, sizeof(storage) - 1,
                             CHMLINK_MODE_ETHERNET, 64) == CHMLINK_OK);
    CHECK(chmlink_batch_validate(writer.buffer, writer.length,
        CHMLINK_MODE_ETHERNET, 64, &count) == CHMLINK_INVALID_BATCH);
    packet[12] = 0x88; /* Preserve a non-IP EtherType and arbitrary payload. */
    packet[13] = 0xb5;
    CHECK(chmlink_batch_append(&writer, packet, 15) == CHMLINK_OK);
    CHECK(writer.length == 44);
    CHECK(writer.buffer[24] == 15 && writer.buffer[25] == 0);
    CHECK(memcmp(writer.buffer + 28, packet, 15) == 0);
    CHECK(writer.buffer[43] == 0);
    CHECK(chmlink_batch_append(&writer, packet, 64) == CHMLINK_OK);
    CHECK(chmlink_batch_validate(writer.buffer, writer.length,
        CHMLINK_MODE_ETHERNET, 64, &count) == CHMLINK_OK && count == 2);
    CHECK(chmlink_batch_validate(writer.buffer, writer.length,
        CHMLINK_MODE_IP, 64, &count) == CHMLINK_MODE_MISMATCH && count == 0);

    memcpy(saved, storage, sizeof(storage));
    length = writer.length;
    CHECK(chmlink_batch_append(&writer, packet, 65) == CHMLINK_INVALID_PACKET);
    CHECK(writer.length == length && writer.count == 2);
    CHECK(memcmp(saved, storage, sizeof(storage)) == 0);
    /* An overlapping source remains intact until copied. */
    CHECK(chmlink_batch_append(&writer, writer.buffer + 28, 15) == CHMLINK_OK);
    CHECK(memcmp(writer.buffer + length + 4, packet, 15) == 0);
    return 0;
}

static int malformed_batches(void)
{
    uint8_t storage[128], copy[128], packet[40] = {0x60};
    chmlink_batch_writer writer;
    uint32_t count;
    size_t i;

    CHECK(chmlink_batch_begin(&writer, storage, sizeof(storage),
                             CHMLINK_MODE_IP, 40) == CHMLINK_OK);
    CHECK(chmlink_batch_append(&writer, packet, sizeof(packet)) == CHMLINK_OK);
    CHECK(chmlink_batch_validate(storage, writer.length,
        CHMLINK_MODE_IP, 40, NULL) == CHMLINK_OK);
    /* Every truncated prefix must fail, including cuts in the header. */
    for (i = 0; i < writer.length; ++i) {
        count = 99;
        CHECK(chmlink_batch_validate(storage, i, CHMLINK_MODE_IP, 40,
                                     &count) != CHMLINK_OK && count == 0);
    }
    /* Every byte of each structural field is significant. */
    for (i = 0; i < 28; ++i) {
        memcpy(copy, storage, writer.length);
        copy[i] ^= 0x80;
        CHECK(chmlink_batch_validate(copy, writer.length, CHMLINK_MODE_IP,
                                     40, NULL) != CHMLINK_OK);
    }
    memcpy(copy, storage, writer.length);
    memset(copy + 24, 0xff, 4); /* Length near UINT32_MAX. */
    CHECK(chmlink_batch_validate(copy, writer.length, CHMLINK_MODE_IP,
                                 40, NULL) == CHMLINK_INVALID_BATCH);
    memcpy(copy, storage, writer.length);
    copy[writer.length] = 0;
    copy[12]++; /* Trailing data with an otherwise consistent total size. */
    CHECK(chmlink_batch_validate(copy, writer.length + 1, CHMLINK_MODE_IP,
                                 40, NULL) == CHMLINK_INVALID_BATCH);
    CHECK(chmlink_batch_validate(storage, writer.length, CHMLINK_MODE_IP,
                                 39, NULL) == CHMLINK_INVALID_PACKET);
    /* A bad second packet invalidates the whole batch. */
    CHECK(chmlink_batch_append(&writer, packet, sizeof(packet)) == CHMLINK_OK);
    storage[72] = 0x70;
    count = 99;
    CHECK(chmlink_batch_validate(storage, writer.length, CHMLINK_MODE_IP,
                                 40, &count) == CHMLINK_INVALID_PACKET);
    CHECK(count == 0);
    return 0;
}

static int limits(void)
{
    static uint8_t storage[CHMLINK_BATCH_MAX_SIZE + 1];
    static uint8_t packet[CHMLINK_PACKET_MAX_SIZE];
    chmlink_batch_writer writer;
    uint32_t i, count;
    size_t length;

    CHECK(chmlink_batch_begin(NULL, storage, sizeof(storage),
                             CHMLINK_MODE_IP, 64) == CHMLINK_INVALID_ARGUMENT);
    CHECK(chmlink_batch_begin(&writer, storage, sizeof(storage),
        CHMLINK_MODE_IP, 0) == CHMLINK_INVALID_ARGUMENT);
    CHECK(chmlink_batch_begin(&writer, storage, sizeof(storage),
        (chmlink_mode)99, 64) == CHMLINK_INVALID_ARGUMENT);
    CHECK(chmlink_batch_begin(&writer, storage, 23,
        CHMLINK_MODE_IP, 64) == CHMLINK_BUFFER_TOO_SMALL);
    CHECK(chmlink_batch_begin(&writer, storage, 43,
        CHMLINK_MODE_ETHERNET, 14) == CHMLINK_OK);
    CHECK(chmlink_batch_append(&writer, packet, 14) == CHMLINK_BUFFER_TOO_SMALL);
    CHECK(writer.length == 24 && writer.count == 0);
    CHECK(chmlink_batch_begin(&writer, storage, 44,
        CHMLINK_MODE_ETHERNET, 14) == CHMLINK_OK);
    CHECK(chmlink_batch_append(&writer, packet, 14) == CHMLINK_OK);
    storage[42] = 1; /* Nonzero record padding is rejected. */
    CHECK(chmlink_batch_validate(storage, 44, CHMLINK_MODE_ETHERNET,
                                 14, NULL) == CHMLINK_INVALID_BATCH);

    CHECK(chmlink_batch_begin(&writer, storage, sizeof(storage),
        CHMLINK_MODE_ETHERNET, 14) == CHMLINK_OK);
    for (i = 0; i < CHMLINK_BATCH_MAX_PACKETS; ++i) {
        CHECK(chmlink_batch_append(&writer, packet, 14) == CHMLINK_OK);
    }
    CHECK(chmlink_batch_append(&writer, packet, 14) == CHMLINK_BUFFER_TOO_SMALL);
    CHECK(chmlink_batch_validate(storage, writer.length, CHMLINK_MODE_ETHERNET,
        14, &count) == CHMLINK_OK && count == CHMLINK_BATCH_MAX_PACKETS);

    CHECK(chmlink_batch_begin(&writer, storage, sizeof(storage),
        CHMLINK_MODE_ETHERNET, sizeof(packet)) == CHMLINK_OK);
    CHECK(writer.capacity == CHMLINK_BATCH_MAX_SIZE);
    while (chmlink_batch_append(&writer, packet, sizeof(packet)) == CHMLINK_OK) {
        /* Fill the byte budget before reaching the packet-count limit. */
    }
    length = writer.length;
    CHECK(writer.count > 0 && writer.count < CHMLINK_BATCH_MAX_PACKETS);
    CHECK(chmlink_batch_append(&writer, packet, sizeof(packet)) ==
          CHMLINK_BUFFER_TOO_SMALL && writer.length == length);
    CHECK(chmlink_batch_validate(storage, length, CHMLINK_MODE_ETHERNET,
                                 sizeof(packet), NULL) == CHMLINK_OK);
    CHECK(chmlink_batch_validate(storage, sizeof(storage), CHMLINK_MODE_ETHERNET,
                                 sizeof(packet), NULL) == CHMLINK_INVALID_BATCH);
    return 0;
}

/* An IPv4 (or IPv6) TCP packet of total length `length` with a 20-byte TCP header. */
static size_t tcp_packet(uint8_t *p, size_t length, int v6)
{
    memset(p, 0, length);
    if (v6) {
        p[0] = 0x60; p[4] = (uint8_t)((length - 40) >> 8); p[5] = (uint8_t)(length - 40); p[6] = 6; p[7] = 64;
        p[40 + 12] = 5 << 4;
        return 40;
    }
    p[0] = 0x45; p[2] = (uint8_t)(length >> 8); p[3] = (uint8_t)length; p[8] = 64; p[9] = 6;
    p[20 + 12] = 5 << 4;
    return 20;
}

static int offload_batches(void)
{
    static uint8_t storage[200000], packet[65535];
    chmlink_batch_writer writer;
    chmlink_batch_reader reader;
    chmlink_offload gso = {CHMLINK_OFFLOAD_NEEDS_CSUM, CHMLINK_GSO_TCPV4, 40, 1460, 20, 16}, got, bad;
    const uint8_t *data;
    uint32_t count, size, seen = 0;
    size_t l4;

    l4 = tcp_packet(packet, 30000, 0);
    CHECK(l4 == 20);
    CHECK(chmlink_batch_begin_offload(&writer, storage, sizeof(storage), CHMLINK_MODE_IP, 1500) == CHMLINK_OK);
    CHECK(chmlink_batch_append_offload(&writer, packet, 30000, &gso) == CHMLINK_OK);
    /* A GSO packet is not held to the MTU; a plain one is. */
    CHECK(chmlink_batch_append(&writer, packet, 30000) == CHMLINK_INVALID_PACKET);
    tcp_packet(packet, 1500, 0);
    CHECK(chmlink_batch_append(&writer, packet, 1500) == CHMLINK_OK);
    CHECK(writer.length == 24 + 4 + 12 + 30000 + 4 + 12 + 1500);
    CHECK(chmlink_batch_validate(storage, writer.length, CHMLINK_MODE_IP, 1500, &count) == CHMLINK_OK && count == 2);
    chmlink_batch_read_begin(&reader, storage);
    while (chmlink_batch_read_next(&reader, &data, &size, &got)) {
        if (seen == 0) CHECK(size == 30000 && got.gso_type == CHMLINK_GSO_TCPV4 && got.gso_size == 1460 &&
                             got.hdr_len == 40 && got.csum_start == 20 && got.csum_offset == 16 &&
                             got.flags == CHMLINK_OFFLOAD_NEEDS_CSUM && data == storage + 24 + 4 + 12);
        else CHECK(size == 1500 && got.gso_type == 0 && got.flags == 0);
        seen++;
    }
    CHECK(seen == 2);
    /* Reserved descriptor bytes must be zero. */
    storage[24 + 4 + 10] = 1;
    CHECK(chmlink_batch_validate(storage, writer.length, CHMLINK_MODE_IP, 1500, NULL) == CHMLINK_INVALID_BATCH);
    storage[24 + 4 + 10] = 0;
    /* A plain reader of an offload batch would misparse: the flag is required to be known. */
    storage[16] = 3;
    CHECK(chmlink_batch_validate(storage, writer.length, CHMLINK_MODE_IP, 1500, NULL) == CHMLINK_INVALID_BATCH);
    storage[16] = 1;

    /* Descriptor checks. */
    tcp_packet(packet, 30000, 0);
    bad = gso; bad.csum_start = 21; CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &bad));
    bad = gso; bad.csum_offset = 6; CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &bad));
    bad = gso; bad.hdr_len = 60; CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &bad));
    bad = gso; bad.gso_size = 0; CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &bad));
    bad = gso; bad.gso_type = CHMLINK_GSO_TCPV6; CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &bad));
    bad = gso; bad.gso_type = 3; CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &bad));
    bad = gso; bad.flags = 4; CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &bad));
    bad = gso; bad.gso_type |= CHMLINK_GSO_ECN; CHECK(chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &bad));
    packet[9] = 17;  /* UDP cannot carry TCP segmentation */
    CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &gso));
    packet[9] = 6; packet[6] = 0x20;  /* a fragment */
    CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 30000, 1500, &gso));
    packet[6] = 0;
    bad.flags = CHMLINK_OFFLOAD_NEEDS_CSUM; bad.gso_type = 0; bad.gso_size = 0; bad.hdr_len = 0;
    bad.csum_start = 1498; bad.csum_offset = 0;
    CHECK(chmlink_offload_valid(CHMLINK_MODE_IP, packet, 1500, 1500, &bad));
    bad.csum_start = 1499;
    CHECK(!chmlink_offload_valid(CHMLINK_MODE_IP, packet, 1500, 1500, &bad));
    /* IPv6, and Ethernet framing. */
    l4 = tcp_packet(packet, 9000, 1);
    gso.gso_type = CHMLINK_GSO_TCPV6; gso.csum_start = 40; gso.hdr_len = 60;
    CHECK(chmlink_offload_valid(CHMLINK_MODE_IP, packet, 9000, 1500, &gso));
    memmove(packet + 14, packet, 9000); memset(packet, 0, 14); packet[12] = 0x86; packet[13] = 0xdd;
    gso.csum_start = 54; gso.hdr_len = 74;
    CHECK(chmlink_offload_valid(CHMLINK_MODE_ETHERNET, packet, 9014, 1514, &gso));
    packet[13] = 0x00;
    CHECK(!chmlink_offload_valid(CHMLINK_MODE_ETHERNET, packet, 9014, 1514, &gso));

    /* Plain batches take no descriptors, and read back as zeroed. */
    CHECK(chmlink_batch_begin(&writer, storage, sizeof(storage), CHMLINK_MODE_IP, 1500) == CHMLINK_OK);
    tcp_packet(packet, 30000, 0); gso.gso_type = CHMLINK_GSO_TCPV4; gso.csum_start = 20; gso.hdr_len = 40;
    CHECK(chmlink_batch_append_offload(&writer, packet, 30000, &gso) == CHMLINK_INVALID_ARGUMENT);
    tcp_packet(packet, 100, 0);
    CHECK(chmlink_batch_append_offload(&writer, packet, 100, NULL) == CHMLINK_OK);
    CHECK(chmlink_batch_validate(storage, writer.length, CHMLINK_MODE_IP, 1500, NULL) == CHMLINK_OK);
    chmlink_batch_read_begin(&reader, storage);
    memset(&got, 0xff, sizeof(got));
    CHECK(chmlink_batch_read_next(&reader, &data, &size, &got) && size == 100 && got.gso_type == 0 && got.hdr_len == 0);
    CHECK(!chmlink_batch_read_next(&reader, &data, &size, &got));
    (void)l4;
    return 0;
}

int main(void)
{
    CHECK(offload_batches() == 0);
    CHECK(round_trip() == 0);
    CHECK(malformed_batches() == 0);
    CHECK(limits() == 0);
    return 0;
}
