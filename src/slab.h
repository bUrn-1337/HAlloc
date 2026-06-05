#ifndef SLAB_H
#define SLAB_H

#include <stddef.h>
#include <stdint.h>

/*
 * Slab allocator — Phase 2 of HAlloc.
 *
 * Handles fixed-size allocations up to SLAB_MAX_SIZE bytes.
 * Larger requests fall through to the free-list allocator via the
 * unified halloc/hfree API in halloc.h.
 *
 * Size classes: 8, 16, 32, 64, 128, 256, 512, 1024 bytes.
 *
 * Each slab:
 *   - Holds exactly SLAB_CAPACITY (64) objects.
 *   - Uses a single uint64_t bitmap where bit i = 1 means object i is allocated.
 *   - Allocation is O(1): __builtin_ctzll(~bitmap) gives the first free slot.
 *
 * Memory layout of one slab's mmap'd region:
 *
 *   [ Slab header (48 B) | bitmap (8 B) | padding to 16-B boundary | 64 × slot_size ]
 *
 * slot_size = (obj_size + 15) & ~15   (round up to 16 bytes so every returned
 *             pointer is 16-byte aligned regardless of the size class).
 *
 * For the 8-byte class, slot_size = 16, so every other slot byte is padding —
 * a deliberate internal-fragmentation trade-off in exchange for guaranteed
 * alignment (same guarantee as the free-list allocator).
 *
 * Free-slab cache:
 *   When a slab empties, instead of munmap'ing immediately, each cache holds
 *   up to SLAB_FREE_CACHE_MAX empty slabs ready to re-use.  slab_alloc pulls
 *   from this cache before calling mmap.  See the trade-off comment in slab.c.
 */

#define SLAB_CAPACITY      64u   /* objects per slab — fits in one uint64_t    */
#define SLAB_MAX_SIZE      1024u /* largest size handled by the slab allocator  */
#define SLAB_NUM_CLASSES   8     /* number of size classes                      */
#define SLAB_ALIGN         16    /* guaranteed alignment of returned pointers   */
#define SLAB_FREE_CACHE_MAX 2    /* empty slabs cached per size class before munmap */

typedef struct Slab {
    size_t       obj_size;  /* size class this slab belongs to */
    size_t       capacity;  /* always SLAB_CAPACITY = 64 */
    size_t       in_use;    /* number of currently allocated objects */
    uint64_t    *bitmap;    /* points into this slab's mmap'd region */
    void        *memory;    /* first object slot in this slab's mmap'd region */
    struct Slab *next;      /* intrusive list link (partial or full chain) */
} Slab;

typedef struct SlabCache {
    size_t  obj_size;
    Slab   *partial;        /* slabs with at least one free slot */
    Slab   *full;           /* slabs where every slot is allocated */
    Slab   *free_slabs;     /* empty slabs held warm (up to SLAB_FREE_CACHE_MAX) */
    size_t  free_count;     /* current length of the free_slabs list */
    size_t  total_allocs;
    size_t  total_frees;
} SlabCache;

/* Per-cache snapshot filled by slab_cache_info() — used by tests. */
typedef struct SlabCacheInfo {
    size_t obj_size;
    size_t partial_count;
    size_t full_count;
    size_t free_slab_count; /* empty slabs sitting in the free-slab cache */
    size_t total_in_use;
    size_t total_capacity;
    size_t total_allocs;
    size_t total_frees;
} SlabCacheInfo;

/* Public API */
void  *slab_alloc(size_t size);
void   slab_free(void *ptr, size_t size);

/*
 * Returns the actual size-class slot size for 'size' (e.g. 7 → 8, 9 → 16).
 * Returns 0 if size > SLAB_MAX_SIZE (caller should use the free-list instead).
 */
size_t slab_size_class(size_t size);

/* Fills *out with live stats for the cache at class index cls (0-based). */
void   slab_cache_info(int cls, SlabCacheInfo *out);

/* Prints a one-line summary for every size class. */
void   slab_stats(void);

#endif /* SLAB_H */
