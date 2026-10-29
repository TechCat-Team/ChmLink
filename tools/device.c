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

static int create(const wchar_t *inf)
{
    HDEVINFO set;
    SP_DEVINFO_DATA device;
    BOOL reboot = FALSE, registered = FALSE;
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
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &device)) goto fail;
    registered = TRUE;
    if (!UpdateDriverForPlugAndPlayDevicesW(NULL, hardware_id, path, 0, &reboot)) goto fail;
    if (!SetupDiGetDeviceInstanceIdW(set, &device, instance, MAX_DEVICE_ID_LEN, NULL)) goto fail;
    wprintf(L"Created %ls\n", instance);
    if (reboot) wprintf(L"A restart is required.\n");
    SetupDiDestroyDeviceInfoList(set);
    return 0;
fail:
    error = GetLastError();
    if (registered && !SetupDiCallClassInstaller(DIF_REMOVE, set, &device))
        fprintf(stderr, "Rollback failed: %lu\n", GetLastError());
    SetupDiDestroyDeviceInfoList(set);
    fprintf(stderr, "Create failed: %lu\n", error);
    return 1;
}

static int remove_device(const wchar_t *instance)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    SP_DEVINFO_DATA device;
    SP_REMOVEDEVICE_PARAMS parameters;
    wchar_t ids[512] = {0};
    DWORD type, bytes, error;
    if (set == INVALID_HANDLE_VALUE) return 1;
    ZeroMemory(&device, sizeof(device));
    device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(set, instance, NULL, 0, &device)) goto fail;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, &type,
                                          (BYTE *)ids, sizeof(ids), &bytes)) goto fail;
    if (type != REG_MULTI_SZ || bytes < 2 * sizeof(wchar_t) ||
        bytes > sizeof(ids) || ids[bytes / sizeof(wchar_t) - 1] != 0 ||
        _wcsicmp(ids, hardware_id) != 0) {
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
