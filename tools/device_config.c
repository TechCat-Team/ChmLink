#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <bcrypt.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "device_config.h"

static BOOL valid_name(const wchar_t *name)
{
    size_t i, length = wcslen(name);
    if (!length || length > 128 || name[0] == L' ' || name[length - 1] == L' ' ||
        wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) return FALSE;
    for (i = 0; i < length; ++i)
        if (name[i] < 32 || wcschr(L"\\/:*?\"<>|", name[i])) return FALSE;
    return TRUE;
}

BOOL ClReadConfig(HDEVINFO set, SP_DEVINFO_DATA *device, CL_DEVICE_CONFIG *config)
{
    HKEY key = SetupDiOpenDevRegKey(set, device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
    DWORD type, bytes = sizeof(config->Bytes);
    LSTATUS error;
    if (key == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_KEY_DOES_NOT_EXIST) SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    ZeroMemory(config, sizeof(*config));
    error = RegQueryValueExW(key, CHMLINK_CONFIG_VALUE, NULL, &type, config->Bytes, &bytes);
    if (error == ERROR_SUCCESS && (type != REG_BINARY || !chmlink_config_valid(config->Bytes, bytes)))
        error = ERROR_INVALID_DATA;
    if (error == ERROR_SUCCESS) {
        bytes = sizeof(config->Name);
        error = RegQueryValueExW(key, CHMLINK_CONFIG_NAME, NULL, &type, (BYTE *)config->Name, &bytes);
        if (error == ERROR_SUCCESS && (type != REG_SZ || bytes < sizeof(wchar_t) ||
            bytes > sizeof(config->Name) || bytes % sizeof(wchar_t) ||
            config->Name[bytes / sizeof(wchar_t) - 1] != 0 ||
            (wcslen(config->Name) + 1) * sizeof(wchar_t) != bytes || !valid_name(config->Name)))
            error = ERROR_INVALID_DATA;
        /* An identity without a name is incomplete, never an unmanaged device. */
        if (error == ERROR_FILE_NOT_FOUND) error = ERROR_INVALID_DATA;
    }
    RegCloseKey(key);
    if (error == ERROR_SUCCESS && ClDeviceMode(set, device) != chmlink_config_mode(config->Bytes))
        error = ERROR_INVALID_DATA;
    if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
    return TRUE;
}

static BOOL parse_guid(const wchar_t *text, GUID *id)
{
    static const GUID zero = {0};
    if (wcslen(text) != 38 || text[0] != L'{' || text[37] != L'}' || FAILED(CLSIDFromString(text, id)) ||
        memcmp(id, &zero, sizeof(*id)) == 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

BOOL ClNewConfig(CL_DEVICE_CONFIG *config, const wchar_t *owner, const wchar_t *name,
                 const wchar_t *mac, unsigned int mode)
{
    GUID id, owner_id;
    wchar_t generated[64], id_text[39];
    unsigned int i;
    ZeroMemory(config, sizeof(*config));
    if ((mode != CHMLINK_CONFIG_MODE_ETHERNET && mode != CHMLINK_CONFIG_MODE_IP) ||
        (mode == CHMLINK_CONFIG_MODE_IP && mac != NULL) || !parse_guid(owner, &owner_id)) {
        SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    if (FAILED(CoCreateGuid(&id))) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    if (name == NULL) {
        StringFromGUID2(&id, id_text, 39);
        swprintf(generated, 64, L"ChmLink-%.*ls", 36, id_text + 1);
        name = generated;
    }
    if (!valid_name(name)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    CopyMemory(config->Bytes, "CLCF", 4);
    config->Bytes[4] = CHMLINK_CONFIG_VERSION; config->Bytes[8] = CHMLINK_CONFIG_SIZE;
    config->Bytes[12] = (BYTE)mode;
    CopyMemory(config->Bytes + 16, &id, sizeof(id));
    CopyMemory(config->Bytes + 32, &owner_id, sizeof(owner_id));
    if (mac != NULL) {
        if (wcslen(mac) != 17) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        for (i = 0; i < 6; ++i) {
            unsigned int digit, value = 0;
            for (digit = 0; digit < 2; ++digit) {
                wchar_t c = mac[i * 3 + digit];
                if (c >= L'0' && c <= L'9') value = value * 16 + c - L'0';
                else if (c >= L'a' && c <= L'f') value = value * 16 + c - L'a' + 10;
                else if (c >= L'A' && c <= L'F') value = value * 16 + c - L'A' + 10;
                else { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
            }
            if (i < 5 && ((mac[2] != L':' && mac[2] != L'-') || mac[i * 3 + 2] != mac[2])) {
                SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
            }
            config->Bytes[48 + i] = (BYTE)value;
        }
    } else if (mode == CHMLINK_CONFIG_MODE_ETHERNET) {
        if (BCryptGenRandom(NULL, config->Bytes + 48, 6, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            SetLastError(ERROR_GEN_FAILURE); return FALSE;
        }
        config->Bytes[48] = (config->Bytes[48] & 0xfc) | 2;
    }
    if (!chmlink_config_valid(config->Bytes, sizeof(config->Bytes))) {
        SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    CopyMemory(config->Name, name, (wcslen(name) + 1) * sizeof(wchar_t));
    return TRUE;
}

BOOL ClCheckUnique(const CL_DEVICE_CONFIG *config)
{
    HDEVINFO all;
    SP_DEVINFO_DATA device;
    CL_DEVICE_CONFIG other;
    PMIB_IF_TABLE2 table;
    DWORD error, i;
    error = GetIfTable2(&table);
    if (error != NO_ERROR) { SetLastError(error); return FALSE; }
    for (i = 0; i < table->NumEntries; ++i) {
        MIB_IF_ROW2 *row = &table->Table[i];
        if (chmlink_config_mode(config->Bytes) == CHMLINK_CONFIG_MODE_ETHERNET &&
            row->PhysicalAddressLength == 6 && memcmp(row->PhysicalAddress, config->Bytes + 48, 6) == 0) break;
        if (_wcsicmp(row->Alias, config->Name) == 0) break;
    }
    error = i == table->NumEntries ? ERROR_SUCCESS : ERROR_DUP_NAME;
    FreeMibTable(table);
    if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
    all = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, NULL, NULL, 0);
    if (all == INVALID_HANDLE_VALUE) return FALSE;
    ZeroMemory(&device, sizeof(device)); device.cbSize = sizeof(device);
    for (i = 0; SetupDiEnumDeviceInfo(all, i, &device); ++i) {
        if (!ClIsDevice(all, &device)) continue;
        if (!ClReadConfig(all, &device, &other)) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
            goto fail;
        }
        if (memcmp(other.Bytes + 16, config->Bytes + 16, 16) == 0 ||
            (chmlink_config_mode(other.Bytes) == CHMLINK_CONFIG_MODE_ETHERNET &&
             chmlink_config_mode(config->Bytes) == CHMLINK_CONFIG_MODE_ETHERNET &&
             memcmp(other.Bytes + 48, config->Bytes + 48, 6) == 0) ||
            _wcsicmp(other.Name, config->Name) == 0) { SetLastError(ERROR_DUP_NAME); goto fail; }
    }
    error = GetLastError(); SetupDiDestroyDeviceInfoList(all);
    if (error != ERROR_NO_MORE_ITEMS) { SetLastError(error); return FALSE; }
    return TRUE;
fail:
    error = GetLastError(); SetupDiDestroyDeviceInfoList(all); SetLastError(error); return FALSE;
}

BOOL ClWriteConfig(HDEVINFO set, SP_DEVINFO_DATA *device, const CL_DEVICE_CONFIG *config)
{
    HKEY key = SetupDiCreateDevRegKeyW(set, device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, NULL, NULL);
    LSTATUS error;
    DWORD bytes = 0;
    if (key == INVALID_HANDLE_VALUE) return FALSE;
    error = RegQueryValueExW(key, CHMLINK_CONFIG_VALUE, NULL, NULL, NULL, &bytes);
    if (error == ERROR_SUCCESS) error = ERROR_ALREADY_EXISTS;
    else if (error == ERROR_FILE_NOT_FOUND) {
        error = RegSetValueExW(key, CHMLINK_CONFIG_NAME, 0, REG_SZ,
            (const BYTE *)config->Name, (DWORD)((wcslen(config->Name) + 1) * sizeof(wchar_t)));
        if (error == ERROR_SUCCESS)
            error = RegSetValueExW(key, CHMLINK_CONFIG_VALUE, 0, REG_BINARY, config->Bytes, sizeof(config->Bytes));
        /* Commit marker is the binary value; never replace an existing identity. */
        if (error == ERROR_SUCCESS) error = RegFlushKey(key);
    }
    RegCloseKey(key);
    if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
    return TRUE;
}

BOOL ClCheckOwner(const CL_DEVICE_CONFIG *config, const wchar_t *owner)
{
    GUID id;
    if (!parse_guid(owner, &id)) return FALSE;
    if (memcmp(config->Bytes + 32, &id, 16) == 0) return TRUE;
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}

static void json_string(const wchar_t *text)
{
    const wchar_t *p;
    putwchar(L'"');
    for (p = text; *p; ++p) wprintf(L"\\u%04x", (unsigned int)*p);
    putwchar(L'"');
}

int ClShowConfig(const wchar_t *instance)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    SP_DEVINFO_DATA device = {sizeof(device)};
    CL_DEVICE_CONFIG config;
    GUID id, owner;
    wchar_t id_text[39], owner_text[39];
    DWORD error;
    if (set == INVALID_HANDLE_VALUE) return 1;
    if (!SetupDiOpenDeviceInfoW(set, instance, NULL, 0, &device)) goto fail;
    if (!ClIsDevice(set, &device)) { SetLastError(ERROR_ACCESS_DENIED); goto fail; }
    if (!ClReadConfig(set, &device, &config)) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) goto fail;
        wprintf(L"{\"Managed\":false,\"InstanceId\":"); json_string(instance); wprintf(L"}\n");
        SetupDiDestroyDeviceInfoList(set); return 0;
    }
    CopyMemory(&id, config.Bytes + 16, 16); CopyMemory(&owner, config.Bytes + 32, 16);
    StringFromGUID2(&id, id_text, 39); StringFromGUID2(&owner, owner_text, 39);
    wprintf(L"{\"Managed\":true,\"Version\":%u,\"Mode\":%u,\"AdapterId\":\"%ls\",\"OwnerId\":\"%ls\",\"Mac\":",
        (unsigned int)config.Bytes[4], chmlink_config_mode(config.Bytes), id_text, owner_text);
    if (chmlink_config_mode(config.Bytes) == CHMLINK_CONFIG_MODE_IP) wprintf(L"null");
    else wprintf(L"\"%02X:%02X:%02X:%02X:%02X:%02X\"", config.Bytes[48], config.Bytes[49], config.Bytes[50],
        config.Bytes[51], config.Bytes[52], config.Bytes[53]);
    wprintf(L",\"Name\":");
    json_string(config.Name); wprintf(L",\"InstanceId\":"); json_string(instance); wprintf(L"}\n");
    SetupDiDestroyDeviceInfoList(set); return 0;
fail:
    error = GetLastError(); SetupDiDestroyDeviceInfoList(set);
    fprintf(stderr, "Configuration query failed: %lu\n", error); return 1;
}
