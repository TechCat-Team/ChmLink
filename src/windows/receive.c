/* Packets from Windows: the from-Windows ring, read in place
 * (chmlink_receive_packet) or copied out as a batch (chmlink_receive). */
#include "internal.h"

static int rx_push(chmlink_adapter *a, uint32_t start, uint32_t end, BOOL released)
{
    if (a->rx_count == a->rx_entries_capacity) {
        if (a->rx_head != 0) {
            memmove(a->rx_entries, a->rx_entries + a->rx_head,
                    (a->rx_count - a->rx_head) * sizeof(*a->rx_entries));
            a->rx_count -= a->rx_head;
            a->rx_head = 0;
        } else {
            uint32_t capacity = a->rx_entries_capacity * 2;
            chmlink_rx_entry *grown;
            if (capacity > a->from_capacity / (CHMLINK_RING_RECORD_HEADER + 16u) + 16u)
                return 0;
            grown = HeapReAlloc(GetProcessHeap(), 0, a->rx_entries,
                                capacity * sizeof(*a->rx_entries));
            if (grown == NULL) return 0;
            a->rx_entries = grown;
            a->rx_entries_capacity = capacity;
        }
    }
    a->rx_entries[a->rx_count].start = start;
    a->rx_entries[a->rx_count].end = end;
    a->rx_entries[a->rx_count].released = released;
    a->rx_count++;
    return 1;
}

/* Publish the contiguous released prefix to the driver. */
static void rx_advance(chmlink_adapter *a)
{
    uint32_t end = 0;
    BOOL advanced = FALSE;
    while (a->rx_head < a->rx_count && a->rx_entries[a->rx_head].released) {
        end = a->rx_entries[a->rx_head].end;
        a->rx_head++;
        advanced = TRUE;
    }
    if (advanced) chmlink_ring_release(&a->from, end);
    if (a->rx_head == a->rx_count) a->rx_head = a->rx_count = 0;
}

int sdk_rx_pending(chmlink_adapter *a)
{
    int pending;
    uint32_t tail;
    AcquireSRWLockExclusive(&a->rx_lock);
    tail = chmlink_ring_load(&a->from, CHMLINK_RING_TAIL_OFFSET);
    pending = a->rx_cursor != tail;
    ReleaseSRWLockExclusive(&a->rx_lock);
    return pending;
}

const uint8_t *chmlink_receive_packet(chmlink_adapter *adapter, uint32_t *size,
                                      chmlink_offload *offload)
{
    chmlink_ring_status status;
    chmlink_offload descriptor;
    uint32_t offset = 0, length = 0, saved;
    const uint8_t *packet = NULL;
    if (size != NULL) *size = 0;
    if (offload != NULL) memset(offload, 0, sizeof(*offload));
    if (size == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!sdk_enter(adapter, NULL)) return NULL;
    AcquireSRWLockExclusive(&adapter->rx_lock);
    saved = adapter->rx_cursor;
    status = chmlink_ring_next(&adapter->from, &adapter->rx_cursor,
        chmlink_ring_load(&adapter->from, CHMLINK_RING_TAIL_OFFSET), adapter->packet_limit,
        &offset, &length, &descriptor);
    if (status == CHMLINK_RING_OK) {
        if (!rx_push(adapter, offset - CHMLINK_RING_RECORD_HEADER, adapter->rx_cursor, FALSE)) {
            adapter->rx_cursor = saved;
            status = CHMLINK_RING_FULL;
        } else {
            packet = adapter->from.data + offset;
            *size = length;
            if (offload != NULL) *offload = descriptor;
        }
    }
    ReleaseSRWLockExclusive(&adapter->rx_lock);
    if (packet == NULL) {
        DWORD error = status == CHMLINK_RING_EMPTY ? ERROR_NO_MORE_ITEMS :
                      status == CHMLINK_RING_FULL ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_DATA;
        sdk_leave(adapter, NULL, 0);
        SetLastError(error);
        return NULL;
    }
    sdk_leave(adapter, NULL, 1);
    return packet;
}

void chmlink_release_packet(chmlink_adapter *adapter, const uint8_t *packet)
{
    uint32_t start, i;
    if (adapter == NULL || packet == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return; }
    if ((uintptr_t)packet < (uintptr_t)(adapter->from.data + CHMLINK_RING_RECORD_HEADER) ||
        (uintptr_t)packet >= (uintptr_t)(adapter->from.data + adapter->from.capacity)) {
        SetLastError(ERROR_INVALID_PARAMETER); return;
    }
    if (!sdk_enter(adapter, NULL)) return;
    AcquireSRWLockExclusive(&adapter->rx_lock);
    start = (uint32_t)(packet - adapter->from.data) - CHMLINK_RING_RECORD_HEADER;
    for (i = adapter->rx_head; i < adapter->rx_count; ++i) {
        if (adapter->rx_entries[i].start == start && !adapter->rx_entries[i].released) {
            adapter->rx_entries[i].released = TRUE;
            rx_advance(adapter);
            break;
        }
    }
    if (adapter->rx_head > 256 && adapter->rx_head * 2 > adapter->rx_count) {
        memmove(adapter->rx_entries, adapter->rx_entries + adapter->rx_head,
                (adapter->rx_count - adapter->rx_head) * sizeof(*adapter->rx_entries));
        adapter->rx_count -= adapter->rx_head;
        adapter->rx_head = 0;
    }
    ReleaseSRWLockExclusive(&adapter->rx_lock);
    sdk_leave(adapter, NULL, 1);
}

int chmlink_receive(chmlink_adapter *adapter, void *buffer, uint32_t capacity,
                    uint32_t *bytes_received)
{
    chmlink_batch_writer writer = {0};
    uint32_t count = 0, probe, tail;
    BOOL offload_batch = FALSE;
    int result = 0;
    if (bytes_received != NULL) *bytes_received = 0;
    if (adapter == NULL || buffer == NULL || bytes_received == NULL ||
        capacity < CHMLINK_BATCH_HEADER_SIZE || capacity > CHMLINK_BATCH_MAX_SIZE) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    if (!sdk_enter(adapter, NULL)) return 0;
    AcquireSRWLockExclusive(&adapter->rx_lock);
    tail = chmlink_ring_load(&adapter->from, CHMLINK_RING_TAIL_OFFSET);
    if (adapter->rx_cursor == tail) {
        ReleaseSRWLockExclusive(&adapter->rx_lock);
        SetLastError(ERROR_NO_MORE_ITEMS);
        sdk_leave(adapter, NULL, 0);
        return 0;
    }
    /* Decide the batch kind before consuming anything: a plain batch must be
     * produced whenever no descriptor is in use. */
    probe = adapter->rx_cursor;
    while (count < CHMLINK_BATCH_MAX_PACKETS) {
        uint32_t offset, length;
        chmlink_offload descriptor;
        chmlink_ring_status status = chmlink_ring_next(&adapter->from, &probe, tail,
            adapter->packet_limit, &offset, &length, &descriptor);
        if (status == CHMLINK_RING_EMPTY) break;
        if (status == CHMLINK_RING_CORRUPT) {
            ReleaseSRWLockExclusive(&adapter->rx_lock);
            SetLastError(ERROR_INVALID_DATA); sdk_leave(adapter, NULL, 0); return 0;
        }
        if (descriptor.flags != 0 || descriptor.gso_type != 0) offload_batch = TRUE;
        count++;
    }
    if (offload_batch)
        chmlink_batch_begin_offload(&writer, buffer, capacity, adapter->mode, adapter->packet_limit);
    else
        chmlink_batch_begin(&writer, buffer, capacity, adapter->mode, adapter->packet_limit);
    count = 0;
    for (;;) {
        uint32_t offset = 0, length = 0, saved = adapter->rx_cursor, record;
        chmlink_offload descriptor;
        chmlink_ring_status status;
        if (count == CHMLINK_BATCH_MAX_PACKETS) break;
        status = chmlink_ring_next(&adapter->from, &adapter->rx_cursor, tail,
            adapter->packet_limit, &offset, &length, &descriptor);
        if (status == CHMLINK_RING_EMPTY) break;
        if (status == CHMLINK_RING_CORRUPT) {
            if (count == 0) { ReleaseSRWLockExclusive(&adapter->rx_lock); SetLastError(ERROR_INVALID_DATA);
                sdk_leave(adapter, NULL, 0); return 0; }
            break;
        }
        /* The record needs a length and maybe a descriptor, plus padding. */
        record = 4u + (offload_batch ? CHMLINK_OFFLOAD_SIZE : 0u) +
                 ((length + 3u) & ~3u);
        if ((size_t)writer.length + record > capacity) {
            adapter->rx_cursor = saved;
            if (count == 0) {
                ReleaseSRWLockExclusive(&adapter->rx_lock);
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                sdk_leave(adapter, NULL, 0);
                return 0;
            }
            break;
        }
        chmlink_batch_writer snapshot = writer;
        if (chmlink_batch_append_offload(&writer, adapter->from.data + offset, length,
                offload_batch ? &descriptor : NULL) != CHMLINK_OK ||
            !rx_push(adapter, offset - CHMLINK_RING_RECORD_HEADER, adapter->rx_cursor, TRUE)) {
            /* Do not advance past a record the SDK cannot track: the driver
             * would never be told the memory is free again. */
            writer = snapshot;
            adapter->rx_cursor = saved;
            break;
        }
        rx_advance(adapter);
        count++;
    }
    if (count != 0) {
        *bytes_received = (uint32_t)writer.length;
        result = 1;
    } else SetLastError(ERROR_NO_MORE_ITEMS);
    ReleaseSRWLockExclusive(&adapter->rx_lock);
    return sdk_leave(adapter, NULL, result);
}
