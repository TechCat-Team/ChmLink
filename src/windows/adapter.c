/* Opening, enumerating, querying and closing adapters, the call gate shared
 * by every SDK entry point, and offload settings. */
#include <winsock2.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <stdio.h>
#include "chmlink/query.h"
#include "internal.h"
#include "log.h"

HANDLE sdk_new_event(void)
{
    return CreateEventW(NULL, TRUE, FALSE, NULL);
}

/* Admit a call and give it a completion event when it needs one. */
int sdk_enter(chmlink_adapter *a, HANDLE *event)
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
    if (event != NULL && *event == NULL) *event = sdk_new_event();
    if (event != NULL && *event == NULL) {
        DWORD error = GetLastError();
        AcquireSRWLockExclusive(&a->gate);
        if (--a->active == 0) WakeAllConditionVariable(&a->drained);
        ReleaseSRWLockExclusive(&a->gate);
        SetLastError(error); return 0;
    }
    return 1;
}

int sdk_leave(chmlink_adapter *a, HANDLE event, int result)
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
BOOL sdk_ioctl(HANDLE device, HANDLE event, DWORD code, const void *input, DWORD input_size,
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
    if (!sdk_ioctl(device, event, CHMLINK_IOCTL_QUERY, NULL, 0, bytes,
                      sizeof(bytes), &received)) {
        error = GetLastError();
        SetLastError(error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ?
                     ERROR_REVISION_MISMATCH : error);
        return 0;
    }
    decoded = chmlink_query_decode(&wire, bytes, received);
    if (decoded == CHMLINK_UNSUPPORTED_VERSION && received >= CHMLINK_INFO_SIZE) {
        uint32_t newest = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
        uint32_t oldest = bytes[64] | bytes[65] << 8 | bytes[66] << 16 | (uint32_t)bytes[67] << 24;
        sdk_log(CHMLINK_LOG_ERROR, L"The driver speaks device contracts %u to %u; this SDK needs %u. "
                L"Install a driver package that includes it.", oldest ? oldest : newest, newest,
                (unsigned int)CHMLINK_DEVICE_VERSION);
    }
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
    next.version = wire.version; next.min_version = wire.min_version; next.capabilities = wire.capabilities;
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
    event = sdk_new_event();
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
    if (!sdk_enter(adapter, &event)) return 0;
    result = query_device(adapter->device, event, info);
    return sdk_leave(adapter, event, result);
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
    rings.version = CHMLINK_DEVICE_VERSION;
    rings.from_windows_ring = (uint64_t)(ULONG_PTR)adapter->from_memory;
    rings.from_windows_capacity = adapter->from_capacity;
    rings.from_windows_event = (uint64_t)(ULONG_PTR)adapter->from_event;
    rings.to_windows_ring = (uint64_t)(ULONG_PTR)adapter->txq[0].memory;
    rings.to_windows_capacity = adapter->to_capacity;
    rings.to_windows_event = (uint64_t)(ULONG_PTR)adapter->txq[0].event;
    chmlink_session_encode(bytes, &rings);
    return sdk_ioctl(adapter->device, event, CHMLINK_IOCTL_REGISTER_RINGS, bytes,
                        sizeof(bytes), NULL, 0, &received) != 0;
}

/* Caller must have validated capacity. Allocates a ring and its address
 * space; the driver locks it at registration. */
int sdk_create_ring(void **memory, chmlink_ring *ring, uint32_t capacity)
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

    event = sdk_new_event();
    if (event == NULL) return NULL;
    device = open_device(index, GENERIC_READ | GENERIC_WRITE);
    if (device == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        sdk_log_error(L"Cannot open the ChmLink adapter of interface %u (another session open, not an "
                      L"administrator, or no such adapter)", index);
        CloseHandle(event); SetLastError(error); return NULL;
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
    if (!sdk_create_ring(&adapter->from_memory, &adapter->from, capacity) ||
        !sdk_create_ring(&adapter->txq[0].memory, &adapter->txq[0].ring, capacity)) {
        error = GetLastError() ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        release_resources(adapter); SetLastError(error); return NULL;
    }
    adapter->txq_count = 1;
    if (!register_rings(adapter, event)) {
        error = GetLastError();
        sdk_log_error(L"Starting the data session on interface %u failed", index);
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
    if (!sdk_enter(adapter, &event)) return 0;
    result = sdk_ioctl(adapter->device, event, CHMLINK_IOCTL_SET_OFFLOAD, input, sizeof(input),
                          output, sizeof(output), &received) != 0;
    if (result) {
        uint32_t value = (uint32_t)output[0] | ((uint32_t)output[1] << 8) |
                         ((uint32_t)output[2] << 16) | ((uint32_t)output[3] << 24);
        adapter->offload = value;
        if (granted != NULL) *granted = value;
    }
    return sdk_leave(adapter, event, result);
}
