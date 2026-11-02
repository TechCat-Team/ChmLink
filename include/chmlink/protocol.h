#ifndef CHMLINK_PROTOCOL_H
#define CHMLINK_PROTOCOL_H

#include "packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private, experimental transport format. Not a stable public SDK ABI yet.
 * All integers are little-endian uint32 values, never native C structures.
 * Header: version, mode, packet count, total bytes, flags, reserved.
 * Records: packet length, packet bytes, zero padding to a four-byte boundary.
 * Batches contain one or more packets, all belonging to the adapter's mode.
 *
 * Offload batches (header flag CHMLINK_BATCH_FLAG_OFFLOAD) put a
 * CHMLINK_OFFLOAD_SIZE-byte descriptor between each record's length and its
 * packet bytes; the length still counts the packet only. The descriptor has the
 * layout and meaning of Linux's struct virtio_net_hdr: flags (u8), gso_type
 * (u8), hdr_len, gso_size, csum_start, csum_offset (little-endian u16) and a
 * reserved u16 that must be zero. Offsets are from the start of the packet.
 *   NEEDS_CSUM: the L4 checksum at csum_start + csum_offset is not computed;
 *     its content is unspecified and the consumer must compute it in full.
 *   DATA_VALID: the checksums have been verified (injection only).
 *   gso_type TCPV4/TCPV6: a TCP packet larger than the MTU to be cut into
 *     gso_size-byte payload segments behind hdr_len header bytes (Windows ->
 *     user mode), or a coalesced segment of that many (user mode -> Windows).
 *     Its IP length fields and IPv4 header checksum are valid for the whole
 *     packet, it may be up to CHMLINK_PACKET_MAX_SIZE bytes, csum_start is
 *     the TCP header and csum_offset 16.
 */
#define CHMLINK_PROTOCOL_VERSION 1u
#define CHMLINK_BATCH_HEADER_SIZE 24u
#define CHMLINK_BATCH_MAX_SIZE 1048576u
#define CHMLINK_BATCH_MAX_PACKETS 256u
#define CHMLINK_PACKET_MAX_SIZE 65557u

typedef enum chmlink_result {
    CHMLINK_OK = 0,
    CHMLINK_INVALID_ARGUMENT,
    CHMLINK_UNSUPPORTED_VERSION,
    CHMLINK_MODE_MISMATCH,
    CHMLINK_INVALID_BATCH,
    CHMLINK_INVALID_PACKET,
    CHMLINK_BUFFER_TOO_SMALL,
    CHMLINK_INVALID_QUERY,
    CHMLINK_TOO_BIG          /* larger than allowed and must not be fragmented */
} chmlink_result;

#define CHMLINK_BATCH_FLAG_OFFLOAD 0x1u
#define CHMLINK_OFFLOAD_SIZE 12u
#define CHMLINK_OFFLOAD_NEEDS_CSUM 0x01u
#define CHMLINK_OFFLOAD_DATA_VALID 0x02u
/* Never a real flag: the SDK marks abandoned records with it
 * (chmlink_abandon_packet), and drivers drop records with unknown flags. */
#define CHMLINK_OFFLOAD_DISCARD 0x80u
#define CHMLINK_GSO_NONE 0u
#define CHMLINK_GSO_TCPV4 1u
#define CHMLINK_GSO_TCPV6 4u
#define CHMLINK_GSO_ECN 0x80u

typedef struct chmlink_offload {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} chmlink_offload;

typedef struct chmlink_batch_writer {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    size_t packet_limit;
    uint32_t count;
    uint32_t flags;
    chmlink_mode mode;
} chmlink_batch_writer;

/* Sequential access to a validated batch, plain or offload. */
typedef struct chmlink_batch_reader {
    const uint8_t *buffer;
    size_t offset;
    uint32_t remaining;
    uint32_t flags;
} chmlink_batch_reader;

/* Failure leaves the output writer and buffer untouched. An initialized empty
 * batch cannot be submitted. packet_limit is the adapter's negotiated limit.
 */
chmlink_result chmlink_batch_begin(chmlink_batch_writer *writer,
    void *buffer, size_t capacity, chmlink_mode mode, size_t packet_limit);

/* As chmlink_batch_begin, for an offload batch. */
chmlink_result chmlink_batch_begin_offload(chmlink_batch_writer *writer,
    void *buffer, size_t capacity, chmlink_mode mode, size_t packet_limit);

/* Failure leaves the batch untouched. The source may overlap the buffer. In an
 * offload batch the packet gets an all-zero descriptor. */
chmlink_result chmlink_batch_append(chmlink_batch_writer *writer,
    const uint8_t *packet, size_t length);

/* Append with a descriptor (NULL = none). Plain batches accept only NULL or an
 * all-zero descriptor. A packet with a GSO type may exceed packet_limit, up
 * to CHMLINK_PACKET_MAX_SIZE. */
chmlink_result chmlink_batch_append_offload(chmlink_batch_writer *writer,
    const uint8_t *packet, size_t length, const chmlink_offload *offload);

/* Structural check of one descriptor against its packet, as the validator
 * applies it: known flags and types, offsets inside the packet, and for GSO a
 * TCP packet of the matching IP version whose header ends at hdr_len. */
int chmlink_offload_valid(chmlink_mode mode, const uint8_t *packet, size_t length,
    size_t packet_limit, const chmlink_offload *offload);

/* Iterate a batch that chmlink_batch_validate accepted. next returns 0 at the
 * end; offload may be NULL and is zeroed for plain batches. */
void chmlink_batch_read_begin(chmlink_batch_reader *reader, const void *buffer);
int chmlink_batch_read_next(chmlink_batch_reader *reader, const uint8_t **packet,
    uint32_t *length, chmlink_offload *offload);

/* Validate an entire batch before processing any packet. The input must be an
 * immutable snapshot owned by the caller, not live user-writable memory.
 * A driver must copy untrusted input first and retain that same snapshot while
 * consuming it. This validates framing, not complete network protocol content.
 * packet_count is optional and is set to zero on failure.
 */
chmlink_result chmlink_batch_validate(const void *buffer, size_t length,
    chmlink_mode expected_mode, size_t packet_limit, uint32_t *packet_count);

#ifdef __cplusplus
}
#endif

#endif
