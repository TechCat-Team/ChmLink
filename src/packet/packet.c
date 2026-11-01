#include "chmlink/packet.h"

int chmlink_ip_packet_valid(const uint8_t *data, size_t length, size_t max_length)
{
    size_t header, declared;
    unsigned int version;
    if (data == NULL || length == 0 || length > max_length) return 0;
    version = data[0] >> 4;
    if (version == 4) {
        if (length < 20) return 0;
        header = (size_t)(data[0] & 15u) * 4u;
        declared = ((size_t)data[2] << 8) | data[3];
        return header >= 20 && header <= length && declared >= header && declared == length;
    }
    if (version == 6) {
        if (length < 40) return 0;
        declared = ((size_t)data[4] << 8) | data[5];
        /* A zero payload is valid for a 40-byte packet; a longer packet with
         * zero payload length would require unsupported jumbo-option parsing. */
        return declared == length - 40;
    }
    return 0;
}

int chmlink_packet_valid(chmlink_mode mode, const uint8_t *data,
                         size_t length, size_t max_length)
{
    if (data == NULL || length == 0 || length > max_length) {
        return 0;
    }

    if (mode == CHMLINK_MODE_ETHERNET) {
        return length >= 14;
    }
    if (mode != CHMLINK_MODE_IP) {
        return 0;
    }

    return chmlink_ip_packet_valid(data, length, max_length);
}

/* Walk at most a few IPv6 extension headers (Hop-by-Hop, Routing,
 * Destination Options, Authentication) to the upper-layer header, so a flow
 * hashes alike with and without them. A Fragment header ends the walk: all
 * fragments of a packet carry the same Next Header there, and none but the
 * first carries the ports, so fragments hash without ports, as in IPv4. An
 * atomic fragment (offset 0, no more fragments) is a whole packet and the walk
 * goes on. Where the chain is cut short or unknown, the protocol is the last
 * header type reached and *ports is cleared: stable for every packet of that
 * shape. Returns the offset of the upper-layer header. */
static size_t ipv6_upper_layer(const uint8_t *ip, size_t available, unsigned int *protocol, int *ports)
{
    size_t offset = 40;
    unsigned int next = ip[6], hops;
    for (hops = 0; hops < 8; ++hops) {
        size_t length;
        if (next == 0 || next == 43 || next == 60 || next == 51) {
            if (available < offset + 8) break;
            length = next == 51 ? ((size_t)ip[offset + 1] + 2) * 4 : ((size_t)ip[offset + 1] + 1) * 8;
        } else if (next == 44) {
            if (available < offset + 8) break;
            if (((ip[offset + 2] << 8 | ip[offset + 3]) & 0xfff9u) != 0) {   /* offset or more fragments */
                *protocol = ip[offset];
                *ports = 0;
                return offset + 8;
            }
            length = 8;
        } else {
            *protocol = next;
            return offset;
        }
        next = ip[offset];
        offset += length;
    }
    *protocol = next;
    *ports = 0;
    return offset;
}

/* One round of a multiplicative mix per 32-bit word. */
static uint32_t mix(uint32_t hash, uint32_t word)
{
    hash ^= word;
    hash *= 0x9e3779b1u;
    return hash ^ (hash >> 15);
}

static uint32_t word_at(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Mix one endpoint (address words, then the port) in. */
static uint32_t endpoint(uint32_t hash, const uint8_t *address, size_t size, const uint8_t *port)
{
    size_t i;
    for (i = 0; i < size; i += 4) hash = mix(hash, word_at(address + i));
    return port ? mix(hash, ((uint32_t)port[0] << 8) | port[1]) : hash;
}

/* Which endpoint goes first, so both directions hash alike. */
static int endpoint_order(const uint8_t *a, const uint8_t *b, size_t size, const uint8_t *pa, const uint8_t *pb)
{
    size_t i;
    for (i = 0; i < size; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    if (pa == NULL) return 0;
    for (i = 0; i < 2; ++i) if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    return 0;
}

uint32_t chmlink_flow_hash(chmlink_mode mode, const uint8_t *data, size_t length)
{
    const uint8_t *ip = data, *source, *destination, *source_port = NULL, *destination_port = NULL;
    size_t available = length, header, size;
    uint32_t hash = 0x811c9dc5u;
    unsigned int protocol;
    int ports;
    if (data == NULL) return 0;
    if (mode == CHMLINK_MODE_ETHERNET) {
        size_t offset = 14;
        unsigned int type;
        if (length < 14) return 0;
        type = ((unsigned int)data[12] << 8) | data[13];
        if (type == 0x8100u) {
            if (length < 18) return 0;
            type = ((unsigned int)data[16] << 8) | data[17];
            offset = 18;
        }
        if (type != 0x0800u && type != 0x86ddu) return 0;
        ip = data + offset;
        available = length - offset;
    } else if (mode != CHMLINK_MODE_IP) {
        return 0;
    }
    if (available < 20) return 0;
    if (ip[0] >> 4 == 4) {
        header = (size_t)(ip[0] & 15) * 4;
        if (header < 20) return 0;
        protocol = ip[9];
        source = ip + 12; destination = ip + 16; size = 4;
        /* Later fragments carry no ports; the first does, but its flow must
         * match theirs, so fragments hash without ports. */
        ports = (ip[6] & 0x3f) == 0 && ip[7] == 0;
    } else if (ip[0] >> 4 == 6) {
        if (available < 40) return 0;
        source = ip + 8; destination = ip + 24; size = 16;
        ports = 1;
        header = ipv6_upper_layer(ip, available, &protocol, &ports);
    } else {
        return 0;
    }
    if (ports && (protocol == 6 || protocol == 17) && available >= header + 4) {
        source_port = ip + header;
        destination_port = ip + header + 2;
    }
    /* Symmetric: a connection's two directions land in one queue, so the
     * stack handles its data and acknowledgements on one core. */
    if (endpoint_order(source, destination, size, source_port, destination_port) <= 0) {
        hash = endpoint(hash, source, size, source_port);
        hash = endpoint(hash, destination, size, destination_port);
    } else {
        hash = endpoint(hash, destination, size, destination_port);
        hash = endpoint(hash, source, size, source_port);
    }
    hash = mix(hash, protocol);
    return hash ? hash : 1;
}
