/*
 * slab.c — fixed-size object slab allocator for HAlloc.
 *
 * Design decisions:
 *
 * 1. 64 objects per slab, one uint64_t bitmap.
 *    A single 64-bit word tracks all slots.  Allocation is O(1) via
 *    __builtin_ctzll(~bitmap): count trailing 1-bits of the inverted bitmap
 *    = index of the first 0-bit = first free slot.  No scanning required.
 *
 * 2. Free-slab cache (up to SLAB_FREE_CACHE_MAX empty slabs per size class).
 *    When a slab empties we park it in a per-cache free list rather than
 *    munmap'ing immediately.  slab_alloc pulls from this list before calling
 *    mmap.  The trade-off:
 *
 *      Eager munmap    — RSS stays minimal; every alloc+free cycle that
 *                        drains a slab pays two syscalls (~2-5 µs each).
 *                        Correct and simple, but kills throughput for any
 *                        workload with alloc/free churn on a single object.
 *
 *      Cached empties  — At most (SLAB_FREE_CACHE_MAX × num_classes) extra
 *                        slabs sit idle, trading a tiny RSS increase for
 *                        O(1) re-use: no syscall, just a pointer swap.
 *                        Amortises mmap cost across many alloc/free cycles.
 *
 *    We evict (munmap) only when the free-slab list already holds
 *    SLAB_FREE_CACHE_MAX entries, so worst-case idle memory is bounded.
 *
 * 3. partial / full split.
 *    Keeping full slabs in a separate list means slab_alloc never has to skip
 *    over them when scanning for a slot.  It always picks from partial (O(1)
 *    given the bitmap trick), or creates a new slab if partial is empty.
 *
 * 4. 16-byte alignment via slot_size rounding.
 *    Every slot address is (memory_base + slot_index * slot_size).
 *    memory_base is placed at a 16-byte-aligned offset within the mmap'd
 *    region, and slot_size is always rounded up to 16 bytes, so every
 *    pointer we return is 16-byte aligned — matching the free-list guarantee.
 *
 * 5. Searching for the owning slab on free.
 *    slab_free() searches the full then partial chain for the slab that owns
 *    ptr by range-checking [memory, memory + 64*slot_size).  This is O(slabs)
 *    in the cache.  A hash-map or radix-tree lookup would be O(1) but is
 *    unnecessary for an educational allocator.
 */

#include "slab.h"

#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Size classes
 * ---------------------------------------------------------------------- */

static const size_t SIZE_CLASSES[SLAB_NUM_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024
};

/*
 * Internal per-slot storage size: rounded up to SLAB_ALIGN (16) so that
 * every slot address is 16-byte aligned.
 *
 * For the 8-byte class this wastes 8 bytes per object (50% internal frag),
 * a deliberate trade-off for alignment uniformity.
 */
static inline size_t slot_size(size_t obj_size)
{
    return (obj_size + (size_t)(SLAB_ALIGN - 1)) & ~(size_t)(SLAB_ALIGN - 1);
}

/*
 * Byte offset from the start of the mmap'd region to the first object slot.
 * Layout: [Slab header (sizeof(Slab))] [bitmap (8 B)] [padding → 16-B align]
 *
 * sizeof(Slab) = 48 bytes (6 × 8-byte fields on LP64).
 * sizeof(uint64_t) = 8 bytes.
 * 48 + 8 = 56; round up to 16 → 64.
 */
static inline size_t obj_start_offset(void)
{
    size_t raw = sizeof(Slab) + sizeof(uint64_t);
    return (raw + (size_t)(SLAB_ALIGN - 1)) & ~(size_t)(SLAB_ALIGN - 1);
}

/* Total bytes to mmap for one slab of a given class. */
static inline size_t slab_region_size(size_t obj_size)
{
    return obj_start_offset() + slot_size(obj_size) * SLAB_CAPACITY;
}

/* -------------------------------------------------------------------------
 * Cache state (one entry per size class)
 * ---------------------------------------------------------------------- */

static SlabCache caches[SLAB_NUM_CLASSES];
static int       caches_ready = 0;

static void init_caches(void)
{
    if (caches_ready) return;
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        caches[i].obj_size     = SIZE_CLASSES[i];
        caches[i].partial      = NULL;
        caches[i].full         = NULL;
        caches[i].free_slabs   = NULL;
        caches[i].free_count   = 0;
        caches[i].total_allocs = 0;
        caches[i].total_frees  = 0;
    }
    caches_ready = 1;
}

/* -------------------------------------------------------------------------
 * Size-class lookup
 * ---------------------------------------------------------------------- */

/* Returns the index of the smallest class >= size, or -1. */
static int find_class_idx(size_t size)
{
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (SIZE_CLASSES[i] >= size)
            return i;
    }
    return -1;
}

size_t slab_size_class(size_t size)
{
    int i = find_class_idx(size);
    return (i >= 0) ? SIZE_CLASSES[i] : 0;
}

/* -------------------------------------------------------------------------
 * Slab lifecycle
 * ---------------------------------------------------------------------- */

static Slab *slab_new(size_t obj_size)
{
    size_t total = slab_region_size(obj_size);
    void  *base  = mmap(NULL, total,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1, 0);
    if (base == MAP_FAILED)
        return NULL;

    Slab *s    = (Slab *)base;
    s->obj_size = obj_size;
    s->capacity = SLAB_CAPACITY;
    s->in_use   = 0;
    s->bitmap   = (uint64_t *)((char *)base + sizeof(Slab));
    *s->bitmap  = 0ULL;                        /* all 64 slots free */
    s->memory   = (char *)base + obj_start_offset();
    s->next     = NULL;

    return s;
}

static void slab_destroy(Slab *s)
{
    munmap(s, slab_region_size(s->obj_size));
}

/* -------------------------------------------------------------------------
 * slab_alloc
 * ---------------------------------------------------------------------- */

void *slab_alloc(size_t size)
{
    if (size == 0)
        return NULL;

    init_caches();

    int idx = find_class_idx(size);
    assert(idx >= 0 && "slab_alloc: size exceeds SLAB_MAX_SIZE");

    SlabCache *cache = &caches[idx];

    /* Get an existing partial slab, recycle a cached empty one, or mmap. */
    Slab *s = cache->partial;
    if (!s) {
        if (cache->free_slabs) {
            /* Reclaim from the free-slab cache — no syscall needed. */
            s = cache->free_slabs;
            cache->free_slabs = s->next;
            cache->free_count--;
            s->next = NULL;
        } else {
            s = slab_new(cache->obj_size);
            if (!s)
                return NULL;
        }
        cache->partial = s;
    }

    /*
     * Find the first free slot.
     * ~(*s->bitmap) flips all bits: zeros (free slots) become ones.
     * __builtin_ctzll counts trailing zeros of that result = index of the
     * lowest free slot.  This is defined because the slab is partial
     * (bitmap != 0xFFFF...F), so ~bitmap has at least one 1-bit.
     */
    int slot = __builtin_ctzll(~(*s->bitmap));

    /* Mark the slot as allocated and update counters. */
    *s->bitmap |= (1ULL << slot);
    s->in_use++;
    cache->total_allocs++;

    void *ptr = (char *)s->memory + (size_t)slot * slot_size(s->obj_size);

    /* If the slab just became full, move it from partial → full. */
    if (s->in_use == SLAB_CAPACITY) {
        cache->partial = s->next;
        s->next        = cache->full;
        cache->full    = s;
    }

    return ptr;
}

/* -------------------------------------------------------------------------
 * slab_free
 * ---------------------------------------------------------------------- */

void slab_free(void *ptr, size_t size)
{
    if (!ptr)
        return;

    init_caches();

    int idx = find_class_idx(size);
    assert(idx >= 0 && "slab_free: size exceeds SLAB_MAX_SIZE");

    SlabCache *cache  = &caches[idx];
    size_t     ssz    = slot_size(cache->obj_size);
    size_t     range  = ssz * SLAB_CAPACITY;

    /*
     * Locate the slab that owns ptr.  We search full slabs first (a freed
     * object was likely in a full slab if the caller just finished using it),
     * then partial slabs.  The search is O(total_slabs_in_cache).
     *
     * `prev` always points to the link that currently points to `s`, so
     * *prev = s->next unlinks s from whichever list it lives in.
     */
    Slab **prev    = &cache->full;
    Slab  *s;
    int    was_full = 1;

    for (s = cache->full; s; prev = &s->next, s = s->next) {
        if ((char *)ptr >= (char *)s->memory &&
            (char *)ptr <  (char *)s->memory + range)
            goto found;
    }

    was_full = 0;
    prev     = &cache->partial;
    for (s = cache->partial; s; prev = &s->next, s = s->next) {
        if ((char *)ptr >= (char *)s->memory &&
            (char *)ptr <  (char *)s->memory + range)
            goto found;
    }

    assert(0 && "slab_free: pointer not found — invalid or double free");
    return;

found:;
    /* Compute the slot index from the pointer offset within s->memory. */
    size_t slot = ((char *)ptr - (char *)s->memory) / ssz;
    assert(slot < SLAB_CAPACITY);

    /* Guard against double free: the bit must currently be set. */
    assert((*s->bitmap >> slot) & 1ULL && "slab_free: double free detected");

    /* Clear the bit and update counters. */
    *s->bitmap &= ~(1ULL << slot);
    s->in_use--;
    cache->total_frees++;

    if (s->in_use == 0) {
        /*
         * Slab is now empty.  Park it in the free-slab cache so the next
         * slab_alloc can reclaim it without a syscall.  If the cache is
         * already full (free_count == SLAB_FREE_CACHE_MAX), munmap instead
         * to keep idle RSS bounded.
         *
         * Trade-off recap:
         *   Eager munmap   → minimal RSS, mmap syscall on every tight cycle.
         *   Cached empties → small RSS headroom, O(1) re-use, no syscall.
         */
        *prev = s->next;
        if (cache->free_count < SLAB_FREE_CACHE_MAX) {
            s->next           = cache->free_slabs;
            cache->free_slabs = s;
            cache->free_count++;
        } else {
            slab_destroy(s);
        }
        return;
    }

    if (was_full) {
        /*
         * Slab went from full to partial.
         * Unlink from the full list, prepend to the partial list.
         */
        *prev          = s->next;
        s->next        = cache->partial;
        cache->partial = s;
    }
    /*
     * If already partial: the bitmap was updated in place.
     * No list relinking needed.
     */
}

/* -------------------------------------------------------------------------
 * Introspection helpers
 * ---------------------------------------------------------------------- */

void slab_cache_info(int cls, SlabCacheInfo *out)
{
    assert(cls >= 0 && cls < SLAB_NUM_CLASSES);

    init_caches();
    SlabCache *c = &caches[cls];

    out->obj_size        = c->obj_size;
    out->partial_count   = 0;
    out->full_count      = 0;
    out->free_slab_count = c->free_count;
    out->total_in_use    = 0;
    out->total_capacity  = 0;
    out->total_allocs    = c->total_allocs;
    out->total_frees     = c->total_frees;

    for (Slab *s = c->partial; s; s = s->next) {
        out->partial_count++;
        out->total_in_use   += s->in_use;
        out->total_capacity += s->capacity;
    }
    for (Slab *s = c->full; s; s = s->next) {
        out->full_count++;
        out->total_in_use   += s->in_use;
        out->total_capacity += s->capacity;
    }
}

void slab_stats(void)
{
    if (!caches_ready) {
        printf("=== HAlloc slab: not yet used ===\n");
        return;
    }
    printf("=== HAlloc slab stats ===\n");
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        SlabCacheInfo info;
        slab_cache_info(i, &info);

        double util = info.total_capacity
                    ? 100.0 * (double)info.total_in_use / (double)info.total_capacity
                    : 0.0;

        printf("  [%4zu B]: partial=%-2zu full=%-2zu free=%-2zu  "
               "%zu/%zu objs  %5.1f%% used  "
               "(allocs=%-6zu frees=%zu)\n",
               info.obj_size,
               info.partial_count, info.full_count, info.free_slab_count,
               info.total_in_use, info.total_capacity, util,
               info.total_allocs, info.total_frees);
    }
}
