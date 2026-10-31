/* chmlink_coalescer: software GRO (chmlink/gro.h) in front of an adapter's
 * zero-copy send path. */
#include <windows.h>
#include "chmlink/api.h"
#include "chmlink/gro.h"
#include "log.h"

struct chmlink_coalescer {
    chmlink_adapter *adapter;
    int merging;
    DWORD error;          /* first failure since the call began */
    chmlink_gro gro;
    uint8_t memory[1];    /* CHMLINK_GRO_MEMORY(limit) */
};

/* Into the ring, waiting while it is full, as chmlink_inject's callers do. */
static void put(const uint8_t *packet, size_t length, const chmlink_offload *offload, void *context)
{
    chmlink_coalescer *c = context;
    uint8_t *slot;
    if (c->error != ERROR_SUCCESS) return;
    for (;;) {
        slot = chmlink_allocate_packet(c->adapter, (uint32_t)length, offload);
        if (slot != NULL) break;
        if (GetLastError() != ERROR_BUFFER_OVERFLOW) { c->error = GetLastError(); return; }
        SwitchToThread();
    }
    memcpy(slot, packet, length);
    chmlink_send_packet(c->adapter, slot);
}

chmlink_coalescer *chmlink_coalescer_create(chmlink_adapter *adapter, uint32_t flags)
{
    chmlink_adapter_info info;
    chmlink_coalescer *c;
    size_t limit;
    if (adapter == NULL || (flags & ~CHMLINK_GRO_TRUSTED)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!chmlink_query(adapter, &info)) return NULL;
    limit = info.mode == CHMLINK_MODE_ETHERNET ? CHMLINK_PACKET_MAX_SIZE : info.packet_limit;
    c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c) + CHMLINK_GRO_MEMORY(limit));
    if (c == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    c->adapter = adapter;
    /* Merging needs somewhere to put the result: room above a usual packet
     * in IP mode, receive coalescing (RSC) granted to the session in Ethernet
     * mode. Otherwise packets pass straight through. */
    c->merging = info.mode == CHMLINK_MODE_ETHERNET ? (info.offload & CHMLINK_OFFLOAD_RX_RSC) != 0
                                                    : info.packet_limit > 1500;
    if (chmlink_gro_init(&c->gro, (chmlink_mode)info.mode, limit, flags, c->memory, CHMLINK_GRO_MEMORY(limit), put, c)
        != CHMLINK_OK) {
        HeapFree(GetProcessHeap(), 0, c);
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!c->merging)
        sdk_log(CHMLINK_LOG_INFO, L"Coalescer passes packets through: %ls.",
                info.mode == CHMLINK_MODE_ETHERNET ? L"the session has no CHMLINK_OFFLOAD_RX_RSC"
                                                   : L"the adapter MTU is 1500 or less");
    return c;
}

int chmlink_coalescer_send(chmlink_coalescer *c, const uint8_t *packet, uint32_t size)
{
    if (c == NULL || packet == NULL || size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    c->error = ERROR_SUCCESS;
    if (c->merging) {
        if (chmlink_gro_add(&c->gro, packet, size) != CHMLINK_OK) c->error = ERROR_INVALID_PARAMETER;
    } else {
        put(packet, size, NULL, c);
    }
    if (c->error != ERROR_SUCCESS) { SetLastError(c->error); return 0; }
    return 1;
}

int chmlink_coalescer_flush(chmlink_coalescer *c)
{
    if (c == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    c->error = ERROR_SUCCESS;
    chmlink_gro_flush(&c->gro);
    if (c->error != ERROR_SUCCESS) { SetLastError(c->error); return 0; }
    return 1;
}

void chmlink_coalescer_free(chmlink_coalescer *c)
{
    if (c == NULL) return;
    (void)chmlink_coalescer_flush(c);
    HeapFree(GetProcessHeap(), 0, c);
}
