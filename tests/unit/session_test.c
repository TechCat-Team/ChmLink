#include "chmlink/session.h"
#include "chmlink/ring.h"
#include "chmlink/version.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    chmlink_session_rings r, got;
    uint8_t bytes[CHMLINK_SESSION_SIZE], copy[CHMLINK_SESSION_SIZE];
    size_t i;
    memset(&r, 0, sizeof(r));
    r.version = CHMLINK_DEVICE_VERSION;
    r.from_windows_ring = 0x10000040ull; r.from_windows_capacity = CHMLINK_RING_MIN_CAPACITY; r.from_windows_event = 0x1234;
    r.to_windows_ring = 0x7ff000000000ull; r.to_windows_capacity = CHMLINK_RING_MAX_CAPACITY; r.to_windows_event = 0x5678;
    chmlink_session_encode(bytes, &r);
    CHECK(bytes[8] == 0x40 && bytes[11] == 0x10 && bytes[16] == 0x00 && bytes[18] == 0x02);
    CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes)) == CHMLINK_OK);
    CHECK(got.to_windows_ring == r.to_windows_ring && got.from_windows_event == 0x1234 &&
          got.to_windows_capacity == CHMLINK_RING_MAX_CAPACITY);
    CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes) - 1) == CHMLINK_INVALID_ARGUMENT);
    /* Every single-byte change of a checked field is refused or decodes to a different value. */
    for (i = 0; i < sizeof(bytes); ++i) {
        memcpy(copy, bytes, sizeof(bytes));
        copy[i] ^= 0x01;
        if (chmlink_session_decode(&got, copy, sizeof(copy)) == CHMLINK_OK) {
            CHECK(memcmp(&got, &r, sizeof(r)) != 0);
        }
    }
    /* The driver takes the contracts of its range only. */
    r.version = CHMLINK_DEVICE_VERSION_MIN - 1; chmlink_session_encode(bytes, &r);
    CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes)) != CHMLINK_OK);
    r.version = CHMLINK_DEVICE_VERSION + 1; chmlink_session_encode(bytes, &r);
    CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes)) != CHMLINK_OK);
    r.version = CHMLINK_DEVICE_VERSION;
    r.flags = 1; chmlink_session_encode(bytes, &r); CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes)) != CHMLINK_OK);
    r.flags = 0; r.to_windows_ring = 0x7ff000000020ull; chmlink_session_encode(bytes, &r);
    CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes)) != CHMLINK_OK);
    r.to_windows_ring = 0x7ff000000000ull; r.from_windows_event = 0; chmlink_session_encode(bytes, &r);
    CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes)) != CHMLINK_OK);
    {
        chmlink_inject_ring q, back;
        uint8_t raw[CHMLINK_INJECT_RING_SIZE], bad[CHMLINK_INJECT_RING_SIZE];
        size_t k;
        memset(&q, 0, sizeof(q));
        q.version = CHMLINK_INJECT_RING_VERSION; q.capacity = 128 * 1024;
        q.ring = 0x7ff000100000ull; q.event = 0x2c4;
        chmlink_inject_ring_encode(raw, &q);
        CHECK(chmlink_inject_ring_decode(&back, raw, sizeof(raw)) == CHMLINK_OK);
        CHECK(back.version == q.version && back.capacity == q.capacity && back.ring == q.ring && back.event == q.event);
        CHECK(chmlink_inject_ring_decode(&back, raw, sizeof(raw) - 1) == CHMLINK_INVALID_ARGUMENT);
        CHECK(chmlink_inject_ring_decode(NULL, raw, sizeof(raw)) == CHMLINK_INVALID_ARGUMENT);
        for (k = 0; k < sizeof(raw); ++k) {   /* every single-bit change is either refused or visible */
            memcpy(bad, raw, sizeof(raw)); bad[k] ^= 0x01;
            if (chmlink_inject_ring_decode(&back, bad, sizeof(bad)) == CHMLINK_OK)
                CHECK(back.capacity != q.capacity || back.ring != q.ring || back.event != q.event);
        }
        q.capacity = 1000; chmlink_inject_ring_encode(raw, &q);
        CHECK(chmlink_inject_ring_decode(&back, raw, sizeof(raw)) != CHMLINK_OK);
        q.capacity = 128 * 1024; q.ring += 32; chmlink_inject_ring_encode(raw, &q);
        CHECK(chmlink_inject_ring_decode(&back, raw, sizeof(raw)) != CHMLINK_OK);
        q.ring -= 32; q.event = 0; chmlink_inject_ring_encode(raw, &q);
        CHECK(chmlink_inject_ring_decode(&back, raw, sizeof(raw)) != CHMLINK_OK);
        q.event = 0x2c4; q.version = 2; chmlink_inject_ring_encode(raw, &q);
        CHECK(chmlink_inject_ring_decode(&back, raw, sizeof(raw)) != CHMLINK_OK);
    }
    puts("session ok");
    return 0;
}
