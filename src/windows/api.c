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

struct chmlink_adapter {
    HANDLE device;
    uint32_t packet_limit;
    chmlink_mode mode;
    uint32_t capabilities;
    HANDLE ready_event, cancel_event, close_event;
    SRWLOCK gate;
    CONDITION_VARIABLE drained;
    ULONG active;
    BOOL closing, waiting;
};

static int enter(chmlink_adapter *a)
{
    if (a == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    AcquireSRWLockExclusive(&a->gate);
    if (a->closing) {
        ReleaseSRWLockExclusive(&a->gate);
        SetLastError(ERROR_OPERATION_ABORTED); return 0;
    }
    a->active++;
    ReleaseSRWLockExclusive(&a->gate);
    return 1;
}

static int leave(chmlink_adapter *a, int result)
{
    DWORD error = GetLastError();
    AcquireSRWLockExclusive(&a->gate);
    a->active--;
    if (a->active == 0) WakeAllConditionVariable(&a->drained);
    ReleaseSRWLockExclusive(&a->gate);
    if (!result) SetLastError(error);
    return result;
}

static HANDLE open_device(uint32_t index, DWORD access)
{
    wchar_t path[64];
    if (index == 0) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    swprintf(path, sizeof(path) / sizeof(path[0]), L"\\\\.\\ChmLink%lu", (unsigned long)index);
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static int query_device(HANDLE device, chmlink_adapter_info *info)
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
    if (!DeviceIoControl(device, CHMLINK_IOCTL_QUERY, NULL, 0, bytes,
                         sizeof(bytes), &received, NULL)) {
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
    next.packet_limit = wire.packet_limit; next.queue_capacity = wire.queue_capacity;
    next.queued_packets = wire.queued_packets; next.packet_filter = wire.packet_filter;
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
    HANDLE device;
    DWORD error;
    int result;
    if (info == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    device = open_device(index, GENERIC_READ);
    if (device == INVALID_HANDLE_VALUE) return 0;
    result = query_device(device, info);
    error = GetLastError();
    CloseHandle(device);
    if (!result) SetLastError(error);
    return result;
}

int chmlink_query(chmlink_adapter *adapter, chmlink_adapter_info *info)
{
    int result;
    if (!enter(adapter)) return 0;
    result = query_device(adapter->device, info);
    return leave(adapter, result);
}

chmlink_adapter *chmlink_open(uint32_t index)
{
    HANDLE device;
    chmlink_adapter *adapter;
    chmlink_adapter_info info;
    device = open_device(index, GENERIC_READ | GENERIC_WRITE);
    if (device == INVALID_HANDLE_VALUE) return NULL;
    if (!query_device(device, &info)) {
        DWORD error = GetLastError();
        CloseHandle(device);
        SetLastError(error);
        return NULL;
    }
    if (info.interface_index != index) {
        CloseHandle(device);
        SetLastError(ERROR_NOT_FOUND);
        return NULL;
    }
    adapter = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*adapter));
    if (adapter == NULL) {
        CloseHandle(device);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    adapter->device = device;
    adapter->packet_limit = info.packet_limit;
    adapter->mode = (chmlink_mode)info.mode;
    adapter->capabilities = info.capabilities;
    InitializeSRWLock(&adapter->gate);
    InitializeConditionVariable(&adapter->drained);
    adapter->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!adapter->ready_event) {
        DWORD error = GetLastError(); chmlink_close(adapter); SetLastError(error); return NULL;
    }
    adapter->cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!adapter->cancel_event) {
        DWORD error = GetLastError(); chmlink_close(adapter); SetLastError(error); return NULL;
    }
    adapter->close_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!adapter->close_event) {
        DWORD error = GetLastError(); chmlink_close(adapter); SetLastError(error); return NULL;
    }
    if (info.capabilities & CHMLINK_CAP_WAIT) {
        uint8_t bytes[8];
        uint64_t handle = (uint64_t)(ULONG_PTR)adapter->ready_event;
        DWORD received;
        unsigned int i;
        for (i = 0; i < 8; ++i) bytes[i] = (uint8_t)(handle >> (i * 8));
        if (!DeviceIoControl(device, CHMLINK_IOCTL_REGISTER_READ_EVENT, bytes,
                             sizeof(bytes), NULL, 0, &received, NULL)) {
            DWORD error = GetLastError(); chmlink_close(adapter); SetLastError(error); return NULL;
        }
    }
    return adapter;
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
        while (adapter->active != 0)
            SleepConditionVariableSRW(&adapter->drained, &adapter->gate, INFINITE, 0);
        ReleaseSRWLockExclusive(&adapter->gate);
        CloseHandle(adapter->device);
        if (adapter->ready_event) CloseHandle(adapter->ready_event);
        if (adapter->cancel_event) CloseHandle(adapter->cancel_event);
        if (adapter->close_event) CloseHandle(adapter->close_event);
        HeapFree(GetProcessHeap(), 0, adapter);
    }
}

int chmlink_receive(chmlink_adapter *adapter, void *buffer, uint32_t capacity,
                    uint32_t *bytes_received)
{
    DWORD received = 0;
    int result;
    if (bytes_received != NULL) *bytes_received = 0;
    if (adapter == NULL || buffer == NULL || bytes_received == NULL ||
        capacity < CHMLINK_BATCH_HEADER_SIZE || capacity > CHMLINK_BATCH_MAX_SIZE) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    if (!enter(adapter)) return 0;
    result = DeviceIoControl(adapter->device, CHMLINK_IOCTL_RECEIVE, NULL, 0,
                              buffer, capacity, &received, NULL) != 0;
    if (!result) return leave(adapter, 0);
    if (chmlink_batch_validate(buffer, received, adapter->mode, adapter->packet_limit, NULL) != CHMLINK_OK) {
        SetLastError(ERROR_INVALID_DATA); return leave(adapter, 0);
    }
    *bytes_received = received;
    return leave(adapter, 1);
}

int chmlink_inject(chmlink_adapter *adapter, const void *batch, uint32_t length)
{
    DWORD received;
    int result;
    if (!enter(adapter)) return 0;
    if (chmlink_batch_validate(batch, length, adapter->mode, adapter->packet_limit, NULL) != CHMLINK_OK) {
        SetLastError(ERROR_INVALID_PARAMETER); return leave(adapter, 0);
    }
    result = DeviceIoControl(adapter->device, CHMLINK_IOCTL_INJECT, (void *)batch,
                           length, NULL, 0, &received, NULL) != 0;
    return leave(adapter, result);
}

int chmlink_cancel_wait(chmlink_adapter *a)
{
    if (!enter(a)) return 0;
    AcquireSRWLockExclusive(&a->gate);
    if (a->waiting) SetEvent(a->cancel_event);
    ReleaseSRWLockExclusive(&a->gate);
    return leave(a, 1);
}

int chmlink_wait_readable(chmlink_adapter *a, uint32_t timeout_ms)
{
    HANDLE events[3];
    ULONGLONG started = GetTickCount64();
    int result = 0;
    DWORD error = ERROR_SUCCESS;
    if (!enter(a)) return 0;
    if (!(a->capabilities & CHMLINK_CAP_WAIT)) {
        SetLastError(ERROR_NOT_SUPPORTED); return leave(a, 0);
    }
    AcquireSRWLockExclusive(&a->gate);
    if (a->closing || a->waiting) {
        error = a->closing ? ERROR_OPERATION_ABORTED : ERROR_BUSY;
        ReleaseSRWLockExclusive(&a->gate);
        SetLastError(error); return leave(a, 0);
    }
    a->waiting = TRUE;
    ResetEvent(a->cancel_event);
    ReleaseSRWLockExclusive(&a->gate);
    events[0] = a->close_event; events[1] = a->cancel_event; events[2] = a->ready_event;
    for (;;) {
        chmlink_adapter_info info;
        DWORD remaining = timeout_ms, wait;
        ULONGLONG elapsed;
        if (WaitForSingleObject(events[0], 0) == WAIT_OBJECT_0 ||
            WaitForSingleObject(events[1], 0) == WAIT_OBJECT_0) {
            error = ERROR_OPERATION_ABORTED; break;
        }
        if (!query_device(a->device, &info)) { error = GetLastError(); break; }
        if (info.state & CHMLINK_STATE_REMOVING) { error = ERROR_DEVICE_NOT_CONNECTED; break; }
        if (!(info.state & CHMLINK_STATE_RUNNING)) { error = ERROR_NOT_READY; break; }
        if (!(info.state & CHMLINK_STATE_SESSION)) { error = ERROR_OPERATION_ABORTED; break; }
        if (info.queued_packets != 0) { result = 1; break; }
        if (timeout_ms != INFINITE) {
            elapsed = GetTickCount64() - started;
            if (elapsed >= timeout_ms) { error = ERROR_TIMEOUT; break; }
            remaining = timeout_ms - (DWORD)elapsed;
        }
        wait = WaitForMultipleObjects(3, events, FALSE, remaining);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) { error = ERROR_OPERATION_ABORTED; break; }
        if (wait == WAIT_TIMEOUT) { error = ERROR_TIMEOUT; break; }
        if (wait == WAIT_FAILED) { error = GetLastError(); break; }
        /* The consumer may have drained the queue before this thread ran.
         * Recheck state and readiness without resetting the driver's event. */
    }
    AcquireSRWLockExclusive(&a->gate);
    a->waiting = FALSE;
    ReleaseSRWLockExclusive(&a->gate);
    if (!result) SetLastError(error);
    return leave(a, result);
}
