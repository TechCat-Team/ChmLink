#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"

/* SDK resource checks: every handle the SDK creates for a session (device,
 * wait events, per-call completion events) is released by chmlink_close, also
 * after many threads have made calls concurrently. */

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "sdk:%d: %s (error %lu)\n", \
    __LINE__, #x, GetLastError()); ExitProcess(1); } } while (0)

#define SDK_THREADS 16
#define SDK_ROUNDS 400

typedef struct caller {
    chmlink_adapter *adapter;
    HANDLE start;
    uint8_t batch[128];
    uint32_t batch_length;
    uint8_t *buffer;
} caller;

static DWORD handles(void)
{
    DWORD count = 0;
    REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &count));
    return count;
}

/* A frame to a unicast address that is not the adapter's: the driver's
 * receive filter discards it, so the test injects without touching the stack. */
static uint32_t foreign_frame_batch(uint8_t *batch, size_t capacity)
{
    uint8_t frame[60];
    chmlink_batch_writer writer;
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x02; frame[5] = 0x7e;               /* destination 02:00:00:00:00:7e */
    frame[6] = 0x02; frame[11] = 0x7f;              /* source 02:00:00:00:00:7f */
    frame[12] = 0x88; frame[13] = 0xb5;             /* local experimental EtherType */
    REQUIRE(chmlink_batch_begin(&writer, batch, capacity, CHMLINK_MODE_ETHERNET, 1522) == CHMLINK_OK);
    REQUIRE(chmlink_batch_append(&writer, frame, sizeof(frame)) == CHMLINK_OK);
    return (uint32_t)writer.length;
}

static DWORD WINAPI call_worker(void *arg)
{
    caller *c = arg;
    chmlink_adapter_info info;
    uint32_t bytes;
    unsigned int i;
    REQUIRE(WaitForSingleObject(c->start, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < SDK_ROUNDS; ++i) {
        REQUIRE(chmlink_query(c->adapter, &info));
        if (!chmlink_receive(c->adapter, c->buffer, 65536, &bytes))
            REQUIRE(GetLastError() == ERROR_NO_MORE_ITEMS || GetLastError() == ERROR_INSUFFICIENT_BUFFER);
        REQUIRE(chmlink_inject(c->adapter, c->batch, c->batch_length));
    }
    return 0;
}

static void concurrent_calls(uint32_t index)
{
    caller callers[SDK_THREADS];
    HANDLE threads[SDK_THREADS], start;
    chmlink_adapter *a;
    chmlink_adapter_info before, after;
    unsigned int i;
    a = chmlink_open(index); REQUIRE(a != NULL);
    REQUIRE(chmlink_query(a, &before));
    start = CreateEventW(NULL, TRUE, FALSE, NULL); REQUIRE(start != NULL);
    for (i = 0; i < SDK_THREADS; ++i) {
        callers[i].adapter = a; callers[i].start = start;
        callers[i].batch_length = foreign_frame_batch(callers[i].batch, sizeof(callers[i].batch));
        callers[i].buffer = malloc(65536); REQUIRE(callers[i].buffer != NULL);
        threads[i] = CreateThread(NULL, 0, call_worker, &callers[i], 0, NULL); REQUIRE(threads[i] != NULL);
    }
    REQUIRE(SetEvent(start));
    REQUIRE(WaitForMultipleObjects(SDK_THREADS, threads, TRUE, 120000) == WAIT_OBJECT_0);
    for (i = 0; i < SDK_THREADS; ++i) {
        DWORD code = 1;
        REQUIRE(GetExitCodeThread(threads[i], &code) && code == 0);
        CloseHandle(threads[i]); free(callers[i].buffer);
    }
    CloseHandle(start);
    /* Injection is asynchronous: wait for the session thread to account for
     * every frame, then check that each was refused by the receive filter. */
    {
        ULONGLONG deadline = GetTickCount64() + 30000;
        do {
            REQUIRE(chmlink_query(a, &after));
            if (after.rx_discards - before.rx_discards >= (uint64_t)SDK_THREADS * SDK_ROUNDS) break;
            Sleep(10);
        } while (GetTickCount64() < deadline);
    }
    REQUIRE(after.rx_discards - before.rx_discards == (uint64_t)SDK_THREADS * SDK_ROUNDS);
    chmlink_close(a);
}

void lifecycle_sdk_check(uint32_t index)
{
    chmlink_adapter *a;
    chmlink_adapter_info info;
    DWORD baseline, now;
    unsigned int i;
    /* Warm up what the system creates once per process (iphlpapi, NSI). */
    a = chmlink_open(index); REQUIRE(a != NULL);
    REQUIRE(chmlink_query(a, &info));
    chmlink_close(a);
    REQUIRE(chmlink_inspect(index, &info));
    concurrent_calls(index);
    baseline = handles();
    for (i = 0; i < 200; ++i) {
        a = chmlink_open(index); REQUIRE(a != NULL);
        REQUIRE(chmlink_query(a, &info));
        REQUIRE(chmlink_wait_readable(a, 0) || GetLastError() == ERROR_TIMEOUT);
        chmlink_close(a);
        REQUIRE(chmlink_inspect(index, &info));
    }
    now = handles();
    if (now != baseline) fprintf(stderr, "sdk: %lu handles before open/close, %lu after\n", baseline, now);
    REQUIRE(now == baseline);
    concurrent_calls(index);
    now = handles();
    if (now != baseline) fprintf(stderr, "sdk: %lu handles before concurrent calls, %lu after\n", baseline, now);
    REQUIRE(now == baseline);
    printf("PASS SDK releases every handle (%d threads x %d rounds; %lu handles)\n",
           SDK_THREADS, SDK_ROUNDS, baseline);
}
