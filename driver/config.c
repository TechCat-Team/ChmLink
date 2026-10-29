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
    NdisMoveMemory(configuration, value.Header.Data, CHMLINK_CONFIG_SIZE);
    return NDIS_STATUS_SUCCESS;
}
