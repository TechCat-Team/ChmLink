/* newdev.h declares DiInstallDevice for Unicode builds only. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <bcrypt.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <initguid.h>   /* defines the DEVPKEY below rather than declaring it */
#include <devpkey.h>
#include <sddl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <wchar.h>
#include "device_config.h"
#include "log.h"

const wchar_t ClHardwareId[] = L"Root\\ChmLink";
const wchar_t ClIpHardwareId[] = L"Root\\ChmLinkIP";

unsigned int ClDeviceMode(HDEVINFO set, SP_DEVINFO_DATA *device)
{
    wchar_t ids[512] = {0};
    DWORD type, bytes;
    size_t offset, count;
    unsigned int mode = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, device, SPDRP_HARDWAREID, &type,
                                          (BYTE *)ids, sizeof(ids), &bytes)) return 0;
    if (type != REG_MULTI_SZ || bytes < 2 * sizeof(wchar_t) ||
        bytes > sizeof(ids) || bytes % sizeof(wchar_t) != 0) return 0;
    count = bytes / sizeof(wchar_t);
    if (ids[count - 1] != 0 || ids[count - 2] != 0) return 0;
    for (offset = 0; offset < count - 1 && ids[offset] != 0;
         offset += wcslen(ids + offset) + 1) {
        unsigned int found = _wcsicmp(ids + offset, ClHardwareId) == 0 ? CHMLINK_CONFIG_MODE_ETHERNET :
            _wcsicmp(ids + offset, ClIpHardwareId) == 0 ? CHMLINK_CONFIG_MODE_IP : 0;
        if (found && mode && found != mode) return 0;
        if (found) mode = found;
    }
    return mode;
}

BOOL ClIsDevice(HDEVINFO set, SP_DEVINFO_DATA *device) { return ClDeviceMode(set, device) != 0; }

BOOL ClIsOemName(const wchar_t *name)
{
    size_t i = 3;
    if (_wcsnicmp(name, L"oem", 3) != 0) return FALSE;
    if (name[i] < L'0' || name[i] > L'9') return FALSE;
    while (name[i] >= L'0' && name[i] <= L'9') ++i;
    return _wcsicmp(name + i, L".inf") == 0;
}

BOOL ClPackageVersion(const wchar_t *path, unsigned long long *package)
{
    HINF inf;
    INFCONTEXT line;
    wchar_t version[80], *cursor, *end;
    unsigned long parts[4];
    unsigned int i;
    inf = SetupOpenInfFileW(path, L"Net", INF_STYLE_WIN4, NULL);
    if (inf == INVALID_HANDLE_VALUE) return FALSE;
    if (!SetupFindFirstLineW(inf, L"Version", L"DriverVer", &line) ||
        !SetupGetStringFieldW(&line, 2, version, 80, NULL)) {
        SetupCloseInfFile(inf); SetLastError(ERROR_INVALID_DATA); return FALSE;
    }
    SetupCloseInfFile(inf);
    cursor = version;
    for (i = 0; i < 4; ++i) {
        if (*cursor < L'0' || *cursor > L'9') { SetLastError(ERROR_INVALID_DATA); return FALSE; }
        errno = 0; parts[i] = wcstoul(cursor, &end, 10);
        if (errno || parts[i] > 65535 || (i < 3 ? *end != L'.' : *end != 0)) {
            SetLastError(ERROR_INVALID_DATA); return FALSE;
        }
        cursor = end + (i < 3 ? 1 : 0);
    }
    *package = CL_PACKAGE(parts[0], parts[1], parts[2], parts[3]);
    if (*package < CL_PACKAGE_CONFIG_2) { SetLastError(ERROR_REVISION_MISMATCH); return FALSE; }
    return TRUE;
}

unsigned long long ClBoundPackage(HDEVINFO set, SP_DEVINFO_DATA *device)
{
    wchar_t text[64];
    DEVPROPTYPE type;
    unsigned long parts[4] = {0};
    if (!SetupDiGetDevicePropertyW(set, device, &DEVPKEY_Device_DriverVersion, &type, (PBYTE)text,
                                   sizeof(text) - sizeof(wchar_t), NULL, 0) || type != DEVPROP_TYPE_STRING)
        return 0;
    text[63] = 0;
    if (swscanf(text, L"%lu.%lu.%lu.%lu", &parts[0], &parts[1], &parts[2], &parts[3]) != 4) return 0;
    return CL_PACKAGE(parts[0], parts[1], parts[2], parts[3]);
}

/* Windows remains responsible for signature, architecture and compatibility
 * checks. */
BOOL ClInstallOnDevice(HDEVINFO set, SP_DEVINFO_DATA *device, const wchar_t *path, BOOL *reboot)
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

/* The network interface of a device: its GUID once the class installer has
 * assigned one, and its index once NDIS has bound the miniport. */
BOOL ClInterfaceOf(HDEVINFO set, SP_DEVINFO_DATA *device, GUID *guid, uint32_t *index)
{
    wchar_t text[39];
    DWORD type, bytes = sizeof(text);
    NET_LUID luid;
    NET_IFINDEX found;
    LSTATUS error;
    HKEY key = SetupDiOpenDevRegKey(set, device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
    if (index != NULL) *index = 0;
    if (key == INVALID_HANDLE_VALUE) return FALSE;
    error = RegQueryValueExW(key, L"NetCfgInstanceId", NULL, &type, (BYTE *)text, &bytes);
    RegCloseKey(key);
    if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
    if (type != REG_SZ || bytes != sizeof(text) || text[38] != 0 || FAILED(CLSIDFromString(text, guid))) {
        SetLastError(ERROR_INVALID_DATA); return FALSE;
    }
    if (index == NULL) return TRUE;
    error = ConvertInterfaceGuidToLuid(guid, &luid);
    if (error == NO_ERROR) error = ConvertInterfaceLuidToIndex(&luid, &found);
    if (error != NO_ERROR) { SetLastError(error); return FALSE; }
    *index = found;
    return TRUE;
}

/* Windows keeps the network configuration of a removed adapter, and with it
 * an interface row that still carries the adapter's name, when the removal
 * was not finished (for example after the adapter failed to start). Such a
 * row belongs to no device, so its name is free to take. */
BOOL ClIsStaleInterface(const GUID *guid)
{
    wchar_t path[160], text[39], instance[MAX_DEVICE_ID_LEN];
    DWORD bytes = sizeof(instance), type;
    DEVINST node;
    LSTATUS error;
    if (StringFromGUID2(guid, text, 39) != 39) return FALSE;
    swprintf(path, 160, L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\%ls\\Connection", text);
    error = RegGetValueW(HKEY_LOCAL_MACHINE, path, L"PnPInstanceId", RRF_RT_REG_SZ, &type, instance, &bytes);
    if (error != ERROR_SUCCESS || instance[0] == 0) return FALSE;
    return CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_PHANTOM) == CR_NO_SUCH_DEVNODE;
}

/* An abandoned lock means a change was interrupted halfway: refuse once so
 * that the caller inspects the devices, as the lock is then released. */
/* The lock lives in a private namespace bounded by BUILTIN\Administrators.
 * Only a token with that group can create the namespace, so an unprivileged
 * process cannot squat the name the way it can a Global\ object. The process
 * keeps its namespace handle open for its lifetime: the namespace disappears
 * with its last handle, and two namespaces of the same name would mean two
 * locks. */
static SRWLOCK namespace_lock = SRWLOCK_INIT;
static HANDLE config_namespace;

static HANDLE open_namespace(void)
{
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(sid), error = ERROR_SUCCESS;
    HANDLE boundary, space = NULL;
    SECURITY_ATTRIBUTES attributes = { sizeof(attributes), NULL, FALSE };
    int attempt;

    AcquireSRWLockExclusive(&namespace_lock);
    if (config_namespace != NULL) {
        space = config_namespace;
        ReleaseSRWLockExclusive(&namespace_lock);
        return space;
    }
    boundary = CreateBoundaryDescriptorW(L"ChmLink", 0);
    if (boundary == NULL) { error = GetLastError(); goto out; }
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sid, &size) ||
        !AddSIDToBoundaryDescriptor(&boundary, sid)) { error = GetLastError(); goto boundary; }
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)",
            SDDL_REVISION_1, &attributes.lpSecurityDescriptor, NULL)) { error = GetLastError(); goto boundary; }
    /* Another process may create it, or close the last handle, between the
     * two calls; a few rounds settle either race. */
    for (attempt = 0; attempt < 8 && space == NULL; attempt++) {
        space = CreatePrivateNamespaceW(&attributes, boundary, L"ChmLink");
        if (space != NULL) break;
        error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) break;
        space = OpenPrivateNamespaceW(boundary, L"ChmLink");
        if (space == NULL) error = GetLastError();
    }
    LocalFree(attributes.lpSecurityDescriptor);
    if (space != NULL) config_namespace = space;
boundary:
    DeleteBoundaryDescriptor(boundary);
out:
    ReleaseSRWLockExclusive(&namespace_lock);
    if (space == NULL) {
        SetLastError(error);
        sdk_log_error(L"Opening the configuration lock namespace failed");
        SetLastError(error);
    }
    return space;
}

BOOL ClLock(HANDLE *mutex)
{
    DWORD locked;
    *mutex = NULL;
    if (open_namespace() == NULL) return FALSE;
    *mutex = CreateMutexW(NULL, FALSE, L"ChmLink\\AdapterConfiguration");
    if (*mutex == NULL) return FALSE;
    locked = WaitForSingleObject(*mutex, 10000);
    if (locked == WAIT_OBJECT_0) return TRUE;
    if (locked == WAIT_ABANDONED) ReleaseMutex(*mutex);
    CloseHandle(*mutex); *mutex = NULL;
    SetLastError(locked == WAIT_ABANDONED ? ERROR_INVALID_STATE : locked == WAIT_TIMEOUT ? ERROR_BUSY : GetLastError());
    return FALSE;
}

void ClUnlock(HANDLE mutex)
{
    DWORD error = GetLastError();
    ReleaseMutex(mutex); CloseHandle(mutex);
    SetLastError(error);
}

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

BOOL ClParseGuid(const wchar_t *text, GUID *id)
{
    static const GUID zero = {0};
    if (wcslen(text) != 38 || text[0] != L'{' || text[37] != L'}' || FAILED(CLSIDFromString(text, id)) ||
        memcmp(id, &zero, sizeof(*id)) == 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}

/* xx:xx:xx:xx:xx:xx or xx-xx-xx-xx-xx-xx */
BOOL ClParseMac(const wchar_t *mac, BYTE out[6])
{
    unsigned int i, digit;
    if (wcslen(mac) != 17) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    for (i = 0; i < 6; ++i) {
        unsigned int value = 0;
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
        out[i] = (BYTE)value;
    }
    return TRUE;
}

BOOL ClNewConfig(CL_DEVICE_CONFIG *config, const GUID *owner, const wchar_t *name,
                 const BYTE *mac, unsigned int mode, unsigned int mtu)
{
    static const GUID zero = {0};
    GUID id;
    wchar_t generated[64], id_text[39];
    ZeroMemory(config, sizeof(*config));
    if ((mode != CHMLINK_CONFIG_MODE_ETHERNET && mode != CHMLINK_CONFIG_MODE_IP) ||
        (mode == CHMLINK_CONFIG_MODE_IP && mac != NULL) || owner == NULL ||
        memcmp(owner, &zero, sizeof(zero)) == 0) {
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
    config->Bytes[CHMLINK_CONFIG_MODE] = (BYTE)mode;
    chmlink_config_set_mtu(config->Bytes, mtu ? mtu : CHMLINK_MTU_DEFAULT);
    CopyMemory(config->Bytes + CHMLINK_CONFIG_ADAPTER_ID, &id, sizeof(id));
    CopyMemory(config->Bytes + CHMLINK_CONFIG_OWNER_ID, owner, sizeof(*owner));
    if (mac != NULL) {
        CopyMemory(config->Bytes + CHMLINK_CONFIG_MAC, mac, CHMLINK_CONFIG_MAC_SIZE);
    } else if (mode == CHMLINK_CONFIG_MODE_ETHERNET) {
        if (BCryptGenRandom(NULL, config->Bytes + CHMLINK_CONFIG_MAC, CHMLINK_CONFIG_MAC_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            SetLastError(ERROR_GEN_FAILURE); return FALSE;
        }
        config->Bytes[CHMLINK_CONFIG_MAC] = (config->Bytes[CHMLINK_CONFIG_MAC] & 0xfc) | 2;
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
        if (!(chmlink_config_mode(config->Bytes) == CHMLINK_CONFIG_MODE_ETHERNET && row->PhysicalAddressLength == 6 &&
              memcmp(row->PhysicalAddress, config->Bytes + CHMLINK_CONFIG_MAC, CHMLINK_CONFIG_MAC_SIZE) == 0) &&
            _wcsicmp(row->Alias, config->Name) != 0) continue;
        if (!ClIsStaleInterface(&row->InterfaceGuid)) break;
        sdk_log(CHMLINK_LOG_INFO, L"Ignoring interface '%ls' left behind by a removed adapter.", row->Alias);
    }
    if (i != table->NumEntries)
        sdk_log(CHMLINK_LOG_WARNING, L"Interface '%ls' (%ls, status %d) already uses this name or MAC.",
                table->Table[i].Alias, table->Table[i].Description, (int)table->Table[i].OperStatus);
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
        if (memcmp(other.Bytes + CHMLINK_CONFIG_ADAPTER_ID, config->Bytes + CHMLINK_CONFIG_ADAPTER_ID,
                   CHMLINK_CONFIG_ID_SIZE) == 0 ||
            (chmlink_config_mode(other.Bytes) == CHMLINK_CONFIG_MODE_ETHERNET &&
             chmlink_config_mode(config->Bytes) == CHMLINK_CONFIG_MODE_ETHERNET &&
             memcmp(other.Bytes + CHMLINK_CONFIG_MAC, config->Bytes + CHMLINK_CONFIG_MAC,
                    CHMLINK_CONFIG_MAC_SIZE) == 0) ||
            _wcsicmp(other.Name, config->Name) == 0) {
            wchar_t id[MAX_DEVICE_ID_LEN];
            ULONG status = 0, problem = 0;
            if (!SetupDiGetDeviceInstanceIdW(all, &device, id, MAX_DEVICE_ID_LEN, NULL)) id[0] = 0;
            CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0);
            sdk_log(CHMLINK_LOG_WARNING, L"Adapter %ls ('%ls', devnode status 0x%lx, problem %lu) already uses this name, MAC or identity.",
                    id, other.Name, status, problem);
            SetLastError(ERROR_DUP_NAME); goto fail;
        }
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

BOOL ClUpdateMtu(HDEVINFO set, SP_DEVINFO_DATA *device, const CL_DEVICE_CONFIG *current, unsigned int mtu)
{
    BYTE bytes[CHMLINK_CONFIG_SIZE], stored[CHMLINK_CONFIG_SIZE];
    DWORD size = sizeof(stored), type = 0;
    HKEY key;
    LSTATUS error;
    CopyMemory(bytes, current->Bytes, sizeof(bytes));
    bytes[4] = CHMLINK_CONFIG_VERSION;
    chmlink_config_set_mtu(bytes, mtu);
    if (!chmlink_config_valid(bytes, sizeof(bytes))) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    key = SetupDiOpenDevRegKey(set, device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE | KEY_SET_VALUE);
    if (key == INVALID_HANDLE_VALUE) return FALSE;
    /* Only the MTU may change: the stored identity must still be the one read. */
    error = RegQueryValueExW(key, CHMLINK_CONFIG_VALUE, NULL, &type, stored, &size);
    if (error == ERROR_SUCCESS &&
        (type != REG_BINARY || size != sizeof(stored) || memcmp(stored, current->Bytes, sizeof(stored)) != 0))
        error = ERROR_INVALID_DATA;
    if (error == ERROR_SUCCESS)
        error = RegSetValueExW(key, CHMLINK_CONFIG_VALUE, 0, REG_BINARY, bytes, sizeof(bytes));
    if (error == ERROR_SUCCESS) error = RegFlushKey(key);
    RegCloseKey(key);
    if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
    return TRUE;
}

BOOL ClCheckOwner(const CL_DEVICE_CONFIG *config, const GUID *owner)
{
    if (owner != NULL && memcmp(config->Bytes + CHMLINK_CONFIG_OWNER_ID, owner, CHMLINK_CONFIG_ID_SIZE) == 0)
        return TRUE;
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}

