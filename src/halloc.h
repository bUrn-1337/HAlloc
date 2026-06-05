#ifndef HALLOC_H
#define HALLOC_H

/*
 * halloc.h — unified HAlloc API (Phase 2).
 *
 * Routing policy:
 *   size ≤ 1024  →  slab allocator  (fixed-size, O(1) alloc/free)
 *   size  > 1024  →  free-list allocator  (arbitrary size, best-fit)
 *
 * Unlike standard realloc(), hfree() and hrealloc() require the caller to
 * supply the original allocation size.  This mirrors typed allocators (e.g.
 * jemalloc's sdallocx) and is necessary because the slab allocator does not
 * embed size metadata in the object — the bitmap stores only allocated/free,
 * not the exact requested bytes.
 */

#include <stddef.h>
#include "freelist.h"
#include "slab.h"

void  *halloc  (size_t size);
void   hfree   (void *ptr, size_t size);
void  *hrealloc(void *ptr, size_t old_size, size_t new_size);

#endif /* HALLOC_H */
