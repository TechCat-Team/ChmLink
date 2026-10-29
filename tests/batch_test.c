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

int main(void)
{
    CHECK(round_trip() == 0);
    CHECK(malformed_batches() == 0);
    CHECK(limits() == 0);
    return 0;
}
