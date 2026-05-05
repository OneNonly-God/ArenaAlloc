/*
 * arena.h  —  Growable Linear Allocator via Virtual-Memory Reservation
 * ======================================================================
 *
 * Design goals:
 *   • O(1) "bump" allocation  (the whole point of an arena)
 *   • Growable              — reserves a giant VA window; commits pages lazily
 *   • 16-byte aligned       — every pointer returned is 16-byte aligned
 *   • Thread-local          — each thread owns a private arena; no locking
 *   • Explicit snapshots    — save/restore the bump position + decommit pages
 *
 * Platform: Linux / macOS / Windows (C11, -std=c11)
 *
 *  ┌──────────────────────────────────────────────────────────────────────┐
 *  │  Virtual address space (ARENA_RESERVE_SIZE, e.g. 4 GiB)             │
 *  │  ┌──────────────┬────────────────────────┬────────────────────────┐ │
 *  │  │  committed   │   committed, available  │   reserved (PROT_NONE) │ │
 *  │  │  + used      │   (committed but free)  │   (no physical pages)  │ │
 *  │  └──────────────┴────────────────────────┴────────────────────────┘ │
 *  │  ^base          ^pos                      ^committed                 │
 *  └──────────────────────────────────────────────────────────────────────┘
 *
 * Usage (single arena):
 *
 *   Arena a;
 *   arena_init(&a, 0);                    // reserve 4 GiB VA
 *
 *   Foo *f = arena_alloc(&a, sizeof *f);  // O(1) bump
 *
 *   ArenaSnapshot snap = arena_snapshot(&a);
 *     Bar *b = arena_alloc(&a, sizeof *b);
 *     // … use b …
 *   arena_restore(snap);                  // b is gone, pages decommitted
 *
 *   arena_destroy(&a);
 *
 * Usage (thread-local arena — no init required):
 *
 *   Foo *f = arena_tl_alloc(sizeof *f);
 *   ArenaSnapshot s = arena_tl_snapshot();
 *   // …
 *   arena_restore(s);
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Tunables ─────────────────────────────────────────────────────────────── */

/* Total virtual address space reserved per arena (default 4 GiB).
 * No physical memory is consumed until pages are actually touched. */
#ifndef ARENA_RESERVE_SIZE
#  define ARENA_RESERVE_SIZE  ((size_t)4 << 30)   /* 4 GiB */
#endif

/* Physical pages are committed in multiples of this chunk.
 * Must be a multiple of the OS page size (4 KiB on x86).
 * 64 KiB matches Windows' allocation-granularity and Linux huge-page hints. */
#ifndef ARENA_COMMIT_CHUNK
#  define ARENA_COMMIT_CHUNK  ((size_t)64 << 10)  /* 64 KiB */
#endif

/* Guaranteed alignment of every pointer returned by arena_alloc(). */
#define ARENA_ALIGN  ((size_t)16)

/* ── Core types ───────────────────────────────────────────────────────────── */

/*
 * Arena — the allocator object.
 * Treat all fields as read-only from user code; mutate only through the API.
 */
typedef struct {
    uint8_t *base;       /* start of the reserved VA region                  */
    size_t   reserved;   /* total reserved bytes (virtual — no physical cost) */
    size_t   committed;  /* bytes backed by physical pages so far             */
    size_t   pos;        /* bump-pointer offset from base (next free byte)    */
} Arena;

/*
 * ArenaSnapshot — a lightweight save-point.
 * Stores both the bump position and the committed watermark so that
 * arena_restore() can return physical pages to the OS.
 */
typedef struct {
    Arena  *arena;      /* the arena this snapshot belongs to                 */
    size_t  pos;        /* saved bump-pointer offset                          */
    size_t  committed;  /* saved committed watermark                          */
} ArenaSnapshot;

/* ── Lifetime ─────────────────────────────────────────────────────────────── */

/*
 * arena_init — reserve virtual address space and prepare the arena.
 *
 * `reserve` bytes of VA space are reserved but no physical pages are
 * committed yet.  Pass 0 to use ARENA_RESERVE_SIZE.
 *
 * Returns false (and leaves *a zeroed) on OS failure (OOM, address-space
 * exhaustion), which is almost impossible in practice with a 4 GiB window.
 */
bool arena_init   (Arena *a, size_t reserve);

/*
 * arena_destroy — decommit all pages and release the VA reservation.
 * Safe to call on a zero-initialised arena.
 */
void arena_destroy(Arena *a);

/* ── Allocation ───────────────────────────────────────────────────────────── */

/*
 * arena_alloc — O(1) bump-allocate `size` bytes, aligned to ARENA_ALIGN.
 *
 * Commits new pages automatically when the current committed range is
 * insufficient.  Returns NULL only if the entire reservation is exhausted
 * (i.e. you allocated more than ARENA_RESERVE_SIZE bytes in one arena).
 *
 * Passing size == 0 returns a unique non-NULL pointer (same as malloc(0)).
 */
void *arena_alloc     (Arena *a, size_t size);

/*
 * arena_alloc_zero — like arena_alloc but zero-fills the returned memory.
 */
void *arena_alloc_zero(Arena *a, size_t size);

/*
 * arena_reset — free all allocations and decommit all physical pages.
 * The VA reservation is kept so the arena can be reused immediately.
 */
void  arena_reset(Arena *a);

/* ── Snapshots ────────────────────────────────────────────────────────────── */

/*
 * arena_snapshot — capture the current bump position.
 * Extremely cheap: two pointer-sized loads and a struct copy.
 */
ArenaSnapshot arena_snapshot(Arena *a);

/*
 * arena_restore — roll the arena back to a previously captured snapshot.
 *
 * Any memory allocated after the snapshot is logically freed.
 * Pages that were committed after the snapshot are decommitted, returning
 * physical memory to the OS.
 *
 * Behaviour is undefined if snap.arena has been destroyed, or if two
 * snapshots from the same arena are restored out of order.
 */
void arena_restore(ArenaSnapshot snap);

/* ── Thread-local convenience API ────────────────────────────────────────── */

/*
 * Every thread that calls any arena_tl_*() function owns a private Arena
 * that is lazily initialised on first use.  No synchronisation is needed.
 *
 * Call arena_tl_destroy() before the thread exits to release its VA
 * reservation.  Skipping the call leaks VA space but not physical memory
 * on most OSes (the reservation is freed when the process exits anyway).
 */

Arena        *arena_tl_get    (void);           /* init-on-first-use accessor */
void          arena_tl_destroy(void);           /* release this thread's arena */

void         *arena_tl_alloc     (size_t size);
void         *arena_tl_alloc_zero(size_t size);
ArenaSnapshot arena_tl_snapshot  (void);
/* NOTE: arena_restore() works unchanged for TL snapshots — the snapshot
 * carries a back-pointer to its arena. */

/* ── Diagnostic helpers ───────────────────────────────────────────────────── */

typedef struct {
    size_t reserved_bytes;
    size_t committed_bytes;
    size_t used_bytes;       /* actual bump-pointer position (post-align) */
    size_t wasted_bytes;     /* committed − used (available without a syscall) */
    double commit_ratio;     /* committed / reserved  [0, 1] */
} ArenaStats;

ArenaStats arena_stats(const Arena *a);
