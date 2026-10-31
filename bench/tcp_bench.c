/* TCP throughput through a ChmLink adapter (IP or Ethernet mode), end to end through the
 * Windows TCP/IP stack. Opt-in, run by tools/bench.ps1 in the test VM.
 *
 *   chmlink-tcp-bench <ifIndex> <local-ipv4> <peer-ipv4> <alias-ipv4> [seconds] [streams]
 *
 * A reflector thread turns one adapter into a wire between two local sockets.
 * A client connects from L (local) to P (peer); the reflector rewrites each of
 * its packets to come from A (alias) to L, so the stack delivers it to a
 * server listening on L as a separate connection from A. The server's packets
 * go to A, and come back rewritten as from P to L. Every byte therefore leaves
 * through the adapter (transmit path) and enters through it again (receive
 * path), twice per round trip, with ACKs flowing the other way.
 *
 * Only addresses change: the IPv4 header and TCP checksums are updated
 * incrementally (RFC 1624). Received batches are rewritten in place and
 * injected as they are. In Ethernet mode the reflector also answers ARP for P
 * and A and swaps the MACs.
 *
 * Where the adapter offers offloads it runs without and then with them, and
 * checks a sample of the stack's GSO packets independently: each segment from
 * chmlink_gso_segment must have valid IPv4 and TCP checksums (RFC 1071),
 * consecutive sequence numbers and the original data. Coalesced packets are
 * injected back as they came (RSC); the receiving socket must still see a
 * complete stream. Output is one JSON object per line. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chmlink/api.h"
#include "chmlink/offload.h"
#include "chmlink/protocol.h"
#include "arp_peer.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (win=%lu wsa=%d)\n", \
    __FILE__, __LINE__, #x, GetLastError(), WSAGetLastError()); ExitProcess(1); } } while (0)

#define PORT_SERVER 5201u
#define MAX_STREAMS 16
#define CHUNK (256 * 1024)

static uint8_t local4[4], peer4[4], alias4[4];
static volatile LONG stop_reflector, stop_traffic;
static volatile LONG64 reflected_packets, reflected_bytes, reflected_batches, rewrite_misses;
/* What the stack handed us: GSO packets (LSO), partial checksums, largest packet. */
static volatile LONG64 gso_packets, csum_partial;
static volatile LONG max_packet;
/* Independent check of what chmlink_gso_segment makes of the stack's GSO packets. */
static volatile LONG64 verified_packets, verified_segments, verify_failures;

/* Ethernet mode: the reflector answers ARP for the peer and alias addresses
 * with an invented MAC and swaps the MACs of every reflected frame. Frames it
 * does not reflect get a foreign destination, so the adapter discards them
 * instead of looping the stack's own neighbour traffic back to it. */
static int g_eth;
static uint8_t g_mac[6];
static const uint8_t g_peer_mac[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0x01 };
static const uint8_t g_drop_mac[6] = { 0x02, 0x00, 0x5e, 0x10, 0x00, 0xff };
static uint8_t arp_replies[8][42];
static int arp_count;

/* IP mode with an MTU above 1500: the stack hands over packets up to the MTU,
 * and the reflector cuts them to 1500 bytes with chmlink_ip_split before it
 * injects them, as a tunnel with a 1500-byte path would. With
 * CHMLINK_BENCH_NO_SPLIT=1 it injects them whole instead, as a tunnel that
 * carries large packets (a stream transport between two large-MTU adapters). */
#define SPLIT_MTU 1500u
static int g_split;
/* Software GRO on the reflector's injecting side (chmlink_coalescer): with
 * CHMLINK_BENCH_GRO=1 in split mode the 1500-byte pieces are merged again
 * before injection, as a receiving tunnel endpoint would; in Ethernet mode the
 * "_gro" runs give Windows plain frames (no LSO) and merge them into RSC. The
 * reflector thread creates and frees the coalescer when this flag changes. */
static volatile LONG g_want_gro;
static char g_prefix[32] = "";
static volatile LONG64 injected_packets;
static LARGE_INTEGER qpf;

static uint16_t fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum;
}

/* RFC 1624: HC' = ~(~HC + ~m + m') over 16-bit words, for each changed word. */
static void checksum_replace(uint8_t *field, const uint8_t *old_words, const uint8_t *new_words, size_t bytes)
{
    uint32_t sum = (uint32_t)(uint16_t)~(((uint16_t)field[0] << 8) | field[1]);
    size_t i;
    for (i = 0; i < bytes; i += 2) {
        sum += (uint16_t)~(((uint16_t)old_words[i] << 8) | old_words[i + 1]);
        sum += ((uint16_t)new_words[i] << 8) | new_words[i + 1];
    }
    sum = (uint16_t)~fold(sum);
    field[0] = (uint8_t)(sum >> 8); field[1] = (uint8_t)sum;
}

/* L->P becomes A->L; L->A becomes P->L. Returns 0 for anything else. */
static int rewrite(uint8_t *ip, uint32_t length)
{
    uint8_t before[8], after[8];
    unsigned header;
    if (length < 20 || (ip[0] >> 4) != 4) return 0;
    header = (ip[0] & 15u) * 4u;
    if (header < 20 || length < header || memcmp(ip + 12, local4, 4) != 0) return 0;
    memcpy(before, ip + 12, 8);
    if (memcmp(ip + 16, peer4, 4) == 0) { memcpy(after, alias4, 4); memcpy(after + 4, local4, 4); }
    else if (memcmp(ip + 16, alias4, 4) == 0) { memcpy(after, peer4, 4); memcpy(after + 4, local4, 4); }
    else return 0;
    memcpy(ip + 12, after, 8);
    checksum_replace(ip + 10, before, after, 8);
    /* TCP's pseudo-header covers both addresses; a first fragment only. */
    if (ip[9] == 6 && length >= header + 18 && (((ip[6] & 0x1f) << 8) | ip[7]) == 0)
        checksum_replace(ip + header + 16, before, after, 8);
    return 1;
}

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (be16(p) << 16) | be16(p + 2); }

/* RFC 1071 sum, written out independently of the library. */
static uint32_t sum16(const uint8_t *p, size_t n, uint32_t sum)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) sum += be16(p + i);
    if (n & 1) sum += (uint32_t)p[n - 1] << 8;
    return sum;
}

typedef struct verify_ctx {
    const uint8_t *payload;   /* TCP payload of the original GSO packet */
    size_t payload_length, offset;
    uint32_t next_seq;
    unsigned l2;
    int ok;
} verify_ctx;

static int verify_segment(const uint8_t *segment, size_t length, void *context)
{
    verify_ctx *v = context;
    const uint8_t *ip = segment + v->l2, *tcp;
    unsigned ihl, doff;
    uint32_t total, tcp_length;
    uint8_t pseudo[12];
    v->ok = 0;
    if (length < v->l2 + 40 || (ip[0] >> 4) != 4) return 0;
    ihl = (ip[0] & 15u) * 4u; total = be16(ip + 2);
    if (ihl < 20 || total != length - v->l2 || total < ihl + 20) return 0;
    if (fold(sum16(ip, ihl, 0)) != 0xffff) return 0;
    tcp = ip + ihl; tcp_length = total - ihl; doff = (tcp[12] >> 4) * 4u;
    if (doff < 20 || doff > tcp_length) return 0;
    memcpy(pseudo, ip + 12, 8); pseudo[8] = 0; pseudo[9] = 6;
    pseudo[10] = (uint8_t)(tcp_length >> 8); pseudo[11] = (uint8_t)tcp_length;
    if (fold(sum16(tcp, tcp_length, sum16(pseudo, 12, 0))) != 0xffff) return 0;
    if (be32(tcp + 4) != v->next_seq) return 0;
    if (v->offset + (tcp_length - doff) > v->payload_length ||
        memcmp(tcp + doff, v->payload + v->offset, tcp_length - doff) != 0) return 0;
    v->offset += tcp_length - doff;
    v->next_seq += tcp_length - doff;
    v->ok = 1;
    InterlockedIncrement64(&verified_segments);
    return 1;
}

/* Segment a copy of an IPv4 GSO packet and check every segment: IPv4 header
 * checksum, full TCP checksum, consecutive sequence numbers and the data.
 * Sampled, as it copies and reads the data twice. Reflector thread only. */
static void verify_gso(const uint8_t *packet, uint32_t length, const chmlink_offload *descriptor)
{
    static uint8_t copy[CHMLINK_PACKET_MAX_SIZE + 64], scratch[128 * 1024];
    static unsigned sample;
    verify_ctx v;
    unsigned l2 = g_eth ? 14u : 0u, ihl, doff;
    const uint8_t *ip = packet + l2, *tcp;
    if ((sample++ & 15) != 0 || verified_packets >= 4000) return;
    if (length < l2 + 40u || length > sizeof(copy) || (ip[0] >> 4) != 4 || ip[9] != 6) return;
    ihl = (ip[0] & 15u) * 4u; tcp = ip + ihl; doff = (tcp[12] >> 4) * 4u;
    if (l2 + ihl + doff > length) return;
    memset(&v, 0, sizeof(v));
    v.l2 = l2; v.ok = 1;
    v.payload = tcp + doff; v.payload_length = length - (size_t)(v.payload - packet);
    v.next_seq = be32(tcp + 4);
    memcpy(copy, packet, length);
    if (chmlink_gso_segment(g_eth ? CHMLINK_MODE_ETHERNET : CHMLINK_MODE_IP, copy, length, descriptor,
                            scratch, sizeof(scratch), verify_segment, &v) != CHMLINK_OK ||
        !v.ok || v.offset != v.payload_length)
        InterlockedIncrement64(&verify_failures);
    InterlockedIncrement64(&verified_packets);
}

/* Ethernet framing around rewrite(). ARP requests for the peer or alias get a
 * queued reply; the request itself, like anything else not reflected, gets a
 * foreign destination MAC. */
static int reflect(uint8_t *packet, uint32_t length)
{
    uint32_t type;
    if (!g_eth) return rewrite(packet, length);
    if (length < 14) return 0;
    type = be16(packet + 12);
    if (type == 0x0800 && rewrite(packet + 14, length - 14)) {
        memcpy(packet, g_mac, 6);
        memcpy(packet + 6, g_peer_mac, 6);
        return 1;
    }
    if (type == 0x0806 && arp_count < 8) {
        if (arp_make_reply(packet, length, local4, peer4, g_peer_mac, arp_replies[arp_count]) ||
            arp_make_reply(packet, length, local4, alias4, g_peer_mac, arp_replies[arp_count]))
            arp_count++;
    }
    memcpy(packet, g_drop_mac, 6);
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Rewrite every packet in place. Descriptors gain DATA_VALID; their checksum
 * fields stay unspecified when NEEDS_CSUM is set, so only the IPv4 header
 * checksum is updated incrementally for those.
 * Returns the packet count and byte total, or 0 if the batch is malformed. */
static uint32_t rewrite_batch(uint8_t *buffer, uint32_t bytes,
                              uint32_t *packets, uint32_t *bytes_out)
{
    uint32_t count = rd32(buffer + 8), i, offset = CHMLINK_BATCH_HEADER_SIZE, total = 0;
    int offload = (rd32(buffer + 16) & CHMLINK_BATCH_FLAG_OFFLOAD) != 0;
    if (bytes > CHMLINK_BATCH_MAX_SIZE || count > CHMLINK_BATCH_MAX_PACKETS) return 0;
    for (i = 0; i < count; ++i) {
        uint32_t length;
        uint8_t *descriptor = NULL, *packet;
        if (bytes - offset < 4) return 0;
        length = rd32(buffer + offset); offset += 4;
        if (offload) {
            if (bytes - offset < CHMLINK_OFFLOAD_SIZE) return 0;
            descriptor = buffer + offset; offset += CHMLINK_OFFLOAD_SIZE;
        }
        if (length > CHMLINK_PACKET_MAX_SIZE || bytes - offset < ((length + 3u) & ~3u)) return 0;
        packet = buffer + offset;
        if ((LONG)length > max_packet) InterlockedExchange(&max_packet, (LONG)length);
        if (descriptor != NULL) {
            chmlink_offload seen;
            memcpy(&seen, descriptor, sizeof(seen));
            /* Before the rewrite: the check needs the stack's own addresses. */
            if (seen.gso_type != CHMLINK_GSO_NONE) verify_gso(packet, length, &seen);
        }
        if (!reflect(packet, length)) InterlockedIncrement64(&rewrite_misses);
        if (descriptor != NULL) {
            chmlink_offload done;
            memcpy(&done, descriptor, sizeof(done));
            if (done.gso_type != CHMLINK_GSO_NONE) InterlockedIncrement64(&gso_packets);
            if (done.flags & CHMLINK_OFFLOAD_NEEDS_CSUM) InterlockedIncrement64(&csum_partial);
            done.flags |= CHMLINK_OFFLOAD_DATA_VALID;
            /* Keep NEEDS_CSUM: the address rewrite invalidates the partial
             * pseudo-header sum the sender left in the field. */
            memset(descriptor, 0, CHMLINK_OFFLOAD_SIZE);
            descriptor[0] = done.flags; descriptor[1] = done.gso_type;
            descriptor[2] = (uint8_t)done.hdr_len; descriptor[3] = (uint8_t)(done.hdr_len >> 8);
            descriptor[4] = (uint8_t)done.gso_size; descriptor[5] = (uint8_t)(done.gso_size >> 8);
            descriptor[6] = (uint8_t)done.csum_start; descriptor[7] = (uint8_t)(done.csum_start >> 8);
            descriptor[8] = (uint8_t)done.csum_offset; descriptor[9] = (uint8_t)(done.csum_offset >> 8);
        }
        total += length;
        offset += (length + 3u) & ~3u;
    }
    *packets = count;
    *bytes_out = total;
    return count;
}

static int append_segment(const uint8_t *segment, size_t length, void *context)
{
    chmlink_batch_writer *writer = context;
    return chmlink_batch_append(writer, segment, length) == CHMLINK_OK;
}

/* Fallback when the driver refuses coalesced segments: cut GSO packets into
 * ordinary ones. Returns bytes written, 0 on failure. */
static uint32_t segment_batch(uint8_t *in, uint32_t bytes, uint8_t *out, uint32_t capacity,
                              uint8_t *scratch, size_t scratch_size, uint32_t limit)
{
    chmlink_batch_reader reader;
    chmlink_batch_writer writer;
    const uint8_t *packet;
    uint32_t size;
    chmlink_offload descriptor;
    chmlink_batch_read_begin(&reader, in);
    chmlink_mode mode = g_eth ? CHMLINK_MODE_ETHERNET : CHMLINK_MODE_IP;
    if (chmlink_batch_begin(&writer, out, capacity, mode, limit) != CHMLINK_OK) return 0;
    while (chmlink_batch_read_next(&reader, &packet, &size, &descriptor)) {
        if (descriptor.gso_type != CHMLINK_GSO_NONE) {
            if (chmlink_gso_segment(mode, (uint8_t *)packet, size, &descriptor,
                                    scratch, scratch_size, append_segment, &writer) != CHMLINK_OK)
                return 0;
        } else {
            if (descriptor.flags != 0 &&
                chmlink_offload_complete(mode, (uint8_t *)packet, size, &descriptor) != CHMLINK_OK)
                return 0;
            if (chmlink_batch_append(&writer, packet, size) != CHMLINK_OK) return 0;
        }
    }
    return (uint32_t)writer.length;
}

typedef struct reflector {
    chmlink_adapter *adapter;
    uint32_t limit;
    uint8_t *fallback, *scratch;
    chmlink_batch_writer out;   /* split packets waiting to be injected */
    uint32_t ipv6_id;
    chmlink_coalescer *coalescer;
} reflector;

/* Inject a batch, waiting while the ring is full. */
static void inject_all(reflector *r, const uint8_t *batch, uint32_t length)
{
    while (!chmlink_inject(r->adapter, batch, length) && GetLastError() == ERROR_BUFFER_OVERFLOW && !stop_reflector)
        SwitchToThread();
}

static int append_split(const uint8_t *segment, size_t length, void *context)
{
    reflector *r = context;
    if (r->coalescer != NULL) {
        REQUIRE(chmlink_coalescer_send(r->coalescer, segment, (uint32_t)length) || stop_reflector);
        InterlockedIncrement64(&injected_packets);
        return 1;
    }
    if (chmlink_batch_append(&r->out, segment, length) != CHMLINK_OK) {
        inject_all(r, r->fallback, (uint32_t)r->out.length);
        REQUIRE(chmlink_batch_begin(&r->out, r->fallback, CHMLINK_BATCH_MAX_SIZE, CHMLINK_MODE_IP, r->limit) == CHMLINK_OK);
        REQUIRE(chmlink_batch_append(&r->out, segment, length) == CHMLINK_OK);
    }
    InterlockedIncrement64(&injected_packets);
    return 1;
}

/* The rewritten batch, cut to SPLIT_MTU and injected as one or more batches. */
static void inject_split(reflector *r, const uint8_t *batch)
{
    chmlink_batch_reader reader;
    const uint8_t *packet;
    uint32_t size;
    chmlink_offload descriptor;
    REQUIRE(chmlink_batch_begin(&r->out, r->fallback, CHMLINK_BATCH_MAX_SIZE, CHMLINK_MODE_IP, r->limit) == CHMLINK_OK);
    chmlink_batch_read_begin(&reader, batch);
    while (chmlink_batch_read_next(&reader, &packet, &size, &descriptor)) {
        chmlink_result result = chmlink_ip_split((uint8_t *)packet, size, SPLIT_MTU, r->ipv6_id++,
                                                 r->scratch, 128 * 1024, append_split, r);
        REQUIRE(result == CHMLINK_OK || result == CHMLINK_TOO_BIG);   /* too big: dropped, as a link would */
    }
    if (r->out.count != 0) inject_all(r, r->fallback, (uint32_t)r->out.length);
    if (r->coalescer != NULL) REQUIRE(chmlink_coalescer_flush(r->coalescer) || stop_reflector);
}

/* Every packet of the rewritten batch through the coalescer, then a flush. */
static void inject_coalesced(reflector *r, const uint8_t *batch)
{
    chmlink_batch_reader reader;
    const uint8_t *packet;
    uint32_t size;
    chmlink_offload descriptor;
    chmlink_batch_read_begin(&reader, batch);
    while (chmlink_batch_read_next(&reader, &packet, &size, &descriptor))
        REQUIRE(chmlink_coalescer_send(r->coalescer, packet, size) || stop_reflector);
    REQUIRE(chmlink_coalescer_flush(r->coalescer) || stop_reflector);
}

static DWORD WINAPI reflector_thread(void *arg)
{
    reflector *r = arg;
    uint8_t *buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    uint32_t bytes, count, payload = 0, idle = 0, fallbacks = 0;
    REQUIRE(buffer != NULL);
    while (!stop_reflector) {
        if (!chmlink_receive(r->adapter, buffer, CHMLINK_BATCH_MAX_SIZE, &bytes)) {
            REQUIRE(GetLastError() == ERROR_NO_MORE_ITEMS);
            /* Spin briefly, then block: a wait costs a system call, a spin a core. */
            if (++idle > 64) { (void)chmlink_wait_readable(r->adapter, 5); idle = 0; }
            continue;
        }
        idle = 0;
        if (g_want_gro && r->coalescer == NULL) REQUIRE((r->coalescer = chmlink_coalescer_create(r->adapter, 0)) != NULL);
        if (!g_want_gro && r->coalescer != NULL) { chmlink_coalescer_free(r->coalescer); r->coalescer = NULL; }
        if (!rewrite_batch(buffer, bytes, &count, &payload)) continue;
        if (g_split) {
            inject_split(r, buffer);
            InterlockedAdd64(&reflected_packets, count);
            InterlockedAdd64(&reflected_bytes, payload);
            InterlockedIncrement64(&reflected_batches);
            continue;
        }
        if (r->coalescer != NULL && g_eth) {
            inject_coalesced(r, buffer);
            InterlockedAdd64(&injected_packets, count);
            InterlockedAdd64(&reflected_packets, count);
            InterlockedAdd64(&reflected_bytes, payload);
            InterlockedIncrement64(&reflected_batches);
            continue;
        }
        InterlockedAdd64(&injected_packets, count);
        /* Packets the rewrite did not recognise (IPv6 neighbour traffic, say) go back unchanged;
         * the stack drops them. Injection failing under load is a dropped batch, like a full link. */
        while (!chmlink_inject(r->adapter, buffer, bytes)) {
            DWORD e = GetLastError();
            /* A full ring is back-pressure: wait for the driver, as a link would queue. */
            if (e == ERROR_BUFFER_OVERFLOW && !stop_reflector) { SwitchToThread(); continue; }
            /* Refused descriptors (coalesced segments without RSC): segment in software. */
            if (e == ERROR_INVALID_PARAMETER) {
                uint32_t rebuilt = segment_batch(buffer, bytes, r->fallback, CHMLINK_BATCH_MAX_SIZE,
                                                 r->scratch, 128 * 1024, r->limit);
                while (rebuilt != 0 && !chmlink_inject(r->adapter, r->fallback, rebuilt) &&
                       GetLastError() == ERROR_BUFFER_OVERFLOW && !stop_reflector)
                    SwitchToThread();
                fallbacks++;
            }
            break;
        }
        if (arp_count != 0) {
            chmlink_batch_writer writer;
            int i;
            if (chmlink_batch_begin(&writer, r->fallback, CHMLINK_BATCH_MAX_SIZE, CHMLINK_MODE_ETHERNET,
                                    r->limit) == CHMLINK_OK) {
                for (i = 0; i < arp_count; ++i) (void)chmlink_batch_append(&writer, arp_replies[i], 42);
                (void)chmlink_inject(r->adapter, r->fallback, (uint32_t)writer.length);
            }
            arp_count = 0;
        }
        InterlockedAdd64(&reflected_packets, count);
        InterlockedAdd64(&reflected_bytes, payload);
        InterlockedIncrement64(&reflected_batches);
    }
    if (r->coalescer != NULL) chmlink_coalescer_free(r->coalescer);
    printf("{\"event\":\"tcp_fallbacks\",\"batches\":%lu}\n", (unsigned long)fallbacks);
    free(buffer);
    return 0;
}

static void tune(SOCKET s)
{
    int size = 4 * 1024 * 1024, one = 1;
    REQUIRE(setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&size, sizeof(size)) == 0);
    REQUIRE(setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&size, sizeof(size)) == 0);
    REQUIRE(setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one)) == 0);
}

typedef struct stream { SOCKET client, server; LONG64 received; } stream;
static stream streams[MAX_STREAMS];

static DWORD WINAPI sender(void *arg)
{
    stream *s = arg;
    char *chunk = malloc(CHUNK);
    int i;
    REQUIRE(chunk != NULL);
    /* Non-zero data, so a misplaced or zeroed byte shows in the segment check. */
    for (i = 0; i < CHUNK; ++i) chunk[i] = (char)(i % 251);
    while (!stop_traffic) if (send(s->client, chunk, CHUNK, 0) <= 0) break;
    free(chunk);
    return 0;
}

static DWORD WINAPI receiver(void *arg)
{
    stream *s = arg;
    char *chunk = malloc(CHUNK);
    int n;
    REQUIRE(chunk != NULL);
    while ((n = recv(s->server, chunk, CHUNK, 0)) > 0) InterlockedAdd64(&s->received, n);
    free(chunk);
    return 0;
}

static ULONGLONG ft64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static double busy_cores(FILETIME i0, FILETIME k0, FILETIME u0, FILETIME i1, FILETIME k1, FILETIME u1, double wall)
{
    /* System kernel time includes idle time. */
    double busy = (double)((ft64(k1) - ft64(k0)) + (ft64(u1) - ft64(u0)) - (ft64(i1) - ft64(i0))) / 1e7;
    return busy / wall;
}

static void run(chmlink_adapter *a, const chmlink_adapter_info *info, int count, double seconds,
                const char *suffix)
{
    SOCKET listener;
    struct sockaddr_in server_addr, client_addr, peer_addr;
    HANDLE threads[2 * MAX_STREAMS];
    LARGE_INTEGER t0, t1;
    FILETIME i0, k0, u0, i1, k1, u1;
    LONG64 packets0, bytes0, batches0, gso0, csum0, verified0, segments0, injected0, received = 0;
    double wall;
    int i;
    (void)info;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); REQUIRE(listener != INVALID_SOCKET);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET; server_addr.sin_port = htons(PORT_SERVER);
    memcpy(&server_addr.sin_addr, local4, 4);
    {
        /* In Ethernet mode the link comes up with the session, and the address
         * stays tentative while duplicate detection runs (ARP probes). */
        int tries, bound = 0;
        for (tries = 0; tries < 100 && !bound; ++tries) {
            if (bind(listener, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) bound = 1;
            else if (WSAGetLastError() != WSAEADDRNOTAVAIL) break;
            else Sleep(100);
        }
        REQUIRE(bound);
    }
    REQUIRE(listen(listener, MAX_STREAMS) == 0);
    for (i = 0; i < count; ++i) {
        stream *s = &streams[i];
        memset(s, 0, sizeof(*s));
        s->client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); REQUIRE(s->client != INVALID_SOCKET);
        tune(s->client);
        memset(&client_addr, 0, sizeof(client_addr));
        client_addr.sin_family = AF_INET; memcpy(&client_addr.sin_addr, local4, 4);
        REQUIRE(bind(s->client, (struct sockaddr *)&client_addr, sizeof(client_addr)) == 0);
        memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.sin_family = AF_INET; peer_addr.sin_port = htons(PORT_SERVER);
        memcpy(&peer_addr.sin_addr, peer4, 4);
        REQUIRE(connect(s->client, (struct sockaddr *)&peer_addr, sizeof(peer_addr)) == 0);
        s->server = accept(listener, NULL, NULL); REQUIRE(s->server != INVALID_SOCKET);
        tune(s->server);
    }
    closesocket(listener);
    Sleep(200);
    packets0 = reflected_packets; bytes0 = reflected_bytes; batches0 = reflected_batches;
    gso0 = gso_packets; csum0 = csum_partial; InterlockedExchange(&max_packet, 0);
    verified0 = verified_packets; segments0 = verified_segments; injected0 = injected_packets;
    REQUIRE(GetSystemTimes(&i0, &k0, &u0));
    QueryPerformanceCounter(&t0);
    InterlockedExchange(&stop_traffic, 0);
    for (i = 0; i < count; ++i) {
        threads[2 * i] = CreateThread(NULL, 0, sender, &streams[i], 0, NULL);
        threads[2 * i + 1] = CreateThread(NULL, 0, receiver, &streams[i], 0, NULL);
        REQUIRE(threads[2 * i] && threads[2 * i + 1]);
    }
    Sleep((DWORD)(seconds * 1000));
    for (i = 0; i < count; ++i) received += streams[i].received;
    QueryPerformanceCounter(&t1);
    REQUIRE(GetSystemTimes(&i1, &k1, &u1));
    wall = (double)(t1.QuadPart - t0.QuadPart) / (double)qpf.QuadPart;
    {
        LONG64 packets = reflected_packets - packets0, bytes = reflected_bytes - bytes0, batches = reflected_batches - batches0;
        printf("{\"event\":\"result\",\"scenario\":\"tcp_%s%dstream%s%s\",\"seconds\":%.2f,\"goodput_mbps\":%.1f,"
               "\"adapter_pps\":%.0f,\"avg_packet_bytes\":%.0f,\"packets_per_batch\":%.1f,\"busy_cores\":%.2f,"
               "\"gso_packets\":%lld,\"csum_partial\":%lld,\"max_packet\":%ld,"
               "\"verified_packets\":%lld,\"verified_segments\":%lld,\"injected_pps\":%.0f,\"errors\":%lld}\n",
               g_prefix, count, count == 1 ? "" : "s", suffix, wall, (double)received * 8 / wall / 1e6,
               (double)packets / wall, packets ? (double)bytes / (double)packets : 0.0,
               batches ? (double)packets / (double)batches : 0.0, busy_cores(i0, k0, u0, i1, k1, u1, wall),
               (long long)(gso_packets - gso0), (long long)(csum_partial - csum0), (long)max_packet,
               (long long)(verified_packets - verified0), (long long)(verified_segments - segments0),
               (double)(injected_packets - injected0) / wall,
               (long long)verify_failures);
        fflush(stdout);
    }
    InterlockedExchange(&stop_traffic, 1);
    for (i = 0; i < count; ++i) { shutdown(streams[i].client, SD_BOTH); shutdown(streams[i].server, SD_BOTH); }
    for (i = 0; i < count; ++i) { closesocket(streams[i].client); closesocket(streams[i].server); }
    REQUIRE(WaitForMultipleObjects((DWORD)(2 * count), threads, TRUE, 30000) == WAIT_OBJECT_0);
    for (i = 0; i < 2 * count; ++i) CloseHandle(threads[i]);
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    chmlink_adapter *a;
    chmlink_adapter_info info;
    reflector r;
    HANDLE thread;
    double seconds = 5.0;
    int streams_max = 4;
    if (argc < 5) {
        fprintf(stderr, "usage: chmlink-tcp-bench <ifIndex> <local-ipv4> <peer-ipv4> <alias-ipv4> [seconds] [streams] (IP or Ethernet mode)\n");
        return 2;
    }
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    REQUIRE(inet_pton(AF_INET, argv[2], local4) == 1 && inet_pton(AF_INET, argv[3], peer4) == 1 &&
            inet_pton(AF_INET, argv[4], alias4) == 1);
    if (argc > 5) seconds = atof(argv[5]);
    if (argc > 6) streams_max = atoi(argv[6]);
    if (seconds < 1.0) seconds = 1.0;
    if (streams_max < 1 || streams_max > MAX_STREAMS) streams_max = 4;
    QueryPerformanceFrequency(&qpf);
    a = chmlink_open((uint32_t)strtoul(argv[1], NULL, 10)); REQUIRE(a != NULL);
    REQUIRE(chmlink_query(a, &info));
    g_eth = info.mode == CHMLINK_MODE_ETHERNET;
    g_split = !g_eth && info.mtu > SPLIT_MTU;
    {
        char text[8];
        int whole = GetEnvironmentVariableA("CHMLINK_BENCH_NO_SPLIT", text, sizeof(text)) != 0 && text[0] == '1';
        int gro = GetEnvironmentVariableA("CHMLINK_BENCH_GRO", text, sizeof(text)) != 0 && text[0] == '1';
        if (g_eth) strcpy(g_prefix, "eth_");
        else if (g_split && whole) { g_split = 0; sprintf(g_prefix, "mtu%u_whole_", info.mtu); }
        else if (g_split && gro) { g_want_gro = 1; sprintf(g_prefix, "mtu%u_gro_", info.mtu); }
        else if (g_split) sprintf(g_prefix, "mtu%u_", info.mtu);
    }
    memcpy(g_mac, info.mac, 6);
    memset(&r, 0, sizeof(r));
    r.adapter = a; r.limit = info.packet_limit;
    r.fallback = malloc(CHMLINK_BATCH_MAX_SIZE);
    r.scratch = malloc(128 * 1024);
    REQUIRE(r.fallback != NULL && r.scratch != NULL);
    thread = CreateThread(NULL, 0, reflector_thread, &r, 0, NULL); REQUIRE(thread != NULL);
    /* Where the adapter offers offloads (Ethernet mode), plain packets first and
     * then every offload, in the same session and VM state; otherwise one plain run. */
    {
        uint32_t granted = 0, offered;
        int m;
        REQUIRE(chmlink_set_offload(a, CHMLINK_OFFLOAD_ALL, &offered));
        /* Plain, then receive coalescing done by the SDK (Windows sends plain
         * frames, the reflector merges them), then every offload. */
        for (m = offered ? 0 : 2; m < 3; ++m) {
            uint32_t requested = m == 2 ? offered : m == 1 ? offered & (CHMLINK_OFFLOAD_RX_RSC | CHMLINK_OFFLOAD_RX_CHECKSUM) : 0;
            const char *suffix = !offered ? "" : m == 0 ? "_nooffload" : m == 1 ? "_gro" : "";
            if (m == 1 && !(offered & CHMLINK_OFFLOAD_RX_RSC)) continue;
            if (offered) InterlockedExchange(&g_want_gro, 0);
            REQUIRE(chmlink_set_offload(a, requested, &granted));
            if (offered && m == 1) InterlockedExchange(&g_want_gro, 1);
            printf("{\"event\":\"offload\",\"requested\":%u,\"granted\":%u}\n", requested, granted);
            fflush(stdout);
            Sleep(1500);   /* let the stack apply the new offload indication */
            run(a, &info, 1, seconds, suffix);
            if (streams_max > 1) run(a, &info, streams_max, seconds, suffix);
        }
    }
    InterlockedExchange(&stop_reflector, 1);
    REQUIRE(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    printf("{\"event\":\"tcp_done\",\"rewrite_misses\":%lld,\"verified_packets\":%lld,"
           "\"verified_segments\":%lld,\"verify_failures\":%lld}\n", (long long)rewrite_misses,
           (long long)verified_packets, (long long)verified_segments, (long long)verify_failures);
    fflush(stdout);
    REQUIRE(verify_failures == 0);
    free(r.fallback); free(r.scratch);
    chmlink_close(a);
    WSACleanup();
    return 0;
}
