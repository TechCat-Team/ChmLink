#include "chmlink/session.h"
#include "chmlink/ring.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    chmlink_session_rings r, got;
    uint8_t bytes[CHMLINK_SESSION_SIZE], copy[CHMLINK_SESSION_SIZE];
    size_t i;
    memset(&r, 0, sizeof(r));
    r.version = CHMLINK_SESSION_VERSION;
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
    r.flags = 1; chmlink_session_encode(bytes, &r); CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes)) != CHMLINK_OK);
    r.flags = 0; r.to_windows_ring = 0x7ff000000020ull; chmlink_session_encode(bytes, &r);
    CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes)) != CHMLINK_OK);
    r.to_windows_ring = 0x7ff000000000ull; r.from_windows_event = 0; chmlink_session_encode(bytes, &r);
    CHECK(chmlink_session_decode(&got, bytes, sizeof(bytes)) != CHMLINK_OK);
    puts("session ok");
    return 0;
}
