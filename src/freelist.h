#ifndef FREELIST_H
#define FREELIST_H

#include <stddef.h>
#include <stdint.h>

/*
 * Boundary-tag free-list allocator.
 *
 * Memory layout of every block:
 *
 *   [ header (8 bytes) | payload ... | footer (8 bytes) ]
 *
 * Both header and footer store: (block_size | allocated_bit)
 * The footer lets us locate the previous block in O(1) during coalescing
 * without walking the free list from the head.
 *
 * Block size is always a multiple of ALIGNMENT (16 bytes).
 * The lowest bit of the stored word encodes the allocated flag (0 = free).
 */

#define FL_ALIGNMENT      16
#define FL_TAG_SIZE       8       /* size of one boundary tag (header/footer) */
#define FL_OVERHEAD       (2 * FL_TAG_SIZE)   /* header + footer */
#define FL_SPLIT_THRESH   32      /* minimum leftover to justify a split */
#define FL_CHUNK_SIZE     (4 * 1024 * 1024)   /* 4 MB mmap chunk */

/* Allocated flag lives in the lowest bit of the tag word. */
#define FL_ALLOC_BIT      1UL
#define FL_SIZE_MASK      (~FL_ALLOC_BIT)

/* Explicit free-list node embedded at the start of each free block's payload. */
typedef struct FLNode {
    struct FLNode *prev;
    struct FLNode *next;
} FLNode;

/* Statistics exposed by fl_stats(). */
typedef struct FLStats {
    size_t total_allocated;    /* bytes currently allocated (payload only) */
    size_t total_free;         /* bytes currently free (payload only) */
    size_t internal_frag;      /* padding wasted inside allocated blocks */
    size_t num_free_blocks;
    size_t largest_free_block; /* payload bytes of the largest free block */
    /* external fragmentation: 1 - (largest_free / total_free); 0 if no free */
    double external_frag;
} FLStats;

/* Public API */
void  *fl_malloc(size_t size);
void   fl_free(void *ptr);
void  *fl_realloc(void *ptr, size_t size);
void   fl_stats(FLStats *out);
void   fl_print_stats(void);

#endif /* FREELIST_H */
