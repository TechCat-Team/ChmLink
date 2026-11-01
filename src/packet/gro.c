/* Software GRO (chmlink/gro.h). */
#include <string.h>
#include "chmlink/gro.h"
#include "chmlink/offload.h"

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (be16(p) << 16) | be16(p + 2); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static uint16_t add16(uint16_t a, uint16_t b)
{
    uint32_t sum = (uint32_t)a + b;
    return (uint16_t)((sum & 0xffffu) + (sum >> 16));
}

static uint16_t swap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

typedef struct segment {
    size_t l3, l4, payload, length;
    unsigned version, tcp_header, flags;
    uint32_t seq;
    uint16_t payload_sum;
    int eligible;   /* may merge */
    int tcp;        /* has a flow identity */
    int related;    /* may carry TCP of a held flow without showing its ports */
} segment;

/* Pseudo-header sum for a TCP length. */
static uint16_t pseudo(const uint8_t *packet, const segment *s, size_t tcp_length)
{
    const uint8_t *ip = packet + s->l3;
    uint8_t tail[8];
    if (s->version == 4) {
        tail[0] = 0; tail[1] = 6;
        tail[2] = (uint8_t)(tcp_length >> 8); tail[3] = (uint8_t)tcp_length;
        return chmlink_checksum_add(tail, 4, chmlink_checksum_add(ip + 12, 8, 0));
    }
    tail[0] = (uint8_t)(tcp_length >> 24); tail[1] = (uint8_t)(tcp_length >> 16);
    tail[2] = (uint8_t)(tcp_length >> 8); tail[3] = (uint8_t)tcp_length;
    tail[4] = tail[5] = tail[6] = 0; tail[7] = 6;
    return chmlink_checksum_add(tail, 8, chmlink_checksum_add(ip + 8, 32, 0));
}

static void parse(const chmlink_gro *g, const uint8_t *p, size_t length, segment *s)
{
    const uint8_t *ip, *tcp;
    memset(s, 0, sizeof(*s));
    s->length = length;
    if (g->mode == CHMLINK_MODE_ETHERNET) {
        unsigned type;
        if (length < 14) return;
        type = be16(p + 12);
        if (type != 0x0800u && type != 0x86ddu) return;
        s->l3 = 14;
    }
    if (length < s->l3 + 20) return;
    ip = p + s->l3;
    s->version = ip[0] >> 4;
    if (s->version == 4) {
        /* A TCP fragment, or TCP behind IP options, may belong to a held flow. */
        s->related = ip[9] == 6;
        if ((ip[0] & 15) != 5 || ip[9] != 6 || be16(ip + 2) != length - s->l3) return;
        if ((ip[6] & 0x3f) || ip[7]) return;                  /* fragments have no ports */
        s->l4 = s->l3 + 20;
    } else if (s->version == 6) {
        if (length < s->l3 + 40) return;
        /* TCP behind extension headers (or a fragment of it) may belong to a
         * held flow; such packets are not merged. */
        s->related = ip[6] == 0 || ip[6] == 43 || ip[6] == 44 || ip[6] == 60 || ip[6] == 51;
        if (ip[6] != 6 || be16(ip + 4) != length - s->l3 - 40) return;
        s->l4 = s->l3 + 40;
    } else {
        return;
    }
    if (length < s->l4 + 20) return;
    tcp = p + s->l4;
    s->tcp_header = (unsigned)(tcp[12] >> 4) * 4;
    if (s->tcp_header < 20 || length < s->l4 + s->tcp_header) return;
    s->tcp = 1;
    s->payload = s->l4 + s->tcp_header;
    s->flags = tcp[13];
    s->seq = be32(tcp + 4);
    if ((s->flags & ~(TCP_ACK | TCP_PSH)) || !(s->flags & TCP_ACK) || s->payload == length) return;
    if (s->version == 4 && chmlink_checksum_add(ip, 20, 0) != 0xffffu) return;
    if (g->verify) {
        s->payload_sum = chmlink_checksum_add(p + s->payload, length - s->payload, 0);
        if (add16(add16(pseudo(p, s, length - s->l4), chmlink_checksum_add(tcp, s->tcp_header, 0)),
                  s->payload_sum) != 0xffffu)
            return;
    } else {
        /* From the checksum field, without reading the payload: a valid
         * segment sums to 0xffff, so its payload sums to the complement of
         * the rest. A wrong field carries over into the merged packet. */
        s->payload_sum = (uint16_t)~add16(pseudo(p, s, length - s->l4), chmlink_checksum_add(tcp, s->tcp_header, 0));
    }
    s->eligible = 1;
}

/* Same flow: link header, addresses, ports. */
static int same_flow(const chmlink_gro_flow *f, const uint8_t *p, const segment *s)
{
    size_t addresses = s->version == 4 ? 8 : 32, at = s->version == 4 ? 12 : 8;
    if (f->l3 != s->l3 || f->l4 != s->l4 || (f->buffer[f->l3] >> 4) != s->version) return 0;
    if (s->l3 && memcmp(f->buffer, p, 14) != 0) return 0;
    return memcmp(f->buffer + f->l3 + at, p + s->l3 + at, addresses) == 0 &&
           memcmp(f->buffer + f->l4, p + s->l4, 4) == 0;
}

/* Same addresses (and link header): the flows a related packet may belong to. */
static int same_hosts(const chmlink_gro_flow *f, const uint8_t *p, const segment *s)
{
    size_t addresses = s->version == 4 ? 8 : 32, at = s->version == 4 ? 12 : 8;
    if (f->l3 != s->l3 || (f->buffer[f->l3] >> 4) != s->version) return 0;
    if (s->l3 && memcmp(f->buffer, p, 14) != 0) return 0;
    return memcmp(f->buffer + f->l3 + at, p + s->l3 + at, addresses) == 0;
}

/* The segment continues the held packet exactly. */
static int continues(const chmlink_gro *g, const chmlink_gro_flow *f, const uint8_t *p, const segment *s)
{
    const uint8_t *held = f->buffer + f->l3, *ip = p + s->l3;
    size_t added = s->length - s->payload;
    if (!s->eligible || f->payload - f->l4 != s->tcp_header || s->seq != f->next_seq) return 0;
    if (added > f->mss || f->length + added > g->limit) return 0;
    if (f->length + added - f->l3 - (s->version == 4 ? 0 : 40) > 65535) return 0;
    if (s->version == 4) {
        if (held[1] != ip[1] || held[8] != ip[8] || (held[6] & 0x40) != (ip[6] & 0x40)) return 0;
    } else if (memcmp(held, ip, 4) != 0 || held[7] != ip[7]) {
        return 0;
    }
    /* Acknowledgement and options alike; the window may move on. */
    return memcmp(f->buffer + f->l4 + 8, p + s->l4 + 8, 4) == 0 &&
           memcmp(f->buffer + f->l4 + 20, p + s->l4 + 20, s->tcp_header - 20) == 0;
}

static void emit(chmlink_gro *g, const uint8_t *packet, size_t length, const chmlink_offload *offload)
{
    g->packets_out++;
    g->output(packet, length, offload, g->context);
}

static void flush_flow(chmlink_gro *g, chmlink_gro_flow *f)
{
    uint8_t *p = f->buffer, *ip = p + f->l3, *tcp = p + f->l4;
    chmlink_offload offload;
    segment s;
    uint16_t sum;
    if (f->length == 0) return;
    if (f->segments == 1) {
        emit(g, p, f->length, NULL);
        f->length = 0;
        return;
    }
    if ((ip[0] >> 4) == 4) {
        put16(ip + 2, (uint32_t)(f->length - f->l3));
        ip[10] = ip[11] = 0;
        put16(ip + 10, (uint16_t)~chmlink_checksum_add(ip, 20, 0));
    } else {
        put16(ip + 4, (uint32_t)(f->length - f->l3 - 40));
    }
    memset(&s, 0, sizeof(s));
    s.l3 = f->l3; s.version = ip[0] >> 4;
    tcp[16] = tcp[17] = 0;
    sum = add16(pseudo(p, &s, f->length - f->l4), chmlink_checksum_add(tcp, f->payload - f->l4, 0));
    put16(tcp + 16, (uint16_t)~add16(sum, f->payload_sum));
    if (g->mode == CHMLINK_MODE_ETHERNET) {
        memset(&offload, 0, sizeof(offload));
        offload.flags = CHMLINK_OFFLOAD_DATA_VALID;
        offload.gso_type = (uint8_t)(s.version == 4 ? CHMLINK_GSO_TCPV4 : CHMLINK_GSO_TCPV6);
        offload.hdr_len = (uint16_t)f->payload;
        offload.gso_size = f->mss;
        offload.csum_start = (uint16_t)f->l4;
        offload.csum_offset = 16;
        emit(g, p, f->length, &offload);
    } else {
        emit(g, p, f->length, NULL);
    }
    f->length = 0;
}

chmlink_result chmlink_gro_init(chmlink_gro *g, chmlink_mode mode, size_t limit, uint32_t flags, uint8_t *memory,
                                size_t memory_size, chmlink_gro_output output, void *context)
{
    unsigned i;
    if (g == NULL || output == NULL || memory == NULL || limit < 64 || limit > CHMLINK_PACKET_MAX_SIZE ||
        (mode != CHMLINK_MODE_IP && mode != CHMLINK_MODE_ETHERNET) || (flags & ~CHMLINK_GRO_TRUSTED))
        return CHMLINK_INVALID_ARGUMENT;
    if (memory_size < CHMLINK_GRO_MEMORY(limit)) return CHMLINK_BUFFER_TOO_SMALL;
    memset(g, 0, sizeof(*g));
    g->mode = mode; g->limit = limit; g->output = output; g->context = context;
    g->verify = mode == CHMLINK_MODE_ETHERNET && !(flags & CHMLINK_GRO_TRUSTED);
    for (i = 0; i < CHMLINK_GRO_FLOWS; ++i) g->flows[i].buffer = memory + (size_t)i * limit;
    return CHMLINK_OK;
}

chmlink_result chmlink_gro_add(chmlink_gro *g, const uint8_t *p, size_t length)
{
    chmlink_gro_flow *f = NULL, *free_flow = NULL, *oldest = NULL;
    segment s;
    unsigned i;
    if (g == NULL || p == NULL || length == 0) return CHMLINK_INVALID_ARGUMENT;
    g->packets_in++;
    g->clock++;
    parse(g, p, length, &s);
    if (!s.tcp && s.related) {
        /* It may be TCP of a held flow in a form that does not show the ports:
         * let out everything between these hosts first, to keep the order. */
        for (i = 0; i < CHMLINK_GRO_FLOWS; ++i)
            if (g->flows[i].length != 0 && same_hosts(&g->flows[i], p, &s)) flush_flow(g, &g->flows[i]);
        emit(g, p, length, NULL);
        return CHMLINK_OK;
    }
    if (s.tcp) {
        for (i = 0; i < CHMLINK_GRO_FLOWS; ++i) {
            chmlink_gro_flow *c = &g->flows[i];
            if (c->length == 0) { if (free_flow == NULL) free_flow = c; continue; }
            if (same_flow(c, p, &s)) { f = c; break; }
            if (oldest == NULL || c->age < oldest->age) oldest = c;
        }
    }
    if (f != NULL) {
        if (continues(g, f, p, &s)) {
            size_t added = length - s.payload;
            memcpy(f->buffer + f->length, p + s.payload, added);
            /* A payload that starts at an odd offset sums byte-swapped. */
            f->payload_sum = add16(f->payload_sum, (f->payload_bytes & 1) ? swap16(s.payload_sum) : s.payload_sum);
            f->payload_bytes += (uint32_t)added;
            f->length += added;
            f->next_seq += (uint32_t)added;
            f->segments++;
            f->age = g->clock;
            memcpy(f->buffer + f->l4 + 14, p + s.l4 + 14, 2);        /* newest window */
            if (s.flags & TCP_PSH) f->buffer[f->l4 + 13] |= TCP_PSH;
            if ((s.flags & TCP_PSH) || added < f->mss) flush_flow(g, f);
            return CHMLINK_OK;
        }
        /* The flow goes on differently: what is held goes first. */
        flush_flow(g, f);
        free_flow = f;
    }
    if (!s.eligible || (s.flags & TCP_PSH) || length > g->limit || length - s.payload > 0xffffu) {
        emit(g, p, length, NULL);
        return CHMLINK_OK;
    }
    if (free_flow == NULL) {
        flush_flow(g, oldest);
        free_flow = oldest;
    }
    f = free_flow;
    memcpy(f->buffer, p, length);
    f->length = length;
    f->l3 = s.l3; f->l4 = s.l4; f->payload = s.payload;
    f->mss = (uint16_t)(length - s.payload);
    f->payload_bytes = f->mss;
    f->payload_sum = s.payload_sum;
    f->next_seq = s.seq + f->mss;
    f->segments = 1;
    f->age = g->clock;
    return CHMLINK_OK;
}

void chmlink_gro_flush(chmlink_gro *g)
{
    unsigned i;
    if (g == NULL) return;
    for (i = 0; i < CHMLINK_GRO_FLOWS; ++i) flush_flow(g, &g->flows[i]);
}
