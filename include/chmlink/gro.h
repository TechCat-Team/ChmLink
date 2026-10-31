#ifndef CHMLINK_GRO_H
#define CHMLINK_GRO_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Software GRO: consecutive TCP segments of one flow become one large packet,
 * so Windows processes a few big packets instead of many small ones, as a NIC
 * with receive segment coalescing does. Portable and allocation free; the SDK
 * wraps it for an adapter as chmlink_coalescer (api.h).
 *
 * Segments merge when they continue the flow exactly: same addresses, ports,
 * IP and TCP header fields (options included, so timestamps must match), the
 * next sequence number, only ACK and PSH set, a payload, and every segment but
 * the last as large as the first. IPv4 header checksums are verified. TCP
 * checksums are not recomputed over the payload: the merged checksum is
 * derived from the segments' checksum fields, so it is exactly as right as
 * they were. The merged packet stays within `limit` and 64 KiB of IP payload.
 * Output:
 *   - CHMLINK_MODE_IP: a complete IP packet with correct lengths and
 *     checksums, for an IP adapter whose MTU (the limit) is large;
 *     A corrupt segment makes the whole merged packet fail Windows' check;
 *   - CHMLINK_MODE_ETHERNET: one frame with a GSO descriptor (gso_size = the
 *     segment size, CHMLINK_OFFLOAD_DATA_VALID), for a session with
 *     CHMLINK_OFFLOAD_RX_RSC. Windows takes the data as verified, so each
 *     segment's TCP checksum is verified first (a segment that fails passes
 *     on untouched, for the stack to drop), unless CHMLINK_GRO_TRUSTED says
 *     the caller already knows the data is intact (say, a tunnel whose
 *     authenticated encryption protects it). Ethernet frames merge only
 *     without a VLAN tag.
 * Anything else, and a single segment that found no partner, comes out
 * unchanged, with no descriptor. Packets of one flow keep their order; flows
 * may overtake each other. Call chmlink_gro_flush when a burst of input ends
 * (before waiting for more): segments are held until then. */
#define CHMLINK_GRO_FLOWS 8u
#define CHMLINK_GRO_TRUSTED 0x1u   /* flags: skip payload checksum verification */
#define CHMLINK_GRO_MEMORY(limit) ((size_t)(limit) * CHMLINK_GRO_FLOWS)

/* packet is valid only during the call; offload is NULL for a plain packet. */
typedef void (*chmlink_gro_output)(const uint8_t *packet, size_t length, const chmlink_offload *offload,
                                   void *context);

typedef struct chmlink_gro_flow {
    uint8_t *buffer;
    size_t length;          /* 0: free */
    size_t l3, l4, payload; /* offsets in buffer */
    uint32_t next_seq;
    uint32_t payload_bytes;
    uint16_t payload_sum;   /* one's complement sum of the payload, as concatenated */
    uint16_t mss;
    uint32_t segments;
    uint32_t age;
} chmlink_gro_flow;

typedef struct chmlink_gro {
    chmlink_mode mode;
    size_t limit;
    chmlink_gro_output output;
    void *context;
    uint32_t clock;
    int verify;
    uint64_t packets_in, packets_out;   /* statistics */
    chmlink_gro_flow flows[CHMLINK_GRO_FLOWS];
} chmlink_gro;

/* limit: the largest packet to produce (the adapter's packet limit); flags:
 * CHMLINK_GRO_*; memory: CHMLINK_GRO_MEMORY(limit) bytes that stay valid while
 * gro is used. */
chmlink_result chmlink_gro_init(chmlink_gro *gro, chmlink_mode mode, size_t limit, uint32_t flags, uint8_t *memory,
                                size_t memory_size, chmlink_gro_output output, void *context);
/* Take one packet; outputs zero or more packets. */
chmlink_result chmlink_gro_add(chmlink_gro *gro, const uint8_t *packet, size_t length);
/* Output everything held. */
void chmlink_gro_flush(chmlink_gro *gro);

#ifdef __cplusplus
}
#endif
#endif
