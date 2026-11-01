/* chmlink-package-test: the adapter-management steps that tools/test-package.ps1
 * and tools/test-surprise.ps1 drive, run through chmlink.dll with its embedded
 * driver as applications use it. Each command prints one JSON line on stdout and
 * the SDK log on stderr.
 *
 *   create <name>                   create an Ethernet adapter (test owner)
 *   delete <instance id>            delete one of the test owner's adapters
 *   delete-driver                   chmlink_delete_driver
 *   hold <index> <ready> <stop>     keep a data session open and working until
 *                                   the named event <stop> is set
 *   filter <instance id> <service>  set the device's upper filter ("-": none)
 *                                   and restart it
 *   surprise-wait <index> <ready>   keep a session open until the adapter goes
 *                                   away, then check that the session fails
 *                                   cleanly rather than hanging
 *   surprise-trigger                ask the test filter driver to report the
 *                                   adapter as failed (a surprise removal)
 */
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <devguid.h>
#include <stdio.h>
#include <stdlib.h>
#include "chmlink/api.h"

static const GUID test_owner = {0x5f1c2a7e, 0x3b4d, 0x4e8f, {0x9a, 0x10, 0x6c, 0x2d, 0x7e, 0x41, 0x88, 0x3b}};

#define SURPRISE_IOCTL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)

static void print_log(chmlink_log_level level, const wchar_t *message, void *context)
{
    static const wchar_t *const names[] = {L"info", L"warning", L"error"};
    (void)context;
    fwprintf(stderr, L"[%ls] %ls\n", names[level], message);
    fflush(stderr);
}

static int result(BOOL ok)
{
    printf("{\"ok\":%d,\"error\":%lu}\n", ok ? 1 : 0, ok ? 0 : GetLastError());
    return ok ? 0 : 1;
}

static HANDLE open_event(const wchar_t *name)
{
    HANDLE event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    if (event == NULL) printf("{\"ok\":0,\"error\":%lu,\"step\":\"open event\"}\n", GetLastError());
    return event;
}

static int create(const wchar_t *name)
{
    chmlink_create_options options = {sizeof(options)};
    chmlink_device device;
    const wchar_t *c;
    options.name = name;
    options.owner = &test_owner;
    if (!chmlink_create_adapter(&options, &device)) return result(FALSE);
    printf("{\"ok\":1,\"error\":0,\"instance\":\"");
    for (c = device.instance_id; *c; ++c) {
        if (*c == L'\\') printf("\\\\");
        else putchar((char)*c);
    }
    printf("\",\"index\":%lu,\"flags\":%lu}\n", (unsigned long)device.interface_index, (unsigned long)device.flags);
    return 0;
}

static int hold(uint32_t index, const wchar_t *ready_name, const wchar_t *stop_name)
{
    HANDLE ready = open_event(ready_name), stop = open_event(stop_name);
    chmlink_adapter *adapter;
    chmlink_adapter_info info = {0};
    unsigned long queries = 0;
    if (ready == NULL || stop == NULL) return 1;
    adapter = chmlink_open(index);
    if (adapter == NULL) { printf("{\"ok\":0,\"error\":%lu,\"step\":\"open\"}\n", GetLastError()); return 1; }
    SetEvent(ready);
    /* The session must keep working the whole time, whatever else happens. */
    do {
        if (!chmlink_query(adapter, &info) || !(info.state & CHMLINK_STATE_RUNNING)) {
            printf("{\"ok\":0,\"error\":%lu,\"step\":\"query\",\"queries\":%lu,\"state\":%lu}\n",
                   GetLastError(), queries, (unsigned long)info.state);
            chmlink_close(adapter);
            return 1;
        }
        ++queries;
    } while (WaitForSingleObject(stop, 100) == WAIT_TIMEOUT);
    chmlink_close(adapter);
    printf("{\"ok\":1,\"error\":0,\"queries\":%lu}\n", queries);
    return 0;
}

static int set_filter(const wchar_t *instance, const wchar_t *service)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
    SP_DEVINFO_DATA device = {sizeof(device)};
    SP_PROPCHANGE_PARAMS change = {{sizeof(SP_CLASSINSTALL_HEADER), DIF_PROPERTYCHANGE}, DICS_PROPCHANGE, DICS_FLAG_GLOBAL, 0};
    wchar_t filters[64] = {0};
    BOOL ok;
    if (set == INVALID_HANDLE_VALUE) return result(FALSE);
    ok = SetupDiOpenDeviceInfoW(set, instance, NULL, 0, &device);
    if (ok && wcscmp(service, L"-") != 0) {
        wcsncpy_s(filters, 62, service, _TRUNCATE);
        ok = SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_UPPERFILTERS, (const BYTE *)filters,
                                               (DWORD)((wcslen(filters) + 2) * sizeof(wchar_t)));
    } else if (ok) {
        ok = SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_UPPERFILTERS, NULL, 0);
    }
    ok = ok && SetupDiSetClassInstallParamsW(set, &device, &change.ClassInstallHeader, sizeof(change)) &&
         SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device);
    result(ok);
    SetupDiDestroyDeviceInfoList(set);
    return ok ? 0 : 1;
}

static int surprise_wait(uint32_t index, const wchar_t *ready_name)
{
    HANDLE ready = open_event(ready_name);
    chmlink_adapter *adapter;
    chmlink_adapter_info info;
    ULONGLONG deadline, failed_at;
    DWORD query_error = 0, wait_error = 0, wait_ms;
    uint8_t batch[256] = {0};
    if (ready == NULL) return 1;
    adapter = chmlink_open(index);
    if (adapter == NULL) { printf("{\"ok\":0,\"error\":%lu,\"step\":\"open\"}\n", GetLastError()); return 1; }
    SetEvent(ready);
    /* A blocked wait must end when the adapter goes away. */
    deadline = GetTickCount64() + 60000;
    for (;;) {
        if (!chmlink_wait_readable(adapter, 200)) {
            wait_error = GetLastError();
            if (wait_error != ERROR_TIMEOUT && wait_error != ERROR_NOT_READY) break;
        } else {
            const uint8_t *packet;
            uint32_t size;
            while ((packet = chmlink_receive_packet(adapter, &size, NULL)) != NULL) chmlink_release_packet(adapter, packet);
        }
        if (!chmlink_query(adapter, &info)) { query_error = GetLastError(); break; }
        if (GetTickCount64() > deadline) {
            printf("{\"ok\":0,\"error\":0,\"step\":\"the session never noticed the removal\"}\n");
            chmlink_close(adapter);
            return 1;
        }
    }
    /* After the removal every call fails promptly; none blocks. */
    failed_at = GetTickCount64();
    if (chmlink_wait_readable(adapter, 5000) || chmlink_inject(adapter, batch, sizeof(batch))) {
        printf("{\"ok\":0,\"error\":0,\"step\":\"a call succeeded on a removed adapter\"}\n");
        chmlink_close(adapter);
        return 1;
    }
    wait_ms = (DWORD)(GetTickCount64() - failed_at);
    chmlink_close(adapter);
    printf("{\"ok\":%d,\"error\":0,\"query_error\":%lu,\"wait_error\":%lu,\"second_wait_ms\":%lu}\n",
           wait_ms < 4000, query_error, wait_error, wait_ms);
    return wait_ms < 4000 ? 0 : 1;
}

static int surprise_trigger(void)
{
    DWORD returned;
    HANDLE filter = CreateFileW(L"\\\\.\\ChmLinkSurprise", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    BOOL ok;
    if (filter == INVALID_HANDLE_VALUE) return result(FALSE);
    ok = DeviceIoControl(filter, SURPRISE_IOCTL, NULL, 0, NULL, 0, &returned, NULL);
    result(ok);
    CloseHandle(filter);
    return ok ? 0 : 1;
}

int wmain(int argc, wchar_t **argv)
{
    chmlink_set_logger(print_log, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && wcscmp(argv[1], L"create") == 0) return create(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"delete") == 0) return result(chmlink_delete_adapter(argv[2], &test_owner));
    if (argc == 2 && wcscmp(argv[1], L"delete-driver") == 0) { result(chmlink_delete_driver()); return 0; }
    if (argc == 5 && wcscmp(argv[1], L"hold") == 0) return hold((uint32_t)wcstoul(argv[2], NULL, 10), argv[3], argv[4]);
    if (argc == 4 && wcscmp(argv[1], L"filter") == 0) return set_filter(argv[2], argv[3]);
    if (argc == 4 && wcscmp(argv[1], L"surprise-wait") == 0) return surprise_wait((uint32_t)wcstoul(argv[2], NULL, 10), argv[3]);
    if (argc == 2 && wcscmp(argv[1], L"surprise-trigger") == 0) return surprise_trigger();
    fwprintf(stderr, L"usage: see the comment at the top of tests/windows/package_test.c\n");
    return 2;
}
