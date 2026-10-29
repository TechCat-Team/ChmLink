#include <windows.h>
#include <stdio.h>
#include "chmlink/api.h"
#include "chmlink/device.h"
#include "chmlink/protocol.h"

struct chmlink_adapter { HANDLE device; };

chmlink_adapter *chmlink_open(uint32_t index)
{
    wchar_t path[64];
    HANDLE device;
    chmlink_adapter *adapter;
    if (index == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    swprintf(path, sizeof(path) / sizeof(path[0]), L"\\\\.\\ChmLink%lu", (unsigned long)index);
    device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) return NULL;
    adapter = HeapAlloc(GetProcessHeap(), 0, sizeof(*adapter));
    if (adapter == NULL) {
        CloseHandle(device);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    adapter->device = device;
    return adapter;
}

void chmlink_close(chmlink_adapter *adapter)
{
    if (adapter != NULL) {
        CloseHandle(adapter->device);
        HeapFree(GetProcessHeap(), 0, adapter);
    }
}

int chmlink_receive(chmlink_adapter *adapter, void *buffer, uint32_t capacity,
                    uint32_t *bytes_received)
{
    DWORD received = 0;
    if (bytes_received != NULL) *bytes_received = 0;
    if (adapter == NULL || buffer == NULL || bytes_received == NULL ||
        capacity < CHMLINK_BATCH_HEADER_SIZE || capacity > CHMLINK_BATCH_MAX_SIZE) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    if (!DeviceIoControl(adapter->device, CHMLINK_IOCTL_RECEIVE, NULL, 0,
                         buffer, capacity, &received, NULL)) return 0;
    if (chmlink_batch_validate(buffer, received, CHMLINK_MODE_ETHERNET,
        CHMLINK_ETHERNET_LIMIT, NULL) != CHMLINK_OK) {
        SetLastError(ERROR_INVALID_DATA); return 0;
    }
    *bytes_received = received;
    return 1;
}

int chmlink_inject(chmlink_adapter *adapter, const void *batch, uint32_t length)
{
    DWORD received;
    if (adapter == NULL || chmlink_batch_validate(batch, length, CHMLINK_MODE_ETHERNET,
        CHMLINK_ETHERNET_LIMIT, NULL) != CHMLINK_OK) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    return DeviceIoControl(adapter->device, CHMLINK_IOCTL_INJECT, (void *)batch,
                           length, NULL, 0, &received, NULL) != 0;
}
