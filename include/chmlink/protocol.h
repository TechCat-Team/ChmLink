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
    CHMLINK_INVALID_QUERY
} chmlink_result;

typedef struct chmlink_batch_writer {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    size_t packet_limit;
    uint32_t count;
    chmlink_mode mode;
} chmlink_batch_writer;

/* Failure leaves the output writer and buffer untouched. An initialized empty
 * batch cannot be submitted. packet_limit is the adapter's negotiated limit.
 */
chmlink_result chmlink_batch_begin(chmlink_batch_writer *writer,
    void *buffer, size_t capacity, chmlink_mode mode, size_t packet_limit);

/* Failure leaves the batch untouched. The source may overlap the buffer. */
chmlink_result chmlink_batch_append(chmlink_batch_writer *writer,
    const uint8_t *packet, size_t length);

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
