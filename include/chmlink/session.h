#ifndef CHMLINK_SESSION_H
#define CHMLINK_SESSION_H

#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CHMLINK_IOCTL_REGISTER_RINGS input: CHMLINK_SESSION_SIZE little-endian
 * bytes, no output. version is the device contract the client speaks
 * (CHMLINK_DEVICE_VERSION of its headers); the driver accepts its range
 * CHMLINK_DEVICE_VERSION_MIN to CHMLINK_DEVICE_VERSION. Sent once on a data-session handle. The rings are memory
 * of the calling process (chmlink/ring.h layout); the events are handles of
 * the calling process with EVENT_MODIFY_STATE access.
 *   from_windows: the driver produces packets Windows sends; it sets the
 *     event when the ring's wait field is set.
 *   to_windows: the application produces packets for Windows; it sets the
 *     event when the ring's wait field is set (the driver is idle).
 * The driver keeps the memory locked and the events referenced until the
 * handle is closed. */
#define CHMLINK_SESSION_SIZE 56u

typedef struct chmlink_session_rings {
    uint32_t version;
    uint32_t flags;               /* zero */
    uint64_t from_windows_ring;   /* address of header + capacity bytes */
    uint32_t from_windows_capacity;
    uint64_t from_windows_event;
    uint64_t to_windows_ring;
    uint32_t to_windows_capacity;
    uint64_t to_windows_event;
} chmlink_session_rings;

void chmlink_session_encode(uint8_t *out, const chmlink_session_rings *rings);

/* Fails (CHMLINK_INVALID_ARGUMENT) for a wrong size, a contract version
 * outside this build's range, non-zero
 * flags or reserved bytes, a capacity chmlink_ring_capacity_valid rejects, a
 * null or misaligned ring address, or a null event. Leaves out untouched. */
chmlink_result chmlink_session_decode(chmlink_session_rings *out, const void *bytes, size_t length);

/* CHMLINK_IOCTL_ADD_INJECT_RING input: CHMLINK_INJECT_RING_SIZE little-endian
 * bytes, no output. After REGISTER_RINGS, on the same handle: one more
 * to-Windows ring with its own driver thread, so producers on several threads
 * are indicated in parallel. Order holds within a ring, not across rings. At
 * most CHMLINK_INJECT_RINGS_MAX to-Windows rings per session, the registered
 * one included; the same memory and event rules as REGISTER_RINGS. */
#define CHMLINK_INJECT_RING_VERSION 1u
#define CHMLINK_INJECT_RING_SIZE 24u
#define CHMLINK_INJECT_RINGS_MAX 4u

typedef struct chmlink_inject_ring {
    uint32_t version;
    uint32_t capacity;
    uint64_t ring;    /* address of header + capacity bytes */
    uint64_t event;
} chmlink_inject_ring;

void chmlink_inject_ring_encode(uint8_t *out, const chmlink_inject_ring *ring);

/* Fails (CHMLINK_INVALID_ARGUMENT) for a wrong size or version, a capacity
 * chmlink_ring_capacity_valid rejects, a null or misaligned ring address, or a
 * null event. Leaves out untouched. */
chmlink_result chmlink_inject_ring_decode(chmlink_inject_ring *out, const void *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
