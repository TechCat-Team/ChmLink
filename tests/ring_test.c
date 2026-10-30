#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ring.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s (seed=%u step=%lu)\n", __FILE__, __LINE__, #expr, seed, step); \
        return 1; \
    } \
} while (0)

/* Reference FIFO of packet descriptors; contents are derived from a tag. */
typedef struct packet { uint32_t length, tag; } packet;
static packet model[70000];
static size_t model_head, model_count;

static uint32_t rng_state;
static unsigned int seed;
static unsigned long step;

static uint32_t next(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

static uint8_t byte_of(uint32_t tag, uint32_t i)
{
    return (uint8_t)(tag * 31u + i * 7u + (i >> 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t pick_length(uint32_t max)
{
    switch (next() % 4) {
    case 0: return next() % 64;
    case 1: return 60 + next() % 40;
    case 2: return 1400 + next() % 123;
    default: return next() % (max + 1);
    }
}

/* write_pct biases the mix toward sends so that every configuration spends
 * time full, exercising both byte and packet-count exhaustion. */
static int run(unsigned int s, uint32_t size, uint32_t max_packets, unsigned long steps,
               uint32_t write_pct, uint32_t clear_per_mille)
{
    uint8_t *storage = malloc(size + 64);
    chmlink_ring ring;
    uint32_t next_tag = 1, i, length;
    unsigned long accepted = 0, rejected = 0, wraps_seen = 0;
    seed = s; rng_state = s * 2654435761u + 1; step = 0;
    model_head = model_count = 0;
    CHECK(storage != NULL);
    memset(storage + size, 0xee, 64);           /* guard bytes */
    CHECK(chmlink_ring_init(&ring, storage, size, max_packets));
    for (step = 0; step < steps; ++step) {
        uint32_t op = next() % 1000;
        if (op < write_pct * 10) {
            /* A send of one to four packets: all of them or none. */
            chmlink_ring_txn txn;
            uint32_t n = 1 + next() % 4, ok = 1, tags[4], lengths[4];
            int commit = (next() % 10) != 0;     /* 10% aborted (failed send) */
            chmlink_ring_begin(&ring, &txn);
            for (i = 0; i < n; ++i) {
                uint8_t *p;
                uint32_t j;
                lengths[i] = pick_length(1522);
                tags[i] = next_tag++;
                p = chmlink_ring_reserve(&ring, &txn, lengths[i]);
                if (p == NULL) { ok = 0; break; }
                CHECK(p >= storage && p + lengths[i] <= storage + size);
                for (j = 0; j < lengths[i]; ++j) p[j] = byte_of(tags[i], j);
            }
            if (ok && commit) {
                chmlink_ring_commit(&ring, &txn);
                for (i = 0; i < n; ++i) {
                    model[(model_head + model_count) % 70000].length = lengths[i];
                    model[(model_head + model_count) % 70000].tag = tags[i];
                    model_count++;
                }
                accepted += n;
            } else if (!ok) {
                rejected++;
                /* With nothing published or staged before it, a packet that
                 * fits the ring at all must be accepted. */
                CHECK(ring.count != 0 || i > 0 || 4 + ((lengths[i] + 3) & ~3u) > size);
            }
        } else if (op < 1000 - clear_per_mille) {
            uint32_t pops = 1 + next() % 8;
            while (pops--) {
                const uint8_t *p = chmlink_ring_front(&ring, &length);
                packet *m;
                if (model_count == 0) { CHECK(p == NULL); break; }
                CHECK(p != NULL);
                m = &model[model_head];
                CHECK(length == m->length);
                for (i = 0; i < length; ++i) CHECK(p[i] == byte_of(m->tag, i));
                /* Record framing matches the batch layout, zero padded. */
                CHECK(rd32(p - 4) == length);
                for (i = length; i < ((length + 3) & ~3u); ++i) CHECK(p[i] == 0);
                if (p - 4 < storage + ring.head) wraps_seen++;
                chmlink_ring_pop(&ring);
                model_head = (model_head + 1) % 70000; model_count--;
            }
        } else {
            CHECK(chmlink_ring_clear(&ring) == model_count);
            model_head = model_count = 0;
        }
        CHECK(ring.count == model_count && ring.count <= max_packets);
        CHECK(ring.used <= size && ring.head < size && ring.tail < size && (ring.head & 3) == 0 && (ring.tail & 3) == 0);
        CHECK(model_count != 0 || (ring.used == 0 && ring.head == 0 && ring.tail == 0));
        for (i = 0; i < 64; ++i) CHECK(storage[size + i] == 0xee);
    }
    CHECK(accepted > 1000 && rejected > 10);
    free(storage);
    (void)wraps_seen;
    return 0;
}

int main(void)
{
    uint8_t small[64];
    chmlink_ring ring;
    chmlink_ring_txn txn;
    uint32_t length;
    unsigned int s;

    seed = 0; step = 0;
    /* Configuration checks. */
    CHECK(!chmlink_ring_init(&ring, small, 6, 4));
    CHECK(!chmlink_ring_init(&ring, small, 10, 4));
    CHECK(!chmlink_ring_init(&ring, small, 64, 0));
    CHECK(chmlink_ring_init(&ring, small, 64, 4));

    /* Packet-count limit, then exact byte fill. */
    chmlink_ring_begin(&ring, &txn);
    CHECK(chmlink_ring_reserve(&ring, &txn, 0) != NULL);
    CHECK(chmlink_ring_reserve(&ring, &txn, 1) != NULL);
    CHECK(chmlink_ring_reserve(&ring, &txn, 2) != NULL);
    CHECK(chmlink_ring_reserve(&ring, &txn, 3) != NULL);
    CHECK(chmlink_ring_reserve(&ring, &txn, 0) == NULL);     /* max_packets */
    chmlink_ring_commit(&ring, &txn);
    CHECK(ring.count == 4 && ring.used == 4 + 8 + 8 + 8);
    CHECK(chmlink_ring_clear(&ring) == 4 && ring.used == 0);
    chmlink_ring_begin(&ring, &txn);
    CHECK(chmlink_ring_reserve(&ring, &txn, 60) != NULL);     /* 64 bytes: full */
    CHECK(chmlink_ring_reserve(&ring, &txn, 0) == NULL);
    chmlink_ring_commit(&ring, &txn);
    CHECK(chmlink_ring_front(&ring, &length) != NULL && length == 60);
    chmlink_ring_pop(&ring);
    CHECK(chmlink_ring_front(&ring, &length) == NULL && ring.used == 0);

    /* Wrap: 24 + 24 bytes, pop first, then a 28-byte record must wrap. */
    chmlink_ring_begin(&ring, &txn);
    CHECK(chmlink_ring_reserve(&ring, &txn, 20) != NULL);
    CHECK(chmlink_ring_reserve(&ring, &txn, 20) != NULL);
    chmlink_ring_commit(&ring, &txn);
    CHECK(chmlink_ring_front(&ring, &length) != NULL); chmlink_ring_pop(&ring);
    chmlink_ring_begin(&ring, &txn);
    CHECK(chmlink_ring_reserve(&ring, &txn, 20) == small + 4);  /* wrapped to 0 */
    chmlink_ring_commit(&ring, &txn);
    CHECK(ring.used == 24 + 16 + 24 && ring.tail == 24);
    CHECK(chmlink_ring_front(&ring, &length) == small + 24 + 4); chmlink_ring_pop(&ring);
    CHECK(chmlink_ring_front(&ring, &length) == small + 4 && ring.head == 0);
    chmlink_ring_pop(&ring);
    CHECK(ring.used == 0 && ring.count == 0);

    /* Aborted transaction leaves no trace. */
    chmlink_ring_begin(&ring, &txn);
    CHECK(chmlink_ring_reserve(&ring, &txn, 8) != NULL);
    CHECK(chmlink_ring_front(&ring, &length) == NULL && ring.count == 0);

    /* Randomized differential runs against the reference FIFO. */
    for (s = 1; s <= 40; ++s) {
        if (run(s, 4096, 64, 20000, 55, 20)) return 1;          /* byte-bound */
        if (run(s + 1000, 65536, 1000, 20000, 75, 5)) return 1;
        if (run(s + 2000, 65536, 16, 20000, 60, 20)) return 1;  /* count-bound */
    }
    if (run(7, 4u << 20, 16384, 300000, 80, 0)) return 1;       /* driver geometry */
    puts("PASS ring");
    return 0;
}
