/* Adapter management (chmlink/manage.h): creating, deleting and changing
 * ChmLink devices through SetupAPI, and reading their configuration. */
/* newdev.h declares DiInstallDevice for Unicode builds only. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <setupapi.h>
#include <newdev.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <wchar.h>
#include "chmlink/manage.h"
#include "chmlink/packet.h"
#include "device_config.h"

static const GUID default_owner = CHMLINK_DEFAULT_OWNER_ID;

static const GUID *owner_or_default(const GUID *owner)
{
    return owner != NULL ? owner : &default_owner;
}

static void fill_device(HDEVINFO set, SP_DEVINFO_DATA *device, const CL_DEVICE_CONFIG *config,
                        chmlink_device *out)
{
    ZeroMemory(out, sizeof(*out));
    out->mode = chmlink_config_mode(config->Bytes);
    out->mtu = chmlink_config_mtu(config->Bytes);
    if (out->mode == CHMLINK_MODE_ETHERNET)
        CopyMemory(out->mac, config->Bytes + CHMLINK_CONFIG_MAC, sizeof(out->mac));
    CopyMemory(&out->adapter_id, config->Bytes + CHMLINK_CONFIG_ADAPTER_ID, sizeof(GUID));
    CopyMemory(&out->owner_id, config->Bytes + CHMLINK_CONFIG_OWNER_ID, sizeof(GUID));
    wcscpy_s(out->name, sizeof(out->name) / sizeof(out->name[0]), config->Name);
    if (!SetupDiGetDeviceInstanceIdW(set, device, out->instance_id,
                                     sizeof(out->instance_id) / sizeof(out->instance_id[0]), NULL))
        out->instance_id[0] = 0;
    /* A GUID read before a failed index lookup is kept: the interface may
     * simply not be up yet. */
    (void)ClInterfaceOf(set, device, &out->interface_guid, &out->interface_index);
}

typedef DWORD (WINAPI *nci_set_connection_name)(const GUID *guid, const wchar_t *name);

/* Wait until the new device has a network interface and give it its name.
 * Windows may take a moment to bind NDIS after the driver installed. */
static BOOL name_interface(HDEVINFO set, SP_DEVINFO_DATA *device, const wchar_t *name)
{
    HMODULE nci = LoadLibraryExW(L"nci.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    nci_set_connection_name set_name;
    ULONGLONG deadline = GetTickCount64() + 15000;
    GUID guid;
    uint32_t index;
    DWORD error = ERROR_TIMEOUT;
    if (nci == NULL) return FALSE;
    set_name = (nci_set_connection_name)(void (*)(void))GetProcAddress(nci, "NciSetConnectionName");
    if (set_name == NULL) { error = GetLastError(); FreeLibrary(nci); SetLastError(error); return FALSE; }
    for (;;) {
        if (ClInterfaceOf(set, device, &guid, &index)) {
            error = set_name(&guid, name);
            if (error == ERROR_SUCCESS) break;
        } else {
            error = GetLastError();
        }
        if (GetTickCount64() >= deadline) break;
        Sleep(50);
    }
    FreeLibrary(nci);
    if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
    return TRUE;
}

int chmlink_create_adapter(const chmlink_create_options *o, chmlink_device *created)
{
    unsigned long long package;
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA device;
    SP_DEVINSTALL_PARAMS_W parameters;
    SP_DRVINFO_DATA_W selected;
    CL_DEVICE_CONFIG config;
    HANDLE lock = NULL;
    BOOL reboot = FALSE, upgraded, registered = FALSE, driver_list = FALSE, locked = FALSE;
    DWORD error = ERROR_SUCCESS, path_length;
    wchar_t path[MAX_PATH], extracted[MAX_PATH] = L"", embedded[MAX_PATH];
    uint32_t mode;
    if (o == NULL || o->size != sizeof(*o)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    mode = o->mode ? o->mode : CHMLINK_MODE_ETHERNET;
    if (!ClNewConfig(&config, owner_or_default(o->owner), o->name, o->mac, mode, o->mtu)) return 0;
    if (o->driver_inf != NULL) {
        path_length = GetFullPathNameW(o->driver_inf, MAX_PATH, path, NULL);
        if (!path_length || path_length >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return 0; }
        if (!ClPackageVersion(path, &package)) return 0;
        if (package < CL_PACKAGE_CONFIG_3) { SetLastError(ERROR_REVISION_MISMATCH); return 0; }
    } else if (!ClExtractDriver(extracted, embedded)) {
        return 0;
    }
    if (!ClLock(&lock)) { error = GetLastError(); goto cleanup; }
    locked = TRUE;
    /* With an explicit package, adapters must already run it: that path never
     * changes the driver of other adapters. */
    if (o->driver_inf == NULL && !ClSelectPackage(embedded, path, &reboot)) goto fail;
    if (!ClCheckPackage(path) || !ClCheckUnique(&config)) goto fail;
    set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    if (set == INVALID_HANDLE_VALUE) goto fail;
    ZeroMemory(&device, sizeof(device));
    device.cbSize = sizeof(device);
    if (!SetupDiCreateDeviceInfoW(set, L"Net", &GUID_DEVCLASS_NET, L"ChmLink", NULL,
                                  DICD_GENERATE_ID, &device)) goto fail;
    /* sizeof includes the terminator; one extra wchar makes a MULTI_SZ. */
    {
        wchar_t ids[16] = {0};
        const wchar_t *id = mode == CHMLINK_MODE_IP ? ClIpHardwareId : ClHardwareId;
        wcscpy_s(ids, 15, id);
        if (!SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, (const BYTE *)ids,
                                               (DWORD)((wcslen(id) + 2) * sizeof(wchar_t)))) goto fail;
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
    /* Install only the selected package on this newly registered device.
     * Do not update every instance matching the hardware ID. */
    upgraded = reboot;
    if (!DiInstallDevice(NULL, set, &device, &selected, 0, &reboot)) goto fail;
    reboot = reboot || upgraded;
    SetupDiDestroyDriverInfoList(set, &device, SPDIT_COMPATDRIVER);
    driver_list = FALSE;
    /* After a restart request the interface only appears with the restart. */
    if (!reboot && !name_interface(set, &device, config.Name)) goto fail;
    if (created != NULL) {
        fill_device(set, &device, &config, created);
        created->flags = reboot ? CHMLINK_DEVICE_REBOOT_REQUIRED : 0;
    }
    SetupDiDestroyDeviceInfoList(set);
    set = INVALID_HANDLE_VALUE;
    goto cleanup;
fail:
    error = GetLastError();
    if (registered && !SetupDiCallClassInstaller(DIF_REMOVE, set, &device)) error = ERROR_INSTALL_FAILURE;
    if (driver_list) SetupDiDestroyDriverInfoList(set, &device, SPDIT_COMPATDRIVER);
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
cleanup:
    if (locked) ClUnlock(lock);
    if (extracted[0]) ClRemoveExtracted(extracted);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

/* Open one ChmLink device by instance ID, with its configuration. */
static HDEVINFO open_device(const wchar_t *instance, SP_DEVINFO_DATA *device, CL_DEVICE_CONFIG *config)
{
    HDEVINFO set;
    DWORD error;
    if (instance == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    if (set == INVALID_HANDLE_VALUE) return set;
    ZeroMemory(device, sizeof(*device)); device->cbSize = sizeof(*device);
    if (!SetupDiOpenDeviceInfoW(set, instance, NULL, 0, device)) goto fail;
    if (!ClIsDevice(set, device)) { SetLastError(ERROR_ACCESS_DENIED); goto fail; }
    if (!ClReadConfig(set, device, config)) goto fail;
    return set;
fail:
    error = GetLastError(); SetupDiDestroyDeviceInfoList(set); SetLastError(error);
    return INVALID_HANDLE_VALUE;
}

int chmlink_delete_adapter(const wchar_t *instance, const GUID *owner)
{
    HDEVINFO set;
    SP_DEVINFO_DATA device;
    SP_REMOVEDEVICE_PARAMS parameters;
    CL_DEVICE_CONFIG config;
    HANDLE lock;
    BOOL ok;
    DWORD error;
    if (!ClLock(&lock)) return 0;
    set = open_device(instance, &device, &config);
    if (set == INVALID_HANDLE_VALUE) { ClUnlock(lock); return 0; }
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    parameters.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    parameters.Scope = DI_REMOVEDEVICE_GLOBAL;
    ok = ClCheckOwner(&config, owner_or_default(owner)) &&
         SetupDiSetClassInstallParamsW(set, &device, &parameters.ClassInstallHeader, sizeof(parameters)) &&
         SetupDiCallClassInstaller(DIF_REMOVE, set, &device);
    error = GetLastError();
    SetupDiDestroyDeviceInfoList(set);
    ClUnlock(lock);
    SetLastError(error);
    return ok;
}

int chmlink_set_adapter_mtu(const wchar_t *instance, const GUID *owner, uint32_t mtu, uint32_t *flags)
{
    HDEVINFO set;
    SP_DEVINFO_DATA device;
    SP_PROPCHANGE_PARAMS change;
    SP_DEVINSTALL_PARAMS_W parameters;
    CL_DEVICE_CONFIG config;
    HANDLE lock;
    DWORD error = ERROR_SUCCESS;
    if (flags != NULL) *flags = 0;
    if (!ClLock(&lock)) return 0;
    set = open_device(instance, &device, &config);
    if (set == INVALID_HANDLE_VALUE) { ClUnlock(lock); return 0; }
    if (!ClCheckOwner(&config, owner_or_default(owner))) { error = GetLastError(); goto done; }
    if (!chmlink_config_mtu_allowed(chmlink_config_mode(config.Bytes), mtu)) { error = ERROR_INVALID_PARAMETER; goto done; }
    /* A driver that predates configuration version 3 would not read it. */
    if (ClBoundPackage(set, &device) < CL_PACKAGE_CONFIG_3) { error = ERROR_REVISION_MISMATCH; goto done; }
    if (!ClUpdateMtu(set, &device, &config, mtu)) { error = GetLastError(); goto done; }
    ZeroMemory(&change, sizeof(change));
    change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    change.StateChange = DICS_PROPCHANGE;
    change.Scope = DICS_FLAG_GLOBAL;
    if (!SetupDiSetClassInstallParamsW(set, &device, &change.ClassInstallHeader, sizeof(change)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device)) { error = ERROR_INSTALL_FAILURE; goto done; }
    ZeroMemory(&parameters, sizeof(parameters)); parameters.cbSize = sizeof(parameters);
    if (flags != NULL && SetupDiGetDeviceInstallParamsW(set, &device, &parameters) &&
        (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART))) *flags = CHMLINK_DEVICE_REBOOT_REQUIRED;
done:
    SetupDiDestroyDeviceInfoList(set);
    ClUnlock(lock);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

int chmlink_get_device(const wchar_t *instance, chmlink_device *out)
{
    SP_DEVINFO_DATA device;
    CL_DEVICE_CONFIG config;
    HDEVINFO set;
    if (out == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    set = open_device(instance, &device, &config);
    if (set == INVALID_HANDLE_VALUE) return 0;
    fill_device(set, &device, &config, out);
    SetupDiDestroyDeviceInfoList(set);
    return 1;
}

int chmlink_enumerate_devices(const GUID *owner, chmlink_device_callback callback, void *context)
{
    HDEVINFO all;
    SP_DEVINFO_DATA device = {sizeof(device)};
    CL_DEVICE_CONFIG config;
    chmlink_device found;
    DWORD i, error;
    if (callback == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    all = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, NULL, NULL, 0);
    if (all == INVALID_HANDLE_VALUE) return 0;
    for (i = 0; SetupDiEnumDeviceInfo(all, i, &device); ++i) {
        if (!ClIsDevice(all, &device) || !ClReadConfig(all, &device, &config)) continue;
        if (owner != NULL && memcmp(config.Bytes + CHMLINK_CONFIG_OWNER_ID, owner, sizeof(*owner)) != 0) continue;
        fill_device(all, &device, &config, &found);
        if (!callback(&found, context)) { SetupDiDestroyDeviceInfoList(all); return 1; }
    }
    error = GetLastError(); SetupDiDestroyDeviceInfoList(all);
    if (error != ERROR_NO_MORE_ITEMS) { SetLastError(error); return 0; }
    return 1;
}
