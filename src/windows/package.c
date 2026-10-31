/* Driver packages: the one embedded in chmlink.dll, which package a new
 * adapter installs, upgrades of existing adapters, and removal of packages
 * that no adapter uses. Callers hold the configuration lock (ClLock). */
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
#include <sddl.h>
#include <setupapi.h>
#include <devguid.h>
#include <stdio.h>
#include <wchar.h>
#include "chmlink/manage.h"
#include "device_config.h"
#include "log.h"

static const wchar_t *const package_files[] = {L"chmlink.inf", L"chmlink.sys", L"chmlink.cat"};

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

/* `inf` with its file name replaced by `name`. */
static BOOL sibling(const wchar_t *inf, const wchar_t *name, wchar_t out[MAX_PATH])
{
    const wchar_t *tail = wcsrchr(inf, L'\\');
    size_t directory;
    if (tail == NULL) { SetLastError(ERROR_BAD_PATHNAME); return FALSE; }
    directory = (size_t)(tail - inf) + 1;
    if (directory + wcslen(name) >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
    wmemcpy(out, inf, directory);
    wcscpy_s(out + directory, MAX_PATH - directory, name);
    return TRUE;
}

/* Whether two INFs come with identical INF, SYS and CAT files. A missing file
 * also counts as a different package. */
static BOOL same_package(const wchar_t *left, const wchar_t *right)
{
    wchar_t a[MAX_PATH], b[MAX_PATH];
    unsigned int i;
    for (i = 0; i < 3; ++i) {
        if (!sibling(left, package_files[i], a) || !sibling(right, package_files[i], b)) return FALSE;
        if (!same_file(a, b)) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) SetLastError(ERROR_REVISION_MISMATCH);
            return FALSE;
        }
    }
    return TRUE;
}

/* The INF in the Driver Store that a device runs. */
static BOOL device_package(HDEVINFO set, SP_DEVINFO_DATA *device, wchar_t inf[MAX_PATH])
{
    wchar_t name[64], published[MAX_PATH];
    DWORD type, bytes = sizeof(name), length;
    LSTATUS error;
    HKEY key = SetupDiOpenDevRegKey(set, device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
    if (key == INVALID_HANDLE_VALUE) return FALSE;
    error = RegQueryValueExW(key, L"InfPath", NULL, &type, (BYTE *)name, &bytes);
    RegCloseKey(key);
    if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes > sizeof(name) ||
        bytes % sizeof(wchar_t) || name[bytes / sizeof(wchar_t) - 1] ||
        (wcslen(name) + 1) * sizeof(wchar_t) != bytes || !ClIsOemName(name)) {
        SetLastError(ERROR_INVALID_DATA); return FALSE;
    }
    length = GetWindowsDirectoryW(published, MAX_PATH);
    if (!length || length + wcslen(name) + 5 >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return FALSE; }
    wcscat_s(published, MAX_PATH, L"\\INF\\"); wcscat_s(published, MAX_PATH, name);
    return SetupGetInfDriverStoreLocationW(published, NULL, NULL, inf, MAX_PATH, NULL);
}

/* No data session open on the device. A device without an interface cannot
 * have one. */
static BOOL device_idle(HDEVINFO set, SP_DEVINFO_DATA *device)
{
    GUID guid;
    uint32_t index;
    wchar_t path[64];
    HANDLE handle;
    if (!ClInterfaceOf(set, device, &guid, &index) || index == 0) return TRUE;
    swprintf(path, 64, L"\\\\.\\ChmLink%lu", (unsigned long)index);
    handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(handle);
    return TRUE;
}

BOOL ClCheckPackage(const wchar_t *source)
{
    HDEVINFO all = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, NULL, NULL, 0);
    SP_DEVINFO_DATA device = {sizeof(device)};
    wchar_t store[MAX_PATH];
    DWORD i, error;
    if (all == INVALID_HANDLE_VALUE) return FALSE;
    for (i = 0; SetupDiEnumDeviceInfo(all, i, &device); ++i) {
        if (!ClIsDevice(all, &device)) continue;
        if (!device_package(all, &device, store) || !same_package(store, source)) {
            error = GetLastError(); SetupDiDestroyDeviceInfoList(all);
            SetLastError(error == ERROR_FILE_NOT_FOUND ? ERROR_REVISION_MISMATCH : error);
            return FALSE;
        }
    }
    error = GetLastError(); SetupDiDestroyDeviceInfoList(all);
    if (error == ERROR_NO_MORE_ITEMS) return TRUE;
    SetLastError(error); return FALSE;
}

/* Newest package wins: a package of existing adapters at least as new as the
 * embedded one is used as it is; an older one is first upgraded to the
 * embedded package, unless an adapter has an open data session; then the
 * existing package is used if it supports creation. */
BOOL ClSelectPackage(const wchar_t *embedded, wchar_t chosen[MAX_PATH], BOOL *reboot)
{
    HDEVINFO all;
    SP_DEVINFO_DATA device = {sizeof(device)};
    unsigned long long embedded_version, version, newest = 0;
    wchar_t store[MAX_PATH], newest_inf[MAX_PATH];
    BOOL busy = FALSE, outdated = FALSE;
    DWORD i, error;
    if (!ClPackageVersion(embedded, &embedded_version)) return FALSE;
    if (embedded_version < CL_PACKAGE_SDK) { SetLastError(ERROR_REVISION_MISMATCH); return FALSE; }
    all = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, NULL, NULL, 0);
    if (all == INVALID_HANDLE_VALUE) return FALSE;
    for (i = 0; SetupDiEnumDeviceInfo(all, i, &device); ++i) {
        if (!ClIsDevice(all, &device)) continue;
        if (!device_package(all, &device, store) || !ClPackageVersion(store, &version)) goto fail;
        if (version > newest) { newest = version; wcscpy_s(newest_inf, MAX_PATH, store); }
        if (!same_package(store, embedded)) {
            if (GetLastError() != ERROR_REVISION_MISMATCH) goto fail;
            outdated = TRUE;
            if (!device_idle(all, &device)) busy = TRUE;
        }
    }
    if (GetLastError() != ERROR_NO_MORE_ITEMS) goto fail;
    if (!outdated) {
        wcscpy_s(chosen, MAX_PATH, embedded);
    } else if (newest >= embedded_version || busy) {
        /* Keep what runs; creation checks that every adapter runs it. */
        if (newest < CL_PACKAGE_SDK) {
            sdk_log(CHMLINK_LOG_ERROR, L"Adapters run driver %u.%u.%u.%u, too old for this SDK, and one has a data session "
                    L"open, so it cannot be upgraded to %u.%u.%u.%u now.", CL_PACKAGE_PARTS(newest),
                    CL_PACKAGE_PARTS(embedded_version));
            SetLastError(busy ? ERROR_BUSY : ERROR_REVISION_MISMATCH); goto fail;
        }
        if (newest < embedded_version)
            sdk_log(CHMLINK_LOG_WARNING, L"Keeping driver %u.%u.%u.%u instead of the embedded %u.%u.%u.%u: an adapter "
                    L"has a data session open.", CL_PACKAGE_PARTS(newest), CL_PACKAGE_PARTS(embedded_version));
        wcscpy_s(chosen, MAX_PATH, newest_inf);
    } else {
        for (i = 0; SetupDiEnumDeviceInfo(all, i, &device); ++i) {
            BOOL restart = FALSE;
            if (!ClIsDevice(all, &device)) continue;
            if (!device_package(all, &device, store)) goto fail;
            if (same_package(store, embedded)) continue;
            sdk_log(CHMLINK_LOG_INFO, L"Upgrading driver %u.%u.%u.%u to the embedded %u.%u.%u.%u on an existing adapter.",
                    CL_PACKAGE_PARTS(newest), CL_PACKAGE_PARTS(embedded_version));
            if (!ClInstallOnDevice(all, &device, embedded, &restart)) {
                sdk_log_error(L"Upgrading an existing adapter failed");
                goto fail;
            }
            if (restart) *reboot = TRUE;
        }
        if (GetLastError() != ERROR_NO_MORE_ITEMS) goto fail;
        wcscpy_s(chosen, MAX_PATH, embedded);
    }
    SetupDiDestroyDeviceInfoList(all);
    return TRUE;
fail:
    error = GetLastError(); SetupDiDestroyDeviceInfoList(all); SetLastError(error);
    return FALSE;
}

/* The embedded package goes into a fresh directory that only SYSTEM and
 * administrators can change, so that nobody swaps files before installation. */
BOOL ClExtractDriver(wchar_t directory[MAX_PATH], wchar_t inf[MAX_PATH])
{
    static const wchar_t *const resources[] = {L"CHMLINK_INF", L"CHMLINK_SYS", L"CHMLINK_CAT"};
    SECURITY_ATTRIBUTES security = {sizeof(security)};
    HMODULE module;
    GUID id;
    wchar_t name[39], path[MAX_PATH];
    DWORD length, written, error;
    unsigned int i;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (const wchar_t *)(void *)ClExtractDriver, &module)) return FALSE;
    if (FindResourceW(module, resources[0], (const wchar_t *)RT_RCDATA) == NULL) {
        SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND); return FALSE;
    }
    length = GetTempPathW(MAX_PATH, directory);
    if (!length || length + 48 >= MAX_PATH || FAILED(CoCreateGuid(&id)) || !StringFromGUID2(&id, name, 39)) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE); return FALSE;
    }
    wcscat_s(directory, MAX_PATH, L"ChmLink-"); wcscat_s(directory, MAX_PATH, name);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)",
            SDDL_REVISION_1, &security.lpSecurityDescriptor, NULL)) return FALSE;
    if (!CreateDirectoryW(directory, &security)) {
        error = GetLastError(); LocalFree(security.lpSecurityDescriptor); SetLastError(error); return FALSE;
    }
    LocalFree(security.lpSecurityDescriptor);
    for (i = 0; i < 3; ++i) {
        HRSRC found = FindResourceW(module, resources[i], (const wchar_t *)RT_RCDATA);
        HGLOBAL loaded = found ? LoadResource(module, found) : NULL;
        const void *data = loaded ? LockResource(loaded) : NULL;
        DWORD size = found ? SizeofResource(module, found) : 0;
        HANDLE file;
        BOOL ok;
        if (data == NULL || size == 0) { SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND); goto fail; }
        if (swprintf(path, MAX_PATH, L"%ls\\%ls", directory, package_files[i]) < 0) goto fail;
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) goto fail;
        ok = WriteFile(file, data, size, &written, NULL) && written == size;
        error = GetLastError(); CloseHandle(file);
        if (!ok) { SetLastError(error); goto fail; }
    }
    return swprintf(inf, MAX_PATH, L"%ls\\chmlink.inf", directory) > 0;
fail:
    error = GetLastError(); ClRemoveExtracted(directory); SetLastError(error);
    return FALSE;
}

void ClRemoveExtracted(const wchar_t *directory)
{
    wchar_t path[MAX_PATH];
    DWORD error = GetLastError();
    unsigned int i;
    for (i = 0; i < 3; ++i)
        if (swprintf(path, MAX_PATH, L"%ls\\%ls", directory, package_files[i]) > 0) DeleteFileW(path);
    RemoveDirectoryW(directory);
    SetLastError(error);
}

BOOL ClIsOwnPackage(const wchar_t *path)
{
    static const wchar_t *const models[] = {L"Models.NTamd64.10.0...14393", L"Models.NTamd64.10.0...25319"};
    static const wchar_t *const services[] = {L"InstallLegacy.NT.Services", L"InstallStore.NT.Services"};
    wchar_t service[64], id[64];
    INFCONTEXT line;
    BOOL owned = FALSE;
    unsigned int i;
    HINF inf = SetupOpenInfFileW(path, L"Net", INF_STYLE_WIN4, NULL);
    if (inf == INVALID_HANDLE_VALUE) return FALSE;
    /* Accept only this project's known INF schema, never arbitrary OEM files. */
    for (i = 0; i < 2; ++i) {
        if (SetupFindFirstLineW(inf, models[i], NULL, &line) &&
            SetupGetStringFieldW(&line, 2, id, 64, NULL) && _wcsicmp(id, ClHardwareId) == 0 &&
            SetupFindFirstLineW(inf, services[i], L"AddService", &line) &&
            SetupGetStringFieldW(&line, 1, service, 64, NULL) && _wcsicmp(service, L"chmlink") == 0)
            owned = TRUE;
    }
    SetupCloseInfFile(inf);
    return owned;
}

int chmlink_delete_driver(void)
{
    WIN32_FIND_DATAW found;
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    HANDLE search, lock;
    DWORD length, error = ERROR_SUCCESS;
    length = GetWindowsDirectoryW(pattern, MAX_PATH);
    if (!length || length + 16 >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return 0; }
    wcscat_s(pattern, MAX_PATH, L"\\INF\\oem*.inf");
    if (!ClLock(&lock)) return 0;
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        ClUnlock(lock);
        if (error == ERROR_FILE_NOT_FOUND) return 1;
        SetLastError(error); return 0;
    }
    do {
        if (!ClIsOemName(found.cFileName) ||
            swprintf(path, MAX_PATH, L"%.*ls%ls", (int)(length + 5), pattern, found.cFileName) < 0 ||
            !ClIsOwnPackage(path)) continue;
        /* Flags zero: Windows refuses packages of present or phantom devices. */
        if (!SetupUninstallOEMInfW(found.cFileName, 0, NULL)) error = GetLastError();
        else sdk_log(CHMLINK_LOG_INFO, L"Removed driver package %ls.", found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    ClUnlock(lock);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}
