#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <stdio.h>

static const wchar_t hardware_id[] = L"Root\\ChmLink";

static BOOL is_chmlink_device(HDEVINFO set, SP_DEVINFO_DATA *device)
{
    wchar_t ids[512] = {0};
    DWORD type, bytes;
    size_t offset, count;
    if (!SetupDiGetDeviceRegistryPropertyW(set, device, SPDRP_HARDWAREID, &type,
                                          (BYTE *)ids, sizeof(ids), &bytes)) return FALSE;
    if (type != REG_MULTI_SZ || bytes < 2 * sizeof(wchar_t) ||
        bytes > sizeof(ids) || bytes % sizeof(wchar_t) != 0) return FALSE;
    count = bytes / sizeof(wchar_t);
    if (ids[count - 1] != 0 || ids[count - 2] != 0) return FALSE;
    for (offset = 0; offset < count - 1 && ids[offset] != 0;
         offset += wcslen(ids + offset) + 1) {
        if (_wcsicmp(ids + offset, hardware_id) == 0) return TRUE;
    }
    return FALSE;
}

static void print_identity(HDEVINFO set, SP_DEVINFO_DATA *device)
{
    HKEY key = SetupDiOpenDevRegKey(set, device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
    wchar_t id[64] = {0};
    DWORD type, bytes = sizeof(id);
    if (key != INVALID_HANDLE_VALUE) {
        LSTATUS error = RegQueryValueExW(key, L"NetCfgInstanceId", NULL, &type, (BYTE *)id, &bytes);
        RegCloseKey(key);
        if (error == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t) &&
            bytes <= sizeof(id) && bytes % sizeof(wchar_t) == 0 && id[bytes / sizeof(wchar_t) - 1] == 0) {
            wprintf(L"Interface GUID: %ls\n", id);
            return;
        }
    }
    wprintf(L"Interface GUID is not available yet; enumerate again after initialization.\n");
}

static int create(const wchar_t *inf)
{
    HDEVINFO set;
    SP_DEVINFO_DATA device;
    BOOL reboot = FALSE, registered = FALSE, driver_list = FALSE;
    SP_DEVINSTALL_PARAMS_W parameters;
    SP_DRVINFO_DATA_W selected;
    DWORD error = ERROR_SUCCESS;
    wchar_t path[MAX_PATH], instance[MAX_DEVICE_ID_LEN];
    DWORD path_length = GetFullPathNameW(inf, MAX_PATH, path, NULL);
    if (!path_length || path_length >= MAX_PATH) {
        fprintf(stderr, "INF path is invalid or too long.\n"); return 1;
    }
    set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    if (set == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Create device set failed: %lu\n", GetLastError()); return 1;
    }
    ZeroMemory(&device, sizeof(device));
    device.cbSize = sizeof(device);
    if (!SetupDiCreateDeviceInfoW(set, L"Net", &GUID_DEVCLASS_NET, L"ChmLAN Link", NULL,
                                  DICD_GENERATE_ID, &device)) goto fail;
    /* sizeof includes the terminator; one extra wchar makes a MULTI_SZ. */
    {
        wchar_t ids[sizeof(hardware_id) / sizeof(wchar_t) + 1] = {0};
        CopyMemory(ids, hardware_id, sizeof(hardware_id));
        if (!SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID,
                                               (const BYTE *)ids, sizeof(ids))) goto fail;
    }
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.cbSize = sizeof(parameters);
    if (!SetupDiGetDeviceInstallParamsW(set, &device, &parameters)) goto fail;
    parameters.Flags |= DI_ENUMSINGLEINF;
    CopyMemory(parameters.DriverPath, path, (wcslen(path) + 1) * sizeof(wchar_t));
    if (!SetupDiSetDeviceInstallParamsW(set, &device, &parameters)) goto fail;
    if (!SetupDiBuildDriverInfoList(set, &device, SPDIT_COMPATDRIVER)) goto fail;
    driver_list = TRUE;
    if (!SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, set, &device)) goto fail;
    ZeroMemory(&selected, sizeof(selected));
    selected.cbSize = sizeof(selected);
    if (!SetupDiGetSelectedDriverW(set, &device, &selected)) goto fail;
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &device)) goto fail;
    registered = TRUE;
    if (!SetupDiGetDeviceInstanceIdW(set, &device, instance, MAX_DEVICE_ID_LEN, NULL)) goto fail;
    /* Install only the selected package on this newly registered device.
     * Do not update every instance matching the hardware ID. */
    if (!DiInstallDevice(NULL, set, &device, &selected, 0, &reboot)) goto fail;
    wprintf(L"Created %ls\n", instance);
    print_identity(set, &device);
    if (reboot) wprintf(L"A restart is required.\n");
    SetupDiDestroyDriverInfoList(set, &device, SPDIT_COMPATDRIVER);
    SetupDiDestroyDeviceInfoList(set);
    return 0;
fail:
    error = GetLastError();
    if (registered && !SetupDiCallClassInstaller(DIF_REMOVE, set, &device))
        fprintf(stderr, "Rollback failed: %lu\n", GetLastError());
    if (driver_list) SetupDiDestroyDriverInfoList(set, &device, SPDIT_COMPATDRIVER);
    SetupDiDestroyDeviceInfoList(set);
    fprintf(stderr, "Create failed: %lu\n", error);
    return 1;
}

static int remove_device(const wchar_t *instance)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    SP_DEVINFO_DATA device;
    SP_REMOVEDEVICE_PARAMS parameters;
    DWORD error;
    if (set == INVALID_HANDLE_VALUE) return 1;
    ZeroMemory(&device, sizeof(device));
    device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(set, instance, NULL, 0, &device)) goto fail;
    if (!is_chmlink_device(set, &device)) {
        SetLastError(ERROR_ACCESS_DENIED); goto fail;
    }
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    parameters.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    parameters.Scope = DI_REMOVEDEVICE_GLOBAL;
    if (!SetupDiSetClassInstallParamsW(set, &device, &parameters.ClassInstallHeader,
                                      sizeof(parameters))) goto fail;
    if (!SetupDiCallClassInstaller(DIF_REMOVE, set, &device)) goto fail;
    SetupDiDestroyDeviceInfoList(set);
    wprintf(L"Removed %ls\n", instance);
    return 0;
fail:
    error = GetLastError();
    SetupDiDestroyDeviceInfoList(set);
    fprintf(stderr, "Remove failed: %lu\n", error);
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 3 && wcscmp(argv[1], L"create") == 0) return create(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"remove") == 0) return remove_device(argv[2]);
    fwprintf(stderr, L"Usage: chmlink-device create <signed-inf-path>\n"
                     L"       chmlink-device remove <exact-device-instance-id>\n");
    return 2;
}
