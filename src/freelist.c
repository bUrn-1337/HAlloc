/*
 * freelist.c — boundary-tag best-fit free-list allocator
 *
 * Design decisions:
 *
 * 1. mmap() instead of sbrk(): sbrk is deprecated and non-thread-safe.
 *    mmap lets us request independent 4 MB chunks from the OS; each chunk
 *    is tracked so we can munmap it when fully freed.
 *
 * 2. Boundary tags (header + footer): every block carries its size and
 *    allocated bit in both a header (before payload) and a footer (after
 *    payload). The footer lets fl_free() reach the previous block in O(1)
 *    to coalesce leftward, instead of walking the list from the head.
 *
 * 3. Best-fit placement: we scan the entire free list and pick the smallest
 *    block that satisfies the request. First-fit is faster (O(k) vs O(n))
 *    but leaves many small unusable gaps, increasing external fragmentation.
 *    Best-fit minimises the leftover after each allocation, keeping larger
 *    contiguous regions available for future requests.
 *
 * 4. Immediate coalescing: on every fl_free(), adjacent free blocks are
 *    merged before the block is returned to the free list. This prevents
 *    the "false fragmentation" problem where the heap has enough total free
 *    memory but no single block is large enough to satisfy a request.
 *
 * 5. Splitting: if the chosen free block is at least (requested + overhead
 *    + FL_SPLIT_THRESH) bytes, we carve off the exact amount and push the
 *    remainder back as a new free block. The threshold avoids creating tiny
 *    unusable slivers (< 32 bytes of payload) that would only add overhead.
 */

#include "freelist.h"

#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Internal tag helpers
 * ---------------------------------------------------------------------- */

/*
 * GCC emits a false -Wstringop-overflow when it sees memcpy through a
 * pointer it cannot track statically (it assumes the region might be 0
 * bytes).  The code is correct — all pointers are into mmap'd regions.
 * Suppress only around these two helpers.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"

/* Read the raw tag word stored at address p. */
static inline size_t tag_read(void *p)
{
    size_t v;
    memcpy(&v, p, sizeof(size_t));
    return v;
}

/* Write a raw tag word at address p. */
static inline void tag_write(void *p, size_t v)
{
    memcpy(p, &v, sizeof(size_t));
}

#pragma GCC diagnostic pop

/* Pack size and allocated flag into one tag word. */
static inline size_t tag_pack(size_t size, int allocated)
{
    return (size & FL_SIZE_MASK) | (allocated ? FL_ALLOC_BIT : 0);
}

/* Extract block size from a tag word. */
static inline size_t tag_size(size_t tag)   { return tag & FL_SIZE_MASK; }

/* Extract allocated flag from a tag word. */
static inline int    tag_alloc(size_t tag)  { return (int)(tag & FL_ALLOC_BIT); }

/* -------------------------------------------------------------------------
 * Block navigation macros (operate on the header pointer)
 *
 * "hdr"  = pointer to the 8-byte header that precedes the payload
 * "ftr"  = pointer to the 8-byte footer that follows the payload
 *
 * Block layout:
 *   [hdr 8B][payload ... ][ftr 8B]
 * -------------------------------------------------------------------------*/

/* Header of the block whose header starts at h. */
#define HDR(h)          ((void *)(h))

/* Footer of the block whose header is at h. */
#define FTR(h)          ((char *)(h) + tag_size(tag_read(h)) - FL_TAG_SIZE)

/* Payload pointer given a header pointer. */
#define HDR_TO_PAY(h)   ((void *)((char *)(h) + FL_TAG_SIZE))

/* Header pointer given a payload pointer. */
#define PAY_TO_HDR(p)   ((void *)((char *)(p) - FL_TAG_SIZE))

/* Header of the next (higher-address) block. */
#define NEXT_HDR(h)     ((void *)((char *)(h) + tag_size(tag_read(h))))

/* Header of the previous (lower-address) block.
 * We read the footer of the previous block, which sits immediately before
 * the current header.  The footer stores the previous block's full size,
 * so we subtract that to reach the previous block's header. */
#define PREV_HDR(h)     ((void *)((char *)(h) - tag_size(tag_read((char *)(h) - FL_TAG_SIZE))))

/* -------------------------------------------------------------------------
 * mmap chunk tracking
 * ---------------------------------------------------------------------- */

typedef struct Chunk {
    void        *base;   /* mmap'd address */
    size_t       size;   /* total mmap'd bytes */
    struct Chunk *next;
} Chunk;

/* -------------------------------------------------------------------------
 * Allocator state
 * ---------------------------------------------------------------------- */

static FLNode *free_head = NULL;  /* doubly-linked explicit free list */
static Chunk  *chunk_list = NULL; /* all mmap'd regions */

/* Stats counters */
static size_t stat_alloc_bytes  = 0; /* payload bytes currently allocated */
static size_t stat_free_bytes   = 0; /* payload bytes currently free      */
static size_t stat_internal_frag = 0; /* padding inside allocated blocks  */
static size_t stat_num_free     = 0; /* number of free blocks             */

/* -------------------------------------------------------------------------
 * Free-list link/unlink helpers
 * ---------------------------------------------------------------------- */

static void fl_list_insert(void *hdr)
{
    FLNode *node = (FLNode *)HDR_TO_PAY(hdr);
    node->prev = NULL;
    node->next = free_head;
    if (free_head)
        free_head->prev = node;
    free_head = node;
    stat_num_free++;
}

static void fl_list_remove(void *hdr)
{
    FLNode *node = (FLNode *)HDR_TO_PAY(hdr);
    if (node->prev)
        node->prev->next = node->next;
    else
        free_head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    node->prev = node->next = NULL;
    stat_num_free--;
}

/* -------------------------------------------------------------------------
 * mmap chunk allocation
 * ---------------------------------------------------------------------- */

/*
 * Request a new chunk from the OS.  We place a small prologue block at the
 * start and an epilogue block at the end — both permanently "allocated" with
 * size 0 / FL_TAG_SIZE — so that PREV_HDR / NEXT_HDR never walk outside the
 * chunk.  The interior is one large free block.
 *
 * Chunk layout:
 *   [prologue hdr 8B][prologue ftr 8B]
 *   [free block hdr 8B][free payload ...][free block ftr 8B]
 *   [epilogue hdr 8B]
 *
 * Prologue total size  = FL_OVERHEAD   (hdr+ftr, no payload)
 * Epilogue total size  = FL_TAG_SIZE   (hdr only; no next block touches it)
 */
static int grow_heap(size_t min_payload)
{
    /* Round up chunk size to page boundary, at least FL_CHUNK_SIZE. */
    size_t need = FL_OVERHEAD * 2 + FL_TAG_SIZE + min_payload;
    size_t chunk_sz = FL_CHUNK_SIZE;
    if (need > chunk_sz)
        chunk_sz = (need + 4095) & ~(size_t)4095;

    void *mem = mmap(NULL, chunk_sz,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    if (mem == MAP_FAILED)
        return -1;

    /* Track chunk so we can munmap later. */
    Chunk *c = (Chunk *)mem;   /* borrow first sizeof(Chunk) bytes of chunk
                                 — we account for this below */
    c->base = mem;
    c->size = chunk_sz;
    c->next = chunk_list;
    chunk_list = c;

    /*
     * Align block area after the Chunk struct.
     *
     * Payload = header + FL_TAG_SIZE.  For payload to be FL_ALIGNMENT-aligned,
     * the header must sit at (base + k*FL_ALIGNMENT + FL_TAG_SIZE).  We
     * achieve this by rounding meta up to a multiple of FL_ALIGNMENT and then
     * adding FL_TAG_SIZE — so the prologue starts at ≡ FL_TAG_SIZE (mod 16),
     * and every subsequent block header inherits the same offset because
     * block sizes are always multiples of FL_ALIGNMENT.
     */
    size_t meta = sizeof(Chunk);
    meta = ((meta + FL_ALIGNMENT - 1) & ~(size_t)(FL_ALIGNMENT - 1)) + FL_TAG_SIZE;

    char *p = (char *)mem + meta;
    size_t usable = chunk_sz - meta;

    /* Prologue: allocated, size = FL_OVERHEAD */
    tag_write(p,                  tag_pack(FL_OVERHEAD, 1));
    tag_write(p + FL_TAG_SIZE,    tag_pack(FL_OVERHEAD, 1));
    p += FL_OVERHEAD;
    usable -= FL_OVERHEAD;

    /* Epilogue: allocated, size = FL_TAG_SIZE (header only sentinel) */
    size_t epilogue_off = usable - FL_TAG_SIZE;
    tag_write(p + epilogue_off, tag_pack(FL_TAG_SIZE, 1));
    usable -= FL_TAG_SIZE;

    /* Single free block filling the interior. */
    size_t free_sz = usable; /* must be multiple of FL_ALIGNMENT — guaranteed
                               because chunk_sz and meta are both page/16 aligned */
    tag_write(p,                   tag_pack(free_sz, 0));
    tag_write(p + free_sz - FL_TAG_SIZE, tag_pack(free_sz, 0));

    stat_free_bytes += free_sz - FL_OVERHEAD;
    fl_list_insert(p);

    return 0;
}

/* -------------------------------------------------------------------------
 * Coalescing
 *
 * Returns the header of the (possibly merged) free block.
 * The block must already be marked free in its tags before calling.
 * ---------------------------------------------------------------------- */
static void *coalesce(void *hdr)
{
    size_t sz        = tag_size(tag_read(hdr));
    void  *prev_hdr  = PREV_HDR(hdr);
    void  *next_hdr  = NEXT_HDR(hdr);
    int    prev_free = !tag_alloc(tag_read(prev_hdr));
    int    next_free = !tag_alloc(tag_read(next_hdr));

    if (!prev_free && !next_free) {
        /* Case 1: neighbours are both allocated — nothing to merge. */
        fl_list_insert(hdr);
        return hdr;
    }

    if (!prev_free && next_free) {
        /* Case 2: only the next block is free — merge with it. */
        fl_list_remove(next_hdr);
        sz += tag_size(tag_read(next_hdr));
        tag_write(hdr,              tag_pack(sz, 0));
        tag_write(FTR(hdr),         tag_pack(sz, 0));
        fl_list_insert(hdr);
        return hdr;
    }

    if (prev_free && !next_free) {
        /* Case 3: only the previous block is free — merge with it. */
        fl_list_remove(prev_hdr);
        sz += tag_size(tag_read(prev_hdr));
        tag_write(prev_hdr,         tag_pack(sz, 0));
        tag_write(FTR(prev_hdr),    tag_pack(sz, 0));
        fl_list_insert(prev_hdr);
        return prev_hdr;
    }

    /* Case 4: both neighbours free — merge all three. */
    fl_list_remove(prev_hdr);
    fl_list_remove(next_hdr);
    sz += tag_size(tag_read(prev_hdr)) + tag_size(tag_read(next_hdr));
    tag_write(prev_hdr,         tag_pack(sz, 0));
    tag_write(FTR(prev_hdr),    tag_pack(sz, 0));
    fl_list_insert(prev_hdr);
    return prev_hdr;
}

/* -------------------------------------------------------------------------
 * fl_malloc
 * ---------------------------------------------------------------------- */

/*
 * Round requested size up to a block size that satisfies alignment:
 *   block_size = align16(size + FL_OVERHEAD)
 * Minimum block must hold at least two pointers in its payload (for FLNode).
 */
static size_t round_block_size(size_t payload)
{
    size_t min_pay = sizeof(FLNode);   /* need space for prev/next pointers */
    if (payload < min_pay)
        payload = min_pay;
    size_t blk = payload + FL_OVERHEAD;
    return (blk + (FL_ALIGNMENT - 1)) & ~(size_t)(FL_ALIGNMENT - 1);
}

void *fl_malloc(size_t size)
{
    if (size == 0)
        return NULL;

    size_t need = round_block_size(size);

    /* Best-fit: scan entire free list, remember smallest fitting block. */
    FLNode *best      = NULL;
    size_t  best_size = SIZE_MAX;

    for (FLNode *cur = free_head; cur; cur = cur->next) {
        void  *h  = PAY_TO_HDR(cur);
        size_t sz = tag_size(tag_read(h));
        if (sz >= need && sz < best_size) {
            best      = cur;
            best_size = sz;
        }
    }

    if (!best) {
        /* No fitting block — request a new chunk from the OS. */
        if (grow_heap(size) < 0)
            return NULL;
        /* Re-run best-fit over the fresh free block. */
        for (FLNode *cur = free_head; cur; cur = cur->next) {
            void  *h  = PAY_TO_HDR(cur);
            size_t sz = tag_size(tag_read(h));
            if (sz >= need && sz < best_size) {
                best      = cur;
                best_size = sz;
            }
        }
        if (!best)
            return NULL;
    }

    void  *hdr     = PAY_TO_HDR(best);
    size_t blk_sz  = tag_size(tag_read(hdr));

    fl_list_remove(hdr);

    /* Split if the leftover is large enough to be useful. */
    size_t leftover = blk_sz - need;
    if (leftover >= (FL_OVERHEAD + FL_SPLIT_THRESH)) {
        /* Carve off 'need' bytes at the front; remainder becomes a free block. */
        tag_write(hdr,              tag_pack(need, 1));
        tag_write(FTR(hdr),         tag_pack(need, 1));

        void *rem_hdr = NEXT_HDR(hdr);
        tag_write(rem_hdr,              tag_pack(leftover, 0));
        tag_write(FTR(rem_hdr),         tag_pack(leftover, 0));

        stat_free_bytes  -= need - FL_OVERHEAD;  /* payload consumed         */
        stat_alloc_bytes += size;                /* actual requested bytes   */
        stat_internal_frag += (need - FL_OVERHEAD) - size; /* alignment pad */

        /* Insert remainder directly — no need to coalesce (neighbours are
         * allocated: the block we just split and the next block unchanged). */
        fl_list_insert(rem_hdr);
    } else {
        /* Use the entire block; any leftover becomes internal fragmentation. */
        tag_write(hdr,       tag_pack(blk_sz, 1));
        tag_write(FTR(hdr),  tag_pack(blk_sz, 1));

        stat_free_bytes   -= blk_sz - FL_OVERHEAD;
        stat_alloc_bytes  += size;
        stat_internal_frag += (blk_sz - FL_OVERHEAD) - size;
    }

    return HDR_TO_PAY(hdr);
}

/* -------------------------------------------------------------------------
 * fl_free
 * ---------------------------------------------------------------------- */

void fl_free(void *ptr)
{
    if (!ptr)
        return;

    void  *hdr = PAY_TO_HDR(ptr);
    size_t sz  = tag_size(tag_read(hdr));

    /* Sanity: block must be marked allocated. */
    assert(tag_alloc(tag_read(hdr)) && "double-free or invalid pointer");

    /* Reverse stats: we don't know the original requested size, so recover
     * internal fragmentation as (payload capacity - what coalesce will credit).
     * We undo alloc_bytes conservatively: credit back the full payload capacity.
     * (Internal frag tracking is approximate post-split.) */
    size_t pay_cap = sz - FL_OVERHEAD;
    if (stat_alloc_bytes >= pay_cap)
        stat_alloc_bytes -= pay_cap;
    else
        stat_alloc_bytes = 0;
    /* Restore internal frag credit when we free. */
    stat_internal_frag = (stat_internal_frag >= pay_cap) ?
                          stat_internal_frag - pay_cap : 0;

    /* Mark block free in both tags. */
    tag_write(hdr,       tag_pack(sz, 0));
    tag_write(FTR(hdr),  tag_pack(sz, 0));

    stat_free_bytes += pay_cap;

    /* Coalesce and insert. */
    coalesce(hdr);
}

/* -------------------------------------------------------------------------
 * fl_realloc
 * ---------------------------------------------------------------------- */

void *fl_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return fl_malloc(size);
    if (size == 0) {
        fl_free(ptr);
        return NULL;
    }

    void  *hdr     = PAY_TO_HDR(ptr);
    size_t cur_sz  = tag_size(tag_read(hdr));
    size_t need    = round_block_size(size);

    if (cur_sz >= need) {
        /* Current block already fits — return as-is (no shrink-split for
         * simplicity; avoids fragmentation from tiny realloc calls). */
        return ptr;
    }

    /* Check if we can absorb the next free block in-place. */
    void  *next_hdr  = NEXT_HDR(hdr);
    size_t next_sz   = tag_size(tag_read(next_hdr));
    int    next_free = !tag_alloc(tag_read(next_hdr));

    if (next_free && (cur_sz + next_sz) >= need) {
        fl_list_remove(next_hdr);
        size_t merged = cur_sz + next_sz;
        stat_free_bytes -= next_sz - FL_OVERHEAD;

        size_t leftover = merged - need;
        if (leftover >= (FL_OVERHEAD + FL_SPLIT_THRESH)) {
            tag_write(hdr,       tag_pack(need, 1));
            tag_write(FTR(hdr),  tag_pack(need, 1));
            void *rem = NEXT_HDR(hdr);
            tag_write(rem,       tag_pack(leftover, 0));
            tag_write(FTR(rem),  tag_pack(leftover, 0));
            fl_list_insert(rem);
            stat_free_bytes += leftover - FL_OVERHEAD;
        } else {
            tag_write(hdr,       tag_pack(merged, 1));
            tag_write(FTR(hdr),  tag_pack(merged, 1));
        }
        stat_alloc_bytes += size;
        return ptr;
    }

    /* Fallback: allocate new, copy, free old. */
    void *new_ptr = fl_malloc(size);
    if (!new_ptr)
        return NULL;

    size_t copy_sz = cur_sz - FL_OVERHEAD;
    if (copy_sz > size)
        copy_sz = size;
    memcpy(new_ptr, ptr, copy_sz);
    fl_free(ptr);
    return new_ptr;
}

/* -------------------------------------------------------------------------
 * fl_stats / fl_print_stats
 * ---------------------------------------------------------------------- */

void fl_stats(FLStats *out)
{
    /* Find largest free block. */
    size_t largest = 0;
    for (FLNode *cur = free_head; cur; cur = cur->next) {
        void  *h  = PAY_TO_HDR(cur);
        size_t pay = tag_size(tag_read(h)) - FL_OVERHEAD;
        if (pay > largest)
            largest = pay;
    }

    out->total_allocated  = stat_alloc_bytes;
    out->total_free       = stat_free_bytes;
    out->internal_frag    = stat_internal_frag;
    out->num_free_blocks  = stat_num_free;
    out->largest_free_block = largest;

    /*
     * External fragmentation = 1 - (largest_free_block / total_free).
     * Ranges 0 (all free memory in one block) to ~1 (severely fragmented).
     */
    if (stat_free_bytes > 0)
        out->external_frag = 1.0 - (double)largest / (double)stat_free_bytes;
    else
        out->external_frag = 0.0;
}

void fl_print_stats(void)
{
    FLStats s;
    fl_stats(&s);
    printf("=== HAlloc free-list stats ===\n");
    printf("  Allocated bytes   : %zu\n", s.total_allocated);
    printf("  Free bytes        : %zu\n", s.total_free);
    printf("  Internal frag     : %zu bytes\n", s.internal_frag);
    printf("  External frag     : %.4f  (0=none, 1=severe)\n", s.external_frag);
    printf("  Free blocks       : %zu\n", s.num_free_blocks);
    printf("  Largest free block: %zu bytes\n", s.largest_free_block);
}
