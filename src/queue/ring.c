#include "ring.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

/* Bytes one record occupies. Callers keep length far below 4 GiB. */
static uint32_t record_size(uint32_t length)
{
    return 4u + ((length + 3u) & ~3u);
}

static void reset(chmlink_ring *ring)
{
    ring->head = ring->tail = ring->used = ring->count = 0;
}

int chmlink_ring_init(chmlink_ring *ring, uint8_t *data, uint32_t size, uint32_t max_packets)
{
    if (ring == NULL || data == NULL || size < 8 || (size & 3u) || max_packets == 0) return 0;
    ring->data = data;
    ring->size = size;
    ring->max_packets = max_packets;
    reset(ring);
    return 1;
}

uint32_t chmlink_ring_clear(chmlink_ring *ring)
{
    uint32_t dropped = ring->count;
    reset(ring);
    return dropped;
}

void chmlink_ring_begin(const chmlink_ring *ring, chmlink_ring_txn *txn)
{
    txn->tail = ring->tail;
    txn->used = ring->used;
    txn->count = ring->count;
}

uint8_t *chmlink_ring_reserve(const chmlink_ring *ring, chmlink_ring_txn *txn, uint32_t length)
{
    uint32_t need, free_bytes;
    uint8_t *record;

    if (length > ring->size) return NULL;
    need = record_size(length);
    free_bytes = ring->size - txn->used;
    if (need > free_bytes || txn->count >= ring->max_packets) return NULL;
    /* Behind head (tail < head) the free run is [tail, head), already known to
     * hold need bytes. Otherwise free space is split into [tail, size) and
     * [0, head); a record that does not fit at the end goes to offset zero. */
    if (ring->size - txn->tail < need) {
        uint32_t waste = ring->size - txn->tail;
        if (need > ring->head) return NULL;
        wr32(ring->data + txn->tail, CHMLINK_RING_WRAP);
        txn->used += waste;
        txn->tail = 0;
    }
    record = ring->data + txn->tail;
    wr32(record, length);
    memset(record + 4 + length, 0, need - 4 - length);
    txn->tail += need;
    if (txn->tail == ring->size) txn->tail = 0;
    txn->used += need;
    txn->count++;
    return record + 4;
}

void chmlink_ring_commit(chmlink_ring *ring, const chmlink_ring_txn *txn)
{
    ring->tail = txn->tail;
    ring->used = txn->used;
    ring->count = txn->count;
}

const uint8_t *chmlink_ring_front(chmlink_ring *ring, uint32_t *length)
{
    while (ring->count != 0) {
        uint32_t value = rd32(ring->data + ring->head);
        if (value == CHMLINK_RING_WRAP) {
            ring->used -= ring->size - ring->head;
            ring->head = 0;
            continue;
        }
        *length = value;
        return ring->data + ring->head + 4;
    }
    return NULL;
}

void chmlink_ring_pop(chmlink_ring *ring)
{
    uint32_t need = record_size(rd32(ring->data + ring->head));
    ring->head += need;
    if (ring->head == ring->size) ring->head = 0;
    ring->used -= need;
    ring->count--;
    /* Only wrap padding can remain once the last packet is gone; restart at
     * offset zero so the next records get the longest contiguous run. */
    if (ring->count == 0) reset(ring);
}
