#include "chmlink/offload.h"
#include <string.h>

/* chmlink_ip_split: an IP packet larger than the tunnel's MTU, as an IP
 * adapter with a large MTU hands it over, cut into packets that fit.
 * TCP is re-segmented (chmlink_gso_segment does the work); anything else is
 * fragmented, IPv4 by RFC 791 and IPv6 with a Fragment header (RFC 8200). */

#define IPV4_DF 0x4000u
#define IPV4_MF 0x2000u
#define IPV4_OFFSET 0x1fffu

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void ipv4_header_checksum(uint8_t *ip, size_t header)
{
    ip[10] = 0; ip[11] = 0;
    put16(ip + 10, (uint16_t)~chmlink_checksum_add(ip, header, 0));
}

/* Re-segment TCP through chmlink_gso_segment, with a descriptor that asks for
 * segments of mtu bytes. Returns CHMLINK_INVALID_PACKET when the packet is not
 * plain TCP it can segment (an IPv4 fragment, IPv6 extension headers). */
static chmlink_result split_tcp(uint8_t *packet, size_t length, size_t mtu, int v4,
    uint8_t *scratch, size_t scratch_size, chmlink_segment_callback callback, void *context)
{
    chmlink_offload o;
    size_t l4, header;
    if (v4) {
        if (packet[9] != 6 || (get16(packet + 6) & (IPV4_MF | IPV4_OFFSET)) != 0) return CHMLINK_INVALID_PACKET;
        l4 = (size_t)(packet[0] & 15u) * 4u;
    } else {
        if (packet[6] != 6) return CHMLINK_INVALID_PACKET;
        l4 = 40;
    }
    if (length < l4 + 20) return CHMLINK_INVALID_PACKET;
    header = l4 + (size_t)(packet[l4 + 12] >> 4) * 4u;
    if (header < l4 + 20 || header >= length) return CHMLINK_INVALID_PACKET;
    if (header >= mtu) return CHMLINK_TOO_BIG;
    memset(&o, 0, sizeof(o));
    o.flags = CHMLINK_OFFLOAD_NEEDS_CSUM;
    o.gso_type = v4 ? CHMLINK_GSO_TCPV4 : CHMLINK_GSO_TCPV6;
    o.hdr_len = (uint16_t)header;
    o.gso_size = (uint16_t)(mtu - header);
    o.csum_start = (uint16_t)l4;
    o.csum_offset = 16;
    if (!chmlink_offload_valid(CHMLINK_MODE_IP, packet, length, CHMLINK_PACKET_MAX_SIZE, &o))
        return CHMLINK_INVALID_PACKET;
    return chmlink_gso_segment(CHMLINK_MODE_IP, packet, length, &o, scratch, scratch_size, callback, context);
}

/* Options of the first header that later fragments carry: those with the
 * copy flag (RFC 791). Writes them padded to 4 bytes; returns their length,
 * or (size_t)-1 for malformed options. */
static size_t copied_options(const uint8_t *options, size_t length, uint8_t *out)
{
    size_t at = 0, written = 0;
    while (at < length) {
        uint8_t type = options[at];
        size_t size;
        if (type == 0) break;                         /* end of list */
        if (type == 1) { at++; continue; }            /* no-operation */
        if (at + 1 >= length || options[at + 1] < 2 || at + options[at + 1] > length) return (size_t)-1;
        size = options[at + 1];
        if (type & 0x80u) { memcpy(out + written, options + at, size); written += size; }
        at += size;
    }
    while (written % 4) out[written++] = 0;
    return written;
}

static chmlink_result fragment_ipv4(const uint8_t *packet, size_t length, size_t mtu,
    uint8_t *scratch, size_t scratch_size, chmlink_segment_callback callback, void *context)
{
    size_t header = (size_t)(packet[0] & 15u) * 4u, later_header, payload, done = 0;
    uint8_t later_options[40];
    uint16_t field = get16(packet + 6);
    size_t base_offset = (size_t)(field & IPV4_OFFSET) * 8u;
    int more = (field & IPV4_MF) != 0;
    size_t options = copied_options(packet + 20, header - 20, later_options);
    if (options == (size_t)-1) return CHMLINK_INVALID_PACKET;
    if (field & IPV4_DF) return CHMLINK_TOO_BIG;
    later_header = 20 + options;
    if (mtu < header + 8 || scratch_size < mtu) return mtu < header + 8 ? CHMLINK_INVALID_ARGUMENT : CHMLINK_BUFFER_TOO_SMALL;
    payload = length - header;
    while (done < payload) {
        int first = done == 0;
        size_t hl = first ? header : later_header;
        size_t room = (mtu - hl) & ~(size_t)7, chunk = payload - done;
        int last = chunk <= room;
        uint16_t flags;
        if (!last) chunk = room;
        if (first) memcpy(scratch, packet, header);
        else {
            memcpy(scratch, packet, 20);
            memcpy(scratch + 20, later_options, options);
            scratch[0] = (uint8_t)(0x40u | (hl / 4));
        }
        memcpy(scratch + hl, packet + header + done, chunk);
        put16(scratch + 2, (uint16_t)(hl + chunk));
        flags = (uint16_t)((field & IPV4_DF) | ((!last || more) ? IPV4_MF : 0) |
                           (((base_offset + done) / 8) & IPV4_OFFSET));
        put16(scratch + 6, flags);
        ipv4_header_checksum(scratch, hl);
        done += chunk;
        if (!callback(scratch, hl + chunk, context)) break;
    }
    return CHMLINK_OK;
}

static chmlink_result fragment_ipv6(const uint8_t *packet, size_t length, size_t mtu, uint32_t id,
    uint8_t *scratch, size_t scratch_size, chmlink_segment_callback callback, void *context)
{
    size_t unfragmentable = 40, previous = 6, payload, done = 0;
    uint8_t next = packet[6];
    /* The unfragmentable part: hop-by-hop options, a routing header, and
     * destination options that precede a routing header. */
    for (;;) {
        size_t size;
        if (next == 44) return CHMLINK_INVALID_PACKET;     /* already a fragment */
        if (next != 0 && next != 43 && next != 60) break;
        if (unfragmentable + 8 > length) return CHMLINK_INVALID_PACKET;
        size = ((size_t)packet[unfragmentable + 1] + 1) * 8u;
        if (unfragmentable + size > length) return CHMLINK_INVALID_PACKET;
        if (next == 60 && (unfragmentable + size + 2 > length || packet[unfragmentable] != 43)) break;
        previous = unfragmentable;                          /* its next-header field is byte 0 */
        next = packet[unfragmentable];
        unfragmentable += size;
    }
    if (mtu < unfragmentable + 8 + 8) return CHMLINK_INVALID_ARGUMENT;
    if (scratch_size < mtu) return CHMLINK_BUFFER_TOO_SMALL;
    payload = length - unfragmentable;
    while (done < payload) {
        size_t room = (mtu - unfragmentable - 8) & ~(size_t)7, chunk = payload - done;
        int last = chunk <= room;
        uint8_t *fragment = scratch + unfragmentable;
        if (!last) chunk = room;
        memcpy(scratch, packet, unfragmentable);
        scratch[previous] = 44;
        fragment[0] = next;
        fragment[1] = 0;
        put16(fragment + 2, (uint16_t)((done & ~(size_t)7) | (last ? 0u : 1u)));
        put32(fragment + 4, id);
        memcpy(fragment + 8, packet + unfragmentable + done, chunk);
        put16(scratch + 4, (uint16_t)(unfragmentable - 40 + 8 + chunk));
        done += chunk;
        if (!callback(scratch, unfragmentable + 8 + chunk, context)) break;
    }
    return CHMLINK_OK;
}

chmlink_result chmlink_ip_split(uint8_t *packet, size_t length, size_t mtu, uint32_t ipv6_id,
    uint8_t *scratch, size_t scratch_size, chmlink_segment_callback callback, void *context)
{
    chmlink_result result;
    int v4;
    if (packet == NULL || callback == NULL || length < 20) return CHMLINK_INVALID_ARGUMENT;
    v4 = (packet[0] >> 4) == 4;
    if (!v4 && (packet[0] >> 4) != 6) return CHMLINK_INVALID_PACKET;
    if (mtu < (v4 ? 576u : 1280u) || mtu > CHMLINK_PACKET_MAX_SIZE) return CHMLINK_INVALID_ARGUMENT;
    if (v4) {
        size_t header = (size_t)(packet[0] & 15u) * 4u;
        if (header < 20 || header > length || get16(packet + 2) != length) return CHMLINK_INVALID_PACKET;
    } else if (length < 40 || (size_t)get16(packet + 4) + 40 != length) {
        return CHMLINK_INVALID_PACKET;
    }
    if (length <= mtu) {
        callback(packet, length, context);
        return CHMLINK_OK;
    }
    if (scratch == NULL) return CHMLINK_INVALID_ARGUMENT;
    if (scratch_size < mtu) return CHMLINK_BUFFER_TOO_SMALL;
    result = split_tcp(packet, length, mtu, v4, scratch, scratch_size, callback, context);
    if (result != CHMLINK_INVALID_PACKET) return result;
    /* Not segmentable TCP (or not TCP): fragment. */
    return v4 ? fragment_ipv4(packet, length, mtu, scratch, scratch_size, callback, context)
              : fragment_ipv6(packet, length, mtu, ipv6_id, scratch, scratch_size, callback, context);
}
