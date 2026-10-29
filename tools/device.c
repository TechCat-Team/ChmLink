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
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include "device_config.h"

static const wchar_t hardware_id[] = L"Root\\ChmLink";
static BOOL is_oem_name(const wchar_t *name);

static BOOL same_file(const wchar_t *left, const wchar_t *right)
{
    HANDLE a, b;
    BYTE first[4096], second[4096];
    DWORD x, y, error = ERROR_SUCCESS;
    a = CreateFileW(left, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (a == INVALID_HANDLE_VALUE) return FALSE;
    b = CreateFileW(right, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (b == INVALID_HANDLE_VALUE) { error = GetLastError(); CloseHandle(a); SetLastError(error); return FALSE; }
    for (;;) {
        if (!ReadFile(a, first, sizeof(first), &x, NULL) || !ReadFile(b, second, sizeof(second), &y, NULL)) {
            error = GetLastError(); break;
        }
        if (x != y || memcmp(first, second, x) != 0) { error = ERROR_REVISION_MISMATCH; break; }
        if (x == 0) break;
    }
    CloseHandle(a); CloseHandle(b); SetLastError(error); return error == ERROR_SUCCESS;
}

/* Every instance shares one service. Creation must never change the package
 * of existing devices as an incidental effect of installing a new instance. */
static BOOL check_creation_package(const wchar_t *source)
{
    HDEVINFO all = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, NULL, NULL, 0);
    SP_DEVINFO_DATA device = {sizeof(device)};
    const wchar_t *files[] = {L"chmlink.inf", L"chmlink.sys", L"chmlink.cat"};
    DWORD i, j, type, bytes, error;
    wchar_t name[64], installed[MAX_PATH], store[MAX_PATH], input[MAX_PATH], *tail;
    HKEY key;
    LSTATUS query;
    if (all == INVALID_HANDLE_VALUE) return FALSE;
    for (i = 0; SetupDiEnumDeviceInfo(all, i, &device); ++i) {
        if (!ClIsDevice(all, &device)) continue;
        key = SetupDiOpenDevRegKey(all, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE) goto fail;
        bytes = sizeof(name);
        query = RegQueryValueExW(key, L"InfPath", NULL, &type, (BYTE *)name, &bytes);
        RegCloseKey(key);
        if (query != ERROR_SUCCESS) { SetLastError(query); goto fail; }
        if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes > sizeof(name) ||
            bytes % sizeof(wchar_t) || name[bytes / sizeof(wchar_t) - 1] ||
            (wcslen(name) + 1) * sizeof(wchar_t) != bytes || !is_oem_name(name)) {
            SetLastError(ERROR_INVALID_DATA); goto fail;
        }
        bytes = GetWindowsDirectoryW(installed, MAX_PATH);
        if (!bytes || bytes + wcslen(name) + 5 >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); goto fail; }
        wcscat_s(installed, MAX_PATH, L"\\INF\\"); wcscat_s(installed, MAX_PATH, name);
        if (!SetupGetInfDriverStoreLocationW(installed, NULL, NULL, store, MAX_PATH, NULL)) goto fail;
        tail = wcsrchr(store, L'\\');
        if (!tail || (size_t)(tail - store) + 13 >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); goto fail; }
        tail[1] = 0;
        wcscpy_s(input, MAX_PATH, source);
        tail = wcsrchr(input, L'\\');
        if (!tail || (size_t)(tail - input) + 13 >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); goto fail; }
        tail[1] = 0;
        for (j = 0; j < 3; ++j) {
            wchar_t existing[MAX_PATH], candidate[MAX_PATH];
            wcscpy_s(existing, MAX_PATH, store); wcscat_s(existing, MAX_PATH, files[j]);
            wcscpy_s(candidate, MAX_PATH, input); wcscat_s(candidate, MAX_PATH, files[j]);
            if (!same_file(existing, candidate)) goto fail;
        }
    }
    error = GetLastError(); SetupDiDestroyDeviceInfoList(all);
    if (error == ERROR_NO_MORE_ITEMS) return TRUE;
    SetLastError(error); return FALSE;
fail:
    error = GetLastError(); SetupDiDestroyDeviceInfoList(all);
    fprintf(stderr, "Existing adapters must use the identical INF/SYS/CAT package; use managed upgrade first.\n");
    SetLastError(error); return FALSE;
}

/* Select from one signed INF and install on one existing devnode. Windows
 * remains responsible for signature, architecture and compatibility checks. */
static BOOL install_on_device(HDEVINFO set, SP_DEVINFO_DATA *device,
                              const wchar_t *path, BOOL *reboot)
{
    SP_DEVINSTALL_PARAMS_W parameters;
    SP_DRVINFO_DATA_W selected;
    DWORD error;
    BOOL result;
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.cbSize = sizeof(parameters);
    if (!SetupDiGetDeviceInstallParamsW(set, device, &parameters)) return FALSE;
    parameters.Flags |= DI_ENUMSINGLEINF;
    if (wcslen(path) >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
    CopyMemory(parameters.DriverPath, path, (wcslen(path) + 1) * sizeof(wchar_t));
    if (!SetupDiSetDeviceInstallParamsW(set, device, &parameters) ||
        !SetupDiBuildDriverInfoList(set, device, SPDIT_COMPATDRIVER)) return FALSE;
    ZeroMemory(&selected, sizeof(selected)); selected.cbSize = sizeof(selected);
    result = SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, set, device) &&
             SetupDiGetSelectedDriverW(set, device, &selected) &&
             DiInstallDevice(NULL, set, device, &selected, 0, reboot);
    error = GetLastError();
    SetupDiDestroyDriverInfoList(set, device, SPDIT_COMPATDRIVER);
    if (!result) SetLastError(error);
    return result;
}

BOOL ClIsDevice(HDEVINFO set, SP_DEVINFO_DATA *device)
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

static int create(const wchar_t *inf, const wchar_t *owner, const wchar_t *name, const wchar_t *mac)
{
    HDEVINFO set;
    SP_DEVINFO_DATA device;
    BOOL reboot = FALSE, registered = FALSE, driver_list = FALSE;
    BOOL rollback_failed = FALSE;
    SP_DEVINSTALL_PARAMS_W parameters;
    SP_DRVINFO_DATA_W selected;
    DWORD error = ERROR_SUCCESS;
    CL_DEVICE_CONFIG config;
    wchar_t path[MAX_PATH], instance[MAX_DEVICE_ID_LEN];
    DWORD path_length = GetFullPathNameW(inf, MAX_PATH, path, NULL);
    if (!path_length || path_length >= MAX_PATH) {
        fprintf(stderr, "INF path is invalid or too long.\n"); return 1;
    }
    if (!check_creation_package(path)) {
        fprintf(stderr, "Package preflight failed: %lu\n", GetLastError()); return 1;
    }
    if (!ClNewConfig(&config, owner, name, mac) || !ClCheckUnique(NULL, NULL, &config)) {
        fprintf(stderr, "Invalid or conflicting configuration: %lu\n", GetLastError()); return 1;
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
    if (!ClWriteConfig(set, &device, &config) ||
        !SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_FRIENDLYNAME,
            (const BYTE *)config.Name, (DWORD)((wcslen(config.Name) + 1) * sizeof(wchar_t)))) goto fail;
    if (!SetupDiGetDeviceInstanceIdW(set, &device, instance, MAX_DEVICE_ID_LEN, NULL)) goto fail;
    /* Install only the selected package on this newly registered device.
     * Do not update every instance matching the hardware ID. */
    if (!DiInstallDevice(NULL, set, &device, &selected, 0, &reboot)) goto fail;
    SetupDiDestroyDriverInfoList(set, &device, SPDIT_COMPATDRIVER);
    SetupDiDestroyDeviceInfoList(set);
    if (ClShowConfig(instance) != 0) {
        fwprintf(stderr, L"Device was created, but its configuration could not be queried: %ls\n", instance);
        return 3;
    }
    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;
fail:
    error = GetLastError();
    if (registered && !SetupDiCallClassInstaller(DIF_REMOVE, set, &device)) {
        rollback_failed = TRUE;
        fprintf(stderr, "Rollback failed: %lu\n", GetLastError());
    }
    if (driver_list) SetupDiDestroyDriverInfoList(set, &device, SPDIT_COMPATDRIVER);
    SetupDiDestroyDeviceInfoList(set);
    fprintf(stderr, "Create failed: %lu\n", error);
    return rollback_failed ? 3 : 1;
}

static int remove_device(const wchar_t *instance, const wchar_t *owner)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    SP_DEVINFO_DATA device;
    SP_REMOVEDEVICE_PARAMS parameters;
    CL_DEVICE_CONFIG config;
    DWORD error;
    if (set == INVALID_HANDLE_VALUE) return 1;
    ZeroMemory(&device, sizeof(device));
    device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(set, instance, NULL, 0, &device)) goto fail;
    if (!ClIsDevice(set, &device)) {
        SetLastError(ERROR_ACCESS_DENIED); goto fail;
    }
    if (!ClReadConfig(set, &device, &config) || !ClCheckOwner(&config, owner)) goto fail;
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

static int install_existing(const wchar_t *instance, const wchar_t *inf)
{
    HDEVINFO set;
    SP_DEVINFO_DATA device;
    wchar_t path[MAX_PATH];
    DWORD length = GetFullPathNameW(inf, MAX_PATH, path, NULL), error;
    BOOL reboot = FALSE;
    CL_DEVICE_CONFIG config;
    if (!length || length >= MAX_PATH) return 1;
    set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    if (set == INVALID_HANDLE_VALUE) return 1;
    ZeroMemory(&device, sizeof(device)); device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(set, instance, NULL, 0, &device)) goto fail;
    if (!ClIsDevice(set, &device)) { SetLastError(ERROR_ACCESS_DENIED); goto fail; }
    if (!ClReadConfig(set, &device, &config)) goto fail;
    if (!install_on_device(set, &device, path, &reboot)) goto fail;
    SetupDiDestroyDeviceInfoList(set);
    wprintf(L"Installed package on %ls\n", instance);
    if (reboot) wprintf(L"A restart is required.\n");
    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;
fail:
    error = GetLastError(); SetupDiDestroyDeviceInfoList(set);
    fprintf(stderr, "Install failed: %lu\n", error);
    return 1;
}

static BOOL is_oem_name(const wchar_t *name)
{
    size_t i = 3;
    if (_wcsnicmp(name, L"oem", 3) != 0) return FALSE;
    if (name[i] < L'0' || name[i] > L'9') return FALSE;
    while (name[i] >= L'0' && name[i] <= L'9') ++i;
    return _wcsicmp(name + i, L".inf") == 0;
}

static int remove_package(const wchar_t *name)
{
    wchar_t path[MAX_PATH], service[64], id[64];
    HINF inf;
    INFCONTEXT line;
    DWORD length;
    BOOL owned = FALSE;
    const wchar_t *models[] = {L"Models.NTamd64.10.0...14393", L"Models.NTamd64.10.0...25319"};
    const wchar_t *services[] = {L"InstallLegacy.NT.Services", L"InstallStore.NT.Services"};
    unsigned int i;
    if (!is_oem_name(name)) { fprintf(stderr, "Expected an OEM INF filename, without a path.\n"); return 2; }
    length = GetWindowsDirectoryW(path, MAX_PATH);
    if (!length || length + 5 + wcslen(name) >= MAX_PATH) return 1;
    wcscat_s(path, MAX_PATH, L"\\INF\\"); wcscat_s(path, MAX_PATH, name);
    inf = SetupOpenInfFileW(path, L"Net", INF_STYLE_WIN4, NULL);
    if (inf == INVALID_HANDLE_VALUE) return 1;
    /* Accept only this project's known INF schema, never arbitrary OEM files. */
    for (i = 0; i < 2; ++i) {
        if (SetupFindFirstLineW(inf, models[i], NULL, &line) &&
            SetupGetStringFieldW(&line, 2, id, 64, NULL) && _wcsicmp(id, hardware_id) == 0 &&
            SetupFindFirstLineW(inf, services[i], L"AddService", &line) &&
            SetupGetStringFieldW(&line, 1, service, 64, NULL) && _wcsicmp(service, L"chmlink") == 0)
            owned = TRUE;
    }
    SetupCloseInfFile(inf);
    if (!owned) { fprintf(stderr, "Package is not a recognized ChmLink package.\n"); return 1; }
    /* Flags zero: Windows refuses packages used by present OR phantom devices.
     * Never force deletion or manually remove files from the Driver Store. */
    if (!SetupUninstallOEMInfW(name, 0, NULL)) {
        fprintf(stderr, "Package removal refused: %lu\n", GetLastError()); return 1;
    }
    wprintf(L"Removed unused package %ls\n", name);
    return 0;
}

static int check_idle(const wchar_t *text)
{
    wchar_t path[64], *end;
    unsigned long index;
    HANDLE device;
    errno = 0;
    index = wcstoul(text, &end, 10);
    if (errno || *end || !index || text[0] == L'-') return 2;
    swprintf(path, 64, L"\\\\.\\ChmLink%lu", index);
    device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (device == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Idle check failed: %lu\n", GetLastError()); return 1;
    }
    CloseHandle(device);
    puts("No data session was active at the time of this check.");
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *owner = CHMLINK_DEFAULT_OWNER, *name = NULL, *mac = NULL;
    BOOL creating, adopting, removing, owner_set = FALSE;
    HANDLE mutex;
    DWORD locked;
    int i, result;
    if (argc == 2 && wcscmp(argv[1], L"version") == 0) { puts("3"); return 0; }
    if (argc == 3 && wcscmp(argv[1], L"show") == 0) return ClShowConfig(argv[2]);
    if (argc == 4 && wcscmp(argv[1], L"install") == 0) return install_existing(argv[2], argv[3]);
    if (argc == 3 && wcscmp(argv[1], L"package-remove") == 0) return remove_package(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"check-idle") == 0) return check_idle(argv[2]);
    creating = argc >= 3 && wcscmp(argv[1], L"create") == 0;
    adopting = argc >= 3 && wcscmp(argv[1], L"adopt") == 0;
    removing = argc >= 3 && wcscmp(argv[1], L"remove") == 0;
    if (!creating && !adopting && !removing) goto usage;
    for (i = 3; i < argc; i += 2) {
        if (i + 1 == argc) goto usage;
        if (wcscmp(argv[i], L"--owner") == 0 && !owner_set) { owner = argv[i + 1]; owner_set = TRUE; }
        else if (wcscmp(argv[i], L"--name") == 0 && !removing && name == NULL) name = argv[i + 1];
        else if (wcscmp(argv[i], L"--mac") == 0 && creating && mac == NULL) mac = argv[i + 1];
        else goto usage;
    }
    mutex = CreateMutexW(NULL, FALSE, L"Global\\ChmLink.AdapterConfiguration");
    if (mutex == NULL) { fprintf(stderr, "Configuration lock failed: %lu\n", GetLastError()); return 1; }
    locked = WaitForSingleObject(mutex, 0);
    if (locked != WAIT_OBJECT_0) {
        if (locked == WAIT_ABANDONED) ReleaseMutex(mutex);
        CloseHandle(mutex);
        fprintf(stderr, "Configuration is busy or was interrupted; inspect the device inventory.\n"); return 1;
    }
    if (creating) result = create(argv[2], owner, name, mac);
    else if (adopting) result = ClAdoptDevice(argv[2], owner, name);
    else result = remove_device(argv[2], owner);
    ReleaseMutex(mutex); CloseHandle(mutex); return result;
usage:
    fwprintf(stderr, L"Usage: chmlink-device create <signed-inf-path> [--owner <GUID>] [--name <name>] [--mac <MAC>]\n"
                     L"       chmlink-device adopt <exact-device-instance-id> [--owner <GUID>] [--name <name>]\n"
                     L"       chmlink-device show <exact-device-instance-id>\n"
                     L"       chmlink-device remove <exact-device-instance-id> [--owner <GUID>]\n"
                     L"       chmlink-device install <exact-device-instance-id> <signed-inf-path>\n"
                     L"       chmlink-device package-remove <oemNN.inf>\n"
                     L"       chmlink-device check-idle <interface-index>\n"
                     L"       chmlink-device version\n");
    return 2;
}
