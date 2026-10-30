#ifndef CHMLINK_RING_H
#define CHMLINK_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Driver-internal byte FIFO for packets waiting to be read by user mode.
 * Portable and lock-free by itself: the caller serializes every call (the
 * driver uses the adapter spin lock). Exposed here only so it can be tested.
 *
 * Each record uses the protocol.h batch record layout: little-endian uint32
 * length, packet bytes, zero padding to four bytes. A record never wraps; when
 * one does not fit before the end, the writer stores CHMLINK_RING_WRAP and
 * continues at offset zero. Writers stage packets in a transaction so that a
 * multi-packet send is published all at once or not at all. */
#define CHMLINK_RING_WRAP 0xffffffffu

typedef struct chmlink_ring {
    uint8_t *data;
    uint32_t size;          /* bytes; multiple of four */
    uint32_t max_packets;
    uint32_t head, tail;    /* byte offsets of the oldest record / next write */
    uint32_t used;          /* bytes between head and tail, wrap padding included */
    uint32_t count;         /* published packets */
} chmlink_ring;

typedef struct chmlink_ring_txn {
    uint32_t tail, used, count;
} chmlink_ring_txn;

/* Returns zero for an unusable configuration (size not a multiple of four or
 * below eight bytes, or zero max_packets). */
int chmlink_ring_init(chmlink_ring *ring, uint8_t *data, uint32_t size, uint32_t max_packets);

/* Discards every published packet and returns how many were discarded. */
uint32_t chmlink_ring_clear(chmlink_ring *ring);

/* Stage packets: reserve returns space for exactly length bytes, or NULL when
 * the ring (bytes or packet count) is full. Staged packets stay invisible to
 * the reader until commit; dropping a transaction without commit discards
 * them. No other ring call may run between begin and commit. */
void chmlink_ring_begin(const chmlink_ring *ring, chmlink_ring_txn *txn);
uint8_t *chmlink_ring_reserve(const chmlink_ring *ring, chmlink_ring_txn *txn, uint32_t length);
void chmlink_ring_commit(chmlink_ring *ring, const chmlink_ring_txn *txn);

/* Oldest packet, or NULL when empty. The pointer stays valid until pop or
 * clear. pop removes the packet front returned. */
const uint8_t *chmlink_ring_front(chmlink_ring *ring, uint32_t *length);
void chmlink_ring_pop(chmlink_ring *ring);

#ifdef __cplusplus
}
#endif

#endif
