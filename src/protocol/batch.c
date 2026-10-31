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

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void read_offload(const uint8_t *p, chmlink_offload *o)
{
    o->flags = p[0];
    o->gso_type = p[1];
    o->hdr_len = read_u16(p + 2);
    o->gso_size = read_u16(p + 4);
    o->csum_start = read_u16(p + 6);
    o->csum_offset = read_u16(p + 8);
}

static void write_offload(uint8_t *p, const chmlink_offload *o)
{
    p[0] = o->flags;
    p[1] = o->gso_type;
    write_u16(p + 2, o->hdr_len);
    write_u16(p + 4, o->gso_size);
    write_u16(p + 6, o->csum_start);
    write_u16(p + 8, o->csum_offset);
    write_u16(p + 10, 0);
}

static int offload_empty(const chmlink_offload *o)
{
    return o == NULL || (o->flags == 0 && o->gso_type == 0 && o->hdr_len == 0 &&
        o->gso_size == 0 && o->csum_start == 0 && o->csum_offset == 0);
}

int chmlink_offload_valid(chmlink_mode mode, const uint8_t *packet, size_t length,
    size_t packet_limit, const chmlink_offload *o)
{
    size_t l3 = mode == CHMLINK_MODE_IP ? 0 : 14, l4, tcp_header;
    unsigned type;
    if (packet == NULL || o == NULL) return 0;
    if (o->flags & ~(unsigned)(CHMLINK_OFFLOAD_NEEDS_CSUM | CHMLINK_OFFLOAD_DATA_VALID)) return 0;
    if ((o->flags & CHMLINK_OFFLOAD_NEEDS_CSUM) &&
        ((size_t)o->csum_start + o->csum_offset + 2 > length)) return 0;
    type = o->gso_type & ~(unsigned)CHMLINK_GSO_ECN;
    if (type == CHMLINK_GSO_NONE) {
        /* ECN without a type, or segmentation fields without one, are malformed. */
        return o->gso_type == 0 && o->gso_size == 0 && o->hdr_len == 0 && length <= packet_limit;
    }
    if ((type != CHMLINK_GSO_TCPV4 && type != CHMLINK_GSO_TCPV6) || o->gso_size == 0 ||
        length > CHMLINK_PACKET_MAX_SIZE || length < l3 + 1) return 0;
    if (mode == CHMLINK_MODE_ETHERNET) {
        unsigned ethertype = ((unsigned)packet[12] << 8) | packet[13];
        if (ethertype != (type == CHMLINK_GSO_TCPV4 ? 0x0800u : 0x86ddu)) return 0;
    }
    if (type == CHMLINK_GSO_TCPV4) {
        if ((packet[l3] >> 4) != 4 || (packet[l3] & 15u) < 5 || length < l3 + 20 || packet[l3 + 9] != 6) return 0;
        l4 = l3 + (size_t)(packet[l3] & 15u) * 4u;
        if (((packet[l3 + 6] & 0x3f) | packet[l3 + 7]) != 0) return 0;   /* no fragments */
    } else {
        if ((packet[l3] >> 4) != 6 || length < l3 + 40 || packet[l3 + 6] != 6) return 0;  /* no extension headers */
        l4 = l3 + 40;
    }
    if (length < l4 + 20) return 0;
    tcp_header = (size_t)(packet[l4 + 12] >> 4) * 4u;
    if (tcp_header < 20 || o->csum_start != l4 || o->csum_offset != 16 ||
        o->hdr_len != l4 + tcp_header || o->hdr_len >= length) return 0;
    return 1;
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static chmlink_result begin(chmlink_batch_writer *writer,
    void *buffer, size_t capacity, chmlink_mode mode, size_t packet_limit, uint32_t flags)
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
    next.flags = flags;
    next.mode = mode;
    write_u32(bytes, CHMLINK_PROTOCOL_VERSION);
    write_u32(bytes + 4, (uint32_t)mode);
    write_u32(bytes + 8, 0);
    write_u32(bytes + 12, CHMLINK_BATCH_HEADER_SIZE);
    write_u32(bytes + 16, flags);
    write_u32(bytes + 20, 0);
    *writer = next;
    return CHMLINK_OK;
}

chmlink_result chmlink_batch_begin(chmlink_batch_writer *writer,
    void *buffer, size_t capacity, chmlink_mode mode, size_t packet_limit)
{
    return begin(writer, buffer, capacity, mode, packet_limit, 0);
}

chmlink_result chmlink_batch_begin_offload(chmlink_batch_writer *writer,
    void *buffer, size_t capacity, chmlink_mode mode, size_t packet_limit)
{
    return begin(writer, buffer, capacity, mode, packet_limit, CHMLINK_BATCH_FLAG_OFFLOAD);
}

chmlink_result chmlink_batch_append(chmlink_batch_writer *writer,
    const uint8_t *packet, size_t length)
{
    return chmlink_batch_append_offload(writer, packet, length, NULL);
}

chmlink_result chmlink_batch_append_offload(chmlink_batch_writer *writer,
    const uint8_t *packet, size_t length, const chmlink_offload *offload)
{
    static const chmlink_offload none = {0, 0, 0, 0, 0, 0};
    size_t padding, record_size, descriptor;
    uint8_t *record;

    if (writer == NULL || writer->buffer == NULL ||
        !valid_mode(writer->mode) || !valid_limit(writer->packet_limit) ||
        writer->capacity > CHMLINK_BATCH_MAX_SIZE ||
        writer->length < CHMLINK_BATCH_HEADER_SIZE ||
        writer->length > writer->capacity ||
        (writer->flags & ~CHMLINK_BATCH_FLAG_OFFLOAD) != 0) {
        return CHMLINK_INVALID_ARGUMENT;
    }
    if (offload == NULL) offload = &none;
    descriptor = (writer->flags & CHMLINK_BATCH_FLAG_OFFLOAD) ? CHMLINK_OFFLOAD_SIZE : 0;
    if (descriptor == 0 && !offload_empty(offload)) return CHMLINK_INVALID_ARGUMENT;
    if (packet == NULL || length == 0 ||
        !chmlink_packet_valid(writer->mode, packet, length,
            offload_empty(offload) ? writer->packet_limit : CHMLINK_PACKET_MAX_SIZE) ||
        (descriptor != 0 && !chmlink_offload_valid(writer->mode, packet, length,
            writer->packet_limit, offload))) {
        return CHMLINK_INVALID_PACKET;
    }
    padding = (4 - (length & 3)) & 3;
    record_size = 4 + descriptor + length + padding;
    if (writer->count >= CHMLINK_BATCH_MAX_PACKETS ||
        record_size > writer->capacity - writer->length) {
        return CHMLINK_BUFFER_TOO_SMALL;
    }
    record = writer->buffer + writer->length;
    memmove(record + 4 + descriptor, packet, length);
    memset(record + 4 + descriptor + length, 0, padding);
    if (descriptor != 0) write_offload(record + 4, offload);
    write_u32(record, (uint32_t)length);
    writer->length += record_size;
    writer->count++;
    write_u32(writer->buffer + 8, writer->count);
    write_u32(writer->buffer + 12, (uint32_t)writer->length);
    return CHMLINK_OK;
}

void chmlink_batch_read_begin(chmlink_batch_reader *reader, const void *buffer)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    reader->buffer = bytes;
    reader->offset = CHMLINK_BATCH_HEADER_SIZE;
    reader->remaining = read_u32(bytes + 8);
    reader->flags = read_u32(bytes + 16);
}

int chmlink_batch_read_next(chmlink_batch_reader *reader, const uint8_t **packet,
    uint32_t *length, chmlink_offload *offload)
{
    uint32_t size;
    if (reader->remaining == 0) return 0;
    size = read_u32(reader->buffer + reader->offset);
    reader->offset += 4;
    if (reader->flags & CHMLINK_BATCH_FLAG_OFFLOAD) {
        if (offload != NULL) read_offload(reader->buffer + reader->offset, offload);
        reader->offset += CHMLINK_OFFLOAD_SIZE;
    } else if (offload != NULL) {
        memset(offload, 0, sizeof(*offload));
    }
    *packet = reader->buffer + reader->offset;
    *length = size;
    reader->offset += (size_t)size + ((4 - (size & 3)) & 3);
    reader->remaining--;
    return 1;
}

chmlink_result chmlink_batch_validate(const void *buffer, size_t length,
    chmlink_mode expected_mode, size_t packet_limit, uint32_t *packet_count)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    uint32_t count, i, flags;
    size_t offset = CHMLINK_BATCH_HEADER_SIZE, descriptor;

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
    flags = read_u32(bytes + 16);
    descriptor = (flags & CHMLINK_BATCH_FLAG_OFFLOAD) ? CHMLINK_OFFLOAD_SIZE : 0;
    if (count == 0 || count > CHMLINK_BATCH_MAX_PACKETS ||
        read_u32(bytes + 12) != length || (flags & ~CHMLINK_BATCH_FLAG_OFFLOAD) != 0 ||
        read_u32(bytes + 20) != 0) {
        return CHMLINK_INVALID_BATCH;
    }
    for (i = 0; i < count; ++i) {
        size_t packet_length, padding, j;
        chmlink_offload offload = {0, 0, 0, 0, 0, 0};
        if (length - offset < 4 + descriptor) {
            return CHMLINK_INVALID_BATCH;
        }
        packet_length = read_u32(bytes + offset);
        offset += 4;
        if (descriptor != 0) {
            if (read_u16(bytes + offset + 10) != 0) return CHMLINK_INVALID_BATCH;
            read_offload(bytes + offset, &offload);
            offset += descriptor;
        }
        padding = (4 - (packet_length & 3)) & 3;
        if (packet_length > length - offset ||
            padding > length - offset - packet_length) {
            return CHMLINK_INVALID_BATCH;
        }
        if (!chmlink_packet_valid(expected_mode, bytes + offset, packet_length,
                offload.gso_type ? CHMLINK_PACKET_MAX_SIZE : packet_limit) ||
            (descriptor != 0 && !chmlink_offload_valid(expected_mode, bytes + offset,
                packet_length, packet_limit, &offload))) {
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
