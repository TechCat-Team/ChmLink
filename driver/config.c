#include "chmlink.h"

NDIS_STATUS ClReadConfiguration(NDIS_HANDLE adapter, UCHAR *configuration)
{
    PDEVICE_OBJECT physical = NULL;
    HANDLE key;
    UNICODE_STRING name = RTL_CONSTANT_STRING(CHMLINK_CONFIG_VALUE);
    union {
        KEY_VALUE_PARTIAL_INFORMATION Header;
        UCHAR Bytes[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + CHMLINK_CONFIG_SIZE];
    } value;
    ULONG bytes;
    WCHAR ids[256];
    ULONG offset, units;
    BOOLEAN ethernet = FALSE, ip = FALSE;
    UNICODE_STRING ethernetId = RTL_CONSTANT_STRING(L"Root\\ChmLink");
    UNICODE_STRING ipId = RTL_CONSTANT_STRING(L"Root\\ChmLinkIP");
    NTSTATUS status;
    NdisMGetDeviceProperty(adapter, &physical, NULL, NULL, NULL, NULL);
    if (physical == NULL) return NDIS_STATUS_FAILURE;
    status = IoOpenDeviceRegistryKey(physical, PLUGPLAY_REGKEY_DEVICE, KEY_QUERY_VALUE, &key);
    if (!NT_SUCCESS(status)) return NDIS_STATUS_FAILURE;
    status = ZwQueryValueKey(key, &name, KeyValuePartialInformation, &value, sizeof(value), &bytes);
    ZwClose(key);
    if (!NT_SUCCESS(status) || value.Header.Type != REG_BINARY ||
        !chmlink_config_valid(value.Header.Data, value.Header.DataLength))
        return NDIS_STATUS_INVALID_DATA;
    status = IoGetDeviceProperty(physical, DevicePropertyHardwareID, sizeof(ids), ids, &bytes);
    if (!NT_SUCCESS(status) || bytes < 2 * sizeof(WCHAR) || bytes > sizeof(ids) ||
        bytes % sizeof(WCHAR)) return NDIS_STATUS_INVALID_DATA;
    units = (ULONG)(bytes / sizeof(WCHAR));
    if (ids[units - 1] != 0 || ids[units - 2] != 0) return NDIS_STATUS_INVALID_DATA;
    for (offset = 0; offset < units - 1 && ids[offset]; ) {
        UNICODE_STRING id;
        ULONG end = offset;
        while (end < units && ids[end]) ++end;
        if (end >= units || (end - offset) > 32766) return NDIS_STATUS_INVALID_DATA;
        id.Buffer = ids + offset; id.Length = (USHORT)((end - offset) * sizeof(WCHAR));
        id.MaximumLength = id.Length;
        if (RtlEqualUnicodeString(&id, &ethernetId, TRUE)) ethernet = TRUE;
        if (RtlEqualUnicodeString(&id, &ipId, TRUE)) ip = TRUE;
        offset = end + 1;
    }
    if (ethernet == ip || (ip ? CHMLINK_CONFIG_MODE_IP : CHMLINK_CONFIG_MODE_ETHERNET) !=
        chmlink_config_mode(value.Header.Data)) return NDIS_STATUS_INVALID_DATA;
    NdisMoveMemory(configuration, value.Header.Data, CHMLINK_CONFIG_SIZE);
    return NDIS_STATUS_SUCCESS;
}
