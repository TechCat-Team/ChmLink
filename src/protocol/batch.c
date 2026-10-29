#include "chmlink/protocol.h"
#include <string.h>

static int valid_mode(chmlink_mode mode)
{
    return mode == CHMLINK_MODE_ETHERNET || mode == CHMLINK_MODE_IP;
}

static int valid_limit(size_t limit)
{
    return limit != 0 && limit <= CHMLINK_PACKET_MAX_SIZE;
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

chmlink_result chmlink_batch_begin(chmlink_batch_writer *writer,
    void *buffer, size_t capacity, chmlink_mode mode, size_t packet_limit)
{
    chmlink_batch_writer next;
    uint8_t *bytes = (uint8_t *)buffer;

    if (writer == NULL || buffer == NULL || !valid_mode(mode) ||
        !valid_limit(packet_limit)) {
        return CHMLINK_INVALID_ARGUMENT;
    }
    if (capacity < CHMLINK_BATCH_HEADER_SIZE) {
        return CHMLINK_BUFFER_TOO_SMALL;
    }
    next.buffer = bytes;
    next.capacity = capacity < CHMLINK_BATCH_MAX_SIZE ?
                    capacity : CHMLINK_BATCH_MAX_SIZE;
    next.length = CHMLINK_BATCH_HEADER_SIZE;
    next.packet_limit = packet_limit;
    next.count = 0;
    next.mode = mode;
    write_u32(bytes, CHMLINK_PROTOCOL_VERSION);
    write_u32(bytes + 4, (uint32_t)mode);
    write_u32(bytes + 8, 0);
    write_u32(bytes + 12, CHMLINK_BATCH_HEADER_SIZE);
    write_u32(bytes + 16, 0);
    write_u32(bytes + 20, 0);
    *writer = next;
    return CHMLINK_OK;
}

chmlink_result chmlink_batch_append(chmlink_batch_writer *writer,
    const uint8_t *packet, size_t length)
{
    size_t padding, record_size;
    uint8_t *record;

    if (writer == NULL || writer->buffer == NULL ||
        !valid_mode(writer->mode) || !valid_limit(writer->packet_limit) ||
        writer->capacity > CHMLINK_BATCH_MAX_SIZE ||
        writer->length < CHMLINK_BATCH_HEADER_SIZE ||
        writer->length > writer->capacity) {
        return CHMLINK_INVALID_ARGUMENT;
    }
    if (!chmlink_packet_valid(writer->mode, packet, length,
                              writer->packet_limit)) {
        return CHMLINK_INVALID_PACKET;
    }
    padding = (4 - (length & 3)) & 3;
    record_size = 4 + length + padding;
    if (writer->count >= CHMLINK_BATCH_MAX_PACKETS ||
        record_size > writer->capacity - writer->length) {
        return CHMLINK_BUFFER_TOO_SMALL;
    }
    record = writer->buffer + writer->length;
    memmove(record + 4, packet, length);
    memset(record + 4 + length, 0, padding);
    write_u32(record, (uint32_t)length);
    writer->length += record_size;
    writer->count++;
    write_u32(writer->buffer + 8, writer->count);
    write_u32(writer->buffer + 12, (uint32_t)writer->length);
    return CHMLINK_OK;
}

chmlink_result chmlink_batch_validate(const void *buffer, size_t length,
    chmlink_mode expected_mode, size_t packet_limit, uint32_t *packet_count)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    uint32_t count, i;
    size_t offset = CHMLINK_BATCH_HEADER_SIZE;

    if (packet_count != NULL) {
        *packet_count = 0;
    }
    if (buffer == NULL || !valid_mode(expected_mode) ||
        !valid_limit(packet_limit)) {
        return CHMLINK_INVALID_ARGUMENT;
    }
    if (length < CHMLINK_BATCH_HEADER_SIZE || length > CHMLINK_BATCH_MAX_SIZE) {
        return CHMLINK_INVALID_BATCH;
    }
    if (read_u32(bytes) != CHMLINK_PROTOCOL_VERSION) {
        return CHMLINK_UNSUPPORTED_VERSION;
    }
    if (read_u32(bytes + 4) != (uint32_t)expected_mode) {
        return CHMLINK_MODE_MISMATCH;
    }
    count = read_u32(bytes + 8);
    if (count == 0 || count > CHMLINK_BATCH_MAX_PACKETS ||
        read_u32(bytes + 12) != length || read_u32(bytes + 16) != 0 ||
        read_u32(bytes + 20) != 0) {
        return CHMLINK_INVALID_BATCH;
    }
    for (i = 0; i < count; ++i) {
        size_t packet_length, padding, j;
        if (length - offset < 4) {
            return CHMLINK_INVALID_BATCH;
        }
        packet_length = read_u32(bytes + offset);
        offset += 4;
        padding = (4 - (packet_length & 3)) & 3;
        if (packet_length > length - offset ||
            padding > length - offset - packet_length) {
            return CHMLINK_INVALID_BATCH;
        }
        if (!chmlink_packet_valid(expected_mode, bytes + offset,
                                  packet_length, packet_limit)) {
            return CHMLINK_INVALID_PACKET;
        }
        offset += packet_length;
        for (j = 0; j < padding; ++j) {
            if (bytes[offset + j] != 0) {
                return CHMLINK_INVALID_BATCH;
            }
        }
        offset += padding;
    }
    if (offset != length) {
        return CHMLINK_INVALID_BATCH;
    }
    if (packet_count != NULL) {
        *packet_count = count;
    }
    return CHMLINK_OK;
}
