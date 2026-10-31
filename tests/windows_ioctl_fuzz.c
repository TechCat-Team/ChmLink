#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "chmlink/device.h"
#include "chmlink/query.h"
#include "chmlink/ring.h"
#include "chmlink/session.h"

/* Fuzzes the control interface and the shared rings with raw requests, below
 * the SDK.
 *
 * Each round opens the data session, registers a pair of valid rings, gives
 * every worker its own duplicate of the handle (so all share one file object)
 * and lets them issue random requests, including second registrations and
 * offload changes. One worker also writes random values into both ring
 * headers and record areas while the driver is using them. Workers close
 * their handles after a random number of requests, so cleanup races requests
 * still in flight on the other handles. An inspection thread opens and closes
 * read-only handles meanwhile.
 *
 * The driver must survive every input: no crash, no hang, no leak. A request
 * is accepted exactly when the contract allows it; a corrupted ring ends the
 * session (state CLOSED) instead of causing an out-of-range access, and the
 * adapter can open a new session afterwards.
 *
 * Usage: chmlink-ioctl-fuzz <interface-index> <seconds> [--seed N] [--tolerant]
 *        [--ops MASK]
 * MASK selects request kinds by bit (default all): 0 query, 1 set_offload,
 * 2 register_rings and add_inject_ring, 3 foreign codes, 4 bad buffers, 5 ring tampering,
 * 6 read/write/cancel, 7 session churn (without it one session lasts the run).
 */

#define FUZZ_WORKERS 6
#define BIG (CHMLINK_BATCH_MAX_SIZE + 64u)
#define RING_CAPACITY (4u * 1024u * 1024u)

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
static volatile LONG64 g_requests, g_registered, g_broken;
/* Whether this round's session has rings: set by the round's registration or,
 * when that failed (tolerant runs under low resources), by the first worker
 * whose registration the driver accepts. Only one can succeed. */
static volatile LONG g_round_registered;
/* Spare to-Windows rings for ADD_INJECT_RING, one set per round. */
#define EXTRA_RINGS (CHMLINK_INJECT_RINGS_MAX - 1)
#define EXTRA_CAPACITY (128u * 1024u)
static uint8_t *g_extra[EXTRA_RINGS];
static HANDLE g_extra_event[EXTRA_RINGS];
static volatile LONG g_extra_next, g_extra_added;
static volatile LONG64 g_added_total;
static uint8_t *g_readonly_page, *g_guard_edge;
static uint8_t *g_from, *g_to;
static HANDLE g_from_event, g_to_event;
static chmlink_session_rings g_rings;
static uint8_t g_register_request[CHMLINK_SESSION_SIZE];

typedef struct rng { unsigned long long s; } rng;
static uint32_t next(rng *r)
{
    r->s ^= r->s >> 12; r->s ^= r->s << 25; r->s ^= r->s >> 27;
    return (uint32_t)((r->s * 2685821657736338717ull) >> 32);
}
static uint32_t below(rng *r, uint32_t n) { return n ? next(r) % n : 0; }

static uint32_t edge_length(rng *r)
{
    static const uint32_t edges[] = { 0, 1, 3, 4, 7, 8, 9, 23, 24, 25, 27, 28, 55, 56, 57, 64,
        127, 128, 129, 4096, 65535, 65536, CHMLINK_BATCH_MAX_SIZE,
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

/* A structurally valid registration request with one random field replaced. */
static uint32_t register_input(rng *r, uint8_t *buffer)
{
    static const uint32_t capacities[] = { 0, 1, 127, 128 * 1024 - 16, 128 * 1024,
        4 * 1024 * 1024, 64 * 1024 * 1024, 64 * 1024 * 1024 + 16 };
    uint32_t kind = below(r, 5), i, capacity;
    if (kind == 0) {
        uint32_t length = edge_length(r);
        if (length > BIG) length = BIG;
        for (i = 0; i < length; ++i) buffer[i] = (uint8_t)next(r);
        return length;
    }
    memcpy(buffer, g_register_request, CHMLINK_SESSION_SIZE);
    if (kind == 1) { buffer[below(r, 56)] ^= (uint8_t)(1u << below(r, 8)); return 56; }
    if (kind == 2) {
        uint32_t length = 56;
        if (below(r, 2)) length += 1 + below(r, 8);
        else if (length > 8) length -= 8;
        return length;
    }
    capacity = capacities[below(r, (uint32_t)(sizeof(capacities) / sizeof(capacities[0])))];
    buffer[16] = (uint8_t)capacity; buffer[17] = (uint8_t)(capacity >> 8);
    buffer[18] = (uint8_t)(capacity >> 16); buffer[19] = (uint8_t)(capacity >> 24);
    if (below(r, 2)) buffer[8] ^= (uint8_t)below(r, 64);   /* misalign the address */
    return 56;
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
    return code == CHMLINK_IOCTL_QUERY || code == CHMLINK_IOCTL_REGISTER_RINGS ||
           code == CHMLINK_IOCTL_SET_OFFLOAD || code == CHMLINK_IOCTL_ADD_INJECT_RING;
}

static void tamper(rng *r)
{
    static const uint32_t values[] = { 0, 1, 2, 3, 15, 16, 17, 0x7fffffffu, 0xffffffffu };
    uint32_t value = values[below(r, (uint32_t)(sizeof(values) / sizeof(values[0])))];
    uint8_t *ring = below(r, 2) ? g_from : g_to;
    switch (below(r, 5)) {
    case 0: memcpy(ring + CHMLINK_RING_HEAD_OFFSET, &value, 4); break;
    case 1: memcpy(ring + CHMLINK_RING_TAIL_OFFSET, &value, 4); break;
    case 2: memcpy(ring + CHMLINK_RING_STATE_OFFSET, &value, 4); break;
    case 3: memcpy(ring + CHMLINK_RING_HEADER_SIZE + below(r, 1024), &value, 4); break;
    default: ring[below(r, CHMLINK_RING_HEADER_SIZE + 2048)] = (uint8_t)next(r); break;
    }
    SetEvent(below(r, 2) ? g_from_event : g_to_event);
}

static void one_request(worker *w, int session)
{
    DWORD in_size, out_size, got, returned, code;
    rng *r = &w->r;
    static const int kinds[8] = { 0, 1, 1, 2, 2, 3, 4, 5 };
    uint32_t pick;
    do pick = below(r, 8); while (!(g_ops & (1ul << kinds[pick])) && (g_ops & 0x7f));
    if (!(g_ops & 0x7f)) return;
    switch (pick) {
    case 0: {
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
    case 1: {
        uint32_t value = below(r, 4) == 0 ? next(r) : next(r) & 0xfu;
        int valid;
        in_size = below(r, 4) == 0 ? edge_length(r) : 4;
        out_size = below(r, 5) == 0 ? edge_length(r) % 64 : 4;
        if (in_size > BIG) in_size = BIG;
        if (out_size > BIG) out_size = BIG;
        for (returned = 0; returned < in_size && returned < 4; ++returned)
            w->in[returned] = (uint8_t)(value >> (8 * returned));
        valid = in_size == 4 && out_size == 4 && (value & ~0xfu) == 0;
        got = issue(w->handle, w->event, CHMLINK_IOCTL_SET_OFFLOAD, in_size ? w->in : NULL, in_size,
                    out_size ? w->out : NULL, out_size, &returned);
        if (!session) expect(got, ERROR_ACCESS_DENIED, "SET_OFFLOAD on inspection handle", in_size, out_size);
        else expect(got, valid ? 0 : ERROR_INVALID_PARAMETER, "SET_OFFLOAD", in_size, out_size);
        if (got == 0 && !valid) FAIL("SET_OFFLOAD accepted a malformed request");
        break;
    }
    case 2:
        if (below(r, 3) == 0) {
            /* ADD_INJECT_RING: a fresh spare ring, possibly with one field broken. */
            chmlink_inject_ring q;
            uint8_t raw[CHMLINK_INJECT_RING_SIZE];
            int valid;
            LONG slot = InterlockedIncrement(&g_extra_next) - 1;
            memset(&q, 0, sizeof(q));
            q.version = CHMLINK_INJECT_RING_VERSION;
            q.capacity = EXTRA_CAPACITY;
            if (slot < (LONG)EXTRA_RINGS) {
                q.ring = (uint64_t)(ULONG_PTR)g_extra[slot];
                q.event = (uint64_t)(ULONG_PTR)g_extra_event[slot];
            } else {   /* spares used up: memory the session already uses */
                q.ring = (uint64_t)(ULONG_PTR)g_extra[0];
                q.event = (uint64_t)(ULONG_PTR)g_extra_event[0];
            }
            chmlink_inject_ring_encode(raw, &q);
            in_size = CHMLINK_INJECT_RING_SIZE;
            if (below(r, 3) == 0) raw[below(r, CHMLINK_INJECT_RING_SIZE)] ^= (uint8_t)(1u << below(r, 8));
            if (below(r, 8) == 0) in_size = below(r, 2) ? in_size - 1 - below(r, 8) : in_size + 1 + below(r, 8);
            out_size = below(r, 6) == 0 ? 4 : 0;
            memcpy(w->in, raw, CHMLINK_INJECT_RING_SIZE);
            valid = in_size == CHMLINK_INJECT_RING_SIZE && out_size == 0 &&
                    chmlink_inject_ring_decode(&q, w->in, in_size) == CHMLINK_OK;
            got = issue(w->handle, w->event, CHMLINK_IOCTL_ADD_INJECT_RING, w->in, in_size,
                        out_size ? w->out : NULL, out_size, &returned);
            if (!session) {
                expect(got, ERROR_ACCESS_DENIED, "ADD_INJECT_RING on inspection handle", in_size, out_size);
            } else if (got == 0) {
                if (!valid) FAIL("ADD_INJECT_RING accepted a malformed request");
                InterlockedIncrement64(&g_added_total);
                if (InterlockedIncrement(&g_extra_added) > (LONG)EXTRA_RINGS)
                    FAIL("ADD_INJECT_RING accepted more than %u rings", (unsigned)CHMLINK_INJECT_RINGS_MAX);
            } else if (!valid) {
                expect(got, ERROR_INVALID_PARAMETER, "ADD_INJECT_RING", in_size, out_size);
            } else if (!g_tolerant && got != ERROR_BAD_COMMAND && got != ERROR_NOACCESS &&
                       got != ERROR_ACCESS_DENIED && got != ERROR_INVALID_HANDLE) {
                /* A decodable request may name a mutated address or handle. */
                FAIL("ADD_INJECT_RING valid request: error %lu", got);
            }
            break;
        }
        /* otherwise REGISTER_RINGS */
    {
        chmlink_session_rings rings;
        int valid;
        in_size = register_input(r, w->in);   /* up to BIG bytes */
        out_size = below(r, 6) == 0 ? edge_length(r) % 64 : 0;
        if (in_size > BIG) in_size = BIG;
        if (out_size > BIG) out_size = BIG;
        valid = in_size == CHMLINK_SESSION_SIZE && out_size == 0 &&
                chmlink_session_decode(&rings, w->in, in_size) == CHMLINK_OK;
        got = issue(w->handle, w->event, CHMLINK_IOCTL_REGISTER_RINGS, in_size ? w->in : NULL, in_size,
                    out_size ? w->out : NULL, out_size, &returned);
        if (!session) {
            expect(got, ERROR_ACCESS_DENIED, "REGISTER on inspection handle", in_size, out_size);
        } else if (valid && got == 0) {
            /* Only possible when the round's own registration failed (tolerant runs). */
            if (!g_tolerant || InterlockedCompareExchange(&g_round_registered, 1, 0) != 0)
                FAIL("REGISTER accepted a second registration (in=%lu)", in_size);
            InterlockedIncrement64(&g_registered);
        } else {
            expect(got, valid ? ERROR_BAD_COMMAND : ERROR_INVALID_PARAMETER, "REGISTER", in_size, out_size);
            /* Strict: the round registered first, so a decodable request finds the session taken.
             * Tolerant: when that registration failed, a decodable request really maps memory
             * and handles and may fail for either. */
            if (!g_tolerant && valid && got != ERROR_BAD_COMMAND)
                FAIL("REGISTER valid request: error %lu", got);
        }
        break;
    }
    case 3: {
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
    case 4: {
        void *in = NULL, *out = NULL;
        DWORD kind = below(r, 4);
        code = below(r, 2) ? CHMLINK_IOCTL_QUERY : CHMLINK_IOCTL_REGISTER_RINGS;
        in_size = code == CHMLINK_IOCTL_QUERY ? 0 : CHMLINK_SESSION_SIZE; out_size = CHMLINK_INFO_SIZE;
        if (kind == 0) out = NULL;
        else if (kind == 1) out = g_readonly_page;
        else if (kind == 2) out = g_guard_edge;
        else { out = NULL; in = g_guard_edge; out_size = 0; }
        got = issue(w->handle, w->event, code, in, in_size, out, out_size, &returned);
        expect_failure(got, "bad buffer", in_size, out_size);
        break;
    }
    case 5:
        /* Only session workers: the rings are freed between rounds while the
         * inspection thread keeps running. */
        if (session) tamper(r);
        break;
    default: {
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
    CloseHandle(w->handle);
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
                            SYNCHRONIZE, FALSE, 0));
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

static int create_rings(void)
{
    g_from = VirtualAlloc(NULL, CHMLINK_RING_HEADER_SIZE + RING_CAPACITY, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_to = VirtualAlloc(NULL, CHMLINK_RING_HEADER_SIZE + RING_CAPACITY, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_from_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_to_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_from || !g_to || !g_from_event || !g_to_event) return 0;
    {
        unsigned i;
        for (i = 0; i < EXTRA_RINGS; ++i) {
            g_extra[i] = VirtualAlloc(NULL, CHMLINK_RING_HEADER_SIZE + EXTRA_CAPACITY, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_READWRITE);
            g_extra_event[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
            if (!g_extra[i] || !g_extra_event[i]) return 0;
        }
        InterlockedExchange(&g_extra_next, 0);
        InterlockedExchange(&g_extra_added, 0);
    }
    memset(&g_rings, 0, sizeof(g_rings));
    g_rings.version = CHMLINK_SESSION_VERSION;
    g_rings.from_windows_ring = (uint64_t)(ULONG_PTR)g_from;
    g_rings.from_windows_capacity = RING_CAPACITY;
    g_rings.from_windows_event = (uint64_t)(ULONG_PTR)g_from_event;
    g_rings.to_windows_ring = (uint64_t)(ULONG_PTR)g_to;
    g_rings.to_windows_capacity = RING_CAPACITY;
    g_rings.to_windows_event = (uint64_t)(ULONG_PTR)g_to_event;
    chmlink_session_encode(g_register_request, &g_rings);
    return 1;
}

static void destroy_rings(void)
{
    if (g_from) { VirtualFree(g_from, 0, MEM_RELEASE); g_from = NULL; }
    if (g_to) { VirtualFree(g_to, 0, MEM_RELEASE); g_to = NULL; }
    if (g_from_event) { CloseHandle(g_from_event); g_from_event = NULL; }
    if (g_to_event) { CloseHandle(g_to_event); g_to_event = NULL; }
    {
        unsigned i;
        for (i = 0; i < EXTRA_RINGS; ++i) {
            if (g_extra[i]) { VirtualFree(g_extra[i], 0, MEM_RELEASE); g_extra[i] = NULL; }
            if (g_extra_event[i]) { CloseHandle(g_extra_event[i]); g_extra_event[i] = NULL; }
        }
    }
}

int wmain(int argc, wchar_t **argv)
{
    worker workers[FUZZ_WORKERS], inspect;
    HANDLE threads[FUZZ_WORKERS], inspect_thread;
    chmlink_query_info info, after;
    unsigned long rounds = 0;
    DWORD old, returned;
    int i;
    uint8_t *pages;
    if (argc < 3) { fwprintf(stderr, L"usage: chmlink-ioctl-fuzz <index> <seconds> [--seed N] [--tolerant]\n"); return 2; }
    g_index = (uint32_t)wcstoul(argv[1], NULL, 10);
    g_deadline = GetTickCount64() + 1000ull * wcstoul(argv[2], NULL, 10);
    g_seed = GetTickCount64() ^ ((unsigned long long)GetCurrentProcessId() << 32);
    for (i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"--seed") == 0 && i + 1 < argc) g_seed = _wcstoui64(argv[++i], NULL, 10);
        else if (wcscmp(argv[i], L"--tolerant") == 0) g_tolerant = 1;
        else if (wcscmp(argv[i], L"--ops") == 0 && i + 1 < argc) g_ops = wcstoul(argv[++i], NULL, 0);
        else { fwprintf(stderr, L"unknown argument %ls\n", argv[i]); return 2; }
    }
    swprintf(g_path, 64, L"\\\\.\\ChmLink%lu", (unsigned long)g_index);
    printf("seed %llu%s\n", (unsigned long long)g_seed, g_tolerant ? " (tolerant)" : ""); fflush(stdout);

    pages = VirtualAlloc(NULL, 3 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); REQUIRE(pages);
    REQUIRE(VirtualProtect(pages, 4096, PAGE_READONLY, &old));
    REQUIRE(VirtualProtect(pages + 2 * 4096, 4096, PAGE_NOACCESS, &old));
    g_readonly_page = pages; g_guard_edge = pages + 2 * 4096 - 64;

    if (!query_state(&info)) {
        ULONGLONG give_up = GetTickCount64() + 20000;
        while (g_tolerant && GetTickCount64() < give_up && !query_state(&info)) Sleep(200);
        if (!query_state(&info)) {
            if (!g_tolerant) FAIL("cannot query adapter %u", g_index);
            printf("SKIP adapter %u unavailable\n", g_index); return 0;
        }
    }
    if (!g_tolerant && (info.state & CHMLINK_STATE_SESSION)) FAIL("adapter %u already has a data session", g_index);
    g_mode = info.mode; g_limit = info.packet_limit;

    {
        wchar_t sub[80];
        HANDLE h;
        swprintf(sub, 80, L"%ls\\extra", g_path);
        h = CreateFileW(sub, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) FAIL("opened a name below the device");
    }

    init_worker(&inspect, g_seed + 1000);
    inspect_thread = CreateThread(NULL, 0, inspector, &inspect, 0, NULL); REQUIRE(inspect_thread);
    while (GetTickCount64() < g_deadline) {
        HANDLE session = open_session();
        LONG64 registered_before = g_registered;
        if (session == NULL) { if (g_tolerant) continue; FAIL("session never became free"); }
        if (!create_rings()) { CloseHandle(session); FAIL("ring allocation failed"); }
        SetLastError(ERROR_SUCCESS);
        InterlockedExchange(&g_round_registered, 0);
        if (DeviceIoControl(session, CHMLINK_IOCTL_REGISTER_RINGS, g_register_request,
                            CHMLINK_SESSION_SIZE, NULL, 0, &returned, NULL)) {
            InterlockedExchange(&g_round_registered, 1);
            InterlockedIncrement64(&g_registered);
        } else if (!g_tolerant) {
            FAIL("REGISTER_RINGS failed: %lu", GetLastError());
        }
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
        REQUIRE(WaitForMultipleObjects(FUZZ_WORKERS, threads, TRUE,
                (DWORD)(g_deadline - GetTickCount64() + 120000)) == WAIT_OBJECT_0);
        for (i = 0; i < FUZZ_WORKERS; ++i) CloseHandle(threads[i]);
        if (g_ops & 0x20) {
            /* A misaligned tail must make the driver close the session rather
             * than read out of range. */
            chmlink_ring view;
            uint32_t bad = 2;
            memcpy(g_to + CHMLINK_RING_TAIL_OFFSET, &bad, 4);
            SetEvent(g_to_event);
            Sleep(50);
            REQUIRE(chmlink_ring_attach(&view, g_from, RING_CAPACITY));
            if ((chmlink_ring_load(&view, CHMLINK_RING_STATE_OFFSET) & CHMLINK_RING_STATE_CLOSED) != 0)
                InterlockedIncrement64(&g_broken);
            else if (!g_tolerant) FAIL("a corrupted ring was not closed");
        }
        destroy_rings();
        if (!g_tolerant) {
            REQUIRE(query_state(&after));
            if (after.state & CHMLINK_STATE_SESSION) FAIL("session still open after every handle closed");
            if (!(after.state & CHMLINK_STATE_RUNNING)) FAIL("adapter not running after round %lu", rounds);
        }
        if (g_registered == registered_before && !g_tolerant) FAIL("no ring registration in a round");
        rounds++;
    }
    InterlockedExchange(&g_stop, 1);
    REQUIRE(WaitForSingleObject(inspect_thread, 30000) == WAIT_OBJECT_0);
    printf("PASS ioctl fuzz: %lu rounds, %lld requests, %lld registrations, %lld added rings, %lld broken sessions\n",
           rounds, (long long)g_requests, (long long)g_registered, (long long)g_added_total, (long long)g_broken);
    return 0;
}
