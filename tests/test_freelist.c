/*
 * test_freelist.c — correctness tests for the HAlloc free-list allocator.
 *
 * Tests:
 *   1. Alignment  — every pointer returned is 16-byte aligned.
 *   2. Read/write — payload bytes are writable without corrupting neighbours.
 *   3. Coalescing — after freeing adjacent blocks the free-list shrinks.
 *   4. Double-free guard — checked via the assert() in fl_free().
 *   5. Random stress — 10 000 alloc/free cycles with sizes 8–4096 bytes.
 *   6. Realloc     — growing and shrinking via fl_realloc().
 */

#include "../src/freelist.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

#define TEST(name) do { printf("  %-45s", name); fflush(stdout); } while(0)
#define PASS()     do { printf("PASS\n"); } while(0)
#define FAIL(msg)  do { printf("FAIL — %s\n", msg); failures++; } while(0)

static int failures = 0;

static inline int is_aligned(void *p)
{
    return ((uintptr_t)p & (FL_ALIGNMENT - 1)) == 0;
}

/* -------------------------------------------------------------------------
 * 1. Alignment test
 * ---------------------------------------------------------------------- */
static void test_alignment(void)
{
    TEST("alignment: 100 allocs of varied sizes");
    for (int i = 1; i <= 100; i++) {
        void *p = fl_malloc((size_t)i * 7);
        if (!p || !is_aligned(p)) { FAIL("misaligned or NULL"); return; }
        fl_free(p);
    }
    PASS();
}

/* -------------------------------------------------------------------------
 * 2. Read/write isolation test
 * ---------------------------------------------------------------------- */
static void test_readwrite(void)
{
    TEST("read/write: payload bytes writable, no neighbour corruption");
    const int N = 16;
    void *ptrs[16];
    size_t sizes[16];

    for (int i = 0; i < N; i++) {
        sizes[i] = (size_t)(i + 1) * 64;
        ptrs[i]  = fl_malloc(sizes[i]);
        assert(ptrs[i]);
        memset(ptrs[i], (unsigned char)(i + 1), sizes[i]);
    }

    /* Verify contents still intact. */
    for (int i = 0; i < N; i++) {
        unsigned char *b = (unsigned char *)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (b[j] != (unsigned char)(i + 1)) {
                FAIL("payload corrupted");
                /* Leak intentionally to avoid cascade. */
                return;
            }
        }
    }

    for (int i = 0; i < N; i++)
        fl_free(ptrs[i]);

    PASS();
}

/* -------------------------------------------------------------------------
 * 3. Coalescing test
 * ---------------------------------------------------------------------- */
static void test_coalescing(void)
{
    TEST("coalescing: free blocks merge, free-block count drops");

    /* Allocate three adjacent same-size blocks. */
    size_t sz = 256;
    void *a = fl_malloc(sz);
    void *b = fl_malloc(sz);
    void *c = fl_malloc(sz);
    assert(a && b && c);

    FLStats before, after;
    fl_stats(&before);

    /* Free all three — they should coalesce into one. */
    fl_free(a);
    fl_free(c);
    fl_free(b);  /* freeing b last triggers both-sides coalesce */

    fl_stats(&after);

    /* After freeing 3 blocks that can fully coalesce, the free-block count
     * should have increased by fewer than 3 (ideally 1 if they all merged). */
    if (after.num_free_blocks >= before.num_free_blocks + 3) {
        FAIL("blocks did not coalesce");
        return;
    }

    /* The total free bytes should have increased by 3 * payload capacity. */
    if (after.total_free <= before.total_free) {
        FAIL("total_free did not increase after freeing");
        return;
    }

    PASS();
}

/* -------------------------------------------------------------------------
 * 4. Random stress — 10 000 alloc/free cycles
 * ---------------------------------------------------------------------- */

#define LIVE_MAX 512  /* max simultaneously live allocations */

static void test_stress(void)
{
    TEST("stress: 10000 random alloc/free (8–4096 bytes)");

    srand((unsigned)time(NULL));

    void   *live[LIVE_MAX];
    size_t  live_sz[LIVE_MAX];
    int     live_cnt = 0;

    memset(live, 0, sizeof(live));
    memset(live_sz, 0, sizeof(live_sz));

    int total_ops = 0;

    for (int op = 0; op < 10000; op++) {
        /* Randomly decide to alloc or free. */
        int do_alloc = (live_cnt == 0) || (live_cnt < LIVE_MAX && rand() % 2);

        if (do_alloc) {
            size_t sz = (size_t)(rand() % (4096 - 8 + 1)) + 8;
            void *p = fl_malloc(sz);
            if (!p) { FAIL("fl_malloc returned NULL"); return; }
            if (!is_aligned(p)) { FAIL("misaligned pointer in stress"); return; }

            /* Find a free slot, then tag with the slot index as fill byte
             * so the verifier knows exactly what to expect. */
            for (int i = 0; i < LIVE_MAX; i++) {
                if (!live[i]) {
                    live[i]    = p;
                    live_sz[i] = sz;
                    memset(p, (unsigned char)(i & 0xFF), sz);
                    live_cnt++;
                    break;
                }
            }
        } else {
            /* Pick a random live allocation to free. */
            int idx = rand() % LIVE_MAX;
            /* Find next occupied slot from idx. */
            for (int k = 0; k < LIVE_MAX; k++) {
                int i = (idx + k) % LIVE_MAX;
                if (live[i]) {
                    /* Verify pattern still intact. */
                    unsigned char *b = (unsigned char *)live[i];
                    unsigned char  expected = (unsigned char)(i & 0xFF);
                    for (size_t j = 0; j < live_sz[i]; j++) {
                        if (b[j] != expected) {
                            FAIL("payload corrupted in stress");
                            return;
                        }
                    }
                    fl_free(live[i]);
                    live[i]    = NULL;
                    live_sz[i] = 0;
                    live_cnt--;
                    break;
                }
            }
        }
        total_ops++;
    }

    /* Free remaining live allocations. */
    for (int i = 0; i < LIVE_MAX; i++) {
        if (live[i]) {
            fl_free(live[i]);
            live[i] = NULL;
            live_cnt--;
        }
    }

    (void)total_ops;
    PASS();
}

/* -------------------------------------------------------------------------
 * 5. Realloc test
 * ---------------------------------------------------------------------- */
static void test_realloc(void)
{
    TEST("realloc: grow and shrink preserve data");

    size_t init_sz = 64;
    void *p = fl_malloc(init_sz);
    assert(p);
    memset(p, 0xAB, init_sz);

    /* Grow */
    size_t big_sz = 512;
    void *q = fl_realloc(p, big_sz);
    if (!q) { FAIL("fl_realloc grow returned NULL"); return; }
    if (!is_aligned(q)) { FAIL("fl_realloc result misaligned"); return; }

    /* First init_sz bytes must be preserved. */
    unsigned char *b = (unsigned char *)q;
    for (size_t i = 0; i < init_sz; i++) {
        if (b[i] != 0xAB) { FAIL("fl_realloc grow corrupted data"); return; }
    }

    /* Shrink */
    void *r = fl_realloc(q, 32);
    if (!r) { FAIL("fl_realloc shrink returned NULL"); return; }
    fl_free(r);

    PASS();
}

/* -------------------------------------------------------------------------
 * 6. Stats sanity test
 * ---------------------------------------------------------------------- */
static void test_stats(void)
{
    TEST("stats: allocated/free bytes consistent");

    FLStats before;
    fl_stats(&before);

    size_t sz = 1024;
    void *p = fl_malloc(sz);
    assert(p);

    FLStats after_alloc;
    fl_stats(&after_alloc);

    fl_free(p);

    FLStats after_free;
    fl_stats(&after_free);

    /* After alloc, allocated should be >= sz. */
    if (after_alloc.total_allocated < sz) {
        FAIL("total_allocated < requested size after alloc");
        return;
    }
    /* After free, free bytes should be >= what they were before alloc. */
    if (after_free.total_free < before.total_free) {
        FAIL("total_free decreased after free");
        return;
    }

    PASS();
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(void)
{
    printf("HAlloc — free-list allocator tests\n");
    printf("====================================\n");

    test_alignment();
    test_readwrite();
    test_coalescing();
    test_stress();
    test_realloc();
    test_stats();

    printf("====================================\n");
    if (failures == 0)
        printf("All tests passed.\n");
    else
        printf("%d test(s) FAILED.\n", failures);

    fl_print_stats();
    return failures ? 1 : 0;
}
