#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L   /* posix_memalign, pthreads under -std=c99 */
#endif
#include "chmlink/ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
typedef HANDLE thread_t;
static DWORD WINAPI thread_entry(void *arg);
static void *(*thread_body)(void *);
static DWORD WINAPI thread_entry(void *arg) { thread_body(arg); return 0; }
static int start_thread(thread_t *t, void *(*body)(void *), void *arg)
{ thread_body = body; *t = CreateThread(NULL, 0, thread_entry, arg, 0, NULL); return *t != NULL; }
static void join_thread(thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#define ALIGNED_ALLOC(size) _aligned_malloc((size), 64)
#define ALIGNED_FREE(p) _aligned_free(p)
#else
#include <pthread.h>
typedef pthread_t thread_t;
static int start_thread(thread_t *t, void *(*body)(void *), void *arg) { return pthread_create(t, NULL, body, arg) == 0; }
static void join_thread(thread_t t) { pthread_join(t, NULL); }
/* posix_memalign, not aligned_alloc: the latter is C11 and undeclared under
 * -std=c99, where an implicit int return truncates the pointer. */
static void *aligned_alloc_64(size_t size) { void *p = NULL; return posix_memalign(&p, 64, size) == 0 ? p : NULL; }
#define ALIGNED_ALLOC(size) aligned_alloc_64(size)
#define ALIGNED_FREE(p) free(p)
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static uint8_t *ring_memory(uint32_t capacity)
{
    uint8_t *m = ALIGNED_ALLOC(CHMLINK_RING_HEADER_SIZE + capacity);
    CHECK(m != NULL);
    memset(m, 0, CHMLINK_RING_HEADER_SIZE + capacity);
    return m;
}

static void fill(uint8_t *p, uint32_t size, uint32_t seq)
{
    uint32_t i;
    for (i = 0; i < size; ++i) p[i] = (uint8_t)(seq * 31u + i);
}

static int check_fill(const uint8_t *p, uint32_t size, uint32_t seq)
{
    uint32_t i;
    for (i = 0; i < size; ++i) if (p[i] != (uint8_t)(seq * 31u + i)) return 0;
    return 1;
}

/* Produce and consume in lockstep-ish bursts across many wraps. */
static void round_trip(void)
{
    chmlink_ring ring;
    uint8_t *m = ring_memory(CHMLINK_RING_MIN_CAPACITY), *packet;
    uint32_t wcur = 0, rcur = 0, produced = 0, consumed = 0, offset, size, round;
    chmlink_offload meta, got;
    CHECK(chmlink_ring_attach(&ring, m, CHMLINK_RING_MIN_CAPACITY));
    chmlink_ring_reset(&ring);
    CHECK(!chmlink_ring_attach(&ring, m + 1, CHMLINK_RING_MIN_CAPACITY));
    CHECK(!chmlink_ring_capacity_valid(CHMLINK_RING_MIN_CAPACITY + 8));
    for (round = 0; round < 20000; ++round) {
        uint32_t burst = 1 + rnd() % 40, i;
        for (i = 0; i < burst; ++i) {
            uint32_t s = 1 + rnd() % (rnd() % 8 == 0 ? 9000 : 1500);
            chmlink_ring_status st;
            memset(&meta, 0, sizeof(meta));
            if (s > 1500) { meta.gso_type = CHMLINK_GSO_TCPV4; meta.gso_size = 1400; }
            meta.csum_start = (uint16_t)produced;
            st = chmlink_ring_reserve(&ring, &wcur, s, &meta, &packet);
            CHECK(st == CHMLINK_RING_OK || st == CHMLINK_RING_FULL);
            if (st == CHMLINK_RING_FULL) break;
            CHECK(packet >= ring.data && packet + s <= ring.data + ring.capacity);
            fill(packet, s, produced++);
        }
        (void)chmlink_ring_publish(&ring, wcur);
        burst = rnd() % 50;
        for (i = 0; i < burst; ++i) {
            chmlink_ring_status st = chmlink_ring_next(&ring, &rcur, chmlink_ring_load(&ring, CHMLINK_RING_TAIL_OFFSET),
                                                       1500, &offset, &size, &got);
            if (st == CHMLINK_RING_EMPTY) break;
            CHECK(st == CHMLINK_RING_OK);
            CHECK(got.csum_start == (uint16_t)consumed);
            CHECK(check_fill(ring.data + offset, size, consumed));
            consumed++;
        }
        chmlink_ring_release(&ring, rcur);
    }
    CHECK(produced > 100000 && consumed <= produced);
    ALIGNED_FREE(m);
}

/* Whatever the other side writes, the reader stays in bounds. */
static void hostile(void)
{
    chmlink_ring ring;
    uint32_t capacity = CHMLINK_RING_MIN_CAPACITY, iteration, corrupt = 0, ok = 0;
    uint8_t *m = ring_memory(capacity), *packet;
    CHECK(chmlink_ring_attach(&ring, m, capacity));
    for (iteration = 0; iteration < 200000; ++iteration) {
        uint32_t cur = (rnd() % (capacity / 16)) * 16, tail, offset, size, i;
        chmlink_offload got;
        chmlink_ring_status st;
        /* Random header values, often near the interesting edges. */
        tail = rnd() % 4 == 0 ? rnd() : (rnd() % (capacity / 16)) * 16;
        for (i = 0; i < 4; ++i) {
            uint32_t v = rnd() % 3 == 0 ? 0xffffffffu : rnd() % (capacity + 64);
            memcpy(m + CHMLINK_RING_HEADER_SIZE + ((cur + i * 16) % capacity), &v, 4);
        }
        if (rnd() % 2) memset(m + CHMLINK_RING_HEADER_SIZE + cur + 4, 0, 12);
        st = chmlink_ring_next(&ring, &cur, tail, 1500, &offset, &size, &got);
        if (st == CHMLINK_RING_OK) {
            ok++;
            CHECK(offset < capacity && size >= 1 && offset + size <= capacity && cur < capacity && cur % 16 == 0);
        } else if (st == CHMLINK_RING_CORRUPT) corrupt++;
        /* Producer against a random consumer head. */
        {
            uint32_t head = rnd() % 3 == 0 ? rnd() : (rnd() % (capacity / 16)) * 16, w = (rnd() % (capacity / 16)) * 16;
            uint32_t want = 1 + rnd() % 70000;
            memcpy(m + CHMLINK_RING_HEAD_OFFSET, &head, 4);
            st = chmlink_ring_reserve(&ring, &w, want, NULL, &packet);
            if (st == CHMLINK_RING_OK) CHECK(packet >= ring.data && packet + want <= ring.data + capacity && w < capacity);
            else if (want > CHMLINK_PACKET_MAX_SIZE) CHECK(st == CHMLINK_RING_INVALID || st == CHMLINK_RING_CORRUPT);
            else CHECK(st == CHMLINK_RING_FULL || st == CHMLINK_RING_CORRUPT);
        }
    }
    CHECK(ok > 0 && corrupt > 0);
    ALIGNED_FREE(m);
}

typedef struct shared { chmlink_ring ring; uint32_t total; } shared;

static void *producer(void *arg)
{
    shared *s = arg;
    uint32_t cursor = 0, seq = 0;
    uint8_t *packet;
    while (seq < s->total) {
        uint32_t size = 1 + (seq * 7919u) % 1500;
        chmlink_ring_status st = chmlink_ring_reserve(&s->ring, &cursor, size, NULL, &packet);
        CHECK(st != CHMLINK_RING_CORRUPT);
        if (st == CHMLINK_RING_FULL) { (void)chmlink_ring_publish(&s->ring, cursor); continue; }
        fill(packet, size, seq);
        if (++seq % 16 == 0) (void)chmlink_ring_publish(&s->ring, cursor);
    }
    (void)chmlink_ring_publish(&s->ring, cursor);
    return NULL;
}

static void threaded(void)
{
    shared s;
    thread_t t;
    uint32_t cursor = 0, seq = 0, offset, size;
    chmlink_offload got;
    uint8_t *m = ring_memory(CHMLINK_RING_MIN_CAPACITY);
    CHECK(chmlink_ring_attach(&s.ring, m, CHMLINK_RING_MIN_CAPACITY));
    chmlink_ring_reset(&s.ring);
    s.total = 2000000;
    CHECK(start_thread(&t, producer, &s));
    while (seq < s.total) {
        chmlink_ring_status st = chmlink_ring_next(&s.ring, &cursor, chmlink_ring_load(&s.ring, CHMLINK_RING_TAIL_OFFSET),
                                                   1500, &offset, &size, &got);
        CHECK(st != CHMLINK_RING_CORRUPT);
        if (st == CHMLINK_RING_EMPTY) continue;
        CHECK(size == 1 + (seq * 7919u) % 1500 && check_fill(s.ring.data + offset, size, seq));
        seq++;
        if (seq % 8 == 0) chmlink_ring_release(&s.ring, cursor);
    }
    chmlink_ring_release(&s.ring, cursor);
    join_thread(t);
    ALIGNED_FREE(m);
}

int main(void)
{
    round_trip();
    hostile();
    threaded();
    puts("shared ring ok");
    return 0;
}
