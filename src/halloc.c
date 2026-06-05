/*
 * halloc.c — unified allocator dispatch (Phase 2).
 *
 * halloc/hfree/hrealloc sit above both sub-allocators and route based on size.
 */

#include "halloc.h"
#include <string.h>

void *halloc(size_t size)
{
    if (size == 0)
        return NULL;
    if (size <= SLAB_MAX_SIZE)
        return slab_alloc(size);
    return fl_malloc(size);
}

void hfree(void *ptr, size_t size)
{
    if (!ptr)
        return;
    if (size <= SLAB_MAX_SIZE)
        slab_free(ptr, size);
    else
        fl_free(ptr);
}

void *hrealloc(void *ptr, size_t old_size, size_t new_size)
{
    if (!ptr)
        return halloc(new_size);
    if (new_size == 0) {
        hfree(ptr, old_size);
        return NULL;
    }

    /*
     * Slab objects within the same size class already have enough capacity
     * for the new request — return the existing pointer unchanged.
     * slab_size_class() returns the actual class bucket (e.g. 7 → 8, 9 → 16).
     */
    if (old_size <= SLAB_MAX_SIZE && new_size <= SLAB_MAX_SIZE) {
        if (slab_size_class(old_size) == slab_size_class(new_size))
            return ptr;
    }

    /* For free-list objects, if the block is already large enough (realloc
     * with a smaller or equal new_size when old is also freelist), fl_realloc
     * handles the in-place case internally. */
    if (old_size > SLAB_MAX_SIZE && new_size > SLAB_MAX_SIZE)
        return fl_realloc(ptr, new_size);

    /* Cross-allocator: alloc, copy, free. */
    void  *new_ptr  = halloc(new_size);
    if (!new_ptr)
        return NULL;
    size_t copy_sz  = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_sz);
    hfree(ptr, old_size);
    return new_ptr;
}
