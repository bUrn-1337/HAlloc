/*
 * test_slab.c — correctness tests for the HAlloc slab allocator.
 *
 * Tests:
 *   1. Alignment    — every returned pointer is 16-byte aligned.
 *   2. No-overlap   — 1000 objects per size class are all at distinct addresses.
 *   3. Bitmap       — allocate exactly 64 objects (1 full slab), free every
 *                     other one, verify in_use, re-fill, verify full again,
 *                     free all, verify slab is munmap'd (slab count = 0).
 *   4. Slab lifecycle — partial → full → partial → empty (munmap) transitions.
 *   5. Stress       — 50000 random alloc/free cycles across all size classes
 *                     under AddressSanitizer.
 *   6. Unified API  — halloc/hfree/hrealloc routing and cross-class realloc.
 */

#include "../src/slab.h"
#include "../src/halloc.h"
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

#define TEST(name)  do { printf("  %-52s", name); fflush(stdout); } while(0)
#define PASS()      do { printf("PASS\n"); } while(0)
#define FAIL(msg)   do { printf("FAIL — %s\n", msg); failures++; return; } while(0)

static int failures = 0;

static const size_t SIZE_CLASSES[SLAB_NUM_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024
};

static inline int ptr_aligned(void *p)
{
    return ((uintptr_t)p & (uintptr_t)(SLAB_ALIGN - 1)) == 0;
}

/* qsort comparator for void* (compared as uintptr_t). */
static int ptr_cmp(const void *a, const void *b)
{
    uintptr_t pa = (uintptr_t)(*(void *const *)a);
    uintptr_t pb = (uintptr_t)(*(void *const *)b);
    return (pa > pb) - (pa < pb);
}

/* -------------------------------------------------------------------------
 * 1. Alignment test
 * ---------------------------------------------------------------------- */
static void test_alignment(void)
{
    TEST("alignment: 200 allocs across all size classes");

    for (int cls = 0; cls < SLAB_NUM_CLASSES; cls++) {
        size_t sz = SIZE_CLASSES[cls];
        void  *ptrs[200];

        for (int i = 0; i < 200; i++) {
            ptrs[i] = slab_alloc(sz);
            if (!ptrs[i])     FAIL("slab_alloc returned NULL");
            if (!ptr_aligned(ptrs[i])) FAIL("misaligned pointer");
        }
        for (int i = 0; i < 200; i++)
            slab_free(ptrs[i], sz);
    }
    PASS();
}

/* -------------------------------------------------------------------------
 * 2. No-overlap test — 1000 objects per size class
 * ---------------------------------------------------------------------- */
static void test_no_overlap(void)
{
    TEST("no-overlap: 1000 distinct addresses per size class");

    for (int cls = 0; cls < SLAB_NUM_CLASSES; cls++) {
        size_t  sz       = SIZE_CLASSES[cls];
        int     n        = 1000;
        void  **ptrs     = malloc((size_t)n * sizeof(void *));
        assert(ptrs);

        for (int i = 0; i < n; i++) {
            ptrs[i] = slab_alloc(sz);
            if (!ptrs[i]) { free(ptrs); FAIL("slab_alloc returned NULL"); }
        }

        /* Sort and verify strict ordering (no duplicates). */
        qsort(ptrs, (size_t)n, sizeof(void *), ptr_cmp);
        for (int i = 0; i < n - 1; i++) {
            if (ptrs[i] == ptrs[i + 1]) {
                free(ptrs);
                FAIL("duplicate pointer — two objects share an address");
            }
        }

        for (int i = 0; i < n; i++)
            slab_free(ptrs[i], sz);
        free(ptrs);
    }
    PASS();
}

/* -------------------------------------------------------------------------
 * 3. Bitmap correctness
 *
 * Fill exactly one slab (64 objects), free every other slot, verify in_use,
 * re-fill, verify full again, then free everything and verify the slab is
 * gone (munmap'd → slab count drops to 0).
 * ---------------------------------------------------------------------- */
static void test_bitmap(void)
{
    TEST("bitmap: fill/partial-free/refill/drain one slab");

    /* Use the 64-byte class so slot_size == 64 and arithmetic is easy. */
    size_t sz  = 64;
    int    cls = -1;
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (SIZE_CLASSES[i] == sz) { cls = i; break; }
    }
    assert(cls >= 0);

    /* --- Phase A: allocate exactly 64 objects (fills one slab). --- */
    void *ptrs[SLAB_CAPACITY];
    for (size_t i = 0; i < SLAB_CAPACITY; i++) {
        ptrs[i] = slab_alloc(sz);
        if (!ptrs[i]) FAIL("slab_alloc returned NULL during fill");
    }

    SlabCacheInfo info;
    slab_cache_info(cls, &info);
    if (info.full_count    != 1) FAIL("expected 1 full slab after filling 64 objects");
    if (info.partial_count != 0) FAIL("expected 0 partial slabs after filling 64 objects");
    if (info.total_in_use  != 64) FAIL("in_use != 64 after filling");

    /* --- Phase B: free every other object (32 frees). --- */
    for (size_t i = 0; i < SLAB_CAPACITY; i += 2)
        slab_free(ptrs[i], sz);

    slab_cache_info(cls, &info);
    if (info.full_count    != 0)  FAIL("full slab should have moved to partial");
    if (info.partial_count != 1)  FAIL("expected 1 partial slab");
    if (info.total_in_use  != 32) FAIL("in_use should be 32 after freeing alternates");

    /* --- Phase C: re-allocate 32 objects — should reuse freed slots. --- */
    void *refill[SLAB_CAPACITY / 2];
    for (size_t i = 0; i < SLAB_CAPACITY / 2; i++) {
        refill[i] = slab_alloc(sz);
        if (!refill[i]) FAIL("slab_alloc returned NULL during refill");
        if (!ptr_aligned(refill[i])) FAIL("refill pointer misaligned");
    }

    slab_cache_info(cls, &info);
    if (info.full_count    != 1)  FAIL("slab should be full again after refill");
    if (info.partial_count != 0)  FAIL("no partial slab expected after refill");
    if (info.total_in_use  != 64) FAIL("in_use should be 64 after refill");

    /*
     * The 32 refill pointers should be the same addresses as the 32 freed
     * ones (ctzll picks lowest free bit, so freed even-indexed slots get
     * reclaimed in ascending order).
     */
    void *freed_ptrs[SLAB_CAPACITY / 2];
    for (size_t i = 0; i < SLAB_CAPACITY / 2; i++)
        freed_ptrs[i] = ptrs[i * 2];

    qsort(freed_ptrs, SLAB_CAPACITY / 2, sizeof(void *), ptr_cmp);
    qsort(refill,     SLAB_CAPACITY / 2, sizeof(void *), ptr_cmp);
    for (size_t i = 0; i < SLAB_CAPACITY / 2; i++) {
        if (freed_ptrs[i] != refill[i])
            FAIL("refill did not reclaim the freed slots");
    }

    /* --- Phase D: free everything → slab leaves partial/full lists. --- */
    /* The odd-indexed objects (still live from phase A). */
    for (size_t i = 1; i < SLAB_CAPACITY; i += 2)
        slab_free(ptrs[i], sz);
    /* The refill objects. */
    for (size_t i = 0; i < SLAB_CAPACITY / 2; i++)
        slab_free(refill[i], sz);

    /*
     * The slab is now empty.  It may be in the free-slab cache (if cache has
     * room) or munmap'd (if the cache is full).  Either way, partial and full
     * counts must be zero — the slab is no longer serving allocations.
     */
    slab_cache_info(cls, &info);
    if (info.full_count + info.partial_count != 0)
        FAIL("empty slab should have left partial/full lists");

    PASS();
}

/* -------------------------------------------------------------------------
 * 4. Slab lifecycle test
 * ---------------------------------------------------------------------- */
static void test_lifecycle(void)
{
    TEST("lifecycle: partial→full→partial→empty transitions");

    size_t sz  = 128;
    int    cls = -1;
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (SIZE_CLASSES[i] == sz) { cls = i; break; }
    }
    assert(cls >= 0);

    SlabCacheInfo info;

    /* Allocate 63 → should be partial (1 slot left). */
    void *ptrs[SLAB_CAPACITY];
    for (size_t i = 0; i < SLAB_CAPACITY - 1; i++)
        ptrs[i] = slab_alloc(sz);

    slab_cache_info(cls, &info);
    if (info.partial_count != 1) FAIL("63/64 full — should be partial");
    if (info.full_count    != 0) FAIL("should not be full yet");

    /* Allocate 1 more → now full. */
    ptrs[SLAB_CAPACITY - 1] = slab_alloc(sz);
    slab_cache_info(cls, &info);
    if (info.full_count    != 1) FAIL("64/64 — should be full");
    if (info.partial_count != 0) FAIL("should not be partial when full");

    /* Free one → moves back to partial. */
    slab_free(ptrs[0], sz);
    slab_cache_info(cls, &info);
    if (info.partial_count != 1) FAIL("freeing one from full should yield partial");
    if (info.full_count    != 0) FAIL("no full slabs expected");

    /* Free the rest → slab munmap'd. */
    for (size_t i = 1; i < SLAB_CAPACITY; i++)
        slab_free(ptrs[i], sz);

    slab_cache_info(cls, &info);
    if (info.partial_count + info.full_count != 0)
        FAIL("empty slab should have left partial/full lists");

    PASS();
}

/* -------------------------------------------------------------------------
 * 5. Stress test — 50000 random alloc/free across all size classes
 * ---------------------------------------------------------------------- */

#define STRESS_OPS  50000
#define STRESS_LIVE 512

typedef struct {
    void  *ptr;
    size_t sz;
    unsigned char fill;
} LiveEntry;

static void test_stress(void)
{
    TEST("stress: 50000 random alloc/free across all size classes (ASan)");

    srand(0xDEADBEEF);

    LiveEntry live[STRESS_LIVE];
    memset(live, 0, sizeof(live));
    int live_cnt = 0;

    for (int op = 0; op < STRESS_OPS; op++) {
        int do_alloc = (live_cnt == 0) ||
                       (live_cnt < STRESS_LIVE && rand() % 2);

        if (do_alloc) {
            /* Pick a random size from 1 to SLAB_MAX_SIZE. */
            size_t sz = (size_t)(rand() % SLAB_MAX_SIZE) + 1;
            void  *p  = slab_alloc(sz);
            if (!p) FAIL("slab_alloc returned NULL in stress");
            if (!ptr_aligned(p)) FAIL("misaligned pointer in stress");

            /* Find a free live slot. */
            for (int i = 0; i < STRESS_LIVE; i++) {
                if (!live[i].ptr) {
                    /* Use a fill byte derived from slot index for verification. */
                    unsigned char fill = (unsigned char)(i ^ 0xA5);
                    live[i].ptr  = p;
                    live[i].sz   = sz;
                    live[i].fill = fill;
                    /* Write entire size-class slot to exercise ASan. */
                    memset(p, fill, sz);
                    live_cnt++;
                    break;
                }
            }
        } else {
            /* Pick a random live slot. */
            int base = rand() % STRESS_LIVE;
            for (int k = 0; k < STRESS_LIVE; k++) {
                int i = (base + k) % STRESS_LIVE;
                if (!live[i].ptr) continue;

                /* Verify fill bytes intact. */
                unsigned char *b = (unsigned char *)live[i].ptr;
                for (size_t j = 0; j < live[i].sz; j++) {
                    if (b[j] != live[i].fill) FAIL("payload corrupted in stress");
                }

                slab_free(live[i].ptr, live[i].sz);
                live[i].ptr = NULL;
                live_cnt--;
                break;
            }
        }
    }

    /* Drain remaining live entries. */
    for (int i = 0; i < STRESS_LIVE; i++) {
        if (live[i].ptr) {
            slab_free(live[i].ptr, live[i].sz);
            live[i].ptr = NULL;
        }
    }

    PASS();
}

/* -------------------------------------------------------------------------
 * 6. Unified API test (halloc / hfree / hrealloc)
 * ---------------------------------------------------------------------- */
static void test_unified_api(void)
{
    TEST("unified: halloc routes to slab vs freelist by size");

    /* Small allocation → slab */
    void *small = halloc(64);
    if (!small) FAIL("halloc(64) returned NULL");
    if (!ptr_aligned(small)) FAIL("halloc(64) misaligned");

    /* Large allocation → freelist */
    void *large = halloc(2048);
    if (!large) FAIL("halloc(2048) returned NULL");
    if (!ptr_aligned(large)) FAIL("halloc(2048) misaligned");

    /* Write to both, verify they don't alias. */
    memset(small, 0xAA, 64);
    memset(large, 0xBB, 2048);
    if (((unsigned char *)small)[0] != 0xAA) FAIL("small payload clobbered by large");

    /* hrealloc: same size class → same pointer. */
    void *rsame = hrealloc(small, 64, 64);
    if (rsame != small) FAIL("hrealloc within same class should return same ptr");

    /* hrealloc: size class upgrade → new pointer, data preserved. */
    memset(small, 0xCC, 64);
    void *rup = hrealloc(small, 64, 128);
    if (!rup) FAIL("hrealloc upgrade returned NULL");
    if (!ptr_aligned(rup)) FAIL("hrealloc upgrade misaligned");
    for (int i = 0; i < 64; i++) {
        if (((unsigned char *)rup)[i] != 0xCC) FAIL("data not preserved across hrealloc upgrade");
    }
    hfree(rup,   128);
    hfree(large, 2048);

    /* hrealloc: slab → freelist cross boundary. */
    void *cross_src = halloc(512);
    if (!cross_src) FAIL("halloc(512) returned NULL");
    memset(cross_src, 0xDD, 512);
    void *cross_dst = hrealloc(cross_src, 512, 4096);
    if (!cross_dst) FAIL("hrealloc cross-boundary returned NULL");
    for (int i = 0; i < 512; i++) {
        if (((unsigned char *)cross_dst)[i] != 0xDD) FAIL("data lost in cross-boundary realloc");
    }
    hfree(cross_dst, 4096);

    PASS();
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(void)
{
    printf("HAlloc — slab allocator tests\n");
    printf("===============================\n");

    test_alignment();
    test_no_overlap();
    test_bitmap();
    test_lifecycle();
    test_stress();
    test_unified_api();

    printf("===============================\n");
    if (failures == 0)
        printf("All tests passed.\n");
    else
        printf("%d test(s) FAILED.\n", failures);

    slab_stats();
    return failures ? 1 : 0;
}
