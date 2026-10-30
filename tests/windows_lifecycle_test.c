#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <wchar.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"

void lifecycle_queue_check(uint32_t first, uint32_t second);

/* Fail the process rather than unwinding a stack still used by a worker.
 * The external runner bounds the whole process, including a stuck SDK close. */
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s (error %lu)\n", __FILE__, __LINE__, #x, GetLastError()); \
    ExitProcess(1); } } while (0)

typedef struct waiter {
    chmlink_adapter *adapter;
    int result;
    DWORD error;
} waiter;

static DWORD WINAPI wait_worker(void *arg)
{
    waiter *w = arg;
    w->result = chmlink_wait_readable(w->adapter, CHMLINK_WAIT_INFINITE);
    w->error = GetLastError();
    return 0; /* No further adapter calls: close may already have returned. */
}

static DWORD WINAPI transition_worker(void *arg)
{
    waiter *w = arg;
    uint8_t *buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    uint32_t bytes;
    CHECK(buffer != NULL);
    /* Normal traffic is not evidence of a lifecycle wake. Consume it and
     * keep waiting until the actual device transition produces an error. */
    while (chmlink_wait_readable(w->adapter, CHMLINK_WAIT_INFINITE)) {
        if (!chmlink_receive(w->adapter, buffer, CHMLINK_BATCH_MAX_SIZE, &bytes) &&
            GetLastError() != ERROR_NO_MORE_ITEMS) break;
    }
    w->result = 0; w->error = GetLastError(); free(buffer); return 0;
}

static void drain(chmlink_adapter *a)
{
    uint8_t *buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    uint32_t size;
    ULONGLONG deadline = GetTickCount64() + 5000, last_packet = GetTickCount64();
    CHECK(buffer != NULL);
    while (GetTickCount64() < deadline) {
        if (!chmlink_receive(a, buffer, CHMLINK_BATCH_MAX_SIZE, &size)) {
            CHECK(GetLastError() == ERROR_NO_MORE_ITEMS);
            if (GetTickCount64() - last_packet >= 200) { free(buffer); return; }
            Sleep(5);
        } else {
            last_packet = GetTickCount64();
        }
    }
    CHECK(!"Traffic did not quiesce; use isolated test adapters");
}

/* ERROR_BUSY proves that the other call has entered the SDK. A start event
 * before the call would not establish that lifetime boundary. */
static int await_waiter(chmlink_adapter *a, HANDLE thread)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) return 0;
        if (!chmlink_wait_readable(a, 0) && GetLastError() == ERROR_BUSY) return 1;
        Sleep(1);
    }
    CHECK(!"Waiter failed to enter"); return 0;
}

static void timeout_check(chmlink_adapter *a)
{
    unsigned int attempt;
    for (attempt = 0; attempt < 64; ++attempt) {
        ULONGLONG started;
        drain(a); started = GetTickCount64();
        if (!chmlink_wait_readable(a, 100)) {
            CHECK(GetLastError() == ERROR_TIMEOUT);
            /* Windows wait timeouts and GetTickCount64 use coarse timer ticks. */
            CHECK(GetTickCount64() - started + 32 >= 100);
            return;
        }
    }
    CHECK(!"No quiet timeout interval");
}

static void wake_check(uint32_t index, int closing)
{
    unsigned int attempt;
    for (attempt = 0; attempt < 64; ++attempt) {
        waiter w = {0};
        HANDLE thread;
        w.adapter = chmlink_open(index); CHECK(w.adapter != NULL);
        drain(w.adapter);
        thread = CreateThread(NULL, 0, wait_worker, &w, 0, NULL); CHECK(thread != NULL);
        if (await_waiter(w.adapter, thread)) {
            if (closing) chmlink_close(w.adapter);
            else CHECK(chmlink_cancel_wait(w.adapter));
            CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
            CloseHandle(thread);
            if (!closing) {
                /* Cancellation is per-wait, not latched into later waits. */
                timeout_check(w.adapter);
                chmlink_close(w.adapter);
            }
            if (!w.result) { CHECK(w.error == ERROR_OPERATION_ABORTED); return; }
            /* Packet readiness may legally win the cancel/close race. */
        } else {
            CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
            CloseHandle(thread); chmlink_close(w.adapter);
            /* The probe can register first and reject the worker instead. */
            CHECK(w.result || w.error == ERROR_BUSY);
        }
    }
    CHECK(!"Could not observe cancellation before packet readiness");
}

static void child_exit_check(uint32_t index)
{
    wchar_t executable[MAX_PATH], command[MAX_PATH + 128], event_name[96];
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    HANDLE ready;
    chmlink_adapter *a;
    DWORD length = GetModuleFileNameW(NULL, executable, MAX_PATH);
    CHECK(length && length < MAX_PATH);
    swprintf(event_name, 96, L"Local\\ChmLinkLifecycle-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    ready = CreateEventW(NULL, TRUE, FALSE, event_name); CHECK(ready != NULL);
    swprintf(command, MAX_PATH + 128, L"\"%ls\" --child %lu %ls", executable, (unsigned long)index, event_name);
    CHECK(CreateProcessW(executable, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process));
    CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0);
    a = chmlink_open(index); CHECK(a == NULL && GetLastError() == ERROR_SHARING_VIOLATION);
    CHECK(TerminateProcess(process.hProcess, 37));
    CHECK(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(ready);
    /* Process exit may precede completion of device cleanup. */
    for (length = 0; length < 500; ++length) {
        a = chmlink_open(index);
        if (a != NULL) { chmlink_close(a); return; }
        CHECK(GetLastError() == ERROR_SHARING_VIOLATION);
        Sleep(10);
    }
    CHECK(!"Session was not reclaimed after process termination");
}

static uint32_t parse_index(const wchar_t *text)
{
    wchar_t *end;
    unsigned long index;
    errno = 0; index = wcstoul(text, &end, 10);
    CHECK(!errno && text[0] >= L'0' && text[0] <= L'9' && !*end && index);
    return (uint32_t)index;
}

int wmain(int argc, wchar_t **argv)
{
    uint32_t first, second;
    unsigned int i;
    chmlink_adapter *a, *b, *duplicate;
    chmlink_adapter_info before, after;
    if (argc == 4 && wcscmp(argv[1], L"--child") == 0) {
        HANDLE ready;
        a = chmlink_open(parse_index(argv[2])); CHECK(a != NULL);
        ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]); CHECK(ready != NULL);
        CHECK(SetEvent(ready)); CloseHandle(ready);
        Sleep(15000); chmlink_close(a); return 1;
    }
    if (argc == 4 && wcscmp(argv[1], L"--transition") == 0) {
        waiter w = {0}; HANDLE thread, ready;
        unsigned int attempt;
        w.adapter = chmlink_open(parse_index(argv[2])); CHECK(w.adapter != NULL);
        drain(w.adapter);
        for (attempt = 0; attempt < 64; ++attempt) {
            thread = CreateThread(NULL, 0, transition_worker, &w, 0, NULL); CHECK(thread != NULL);
            if (await_waiter(w.adapter, thread)) break;
            CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
            CloseHandle(thread); CHECK(w.error == ERROR_BUSY);
        }
        CHECK(attempt != 64);
        ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]); CHECK(ready != NULL);
        CHECK(SetEvent(ready)); CloseHandle(ready);
        CHECK(WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0);
        printf("Transition result=%d error=%lu\n", w.result, w.error);
        CHECK(!w.result && (w.error == ERROR_NOT_READY || w.error == ERROR_DEVICE_REMOVED ||
              w.error == ERROR_DEVICE_NOT_CONNECTED || w.error == ERROR_OPERATION_ABORTED));
        CloseHandle(thread); chmlink_close(w.adapter);
        printf("PASS transition woke waiter (error %lu)\n", w.error); return 0;
    }
    if (argc != 3) { fwprintf(stderr, L"Usage: chmlink-lifecycle-test <first-index> <second-index>\n"); return 2; }
    first = parse_index(argv[1]); second = parse_index(argv[2]); CHECK(first != second);
    a = chmlink_open(first); b = chmlink_open(second); CHECK(a && b);
    CHECK(chmlink_inspect(first, &before) && before.mode == CHMLINK_MODE_ETHERNET);
    duplicate = chmlink_open(first); CHECK(!duplicate && GetLastError() == ERROR_SHARING_VIOLATION);
    CHECK(chmlink_query(b, &after)); CHECK(after.interface_index == second && after.mode == CHMLINK_MODE_ETHERNET);
    timeout_check(a); timeout_check(b);
    CHECK(chmlink_cancel_wait(a)); timeout_check(a); /* no active wait: no latch */
    chmlink_close(a);
    for (i = 0; i < 16; ++i) { wake_check(first, 0); wake_check(first, 1); }
    CHECK(chmlink_query(b, &after)); CHECK(after.interface_index == second);
    chmlink_close(b);
    child_exit_check(first);
    for (i = 0; i < 100; ++i) {
        a = chmlink_open_by_id(before.interface_id); CHECK(a != NULL); chmlink_close(a);
    }
    puts("PASS timeout, cancellation, busy wait, close drain, session isolation, process exit and reopen");
    lifecycle_queue_check(first, second);
    return 0;
}
