/*
 * arena_test.c  —  Test suite for the growable linear allocator
 *
 * Covers:
 *   1. Basic init / destroy
 *   2. Alignment guarantee (every pointer is 16-byte aligned)
 *   3. Growability (commit more pages on demand)
 *   4. Snapshots (save / restore + physical decommit)
 *   5. Reset
 *   6. Thread-local arena (2 worker threads + main thread)
 *   7. Stress: allocate → snapshot → fill → restore (cyclic)
 *   8. Diagnostics / stats
 *   9. Edge cases: zero-size alloc, overflow guard
 *
 * Build:
 *   Linux / macOS:  cc -std=c11 -pthread -O2 -o arena_test arena_test.c arena.c
 *   Windows (MSVC): cl /std:c11 /O2 arena_test.c arena.c
 */

#include "arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <threads.h>   /* C11: thrd_create / thrd_join */

/* ── Mini test framework ─────────────────────────────────────────────────── */

static int  g_tests   = 0;
static int  g_passed  = 0;
static int  g_failed  = 0;

#define ANSI_GREEN "\033[32m"
#define ANSI_RED   "\033[31m"
#define ANSI_CYAN  "\033[36m"
#define ANSI_RESET "\033[0m"

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_tests;                                                       \
        if (cond) {                                                      \
            ++g_passed;                                                  \
            printf("  " ANSI_GREEN "PASS" ANSI_RESET "  %s\n", #cond); \
        } else {                                                         \
            ++g_failed;                                                  \
            printf("  " ANSI_RED  "FAIL" ANSI_RESET "  %s  "           \
                   "(" __FILE__ ":%d)\n", #cond, __LINE__);             \
        }                                                                \
    } while(0)

#define SECTION(name) \
    printf("\n" ANSI_CYAN "── " name " ──" ANSI_RESET "\n")

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static bool is_aligned(const void *p, size_t align)
{
    return ((uintptr_t)p & (align - 1u)) == 0;
}

static bool pointers_dont_overlap(const void *a, size_t sa,
                                   const void *b, size_t sb)
{
    uintptr_t ea = (uintptr_t)a + sa;
    uintptr_t eb = (uintptr_t)b + sb;
    return (uintptr_t)b >= ea || (uintptr_t)a >= eb;
}

/* ── Test 1: Basic init / destroy ─────────────────────────────────────────── */

static void test_init_destroy(void)
{
    SECTION("1. Init / Destroy");

    Arena a;
    bool ok = arena_init(&a, 0);
    CHECK(ok);
    CHECK(a.base      != NULL);
    CHECK(a.reserved  == ((size_t)4 << 30));  /* default 4 GiB */
    CHECK(a.committed == 0);
    CHECK(a.pos       == 0);

    arena_destroy(&a);
    CHECK(a.base     == NULL);
    CHECK(a.reserved == 0);

    /* Destroy a zero-init arena — must not crash */
    Arena z = {0};
    arena_destroy(&z);
    CHECK(true);  /* reached here */
}

/* ── Test 2: Alignment ────────────────────────────────────────────────────── */

static void test_alignment(void)
{
    SECTION("2. 16-byte alignment");

    Arena a;
    arena_init(&a, 0);

    /* Vary sizes to stress the alignment logic */
    static const size_t sizes[] = { 1, 3, 7, 8, 13, 15, 16, 17,
                                     31, 32, 63, 64, 127, 256, 1023, 4096 };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        void *p = arena_alloc(&a, sizes[i]);
        CHECK(p != NULL);
        CHECK(is_aligned(p, ARENA_ALIGN));
    }

    arena_destroy(&a);
}

/* ── Test 3: Growability (cross-page boundary) ────────────────────────────── */

static void test_growability(void)
{
    SECTION("3. Growability (lazy page commit)");

    Arena a;
    arena_init(&a, 0);

    /* Allocate 3 × 64-KiB chunks to force multiple commit operations */
    const size_t chunk = 64 * 1024;
    void *p1 = arena_alloc(&a, chunk);
    CHECK(p1 != NULL);
    CHECK(a.committed >= chunk);   /* at least one chunk committed */

    void *p2 = arena_alloc(&a, chunk);
    CHECK(p2 != NULL);

    void *p3 = arena_alloc(&a, chunk);
    CHECK(p3 != NULL);

    /* Pointers must not overlap */
    CHECK(pointers_dont_overlap(p1, chunk, p2, chunk));
    CHECK(pointers_dont_overlap(p2, chunk, p3, chunk));

    /* Actually write to the memory to prove it is accessible */
    memset(p1, 0xAA, chunk);
    memset(p2, 0xBB, chunk);
    memset(p3, 0xCC, chunk);
    CHECK(((uint8_t*)p1)[0]          == 0xAA);
    CHECK(((uint8_t*)p2)[chunk - 1]  == 0xBB);
    CHECK(((uint8_t*)p3)[chunk / 2]  == 0xCC);

    ArenaStats s = arena_stats(&a);
    printf("    committed=%zu used=%zu\n", s.committed_bytes, s.used_bytes);
    CHECK(s.used_bytes >= 3 * chunk);

    arena_destroy(&a);
}

/* ── Test 4: Snapshots ───────────────────────────────────────────────────── */

static void test_snapshots(void)
{
    SECTION("4. Snapshots");

    Arena a;
    arena_init(&a, 0);

    /* Baseline: a few small allocations */
    int   *base_a = arena_alloc(&a, sizeof(int) * 16);
    float *base_b = arena_alloc(&a, sizeof(float) * 4);
    CHECK(base_a != NULL && base_b != NULL);

    size_t pos_before_snap = a.pos;

    /* Take snapshot */
    ArenaSnapshot snap = arena_snapshot(&a);
    CHECK(snap.pos       == pos_before_snap);
    CHECK(snap.committed == a.committed);

    /* Allocate a 256-KiB slab (forces at least 4 new committed chunks) */
    size_t big = 256 * 1024;
    void *slab = arena_alloc(&a, big);
    CHECK(slab != NULL);
    CHECK(a.committed > snap.committed);   /* new pages were committed */

    memset(slab, 0x42, big);

    /* Restore snapshot — bump must roll back, pages must be returned */
    size_t committed_after_alloc = a.committed;
    arena_restore(snap);

    CHECK(a.pos       == pos_before_snap);
    CHECK(a.committed <= committed_after_alloc);   /* pages returned to OS */

    /* The baseline allocations must still be intact */
    base_a[0] = 99;
    base_b[0] = 3.14f;
    CHECK(base_a[0] == 99);

    /* Allocate again after restore — must get the same range as the slab */
    void *slab2 = arena_alloc(&a, big);
    CHECK(slab2 != NULL);
    CHECK(slab2 == slab);   /* same VA — bump reset to same position */

    /* Nested snapshots */
    ArenaSnapshot s1 = arena_snapshot(&a);
    void *n1 = arena_alloc(&a, 64);
    ArenaSnapshot s2 = arena_snapshot(&a);
    void *n2 = arena_alloc(&a, 64);

    CHECK(n1 != NULL && n2 != NULL);
    CHECK(s2.pos > s1.pos);

    arena_restore(s2);
    CHECK(a.pos == s2.pos);

    arena_restore(s1);
    CHECK(a.pos == s1.pos);

    arena_destroy(&a);
}

/* ── Test 5: Reset ───────────────────────────────────────────────────────── */

static void test_reset(void)
{
    SECTION("5. Reset");

    Arena a;
    arena_init(&a, 0);

    arena_alloc(&a, 1024 * 1024);  /* trigger commits */
    CHECK(a.committed > 0);

    arena_reset(&a);
    CHECK(a.pos       == 0);
    CHECK(a.committed == 0);   /* all pages decommitted */
    CHECK(a.base      != NULL);   /* VA reservation kept */

    /* Reuse after reset */
    void *p = arena_alloc(&a, 100);
    CHECK(p != NULL);
    CHECK(is_aligned(p, ARENA_ALIGN));

    arena_destroy(&a);
}

/* ── Test 6: Thread-local arena ──────────────────────────────────────────── */

typedef struct {
    int    thread_id;
    Arena *expected_arena;   /* for verification */
    bool   ok;
} TlThreadArg;

static int tl_worker(void *arg_)
{
    TlThreadArg *arg = (TlThreadArg*)arg_;

    /* Each thread gets its own arena */
    Arena *tla = arena_tl_get();
    arg->ok = (tla != NULL);
    if (!arg->ok) return 1;

    /* Must be different from other threads' arenas */
    arg->expected_arena = tla;

    /* Allocate and write — no locking needed */
    int *vals = arena_tl_alloc(sizeof(int) * 64);
    if (!vals) { arg->ok = false; return 1; }

    for (int i = 0; i < 64; ++i) vals[i] = arg->thread_id * 1000 + i;

    /* Snapshot → extra alloc → restore */
    ArenaSnapshot snap = arena_tl_snapshot();
    char *tmp = arena_tl_alloc(512);
    memset(tmp, (unsigned char)arg->thread_id, 512);
    arena_restore(snap);

    /* Verify the original vals are unchanged */
    arg->ok = (vals[63] == arg->thread_id * 1000 + 63);

    arena_tl_destroy();
    return 0;
}

static void test_thread_local(void)
{
    SECTION("6. Thread-local arena");

    /* Main thread's TL arena */
    Arena *main_tl = arena_tl_get();
    CHECK(main_tl != NULL);

    const int N = 4;
    thrd_t      threads[4];
    TlThreadArg args[4];

    for (int i = 0; i < N; ++i) {
        args[i].thread_id      = i + 1;
        args[i].expected_arena = NULL;
        args[i].ok             = false;
        thrd_create(&threads[i], tl_worker, &args[i]);
    }

    int result;
    for (int i = 0; i < N; ++i) {
        thrd_join(threads[i], &result);
        CHECK(result == 0);
        CHECK(args[i].ok);
        /* Each thread's arena must be distinct from main's */
        CHECK(args[i].expected_arena != main_tl);
    }

    /* Also verify all thread arenas were distinct from each other */
    for (int i = 0; i < N; ++i)
        for (int j = i + 1; j < N; ++j)
            CHECK(args[i].expected_arena != args[j].expected_arena);

    arena_tl_destroy();
}

/* ── Test 7: Cyclic stress ───────────────────────────────────────────────── */

static void test_stress_cyclic(void)
{
    SECTION("7. Stress: cyclic snapshot / restore");

    Arena a;
    arena_init(&a, 0);

    const int OUTER = 10;
    const int INNER = 1000;

    for (int o = 0; o < OUTER; ++o) {
        ArenaSnapshot outer_snap = arena_snapshot(&a);

        /* Each inner iteration: allocate 4 KiB, write, restore */
        for (int i = 0; i < INNER; ++i) {
            ArenaSnapshot inner_snap = arena_snapshot(&a);

            uint8_t *buf = arena_alloc(&a, 4096);
            assert(buf);
            memset(buf, (uint8_t)(o * INNER + i), 4096);

            arena_restore(inner_snap);
        }

        arena_restore(outer_snap);
    }

    /* After all cycles the arena should be back to an empty state */
    CHECK(a.pos == 0);

    /* Allocate after stress — must still work */
    void *p = arena_alloc(&a, 128);
    CHECK(p != NULL);
    CHECK(is_aligned(p, ARENA_ALIGN));

    arena_destroy(&a);
    printf("    stress test completed: %d × %d cycles\n", OUTER, INNER);
}

/* ── Test 8: Diagnostics ─────────────────────────────────────────────────── */

static void test_stats(void)
{
    SECTION("8. Diagnostics / stats");

    Arena a;
    arena_init(&a, 0);

    ArenaStats s0 = arena_stats(&a);
    CHECK(s0.reserved_bytes  == (size_t)4 << 30);
    CHECK(s0.committed_bytes == 0);
    CHECK(s0.used_bytes      == 0);

    arena_alloc(&a, 1);   /* tiny alloc forces one commit chunk */
    ArenaStats s1 = arena_stats(&a);
    CHECK(s1.committed_bytes >= ARENA_COMMIT_CHUNK);
    /* pos advances by the raw `size`, alignment is applied to the *start*
     * of each alloc, so used_bytes is the actual bump position (== 1 here). */
    CHECK(s1.used_bytes == 1);
    CHECK(s1.wasted_bytes    == s1.committed_bytes - s1.used_bytes);
    CHECK(s1.commit_ratio    > 0.0 && s1.commit_ratio < 1.0);

    printf("    reserved=%zu MiB  committed=%zu KiB  used=%zu B  ratio=%.6f%%\n",
           s1.reserved_bytes >> 20,
           s1.committed_bytes >> 10,
           s1.used_bytes,
           s1.commit_ratio * 100.0);

    arena_destroy(&a);
}

/* ── Test 9: Edge cases ──────────────────────────────────────────────────── */

static void test_edge_cases(void)
{
    SECTION("9. Edge cases");

    Arena a;
    arena_init(&a, 0);

    /* Zero-size allocation: must return non-NULL, aligned pointer */
    void *z = arena_alloc(&a, 0);
    CHECK(z != NULL);
    CHECK(is_aligned(z, ARENA_ALIGN));

    /* Two consecutive zero-size allocs must return distinct pointers */
    void *z2 = arena_alloc(&a, 0);
    CHECK(z2 != NULL);
    CHECK(z2 != z);

    /* Allocate exactly one byte and verify it sits at an aligned address */
    void *one = arena_alloc(&a, 1);
    CHECK(one != NULL);
    CHECK(is_aligned(one, ARENA_ALIGN));

    /* Very small custom reservation */
    Arena small;
    bool ok = arena_init(&small, ARENA_COMMIT_CHUNK);
    CHECK(ok);
    void *s = arena_alloc(&small, ARENA_COMMIT_CHUNK);
    CHECK(s != NULL);
    /* Next alloc must fail — reservation exhausted */
    void *s2 = arena_alloc(&small, 1);
    CHECK(s2 == NULL);
    arena_destroy(&small);

    /* Snapshot on an arena that already has a few bytes used */
    size_t pos_at_snap = a.pos;
    ArenaSnapshot snap = arena_snapshot(&a);
    arena_alloc(&a, 1024);
    arena_restore(snap);
    CHECK(a.pos == pos_at_snap);   /* restored to exactly the snapshot pos */

    arena_destroy(&a);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf(ANSI_CYAN
           "╔══════════════════════════════════════════════════╗\n"
           "║  Growable Linear Allocator  —  Test Suite        ║\n"
           "╚══════════════════════════════════════════════════╝"
           ANSI_RESET "\n");

    test_init_destroy();
    test_alignment();
    test_growability();
    test_snapshots();
    test_reset();
    test_thread_local();
    test_stress_cyclic();
    test_stats();
    test_edge_cases();

    printf("\n──────────────────────────────────────────────────\n");
    printf("Results: %d / %d passed", g_passed, g_tests);

    if (g_failed == 0) {
        printf("  " ANSI_GREEN "All tests passed ✓" ANSI_RESET "\n");
        return EXIT_SUCCESS;
    } else {
        printf("  " ANSI_RED "%d FAILED ✗" ANSI_RESET "\n", g_failed);
        return EXIT_FAILURE;
    }
}
