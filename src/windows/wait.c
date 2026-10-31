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

/* About 20-50 microseconds of PAUSE on current x86 cores. */
#define CHMLINK_WAIT_SPIN 2048u

int chmlink_wait_readable(chmlink_adapter *a, uint32_t timeout_ms)
{
    HANDLE events[3];
    ULONGLONG started = GetTickCount64();
    int result = 0;
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
        {
            unsigned spin;
            for (spin = 0; spin < CHMLINK_WAIT_SPIN; ++spin) {
                if (chmlink_ring_load(&a->from, CHMLINK_RING_TAIL_OFFSET) != a->rx_cursor) break;
                YieldProcessor();
            }
            if (spin < CHMLINK_WAIT_SPIN) continue;
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
    AcquireSRWLockExclusive(&a->gate);
    a->waiting = FALSE;
    ReleaseSRWLockExclusive(&a->gate);
    if (!result) SetLastError(error);
    return sdk_leave(a, NULL, result);
}
