/* Waiting for packets from Windows, and cancelling a wait. */
#include "internal.h"

int chmlink_cancel_wait(chmlink_adapter *a)
{
    if (a == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    AcquireSRWLockExclusive(&a->gate);
    if (a->closing) {
        ReleaseSRWLockExclusive(&a->gate);
        SetLastError(ERROR_OPERATION_ABORTED); return 0;
    }
    if (a->waiting) SetEvent(a->cancel_event);
    ReleaseSRWLockExclusive(&a->gate);
    return 1;
}

/* Before sleeping, a wait polls the ring for up to CHMLINK_WAIT_POLL_US, but
 * only while packets have recently come within that time of a wait's start
 * (CHMLINK_WAIT_POLL_US is also the threshold): at high rates and in
 * request-response traffic polling spares a sleep and a wake-up per packet and
 * keeps latency low; with packets further apart it would only keep the core
 * busy. A request-response exchange stays under the threshold even without
 * polling, so it turns polling back on. */
#define CHMLINK_WAIT_POLL_US 64

int chmlink_wait_readable(chmlink_adapter *a, uint32_t timeout_ms)
{
    HANDLE events[3];
    ULONGLONG started = GetTickCount64();
    LARGE_INTEGER frequency, entered, now;
    LONGLONG window, sample;
    int result = 0, poll;
    DWORD error = ERROR_SUCCESS, remaining = timeout_ms;
    if (!sdk_enter(a, NULL)) return 0;
    AcquireSRWLockExclusive(&a->gate);
    if (a->closing || a->waiting) {
        error = a->closing ? ERROR_OPERATION_ABORTED : ERROR_BUSY;
        ReleaseSRWLockExclusive(&a->gate);
        SetLastError(error); return sdk_leave(a, NULL, 0);
    }
    a->waiting = TRUE;
    ResetEvent(a->cancel_event);
    ReleaseSRWLockExclusive(&a->gate);
    events[0] = a->close_event; events[1] = a->cancel_event; events[2] = a->from_event;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&entered);
    window = frequency.QuadPart * CHMLINK_WAIT_POLL_US / 1000000;
    poll = a->wait_gap < window;
    for (;;) {
        uint32_t state;
        ULONGLONG elapsed;
        DWORD wait;
        if (WaitForSingleObject(events[0], 0) == WAIT_OBJECT_0 ||
            WaitForSingleObject(events[1], 0) == WAIT_OBJECT_0) {
            error = ERROR_OPERATION_ABORTED; break;
        }
        if (sdk_rx_pending(a)) { result = 1; break; }
        state = chmlink_ring_load(&a->from, CHMLINK_RING_STATE_OFFSET);
        if (state & CHMLINK_RING_STATE_CLOSED) { error = ERROR_OPERATION_ABORTED; break; }
        if (!(state & CHMLINK_RING_STATE_RUNNING)) { error = ERROR_NOT_READY; break; }
        elapsed = GetTickCount64() - started;
        if (timeout_ms != CHMLINK_WAIT_INFINITE && elapsed >= timeout_ms) {
            error = ERROR_TIMEOUT; break;
        }
        if (timeout_ms != CHMLINK_WAIT_INFINITE) remaining = timeout_ms - (DWORD)elapsed;
        /* Poll briefly first: at high packet rates the next record arrives
         * within microseconds, and staying out of the alertable state spares
         * the driver a KeSetEvent and us a sleep and wake per few packets. */
        if (poll) {
            LARGE_INTEGER spun;
            unsigned spin;
            BOOL arrived = FALSE;
            QueryPerformanceCounter(&spun);
            for (;;) {
                for (spin = 0; spin < 32; ++spin) {
                    if (chmlink_ring_load(&a->from, CHMLINK_RING_TAIL_OFFSET) != a->rx_cursor) { arrived = TRUE; break; }
                    YieldProcessor();
                }
                QueryPerformanceCounter(&now);
                if (arrived || now.QuadPart - spun.QuadPart >= window) break;
            }
            poll = FALSE;   /* once per wait */
            if (arrived) continue;
        }
        /* Announce that a wake-up is needed before re-reading the tail, so a
         * publish cannot slip between the check and the wait. */
        chmlink_ring_store(&a->from, CHMLINK_RING_WAIT_OFFSET, 1);
        if (chmlink_ring_load(&a->from, CHMLINK_RING_TAIL_OFFSET) != a->rx_cursor) {
            chmlink_ring_store(&a->from, CHMLINK_RING_WAIT_OFFSET, 0);
            continue;
        }
        wait = WaitForMultipleObjects(3, events, FALSE, remaining);
        chmlink_ring_store(&a->from, CHMLINK_RING_WAIT_OFFSET, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) {
            error = ERROR_OPERATION_ABORTED; break;
        }
        if (wait == WAIT_FAILED) { error = GetLastError(); break; }
    }
    /* How long packets took to come, smoothed; only the one waiter updates it. */
    QueryPerformanceCounter(&now);
    sample = now.QuadPart - entered.QuadPart;
    if (sample > 16 * window) sample = 16 * window;
    a->wait_gap += (sample - a->wait_gap) / 8;
    AcquireSRWLockExclusive(&a->gate);
    a->waiting = FALSE;
    ReleaseSRWLockExclusive(&a->gate);
    if (!result) SetLastError(error);
    return sdk_leave(a, NULL, result);
}
