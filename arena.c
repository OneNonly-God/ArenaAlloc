/*
 * arena.c  —  Growable Linear Allocator via Virtual-Memory Reservation
 *
 * See arena.h for the full design rationale and public API.
 *
 * VM model
 * ─────────
 *  RESERVE   No physical pages; accesses fault.   mmap(PROT_NONE) / MEM_RESERVE
 *  COMMIT    Backed by physical pages; R/W.        mprotect(RW)   / MEM_COMMIT
 *  DECOMMIT  Back to reserved; pages returned.    madvise+mprotect / MEM_DECOMMIT
 *  RELEASE   VA range returned to the OS.         munmap           / MEM_RELEASE
 *
 * Commit granularity
 * ───────────────────
 * Pages are committed in ARENA_COMMIT_CHUNK-aligned chunks so that:
 *   1. We minimise syscall frequency.
 *   2. We stay aligned to Windows' allocation-granularity requirement.
 *
 * Thread safety
 * ─────────────
 * Arena itself has no lock — one Arena must not be shared across threads.
 * The thread-local arena (arena_tl_*) is entirely private to each thread.
 */

#include "arena.h"

#include <assert.h>
#include <string.h>   /* memset */
#include <stdio.h>    /* fprintf (diagnostics) */

/* ── Platform selection ──────────────────────────────────────────────────── */

#if defined(_WIN32)
#  define ARENA_PLATFORM_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  define ARENA_PLATFORM_POSIX
#  include <sys/mman.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <mach/mach.h>
#  endif
#endif

/* ── Internal utility ────────────────────────────────────────────────────── */

static inline size_t align_up(size_t x, size_t align)
{
    /* align must be a power of two */
    return (x + align - 1u) & ~(align - 1u);
}

static inline size_t align_down(size_t x, size_t align)
{
    return x & ~(align - 1u);
}

/* ── OS virtual-memory primitives ────────────────────────────────────────── */

/*
 * vm_reserve — carve a VA region without touching physical memory.
 * The entire range is inaccessible (PROT_NONE / PAGE_NOACCESS) until
 * vm_commit() is called on a sub-range.
 */
static void *vm_reserve(size_t size)
{
#if defined(ARENA_PLATFORM_WINDOWS)
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
#else
    /*
     * MAP_NORESERVE: don't reserve swap space — we are the VM manager here.
     * MAP_ANONYMOUS | MAP_PRIVATE: anonymous mapping, not backed by any fd.
     * PROT_NONE: no access until we commit.
     */
    void *p = mmap(NULL, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS
#if defined(MAP_NORESERVE)
                   | MAP_NORESERVE
#endif
                   , -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
#endif
}

/*
 * vm_commit — make [addr, addr+size) accessible (R/W).
 * `addr` must lie inside a previously reserved region.
 * `size` must be a multiple of the OS page size.
 */
static bool vm_commit(void *addr, size_t size)
{
#if defined(ARENA_PLATFORM_WINDOWS)
    return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    /*
     * On Linux/macOS, PROT_NONE pages transition directly to PROT_READ|WRITE
     * via mprotect.  The kernel backs them lazily on first write (CoW).
     *
     * We also hint MADV_WILLNEED so the kernel can pre-fault pages on
     * architectures/kernels that honour the advice.
     */
    if (mprotect(addr, size, PROT_READ | PROT_WRITE) != 0)
        return false;
#  if defined(MADV_WILLNEED)
    madvise(addr, size, MADV_WILLNEED);
#  endif
    return true;
#endif
}

/*
 * vm_decommit — return physical pages to the OS while keeping VA.
 * The range becomes inaccessible again (same state as after vm_reserve).
 *
 * After this call the memory is guaranteed to read as zero when next
 * committed (Linux: MADV_DONTNEED zeros on re-fault; Windows: MEM_RESET).
 */
static bool vm_decommit(void *addr, size_t size)
{
    if (size == 0) return true;

#if defined(ARENA_PLATFORM_WINDOWS)
    /*
     * MEM_DECOMMIT releases physical pages.  A subsequent VirtualAlloc with
     * MEM_COMMIT will zero the pages again (Windows guarantee).
     */
    return VirtualFree(addr, size, MEM_DECOMMIT);
#else
    /*
     * MADV_DONTNEED tells the kernel it may reclaim the physical pages
     * immediately.  For anonymous MAP_PRIVATE mappings, re-faulting those
     * pages gives fresh zero pages (Linux guarantee, BSD-compatible).
     *
     * We then mprotect(PROT_NONE) so any accidental use after decommit
     * triggers a hard fault rather than silently returning stale data.
     */
#  if defined(MADV_FREE)
    /* macOS / newer Linux: MADV_FREE is a weaker (and cheaper) hint */
    madvise(addr, size, MADV_FREE);
#  endif
#  if defined(MADV_DONTNEED)
    madvise(addr, size, MADV_DONTNEED);
#  endif
    return mprotect(addr, size, PROT_NONE) == 0;
#endif
}

/*
 * vm_release — return the entire VA reservation to the OS.
 */
static void vm_release(void *addr, size_t size)
{
#if defined(ARENA_PLATFORM_WINDOWS)
    /* size must be 0 for MEM_RELEASE */
    (void)size;
    VirtualFree(addr, 0, MEM_RELEASE);
#else
    munmap(addr, size);
#endif
}

/* ── Commit growth ───────────────────────────────────────────────────────── */

/*
 * arena_ensure_committed — guarantee that at least `needed` bytes from
 * a->base are backed by physical pages.
 *
 * Commits in ARENA_COMMIT_CHUNK increments so that we don't call the OS
 * on every allocation.
 *
 * Returns false only if `needed` exceeds the reservation.
 */
static bool arena_ensure_committed(Arena *a, size_t needed)
{
    if (needed <= a->committed) return true;   /* fast path: already committed */
    if (needed  > a->reserved)  return false;  /* reservation exhausted        */

    /* Round up to the next commit-chunk boundary */
    size_t new_committed = align_up(needed, ARENA_COMMIT_CHUNK);
    if (new_committed > a->reserved)
        new_committed = a->reserved;

    size_t commit_size = new_committed - a->committed;
    if (!vm_commit(a->base + a->committed, commit_size))
        return false;

    a->committed = new_committed;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API — Lifetime
 * ═══════════════════════════════════════════════════════════════════════════ */

bool arena_init(Arena *a, size_t reserve)
{
    if (reserve == 0)
        reserve = ARENA_RESERVE_SIZE;

    /* Round reserve up to commit-chunk boundary */
    reserve = align_up(reserve, ARENA_COMMIT_CHUNK);

    void *base = vm_reserve(reserve);
    if (!base) {
        /* Zero-init on failure so the caller can safely pass *a to
         * arena_destroy() without a double-free. */
        *a = (Arena){0};
        return false;
    }

    *a = (Arena){
        .base      = (uint8_t *)base,
        .reserved  = reserve,
        .committed = 0,
        .pos       = 0,
    };
    return true;
}

void arena_destroy(Arena *a)
{
    if (!a || !a->base) return;
    vm_release(a->base, a->reserved);
    *a = (Arena){0};
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API — Allocation
 * ═══════════════════════════════════════════════════════════════════════════ */

void *arena_alloc(Arena *a, size_t size)
{
    assert(a && a->base && "arena_alloc: uninitialised arena");

    /* Guarantee a unique non-NULL pointer even for zero-size requests,
     * matching the contract of C11 aligned_alloc / POSIX memalign. */
    if (size == 0) size = ARENA_ALIGN;

    /* Align the current bump pointer to ARENA_ALIGN */
    size_t aligned_pos = align_up(a->pos, ARENA_ALIGN);

    /* Overflow check (extremely unlikely with a 4 GiB reservation) */
    if (aligned_pos + size < aligned_pos) return NULL;  /* wraparound */

    size_t new_pos = aligned_pos + size;

    /* Grow committed region if needed */
    if (!arena_ensure_committed(a, new_pos)) return NULL;

    a->pos = new_pos;
    return a->base + aligned_pos;
}

void *arena_alloc_zero(Arena *a, size_t size)
{
    void *p = arena_alloc(a, size);
    if (p) memset(p, 0, size);
    return p;
}

void arena_reset(Arena *a)
{
    assert(a && a->base && "arena_reset: uninitialised arena");

    if (a->committed > 0) {
        vm_decommit(a->base, a->committed);
        a->committed = 0;
    }
    a->pos = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API — Snapshots
 * ═══════════════════════════════════════════════════════════════════════════ */

ArenaSnapshot arena_snapshot(Arena *a)
{
    assert(a && a->base && "arena_snapshot: uninitialised arena");
    return (ArenaSnapshot){
        .arena     = a,
        .pos       = a->pos,
        .committed = a->committed,
    };
}

void arena_restore(ArenaSnapshot snap)
{
    Arena *a = snap.arena;
    assert(a && a->base && "arena_restore: uninitialised arena");
    assert(snap.pos       <= a->pos       && "arena_restore: snapshot is from the future");
    assert(snap.committed <= a->committed && "arena_restore: committed watermark went backwards");

    /*
     * Decommit pages that were committed after the snapshot was taken.
     *
     * We align snap.committed DOWN to a page boundary before decommitting
     * to ensure we don't decommit a partial page that still holds live data.
     * In practice snap.committed is already chunk-aligned because commits
     * always round up, so align_down is a no-op here — but it is defensive.
     */
    if (a->committed > snap.committed) {
        size_t decommit_start = align_up  (snap.committed, ARENA_COMMIT_CHUNK);
        size_t decommit_size  = a->committed - decommit_start;

        if (decommit_size > 0) {
            vm_decommit(a->base + decommit_start, decommit_size);
            a->committed = decommit_start;
        }
    }

    /*
     * Reset the bump pointer.  Any bytes between snap.pos and the new
     * committed watermark are "free" and will be overwritten by the next
     * arena_alloc call (they may still contain stale data, which is fine
     * for an arena — callers are responsible for initialising memory).
     */
    a->pos = snap.pos;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Thread-local arena
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * _Thread_local ensures each thread gets its own copy of the variables.
 * No mutex is needed.  The arena is initialised lazily on first access.
 *
 * Portability:
 *   C11 : _Thread_local  (GCC ≥ 4.9, Clang ≥ 3.3, MSVC 2019+)
 *   GCC : __thread        (older compilers — uncomment below if needed)
 */

static _Thread_local Arena tl_arena         = {0};
static _Thread_local bool  tl_arena_inited  = false;

Arena *arena_tl_get(void)
{
    if (!tl_arena_inited) {
        bool ok = arena_init(&tl_arena, 0);
        /* Treat failure as a fatal condition — if we cannot reserve VA space
         * the process is in deep trouble.  Replace with error-return if you
         * need graceful degradation. */
        if (!ok) {
            fprintf(stderr, "[arena] FATAL: thread-local arena_init failed\n");
            __builtin_trap();   /* guaranteed to abort; replace with abort() */
        }
        tl_arena_inited = true;
    }
    return &tl_arena;
}

void arena_tl_destroy(void)
{
    if (tl_arena_inited) {
        arena_destroy(&tl_arena);
        tl_arena_inited = false;
    }
}

void *arena_tl_alloc(size_t size)
{
    return arena_alloc(arena_tl_get(), size);
}

void *arena_tl_alloc_zero(size_t size)
{
    return arena_alloc_zero(arena_tl_get(), size);
}

ArenaSnapshot arena_tl_snapshot(void)
{
    return arena_snapshot(arena_tl_get());
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Diagnostics
 * ═══════════════════════════════════════════════════════════════════════════ */

ArenaStats arena_stats(const Arena *a)
{
    assert(a);
    ArenaStats s = {
        .reserved_bytes  = a->reserved,
        .committed_bytes = a->committed,
        .used_bytes      = a->pos,
        .wasted_bytes    = (a->committed > a->pos) ? (a->committed - a->pos) : 0,
        .commit_ratio    = a->reserved ? (double)a->committed / (double)a->reserved : 0.0,
    };
    return s;
}
