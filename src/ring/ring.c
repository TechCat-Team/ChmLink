#include "chmlink/ring.h"
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
static uint32_t load_acquire(const volatile uint32_t *p) { return (uint32_t)_InterlockedOr((volatile long *)p, 0); }
static void store_release(volatile uint32_t *p, uint32_t v) { (void)_InterlockedExchange((volatile long *)p, (long)v); }
static uint32_t load_once(const volatile uint8_t *p) { return *(const volatile uint32_t *)p; }
#else
static uint32_t load_acquire(const volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store_release(volatile uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint32_t load_once(const volatile uint8_t *p) { return __atomic_load_n((const volatile uint32_t *)p, __ATOMIC_RELAXED); }
#endif

static volatile uint32_t *field(const chmlink_ring *ring, uint32_t offset)
{
    return (volatile uint32_t *)(ring->base + offset);
}

static int position_valid(const chmlink_ring *ring, uint32_t position)
{
    return position < ring->capacity && (position % CHMLINK_RING_ALIGN) == 0;
}

int chmlink_ring_capacity_valid(uint32_t capacity)
{
    return capacity >= CHMLINK_RING_MIN_CAPACITY && capacity <= CHMLINK_RING_MAX_CAPACITY &&
           (capacity % CHMLINK_RING_ALIGN) == 0;
}

int chmlink_ring_attach(chmlink_ring *ring, void *memory, uint32_t capacity)
{
    if (ring == NULL || memory == NULL || ((uintptr_t)memory % 64u) != 0 ||
        !chmlink_ring_capacity_valid(capacity)) return 0;
    ring->base = (uint8_t *)memory;
    ring->data = ring->base + CHMLINK_RING_HEADER_SIZE;
    ring->capacity = capacity;
    return 1;
}

void chmlink_ring_reset(chmlink_ring *ring)
{
    memset(ring->base, 0, CHMLINK_RING_HEADER_SIZE);
}

uint32_t chmlink_ring_load(const chmlink_ring *ring, uint32_t field_offset)
{
    return load_acquire(field(ring, field_offset));
}

void chmlink_ring_store(chmlink_ring *ring, uint32_t field_offset, uint32_t value)
{
    store_release(field(ring, field_offset), value);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void write_record_header(uint8_t *record, uint32_t size, const chmlink_offload *o)
{
    static const chmlink_offload none = {0, 0, 0, 0, 0, 0};
    if (o == NULL) o = &none;
    memcpy(record, &size, 4);
    record[4] = o->flags;
    record[5] = o->gso_type;
    put16(record + 6, o->hdr_len);
    put16(record + 8, o->gso_size);
    put16(record + 10, o->csum_start);
    put16(record + 12, o->csum_offset);
    put16(record + 14, 0);
}

void chmlink_ring_describe(uint8_t *packet, const chmlink_offload *o)
{
    static const chmlink_offload none = {0, 0, 0, 0, 0, 0};
    uint8_t *record = packet - CHMLINK_RING_RECORD_HEADER;
    if (o == NULL) o = &none;
    record[4] = o->flags;
    record[5] = o->gso_type;
    put16(record + 6, o->hdr_len);
    put16(record + 8, o->gso_size);
    put16(record + 10, o->csum_start);
    put16(record + 12, o->csum_offset);
    put16(record + 14, 0);
}

chmlink_ring_status chmlink_ring_reserve(chmlink_ring *ring, uint32_t *cursor,
    uint32_t packet_size, const chmlink_offload *offload, uint8_t **packet)
{
    uint32_t head = chmlink_ring_load(ring, CHMLINK_RING_HEAD_OFFSET), at = *cursor, need, place;
    if (!position_valid(ring, head)) return CHMLINK_RING_CORRUPT;
    if (packet_size == 0 || packet_size > CHMLINK_PACKET_MAX_SIZE) return CHMLINK_RING_INVALID;
    need = chmlink_ring_record_size(packet_size);
    if (need > ring->capacity - CHMLINK_RING_ALIGN) return CHMLINK_RING_INVALID;
    if (at >= head) {
        uint32_t room = ring->capacity - at;
        if (need < room || (need == room && head != 0)) {
            place = at;
        } else if (need < head) {
            /* Room at the start: mark the rest of the end as skipped. */
            uint32_t wrap = CHMLINK_RING_WRAP;
            memcpy(ring->data + at, &wrap, 4);
            place = 0;
        } else {
            return CHMLINK_RING_FULL;
        }
    } else {
        if (need >= head - at) return CHMLINK_RING_FULL;
        place = at;
    }
    write_record_header(ring->data + place, packet_size, offload);
    *packet = ring->data + place + CHMLINK_RING_RECORD_HEADER;
    place += need;
    *cursor = place == ring->capacity ? 0 : place;
    return CHMLINK_RING_OK;
}

int chmlink_ring_publish(chmlink_ring *ring, uint32_t cursor)
{
    chmlink_ring_store(ring, CHMLINK_RING_TAIL_OFFSET, cursor);
    return chmlink_ring_load(ring, CHMLINK_RING_WAIT_OFFSET) != 0;
}

chmlink_ring_status chmlink_ring_next(const chmlink_ring *ring, uint32_t *cursor, uint32_t tail,
    uint32_t packet_limit, uint32_t *packet_offset, uint32_t *packet_size, chmlink_offload *offload)
{
    uint32_t at = *cursor, end, size, need, limit;
    uint8_t descriptor[12];
    if (!position_valid(ring, tail)) return CHMLINK_RING_CORRUPT;
    if (at == tail) return CHMLINK_RING_EMPTY;
    end = tail > at ? tail : ring->capacity;
    size = load_once(ring->data + at);
    if (size == CHMLINK_RING_WRAP) {
        /* Only a producer that went back to the start leaves a marker. */
        if (tail > at) return CHMLINK_RING_CORRUPT;
        at = 0;
        if (at == tail) return CHMLINK_RING_CORRUPT;
        end = tail;
        size = load_once(ring->data);
    }
    memcpy(descriptor, ring->data + at + 4, sizeof(descriptor));
    if (get16(descriptor + 10) != 0) return CHMLINK_RING_CORRUPT;
    offload->flags = descriptor[0];
    offload->gso_type = descriptor[1];
    offload->hdr_len = get16(descriptor + 2);
    offload->gso_size = get16(descriptor + 4);
    offload->csum_start = get16(descriptor + 6);
    offload->csum_offset = get16(descriptor + 8);
    limit = offload->gso_type != CHMLINK_GSO_NONE ? CHMLINK_PACKET_MAX_SIZE : packet_limit;
    if (size == 0 || size > limit) return CHMLINK_RING_CORRUPT;
    need = chmlink_ring_record_size(size);
    if (need > end - at) return CHMLINK_RING_CORRUPT;
    *packet_offset = at + CHMLINK_RING_RECORD_HEADER;
    *packet_size = size;
    at += need;
    *cursor = at == ring->capacity ? 0 : at;
    return CHMLINK_RING_OK;
}

void chmlink_ring_release(chmlink_ring *ring, uint32_t head)
{
    chmlink_ring_store(ring, CHMLINK_RING_HEAD_OFFSET, head);
}
