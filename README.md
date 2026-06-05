# HAlloc

HAlloc is a high-performance memory allocator written in C, built from scratch as a deep-dive into allocator internals.  It implements two allocation strategies — a boundary-tag free-list allocator and a slab allocator — exposed through a single unified API.  Every design decision is commented in the source so you can follow the reasoning alongside the code.

---

## Architecture

```
halloc(size) / hfree(ptr, size) / hrealloc(ptr, old, new)
              ↓ halloc.c — routing layer
       ┌──────┴──────┐
  size ≤ 1024     size > 1024
       ↓               ↓
  slab allocator   free-list allocator
  (src/slab.c)     (src/freelist.c)
```

### Free-list allocator (`src/freelist.c`)

Handles arbitrary-size allocations.  Memory is requested from the OS in 4 MB chunks via `mmap`; allocations come from within those chunks with no further syscalls until a chunk is exhausted.

- **Boundary tags** — every block carries a header *and* footer, both storing `(block_size | allocated_bit)`.  The footer lets `fl_free` coalesce with the *previous* block in O(1) without walking the list from the head.
- **Explicit free list** — free blocks are linked through doubly-linked `FLNode` structs embedded in their payloads, giving O(1) insert/remove.
- **Best-fit placement** — the entire free list is scanned and the smallest fitting block is chosen.  First-fit is faster (O(k) vs O(n)) but leaves more small gaps; best-fit minimises the leftover after each allocation.
- **Immediate coalescing** — on every `fl_free`, adjacent free blocks are merged in all four neighbour combinations before re-insertion.  Prevents "false fragmentation" where total free memory is sufficient but no single block is large enough.
- **Splitting** — a free block is split only when the leftover would be at least 32 bytes (`FL_SPLIT_THRESH`).  Smaller leftovers are kept whole to avoid creating unusable slivers.
- **16-byte alignment** — block sizes are always multiples of 16, and the heap is bootstrapped so the first payload is 16-byte aligned; all subsequent payloads inherit that alignment.

### Slab allocator (`src/slab.c`)

Handles fixed-size allocations up to 1024 bytes.  Maintains one cache per size class: `{8, 16, 32, 64, 128, 256, 512, 1024}` bytes.

- **64 objects per slab, one `uint64_t` bitmap** — a single 64-bit word tracks all slots.  Allocation finds the first free slot with `__builtin_ctzll(~bitmap)`: count trailing 1-bits of the inverted bitmap = index of the lowest 0-bit.  O(1), no scanning.
- **partial / full split** — full slabs are kept in a separate list so `slab_alloc` never skips over them; it always pulls from the head of `partial` in O(1).
- **Free-slab cache** — instead of `munmap`-ing immediately when a slab empties, each cache holds up to `SLAB_FREE_CACHE_MAX` (= 2) empty slabs.  `slab_alloc` pulls from this cache before calling `mmap`.  Without the cache, a tight alloc+free loop that drains the slab on every iteration costs two syscalls per operation (~4300 ns/op); with the cache, the same loop reuses the warm slab with no syscall (~9 ns/op).
- **16-byte alignment** — `slot_size = (obj_size + 15) & ~15`.  The 8-byte class uses 16-byte slots (8 bytes wasted per object) in exchange for uniform alignment across both allocators.

### Unified API (`src/halloc.c`)

```c
void *halloc  (size_t size);
void  hfree   (void *ptr, size_t size);       // size required — slab stores no metadata
void *hrealloc(void *ptr, size_t old, size_t new);
```

`hrealloc` has a fast path: if `old` and `new` map to the same slab size class (e.g. `hrealloc(p, 56, 64)` — both round to class 64), the same pointer is returned unchanged with zero copy overhead.

---

## Design decisions

| Decision | Why |
|---|---|
| `mmap` not `sbrk` | `sbrk` is deprecated and not thread-safe.  `mmap` lets us request independent regions and release them individually. |
| Boundary tags (header + footer) | Footer enables O(1) backward coalescing — no list traversal needed to find the previous block. |
| Best-fit vs first-fit | Best-fit leaves smaller leftovers, keeping larger contiguous regions available.  Measured external fragmentation: **0.06** after heavy churn. |
| `__builtin_ctzll(~bitmap)` | One instruction finds the lowest free slot in a 64-object slab.  No loop, no branch. |
| Free-slab cache | Bounds the mmap syscall cost per size class to at most one call per 64 allocations in steady state.  Idle RSS is bounded: at most `SLAB_FREE_CACHE_MAX × num_classes` empty slabs sit warm. |
| Caller supplies size to `hfree` | The slab stores no per-object size metadata (bitmap only tracks free/allocated).  Requiring the caller to pass `size` mirrors typed allocators like jemalloc's `sdallocx` and avoids a header word per object. |

---

## Benchmark results

Measured on Linux x86-64, `-O2`, `CLOCK_MONOTONIC`.  System malloc is glibc's `ptmalloc2`.

```
╔════════════════════════════════════════════════════════════════════╗
║                      HAlloc Benchmark Results                      ║
╠══════════════════════╬════════════════╬════════════════╬═══════════╣
║ Benchmark            ║ HAlloc         ║ System malloc  ║ Delta     ║
╠══════════════════════╬════════════════╬════════════════╬═══════════╣
║ Slab 64B (1M ops)    ║ 9.2 ns/op      ║ 8.4 ns/op      ║ +9%       ║
║ Freelist 64B (1M)    ║ 7.2 ns/op      ║ 8.4 ns/op      ║ -15%      ║
║ Mixed sizes (1M)     ║ 18.8 ns/op     ║ 11.3 ns/op     ║ +66%      ║
║ Locality 64B         ║ 65.1 ns/op     ║ 24.7 ns/op     ║ +164%     ║
║ Realloc 64->128->64  ║ 10672.7 ns/op  ║ 30.0 ns/op     ║ +35428%   ║
╚══════════════════════╩════════════════╩════════════════╩═══════════╝

Fragmentation after churn (10K alloc / free half / 5K more):
  External frag : 0.0613   (0 = none, 1 = severe)
  Internal frag : 46 756 bytes
  Free blocks   : 443
  Largest free  : ~4.1 MB
  Peak RSS      : ~44 MB
```

**Reading the numbers:**

- **Slab 64B** is within 9% of system malloc for a tight single-object loop.  The free-slab cache (`SLAB_FREE_CACHE_MAX = 2`) means the benchmark reuses the same warm slab on every iteration — no `mmap` syscall.  Without the cache this was **~4300 ns/op**.

- **Freelist 64B** beats system malloc by 15%.  The 4 MB chunk is allocated once and reused across all 1M iterations with zero further OS calls; the only work is pointer manipulation in the doubly-linked free list.

- **Mixed sizes** is slower because roughly 20% of the random sizes (`2048`, `4096`) route to the free-list and must traverse its best-fit scan, while the other 80% go through the slab's O(slabs) ownership search in `slab_free`.

- **Realloc 64→128→64** is slow because 64 and 128 are *different* size classes, forcing a full `slab_alloc + memcpy + slab_free` on every call.  The same-class fast path (`hrealloc(p, 56, 64)` — both mapping to class 64) returns the same pointer unchanged with no copy.

- **External fragmentation of 0.06** after heavy churn means 94% of all free memory sits in the single largest free block — a direct consequence of best-fit placement and immediate coalescing.

---

## Build

```sh
make          # build everything (tests + benchmark)
make test     # compile and run all 12 correctness tests (ASan + UBSan)
make bench    # compile and run the benchmark harness
make clean    # remove all compiled objects and binaries
```

**Requirements:** GCC with AddressSanitizer support (`-fsanitize=address,undefined`), Linux (for `mmap(MAP_ANONYMOUS)` and `/proc/self/status`).

---

## Project structure

```
HAlloc/
├── src/
│   ├── freelist.h / freelist.c   ← boundary-tag best-fit allocator
│   ├── slab.h / slab.c           ← bitmap slab allocator
│   └── halloc.h / halloc.c       ← unified halloc/hfree/hrealloc API
├── tests/
│   ├── test_freelist.c           ← 6 correctness tests (ASan)
│   └── test_slab.c               ← 6 correctness tests (ASan)
├── bench/
│   ├── bench.h                   ← CLOCK_MONOTONIC timing helpers
│   └── bench.c                   ← 5 benchmark scenarios
└── Makefile
```
