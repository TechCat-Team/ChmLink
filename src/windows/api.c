#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include "chmlink/api.h"
#include "chmlink/device.h"
#include "chmlink/protocol.h"
#include "chmlink/query.h"
#include "chmlink/ring.h"
#include "chmlink/session.h"

/* Completion events for the remaining synchronous requests (QUERY,
 * SET_OFFLOAD, REGISTER_RINGS) are cached per adapter: a call takes one in
 * enter() and returns it in leave(). Ring calls do not need an event and do
 * not take one. The cache holds at most CHMLINK_EVENT_CACHE events,
 * enough for the usual query and offload threads; more concurrent calls
 * create and close their own. Close frees them all. The SDK keeps no
 * per-thread or process-wide state, so it can be linked into a DLL that is
 * unloaded. */
#define CHMLINK_EVENT_CACHE 8
#define CHMLINK_DEFAULT_RING (4u * 1024u * 1024u)
#define CHMLINK_RX_ENTRIES_MIN 1024u
#define CHMLINK_TX_ENTRIES_MIN 256u

/* A record read but not released yet. start is the record header offset in
 * the ring, end the position after it. */
typedef struct chmlink_rx_entry {
    uint32_t start;
    uint32_t end;
    BOOL released;
} chmlink_rx_entry;

/* A reserved but not published send record. */
typedef struct chmlink_tx_entry {
    uint32_t start;
    uint32_t end;
    BOOL published;
} chmlink_tx_entry;

/* One to-Windows ring with its producer state. The driver indicates each ring
 * from its own thread, so threads injecting into different rings are
 * processed by the stack in parallel; order holds within a ring. */
typedef struct chmlink_txq {
    void *memory;
    chmlink_ring ring;
    HANDLE event;
    SRWLOCK lock;
    uint32_t cursor;               /* next free record position */
    chmlink_tx_entry *entries;
    uint32_t count, entries_capacity, head;
} chmlink_txq;

/* Injecting threads remembered for queue assignment; later ones share. */
#define CHMLINK_TXQ_THREADS 64u

struct chmlink_adapter {
    HANDLE device;
    uint32_t packet_limit;
    chmlink_mode mode;
    uint32_t capabilities;
    uint32_t offload;
    HANDLE cancel_event, close_event;
    SRWLOCK gate;
    CONDITION_VARIABLE drained;
    ULONG active;
    BOOL closing, waiting;
    ULONG cached;
    HANDLE events[CHMLINK_EVENT_CACHE];

    void *from_memory;
    uint32_t from_capacity, to_capacity;
    chmlink_ring from;
    HANDLE from_event;

    SRWLOCK rx_lock;
    uint32_t rx_cursor;            /* next record to read */
    chmlink_rx_entry *rx_entries;
    uint32_t rx_count, rx_entries_capacity, rx_head;

    /* txq[0] is registered with the session; the first injecting thread
     * uses it, each further one gets a new ring (ADD_INJECT_RING) up to
     * CHMLINK_INJECT_RINGS_MAX, after which threads share. txq_count only
     * grows, after the new queue is complete. */
    chmlink_txq txq[CHMLINK_INJECT_RINGS_MAX];
    volatile LONG txq_count;
    SRWLOCK txq_gate;              /* the thread table and queue creation */
    DWORD txq_threads[CHMLINK_TXQ_THREADS];
    uint8_t txq_of[CHMLINK_TXQ_THREADS];
    uint32_t txq_mapped;
};

static HANDLE new_request_event(void)
{
    return CreateEventW(NULL, TRUE, FALSE, NULL);
}

/* Admit a call and give it a completion event when it needs one. */
static int enter(chmlink_adapter *a, HANDLE *event)
{
    if (event != NULL) *event = NULL;
    if (a == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    AcquireSRWLockExclusive(&a->gate);
    if (a->closing) {
        ReleaseSRWLockExclusive(&a->gate);
        SetLastError(ERROR_OPERATION_ABORTED); return 0;
    }
    a->active++;
    if (event != NULL && a->cached != 0) *event = a->events[--a->cached];
    ReleaseSRWLockExclusive(&a->gate);
    if (event != NULL && *event == NULL) *event = new_request_event();
    if (event != NULL && *event == NULL) {
        DWORD error = GetLastError();
        AcquireSRWLockExclusive(&a->gate);
        if (--a->active == 0) WakeAllConditionVariable(&a->drained);
        ReleaseSRWLockExclusive(&a->gate);
        SetLastError(error); return 0;
    }
    return 1;
}

static int leave(chmlink_adapter *a, HANDLE event, int result)
{
    DWORD error = GetLastError();
    AcquireSRWLockExclusive(&a->gate);
    if (event != NULL && a->cached < CHMLINK_EVENT_CACHE) { a->events[a->cached++] = event; event = NULL; }
    a->active--;
    if (a->active == 0) WakeAllConditionVariable(&a->drained);
    ReleaseSRWLockExclusive(&a->gate);
    if (event != NULL) CloseHandle(event);
    if (!result) SetLastError(error);
    return result;
}

/* Device handles are opened for overlapped I/O. On a synchronous handle the
 * I/O manager serializes every request on the file object. Each request
 * still behaves synchronously for the caller: device_ioctl waits on the
 * caller's event until the driver completes it. Preserves the request's
 * error code; a request cancelled by chmlink_close fails with
 * ERROR_OPERATION_ABORTED. */
static BOOL device_ioctl(HANDLE device, HANDLE event, DWORD code, const void *input, DWORD input_size,
                         void *output, DWORD output_size, DWORD *returned)
{
    OVERLAPPED overlapped;
    DWORD bytes = 0, error = ERROR_SUCCESS;
    BOOL ok;
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = event;
    ok = DeviceIoControl(device, code, (void *)input, input_size, output, output_size,
                         NULL, &overlapped);
    if (ok || GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(device, &overlapped, &bytes, TRUE);
    }
    if (!ok) error = GetLastError();
    if (returned != NULL) *returned = ok ? bytes : 0;
    if (!ok) SetLastError(error);
    return ok;
}

static HANDLE open_device(uint32_t index, DWORD access)
{
    wchar_t path[64];
    if (index == 0) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    swprintf(path, sizeof(path) / sizeof(path[0]), L"\\\\.\\ChmLink%lu", (unsigned long)index);
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
}

static int query_device(HANDLE device, HANDLE event, chmlink_adapter_info *info)
{
    uint8_t bytes[CHMLINK_INFO_SIZE];
    chmlink_query_info wire;
    chmlink_adapter_info next;
    chmlink_result decoded;
    DWORD received, error;
    NET_LUID luid;
    GUID guid;
    wchar_t id[39];
    int i;
    if (info == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (!device_ioctl(device, event, CHMLINK_IOCTL_QUERY, NULL, 0, bytes,
                      sizeof(bytes), &received)) {
        error = GetLastError();
        SetLastError(error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ?
                     ERROR_REVISION_MISMATCH : error);
        return 0;
    }
    decoded = chmlink_query_decode(&wire, bytes, received);
    if (decoded != CHMLINK_OK) {
        SetLastError(decoded == CHMLINK_UNSUPPORTED_VERSION ?
                     ERROR_REVISION_MISMATCH : ERROR_INVALID_DATA);
        return 0;
    }
    error = ConvertInterfaceIndexToLuid(wire.interface_index, &luid);
    if (error != NO_ERROR) { SetLastError(error); return 0; }
    if (luid.Value != wire.luid) { SetLastError(ERROR_NOT_FOUND); return 0; }
    error = ConvertInterfaceLuidToGuid(&luid, &guid);
    if (error != NO_ERROR) { SetLastError(error); return 0; }
    if (StringFromGUID2(&guid, id, 39) != 39) { SetLastError(ERROR_INVALID_DATA); return 0; }
    memset(&next, 0, sizeof(next));
    next.version = wire.version; next.capabilities = wire.capabilities;
    next.state = wire.state; next.mode = wire.mode;
    next.interface_index = wire.interface_index; next.mtu = wire.mtu;
    next.packet_limit = wire.packet_limit; next.ring_capacity = wire.ring_capacity;
    next.ring_pending_bytes = wire.ring_pending_bytes; next.packet_filter = wire.packet_filter;
    next.offload = wire.offload;
    memcpy(next.mac, wire.mac, sizeof(next.mac));
    for (i = 0; i < 39; ++i) next.interface_id[i] = (char)id[i];
    next.tx_packets = wire.tx_packets; next.tx_bytes = wire.tx_bytes;
    next.tx_errors = wire.tx_errors; next.rx_packets = wire.rx_packets;
    next.rx_bytes = wire.rx_bytes; next.rx_discards = wire.rx_discards;
    next.tx_queue_drops = wire.tx_queue_drops;
    *info = next;
    return 1;
}

int chmlink_inspect(uint32_t index, chmlink_adapter_info *info)
{
    HANDLE device, event;
    DWORD error;
    int result;
    if (info == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    event = new_request_event();
    if (event == NULL) return 0;
    device = open_device(index, GENERIC_READ);
    if (device == INVALID_HANDLE_VALUE) {
        error = GetLastError(); CloseHandle(event); SetLastError(error); return 0;
    }
    result = query_device(device, event, info);
    error = GetLastError();
    CloseHandle(device);
    CloseHandle(event);
    if (!result) SetLastError(error);
    return result;
}

int chmlink_query(chmlink_adapter *adapter, chmlink_adapter_info *info)
{
    HANDLE event;
    int result;
    if (!enter(adapter, &event)) return 0;
    result = query_device(adapter->device, event, info);
    return leave(adapter, event, result);
}

static void release_resources(chmlink_adapter *adapter)
{
    ULONG i;
    if (adapter->device != NULL && adapter->device != INVALID_HANDLE_VALUE) {
        CloseHandle(adapter->device);
        adapter->device = INVALID_HANDLE_VALUE;
    }
    if (adapter->from_memory != NULL) VirtualFree(adapter->from_memory, 0, MEM_RELEASE);
    if (adapter->from_event != NULL) CloseHandle(adapter->from_event);
    for (i = 0; i < CHMLINK_INJECT_RINGS_MAX; ++i) {
        if (adapter->txq[i].memory != NULL) VirtualFree(adapter->txq[i].memory, 0, MEM_RELEASE);
        if (adapter->txq[i].event != NULL) CloseHandle(adapter->txq[i].event);
        HeapFree(GetProcessHeap(), 0, adapter->txq[i].entries);
    }
    if (adapter->cancel_event != NULL) CloseHandle(adapter->cancel_event);
    if (adapter->close_event != NULL) CloseHandle(adapter->close_event);
    for (i = 0; i < adapter->cached; ++i) CloseHandle(adapter->events[i]);
    HeapFree(GetProcessHeap(), 0, adapter->rx_entries);
    HeapFree(GetProcessHeap(), 0, adapter);
}

static int register_rings(chmlink_adapter *adapter, HANDLE event)
{
    uint8_t bytes[CHMLINK_SESSION_SIZE];
    chmlink_session_rings rings;
    DWORD received;
    memset(&rings, 0, sizeof(rings));
    rings.version = CHMLINK_SESSION_VERSION;
    rings.from_windows_ring = (uint64_t)(ULONG_PTR)adapter->from_memory;
    rings.from_windows_capacity = adapter->from_capacity;
    rings.from_windows_event = (uint64_t)(ULONG_PTR)adapter->from_event;
    rings.to_windows_ring = (uint64_t)(ULONG_PTR)adapter->txq[0].memory;
    rings.to_windows_capacity = adapter->to_capacity;
    rings.to_windows_event = (uint64_t)(ULONG_PTR)adapter->txq[0].event;
    chmlink_session_encode(bytes, &rings);
    return device_ioctl(adapter->device, event, CHMLINK_IOCTL_REGISTER_RINGS, bytes,
                        sizeof(bytes), NULL, 0, &received) != 0;
}

/* Caller must have validated capacity. Allocates a ring and its address
 * space; the driver locks it at registration. */
static int create_ring(void **memory, chmlink_ring *ring, uint32_t capacity)
{
    void *base = VirtualAlloc(NULL, CHMLINK_RING_HEADER_SIZE + capacity,
                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (base == NULL) return 0;
    if (!chmlink_ring_attach(ring, base, capacity)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return 0;
    }
    chmlink_ring_reset(ring);
    *memory = base;
    return 1;
}

chmlink_adapter *chmlink_open_ex(uint32_t index, const chmlink_open_options *options)
{
    HANDLE device, event;
    chmlink_adapter *adapter;
    chmlink_adapter_info info;
    uint32_t capacity = CHMLINK_DEFAULT_RING;
    DWORD error;

    if (options != NULL) {
        if (options->size != sizeof(*options)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
        if (options->ring_capacity != 0) capacity = options->ring_capacity;
    }
    if (!chmlink_ring_capacity_valid(capacity)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    event = new_request_event();
    if (event == NULL) return NULL;
    device = open_device(index, GENERIC_READ | GENERIC_WRITE);
    if (device == INVALID_HANDLE_VALUE) {
        error = GetLastError(); CloseHandle(event); SetLastError(error); return NULL;
    }
    if (!query_device(device, event, &info)) {
        error = GetLastError(); CloseHandle(device); CloseHandle(event); SetLastError(error);
        return NULL;
    }
    if (info.interface_index != index) {
        CloseHandle(device); CloseHandle(event);
        SetLastError(ERROR_NOT_FOUND);
        return NULL;
    }

    adapter = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*adapter));
    if (adapter == NULL) {
        CloseHandle(device); CloseHandle(event);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    /* The first cached event; chmlink_close frees it from here on. */
    adapter->events[adapter->cached++] = event;
    adapter->device = device;
    adapter->packet_limit = info.packet_limit;
    adapter->mode = (chmlink_mode)info.mode;
    adapter->capabilities = info.capabilities;
    adapter->offload = info.offload;
    adapter->from_capacity = capacity;
    adapter->to_capacity = capacity;
    InitializeSRWLock(&adapter->gate);
    InitializeSRWLock(&adapter->rx_lock);
    InitializeSRWLock(&adapter->txq_gate);
    InitializeSRWLock(&adapter->txq[0].lock);
    InitializeConditionVariable(&adapter->drained);
    adapter->cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    adapter->close_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    adapter->from_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    adapter->txq[0].event = CreateEventW(NULL, FALSE, FALSE, NULL);
    adapter->rx_entries_capacity = CHMLINK_RX_ENTRIES_MIN;
    adapter->txq[0].entries_capacity = CHMLINK_TX_ENTRIES_MIN;
    adapter->rx_entries = HeapAlloc(GetProcessHeap(), 0,
        adapter->rx_entries_capacity * sizeof(*adapter->rx_entries));
    adapter->txq[0].entries = HeapAlloc(GetProcessHeap(), 0,
        adapter->txq[0].entries_capacity * sizeof(*adapter->txq[0].entries));
    if (adapter->cancel_event == NULL || adapter->close_event == NULL ||
        adapter->from_event == NULL || adapter->txq[0].event == NULL ||
        adapter->rx_entries == NULL || adapter->txq[0].entries == NULL) {
        error = GetLastError(); release_resources(adapter); SetLastError(error); return NULL;
    }
    if (!create_ring(&adapter->from_memory, &adapter->from, capacity) ||
        !create_ring(&adapter->txq[0].memory, &adapter->txq[0].ring, capacity)) {
        error = GetLastError() ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        release_resources(adapter); SetLastError(error); return NULL;
    }
    adapter->txq_count = 1;
    if (!register_rings(adapter, event)) {
        error = GetLastError();
        /* release_resources closes the device, which is what unlocks the
         * pages, and only then frees the ring memory. */
        release_resources(adapter);
        SetLastError(error);
        return NULL;
    }
    return adapter;
}

chmlink_adapter *chmlink_open(uint32_t index)
{
    return chmlink_open_ex(index, NULL);
}

chmlink_adapter *chmlink_open_by_id(const char *id)
{
    wchar_t wide[39];
    GUID guid;
    NET_LUID luid;
    NET_IFINDEX index;
    DWORD error;
    size_t i;
    chmlink_adapter *adapter;
    chmlink_adapter_info info;
    if (id == NULL || strlen(id) != 38 || id[0] != '{' || id[37] != '}') {
        SetLastError(ERROR_INVALID_PARAMETER); return NULL;
    }
    for (i = 0; i < 39; ++i) wide[i] = (wchar_t)(unsigned char)id[i];
    if (FAILED(CLSIDFromString(wide, &guid))) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    error = ConvertInterfaceGuidToLuid(&guid, &luid);
    if (error == NO_ERROR) error = ConvertInterfaceLuidToIndex(&luid, &index);
    if (error != NO_ERROR) { SetLastError(error); return NULL; }
    adapter = chmlink_open(index);
    if (adapter == NULL) return NULL;
    if (!chmlink_query(adapter, &info)) {
        error = GetLastError(); chmlink_close(adapter); SetLastError(error); return NULL;
    }
    if (_stricmp(info.interface_id, id) != 0) {
        chmlink_close(adapter); SetLastError(ERROR_NOT_FOUND); return NULL;
    }
    return adapter;
}

int chmlink_enumerate(chmlink_enum_callback callback, void *context)
{
    IP_ADAPTER_ADDRESSES *addresses = NULL, *p;
    ULONG size = 16384;
    ULONG result;
    unsigned int attempt;
    if (callback == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    for (attempt = 0; attempt < 4; ++attempt) {
        addresses = HeapAlloc(GetProcessHeap(), 0, size);
        if (addresses == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES,
                                      NULL, addresses, &size);
        if (result != ERROR_BUFFER_OVERFLOW) break;
        HeapFree(GetProcessHeap(), 0, addresses);
        addresses = NULL;
    }
    if (result != NO_ERROR) {
        if (addresses != NULL) HeapFree(GetProcessHeap(), 0, addresses);
        if (result == ERROR_NO_DATA) return 1;
        SetLastError(result); return 0;
    }
    for (p = addresses; p != NULL; p = p->Next) {
        chmlink_adapter_info info;
        uint32_t index = p->IfIndex ? p->IfIndex : p->Ipv6IfIndex;
        if (index == 0) continue;
        if (!chmlink_inspect(index, &info)) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
                error == ERROR_NOT_FOUND || error == ERROR_DEVICE_NOT_CONNECTED) continue;
            HeapFree(GetProcessHeap(), 0, addresses); SetLastError(error); return 0;
        }
        if (!callback(&info, context)) break;
    }
    HeapFree(GetProcessHeap(), 0, addresses);
    return 1;
}

void chmlink_close(chmlink_adapter *adapter)
{
    if (adapter != NULL) {
        AcquireSRWLockExclusive(&adapter->gate);
        adapter->closing = TRUE;
        if (adapter->close_event) SetEvent(adapter->close_event);
        /* No new call is admitted now. Wake waits and wait for calls already
         * inside to leave before the rings disappear. */
        while (adapter->active != 0) {
            ReleaseSRWLockExclusive(&adapter->gate);
            if (adapter->device != INVALID_HANDLE_VALUE && adapter->device != NULL)
                (void)CancelIoEx(adapter->device, NULL);
            AcquireSRWLockExclusive(&adapter->gate);
            if (adapter->active != 0)
                SleepConditionVariableSRW(&adapter->drained, &adapter->gate, 10, 0);
        }
        ReleaseSRWLockExclusive(&adapter->gate);
        /* Closing the device runs the driver's cleanup, which waits for its
         * thread and every indicated NBL, then unlocks the ring pages. */
        if (adapter->device != INVALID_HANDLE_VALUE && adapter->device != NULL) {
            CloseHandle(adapter->device);
            adapter->device = INVALID_HANDLE_VALUE;
        }
        release_resources(adapter);
    }
}

void *chmlink_get_read_event(chmlink_adapter *adapter)
{
    if (adapter == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    return adapter->from_event;
}

/* ---- receiving ---- */

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

static int rx_pending(chmlink_adapter *a)
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
    if (!enter(adapter, NULL)) return NULL;
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
        leave(adapter, NULL, 0);
        SetLastError(error);
        return NULL;
    }
    leave(adapter, NULL, 1);
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
    if (!enter(adapter, NULL)) return;
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
    leave(adapter, NULL, 1);
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
    if (!enter(adapter, NULL)) return 0;
    AcquireSRWLockExclusive(&adapter->rx_lock);
    tail = chmlink_ring_load(&adapter->from, CHMLINK_RING_TAIL_OFFSET);
    if (adapter->rx_cursor == tail) {
        ReleaseSRWLockExclusive(&adapter->rx_lock);
        SetLastError(ERROR_NO_MORE_ITEMS);
        leave(adapter, NULL, 0);
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
            SetLastError(ERROR_INVALID_DATA); leave(adapter, NULL, 0); return 0;
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
                leave(adapter, NULL, 0); return 0; }
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
                leave(adapter, NULL, 0);
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
    return leave(adapter, NULL, result);
}

/* ---- sending ---- */

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
    HANDLE event = new_request_event();
    DWORD received;
    int ok = 0;
    if (event == NULL) return 0;
    InitializeSRWLock(&q->lock);
    q->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    q->entries_capacity = CHMLINK_TX_ENTRIES_MIN;
    q->entries = HeapAlloc(GetProcessHeap(), 0, q->entries_capacity * sizeof(*q->entries));
    if (q->event != NULL && q->entries != NULL && create_ring(&q->memory, &q->ring, a->to_capacity)) {
        memset(&request, 0, sizeof(request));
        request.version = CHMLINK_INJECT_RING_VERSION;
        request.capacity = a->to_capacity;
        request.ring = (uint64_t)(ULONG_PTR)q->memory;
        request.event = (uint64_t)(ULONG_PTR)q->event;
        chmlink_inject_ring_encode(bytes, &request);
        ok = device_ioctl(a->device, event, CHMLINK_IOCTL_ADD_INJECT_RING, bytes, sizeof(bytes),
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
 * yet, else a new queue while there is room, else shares round-robin. */
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
    if (adapter == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!enter(adapter, NULL)) return NULL;
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
        SetLastError(status == CHMLINK_RING_FULL ? ERROR_BUFFER_OVERFLOW : ERROR_INVALID_DATA);
        leave(adapter, NULL, 0);
        return NULL;
    }
    leave(adapter, NULL, 1);
    return packet;
}

void chmlink_send_packet(chmlink_adapter *adapter, uint8_t *packet)
{
    uint32_t start, i;
    chmlink_txq *q;
    BOOL wake = FALSE;
    if (adapter == NULL || packet == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return; }
    if (!enter(adapter, NULL)) return;
    q = txq_of_packet(adapter, packet);
    if (q == NULL) { leave(adapter, NULL, 0); SetLastError(ERROR_INVALID_PARAMETER); return; }
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
    leave(adapter, NULL, 1);
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
    if (!enter(adapter, NULL)) return 0;
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
            leave(adapter, NULL, 0); SetLastError(ERROR_INVALID_PARAMETER); return 0;
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
        if (!result) SetLastError(status == CHMLINK_RING_CORRUPT ? ERROR_INVALID_DATA : ERROR_BUFFER_OVERFLOW);
    }
    return leave(adapter, NULL, result);
}

int chmlink_set_offload(chmlink_adapter *adapter, uint32_t requested, uint32_t *granted)
{
    uint8_t input[4], output[4];
    DWORD received;
    HANDLE event;
    int result;
    if (granted != NULL) *granted = 0;
    if (adapter == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (requested & ~(uint32_t)CHMLINK_OFFLOAD_ALL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    input[0] = (uint8_t)requested; input[1] = (uint8_t)(requested >> 8);
    input[2] = (uint8_t)(requested >> 16); input[3] = (uint8_t)(requested >> 24);
    if (!enter(adapter, &event)) return 0;
    result = device_ioctl(adapter->device, event, CHMLINK_IOCTL_SET_OFFLOAD, input, sizeof(input),
                          output, sizeof(output), &received) != 0;
    if (result) {
        uint32_t value = (uint32_t)output[0] | ((uint32_t)output[1] << 8) |
                         ((uint32_t)output[2] << 16) | ((uint32_t)output[3] << 24);
        adapter->offload = value;
        if (granted != NULL) *granted = value;
    }
    return leave(adapter, event, result);
}

int chmlink_cancel_wait(chmlink_adapter *a)
{
    if (a == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    AcquireSRWLockExclusive(&a->gate);
    if (a->closing) {
        ReleaseSRWLockExclusive(&a->gate);
        SetLastError(ERROR_OPERATION_ABORTED); return 0;
    }
    if (a->waiting) SetEvent(a->cancel_event);
    ReleaseSRWLockExclusive(&a->gate);
    return 1;
}

/* About 20-50 microseconds of PAUSE on current x86 cores. */
#define CHMLINK_WAIT_SPIN 2048u

int chmlink_wait_readable(chmlink_adapter *a, uint32_t timeout_ms)
{
    HANDLE events[3];
    ULONGLONG started = GetTickCount64();
    int result = 0;
    DWORD error = ERROR_SUCCESS, remaining = timeout_ms;
    if (!enter(a, NULL)) return 0;
    AcquireSRWLockExclusive(&a->gate);
    if (a->closing || a->waiting) {
        error = a->closing ? ERROR_OPERATION_ABORTED : ERROR_BUSY;
        ReleaseSRWLockExclusive(&a->gate);
        SetLastError(error); return leave(a, NULL, 0);
    }
    a->waiting = TRUE;
    ResetEvent(a->cancel_event);
    ReleaseSRWLockExclusive(&a->gate);
    events[0] = a->close_event; events[1] = a->cancel_event; events[2] = a->from_event;
    for (;;) {
        uint32_t state;
        ULONGLONG elapsed;
        DWORD wait;
        if (WaitForSingleObject(events[0], 0) == WAIT_OBJECT_0 ||
            WaitForSingleObject(events[1], 0) == WAIT_OBJECT_0) {
            error = ERROR_OPERATION_ABORTED; break;
        }
        if (rx_pending(a)) { result = 1; break; }
        state = chmlink_ring_load(&a->from, CHMLINK_RING_STATE_OFFSET);
        if (state & CHMLINK_RING_STATE_CLOSED) { error = ERROR_OPERATION_ABORTED; break; }
        if (!(state & CHMLINK_RING_STATE_RUNNING)) { error = ERROR_NOT_READY; break; }
        elapsed = GetTickCount64() - started;
        if (timeout_ms != CHMLINK_WAIT_INFINITE && elapsed >= timeout_ms) {
            error = ERROR_TIMEOUT; break;
        }
        if (timeout_ms != CHMLINK_WAIT_INFINITE) remaining = timeout_ms - (DWORD)elapsed;
        /* Poll briefly first: at high packet rates the next record arrives
         * within microseconds, and staying out of the alertable state spares
         * the driver a KeSetEvent and us a sleep and wake per few packets. */
        {
            unsigned spin;
            for (spin = 0; spin < CHMLINK_WAIT_SPIN; ++spin) {
                if (chmlink_ring_load(&a->from, CHMLINK_RING_TAIL_OFFSET) != a->rx_cursor) break;
                YieldProcessor();
            }
            if (spin < CHMLINK_WAIT_SPIN) continue;
        }
        /* Announce that a wake-up is needed before re-reading the tail, so a
         * publish cannot slip between the check and the wait. */
        chmlink_ring_store(&a->from, CHMLINK_RING_WAIT_OFFSET, 1);
        if (chmlink_ring_load(&a->from, CHMLINK_RING_TAIL_OFFSET) != a->rx_cursor) {
            chmlink_ring_store(&a->from, CHMLINK_RING_WAIT_OFFSET, 0);
            continue;
        }
        wait = WaitForMultipleObjects(3, events, FALSE, remaining);
        chmlink_ring_store(&a->from, CHMLINK_RING_WAIT_OFFSET, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) {
            error = ERROR_OPERATION_ABORTED; break;
        }
        if (wait == WAIT_FAILED) { error = GetLastError(); break; }
    }
    AcquireSRWLockExclusive(&a->gate);
    a->waiting = FALSE;
    ReleaseSRWLockExclusive(&a->gate);
    if (!result) SetLastError(error);
    return leave(a, NULL, result);
}
