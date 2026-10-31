#include "chmlink/offload.h"
#include <string.h>

uint16_t chmlink_checksum_add(const uint8_t *data, size_t length, uint16_t seed)
{
    uint64_t sum = seed;
    size_t i = 0;
    for (; i + 1 < length; i += 2) sum += ((uint64_t)data[i] << 8) | data[i + 1];
    if (i < length) sum += (uint64_t)data[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* Offset of the IP header, or SIZE_MAX for an unsupported frame. */
static size_t ip_offset(chmlink_mode mode, const uint8_t *packet, size_t length)
{
    if (mode == CHMLINK_MODE_IP) return length >= 20 ? 0 : (size_t)-1;
    if (mode != CHMLINK_MODE_ETHERNET || length < 14 + 20) return (size_t)-1;
    if ((packet[12] == 0x08 && packet[13] == 0x00) || (packet[12] == 0x86 && packet[13] == 0xdd)) return 14;
    return (size_t)-1;
}

/* Pseudo-header sum for an L4 segment of l4_length bytes. Returns 0 and
 * sets *protocol, or -1 for a packet that is not IPv4/IPv6. */
static int pseudo_sum(const uint8_t *ip, size_t available, size_t l4_length, uint16_t *sum, uint8_t *protocol)
{
    uint8_t length_bytes[4];
    unsigned version = ip[0] >> 4;
    if (version == 4) {
        uint8_t tail[2];
        if (available < 20) return -1;
        *protocol = ip[9];
        tail[0] = 0; tail[1] = ip[9];
        put16(length_bytes, (uint16_t)l4_length);
        *sum = chmlink_checksum_add(ip + 12, 8, 0);
        *sum = chmlink_checksum_add(tail, 2, *sum);
        *sum = chmlink_checksum_add(length_bytes, 2, *sum);
        return 0;
    }
    if (version == 6) {
        uint8_t tail[4] = {0, 0, 0, 0};
        if (available < 40) return -1;
        *protocol = ip[6];
        tail[3] = ip[6];
        put32(length_bytes, (uint32_t)l4_length);
        *sum = chmlink_checksum_add(ip + 8, 32, 0);
        *sum = chmlink_checksum_add(length_bytes, 4, *sum);
        *sum = chmlink_checksum_add(tail, 4, *sum);
        return 0;
    }
    return -1;
}

static chmlink_result fill_l4_checksum(uint8_t *packet, size_t length, size_t l3, size_t l4, size_t checksum_at)
{
    uint16_t sum, result;
    uint8_t protocol;
    if (checksum_at + 2 > length || l4 > checksum_at || l4 <= l3) return CHMLINK_INVALID_PACKET;
    if (pseudo_sum(packet + l3, length - l3, length - l4, &sum, &protocol) != 0) return CHMLINK_INVALID_PACKET;
    packet[checksum_at] = 0; packet[checksum_at + 1] = 0;
    result = (uint16_t)~chmlink_checksum_add(packet + l4, length - l4, sum);
    if (protocol == 17 && result == 0) result = 0xffff;   /* UDP: zero means "no checksum" */
    put16(packet + checksum_at, result);
    return CHMLINK_OK;
}

chmlink_result chmlink_offload_complete(chmlink_mode mode, uint8_t *packet, size_t length,
    const chmlink_offload *offload)
{
    size_t l3;
    if (packet == NULL || offload == NULL) return CHMLINK_INVALID_ARGUMENT;
    if (!(offload->flags & CHMLINK_OFFLOAD_NEEDS_CSUM)) return CHMLINK_OK;
    if (offload->gso_type != CHMLINK_GSO_NONE) return CHMLINK_INVALID_ARGUMENT;
    l3 = ip_offset(mode, packet, length);
    if (l3 == (size_t)-1) return CHMLINK_INVALID_PACKET;
    return fill_l4_checksum(packet, length, l3, offload->csum_start,
                            (size_t)offload->csum_start + offload->csum_offset);
}

chmlink_result chmlink_gso_segment(chmlink_mode mode, uint8_t *packet, size_t length,
    const chmlink_offload *offload, uint8_t *scratch, size_t scratch_size,
    chmlink_segment_callback callback, void *context)
{
    size_t l3, l4, header, payload, mss, done, index;
    uint32_t sequence;
    uint16_t identification = 0;
    int v4;
    chmlink_result result;
    if (packet == NULL || offload == NULL || callback == NULL) return CHMLINK_INVALID_ARGUMENT;
    if ((offload->gso_type & ~(unsigned)CHMLINK_GSO_ECN) == CHMLINK_GSO_NONE) {
        result = chmlink_offload_complete(mode, packet, length, offload);
        if (result != CHMLINK_OK) return result;
        callback(packet, length, context);
        return CHMLINK_OK;
    }
    if (!chmlink_offload_valid(mode, packet, length, CHMLINK_PACKET_MAX_SIZE, offload)) return CHMLINK_INVALID_PACKET;
    if (scratch == NULL || scratch_size < (size_t)offload->hdr_len + offload->gso_size) return CHMLINK_BUFFER_TOO_SMALL;
    l3 = mode == CHMLINK_MODE_IP ? 0 : 14;
    l4 = offload->csum_start;
    header = offload->hdr_len;
    payload = length - header;
    mss = offload->gso_size;
    v4 = (packet[l3] >> 4) == 4;
    if (v4) identification = get16(packet + l3 + 4);
    sequence = get32(packet + l4 + 4);
    for (done = 0, index = 0; done < payload; done += mss, ++index) {
        size_t chunk = payload - done < mss ? payload - done : mss, size = header + chunk;
        int first = done == 0, last = done + chunk == payload;
        memcpy(scratch, packet, header);
        memcpy(scratch + header, packet + header + done, chunk);
        if (v4) {
            put16(scratch + l3 + 2, (uint16_t)(size - l3));
            put16(scratch + l3 + 4, (uint16_t)((identification + index) & 0x7fff));
            scratch[l3 + 10] = 0; scratch[l3 + 11] = 0;
            put16(scratch + l3 + 10, (uint16_t)~chmlink_checksum_add(scratch + l3, l4 - l3, 0));
        } else {
            put16(scratch + l3 + 4, (uint16_t)(size - l3 - 40));
        }
        put32(scratch + l4 + 4, sequence + (uint32_t)done);
        if (!last) scratch[l4 + 13] = (uint8_t)(scratch[l4 + 13] & 0xf6u);    /* clear FIN, PSH */
        if (!first) scratch[l4 + 13] = (uint8_t)(scratch[l4 + 13] & 0x7fu);   /* clear CWR */
        result = fill_l4_checksum(scratch, size, l3, l4, l4 + 16);
        if (result != CHMLINK_OK) return result;
        if (!callback(scratch, size, context)) break;
    }
    return CHMLINK_OK;
}
