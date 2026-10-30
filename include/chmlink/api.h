#ifndef CHMLINK_API_H
#define CHMLINK_API_H

#include <stdint.h>
#include "version.h"
#include "packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental synchronous C API. Errors use Windows GetLastError().
 * One open session per adapter. Calls may run on different threads, but the
 * caller must prevent new calls before closing, and close exactly once.
 * Close cancels an existing wait, drains calls already in progress, then
 * destroys the handle. The pointer must never be used after close returns.
 * These functions do not create devices or modify IP addresses/routes.
 */
typedef struct chmlink_adapter chmlink_adapter;

/* SDK-local structure, never copied across the kernel boundary. interface_id
 * is a brace-delimited Windows interface GUID, stable for that installed
 * adapter. Interface indices and LUIDs can change across system restarts.
 * Removing/recreating a device can assign a new GUID.
 */
typedef struct chmlink_adapter_info {
    uint32_t version, capabilities, state, mode;
    uint32_t interface_index, mtu, packet_limit, queue_capacity, queued_packets;
    uint32_t packet_filter;
    uint8_t mac[6]; /* Ethernet MAC; all zero for IP mode. */
    char interface_id[39];
    /* TX means Windows -> user mode; RX means user mode -> Windows.
     * Packet/byte totals count accepted packets, not peer delivery. tx_errors
     * counts failed send NBLs. tx_queue_drops counts packets rejected by a
     * full queue or flushed when the session closes / adapter pauses.
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
chmlink_adapter *chmlink_open_by_id(const char *interface_id);

chmlink_adapter *chmlink_open(uint32_t interface_index);
void chmlink_close(chmlink_adapter *adapter);

#define CHMLINK_WAIT_INFINITE 0xffffffffu
/* Wait for readability without consuming data. One waiter per adapter;
 * concurrent waits fail with ERROR_BUSY. A competing receiver may drain data
 * before the caller receives; ERROR_NO_MORE_ITEMS remains valid.
 * Zero polls readiness, INFINITE has no deadline. Failure uses GetLastError:
 * ERROR_TIMEOUT, ERROR_OPERATION_ABORTED (cancel/close), ERROR_NOT_READY
 * (paused), device-removal errors, or ERROR_NOT_SUPPORTED on older drivers.
 * Cancel affects the current wait only; later waits start uncancelled.
 * Readiness and cancellation can race; an already completed wait may succeed.
 */
int chmlink_wait_readable(chmlink_adapter *adapter, uint32_t timeout_ms);
int chmlink_cancel_wait(chmlink_adapter *adapter);

/* Receive a protocol.h batch. ERROR_NO_MORE_ITEMS means the queue is empty.
 * A buffer of CHMLINK_BATCH_MAX_SIZE can hold the entire bounded queue.
 * A smaller buffer may return a partial batch; an oversized first packet
 * returns ERROR_INSUFFICIENT_BUFFER and is left queued for a larger buffer.
 */
int chmlink_receive(chmlink_adapter *adapter, void *buffer, uint32_t capacity,
                    uint32_t *bytes_received);
int chmlink_inject(chmlink_adapter *adapter, const void *batch, uint32_t length);

#ifdef __cplusplus
}
#endif
#endif
