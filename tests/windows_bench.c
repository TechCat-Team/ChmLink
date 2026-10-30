/* Data-path benchmark for an IP-mode ChmLink adapter. Opt-in, run only in the
 * disposable test VM through tools/bench.ps1, which creates and configures the
 * adapter. Output is one JSON object per line.
 *
 *   chmlink-bench <ifIndex> <local-ipv4> <peer-ipv4> [seconds] [scenario-substring]
 *
 * "tx" is Windows -> user mode (UDP sockets send to the simulated peer and a
 * consumer drains the adapter). "rx" is user mode -> Windows (prebuilt batches
 * are injected and a UDP socket counts what the stack delivers). The loopback
 * scenarios measure the plain UDP stack ceiling without the driver. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chmlink/api.h"
#include "chmlink/protocol.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (win=%lu wsa=%d)\n", \
    __FILE__, __LINE__, #x, GetLastError(), WSAGetLastError()); ExitProcess(1); } } while (0)

#define PORT_ECHO 50000u
#define PORT_RX 50001u
#define PORT_RX_SRC 50002u
#define PORT_BASE 50003u
#define MAX_RTT_SAMPLES 400000u

static LARGE_INTEGER qpf;
static uint8_t local4[4], peer4[4];
static DWORD cpu_count;

static double elapsed_s(LARGE_INTEGER a, LARGE_INTEGER b)
{
    return (double)(b.QuadPart - a.QuadPart) / (double)qpf.QuadPart;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- CPU accounting (tick granularity; use windows of several seconds) ---- */
typedef struct cpu_sample { ULONGLONG kernel, user, sys_idle, sys_kernel, sys_user; } cpu_sample;

static ULONGLONG ft64(FILETIME f)
{
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

static void cpu_take(cpu_sample *s)
{
    FILETIME c, e, k, u, i, sk, su;
    REQUIRE(GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u));
    REQUIRE(GetSystemTimes(&i, &sk, &su));
    s->kernel = ft64(k); s->user = ft64(u);
    s->sys_idle = ft64(i); s->sys_kernel = ft64(sk); s->sys_user = ft64(su);
}

typedef struct cpu_delta { double proc_kernel_s, proc_user_s, busy_cores; } cpu_delta;

static cpu_delta cpu_diff(const cpu_sample *a, const cpu_sample *b, double wall)
{
    cpu_delta d;
    ULONGLONG busy = (b->sys_kernel - a->sys_kernel) - (b->sys_idle - a->sys_idle) +
                     (b->sys_user - a->sys_user);
    d.proc_kernel_s = (double)(b->kernel - a->kernel) / 1e7;
    d.proc_user_s = (double)(b->user - a->user) / 1e7;
    d.busy_cores = wall > 0 ? ((double)busy / 1e7) / wall : 0;
    return d;
}

/* ---- packet construction ---- */
static uint16_t ip_checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    while (n >= 2) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static size_t build_udp4(uint8_t *out, const uint8_t *src, const uint8_t *dst,
                         unsigned sport, unsigned dport, size_t payload, uint32_t seq)
{
    size_t total = 28 + payload, i;
    uint16_t sum;
    memset(out, 0, total);
    out[0] = 0x45; out[2] = (uint8_t)(total >> 8); out[3] = (uint8_t)total;
    out[4] = (uint8_t)(seq >> 8); out[5] = (uint8_t)seq; out[6] = 0x40;
    out[8] = 64; out[9] = IPPROTO_UDP;
    memcpy(out + 12, src, 4); memcpy(out + 16, dst, 4);
    sum = ip_checksum(out, 20);
    out[10] = (uint8_t)(sum >> 8); out[11] = (uint8_t)sum;
    out[20] = (uint8_t)(sport >> 8); out[21] = (uint8_t)sport;
    out[22] = (uint8_t)(dport >> 8); out[23] = (uint8_t)dport;
    out[24] = (uint8_t)((8 + payload) >> 8); out[25] = (uint8_t)(8 + payload);
    for (i = 0; i < payload; ++i) out[28 + i] = (uint8_t)(0xa0 + i);
    return total;
}

static uint8_t *make_batch(uint32_t packets, size_t payload, uint32_t limit, uint32_t *length)
{
    uint8_t packet[1600], *buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    chmlink_batch_writer w;
    uint32_t i;
    REQUIRE(buffer != NULL);
    REQUIRE(chmlink_batch_begin(&w, buffer, CHMLINK_BATCH_MAX_SIZE, CHMLINK_MODE_IP, limit) == CHMLINK_OK);
    for (i = 0; i < packets; ++i) {
        size_t n = build_udp4(packet, peer4, local4, PORT_RX_SRC, PORT_RX, payload, i);
        REQUIRE(chmlink_batch_append(&w, packet, n) == CHMLINK_OK);
    }
    *length = (uint32_t)w.length;
    return buffer;
}

/* ---- adapter helpers ---- */
static void drain(chmlink_adapter *a, uint8_t *buffer)
{
    uint32_t n;
    int idle = 0;
    while (idle < 3) {
        if (chmlink_receive(a, buffer, CHMLINK_BATCH_MAX_SIZE, &n)) { idle = 0; continue; }
        REQUIRE(GetLastError() == ERROR_NO_MORE_ITEMS);
        Sleep(30); idle++;
    }
}

/* ---- worker threads ---- */
typedef struct sender_args {
    SOCKET so; int payload; volatile LONG *stop; uint64_t sent, errors;
} sender_args;

static DWORD WINAPI sender_thread(void *arg)
{
    sender_args *s = arg;
    char buffer[1500];
    memset(buffer, 0x5a, sizeof(buffer));
    while (!*s->stop) {
        if (send(s->so, buffer, s->payload, 0) == s->payload) s->sent++; else s->errors++;
    }
    return 0;
}

typedef struct consumer_args {
    chmlink_adapter *a; volatile LONG *stop; int echo; uint32_t limit;
    uint8_t *rx, *tx;
    uint64_t packets, bytes, calls, echoed, waits; DWORD error;
    DWORD stall_every_ms, stall_ms;  /* simulate a consumer descheduled periodically */
    LARGE_INTEGER last_stall;
    double wait_s, receive_s, max_wait_s, max_receive_s; /* time inside SDK calls */
} consumer_args;

static DWORD WINAPI consumer_thread(void *arg)
{
    consumer_args *c = arg;
    for (;;) {
        uint32_t n, count, i, off = CHMLINK_BATCH_HEADER_SIZE, reply = 0;
        chmlink_batch_writer w;
        int stopping = *c->stop, ok;
        LARGE_INTEGER q0, q1;
        double dt;
        if (c->stall_ms && !stopping) {
            QueryPerformanceCounter(&q0);
            if (c->last_stall.QuadPart == 0) c->last_stall = q0;
            if (elapsed_s(c->last_stall, q0) * 1000.0 >= c->stall_every_ms) {
                Sleep(c->stall_ms);
                QueryPerformanceCounter(&c->last_stall);
            }
        }
        if (!stopping) {
            QueryPerformanceCounter(&q0);
            ok = chmlink_wait_readable(c->a, 20);
            QueryPerformanceCounter(&q1);
            dt = elapsed_s(q0, q1); c->wait_s += dt; c->waits++;
            if (dt > c->max_wait_s) c->max_wait_s = dt;
            if (!ok) {
                if (GetLastError() == ERROR_TIMEOUT) continue;
                c->error = GetLastError(); break;
            }
        }
        QueryPerformanceCounter(&q0);
        ok = chmlink_receive(c->a, c->rx, CHMLINK_BATCH_MAX_SIZE, &n);
        QueryPerformanceCounter(&q1);
        dt = elapsed_s(q0, q1); c->receive_s += dt;
        if (dt > c->max_receive_s) c->max_receive_s = dt;
        if (!ok) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) { if (stopping) break; continue; }
            c->error = GetLastError(); break;
        }
        c->calls++;
        count = rd32(c->rx + 8);
        if (c->echo) REQUIRE(chmlink_batch_begin(&w, c->tx, CHMLINK_BATCH_MAX_SIZE, CHMLINK_MODE_IP, c->limit) == CHMLINK_OK);
        for (i = 0; i < count; ++i) {
            uint8_t *p, t[4];
            uint32_t len = rd32(c->rx + off);
            p = c->rx + off + 4;
            off += 4 + ((len + 3) & ~3u);
            c->packets++; c->bytes += len;
            /* Echo only IPv4/UDP datagrams addressed to the echo port; the
             * header and UDP checksums are unchanged by swapping endpoints. */
            if (c->echo && len >= 28 && p[0] == 0x45 && p[9] == IPPROTO_UDP &&
                (((unsigned)p[22] << 8) | p[23]) == PORT_ECHO) {
                memcpy(t, p + 12, 4); memcpy(p + 12, p + 16, 4); memcpy(p + 16, t, 4);
                memcpy(t, p + 20, 2); memcpy(p + 20, p + 22, 2); memcpy(p + 22, t, 2);
                if (chmlink_batch_append(&w, p, len) == CHMLINK_OK) reply++;
            }
        }
        if (reply) {
            if (!chmlink_inject(c->a, c->tx, (uint32_t)w.length)) { c->error = GetLastError(); break; }
            c->echoed += reply;
        }
    }
    return 0;
}

typedef struct injector_args {
    chmlink_adapter *a; const uint8_t *batch; uint32_t length, count;
    volatile LONG *stop; uint64_t calls, packets, errors; DWORD last_error;
} injector_args;

static DWORD WINAPI injector_thread(void *arg)
{
    injector_args *j = arg;
    while (!*j->stop) {
        if (chmlink_inject(j->a, j->batch, j->length)) { j->calls++; j->packets += j->count; }
        else { j->errors++; j->last_error = GetLastError(); SwitchToThread(); }
    }
    return 0;
}

typedef struct receiver_args {
    SOCKET so; volatile LONG *stop; uint64_t packets, bytes;
} receiver_args;

static DWORD WINAPI receiver_thread(void *arg)
{
    receiver_args *r = arg;
    char buffer[2048];
    for (;;) {
        int n = recv(r->so, buffer, sizeof(buffer), 0);
        if (n >= 0) { r->packets++; r->bytes += (uint64_t)n; continue; }
        if (WSAGetLastError() == WSAETIMEDOUT) { if (*r->stop) break; continue; }
        if (WSAGetLastError() == WSAECONNRESET) continue;
        break;
    }
    return 0;
}

static SOCKET udp_socket(const uint8_t *bind_addr, unsigned bind_port, const uint8_t *connect_addr,
                         unsigned connect_port, int rcvbuf)
{
    SOCKET so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa;
    DWORD timeout = 100;
    REQUIRE(so != INVALID_SOCKET);
    if (rcvbuf) {
        REQUIRE(setsockopt(so, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf)) == 0);
        REQUIRE(setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0);
    }
    if (bind_addr) {
        memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_port = htons((u_short)bind_port);
        memcpy(&sa.sin_addr, bind_addr, 4);
        REQUIRE(bind(so, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    }
    if (connect_addr) {
        memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_port = htons((u_short)connect_port);
        memcpy(&sa.sin_addr, connect_addr, 4);
        REQUIRE(connect(so, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    }
    return so;
}

static void join(HANDLE *threads, int n)
{
    int i;
    for (i = 0; i < n; ++i) { WaitForSingleObject(threads[i], INFINITE); CloseHandle(threads[i]); }
}

/* ---- scenarios ---- */
typedef struct load_config {
    const char *name;
    int tx_senders, tx_payload;
    int rx_injectors, rx_payload; uint32_t rx_batch;
    DWORD stall_every_ms, stall_ms;
} load_config;

static const char *scenario_filter = "";

static int selected(const char *name)
{
    return scenario_filter[0] == '\0' || strstr(name, scenario_filter) != NULL;
}

static void run_load(chmlink_adapter *a, const chmlink_adapter_info *info, const load_config *cfg, double seconds)
{
    if (!selected(cfg->name)) return;
    volatile LONG stop_producers = 0, stop_consumers = 0;
    sender_args senders[8]; injector_args injectors[8]; consumer_args consumer; receiver_args receiver;
    HANDLE producers[16], consumers[2];
    int np = 0, nc = 0, i;
    chmlink_adapter_info before, after;
    cpu_sample c0, c1;
    LARGE_INTEGER t0, t1, t2;
    uint8_t *drain_buffer = malloc(CHMLINK_BATCH_MAX_SIZE), *batch = NULL;
    uint32_t batch_length = 0;
    uint64_t sent = 0, send_errors = 0, injected = 0, inject_calls = 0, inject_errors = 0;
    double wall_producers, wall_total;
    cpu_delta cpu;
    SOCKET tx_socket = INVALID_SOCKET, rx_socket = INVALID_SOCKET;
    DWORD inject_error = 0;
    REQUIRE(drain_buffer != NULL);
    memset(&consumer, 0, sizeof(consumer)); memset(&receiver, 0, sizeof(receiver));
    drain(a, drain_buffer);

    if (cfg->tx_senders) {
        tx_socket = udp_socket(NULL, 0, peer4, PORT_ECHO, 0);
        consumer.a = a; consumer.stop = &stop_consumers; consumer.limit = info->packet_limit;
        consumer.rx = drain_buffer;
        consumer.stall_every_ms = cfg->stall_every_ms; consumer.stall_ms = cfg->stall_ms;
    }
    if (cfg->rx_injectors) {
        rx_socket = udp_socket(local4, PORT_RX, NULL, 0, 32 * 1024 * 1024);
        receiver.so = rx_socket; receiver.stop = &stop_consumers;
        batch = make_batch(cfg->rx_batch, (size_t)cfg->rx_payload, info->packet_limit, &batch_length);
    }
    REQUIRE(chmlink_query(a, &before));
    cpu_take(&c0); QueryPerformanceCounter(&t0);
    if (cfg->tx_senders) {
        consumers[nc++] = CreateThread(NULL, 0, consumer_thread, &consumer, 0, NULL);
        for (i = 0; i < cfg->tx_senders; ++i) {
            memset(&senders[i], 0, sizeof(senders[i]));
            senders[i].so = tx_socket; senders[i].payload = cfg->tx_payload; senders[i].stop = &stop_producers;
            producers[np++] = CreateThread(NULL, 0, sender_thread, &senders[i], 0, NULL);
        }
    }
    if (cfg->rx_injectors) {
        consumers[nc++] = CreateThread(NULL, 0, receiver_thread, &receiver, 0, NULL);
        for (i = 0; i < cfg->rx_injectors; ++i) {
            memset(&injectors[i], 0, sizeof(injectors[i]));
            injectors[i].a = a; injectors[i].batch = batch; injectors[i].length = batch_length;
            injectors[i].count = cfg->rx_batch; injectors[i].stop = &stop_producers;
            producers[np++] = CreateThread(NULL, 0, injector_thread, &injectors[i], 0, NULL);
        }
    }
    Sleep((DWORD)(seconds * 1000));
    InterlockedExchange((volatile LONG *)&stop_producers, 1);
    join(producers, np);
    QueryPerformanceCounter(&t1);
    InterlockedExchange((volatile LONG *)&stop_consumers, 1);
    join(consumers, nc);
    QueryPerformanceCounter(&t2); cpu_take(&c1);
    REQUIRE(chmlink_query(a, &after));
    wall_producers = elapsed_s(t0, t1); wall_total = elapsed_s(t0, t2);
    cpu = cpu_diff(&c0, &c1, wall_total);

    for (i = 0; i < cfg->tx_senders; ++i) { sent += senders[i].sent; send_errors += senders[i].errors; }
    for (i = 0; i < cfg->rx_injectors; ++i) {
        injected += injectors[i].packets; inject_calls += injectors[i].calls;
        inject_errors += injectors[i].errors; if (injectors[i].last_error) inject_error = injectors[i].last_error;
    }
    {
        uint64_t delivered = consumer.packets;
        double pk = (double)(delivered + receiver.packets);
        printf("{\"event\":\"result\",\"scenario\":\"%s\",\"seconds\":%.2f", cfg->name, wall_producers);
        if (cfg->tx_senders) {
            printf(",\"tx_senders\":%d,\"tx_payload\":%d,\"tx_offered_pps\":%.0f,\"tx_delivered_pps\":%.0f,"
                   "\"tx_delivered_mbps\":%.1f,\"tx_loss_pct\":%.2f,\"tx_pkts_per_receive\":%.1f,"
                   "\"tx_receive_calls_per_s\":%.0f,\"tx_send_errors\":%llu,\"tx_consumer_error\":%lu,"
                   "\"drv_tx_packets\":%llu,\"drv_tx_queue_drops\":%llu,\"drv_tx_errors\":%llu,"
                   "\"tx_consumer_wait_pct\":%.1f,\"tx_consumer_receive_pct\":%.1f,\"tx_waits_per_s\":%.0f,"
                   "\"tx_max_wait_ms\":%.2f,\"tx_max_receive_ms\":%.2f",
                   cfg->tx_senders, cfg->tx_payload, sent / wall_producers, delivered / wall_producers,
                   consumer.bytes * 8.0 / wall_producers / 1e6,
                   sent ? 100.0 * (1.0 - (double)delivered / (double)sent) : 0.0,
                   consumer.calls ? (double)delivered / (double)consumer.calls : 0.0,
                   consumer.calls / wall_producers, (unsigned long long)send_errors, consumer.error,
                   (unsigned long long)(after.tx_packets - before.tx_packets),
                   (unsigned long long)(after.tx_queue_drops - before.tx_queue_drops),
                   (unsigned long long)(after.tx_errors - before.tx_errors),
                   100.0 * consumer.wait_s / wall_total, 100.0 * consumer.receive_s / wall_total,
                   consumer.waits / wall_total, consumer.max_wait_s * 1000.0, consumer.max_receive_s * 1000.0);
        }
        if (cfg->rx_injectors) {
            printf(",\"rx_injectors\":%d,\"rx_payload\":%d,\"rx_batch\":%u,\"rx_injected_pps\":%.0f,"
                   "\"rx_received_pps\":%.0f,\"rx_received_mbps\":%.1f,\"rx_loss_pct\":%.2f,"
                   "\"rx_inject_calls_per_s\":%.0f,\"rx_inject_errors\":%llu,\"rx_last_error\":%lu,"
                   "\"drv_rx_packets\":%llu,\"drv_rx_discards\":%llu",
                   cfg->rx_injectors, cfg->rx_payload, cfg->rx_batch, injected / wall_producers,
                   receiver.packets / wall_producers, (receiver.packets * (28.0 + cfg->rx_payload)) * 8.0 / wall_producers / 1e6,
                   injected ? 100.0 * (1.0 - (double)receiver.packets / (double)injected) : 0.0,
                   inject_calls / wall_producers, (unsigned long long)inject_errors, inject_error,
                   (unsigned long long)(after.rx_packets - before.rx_packets),
                   (unsigned long long)(after.rx_discards - before.rx_discards));
        }
        printf(",\"cpu_kernel_s\":%.2f,\"cpu_user_s\":%.2f,\"system_busy_cores\":%.2f,\"cpu_us_per_pkt\":%.2f}\n",
               cpu.proc_kernel_s, cpu.proc_user_s, cpu.busy_cores,
               pk > 0 ? (cpu.proc_kernel_s + cpu.proc_user_s) * 1e6 / pk : 0.0);
        fflush(stdout);
    }
    if (tx_socket != INVALID_SOCKET) closesocket(tx_socket);
    if (rx_socket != INVALID_SOCKET) closesocket(rx_socket);
    free(batch); free(drain_buffer);
}

static int compare_double(const void *x, const void *y)
{
    double a = *(const double *)x, b = *(const double *)y;
    return a < b ? -1 : a > b;
}

static void run_latency(chmlink_adapter *a, const chmlink_adapter_info *info, double seconds, int payload)
{
    volatile LONG stop = 0;
    consumer_args c;
    HANDLE thread;
    SOCKET so = udp_socket(NULL, 0, peer4, PORT_ECHO, 1 << 20);
    uint8_t out[1500], in[2048];
    double *rtt = malloc(MAX_RTT_SAMPLES * sizeof(double)), sum = 0;
    size_t n = 0, i;
    uint64_t lost = 0, sent = 0;
    LARGE_INTEGER start, now, p0, p1;
    int warmup = 300;
    DWORD timeout = 500;
    uint8_t *drain_buffer = malloc(CHMLINK_BATCH_MAX_SIZE);
    REQUIRE(rtt != NULL && drain_buffer != NULL);
    drain(a, drain_buffer);
    memset(&c, 0, sizeof(c));
    c.a = a; c.stop = &stop; c.echo = 1; c.limit = info->packet_limit;
    c.rx = drain_buffer; c.tx = malloc(CHMLINK_BATCH_MAX_SIZE); REQUIRE(c.tx != NULL);
    REQUIRE(setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0);
    memset(out, 0x33, sizeof(out));
    thread = CreateThread(NULL, 0, consumer_thread, &c, 0, NULL);
    QueryPerformanceCounter(&start);
    for (;;) {
        QueryPerformanceCounter(&now);
        if (n >= MAX_RTT_SAMPLES || (warmup == 0 && elapsed_s(start, now) >= seconds)) break;
        QueryPerformanceCounter(&p0);
        if (send(so, (char *)out, payload, 0) != payload) { lost++; continue; }
        sent++;
        if (recv(so, (char *)in, sizeof(in), 0) < 0) { lost++; continue; }
        QueryPerformanceCounter(&p1);
        if (warmup) { warmup--; if (!warmup) QueryPerformanceCounter(&start); continue; }
        rtt[n++] = elapsed_s(p0, p1) * 1e6;
    }
    InterlockedExchange((volatile LONG *)&stop, 1);
    WaitForSingleObject(thread, INFINITE); CloseHandle(thread);
    qsort(rtt, n, sizeof(double), compare_double);
    for (i = 0; i < n; ++i) sum += rtt[i];
    printf("{\"event\":\"result\",\"scenario\":\"latency\",\"payload\":%d,\"samples\":%llu,\"lost\":%llu,"
           "\"mean_us\":%.1f,\"p50_us\":%.1f,\"p90_us\":%.1f,\"p99_us\":%.1f,\"p999_us\":%.1f,\"max_us\":%.1f,"
           "\"echo_error\":%lu}\n",
           payload, (unsigned long long)n, (unsigned long long)lost, n ? sum / n : 0.0,
           n ? rtt[n / 2] : 0.0, n ? rtt[(size_t)(n * 0.90)] : 0.0, n ? rtt[(size_t)(n * 0.99)] : 0.0,
           n ? rtt[(size_t)(n * 0.999)] : 0.0, n ? rtt[n - 1] : 0.0, c.error);
    fflush(stdout);
    closesocket(so); free(rtt); free(c.tx); free(drain_buffer);
    (void)sent;
}

/* Plain UDP over 127.0.0.1: the cost of the Winsock stack without the driver. */
static void run_loopback(int payload, double seconds)
{
    volatile LONG stop_producers = 0, stop_consumers = 0;
    uint8_t loopback[4] = {127, 0, 0, 1};
    SOCKET rx = udp_socket(loopback, PORT_BASE, NULL, 0, 32 * 1024 * 1024);
    SOCKET tx = udp_socket(NULL, 0, loopback, PORT_BASE, 0);
    sender_args s; receiver_args r;
    HANDLE hs, hr;
    cpu_sample c0, c1; LARGE_INTEGER t0, t1;
    double wall; cpu_delta cpu;
    memset(&s, 0, sizeof(s)); memset(&r, 0, sizeof(r));
    s.so = tx; s.payload = payload; s.stop = &stop_producers;
    r.so = rx; r.stop = &stop_consumers;
    cpu_take(&c0); QueryPerformanceCounter(&t0);
    hr = CreateThread(NULL, 0, receiver_thread, &r, 0, NULL);
    hs = CreateThread(NULL, 0, sender_thread, &s, 0, NULL);
    Sleep((DWORD)(seconds * 1000));
    InterlockedExchange((volatile LONG *)&stop_producers, 1);
    join(&hs, 1);
    QueryPerformanceCounter(&t1);
    InterlockedExchange((volatile LONG *)&stop_consumers, 1);
    join(&hr, 1);
    cpu_take(&c1);
    wall = elapsed_s(t0, t1); cpu = cpu_diff(&c0, &c1, wall);
    printf("{\"event\":\"result\",\"scenario\":\"loopback\",\"payload\":%d,\"seconds\":%.2f,\"sent_pps\":%.0f,"
           "\"received_pps\":%.0f,\"loss_pct\":%.2f,\"cpu_kernel_s\":%.2f,\"cpu_user_s\":%.2f,"
           "\"system_busy_cores\":%.2f}\n",
           payload, wall, s.sent / wall, r.packets / wall,
           s.sent ? 100.0 * (1.0 - (double)r.packets / (double)s.sent) : 0.0,
           cpu.proc_kernel_s, cpu.proc_user_s, cpu.busy_cores);
    fflush(stdout);
    closesocket(rx); closesocket(tx);
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    chmlink_adapter *a;
    chmlink_adapter_info info;
    double seconds = 5.0;
    SYSTEM_INFO si;
    static const int payloads[] = {64, 512, 1400};
    static const uint32_t batches[] = {1, 8, 128};
    char name[64];
    load_config cfg;
    size_t i;

    if (argc < 4) {
        fprintf(stderr, "usage: chmlink-bench <ifIndex> <local-ipv4> <peer-ipv4> [seconds] [scenario]\n");
        return 2;
    }
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    REQUIRE(inet_pton(AF_INET, argv[2], local4) == 1 && inet_pton(AF_INET, argv[3], peer4) == 1);
    if (argc > 4) seconds = atof(argv[4]);
    if (argc > 5) scenario_filter = argv[5];
    if (seconds < 1.0) seconds = 1.0;
    QueryPerformanceFrequency(&qpf);
    GetSystemInfo(&si); cpu_count = si.dwNumberOfProcessors;
    a = chmlink_open((uint32_t)strtoul(argv[1], NULL, 10));
    REQUIRE(a != NULL);
    REQUIRE(chmlink_query(a, &info));
    REQUIRE(info.mode == CHMLINK_MODE_IP);
    printf("{\"event\":\"info\",\"cpus\":%lu,\"seconds\":%.1f,\"mtu\":%u,\"packet_limit\":%u,"
           "\"queue_capacity\":%u,\"capabilities\":%u}\n",
           cpu_count, seconds, info.mtu, info.packet_limit, info.queue_capacity, info.capabilities);
    fflush(stdout);

    if (selected("latency")) run_latency(a, &info, seconds, 64);
    for (i = 0; i < 3; ++i) {
        memset(&cfg, 0, sizeof(cfg)); sprintf(name, "tx_%dB", payloads[i]);
        cfg.name = name; cfg.tx_senders = 1; cfg.tx_payload = payloads[i];
        run_load(a, &info, &cfg, seconds);
    }
    memset(&cfg, 0, sizeof(cfg)); cfg.name = "tx_64B_4senders"; cfg.tx_senders = 4; cfg.tx_payload = 64;
    run_load(a, &info, &cfg, seconds);
    /* Consumer descheduled for 20 ms every 100 ms: how much burst the queue absorbs. */
    for (i = 0; i < 3; ++i) {
        memset(&cfg, 0, sizeof(cfg)); sprintf(name, "tx_%dB_stall20ms", payloads[i]);
        cfg.name = name; cfg.tx_senders = 1; cfg.tx_payload = payloads[i];
        cfg.stall_every_ms = 100; cfg.stall_ms = 20;
        run_load(a, &info, &cfg, seconds);
    }
    for (i = 0; i < 3; ++i) {
        memset(&cfg, 0, sizeof(cfg)); sprintf(name, "rx_%dB_batch32", payloads[i]);
        cfg.name = name; cfg.rx_injectors = 1; cfg.rx_payload = payloads[i]; cfg.rx_batch = 32;
        run_load(a, &info, &cfg, seconds);
    }
    for (i = 0; i < 3; ++i) {
        memset(&cfg, 0, sizeof(cfg)); sprintf(name, "rx_64B_batch%u", batches[i]);
        cfg.name = name; cfg.rx_injectors = 1; cfg.rx_payload = 64; cfg.rx_batch = batches[i];
        run_load(a, &info, &cfg, seconds);
    }
    memset(&cfg, 0, sizeof(cfg)); cfg.name = "rx_64B_batch32_2injectors"; cfg.rx_injectors = 2;
    cfg.rx_payload = 64; cfg.rx_batch = 32;
    run_load(a, &info, &cfg, seconds);
    memset(&cfg, 0, sizeof(cfg)); cfg.name = "duplex_64B"; cfg.tx_senders = 1; cfg.tx_payload = 64;
    cfg.rx_injectors = 1; cfg.rx_payload = 64; cfg.rx_batch = 32;
    run_load(a, &info, &cfg, seconds);
    for (i = 0; i < 3; ++i) if (selected("loopback")) run_loopback(payloads[i], seconds);

    chmlink_close(a);
    WSACleanup();
    printf("{\"event\":\"done\"}\n");
    return 0;
}
