/* Packets to Windows: one ring per injecting thread (up to
 * CHMLINK_INJECT_RINGS_MAX), written in place (chmlink_allocate_packet) or
 * from a batch (chmlink_inject). */
#include "internal.h"

static int tx_push(chmlink_txq *q, uint32_t start, uint32_t end)
{
    if (q->count == q->entries_capacity) {
        uint32_t capacity;
        chmlink_tx_entry *grown;
        if (q->head != 0) {
            memmove(q->entries, q->entries + q->head, (q->count - q->head) * sizeof(*q->entries));
            q->count -= q->head;
            q->head = 0;
        } else {
            capacity = q->entries_capacity * 2;
            grown = HeapReAlloc(GetProcessHeap(), 0, q->entries, capacity * sizeof(*q->entries));
            if (grown == NULL) return 0;
            q->entries = grown;
            q->entries_capacity = capacity;
        }
    }
    q->entries[q->count].start = start;
    q->entries[q->count].end = end;
    q->entries[q->count].published = FALSE;
    q->count++;
    return 1;
}

/* Publish the contiguous published prefix and wake the driver if it sleeps. */
static void tx_advance(chmlink_txq *q, BOOL *wake)
{
    uint32_t end = 0;
    BOOL advanced = FALSE;
    while (q->head < q->count && q->entries[q->head].published) {
        end = q->entries[q->head].end;
        q->head++;
        advanced = TRUE;
    }
    if (advanced) *wake = chmlink_ring_publish(&q->ring, end) != 0;
    if (q->head == q->count) q->head = q->count = 0;
}

/* txq_gate held exclusively. Registers txq[txq_count] with the driver; on
 * failure the queue is left empty and the caller shares an existing one. */
static int add_txq(chmlink_adapter *a)
{
    uint8_t bytes[CHMLINK_INJECT_RING_SIZE];
    chmlink_inject_ring request;
    chmlink_txq *q = &a->txq[a->txq_count];
    HANDLE event = sdk_new_event();
    DWORD received;
    int ok = 0;
    if (event == NULL) return 0;
    InitializeSRWLock(&q->lock);
    q->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    q->entries_capacity = CHMLINK_TX_ENTRIES_MIN;
    q->entries = HeapAlloc(GetProcessHeap(), 0, q->entries_capacity * sizeof(*q->entries));
    if (q->event != NULL && q->entries != NULL && sdk_create_ring(&q->memory, &q->ring, a->to_capacity)) {
        memset(&request, 0, sizeof(request));
        request.version = CHMLINK_INJECT_RING_VERSION;
        request.capacity = a->to_capacity;
        request.ring = (uint64_t)(ULONG_PTR)q->memory;
        request.event = (uint64_t)(ULONG_PTR)q->event;
        chmlink_inject_ring_encode(bytes, &request);
        ok = sdk_ioctl(a->device, event, CHMLINK_IOCTL_ADD_INJECT_RING, bytes, sizeof(bytes),
                          NULL, 0, &received) != 0;
    }
    CloseHandle(event);
    if (!ok) {
        /* Not registered, so nothing in the driver refers to it. */
        if (q->memory != NULL) VirtualFree(q->memory, 0, MEM_RELEASE);
        if (q->event != NULL) CloseHandle(q->event);
        HeapFree(GetProcessHeap(), 0, q->entries);
        memset(q, 0, sizeof(*q));
        return 0;
    }
    InterlockedIncrement(&a->txq_count);   /* publishes the complete queue */
    return 1;
}

/* The calling thread's queue. A new thread gets txq[0] if nobody uses it
 * yet, else a new queue while there is room, else shares round-robin. The
 * first CHMLINK_TXQ_THREADS threads are remembered; later ones share by ID. */
static chmlink_txq *pick_txq(chmlink_adapter *a)
{
    DWORD self = GetCurrentThreadId();
    uint32_t i, index;
    AcquireSRWLockShared(&a->txq_gate);
    for (i = 0; i < a->txq_mapped; ++i) {
        if (a->txq_threads[i] == self) {
            index = a->txq_of[i];
            ReleaseSRWLockShared(&a->txq_gate);
            return &a->txq[index];
        }
    }
    /* Past CHMLINK_TXQ_THREADS threads (a thread pool, say) every queue
     * exists already: share them by thread ID, without the exclusive lock. */
    if (a->txq_mapped >= CHMLINK_TXQ_THREADS) {
        index = (self / 4) % (uint32_t)a->txq_count;
        ReleaseSRWLockShared(&a->txq_gate);
        return &a->txq[index];
    }
    ReleaseSRWLockShared(&a->txq_gate);
    AcquireSRWLockExclusive(&a->txq_gate);
    for (i = 0; i < a->txq_mapped; ++i) {
        if (a->txq_threads[i] == self) {
            index = a->txq_of[i];
            ReleaseSRWLockExclusive(&a->txq_gate);
            return &a->txq[index];
        }
    }
    if (a->txq_mapped == 0) index = 0;
    else if ((uint32_t)a->txq_count < CHMLINK_INJECT_RINGS_MAX && add_txq(a)) index = (uint32_t)a->txq_count - 1;
    else index = a->txq_mapped % (uint32_t)a->txq_count;
    if (a->txq_mapped < CHMLINK_TXQ_THREADS) {
        a->txq_threads[a->txq_mapped] = self;
        a->txq_of[a->txq_mapped] = (uint8_t)index;
        a->txq_mapped++;
    }
    ReleaseSRWLockExclusive(&a->txq_gate);
    return &a->txq[index];
}

/* The queue whose ring holds `packet`, or NULL. */
static chmlink_txq *txq_of_packet(chmlink_adapter *a, const uint8_t *packet)
{
    LONG i, count = a->txq_count;
    for (i = 0; i < count; ++i) {
        const chmlink_txq *q = &a->txq[i];
        if ((uintptr_t)packet >= (uintptr_t)(q->ring.data + CHMLINK_RING_RECORD_HEADER) &&
            (uintptr_t)packet < (uintptr_t)(q->ring.data + q->ring.capacity))
            return &a->txq[i];
    }
    return NULL;
}

uint8_t *chmlink_allocate_packet(chmlink_adapter *adapter, uint32_t size,
                                 const chmlink_offload *offload)
{
    chmlink_ring_status status;
    chmlink_txq *q;
    uint8_t *packet = NULL;
    uint32_t saved;
    if (adapter == NULL || size == 0 ||
        size > (offload != NULL && offload->gso_type != CHMLINK_GSO_NONE ? CHMLINK_PACKET_MAX_SIZE : adapter->packet_limit)) {
        SetLastError(ERROR_INVALID_PARAMETER); return NULL;
    }
    if (!sdk_enter(adapter, NULL)) return NULL;
    q = pick_txq(adapter);
    AcquireSRWLockExclusive(&q->lock);
    saved = q->cursor;
    status = chmlink_ring_reserve(&q->ring, &q->cursor, size, offload, &packet);
    if (status == CHMLINK_RING_OK) {
        if (!tx_push(q, (uint32_t)(packet - q->ring.data) - CHMLINK_RING_RECORD_HEADER, q->cursor)) {
            q->cursor = saved;
            packet = NULL;
            status = CHMLINK_RING_FULL;
        }
    }
    ReleaseSRWLockExclusive(&q->lock);
    if (packet == NULL) {
        SetLastError(status == CHMLINK_RING_FULL ? ERROR_BUFFER_OVERFLOW :
                     status == CHMLINK_RING_INVALID ? ERROR_INVALID_PARAMETER : ERROR_INVALID_DATA);
        sdk_leave(adapter, NULL, 0);
        return NULL;
    }
    sdk_leave(adapter, NULL, 1);
    return packet;
}

void chmlink_send_packet(chmlink_adapter *adapter, uint8_t *packet)
{
    uint32_t start, i;
    chmlink_txq *q;
    BOOL wake = FALSE;
    if (adapter == NULL || packet == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return; }
    if (!sdk_enter(adapter, NULL)) return;
    q = txq_of_packet(adapter, packet);
    if (q == NULL) { sdk_leave(adapter, NULL, 0); SetLastError(ERROR_INVALID_PARAMETER); return; }
    AcquireSRWLockExclusive(&q->lock);
    start = (uint32_t)(packet - q->ring.data) - CHMLINK_RING_RECORD_HEADER;
    for (i = q->head; i < q->count; ++i) {
        if (q->entries[i].start == start && !q->entries[i].published) {
            q->entries[i].published = TRUE;
            break;
        }
    }
    tx_advance(q, &wake);
    ReleaseSRWLockExclusive(&q->lock);
    if (wake) SetEvent(q->event);
    sdk_leave(adapter, NULL, 1);
}

/* The record goes out marked with CHMLINK_OFFLOAD_DISCARD, a flag no driver
 * accepts, so the driver drops it (rx_discards) and the packets allocated
 * after it are not held back. */
void chmlink_abandon_packet(chmlink_adapter *adapter, uint8_t *packet)
{
    static const chmlink_offload discard = {CHMLINK_OFFLOAD_DISCARD, 0, 0, 0, 0, 0};
    if (adapter == NULL || packet == NULL || txq_of_packet(adapter, packet) == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER); return;
    }
    chmlink_ring_describe(packet, &discard);
    chmlink_send_packet(adapter, packet);
}

int chmlink_inject(chmlink_adapter *adapter, const void *batch, uint32_t length)
{
    chmlink_batch_reader reader;
    const uint8_t *packet;
    uint32_t size, count;
    chmlink_offload descriptor;
    int result = 0;
    if (adapter == NULL || batch == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (chmlink_batch_validate(batch, length, adapter->mode, adapter->packet_limit, &count) != CHMLINK_OK ||
        count == 0) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    if (!sdk_enter(adapter, NULL)) return 0;
    /* Refuse what the driver would drop anyway, so the caller gets an error
     * instead of a silent loss. */
    chmlink_batch_read_begin(&reader, batch);
    while (chmlink_batch_read_next(&reader, &packet, &size, &descriptor)) {
        if (descriptor.flags == 0 && descriptor.gso_type == 0) continue;
        if (((descriptor.flags & CHMLINK_OFFLOAD_NEEDS_CSUM) &&
             !(descriptor.flags & CHMLINK_OFFLOAD_DATA_VALID)) ||
            (descriptor.gso_type != CHMLINK_GSO_NONE &&
             !(adapter->offload & CHMLINK_OFFLOAD_RX_RSC)) ||
            !(adapter->offload & CHMLINK_OFFLOAD_RX_CHECKSUM)) {
            sdk_leave(adapter, NULL, 0); SetLastError(ERROR_INVALID_PARAMETER); return 0;
        }
    }
    /* All or nothing, as one ring publication: a full ring leaves the cursor
     * where it was, so the caller can retry the whole batch. */
    {
        chmlink_ring_status status = CHMLINK_RING_OK;
        chmlink_txq *q = pick_txq(adapter);
        uint32_t saved, start = 0;
        int first = 1;
        BOOL wake = FALSE;
        AcquireSRWLockExclusive(&q->lock);
        saved = q->cursor;
        chmlink_batch_read_begin(&reader, batch);
        while (chmlink_batch_read_next(&reader, &packet, &size, &descriptor)) {
            chmlink_offload *described = descriptor.flags != 0 || descriptor.gso_type != 0 ? &descriptor : NULL;
            uint8_t *slot;
            status = chmlink_ring_reserve(&q->ring, &q->cursor, size, described, &slot);
            if (status != CHMLINK_RING_OK) break;
            if (first) { start = (uint32_t)(slot - q->ring.data) - CHMLINK_RING_RECORD_HEADER; first = 0; }
            memcpy(slot, packet, size);
        }
        if (status == CHMLINK_RING_OK && !first) {
            if (tx_push(q, start, q->cursor)) {
                q->entries[q->count - 1].published = TRUE;
                tx_advance(q, &wake);
                result = 1;
            } else {
                status = CHMLINK_RING_FULL;
            }
        }
        if (!result) q->cursor = saved;
        ReleaseSRWLockExclusive(&q->lock);
        if (wake) SetEvent(q->event);
        if (!result) SetLastError(status == CHMLINK_RING_CORRUPT ? ERROR_INVALID_DATA :
                                  status == CHMLINK_RING_INVALID ? ERROR_INVALID_PARAMETER : ERROR_BUFFER_OVERFLOW);
    }
    return sdk_leave(adapter, NULL, result);
}
