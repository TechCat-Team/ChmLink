#ifndef CHMLINK_RING_H
#define CHMLINK_RING_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Packet ring in memory shared by an application and the driver. One side
 * produces, the other consumes; each side serialises its own threads.
 *
 * Layout: a CHMLINK_RING_HEADER_SIZE-byte header, then `capacity` data bytes.
 * The header fields are 32-bit, each on its own 64-byte line:
 *   head  - consumer position, written by the consumer
 *   tail  - producer position, written by the producer
 *   wait  - non-zero while the consumer is about to sleep on its event
 *   state - CHMLINK_RING_STATE_* bits, written by the driver
 * Positions are byte offsets in [0, capacity), multiples of 16.
 *
 * A record is a CHMLINK_RING_RECORD_HEADER-byte header (uint32 packet size,
 * then a chmlink_offload descriptor in protocol.h layout) followed by the
 * packet, padded to 16 bytes. Records do not wrap: a producer that reaches the
 * end writes CHMLINK_RING_WRAP as the size and continues at offset zero. The
 * ring is empty when head == tail; a producer always leaves at least 16 bytes
 * free, so a full ring never looks empty.
 *
 * The reading side checks every value it reads from the other side before
 * using it; a value outside the rules makes the ring unusable
 * (CHMLINK_RING_CORRUPT) rather than causing an out-of-range access. */
#define CHMLINK_RING_HEADER_SIZE 256u
#define CHMLINK_RING_RECORD_HEADER 16u
#define CHMLINK_RING_ALIGN 16u
#define CHMLINK_RING_WRAP 0xffffffffu
#define CHMLINK_RING_MIN_CAPACITY (128u * 1024u)
#define CHMLINK_RING_MAX_CAPACITY (64u * 1024u * 1024u)
#define CHMLINK_RING_STATE_RUNNING 0x1u   /* the adapter moves packets */
#define CHMLINK_RING_STATE_CLOSED 0x2u    /* the session has ended; stop using the ring */

#define CHMLINK_RING_HEAD_OFFSET 0u
#define CHMLINK_RING_TAIL_OFFSET 64u
#define CHMLINK_RING_WAIT_OFFSET 128u
#define CHMLINK_RING_STATE_OFFSET 192u

typedef enum chmlink_ring_status {
    CHMLINK_RING_OK = 0,
    CHMLINK_RING_EMPTY,      /* consumer: nothing to read */
    CHMLINK_RING_FULL,       /* producer: no room for the record now */
    CHMLINK_RING_CORRUPT,    /* the other side broke the rules */
    CHMLINK_RING_INVALID     /* producer: a record of this size never fits; do not retry */
} chmlink_ring_status;

/* One side's view of a ring. `base` points at the header. */
typedef struct chmlink_ring {
    uint8_t *base;
    uint8_t *data;
    uint32_t capacity;
} chmlink_ring;

/* Bytes a record for `packet_size` packet bytes occupies. */
static __inline uint32_t chmlink_ring_record_size(uint32_t packet_size)
{
    return CHMLINK_RING_RECORD_HEADER + ((packet_size + CHMLINK_RING_ALIGN - 1) & ~(CHMLINK_RING_ALIGN - 1));
}

/* Valid capacity: a multiple of 16 within the limits. */
int chmlink_ring_capacity_valid(uint32_t capacity);

/* Attach to memory of CHMLINK_RING_HEADER_SIZE + capacity bytes. Does not
 * write; chmlink_ring_reset clears the header for a new session. */
int chmlink_ring_attach(chmlink_ring *ring, void *memory, uint32_t capacity);
void chmlink_ring_reset(chmlink_ring *ring);

/* Header field access with ordering: loads acquire, stores release. */
uint32_t chmlink_ring_load(const chmlink_ring *ring, uint32_t field_offset);
void chmlink_ring_store(chmlink_ring *ring, uint32_t field_offset, uint32_t value);

/* Producer. `cursor` is the producer's private write position (its last
 * published tail plus unpublished records). reserve places a record header for
 * `packet_size` bytes and returns where the packet bytes go; the record is
 * invisible until publish stores the cursor as tail. publish returns non-zero
 * when the consumer is waiting and should be woken. */
chmlink_ring_status chmlink_ring_reserve(chmlink_ring *ring, uint32_t *cursor,
    uint32_t packet_size, const chmlink_offload *offload, uint8_t **packet);
/* Replace the descriptor of a reserved, not yet published record, after the
 * producer has seen the packet bytes. */
void chmlink_ring_describe(uint8_t *packet, const chmlink_offload *offload);
int chmlink_ring_publish(chmlink_ring *ring, uint32_t cursor);

/* Consumer. `cursor` is the consumer's private read position (its last
 * released head plus records already read). next reads the record at the
 * cursor against `tail` (a value the caller loaded once) and advances the
 * cursor past it. The packet stays valid until release stores a head beyond
 * it. Offsets returned are from ring->data. */
chmlink_ring_status chmlink_ring_next(const chmlink_ring *ring, uint32_t *cursor, uint32_t tail,
    uint32_t packet_limit, uint32_t *packet_offset, uint32_t *packet_size, chmlink_offload *offload);
void chmlink_ring_release(chmlink_ring *ring, uint32_t head);

#ifdef __cplusplus
}
#endif

#endif
