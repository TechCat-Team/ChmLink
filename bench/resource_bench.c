/* Resource use of an open session: CPU at fixed packet rates, memory and
 * threads per session, and (ChmLink only) a long soak for leaks in the SDK.
 * Built as chmlink-resource-bench and, with bench/wintun_shim.c, as
 * wintun-resource-bench, so both drivers run the same traffic and accounting.
 * Opt-in, for the disposable test VM through tools/bench-resources.ps1.
 * Output is one JSON object per line.
 *
 *   rate <ifIndex> <local-ipv4> <peer-ipv4> <seconds> <pps>...
 *       For each rate: <pps> UDP packets per second from Windows (a socket
 *       sends to the peer, a consumer thread waits and receives) and <pps>
 *       per second to Windows (a pacer injects, a socket receives), at the
 *       same time. Rates up to 1000 are paced by a timer; higher ones twice:
 *       "burst", a batch every millisecond (as interrupt moderation delivers
 *       network traffic), and "even", evenly spaced by a spinning pacer whose
 *       own CPU time is subtracted. 0 is an open, idle session.
 *   memory <ifIndex>
 *       Memory, threads and kernel pool before and after opening a session,
 *       and (ChmLink) after four threads each injected once (four rings).
 *   soak <signed-inf> <minutes>   (ChmLink only)
 *       Create an adapter, address it, open a session, move traffic both ways,
 *       close and delete, over and over; reports the process's private bytes,
 *       handles and threads as it goes.
 *
 * CPU figures: "machine" is all cores busy above the baseline measured before
 * the session opened, minus the pacers' own time (they stand in for the
 * network); "system" is the System process (driver threads and the stack work
 * they carry); "app" is the receiving thread of the application. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (win=%lu wsa=%d)\n", \
    __FILE__, __LINE__, #x, GetLastError(), WSAGetLastError()); ExitProcess(1); } } while (0)

#define PORT_TO_PEER 51000u
#define PORT_FROM_PEER 51001u
#define PORT_LOCAL_RX 51002u
#define PORT_LOCAL_TX 51003u
#define PAYLOAD 64

static LARGE_INTEGER qpf;
static uint8_t local4[4], peer4[4];
static chmlink_adapter *adapter;
static volatile LONG stopping;

static ULONGLONG ft64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static double now_s(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)qpf.QuadPart; }

static double thread_cpu(HANDLE thread)
{
    FILETIME c, e, k, u;
    REQUIRE(GetThreadTimes(thread, &c, &e, &k, &u));
    return (double)(ft64(k) + ft64(u)) / 1e7;
}

static double system_process_cpu(void)
{
    HANDLE system = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 4);
    FILETIME c, e, k, u;
    double t = 0;
    if (system != NULL && GetProcessTimes(system, &c, &e, &k, &u)) t = (double)(ft64(k) + ft64(u)) / 1e7;
    if (system != NULL) CloseHandle(system);
    return t;
}

/* All cores: busy seconds so far. */
static double machine_busy(void)
{
    FILETIME i, k, u;
    REQUIRE(GetSystemTimes(&i, &k, &u));
    return (double)(ft64(k) - ft64(i) + ft64(u)) / 1e7;
}

static DWORD threads_of(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 t = {sizeof(t)};
    DWORD n = 0;
    REQUIRE(snap != INVALID_HANDLE_VALUE);
    for (BOOL ok = Thread32First(snap, &t); ok; ok = Thread32Next(snap, &t)) if (t.th32OwnerProcessID == pid) ++n;
    CloseHandle(snap);
    return n;
}

/* ---- context switches per second, machine-wide (PDH) ---- */
static PDH_HQUERY pdh_query;
static PDH_HCOUNTER pdh_switches;

static void switches_start(void)
{
    if (pdh_query == NULL) {
        REQUIRE(PdhOpenQueryW(NULL, 0, &pdh_query) == ERROR_SUCCESS);
        REQUIRE(PdhAddEnglishCounterW(pdh_query, L"\\System\\Context Switches/sec", 0, &pdh_switches) == ERROR_SUCCESS);
    }
    PdhCollectQueryData(pdh_query);
}

static double switches_rate(void)
{
    PDH_FMT_COUNTERVALUE v;
    if (PdhCollectQueryData(pdh_query) != ERROR_SUCCESS ||
        PdhGetFormattedCounterValue(pdh_switches, PDH_FMT_DOUBLE, NULL, &v) != ERROR_SUCCESS) return -1;
    return v.doubleValue;
}

/* ---- packets ---- */
static uint16_t ip_checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    while (n >= 2) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static size_t build_udp4(uint8_t *out, unsigned sport, unsigned dport, uint32_t seq)
{
    size_t total = 28 + PAYLOAD;
    uint16_t sum;
    memset(out, 0, total);
    out[0] = 0x45; out[2] = (uint8_t)(total >> 8); out[3] = (uint8_t)total;
    out[4] = (uint8_t)(seq >> 8); out[5] = (uint8_t)seq; out[6] = 0x40;
    out[8] = 64; out[9] = IPPROTO_UDP;
    memcpy(out + 12, peer4, 4); memcpy(out + 16, local4, 4);
    sum = ip_checksum(out, 20);
    out[10] = (uint8_t)(sum >> 8); out[11] = (uint8_t)sum;
    out[20] = (uint8_t)(sport >> 8); out[21] = (uint8_t)sport;
    out[22] = (uint8_t)(dport >> 8); out[23] = (uint8_t)dport;
    out[24] = (uint8_t)((8 + PAYLOAD) >> 8); out[25] = (uint8_t)(8 + PAYLOAD);
    return total;
}

/* A batch of `count` packets to Windows (UDP checksum zero: not computed). */
static uint8_t *make_batch(uint32_t count, uint32_t *length)
{
    uint8_t packet[256], *buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    chmlink_batch_writer w;
    uint32_t i;
    REQUIRE(buffer != NULL);
    REQUIRE(chmlink_batch_begin(&w, buffer, CHMLINK_BATCH_MAX_SIZE, CHMLINK_MODE_IP, 1500) == CHMLINK_OK);
    for (i = 0; i < count; ++i)
        REQUIRE(chmlink_batch_append(&w, packet, build_udp4(packet, PORT_FROM_PEER, PORT_LOCAL_RX, i)) == CHMLINK_OK);
    *length = (uint32_t)w.length;
    return buffer;
}

static SOCKET udp_socket(unsigned bind_port, int connect_peer, unsigned peer_port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {0};
    DWORD timeout = 100;
    int big = 4 << 20;
    REQUIRE(s != INVALID_SOCKET);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)bind_port); memcpy(&a.sin_addr, local4, 4);
    REQUIRE(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof(big));
    if (connect_peer) {
        a.sin_port = htons((u_short)peer_port); memcpy(&a.sin_addr, peer4, 4);
        REQUIRE(connect(s, (struct sockaddr *)&a, sizeof(a)) == 0);
    }
    return s;
}

/* ---- the threads of one rate step ---- */
typedef struct pacer {
    uint32_t pps, per_tick;
    double interval;          /* seconds between ticks */
    int spin;                 /* pace by spinning (sub-millisecond intervals) */
    int inject;               /* inject to Windows, else send from Windows */
    SOCKET so;
    uint8_t *batch; uint32_t batch_length;
    volatile LONG64 sent;
} pacer;

static DWORD WINAPI pacer_thread(void *arg)
{
    pacer *p = arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    char payload[PAYLOAD];
    double next = now_s();
    uint32_t i;
    memset(payload, 0x5a, sizeof(payload));
    if (timer == NULL) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    REQUIRE(timer != NULL);
    while (!stopping) {
        if (p->inject) {
            if (chmlink_inject(adapter, p->batch, p->batch_length)) p->sent += p->per_tick;
        } else {
            for (i = 0; i < p->per_tick; ++i) if (send(p->so, payload, PAYLOAD, 0) == PAYLOAD) p->sent++;
        }
        next += p->interval;
        if (p->spin) {
            while (now_s() < next && !stopping) YieldProcessor();
        } else {
            double wait = next - now_s();
            if (wait > 0) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(wait * 1e7);
                SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                WaitForSingleObject(timer, INFINITE);
            }
        }
    }
    CloseHandle(timer);
    return 0;
}

static volatile LONG64 consumed, received;

/* The application's receive loop: wait, then take everything. */
static DWORD WINAPI consumer_thread(void *arg)
{
    uint8_t *buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    uint32_t bytes;
    (void)arg;
    REQUIRE(buffer != NULL);
    while (!stopping) {
        if (!chmlink_wait_readable(adapter, 100)) continue;
        while (chmlink_receive(adapter, buffer, CHMLINK_BATCH_MAX_SIZE, &bytes)) {
            chmlink_batch_reader r;
            const uint8_t *packet;
            uint32_t size;
            (void)bytes;
            chmlink_batch_read_begin(&r, buffer);
            while (chmlink_batch_read_next(&r, &packet, &size, NULL)) consumed++;
        }
    }
    free(buffer);
    return 0;
}

static DWORD WINAPI receiver_thread(void *arg)
{
    SOCKET s = *(SOCKET *)arg;
    char buffer[2048];
    while (!stopping) if (recv(s, buffer, sizeof(buffer), 0) > 0) received++;
    return 0;
}

typedef struct step_result {
    double machine, system, app, socket_rx, switches, from_pps, to_pps;
} step_result;

static double baseline_busy, baseline_switches;

static void measure_baseline(double seconds)
{
    double t0, b0;
    switches_start();
    t0 = now_s(); b0 = machine_busy();
    Sleep((DWORD)(seconds * 1000));
    baseline_busy = (machine_busy() - b0) / (now_s() - t0);
    baseline_switches = switches_rate();
}

static step_result run_step(uint32_t pps, int spin, double seconds)
{
    pacer to = {0}, from = {0};
    HANDLE threads[4] = {0}, receiver = NULL;
    SOCKET rx = udp_socket(PORT_LOCAL_RX, 0, 0), tx = udp_socket(PORT_LOCAL_TX, 1, PORT_TO_PEER);
    step_result r = {0};
    double t0, t1, busy0, sys0, app0, sock0, pace0;
    LONG64 c0, r0;
    int n = 0, i;
    stopping = 0;
    threads[n++] = CreateThread(NULL, 0, consumer_thread, NULL, 0, NULL);
    receiver = CreateThread(NULL, 0, receiver_thread, &rx, 0, NULL);
    if (pps > 0) {
        double interval = spin ? 1.0 / pps : (pps <= 1000 ? 1.0 / pps : 0.001);
        uint32_t per_tick = spin || pps <= 1000 ? 1 : pps / 1000;
        to.pps = from.pps = pps; to.spin = from.spin = spin;
        to.interval = from.interval = interval; to.per_tick = from.per_tick = per_tick;
        to.inject = 1; to.batch = make_batch(per_tick, &to.batch_length);
        from.so = tx;
        threads[n++] = CreateThread(NULL, 0, pacer_thread, &to, 0, NULL);
        threads[n++] = CreateThread(NULL, 0, pacer_thread, &from, 0, NULL);
    }
    for (i = 0; i < n; ++i) REQUIRE(threads[i] != NULL);
    REQUIRE(receiver != NULL);
    Sleep(1000);                                   /* settle */
    switches_start();
    t0 = now_s(); busy0 = machine_busy(); sys0 = system_process_cpu();
    app0 = thread_cpu(threads[0]); sock0 = thread_cpu(receiver);
    pace0 = 0; for (i = 1; i < n; ++i) pace0 += thread_cpu(threads[i]);
    c0 = consumed; r0 = received;
    Sleep((DWORD)(seconds * 1000));
    t1 = now_s();
    {
        double pace1 = 0, wall = t1 - t0;
        for (i = 1; i < n; ++i) pace1 += thread_cpu(threads[i]);
        r.machine = (machine_busy() - busy0 - (pace1 - pace0)) / wall - baseline_busy;
        r.system = (system_process_cpu() - sys0) / wall;
        r.app = (thread_cpu(threads[0]) - app0) / wall;
        r.socket_rx = (thread_cpu(receiver) - sock0) / wall;
        r.switches = switches_rate() - baseline_switches;
        r.from_pps = (double)(consumed - c0) / wall;
        r.to_pps = (double)(received - r0) / wall;
    }
    stopping = 1;
    for (i = 0; i < n; ++i) { WaitForSingleObject(threads[i], INFINITE); CloseHandle(threads[i]); }
    WaitForSingleObject(receiver, INFINITE); CloseHandle(receiver);
    closesocket(rx); closesocket(tx);
    free(to.batch);
    return r;
}

static int run_rates(int argc, char **argv)
{
    double seconds = atof(argv[5]);
    int i, spin;
    REQUIRE(seconds >= 2);
    measure_baseline(seconds);
    printf("{\"step\":\"baseline\",\"busy_cores\":%.4f,\"switches_per_s\":%.0f}\n", baseline_busy, baseline_switches);
    adapter = chmlink_open((uint32_t)atoi(argv[2]));
    REQUIRE(adapter != NULL);
    Sleep(2000);
    for (i = 6; i < argc; ++i) {
        uint32_t pps = (uint32_t)atoi(argv[i]);
        for (spin = 0; spin <= (pps > 1000 ? 1 : 0); ++spin) {
            step_result r = run_step(pps, spin, seconds);
            printf("{\"step\":\"rate\",\"pps\":%u,\"pacing\":\"%s\",\"machine_cores\":%.4f,\"system_cores\":%.4f,"
                   "\"app_cores\":%.4f,\"socket_rx_cores\":%.4f,\"extra_switches_per_s\":%.0f,"
                   "\"from_windows_pps\":%.0f,\"to_windows_pps\":%.0f}\n",
                   pps, pps <= 1000 ? "timer" : spin ? "even" : "burst", r.machine, r.system, r.app, r.socket_rx,
                   r.switches, r.from_pps, r.to_pps);
            fflush(stdout);
        }
    }
    chmlink_close(adapter);
    return 0;
}

/* ---- memory ---- */
typedef struct footprint { double private_mib, working_mib, nonpaged_mib, commit_mib; DWORD threads, system_threads; } footprint;

static footprint take_footprint(void)
{
    PROCESS_MEMORY_COUNTERS_EX m = {sizeof(m)};
    PERFORMANCE_INFORMATION p = {sizeof(p)};
    footprint f;
    Sleep(1500);
    REQUIRE(GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof(m)));
    REQUIRE(GetPerformanceInfo(&p, sizeof(p)));
    f.private_mib = (double)m.PrivateUsage / 1048576.0;
    f.working_mib = (double)m.WorkingSetSize / 1048576.0;
    f.nonpaged_mib = (double)p.KernelNonpaged * p.PageSize / 1048576.0;
    f.commit_mib = (double)p.CommitTotal * p.PageSize / 1048576.0;
    f.threads = threads_of(GetCurrentProcessId());
    f.system_threads = threads_of(4);
    return f;
}

static void print_delta(const char *what, const footprint *a, const footprint *b)
{
    printf("{\"step\":\"memory\",\"after\":\"%s\",\"private_mib\":%.2f,\"working_set_mib\":%.2f,\"kernel_nonpaged_mib\":%.2f,"
           "\"commit_mib\":%.2f,\"process_threads\":%ld,\"system_threads\":%ld}\n", what,
           b->private_mib - a->private_mib, b->working_mib - a->working_mib, b->nonpaged_mib - a->nonpaged_mib,
           b->commit_mib - a->commit_mib, (long)b->threads - (long)a->threads,
           (long)b->system_threads - (long)a->system_threads);
}

static DWORD WINAPI inject_once(void *arg)
{
    uint32_t length;
    uint8_t *batch = make_batch(1, &length);
    (void)arg;
    chmlink_inject(adapter, batch, length);
    free(batch);
    Sleep(4000);   /* keep the thread (and its ring mapping) while measured */
    return 0;
}

static int run_memory(char **argv)
{
    footprint before = take_footprint(), opened, rings;
    HANDLE threads[3];
    int i;
    adapter = chmlink_open((uint32_t)atoi(argv[2]));
    REQUIRE(adapter != NULL);
    opened = take_footprint();
    print_delta("open", &before, &opened);
#ifndef CHMLINK_RESOURCE_WINTUN
    {
        uint32_t length;
        uint8_t *batch = make_batch(1, &length);
        chmlink_inject(adapter, batch, length);   /* this thread: ring 1 */
        free(batch);
        for (i = 0; i < 3; ++i) threads[i] = CreateThread(NULL, 0, inject_once, NULL, 0, NULL);
        rings = take_footprint();
        rings.threads -= 3;                        /* the injecting threads themselves */
        print_delta("four_rings", &before, &rings);
        for (i = 0; i < 3; ++i) { WaitForSingleObject(threads[i], INFINITE); CloseHandle(threads[i]); }
    }
#else
    (void)threads; (void)i; (void)rings;
#endif
    chmlink_close(adapter);
    {
        footprint closed = take_footprint();
        print_delta("close", &before, &closed);
    }
    return 0;
}

/* ---- soak (ChmLink) ---- */
#ifndef CHMLINK_RESOURCE_WINTUN
static const GUID soak_owner = {0x6e2b1c4d, 0x8a3f, 0x4b71, {0x9c, 0x05, 0x3d, 0x7e, 0x12, 0xa4, 0x6b, 0x90}};

static void report(const char *label, unsigned cycles, double minutes)
{
    PROCESS_MEMORY_COUNTERS_EX m = {sizeof(m)};
    PERFORMANCE_INFORMATION p = {sizeof(p)};
    DWORD handles = 0;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof(m));
    GetPerformanceInfo(&p, sizeof(p));
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    printf("{\"step\":\"%s\",\"minutes\":%.1f,\"cycles\":%u,\"private_kib\":%llu,\"handles\":%lu,\"threads\":%lu,"
           "\"kernel_nonpaged_kib\":%llu}\n", label, minutes, cycles, (unsigned long long)m.PrivateUsage / 1024,
           handles, threads_of(GetCurrentProcessId()), (unsigned long long)p.KernelNonpaged * p.PageSize / 1024);
    fflush(stdout);
}

static int run_soak(char **argv)
{
    double minutes = atof(argv[3]), start = now_s(), last = -1;
    unsigned cycles = 0, failures = 0;
    REQUIRE(minutes > 0);
    while ((now_s() - start) / 60.0 < minutes) {
        chmlink_create_options o = {sizeof(o)};
        chmlink_device d;
        MIB_UNICASTIPADDRESS_ROW row;
        o.mode = CHMLINK_MODE_IP; o.name = L"ChmLink Soak"; o.owner = &soak_owner; o.driver_inf = NULL;
        {
            static wchar_t inf[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, argv[2], -1, inf, MAX_PATH);
            o.driver_inf = inf;
        }
        if (!chmlink_create_adapter(&o, &d)) { failures++; fprintf(stderr, "create failed %lu\n", GetLastError()); Sleep(1000); continue; }
        adapter = chmlink_open(d.interface_index);
        if (adapter != NULL) {
            InitializeUnicastIpAddressEntry(&row);
            row.InterfaceLuid.Value = 0;
            ConvertInterfaceIndexToLuid(d.interface_index, &row.InterfaceLuid);
            row.Address.Ipv4.sin_family = AF_INET; memcpy(&row.Address.Ipv4.sin_addr, local4, 4);
            row.OnLinkPrefixLength = 24; row.DadState = IpDadStatePreferred;
            if (CreateUnicastIpAddressEntry(&row) == NO_ERROR) {
                int i;
                for (i = 0; i < 40 && GetUnicastIpAddressEntry(&row) == NO_ERROR && row.DadState != IpDadStatePreferred; ++i) Sleep(50);
                /* a short burst both ways, with the coalescer and zero-copy calls too */
                {
                    step_result r = run_step(5000, 0, 2);
                    if (r.from_pps < 1000 || r.to_pps < 1000) { failures++; fprintf(stderr, "traffic %.0f/%.0f\n", r.from_pps, r.to_pps); }
                }
            } else failures++;
            chmlink_close(adapter);
            adapter = NULL;
        } else failures++;
        if (!chmlink_delete_adapter(d.instance_id, &soak_owner)) { failures++; fprintf(stderr, "delete failed %lu\n", GetLastError()); }
        ++cycles;
        if (cycles == 5) report("soak_start", cycles, (now_s() - start) / 60.0);
        if ((now_s() - start) - last >= 300) { report("soak", cycles, (now_s() - start) / 60.0); last = now_s() - start; }
    }
    report("soak_end", cycles, (now_s() - start) / 60.0);
    printf("{\"step\":\"soak_result\",\"cycles\":%u,\"failures\":%u}\n", cycles, failures);
    return failures == 0 ? 0 : 1;
}
#endif

int main(int argc, char **argv)
{
    WSADATA wsa;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    QueryPerformanceFrequency(&qpf);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    if (argc >= 7 && strcmp(argv[1], "rate") == 0) {
        REQUIRE(inet_pton(AF_INET, argv[3], local4) == 1 && inet_pton(AF_INET, argv[4], peer4) == 1);
        return run_rates(argc, argv);
    }
    if (argc == 3 && strcmp(argv[1], "memory") == 0) {
        REQUIRE(inet_pton(AF_INET, "198.18.0.1", local4) == 1 && inet_pton(AF_INET, "198.18.0.2", peer4) == 1);
        return run_memory(argv);
    }
#ifndef CHMLINK_RESOURCE_WINTUN
    if (argc == 4 && strcmp(argv[1], "soak") == 0) {
        REQUIRE(inet_pton(AF_INET, "198.19.0.1", local4) == 1 && inet_pton(AF_INET, "198.19.0.2", peer4) == 1);
        return run_soak(argv);
    }
#endif
    fprintf(stderr, "usage: see the comment at the top of bench/resource_bench.c\n");
    return 2;
}
