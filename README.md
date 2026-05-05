# Arena Allocator

[Arena Allocator](https://en.wikipedia.org/wiki/Region-based_memory_management) implementation in pure C.
This is my take on arena allocators, massively built by claude.

---

## Quick Start

Include the arena.h and arena.c files appropriately in your project and include it.

The allocator reserves a large virtual address range (default: 4 GiB) but only commits physical memory as needed.

[ Reserved VA Space ]
┌──────────────┬──────────────────────┬────────────────────────┐
│   Used       │   Free (committed)   │   Uncommitted          │
│              │                      │   (no physical pages)  │
└──────────────┴──────────────────────┴────────────────────────┘
^base          ^pos                   ^committed


Reserve → no physical memory used
Commit → pages become readable/writable
Decommit → pages returned to OS
Release → entire region freed

## Basic Usage

```c
#include "arena.h"

int main(void) {
    Arena arena;

    if (!arena_init(&arena, 0)) {
        return 1;
    }

    int *x = arena_alloc(&arena, sizeof(int));
    *x = 42;

    arena_destroy(&arena);
}
```

## Snapshot / Restore

```c
ArenaSnapshot snap = arena_snapshot(&arena);

int *temp = arena_alloc(&arena, sizeof(int));

arena_restore(snap); // temp is invalid now
```

This is extremely cheap and behaves like a scoped allocation.

## Thread-Local Arena

```c
int *x = arena_tl_alloc(sizeof(int));

ArenaSnapshot s = arena_tl_snapshot();

/* ... */

arena_restore(s);
```

Each thread gets its own arena no locking.

---

## API OVERVIEW

**Lifetime**

```c
bool arena_init(Arena *a, size_t reserve);
void arena_destroy(Arena *a);
void arena_reset(Arena *a);
```

**Allocation**
```c
void *arena_alloc(Arena *a, size_t size);
void *arena_alloc_zero(Arena *a, size_t size);

```
Always 16-byte aligned
size == 0 returns a unique pointer

**Snapshots**
```c
ArenaSnapshot arena_snapshot(Arena *a);
void arena_restore(ArenaSnapshot snap);
```

Restores both position and committed memory
Decommits unused pages automatically


**Thread-local**
```c
Arena *arena_tl_get(void);
void arena_tl_destroy(void);

void *arena_tl_alloc(size_t size);
void *arena_tl_alloc_zero(size_t size);
ArenaSnapshot arena_tl_snapshot(void);
```

**Diagnostics**
```c
ArenaStats arena_stats(const Arena *a);
```

Provides:
reserved bytes
committed bytes
used bytes
wasted bytes
commit ratio
---
