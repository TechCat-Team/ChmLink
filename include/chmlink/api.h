#ifndef CHMLINK_API_H
#define CHMLINK_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental synchronous C API. Errors use Windows GetLastError().
 * One open session per adapter. Calls may run on different threads, but the
 * caller must finish all operations before closing the adapter.
 * These functions do not create devices or modify IP addresses/routes.
 */
typedef struct chmlink_adapter chmlink_adapter;

chmlink_adapter *chmlink_open(uint32_t interface_index);
void chmlink_close(chmlink_adapter *adapter);

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
