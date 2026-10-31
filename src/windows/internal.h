/* Internal to the C SDK (src/windows): the adapter object and helpers shared
 * by adapter.c, receive.c, send.c and wait.c. Not installed or exported. The
 * sdk_ prefix keeps these out of applications' names when the static library
 * is linked in. */
#ifndef CHMLINK_SDK_INTERNAL_H
#define CHMLINK_SDK_INTERNAL_H

#include <windows.h>
#include <string.h>
#include "chmlink/api.h"
#include "chmlink/device.h"
#include "chmlink/protocol.h"
#include "chmlink/ring.h"
#include "chmlink/session.h"

/* Completion events for the remaining IOCTLs (QUERY, SET_OFFLOAD,
 * REGISTER_RINGS, ADD_INJECT_RING) are cached per adapter: a call takes one in
 * sdk_enter() and returns it in sdk_leave(). Ring calls do not need an event and do
 * not take one. The cache holds at most CHMLINK_EVENT_CACHE events, enough for
 * the usual query and offload threads; more concurrent calls create and close
 * their own. Close frees them all. Everything, including the table that gives
 * each injecting thread its ring, lives in the adapter: no thread-local or
 * process-wide state, so the SDK can be linked into a DLL that is unloaded. */
#define CHMLINK_EVENT_CACHE 8
#define CHMLINK_DEFAULT_RING (4u * 1024u * 1024u)
#define CHMLINK_RX_ENTRIES_MIN 1024u
#define CHMLINK_TX_ENTRIES_MIN 256u

/* A record read but not released yet. start is the record header offset in
 * the ring, end the position after it. */
typedef struct chmlink_rx_entry {
    uint32_t start;
    uint32_t end;
    BOOL released;
} chmlink_rx_entry;

/* A reserved but not published send record. */
typedef struct chmlink_tx_entry {
    uint32_t start;
    uint32_t end;
    BOOL published;
} chmlink_tx_entry;

/* One to-Windows ring with its producer state. The driver indicates each ring
 * from its own thread, so threads injecting into different rings are
 * processed by the stack in parallel; order holds within a ring. */
typedef struct chmlink_txq {
    void *memory;
    chmlink_ring ring;
    HANDLE event;
    SRWLOCK lock;
    uint32_t cursor;               /* next free record position */
    chmlink_tx_entry *entries;
    uint32_t count, entries_capacity, head;
} chmlink_txq;

/* Injecting threads remembered for queue assignment; later ones share. */
#define CHMLINK_TXQ_THREADS 64u

struct chmlink_adapter {
    HANDLE device;
    uint32_t packet_limit;
    chmlink_mode mode;
    uint32_t capabilities;
    uint32_t offload;
    HANDLE cancel_event, close_event;
    SRWLOCK gate;
    CONDITION_VARIABLE drained;
    ULONG active;
    BOOL closing, waiting;
    ULONG cached;
    HANDLE events[CHMLINK_EVENT_CACHE];

    void *from_memory;
    uint32_t from_capacity, to_capacity;
    chmlink_ring from;
    HANDLE from_event;

    SRWLOCK rx_lock;
    uint32_t rx_cursor;            /* next record to read */
    chmlink_rx_entry *rx_entries;
    uint32_t rx_count, rx_entries_capacity, rx_head;

    /* txq[0] is registered with the session; the first injecting thread
     * uses it, each further one gets a new ring (ADD_INJECT_RING) up to
     * CHMLINK_INJECT_RINGS_MAX, after which threads share. txq_count only
     * grows, after the new queue is complete. */
    chmlink_txq txq[CHMLINK_INJECT_RINGS_MAX];
    volatile LONG txq_count;
    SRWLOCK txq_gate;              /* the thread table and queue creation */
    DWORD txq_threads[CHMLINK_TXQ_THREADS];
    uint8_t txq_of[CHMLINK_TXQ_THREADS];
    uint32_t txq_mapped;
};

/* adapter.c */
HANDLE sdk_new_event(void);
/* Admit a call (fails once closing) and give it a completion event when asked;
 * every successful sdk_enter is paired with one sdk_leave. */
int sdk_enter(chmlink_adapter *a, HANDLE *event);
int sdk_leave(chmlink_adapter *a, HANDLE event, int result);
BOOL sdk_ioctl(HANDLE device, HANDLE event, DWORD code, const void *input, DWORD input_size,
               void *output, DWORD output_size, DWORD *returned);
int sdk_create_ring(void **memory, chmlink_ring *ring, uint32_t capacity);

/* receive.c */
int sdk_rx_pending(chmlink_adapter *a);

#endif
