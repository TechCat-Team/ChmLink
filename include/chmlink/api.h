#ifndef CHMLINK_API_H
#define CHMLINK_API_H

#include <stdint.h>
#include "version.h"
#include "packet.h"
#include "protocol.h"
#include "offload.h"
#ifdef _WIN32
#include "manage.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental C API. Errors use Windows GetLastError().
 * One open session per adapter. Calls may run on different threads, but the
 * caller must prevent new calls before closing, and close exactly once.
 * Close cancels an existing wait, drains calls already in progress, closes
 * the device (the driver then unlocks the ring pages), and finally releases
 * the rings and events. The pointer must never be used after close returns.
 * These functions do not modify IP addresses or routes; chmlink/manage.h
 * (included below) creates and deletes adapters.
 *
 * Packets move through two shared-memory rings (chmlink/ring.h) that the SDK
 * allocates and the driver locks. chmlink_receive_packet and
 * chmlink_allocate_packet are zero copy; chmlink_receive and chmlink_inject
 * remain batch-shaped conveniences for callers that want one buffer.
 */
typedef struct chmlink_adapter chmlink_adapter;

/* SDK-local structure, never copied across the kernel boundary. interface_id
 * is a brace-delimited Windows interface GUID, stable for that installed
 * adapter. Interface indices and LUIDs can change across system restarts.
 * Removing/recreating a device can assign a new GUID.
 */
typedef struct chmlink_adapter_info {
    uint32_t version, capabilities, state, mode;
    uint32_t interface_index, mtu, packet_limit, ring_capacity, ring_pending_bytes;
    uint32_t packet_filter;
    uint32_t offload; /* CHMLINK_OFFLOAD_* bits in effect for this session */
    uint8_t mac[6]; /* Ethernet MAC; all zero for IP mode. */
    char interface_id[39];
    /* TX means Windows -> user mode; RX means user mode -> Windows.
     * Packet/byte totals count accepted packets, not peer delivery. tx_errors
     * counts failed send NBLs. tx_queue_drops counts packets rejected by a
     * full ring or flushed when the session closes / adapter pauses.
     * For large send packets, tx_packets counts the TCP segments the packet
     * stands for, as Windows does; tx_bytes counts the wire bytes handed over.
     */
    uint64_t tx_packets, tx_bytes, tx_errors;
    uint64_t rx_packets, rx_bytes, rx_discards, tx_queue_drops;
} chmlink_adapter_info;

/* Enumeration is a snapshot. Callback data is valid only during the call;
 * return zero to stop successfully. Callbacks may open a separate session.
 * Inspection does not take ownership of or connect the data session.
 * The device ACL still requires administrator/SYSTEM access. Errors leave
 * info untouched. The callback may run before a later enumeration failure.
 */
typedef int (*chmlink_enum_callback)(const chmlink_adapter_info *info, void *context);
int chmlink_enumerate(chmlink_enum_callback callback, void *context);
int chmlink_inspect(uint32_t interface_index, chmlink_adapter_info *info);
int chmlink_query(chmlink_adapter *adapter, chmlink_adapter_info *info);

/* Session configuration. size must be sizeof(chmlink_open_options); a zero
 * capacity means the default 4 MiB per ring. Capacities must satisfy
 * chmlink_ring_capacity_valid; smaller or larger values fail with
 * ERROR_INVALID_PARAMETER. */
typedef struct chmlink_open_options {
    uint32_t size;
    uint32_t ring_capacity;
} chmlink_open_options;

chmlink_adapter *chmlink_open(uint32_t interface_index);
chmlink_adapter *chmlink_open_ex(uint32_t interface_index, const chmlink_open_options *options);
chmlink_adapter *chmlink_open_by_id(const char *interface_id);
void chmlink_close(chmlink_adapter *adapter);

#define CHMLINK_WAIT_INFINITE 0xffffffffu
/* Wait for readability without consuming data. One waiter per adapter;
 * concurrent waits fail with ERROR_BUSY. A competing receiver may drain data
 * before the caller receives; ERROR_NO_MORE_ITEMS remains valid.
 * Zero polls readiness, INFINITE has no deadline. Failure uses GetLastError:
 * ERROR_TIMEOUT, ERROR_OPERATION_ABORTED (cancel/close, or the session ended),
 * ERROR_NOT_READY (paused, including system sleep: CHMLINK_STATE_LOW_POWER),
 * device-removal errors, ERROR_SHUTDOWN_IN_PROGRESS.
 * Cancel affects the current wait only; later waits start uncancelled.
 * Readiness and cancellation can race; an already completed wait may succeed.
 */
int chmlink_wait_readable(chmlink_adapter *adapter, uint32_t timeout_ms);
int chmlink_cancel_wait(chmlink_adapter *adapter);

/* The auto-reset event the driver signals when packets arrive or the ring
 * state changes. Do not close or modify it; use chmlink_wait_readable
 * instead unless integrating with another wait set. */
void *chmlink_get_read_event(chmlink_adapter *adapter);

/* Zero-copy receive. Returns a pointer into the ring, valid until the call to
 * chmlink_release_packet; NULL means ERROR_NO_MORE_ITEMS (empty) or another
 * error. offload may be NULL; it receives what the packet still needs
 * (CHMLINK_OFFLOAD_NEEDS_CSUM, GSO) when the session has offload enabled.
 * Packets may be released in any order; the driver is only told that memory
 * up to the last contiguously released packet is free again. */
const uint8_t *chmlink_receive_packet(chmlink_adapter *adapter, uint32_t *size,
                                      chmlink_offload *offload);
void chmlink_release_packet(chmlink_adapter *adapter, const uint8_t *packet);

/* Zero-copy send. chmlink_allocate_packet reserves exactly `size` bytes and
 * returns where to write them, or NULL with ERROR_BUFFER_OVERFLOW (ring full)
 * or ERROR_INVALID_PARAMETER. offload describes checksums the caller left
 * unfinished and GSO coalescing; NULL is a plain packet.
 * chmlink_send_packet publishes the reservation. Threads may allocate and
 * send concurrently; the SDK publishes in allocation order, so a caller that
 * allocates and then stalls delays later packets. */
uint8_t *chmlink_allocate_packet(chmlink_adapter *adapter, uint32_t size,
                                 const chmlink_offload *offload);
void chmlink_send_packet(chmlink_adapter *adapter, uint8_t *packet);

/* Receive one protocol.h batch copied out of the ring (plain, or an offload
 * batch when descriptors are present). ERROR_NO_MORE_ITEMS means the ring is
 * empty. One call returns at most CHMLINK_BATCH_MAX_PACKETS packets; keep
 * receiving until ERROR_NO_MORE_ITEMS. A smaller buffer may return a partial
 * batch; an oversized first packet returns ERROR_INSUFFICIENT_BUFFER and is
 * left unread. Use chmlink_batch_read_* to walk the result. */
int chmlink_receive(chmlink_adapter *adapter, void *buffer, uint32_t capacity,
                    uint32_t *bytes_received);
/* Copy a validated batch into the sending ring. Delivery to Windows is
 * asynchronous: the stack may not have seen the packets when this returns. */
int chmlink_inject(chmlink_adapter *adapter, const void *batch, uint32_t length);

/* Ask the driver for offloads (CHMLINK_OFFLOAD_* in version.h). granted may
 * be NULL and receives the subset the adapter and Windows allow. Refused
 * bits are not an error; invalid bits return ERROR_INVALID_PARAMETER. The
 * setting ends with the session. Ethernet mode only: Windows does not use task
 * offloads on IP-mode adapters, so those grant nothing (and QUERY reports no
 * CHMLINK_CAP_OFFLOAD). */
int chmlink_set_offload(chmlink_adapter *adapter, uint32_t requested, uint32_t *granted);

#ifdef __cplusplus
}
#endif
#endif
