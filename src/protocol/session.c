#include "chmlink/session.h"
#include "chmlink/version.h"
#include "chmlink/ring.h"
#include <string.h>

/* Byte layout: 0 version, 4 flags, 8 from ring, 16 from capacity, 20 reserved,
 * 24 from event, 32 to ring, 40 to capacity, 44 reserved, 48 to event. */
static void put32(uint8_t *p, uint32_t v) { int i; for (i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { int i; for (i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; int i; for (i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; int i; for (i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

void chmlink_session_encode(uint8_t *out, const chmlink_session_rings *r)
{
    memset(out, 0, CHMLINK_SESSION_SIZE);
    put32(out, r->version);
    put32(out + 4, r->flags);
    put64(out + 8, r->from_windows_ring);
    put32(out + 16, r->from_windows_capacity);
    put64(out + 24, r->from_windows_event);
    put64(out + 32, r->to_windows_ring);
    put32(out + 40, r->to_windows_capacity);
    put64(out + 48, r->to_windows_event);
}

chmlink_result chmlink_session_decode(chmlink_session_rings *out, const void *bytes, size_t length)
{
    const uint8_t *p = (const uint8_t *)bytes;
    chmlink_session_rings r;
    if (out == NULL || p == NULL || length != CHMLINK_SESSION_SIZE) return CHMLINK_INVALID_ARGUMENT;
    r.version = get32(p);
    r.flags = get32(p + 4);
    r.from_windows_ring = get64(p + 8);
    r.from_windows_capacity = get32(p + 16);
    r.from_windows_event = get64(p + 24);
    r.to_windows_ring = get64(p + 32);
    r.to_windows_capacity = get32(p + 40);
    r.to_windows_event = get64(p + 48);
    if (r.version < CHMLINK_DEVICE_VERSION_MIN || r.version > CHMLINK_DEVICE_VERSION || r.flags != 0 || get32(p + 20) != 0 || get32(p + 44) != 0 ||
        !chmlink_ring_capacity_valid(r.from_windows_capacity) || !chmlink_ring_capacity_valid(r.to_windows_capacity) ||
        r.from_windows_ring == 0 || r.to_windows_ring == 0 || (r.from_windows_ring % 64) != 0 ||
        (r.to_windows_ring % 64) != 0 || r.from_windows_event == 0 || r.to_windows_event == 0) {
        return CHMLINK_INVALID_ARGUMENT;
    }
    *out = r;
    return CHMLINK_OK;
}

/* Byte layout: 0 version, 4 capacity, 8 ring, 16 event. */
void chmlink_inject_ring_encode(uint8_t *out, const chmlink_inject_ring *r)
{
    memset(out, 0, CHMLINK_INJECT_RING_SIZE);
    put32(out, r->version);
    put32(out + 4, r->capacity);
    put64(out + 8, r->ring);
    put64(out + 16, r->event);
}

chmlink_result chmlink_inject_ring_decode(chmlink_inject_ring *out, const void *bytes, size_t length)
{
    const uint8_t *p = (const uint8_t *)bytes;
    chmlink_inject_ring r;
    if (out == NULL || p == NULL || length != CHMLINK_INJECT_RING_SIZE) return CHMLINK_INVALID_ARGUMENT;
    r.version = get32(p);
    r.capacity = get32(p + 4);
    r.ring = get64(p + 8);
    r.event = get64(p + 16);
    if (r.version != CHMLINK_INJECT_RING_VERSION || !chmlink_ring_capacity_valid(r.capacity) ||
        r.ring == 0 || (r.ring % 64) != 0 || r.event == 0)
        return CHMLINK_INVALID_ARGUMENT;
    *out = r;
    return CHMLINK_OK;
}
