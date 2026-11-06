/* chmlink-device: the command-line front end of chmlink/manage.h, plus the
 * maintenance commands the PowerShell managers use (package install and
 * removal on existing devices). Links the SDK statically, so it runs alone. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <setupapi.h>
#include <objbase.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include "chmlink/api.h"
#include "chmlink/packet.h"
#include "device_config.h"

/* The SDK's warnings and errors say which step failed and why. */
static void log_problem(chmlink_log_level level, const wchar_t *message, void *context)
{
    (void)context;
    if (level != CHMLINK_LOG_INFO) fwprintf(stderr, L"%ls\n", message);
}

static void json_string(const wchar_t *text)
{
    const wchar_t *p;
    putwchar(L'"');
    for (p = text; *p; ++p) wprintf(L"\\u%04x", (unsigned int)*p);
    putwchar(L'"');
}

static int show(const wchar_t *instance)
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
    CopyMemory(&id, config.Bytes + CHMLINK_CONFIG_ADAPTER_ID, CHMLINK_CONFIG_ID_SIZE);
    CopyMemory(&owner, config.Bytes + CHMLINK_CONFIG_OWNER_ID, CHMLINK_CONFIG_ID_SIZE);
    StringFromGUID2(&id, id_text, 39); StringFromGUID2(&owner, owner_text, 39);
    wprintf(L"{\"Managed\":true,\"Version\":%u,\"Mode\":%u,\"AdapterId\":\"%ls\",\"OwnerId\":\"%ls\",\"Mac\":",
        (unsigned int)config.Bytes[4], chmlink_config_mode(config.Bytes), id_text, owner_text);
    if (chmlink_config_mode(config.Bytes) == CHMLINK_CONFIG_MODE_IP) wprintf(L"null");
    else wprintf(L"\"%02X:%02X:%02X:%02X:%02X:%02X\"", config.Bytes[CHMLINK_CONFIG_MAC], config.Bytes[CHMLINK_CONFIG_MAC + 1],
        config.Bytes[CHMLINK_CONFIG_MAC + 2], config.Bytes[CHMLINK_CONFIG_MAC + 3],
        config.Bytes[CHMLINK_CONFIG_MAC + 4], config.Bytes[CHMLINK_CONFIG_MAC + 5]);
    wprintf(L",\"Mtu\":%u,\"Name\":", chmlink_config_mtu(config.Bytes));
    json_string(config.Name); wprintf(L",\"InstanceId\":"); json_string(instance); wprintf(L"}\n");
    SetupDiDestroyDeviceInfoList(set); return 0;
fail:
    error = GetLastError(); SetupDiDestroyDeviceInfoList(set);
    fprintf(stderr, "Configuration query failed: %lu\n", error); return 1;
}

/* Exit code of a failed change: 3 when it was partly applied. */
static int failed(const char *what, DWORD error)
{
    switch (error) {
    case ERROR_REVISION_MISMATCH:
        fprintf(stderr, "%s failed: the driver package is unsupported (0.4.3.0 or later is needed, 0.5.1.6 for an "
                        "Ethernet MTU other than 1500), or the existing adapters run a different package (use a "
                        "managed upgrade first).\n", what);
        break;
    case ERROR_DUP_NAME:
        fprintf(stderr, "%s failed: the name, MAC or identity is already in use.\n", what); break;
    case ERROR_BUSY:
        fprintf(stderr, "%s failed: the configuration is busy.\n", what); break;
    case ERROR_INVALID_STATE:
        fprintf(stderr, "%s failed: an earlier change was interrupted; inspect the device inventory.\n", what); break;
    case ERROR_INSTALL_FAILURE:
        fprintf(stderr, "%s was only partly applied; inspect the device (restart it to apply an MTU).\n", what);
        return 3;
    default:
        fprintf(stderr, "%s failed: %lu\n", what, error);
    }
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
    unsigned long long package;
    if (!length || length >= MAX_PATH) return 1;
    if (!ClPackageVersion(path, &package)) { fprintf(stderr, "Unsupported driver package version: %lu\n", GetLastError()); return 1; }
    set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    if (set == INVALID_HANDLE_VALUE) return 1;
    ZeroMemory(&device, sizeof(device)); device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(set, instance, NULL, 0, &device)) goto fail;
    if (!ClIsDevice(set, &device)) { SetLastError(ERROR_ACCESS_DENIED); goto fail; }
    if (!ClReadConfig(set, &device, &config)) goto fail;
    if (config.Bytes[4] == CHMLINK_CONFIG_VERSION && package < CL_PACKAGE_CONFIG_3) {
        SetupDiDestroyDeviceInfoList(set);
        fprintf(stderr, "The package cannot read this device's configuration version 3 (needs 0.4.2.0 or later).\n");
        return 1;
    }
    if (!ClInstallOnDevice(set, &device, path, &reboot)) goto fail;
    SetupDiDestroyDeviceInfoList(set);
    wprintf(L"Installed package on %ls\n", instance);
    if (reboot) wprintf(L"A restart is required.\n");
    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;
fail:
    error = GetLastError(); SetupDiDestroyDeviceInfoList(set);
    fprintf(stderr, "Install failed: %lu\n", error);
    return 1;
}

static int remove_package(const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    DWORD length;
    if (!ClIsOemName(name)) { fprintf(stderr, "Expected an OEM INF filename, without a path.\n"); return 2; }
    length = GetWindowsDirectoryW(path, MAX_PATH);
    if (!length || length + 5 + wcslen(name) >= MAX_PATH) return 1;
    wcscat_s(path, MAX_PATH, L"\\INF\\"); wcscat_s(path, MAX_PATH, name);
    if (!ClIsOwnPackage(path)) { fprintf(stderr, "Package is not a recognized ChmLink package.\n"); return 1; }
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

static BOOL parse_number(const wchar_t *text, unsigned long *value)
{
    wchar_t *end;
    errno = 0;
    *value = wcstoul(text, &end, 10);
    return !errno && !*end && text[0] >= L'0' && text[0] <= L'9' && *value <= 65535;
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *name = NULL, *mac = NULL, *mode = NULL, *mtu = NULL;
    BOOL creating, removing, mtu_setting, owner_set = FALSE;
    GUID owner_id;
    const GUID *owner = NULL;
    BYTE mac_bytes[6];
    unsigned long mtu_value = 0;
    int i;
    if (argc == 2 && wcscmp(argv[1], L"version") == 0) { puts("5"); return 0; }
    chmlink_set_logger(log_problem, NULL);
    if (argc == 3 && wcscmp(argv[1], L"show") == 0) return show(argv[2]);
    if (argc == 4 && wcscmp(argv[1], L"install") == 0) return install_existing(argv[2], argv[3]);
    if (argc == 3 && wcscmp(argv[1], L"package-remove") == 0) return remove_package(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"check-idle") == 0) return check_idle(argv[2]);
    creating = argc >= 3 && wcscmp(argv[1], L"create") == 0;
    removing = argc >= 3 && wcscmp(argv[1], L"remove") == 0;
    mtu_setting = argc >= 4 && wcscmp(argv[1], L"set-mtu") == 0;
    if (!creating && !removing && !mtu_setting) goto usage;
    for (i = mtu_setting ? 4 : 3; i < argc; i += 2) {
        if (i + 1 == argc) goto usage;
        if (wcscmp(argv[i], L"--owner") == 0 && !owner_set) {
            if (!ClParseGuid(argv[i + 1], &owner_id)) goto usage;
            owner = &owner_id; owner_set = TRUE;
        }
        else if (wcscmp(argv[i], L"--name") == 0 && creating && name == NULL) name = argv[i + 1];
        else if (wcscmp(argv[i], L"--mtu") == 0 && creating && mtu == NULL) mtu = argv[i + 1];
        else if (wcscmp(argv[i], L"--mac") == 0 && creating && mac == NULL) mac = argv[i + 1];
        else if (wcscmp(argv[i], L"--mode") == 0 && creating && mode == NULL) mode = argv[i + 1];
        else goto usage;
    }
    if (mode && wcscmp(mode, L"ethernet") != 0 && wcscmp(mode, L"ip") != 0) goto usage;
    if (mode && wcscmp(mode, L"ip") == 0 && mac != NULL) goto usage;
    if (mac && !ClParseMac(mac, mac_bytes)) goto usage;
    if (mtu && (!parse_number(mtu, &mtu_value) || mtu_value == 0)) goto usage;
    if (creating) {
        chmlink_create_options options;
        chmlink_device created;
        ZeroMemory(&options, sizeof(options));
        options.size = sizeof(options);
        options.mode = mode && wcscmp(mode, L"ip") == 0 ? CHMLINK_MODE_IP : CHMLINK_MODE_ETHERNET;
        options.name = name; options.owner = owner; options.mac = mac ? mac_bytes : NULL;
        options.mtu = mtu_value; options.driver_inf = argv[2];
        if (!chmlink_create_adapter(&options, &created)) return failed("Create", GetLastError());
        if (show(created.instance_id) != 0) {
            fwprintf(stderr, L"Device was created, but its configuration could not be queried: %ls\n", created.instance_id);
            return 3;
        }
        return created.flags & CHMLINK_DEVICE_REBOOT_REQUIRED ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;
    }
    if (mtu_setting) {
        uint32_t flags;
        if (!parse_number(argv[3], &mtu_value)) goto usage;
        if (!chmlink_set_adapter_mtu(argv[2], owner, (uint32_t)mtu_value, &flags)) {
            DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) {
                fprintf(stderr, "MTU %lu is not allowed: Ethernet adapters take 1280 to 9000, IP adapters 1280 to 65535.\n", mtu_value);
                return 1;
            }
            if (error == ERROR_REVISION_MISMATCH) {
                fprintf(stderr, "The installed driver is too old for this MTU (0.4.2.0 for IP, 0.5.1.6 for Ethernet); upgrade it first.\n");
                return 1;
            }
            return failed("Set MTU", error);
        }
        if (show(argv[2]) != 0) return 3;
        return flags & CHMLINK_DEVICE_REBOOT_REQUIRED ? ERROR_SUCCESS_REBOOT_REQUIRED : 0;
    }
    if (!chmlink_delete_adapter(argv[2], owner)) return failed("Remove", GetLastError());
    wprintf(L"Removed %ls\n", argv[2]);
    return 0;
usage:
    fwprintf(stderr, L"Usage: chmlink-device create <signed-inf-path> [--owner <GUID>] [--name <name>] [--mac <MAC>] [--mode ethernet|ip] [--mtu <IP 1280-65535, Ethernet 1280-9000>]\n"
                     L"       chmlink-device set-mtu <exact-device-instance-id> <mtu> [--owner <GUID>]   (restarts the device)\n"
                     L"       chmlink-device show <exact-device-instance-id>\n"
                     L"       chmlink-device remove <exact-device-instance-id> [--owner <GUID>]\n"
                     L"       chmlink-device install <exact-device-instance-id> <signed-inf-path>\n"
                     L"       chmlink-device package-remove <oemNN.inf>\n"
                     L"       chmlink-device check-idle <interface-index>\n"
                     L"       chmlink-device version\n");
    return 2;
}
