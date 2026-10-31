/* Wintun back end for the benchmarks, for a like-for-like comparison.
 *
 * Built into wintun-bench and wintun-tcp-bench together with the unchanged
 * bench/packet_bench.c and bench/tcp_bench.c, it implements the few SDK calls
 * they use on top of wintun.dll (0.14.1 API, loaded at run time from the
 * executable's directory), so both drivers run the same scenarios, threads
 * and accounting:
 *
 *   chmlink_open        creates a Wintun adapter, starts a session with 4 MiB
 *                       rings (the ChmLink SDK's default), sets the interface
 *                       MTU to ChmLink's 1500 and assigns the address from
 *                       CHMLINK_WINTUN_ADDRESS (for example 198.18.0.1/24).
 *                       The interface index argument is ignored.
 *   chmlink_receive     copies packets out of the ring into one batch, as the
 *                       ChmLink SDK does.
 *   chmlink_inject      copies a batch into the ring. Wintun has no all-or-
 *                       nothing batch: the batch fails only when its first
 *                       packet does not fit; later ones wait for room.
 *   chmlink_wait_readable  polls the ring briefly before waiting on the read
 *                       event, with the same spin as the ChmLink SDK and as
 *                       Wintun's own documentation recommends.
 *   chmlink_set_offload grants nothing: Wintun has no offloads.
 *
 * Driver counters are not available and read as zero. Wintun is IP only. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"

/* wintun.h 0.14.1 signatures, declared here rather than copied. */
typedef void *WINTUN_ADAPTER_HANDLE;
typedef void *WINTUN_SESSION_HANDLE;
typedef WINTUN_ADAPTER_HANDLE(WINAPI *create_adapter_fn)(LPCWSTR, LPCWSTR, const GUID *);
typedef void(WINAPI *close_adapter_fn)(WINTUN_ADAPTER_HANDLE);
typedef void(WINAPI *get_luid_fn)(WINTUN_ADAPTER_HANDLE, NET_LUID *);
typedef WINTUN_SESSION_HANDLE(WINAPI *start_session_fn)(WINTUN_ADAPTER_HANDLE, DWORD);
typedef void(WINAPI *end_session_fn)(WINTUN_SESSION_HANDLE);
typedef HANDLE(WINAPI *read_event_fn)(WINTUN_SESSION_HANDLE);
typedef BYTE *(WINAPI *receive_fn)(WINTUN_SESSION_HANDLE, DWORD *);
typedef void(WINAPI *release_fn)(WINTUN_SESSION_HANDLE, const BYTE *);
typedef BYTE *(WINAPI *allocate_fn)(WINTUN_SESSION_HANDLE, DWORD);
typedef void(WINAPI *send_fn)(WINTUN_SESSION_HANDLE, const BYTE *);

#define RING_CAPACITY (4u * 1024u * 1024u)
#define WAIT_SPIN 2048u               /* as CHMLINK_WAIT_SPIN in the SDK */

static uint32_t packet_limit = 1500;  /* the interface MTU set below */

static create_adapter_fn create_adapter;
static close_adapter_fn close_adapter;
static get_luid_fn get_luid;
static start_session_fn start_session;
static end_session_fn end_session;
static read_event_fn read_event;
static receive_fn receive_packet;
static release_fn release_packet;
static allocate_fn allocate_packet;
static send_fn send_packet;

struct chmlink_adapter {
    WINTUN_ADAPTER_HANDLE adapter;
    WINTUN_SESSION_HANDLE session;
    HANDLE event;
    NET_LUID luid;
    uint32_t index;
    SRWLOCK lock;                     /* the held packet */
    BYTE *held;                       /* received while polling, not yet copied out */
    DWORD held_size;
};

static int load(void)
{
    HMODULE dll = LoadLibraryExW(L"wintun.dll", NULL,
                                 LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dll == NULL) return 0;
#define GET(var, type, name) var = (type)GetProcAddress(dll, name)
    GET(create_adapter, create_adapter_fn, "WintunCreateAdapter");
    GET(close_adapter, close_adapter_fn, "WintunCloseAdapter");
    GET(get_luid, get_luid_fn, "WintunGetAdapterLUID");
    GET(start_session, start_session_fn, "WintunStartSession");
    GET(end_session, end_session_fn, "WintunEndSession");
    GET(read_event, read_event_fn, "WintunGetReadWaitEvent");
    GET(receive_packet, receive_fn, "WintunReceivePacket");
    GET(release_packet, release_fn, "WintunReleaseReceivePacket");
    GET(allocate_packet, allocate_fn, "WintunAllocateSendPacket");
    GET(send_packet, send_fn, "WintunSendPacket");
#undef GET
    return create_adapter && close_adapter && get_luid && start_session && end_session && read_event &&
           receive_packet && release_packet && allocate_packet && send_packet;
}

/* Wintun's interface MTU defaults to 65535, which lets the stack send IP
 * packets of up to 64 KiB, as large send offload would. The comparison runs
 * both drivers at ChmLink's 1500 (CHMLINK_WINTUN_MTU overrides it). */
static int set_mtu(const NET_LUID *luid)
{
    char text[16];
    ULONG mtu = 1500;
    ADDRESS_FAMILY families[2] = { AF_INET, AF_INET6 };
    int i;
    if (GetEnvironmentVariableA("CHMLINK_WINTUN_MTU", text, sizeof(text)) != 0) mtu = (ULONG)atoi(text);
    if (mtu < 576 || mtu > 65535) return 0;
    packet_limit = mtu;
    for (i = 0; i < 2; ++i) {
        MIB_IPINTERFACE_ROW row;
        InitializeIpInterfaceEntry(&row);
        row.InterfaceLuid = *luid;
        row.Family = families[i];
        if (GetIpInterfaceEntry(&row) != NO_ERROR) continue;   /* IPv6 may be unbound */
        row.NlMtu = mtu;
        row.SitePrefixLength = 0;   /* must be zero for IPv4 when setting */
        if (SetIpInterfaceEntry(&row) != NO_ERROR && families[i] == AF_INET) return 0;
    }
    return 1;
}

/* "a.b.c.d/len" from CHMLINK_WINTUN_ADDRESS, assigned and waited on until usable. */
static int assign_address(const NET_LUID *luid)
{
    char text[64], *slash;
    MIB_UNICASTIPADDRESS_ROW row;
    DWORD len = GetEnvironmentVariableA("CHMLINK_WINTUN_ADDRESS", text, sizeof(text));
    int i;
    if (len == 0 || len >= sizeof(text) || (slash = strchr(text, '/')) == NULL) return 0;
    *slash = 0;
    InitializeUnicastIpAddressEntry(&row);
    row.InterfaceLuid = *luid;
    row.Address.Ipv4.sin_family = AF_INET;
    if (inet_pton(AF_INET, text, &row.Address.Ipv4.sin_addr) != 1) return 0;
    row.OnLinkPrefixLength = (UINT8)atoi(slash + 1);
    row.DadState = IpDadStatePreferred;
    {
        DWORD error = CreateUnicastIpAddressEntry(&row);
        if (error != NO_ERROR && error != ERROR_OBJECT_ALREADY_EXISTS) {
            fprintf(stderr, "wintun shim: CreateUnicastIpAddressEntry %lu\n", error);
            return 0;
        }
    }
    for (i = 0; i < 200; ++i) {
        if (GetUnicastIpAddressEntry(&row) == NO_ERROR && row.DadState == IpDadStatePreferred) return 1;
        Sleep(50);
    }
    fprintf(stderr, "wintun shim: address state %d after 10 s\n", (int)row.DadState);
    return 0;
}

chmlink_adapter *chmlink_open(uint32_t interface_index)
{
    /* Fixed GUID so a leftover adapter from an aborted run is reused by name. */
    static const GUID guid = { 0x5c1f3b2a, 0x7d44, 0x4e0b, { 0x9a, 0x61, 0x2b, 0x8e, 0x41, 0xc7, 0x0d, 0x93 } };
    chmlink_adapter *a;
    NET_IFINDEX index = 0;
    (void)interface_index;
    if (!load()) { SetLastError(ERROR_MOD_NOT_FOUND); return NULL; }
    a = calloc(1, sizeof(*a));
    if (a == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    InitializeSRWLock(&a->lock);
    a->adapter = create_adapter(L"WintunBench", L"Bench", &guid);
    if (a->adapter == NULL) { free(a); return NULL; }
    get_luid(a->adapter, &a->luid);
    if (ConvertInterfaceLuidToIndex(&a->luid, &index) == NO_ERROR) a->index = index;
    /* The link comes up with the session; an address assigned before stays
     * tentative until then. */
    a->session = start_session(a->adapter, RING_CAPACITY);
    if (a->session == NULL) { close_adapter(a->adapter); free(a); return NULL; }
    a->event = read_event(a->session);
    if (!set_mtu(&a->luid) || !assign_address(&a->luid)) {
        fprintf(stderr, "wintun shim: cannot set the MTU or assign CHMLINK_WINTUN_ADDRESS\n");
        end_session(a->session); close_adapter(a->adapter); free(a);
        SetLastError(ERROR_INVALID_PARAMETER); return NULL;
    }
    Sleep(2000);   /* let address registration settle, as bench.ps1 does */
    return a;
}

void chmlink_close(chmlink_adapter *a)
{
    if (a == NULL) return;
    if (a->held != NULL) release_packet(a->session, a->held);
    end_session(a->session);
    close_adapter(a->adapter);   /* also removes the adapter */
    free(a);
}

int chmlink_query(chmlink_adapter *a, chmlink_adapter_info *info)
{
    if (a == NULL || info == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    memset(info, 0, sizeof(*info));
    info->mode = CHMLINK_MODE_IP;
    info->capabilities = CHMLINK_CAP_IP | CHMLINK_CAP_BATCH | CHMLINK_CAP_WAIT;
    info->interface_index = a->index;
    info->mtu = packet_limit;
    info->packet_limit = packet_limit;
    info->ring_capacity = RING_CAPACITY;
    return 1;
}

int chmlink_set_offload(chmlink_adapter *a, uint32_t requested, uint32_t *granted)
{
    (void)a; (void)requested;
    if (granted != NULL) *granted = 0;
    return 1;
}

/* The next packet: the held one, else one from the ring. */
static BYTE *next_packet(chmlink_adapter *a, DWORD *size)
{
    BYTE *p;
    AcquireSRWLockExclusive(&a->lock);
    p = a->held; *size = a->held_size; a->held = NULL;
    ReleaseSRWLockExclusive(&a->lock);
    return p != NULL ? p : receive_packet(a->session, size);
}

int chmlink_receive(chmlink_adapter *a, void *buffer, uint32_t capacity, uint32_t *bytes)
{
    chmlink_batch_writer writer;
    BYTE *p;
    DWORD size;
    uint32_t count = 0;
    if (bytes != NULL) *bytes = 0;
    if (a == NULL || buffer == NULL || bytes == NULL ||
        chmlink_batch_begin(&writer, buffer, capacity, CHMLINK_MODE_IP, packet_limit) != CHMLINK_OK) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    while (count < CHMLINK_BATCH_MAX_PACKETS && (p = next_packet(a, &size)) != NULL) {
        if (chmlink_batch_append(&writer, p, size) != CHMLINK_OK) {
            AcquireSRWLockExclusive(&a->lock);   /* keep it for the next call */
            a->held = p; a->held_size = size;
            ReleaseSRWLockExclusive(&a->lock);
            if (count == 0) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            break;
        }
        release_packet(a->session, p);
        count++;
    }
    if (count == 0) {
        if (GetLastError() != ERROR_HANDLE_EOF) SetLastError(ERROR_NO_MORE_ITEMS);
        return 0;
    }
    *bytes = (uint32_t)writer.length;
    return 1;
}

int chmlink_inject(chmlink_adapter *a, const void *batch, uint32_t length)
{
    chmlink_batch_reader reader;
    const uint8_t *packet;
    uint32_t size, count;
    chmlink_offload descriptor;
    int first = 1;
    if (a == NULL || batch == NULL ||
        chmlink_batch_validate(batch, length, CHMLINK_MODE_IP, packet_limit, &count) != CHMLINK_OK || count == 0) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    chmlink_batch_read_begin(&reader, batch);
    while (chmlink_batch_read_next(&reader, &packet, &size, &descriptor)) {
        BYTE *slot;
        while ((slot = allocate_packet(a->session, size)) == NULL) {
            DWORD e = GetLastError();
            if (e != ERROR_BUFFER_OVERFLOW) return 0;
            if (first) return 0;   /* nothing taken: the caller retries the batch */
            SwitchToThread();
        }
        memcpy(slot, packet, size);
        send_packet(a->session, slot);
        first = 0;
    }
    return 1;
}

int chmlink_wait_readable(chmlink_adapter *a, uint32_t timeout_ms)
{
    unsigned spin;
    DWORD size, wait;
    BYTE *p;
    if (a == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    AcquireSRWLockShared(&a->lock);
    p = a->held;
    ReleaseSRWLockShared(&a->lock);
    if (p != NULL) return 1;
    for (spin = 0; spin < WAIT_SPIN; ++spin) {
        p = receive_packet(a->session, &size);
        if (p != NULL) {
            AcquireSRWLockExclusive(&a->lock);
            a->held = p; a->held_size = size;
            ReleaseSRWLockExclusive(&a->lock);
            return 1;
        }
        if (GetLastError() != ERROR_NO_MORE_ITEMS) return 0;
        YieldProcessor();
    }
    wait = WaitForSingleObject(a->event, timeout_ms == CHMLINK_WAIT_INFINITE ? INFINITE : timeout_ms);
    if (wait == WAIT_OBJECT_0) return 1;
    SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    return 0;
}

/* Wintun has no coalescing counterpart; the Wintun benchmarks never ask for it. */
chmlink_coalescer *chmlink_coalescer_create(chmlink_adapter *a, uint32_t flags)
{
    (void)a; (void)flags;
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

int chmlink_coalescer_send(chmlink_coalescer *c, const uint8_t *packet, uint32_t size)
{
    (void)c; (void)packet; (void)size;
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

int chmlink_coalescer_flush(chmlink_coalescer *c)
{
    (void)c;
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

void chmlink_coalescer_free(chmlink_coalescer *c) { (void)c; }
