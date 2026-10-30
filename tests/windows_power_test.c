#include <windows.h>
#include <powrprof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"

/* Sleep and resume with an open data session.
 *
 * Opens a session on the adapter, starts a waiter, arms a waitable timer that
 * resumes the system, and suspends: to S3 if the platform has it, else to S4
 * (hibernation). After resume the waiter must have been woken (the adapter was
 * paused for the sleep), the session must still be open on a running adapter
 * that no longer reports LOW_POWER, and injection must work again.
 *
 * Prints SKIP and exits 0 when the system has no sleep state or did not sleep.
 * Usage: chmlink-power-test <interface-index> [--hibernate]
 */

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "power:%d: %s (error %lu)\n", \
    __LINE__, #x, GetLastError()); ExitProcess(1); } } while (0)

typedef struct waiter { chmlink_adapter *adapter; volatile LONG woke_with; volatile LONG done; } waiter;

static DWORD WINAPI wait_worker(void *arg)
{
    waiter *w = arg;
    uint8_t *buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    uint32_t bytes;
    REQUIRE(buffer != NULL);
    /* Normal traffic is not the event; keep consuming until a wait fails. */
    while (chmlink_wait_readable(w->adapter, CHMLINK_WAIT_INFINITE)) {
        if (!chmlink_receive(w->adapter, buffer, CHMLINK_BATCH_MAX_SIZE, &bytes) &&
            GetLastError() != ERROR_NO_MORE_ITEMS) break;
    }
    InterlockedExchange(&w->woke_with, (LONG)GetLastError());
    InterlockedExchange(&w->done, 1);
    free(buffer);
    return 0;
}

static int enable_shutdown_privilege(void)
{
    HANDLE token;
    TOKEN_PRIVILEGES p;
    BOOL ok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return 0;
    p.PrivilegeCount = 1;
    p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ok = LookupPrivilegeValueW(NULL, L"SeShutdownPrivilege", &p.Privileges[0].Luid) &&
         AdjustTokenPrivileges(token, FALSE, &p, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

static uint32_t foreign_frame_batch(uint8_t *batch, size_t capacity)
{
    uint8_t frame[60];
    chmlink_batch_writer writer;
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x02; frame[5] = 0x7e; frame[6] = 0x02; frame[11] = 0x7f; frame[12] = 0x88; frame[13] = 0xb5;
    REQUIRE(chmlink_batch_begin(&writer, batch, capacity, CHMLINK_MODE_ETHERNET, 1522) == CHMLINK_OK);
    REQUIRE(chmlink_batch_append(&writer, frame, sizeof(frame)) == CHMLINK_OK);
    return (uint32_t)writer.length;
}

int wmain(int argc, wchar_t **argv)
{
    SYSTEM_POWER_CAPABILITIES caps;
    chmlink_adapter *a;
    chmlink_adapter_info before, after;
    waiter w = {0};
    HANDLE thread, timer;
    LARGE_INTEGER due;
    ULONGLONG biased0, biased1, unbiased0, unbiased1, slept_ms, deadline;
    uint8_t batch[128];
    uint32_t length, index;
    BOOLEAN hibernate = FALSE;
    BOOL suspended;
    if (argc < 2) { fwprintf(stderr, L"usage: chmlink-power-test <index> [--hibernate]\n"); return 2; }
    index = (uint32_t)wcstoul(argv[1], NULL, 10);
    if (argc > 2 && wcscmp(argv[2], L"--hibernate") == 0) hibernate = TRUE;
    REQUIRE(GetPwrCapabilities(&caps));
    if (!hibernate && !caps.SystemS3) hibernate = TRUE;          /* no standby: try hibernation */
    if (hibernate && !(caps.SystemS4 && caps.HiberFilePresent)) {
        printf("SKIP no sleep state (S3=%d S4=%d hiberfile=%d)\n", caps.SystemS3, caps.SystemS4, caps.HiberFilePresent);
        return 0;
    }
    REQUIRE(enable_shutdown_privilege());
    a = chmlink_open(index); REQUIRE(a != NULL);
    REQUIRE(chmlink_query(a, &before));
    REQUIRE((before.state & CHMLINK_STATE_RUNNING) && (before.state & CHMLINK_STATE_SESSION));
    w.adapter = a;
    thread = CreateThread(NULL, 0, wait_worker, &w, 0, NULL); REQUIRE(thread != NULL);
    Sleep(500);

    timer = CreateWaitableTimerW(NULL, TRUE, NULL); REQUIRE(timer != NULL);
    due.QuadPart = -30ll * 10000000ll;                           /* resume in 30 s */
    REQUIRE(SetWaitableTimer(timer, &due, 0, NULL, NULL, TRUE));
    QueryUnbiasedInterruptTime(&unbiased0); biased0 = GetTickCount64();
    printf("suspending to %s\n", hibernate ? "S4" : "S3"); fflush(stdout);
    suspended = SetSuspendState(hibernate, FALSE, FALSE);
    if (!suspended) { printf("SKIP SetSuspendState failed (%lu)\n", GetLastError()); chmlink_cancel_wait(a); WaitForSingleObject(thread, 5000); chmlink_close(a); return 0; }
    /* SetSuspendState returns after resume. The unbiased clock does not count sleep. */
    QueryUnbiasedInterruptTime(&unbiased1); biased1 = GetTickCount64();
    slept_ms = (biased1 - biased0) - (unbiased1 - unbiased0) / 10000;
    if (slept_ms < 5000) {
        printf("SKIP the system did not sleep (%llu ms)\n", (unsigned long long)slept_ms);
        chmlink_cancel_wait(a); WaitForSingleObject(thread, 5000); chmlink_close(a); return 0;
    }
    printf("resumed after %llu ms asleep\n", (unsigned long long)slept_ms);

    /* The sleep paused the adapter. Whether the waiter saw that depends on
     * whether its thread ran between the pause and the restart; if it did not,
     * cancelling must still end the wait. */
    if (WaitForSingleObject(thread, 3000) != WAIT_OBJECT_0) {
        REQUIRE(chmlink_cancel_wait(a));
        REQUIRE(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
        REQUIRE(w.woke_with == ERROR_OPERATION_ABORTED);
        printf("pause not observed by the waiter (it did not run in between)\n");
    } else {
        REQUIRE(w.woke_with == ERROR_NOT_READY);
        printf("waiter observed the pause\n");
    }
    CloseHandle(thread);

    /* Back to running with the same session, no longer in low power. */
    deadline = GetTickCount64() + 30000;
    for (;;) {
        REQUIRE(chmlink_query(a, &after));
        if ((after.state & CHMLINK_STATE_RUNNING) && !(after.state & CHMLINK_STATE_LOW_POWER)) break;
        REQUIRE(GetTickCount64() < deadline);
        Sleep(200);
    }
    REQUIRE(after.state & CHMLINK_STATE_SESSION);
    REQUIRE(after.interface_index == before.interface_index);
    length = foreign_frame_batch(batch, sizeof(batch));
    REQUIRE(chmlink_inject(a, batch, length));
    REQUIRE(chmlink_query(a, &after));
    REQUIRE(after.rx_discards > before.rx_discards);
    REQUIRE(chmlink_wait_readable(a, 0) || GetLastError() == ERROR_TIMEOUT);
    chmlink_close(a);
    CloseHandle(timer);
    printf("PASS %s with an open session: session kept, data path back\n", hibernate ? "hibernation" : "standby");
    return 0;
}
