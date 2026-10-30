#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "chmlink/device.h"
#include "chmlink/protocol.h"

/* Fuzzes the device control interface with raw requests, below the SDK.
 *
 * Each round opens the data session, gives every worker its own duplicate of
 * the handle (so all share one file object) and lets them issue random
 * requests; each worker closes its handle after a random number of requests,
 * so the session's IRP_MJ_CLEANUP races requests still in flight on the other
 * handles. An inspection thread opens and closes read-only handles meanwhile.
 *
 * Every result is checked against what the request deserves: the driver must
 * accept an injected batch exactly when chmlink_batch_validate accepts it, and
 * every rejection must carry the error that the dispatch code assigns to it.
 * --tolerant (adapter disabled/removed underneath, or Driver Verifier fault
 * injection) only keeps the safety half: nothing invalid may succeed.
 *
 * Usage: chmlink-ioctl-fuzz <interface-index> <seconds> [--seed N] [--tolerant] [--ops MASK]
 * MASK selects request kinds by bit (default all): 0 query, 1 receive,
 * 2 inject, 3 register, 4 foreign codes, 5 bad buffers, 6 read/write/cancel,
 * 7 session churn (without it one session lasts the whole run).
 */

#define FUZZ_WORKERS 6
#define BIG (CHMLINK_BATCH_MAX_SIZE + 64u)

#define FAIL(...) do { fprintf(stderr, "fuzz: "); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, " (seed %llu)\n", (unsigned long long)g_seed); fflush(stderr); ExitProcess(1); } while (0)
#define REQUIRE(x) do { if (!(x)) FAIL("%s:%d: %s (error %lu)", __FILE__, __LINE__, #x, GetLastError()); } while (0)

static unsigned long long g_seed;
static int g_tolerant;
static unsigned long g_ops = 0xff;
static uint32_t g_index, g_mode, g_limit;
static wchar_t g_path[64];
static volatile LONG g_stop;
static ULONGLONG g_deadline;
static volatile LONG64 g_requests, g_accepted_injects, g_registered, g_register_conflicts;
static uint8_t *g_readonly_page, *g_guard_edge;

typedef struct rng { unsigned long long s; } rng;
static uint32_t next(rng *r)
{
    r->s ^= r->s >> 12; r->s ^= r->s << 25; r->s ^= r->s >> 27;
    return (uint32_t)((r->s * 2685821657736338717ull) >> 32);
}
static uint32_t below(rng *r, uint32_t n) { return n ? next(r) % n : 0; }

/* Lengths that sit on the boundaries the driver and the I/O manager check. */
static uint32_t edge_length(rng *r)
{
    static const uint32_t edges[] = { 0, 1, 3, 4, 7, 8, 9, 23, 24, 25, 27, 28, 64, 127, 128, 129,
        1514, 1522, 4096, 65535, 65536, CHMLINK_BATCH_MAX_SIZE - 1, CHMLINK_BATCH_MAX_SIZE,
        CHMLINK_BATCH_MAX_SIZE + 1 };
    if (below(r, 4) == 0) return below(r, 2048);
    return edges[below(r, (uint32_t)(sizeof(edges) / sizeof(edges[0])))];
}

static int is_lifecycle_error(DWORD e)
{
    return e == ERROR_NOT_READY || e == ERROR_DEVICE_REMOVED || e == ERROR_DEVICE_NOT_CONNECTED ||
           e == ERROR_FILE_NOT_FOUND || e == ERROR_INVALID_HANDLE || e == ERROR_OPERATION_ABORTED ||
           e == ERROR_NO_SYSTEM_RESOURCES || e == ERROR_NOT_ENOUGH_MEMORY || 
           e == ERROR_BAD_COMMAND || e == ERROR_GEN_FAILURE;
}

/* One request, waited for synchronously. Returns the Win32 error, 0 on success. */
static DWORD issue(HANDLE h, HANDLE event, DWORD code, void *in, DWORD in_size,
                   void *out, DWORD out_size, DWORD *returned)
{
    OVERLAPPED o;
    DWORD bytes = 0;
    BOOL ok;
    memset(&o, 0, sizeof(o));
    o.hEvent = event;
    ok = DeviceIoControl(h, code, in, in_size, out, out_size, NULL, &o);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &o, &bytes, TRUE);
    else if (ok) ok = GetOverlappedResult(h, &o, &bytes, TRUE);
    InterlockedIncrement64(&g_requests);
    if (returned) *returned = ok ? bytes : 0;
    return ok ? 0 : GetLastError();
}

static void expect(DWORD got, DWORD want, const char *what, DWORD in_size, DWORD out_size)
{
    if (got == want) return;
    if (g_tolerant && got != 0 && (want != 0 || is_lifecycle_error(got))) return;
    FAIL("%s in=%lu out=%lu: error %lu, expected %lu", what, in_size, out_size, got, want);
}

static void expect_failure(DWORD got, const char *what, DWORD in_size, DWORD out_size)
{
    if (got == 0) FAIL("%s in=%lu out=%lu succeeded", what, in_size, out_size);
}

/* A valid batch of small frames (Ethernet to a foreign unicast address, which
 * the receive filter discards) or minimal IPv4/IPv6 packets. */
static uint32_t valid_batch(rng *r, uint8_t *buffer, uint32_t capacity)
{
    chmlink_batch_writer w;
    uint8_t packet[256];
    uint32_t count = 1 + below(r, 8), i, length;
    REQUIRE(chmlink_batch_begin(&w, buffer, capacity, (chmlink_mode)g_mode, g_limit) == CHMLINK_OK);
    for (i = 0; i < count; ++i) {
        memset(packet, 0, sizeof(packet));
        if (g_mode == CHMLINK_MODE_ETHERNET) {
            length = 14 + below(r, 200);
            packet[0] = 0x02; packet[5] = 0x7e; packet[6] = 0x02; packet[11] = 0x7f;
            packet[12] = 0x88; packet[13] = 0xb5;
        } else if (below(r, 2)) {
            length = 20 + below(r, 100);
            packet[0] = 0x45; packet[2] = (uint8_t)(length >> 8); packet[3] = (uint8_t)length;
            packet[8] = 1; packet[9] = 253;                  /* TTL 1, experimental protocol */
            packet[12] = 192; packet[13] = 0; packet[14] = 2; packet[15] = 1;   /* TEST-NET-1 */
            packet[16] = 192; packet[17] = 0; packet[18] = 2; packet[19] = 2;
        } else {
            length = 40 + below(r, 100);
            packet[0] = 0x60; packet[4] = (uint8_t)((length - 40) >> 8); packet[5] = (uint8_t)(length - 40);
            packet[6] = 253; packet[7] = 1;                  /* next header, hop limit */
            packet[8] = 0x20; packet[9] = 0x01; packet[10] = 0x0d; packet[11] = 0xb8;   /* 2001:db8:: */
            packet[24] = 0x20; packet[25] = 0x01; packet[26] = 0x0d; packet[27] = 0xb8;
        }
        if (chmlink_batch_append(&w, packet, length) != CHMLINK_OK) break;
    }
    return (uint32_t)w.length;
}

static uint32_t inject_input(rng *r, uint8_t *buffer)
{
    uint32_t length, i, flips;
    switch (below(r, 5)) {
    case 0:                                    /* random bytes, random length */
        length = edge_length(r); if (length > BIG) length = BIG;
        for (i = 0; i < length; ++i) buffer[i] = (uint8_t)next(r);
        return length;
    case 1: case 2:                            /* a valid batch, mutated */
        length = valid_batch(r, buffer, BIG);
        flips = 1 + below(r, 4);
        for (i = 0; i < flips; ++i) {
            uint32_t at = below(r, length);
            if (below(r, 3) == 0 && length >= 24) {
                /* overwrite one header or record field with an edge value */
                static const uint32_t values[] = { 0, 1, 2, 3, 24, 255, 256, 257, 65557, 0x7fffffff, 0xffffffff };
                uint32_t v = values[below(r, (uint32_t)(sizeof(values) / sizeof(values[0])))];
                at = (below(r, 7) * 4); if (at + 4 > length) at = 0;
                memcpy(buffer + at, &v, 4);
            } else buffer[at] ^= (uint8_t)(1u << below(r, 8));
        }
        if (below(r, 4) == 0) length = below(r, length + 8);   /* truncate or overrun */
        return length;
    default:                                   /* valid */
        return valid_batch(r, buffer, BIG);
    }
}

typedef struct worker {
    HANDLE handle, event;
    rng r;
    uint32_t requests;
    uint8_t *in, *out;
    HANDLE wrong_type, weak_event;
} worker;

static DWORD code_with(DWORD function, DWORD method, DWORD access)
{
    return CTL_CODE(FILE_DEVICE_UNKNOWN, function, method, access);
}

static int is_contract_code(DWORD code)
{
    return code == CHMLINK_IOCTL_RECEIVE || code == CHMLINK_IOCTL_INJECT ||
           code == CHMLINK_IOCTL_QUERY || code == CHMLINK_IOCTL_REGISTER_READ_EVENT;
}

static void one_request(worker *w, int session)
{
    DWORD in_size, out_size, got, returned, code;
    rng *r = &w->r;
    static const int kinds[10] = { 0, 1, 1, 2, 2, 2, 3, 4, 5, 6 };
    uint32_t pick;
    do pick = below(r, 10); while (!(g_ops & (1ul << kinds[pick])) && (g_ops & 0x7f));
    if (!(g_ops & 0x7f)) return;
    switch (pick) {
    case 0: {                                  /* QUERY */
        in_size = below(r, 3) == 0 ? edge_length(r) : 0;
        out_size = below(r, 3) == 0 ? edge_length(r) : CHMLINK_INFO_SIZE + below(r, 64);
        if (in_size > BIG) in_size = BIG;
        if (out_size > BIG) out_size = BIG;
        got = issue(w->handle, w->event, CHMLINK_IOCTL_QUERY, in_size ? w->in : NULL, in_size,
                    out_size ? w->out : NULL, out_size, &returned);
        if (in_size) expect(got, ERROR_INVALID_PARAMETER, "QUERY with input", in_size, out_size);
        else if (out_size < CHMLINK_INFO_SIZE) expect(got, ERROR_INSUFFICIENT_BUFFER, "QUERY short", in_size, out_size);
        else {
            chmlink_query_info info;
            expect(got, 0, "QUERY", in_size, out_size);
            if (got == 0) {
                REQUIRE(returned == CHMLINK_INFO_SIZE);
                REQUIRE(chmlink_query_decode(&info, w->out, returned) == CHMLINK_OK);
                REQUIRE(info.interface_index == g_index && info.mode == g_mode);
            }
        }
        break;
    }
    case 1: case 2: {                          /* RECEIVE */
        in_size = below(r, 4) == 0 ? edge_length(r) : 0;
        out_size = edge_length(r);
        if (in_size > BIG) in_size = BIG;
        if (out_size > BIG) out_size = BIG;
        got = issue(w->handle, w->event, CHMLINK_IOCTL_RECEIVE, in_size ? w->in : NULL, in_size,
                    out_size ? w->out : NULL, out_size, &returned);
        if (!session) expect(got, ERROR_ACCESS_DENIED, "RECEIVE on inspection handle", in_size, out_size);
        else if (in_size) expect(got, ERROR_INVALID_PARAMETER, "RECEIVE with input", in_size, out_size);
        else if (out_size > CHMLINK_BATCH_MAX_SIZE) expect(got, ERROR_INVALID_USER_BUFFER, "RECEIVE oversized", in_size, out_size);
        else if (out_size < CHMLINK_BATCH_HEADER_SIZE) expect(got, ERROR_INSUFFICIENT_BUFFER, "RECEIVE short", in_size, out_size);
        else if (got == 0) {
            if (chmlink_batch_validate(w->out, returned, (chmlink_mode)g_mode, g_limit, NULL) != CHMLINK_OK)
                FAIL("RECEIVE returned an invalid batch of %lu bytes", returned);
        } else if (got != ERROR_NO_MORE_ITEMS && got != ERROR_INSUFFICIENT_BUFFER)
            expect(got, ERROR_NO_MORE_ITEMS, "RECEIVE", in_size, out_size);
        break;
    }
    case 3: case 4: case 5: {                  /* INJECT */
        int valid;
        in_size = inject_input(r, w->in);
        out_size = below(r, 6) == 0 ? edge_length(r) % 4096 : 0;
        valid = chmlink_batch_validate(w->in, in_size, (chmlink_mode)g_mode, g_limit, NULL) == CHMLINK_OK;
        got = issue(w->handle, w->event, CHMLINK_IOCTL_INJECT, in_size ? w->in : NULL, in_size,
                    out_size ? w->out : NULL, out_size, &returned);
        if (!session) expect(got, ERROR_ACCESS_DENIED, "INJECT on inspection handle", in_size, out_size);
        else if (out_size || !valid) expect(got, ERROR_INVALID_PARAMETER, valid ? "INJECT with output" : "INJECT invalid batch", in_size, out_size);
        else {
            expect(got, 0, "INJECT valid batch", in_size, out_size);
            if (got == 0) InterlockedIncrement64(&g_accepted_injects);
        }
        if (got == 0 && (!valid || out_size || !session)) FAIL("INJECT accepted a request it must refuse");
        break;
    }
    case 6: {                                  /* REGISTER_READ_EVENT */
        uint64_t value;
        HANDLE event = NULL;
        int kind = (int)below(r, 6);
        in_size = 8; out_size = 0;
        if (kind == 0) { event = CreateEventW(NULL, TRUE, FALSE, NULL); REQUIRE(event); value = (uint64_t)(ULONG_PTR)event; }
        else if (kind == 1) value = (uint64_t)(ULONG_PTR)w->wrong_type;
        else if (kind == 2) value = (uint64_t)(ULONG_PTR)w->weak_event;
        else if (kind == 3) value = ((uint64_t)next(r) << 32) | next(r);
        else if (kind == 4) { value = 0; in_size = edge_length(r) % 64; }
        else { value = (uint64_t)(ULONG_PTR)GetCurrentProcess(); }   /* pseudo-handle */
        memcpy(w->in, &value, 8);
        if (kind == 4) { DWORD i; for (i = 8; i < in_size; ++i) w->in[i] = (uint8_t)next(r); }
        if (below(r, 8) == 0) out_size = 1 + below(r, 64);
        got = issue(w->handle, w->event, CHMLINK_IOCTL_REGISTER_READ_EVENT, in_size ? w->in : NULL, in_size,
                    out_size ? w->out : NULL, out_size, &returned);
        if (!session) expect(got, ERROR_ACCESS_DENIED, "REGISTER on inspection handle", in_size, out_size);
        else if (out_size || in_size != 8) expect(got, ERROR_INVALID_PARAMETER, "REGISTER malformed", in_size, out_size);
        else if (kind == 0) {
            if (got == 0) InterlockedIncrement64(&g_registered);
            else if (got == ERROR_BAD_COMMAND) InterlockedIncrement64(&g_register_conflicts);
            else expect(got, 0, "REGISTER event", in_size, out_size);
        } else expect_failure(got, "REGISTER non-event", in_size, out_size);
        if (event) CloseHandle(event);        /* the driver keeps its own reference */
        break;
    }
    case 7: {                                  /* codes outside the contract */
        do {
            code = code_with(0x7f0 + below(r, 0x40), below(r, 4), below(r, 4));
        } while (is_contract_code(code));
        in_size = edge_length(r) % 4096; out_size = edge_length(r) % 4096;
        got = issue(w->handle, w->event, code, in_size ? w->in : NULL, in_size,
                    out_size ? w->out : NULL, out_size, &returned);
        if (got != ERROR_INVALID_FUNCTION && got != ERROR_ACCESS_DENIED && got != ERROR_INVALID_PARAMETER &&
            !(g_tolerant && got != 0))
            FAIL("code 0x%08lx: error %lu", code, got);
        break;
    }
    case 8: {                                  /* bad buffers: the I/O manager must refuse, the driver never sees them */
        void *in = NULL, *out = NULL;
        DWORD kind = below(r, 4);
        code = below(r, 2) ? CHMLINK_IOCTL_QUERY : CHMLINK_IOCTL_RECEIVE;
        in_size = 0; out_size = CHMLINK_INFO_SIZE;
        if (kind == 0) out = NULL;                                  /* NULL with a length */
        else if (kind == 1) out = g_readonly_page;                  /* not writable */
        else if (kind == 2) out = g_guard_edge;                     /* runs into a no-access page */
        else { code = CHMLINK_IOCTL_INJECT; in = g_guard_edge; in_size = 4096; out_size = 0; }
        got = issue(w->handle, w->event, code, in, in_size, out, out_size, &returned);
        expect_failure(got, "bad buffer", in_size, out_size);
        break;
    }
    default: {                                 /* other major functions and cancellation */
        OVERLAPPED o;
        DWORD bytes;
        memset(&o, 0, sizeof(o)); o.hEvent = w->event;
        if (below(r, 2)) {
            if (ReadFile(w->handle, w->out, 64, NULL, &o) || GetLastError() == ERROR_IO_PENDING)
                if (GetOverlappedResult(w->handle, &o, &bytes, TRUE)) FAIL("ReadFile succeeded");
        } else {
            if (WriteFile(w->handle, w->in, 64, NULL, &o) || GetLastError() == ERROR_IO_PENDING)
                if (GetOverlappedResult(w->handle, &o, &bytes, TRUE)) FAIL("WriteFile succeeded");
        }
        (void)CancelIoEx(w->handle, NULL);
        InterlockedIncrement64(&g_requests);
        break;
    }
    }
}

static DWORD WINAPI session_worker(void *arg)
{
    worker *w = arg;
    uint32_t i;
    for (i = 0; i < w->requests && !g_stop && GetTickCount64() < g_deadline; ++i) one_request(w, 1);
    CloseHandle(w->handle);   /* the last close runs IRP_MJ_CLEANUP while others may still be in a request */
    w->handle = NULL;
    return 0;
}

static DWORD WINAPI inspector(void *arg)
{
    worker *w = arg;
    while (!g_stop) {
        uint32_t i, n = 1 + below(&w->r, 40);
        w->handle = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (w->handle == INVALID_HANDLE_VALUE) {
            if (!g_tolerant) FAIL("inspection open failed: %lu", GetLastError());
            Sleep(10); continue;
        }
        for (i = 0; i < n && !g_stop; ++i) one_request(w, 0);
        CloseHandle(w->handle);
    }
    return 0;
}

static void init_worker(worker *w, unsigned long long seed)
{
    w->r.s = seed * 0x9e3779b97f4a7c15ull + 1;
    w->event = CreateEventW(NULL, TRUE, FALSE, NULL); REQUIRE(w->event);
    w->in = malloc(BIG + 64); w->out = malloc(BIG + 64); REQUIRE(w->in && w->out);
    w->wrong_type = CreateSemaphoreW(NULL, 0, 1, NULL); REQUIRE(w->wrong_type);
    REQUIRE(DuplicateHandle(GetCurrentProcess(), w->event, GetCurrentProcess(), &w->weak_event,
                            SYNCHRONIZE, FALSE, 0));        /* an event without EVENT_MODIFY_STATE */
}

static HANDLE open_session(void)
{
    unsigned int attempt;
    for (attempt = 0; attempt < 500; ++attempt) {
        HANDLE h = CreateFileW(g_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (!g_tolerant && GetLastError() != ERROR_SHARING_VIOLATION) FAIL("session open failed: %lu", GetLastError());
        Sleep(10);
    }
    return NULL;
}

static int query_state(chmlink_query_info *info)
{
    HANDLE h, event;
    uint8_t bytes[CHMLINK_INFO_SIZE];
    DWORD got, returned;
    h = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    event = CreateEventW(NULL, TRUE, FALSE, NULL); REQUIRE(event);
    got = issue(h, event, CHMLINK_IOCTL_QUERY, NULL, 0, bytes, sizeof(bytes), &returned);
    CloseHandle(event); CloseHandle(h);
    return got == 0 && chmlink_query_decode(info, bytes, returned) == CHMLINK_OK;
}

int wmain(int argc, wchar_t **argv)
{
    worker workers[FUZZ_WORKERS], inspect;
    HANDLE threads[FUZZ_WORKERS], inspect_thread;
    chmlink_query_info info, after;
    ULONGLONG deadline;
    unsigned long rounds = 0;
    DWORD old;
    int i;
    uint8_t *pages;
    if (argc < 3) { fwprintf(stderr, L"usage: chmlink-ioctl-fuzz <index> <seconds> [--seed N] [--tolerant]\n"); return 2; }
    g_index = (uint32_t)wcstoul(argv[1], NULL, 10);
    deadline = g_deadline = GetTickCount64() + 1000ull * wcstoul(argv[2], NULL, 10);
    g_seed = GetTickCount64() ^ ((unsigned long long)GetCurrentProcessId() << 32);
    for (i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"--seed") == 0 && i + 1 < argc) g_seed = _wcstoui64(argv[++i], NULL, 10);
        else if (wcscmp(argv[i], L"--tolerant") == 0) g_tolerant = 1;
        else if (wcscmp(argv[i], L"--ops") == 0 && i + 1 < argc) g_ops = wcstoul(argv[++i], NULL, 0);
        else { fwprintf(stderr, L"unknown argument %ls\n", argv[i]); return 2; }
    }
    swprintf(g_path, 64, L"\\\\.\\ChmLink%lu", (unsigned long)g_index);
    printf("seed %llu%s\n", (unsigned long long)g_seed, g_tolerant ? " (tolerant)" : ""); fflush(stdout);

    /* A read-only page, then a writable page followed by a no-access page. */
    pages = VirtualAlloc(NULL, 3 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); REQUIRE(pages);
    REQUIRE(VirtualProtect(pages, 4096, PAGE_READONLY, &old));
    REQUIRE(VirtualProtect(pages + 2 * 4096, 4096, PAGE_NOACCESS, &old));
    g_readonly_page = pages; g_guard_edge = pages + 2 * 4096 - 64;

    if (!query_state(&info)) {
        /* Under fault injection or churn the adapter may be starting, failed or gone. */
        ULONGLONG give_up = GetTickCount64() + 20000;
        while (g_tolerant && GetTickCount64() < give_up && !query_state(&info)) Sleep(200);
        if (!query_state(&info)) {
            if (!g_tolerant) FAIL("cannot query adapter %u", g_index);
            printf("SKIP adapter %u unavailable\n", g_index); return 0;
        }
    }
    if (!g_tolerant && (info.state & CHMLINK_STATE_SESSION)) FAIL("adapter %u already has a data session", g_index);
    g_mode = info.mode; g_limit = info.packet_limit;

    /* A name below the device must not open. */
    {
        wchar_t sub[80];
        HANDLE h;
        swprintf(sub, 80, L"%ls\\extra", g_path);
        h = CreateFileW(sub, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) FAIL("opened a name below the device");
    }

    init_worker(&inspect, g_seed + 1000);
    inspect_thread = CreateThread(NULL, 0, inspector, &inspect, 0, NULL); REQUIRE(inspect_thread);
    while (GetTickCount64() < deadline) {
        HANDLE session = open_session();
        LONG64 registered_before = g_registered, conflicts_before = g_register_conflicts;
        if (session == NULL) { if (g_tolerant) continue; FAIL("session never became free"); }
        for (i = 0; i < FUZZ_WORKERS; ++i) {
            if (rounds == 0) init_worker(&workers[i], g_seed + (unsigned long long)i);
            REQUIRE(DuplicateHandle(GetCurrentProcess(), session, GetCurrentProcess(), &workers[i].handle,
                                    0, FALSE, DUPLICATE_SAME_ACCESS));
            workers[i].requests = (g_ops & 0x80) ? below(&workers[i].r, 400) : 0xffffffffu;
        }
        CloseHandle(session);
        for (i = 0; i < FUZZ_WORKERS; ++i) {
            threads[i] = CreateThread(NULL, 0, session_worker, &workers[i], 0, NULL); REQUIRE(threads[i]);
        }
        REQUIRE(WaitForMultipleObjects(FUZZ_WORKERS, threads, TRUE, (DWORD)(deadline - GetTickCount64() + 120000)) == WAIT_OBJECT_0);
        for (i = 0; i < FUZZ_WORKERS; ++i) CloseHandle(threads[i]);
        /* One read event per session at most; a refused second one implies the first. */
        if (g_registered - registered_before > 1) FAIL("two read events registered in one session");
        if (!g_tolerant && g_register_conflicts > conflicts_before && g_registered == registered_before)
            FAIL("read event refused as a duplicate with none registered");
        /* Cleanup ran with the last close: the session must be free again. */
        if (!g_tolerant) {
            REQUIRE(query_state(&after));
            if (after.state & CHMLINK_STATE_SESSION) FAIL("session still open after every handle closed");
            if (!(after.state & CHMLINK_STATE_RUNNING)) FAIL("adapter not running after round %lu", rounds);
        }
        rounds++;
    }
    InterlockedExchange(&g_stop, 1);
    REQUIRE(WaitForSingleObject(inspect_thread, 30000) == WAIT_OBJECT_0);
    if (!g_tolerant) {
        REQUIRE(query_state(&after));
        if (after.rx_packets + after.rx_discards < info.rx_packets + info.rx_discards + (uint64_t)g_accepted_injects)
            FAIL("counters lost injected packets");
    }
    printf("PASS ioctl fuzz: %lu rounds, %lld requests, %lld accepted injections, %lld read events\n",
           rounds, (long long)g_requests, (long long)g_accepted_injects, (long long)g_registered);
    return 0;
}
