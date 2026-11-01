/* tap-windows6 back end for the benchmarks, as bench/wintun_shim.c is for
 * Wintun: built into tap-bench and tap-tcp-bench with the unchanged
 * bench/packet_bench.c and bench/tcp_bench.c, so all three drivers run the
 * same scenarios, threads and accounting.
 *
 *   chmlink_open        opens the one installed TAP-Windows Adapter V9
 *                       (tap-windows6, root\tap0901), switches it to TUN mode
 *                       (TAP_WIN_IOCTL_CONFIG_TUN, IPv4 packets without an
 *                       Ethernet header; the driver answers ARP for the
 *                       subnet), sets the media status connected and assigns
 *                       CHMLINK_TAP_ADDRESS (for example 198.18.0.1/24). The
 *                       interface index argument is ignored. MTU: 1500, the
 *                       driver's default.
 *   chmlink_receive     tap-windows6 hands over one packet per ReadFile. The
 *                       shim keeps READS overlapped reads posted, as a tuned
 *                       application would, and collects the completed ones,
 *                       in order, into one batch. Reads cancelled because the
 *                       thread that posted them exited are posted again.
 *   chmlink_inject      one WriteFile per packet, overlapped, with up to
 *                       WRITES in flight before the oldest is waited for.
 *   chmlink_wait_readable  waits for the oldest posted read.
 *   chmlink_set_offload grants nothing: TUN mode has no offloads here.
 *
 * Driver counters are not available and read as zero. IPv4 only. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"

/* tap-windows.h (tap-windows6 9.27.0), declared here rather than copied. */
#define TAP_WIN_CONTROL_CODE(request, method) CTL_CODE(FILE_DEVICE_UNKNOWN, request, method, FILE_ANY_ACCESS)
#define TAP_WIN_IOCTL_SET_MEDIA_STATUS TAP_WIN_CONTROL_CODE(6, METHOD_BUFFERED)
#define TAP_WIN_IOCTL_CONFIG_TUN TAP_WIN_CONTROL_CODE(10, METHOD_BUFFERED)

#define READS 64u
#define WRITES 64u
#define SLOT 1600u
#define PACKET_LIMIT 1500u

typedef struct io_slot {
    OVERLAPPED overlapped;
    BOOL pending;
    uint8_t data[SLOT];
} io_slot;

struct chmlink_adapter {
    HANDLE device;
    NET_LUID luid;
    uint32_t index;
    MIB_UNICASTIPADDRESS_ROW address;
    io_slot reads[READS];
    unsigned read_head;           /* oldest posted read */
    SRWLOCK write_lock;
    io_slot writes[WRITES];
    unsigned write_next;
};

static int find_tap(NET_LUID *luid, GUID *guid, uint32_t *index)
{
    PMIB_IF_TABLE2 table;
    ULONG i;
    int found = 0;
    if (GetIfTable2(&table) != NO_ERROR) return 0;
    for (i = 0; i < table->NumEntries; ++i) {
        MIB_IF_ROW2 *row = &table->Table[i];
        if (wcsncmp(row->Description, L"TAP-Windows Adapter V9", 22) != 0 || row->InterfaceAndOperStatusFlags.FilterInterface)
            continue;
        *luid = row->InterfaceLuid; *guid = row->InterfaceGuid; *index = row->InterfaceIndex;
        found = 1;
        break;
    }
    FreeMibTable(table);
    return found;
}

static int assign_address(chmlink_adapter *a, uint32_t *local, uint32_t *network, uint32_t *mask)
{
    char text[64], *slash;
    MIB_UNICASTIPADDRESS_ROW *row = &a->address;
    DWORD len = GetEnvironmentVariableA("CHMLINK_TAP_ADDRESS", text, sizeof(text));
    int prefix;
    if (len == 0 || len >= sizeof(text) || (slash = strchr(text, '/')) == NULL) return 0;
    *slash = 0;
    prefix = atoi(slash + 1);
    if (prefix < 8 || prefix > 30) return 0;
    InitializeUnicastIpAddressEntry(row);
    row->InterfaceLuid = a->luid;
    row->Address.Ipv4.sin_family = AF_INET;
    if (inet_pton(AF_INET, text, &row->Address.Ipv4.sin_addr) != 1) return 0;
    row->OnLinkPrefixLength = (UINT8)prefix;
    row->DadState = IpDadStatePreferred;
    *local = row->Address.Ipv4.sin_addr.S_un.S_addr;
    *mask = htonl(0xffffffffu << (32 - prefix));
    *network = *local & *mask;
    return 1;
}

static int wait_address(chmlink_adapter *a)
{
    DWORD error = CreateUnicastIpAddressEntry(&a->address);
    int i;
    if (error != NO_ERROR && error != ERROR_OBJECT_ALREADY_EXISTS) {
        fprintf(stderr, "tap shim: CreateUnicastIpAddressEntry %lu\n", error);
        return 0;
    }
    for (i = 0; i < 200; ++i) {
        if (GetUnicastIpAddressEntry(&a->address) == NO_ERROR && a->address.DadState == IpDadStatePreferred) return 1;
        Sleep(50);
    }
    fprintf(stderr, "tap shim: address state %d after 10 s\n", (int)a->address.DadState);
    return 0;
}

static BOOL post_read(chmlink_adapter *a, io_slot *s)
{
    ResetEvent(s->overlapped.hEvent);
    if (ReadFile(a->device, s->data, SLOT, NULL, &s->overlapped) || GetLastError() == ERROR_IO_PENDING) {
        s->pending = TRUE;
        return TRUE;
    }
    return FALSE;
}

chmlink_adapter *chmlink_open(uint32_t interface_index)
{
    chmlink_adapter *a;
    GUID guid;
    wchar_t text[39], path[96];
    uint32_t tun[3], connected = 1;
    DWORD bytes;
    unsigned i;
    (void)interface_index;
    a = calloc(1, sizeof(*a));
    if (a == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    InitializeSRWLock(&a->write_lock);
    if (!find_tap(&a->luid, &guid, &a->index)) {
        fprintf(stderr, "tap shim: no TAP-Windows Adapter V9 installed\n");
        free(a); SetLastError(ERROR_NOT_FOUND); return NULL;
    }
    StringFromGUID2(&guid, text, 39);
    swprintf(path, 96, L"\\\\.\\Global\\%ls.tap", text);
    a->device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED, NULL);
    if (a->device == INVALID_HANDLE_VALUE) { fprintf(stderr, "tap shim: open %lu\n", GetLastError()); free(a); return NULL; }
    if (!assign_address(a, &tun[0], &tun[1], &tun[2])) {
        fprintf(stderr, "tap shim: set CHMLINK_TAP_ADDRESS\n");
        CloseHandle(a->device); free(a); SetLastError(ERROR_INVALID_PARAMETER); return NULL;
    }
    for (i = 0; i < READS; ++i) a->reads[i].overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < WRITES; ++i) a->writes[i].overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    /* Synchronous IOCTLs on an overlapped handle: wait through the event. */
    {
        OVERLAPPED o = {0};
        o.hEvent = a->writes[0].overlapped.hEvent;
        if ((!DeviceIoControl(a->device, TAP_WIN_IOCTL_CONFIG_TUN, tun, sizeof(tun), tun, sizeof(tun), NULL, &o) &&
             GetLastError() != ERROR_IO_PENDING) || !GetOverlappedResult(a->device, &o, &bytes, TRUE) ||
            (!DeviceIoControl(a->device, TAP_WIN_IOCTL_SET_MEDIA_STATUS, &connected, sizeof(connected), &connected,
                              sizeof(connected), NULL, &o) && GetLastError() != ERROR_IO_PENDING) ||
            !GetOverlappedResult(a->device, &o, &bytes, TRUE)) {
            fprintf(stderr, "tap shim: TUN configuration %lu\n", GetLastError());
            CloseHandle(a->device); free(a); return NULL;
        }
    }
    if (!wait_address(a)) { CloseHandle(a->device); free(a); SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    for (i = 0; i < READS; ++i) {
        if (!post_read(a, &a->reads[i])) { fprintf(stderr, "tap shim: read %lu\n", GetLastError()); return NULL; }
    }
    Sleep(2000);   /* let address registration settle, as the other shims do */
    return a;
}

void chmlink_close(chmlink_adapter *a)
{
    uint32_t disconnected = 0;
    DWORD bytes;
    unsigned i;
    OVERLAPPED o = {0};
    if (a == NULL) return;
    CancelIoEx(a->device, NULL);
    for (i = 0; i < READS; ++i) if (a->reads[i].pending) GetOverlappedResult(a->device, &a->reads[i].overlapped, &bytes, TRUE);
    for (i = 0; i < WRITES; ++i) if (a->writes[i].pending) GetOverlappedResult(a->device, &a->writes[i].overlapped, &bytes, TRUE);
    o.hEvent = a->writes[0].overlapped.hEvent;
    ResetEvent(o.hEvent);
    if (DeviceIoControl(a->device, TAP_WIN_IOCTL_SET_MEDIA_STATUS, &disconnected, sizeof(disconnected), &disconnected,
                        sizeof(disconnected), NULL, &o) || GetLastError() == ERROR_IO_PENDING)
        GetOverlappedResult(a->device, &o, &bytes, TRUE);
    DeleteUnicastIpAddressEntry(&a->address);
    for (i = 0; i < READS; ++i) CloseHandle(a->reads[i].overlapped.hEvent);
    for (i = 0; i < WRITES; ++i) CloseHandle(a->writes[i].overlapped.hEvent);
    CloseHandle(a->device);
    free(a);
}

int chmlink_query(chmlink_adapter *a, chmlink_adapter_info *info)
{
    if (a == NULL || info == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    memset(info, 0, sizeof(*info));
    info->mode = CHMLINK_MODE_IP;
    info->capabilities = CHMLINK_CAP_IP | CHMLINK_CAP_BATCH | CHMLINK_CAP_WAIT;
    info->interface_index = a->index;
    info->mtu = PACKET_LIMIT;
    info->packet_limit = PACKET_LIMIT;
    return 1;
}

int chmlink_set_offload(chmlink_adapter *a, uint32_t requested, uint32_t *granted)
{
    (void)a; (void)requested;
    if (granted != NULL) *granted = 0;
    return 1;
}

int chmlink_receive(chmlink_adapter *a, void *buffer, uint32_t capacity, uint32_t *bytes)
{
    chmlink_batch_writer writer;
    uint32_t count = 0;
    if (bytes != NULL) *bytes = 0;
    if (a == NULL || buffer == NULL || bytes == NULL ||
        chmlink_batch_begin(&writer, buffer, capacity, CHMLINK_MODE_IP, PACKET_LIMIT) != CHMLINK_OK) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    while (count < CHMLINK_BATCH_MAX_PACKETS) {
        io_slot *s = &a->reads[a->read_head];
        DWORD size;
        if (!GetOverlappedResult(a->device, &s->overlapped, &size, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) break;
            /* Windows cancels a thread's pending I/O when the thread exits;
             * the benchmark's threads come and go, so post the read again. */
            if (GetLastError() != ERROR_OPERATION_ABORTED || !post_read(a, s)) return 0;
            a->read_head = (a->read_head + 1) % READS;
            continue;
        }
        s->pending = FALSE;
        if (size != 0 && chmlink_batch_append(&writer, s->data, size) != CHMLINK_OK) {
            if (count == 0) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            s->pending = TRUE;      /* completed; taken by the next call */
            break;
        }
        if (!post_read(a, s)) return 0;
        a->read_head = (a->read_head + 1) % READS;
        if (size != 0) count++;
    }
    if (count == 0) { SetLastError(ERROR_NO_MORE_ITEMS); return 0; }
    *bytes = (uint32_t)writer.length;
    return 1;
}

int chmlink_inject(chmlink_adapter *a, const void *batch, uint32_t length)
{
    chmlink_batch_reader reader;
    const uint8_t *packet;
    uint32_t size, count;
    chmlink_offload descriptor;
    if (a == NULL || batch == NULL ||
        chmlink_batch_validate(batch, length, CHMLINK_MODE_IP, PACKET_LIMIT, &count) != CHMLINK_OK || count == 0) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    AcquireSRWLockExclusive(&a->write_lock);
    chmlink_batch_read_begin(&reader, batch);
    while (chmlink_batch_read_next(&reader, &packet, &size, &descriptor)) {
        io_slot *s = &a->writes[a->write_next];
        DWORD done;
        a->write_next = (a->write_next + 1) % WRITES;
        if (s->pending) GetOverlappedResult(a->device, &s->overlapped, &done, TRUE);
        s->pending = FALSE;
        memcpy(s->data, packet, size);
        ResetEvent(s->overlapped.hEvent);
        if (WriteFile(a->device, s->data, size, NULL, &s->overlapped)) continue;
        if (GetLastError() != ERROR_IO_PENDING) { ReleaseSRWLockExclusive(&a->write_lock); return 0; }
        s->pending = TRUE;
    }
    ReleaseSRWLockExclusive(&a->write_lock);
    return 1;
}

int chmlink_wait_readable(chmlink_adapter *a, uint32_t timeout_ms)
{
    DWORD wait;
    if (a == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    wait = WaitForSingleObject(a->reads[a->read_head].overlapped.hEvent,
                               timeout_ms == CHMLINK_WAIT_INFINITE ? INFINITE : timeout_ms);
    if (wait == WAIT_OBJECT_0) return 1;
    SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    return 0;
}

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
