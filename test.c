#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void* checked_malloc(size_t sz);
void checked_free(void* p);
void assert_failed(const char* cond, const char* file, int line);

#define ARENA_IMPLEMENTATION
#define ARENA_MALLOC(sz) checked_malloc(sz)
#define ARENA_FREE(p) checked_free(p)
#define ARENA_ASSERT(cond)                                                     \
    ((cond) ? (void)0 : assert_failed(#cond, __FILE__, __LINE__))
#include "arena.h"

static int failures = 0;

/* Unlike assert(), this is not compiled away with NDEBUG */
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  %s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* Check that stmt trips an ARENA_ASSERT() */
static jmp_buf assert_jmp;
static int expect_assert = 0;

#define CHECK_ASSERTS(stmt)                                                    \
    do {                                                                       \
        expect_assert = 1;                                                     \
        if (setjmp(assert_jmp) == 0) {                                         \
            stmt;                                                              \
            printf("  %s:%d: expected assertion: %s\n", __FILE__, __LINE__,    \
                   #stmt);                                                     \
            failures++;                                                        \
        }                                                                      \
        expect_assert = 0;                                                     \
    } while (0)

void assert_failed(const char* cond, const char* file, int line)
{
    if (expect_assert) {
        longjmp(assert_jmp, 1);
    }
    printf("%s:%d: assertion failed: %s\n", file, line, cond);
    abort();
}

/* When fail_malloc is set, the allocation returns NULL without touching any
 * memory, so allocation failures can be tested */
static int fail_malloc = 0;
static int malloc_calls = 0;

void* checked_malloc(size_t sz)
{
    malloc_calls++;
    if (fail_malloc)
        return NULL;
    void* p = malloc(sz);
    if (!p)
        printf("Unable to allocate memory\n");
    return p;
}

static int free_calls = 0;

void checked_free(void* p)
{
    if (p)
        free_calls++;
    free(p);
}

static int is_aligned(const void* p, size_t align)
{
    return ((uintptr_t)p & (align - 1)) == 0;
}

static uint8_t* block_data(arena_block* b)
{
    return (uint8_t*)(b + 1);
}

void test_make()
{
    arena a = arena_make(64, 0);

    CHECK(a.block != NULL);
    CHECK(a.block->prev == NULL);
    CHECK(a.block->capacity == 64);
    CHECK(a.used == 0);
    CHECK(a.blocksize == 64);
    CHECK(a.limit == 0);
    CHECK(a.total == 64);
    CHECK(a.peak == 64);

    int frees = free_calls;
    arena_free(&a);
    CHECK(free_calls == frees + 1);
    CHECK(a.block == NULL);
    CHECK(a.used == 0);
    CHECK(a.blocksize == 0);
    CHECK(a.total == 0);
    CHECK(a.peak == 0);
}

void test_make_zero()
{
    /* Nothing to allocate up front, so malloc(0) is never called */
    int calls = malloc_calls;
    arena a = arena_make(0, 0);

    CHECK(malloc_calls == calls);
    CHECK(a.block == NULL);
    CHECK(a.total == 0);

    /* Every allocation gets a block of its own, sized to fit */
    int* p = arena_alloc(&a, int, 4);
    CHECK(p != NULL);
    CHECK(malloc_calls == calls + 1);
    CHECK(a.total == 4 * sizeof(int) + _Alignof(int) - 1);

    arena_free(&a);
}

void test_make_failure()
{
    fail_malloc = 1;
    arena a = arena_make(64, 0);
    fail_malloc = 0;

    CHECK(a.block == NULL);
    CHECK(a.used == 0);
    CHECK(a.blocksize == 64);
    CHECK(a.total == 0);
    CHECK(a.peak == 0);

    /* The failed arena can still be allocated from, which tries again */
    int* p = arena_alloc(&a, int, 1);
    CHECK(p != NULL);
    CHECK(a.block != NULL);
    CHECK(a.total == 64);

    arena_free(&a);
}

void test_make_over_limit()
{
    /* The first block alone would exceed the limit */
    int calls = malloc_calls;
    arena a = arena_make(128, 64);

    CHECK(malloc_calls == calls);
    CHECK(a.block == NULL);
    CHECK(a.total == 0);

    arena_free(&a);
}

void test_make_size_overflow()
{
    /* The block header would not fit on top of the capacity */
    int calls = malloc_calls;
    arena a = arena_make(SIZE_MAX, 0);

    CHECK(malloc_calls == calls);
    CHECK(a.block == NULL);

    arena_free(&a);
}

void test_alloc()
{
    arena a = arena_make(64, 0);

    int* p = arena_alloc(&a, int, 4);
    CHECK(p != NULL);
    CHECK(a.used == 4 * sizeof(int));

    int* q = arena_alloc(&a, int, 4);
    CHECK(q != NULL);
    CHECK(a.used == 8 * sizeof(int));

    /* Consecutive allocations of the same type are packed back to back */
    CHECK(q == p + 4);

    for (int i = 0; i < 4; i++) {
        p[i] = i;
        q[i] = 10 + i;
    }
    for (int i = 0; i < 4; i++) {
        CHECK(p[i] == i);
        CHECK(q[i] == 10 + i);
    }

    arena_free(&a);
}

void test_alloc_zero_init()
{
    arena a = arena_make(64, 0);

    uint8_t* p = arena_alloc(&a, uint8_t, 64);
    memset(p, 0xff, 64);

    /* Memory handed out again after a reset is zeroed, not left dirty */
    arena_reset(&a);
    uint8_t* q = arena_alloc(&a, uint8_t, 64);
    CHECK(q == p);
    for (int i = 0; i < 64; i++) {
        CHECK(q[i] == 0);
    }

    arena_free(&a);
}

void test_alloc_zero_count()
{
    arena a = arena_make(64, 0);

    CHECK(arena_alloc(&a, int, 0) == NULL);
    CHECK(a.used == 0);

    arena_free(&a);
}

void test_alloc_alignment()
{
    arena a = arena_make(256, 0);

    /* Throw the offset off by one, the double must still be aligned */
    char* c = arena_alloc(&a, char, 1);
    double* d = arena_alloc(&a, double, 1);
    CHECK(c != NULL);
    CHECK(d != NULL);
    CHECK(is_aligned(d, _Alignof(double)));

    /* Alignment larger than malloc() guarantees */
    arena_alloc(&a, char, 1);
    void* p = arena_alloc_(&a, 1, 1, 64);
    CHECK(p != NULL);
    CHECK(is_aligned(p, 64));

    arena_free(&a);
}

void test_alloc_bad_alignment()
{
    arena a = arena_make(64, 0);

    CHECK_ASSERTS(arena_alloc_(&a, 1, 1, 0));
    CHECK_ASSERTS(arena_alloc_(&a, 1, 1, 3));
    CHECK(a.used == 0);

    arena_free(&a);
}

void test_alloc_size_overflow()
{
    arena a = arena_make(64, 0);
    int calls = malloc_calls;

    /* elemsize * count would wrap around to a tiny size */
    CHECK(arena_alloc_(&a, SIZE_MAX / 2 + 1, 2, 1) == NULL);

    /* The size fits, but not with room for the padding */
    CHECK(arena_alloc_(&a, SIZE_MAX, 1, 16) == NULL);

    CHECK(malloc_calls == calls);
    CHECK(a.used == 0);
    CHECK(a.total == 64);

    arena_free(&a);
}

typedef struct {
    int x;
    double y;
} point;

void test_alloc_struct()
{
    arena a = arena_make(256, 0);

    arena_alloc(&a, char, 1);
    point* p = arena_alloc(&a, point, 3);
    CHECK(p != NULL);
    CHECK(is_aligned(p, _Alignof(point)));

    p[2] = (point){3, 4.5};
    CHECK(p[2].x == 3);
    CHECK(p[2].y == 4.5);
    CHECK(p[0].x == 0);
    CHECK(p[0].y == 0.0);

    arena_free(&a);
}

void test_grow()
{
    arena a = arena_make(4 * sizeof(int), 0);
    arena_block* first = a.block;

    /* Exactly fills the first block */
    int* p = arena_alloc(&a, int, 4);
    CHECK(p != NULL);
    CHECK(a.used == 4 * sizeof(int));
    for (int i = 0; i < 4; i++) {
        p[i] = i;
    }

    /* The first block is full, so a new block is added in front of it */
    int calls = malloc_calls;
    int* q = arena_alloc(&a, int, 1);
    CHECK(q != NULL);
    CHECK(malloc_calls == calls + 1);
    CHECK(a.block != first);
    CHECK(a.block->prev == first);
    CHECK((uint8_t*)q == block_data(a.block));
    CHECK(a.used == sizeof(int));
    CHECK(a.total == 8 * sizeof(int));
    CHECK(a.peak == 8 * sizeof(int));

    /* Keep growing, earlier allocations must never move */
    for (int i = 0; i < 100; i++) {
        CHECK(arena_alloc(&a, int, 3) != NULL);
    }
    for (int i = 0; i < 4; i++) {
        CHECK(p[i] == i);
    }

    arena_free(&a);
}

void test_grow_leftover()
{
    arena a = arena_make(64, 0);
    arena_block* first = a.block;

    arena_alloc(&a, uint8_t, 40);

    /* 32 bytes do not fit in the 24 left, and are never split. The leftover
     * of the first block is not used again. */
    uint8_t* p = arena_alloc(&a, uint8_t, 32);
    CHECK(p == block_data(a.block));
    CHECK(a.block->prev == first);
    CHECK(a.block->capacity == 64);
    CHECK(a.used == 32);

    CHECK(arena_alloc(&a, uint8_t, 8) == p + 32);

    arena_free(&a);
}

void test_grow_failure()
{
    arena a = arena_make(16, 0);
    arena_alloc(&a, uint8_t, 16);

    arena_block* block = a.block;
    fail_malloc = 1;
    void* p = arena_alloc(&a, uint8_t, 1);
    void* q = arena_alloc(&a, uint8_t, 100);
    fail_malloc = 0;

    /* Nothing was changed by the failed calls */
    CHECK(p == NULL);
    CHECK(q == NULL);
    CHECK(a.block == block);
    CHECK(a.block->prev == NULL);
    CHECK(a.used == 16);
    CHECK(a.total == 16);
    CHECK(a.peak == 16);

    arena_free(&a);
}

void test_oversized()
{
    arena a = arena_make(64, 0);
    arena_block* first = a.block;

    int* p = arena_alloc(&a, int, 1);

    /* Larger than a block, it gets a block of its own, sized to fit */
    int* big = arena_alloc(&a, int, 100);
    CHECK(big != NULL);
    CHECK(a.total == 64 + 100 * sizeof(int) + _Alignof(int) - 1);

    /* The oversized block goes behind the current block, which keeps being
     * used for the smaller allocations that follow */
    CHECK(a.block == first);
    CHECK(a.used == sizeof(int));
    CHECK(first->prev != NULL);
    CHECK((uint8_t*)big == block_data(first->prev));
    CHECK(arena_alloc(&a, int, 1) == p + 1);

    /* The whole allocation is usable */
    for (int i = 0; i < 100; i++) {
        CHECK(big[i] == 0);
        big[i] = i;
    }
    CHECK(big[99] == 99);

    arena_free(&a);
}

void test_oversized_boundary()
{
    arena a = arena_make(64, 0);
    arena_alloc(&a, uint8_t, 64);

    /* Exactly one block, with no padding needed, uses a regular block */
    uint8_t* p = arena_alloc(&a, uint8_t, 64);
    CHECK(p == block_data(a.block));
    CHECK(a.block->capacity == 64);

    /* Exactly one block, but a new block might need padding before it, so it
     * gets a block of its own with room for that */
    arena_block* block = a.block;
    int* q = arena_alloc(&a, int, 16);
    CHECK(q != NULL);
    CHECK(a.block == block);
    CHECK(block->prev->capacity == 16 * sizeof(int) + _Alignof(int) - 1);

    arena_free(&a);
}

void test_limit()
{
    arena a = arena_make(64, 128);

    /* Fill two blocks, which reaches the limit exactly */
    CHECK(arena_alloc(&a, uint8_t, 64) != NULL);
    CHECK(arena_alloc(&a, uint8_t, 64) != NULL);
    CHECK(a.total == 128);

    /* A third block would exceed the limit, the allocation must fail before
     * anything is allocated, and leave the arena unchanged */
    arena_block* block = a.block;
    int calls = malloc_calls;
    CHECK(arena_alloc(&a, uint8_t, 1) == NULL);
    CHECK(malloc_calls == calls);
    CHECK(a.block == block);
    CHECK(a.used == 64);
    CHECK(a.total == 128);

    arena_free(&a);
}

void test_limit_oversized()
{
    arena a = arena_make(64, 256);

    /* Fits within the limit on its own */
    CHECK(arena_alloc(&a, uint8_t, 128) != NULL);
    CHECK(a.total == 64 + 128);

    /* Would take the total past the limit */
    CHECK(arena_alloc(&a, uint8_t, 128) == NULL);
    CHECK(a.total == 64 + 128);

    /* Space left in the current block can still be used */
    CHECK(arena_alloc(&a, uint8_t, 64) != NULL);

    arena_free(&a);
}

void test_reset()
{
    arena a = arena_make(64, 0);

    /* Grow to four blocks, one of them oversized */
    arena_alloc(&a, uint8_t, 64);
    arena_alloc(&a, uint8_t, 64);
    arena_alloc(&a, uint8_t, 200);
    arena_alloc(&a, uint8_t, 64);
    size_t peak = a.total;
    CHECK(peak == 3 * 64 + 200);
    CHECK(a.peak == peak);

    /* Only one block of the regular size is kept */
    int frees = free_calls;
    arena_reset(&a);
    CHECK(free_calls == frees + 3);
    CHECK(a.block != NULL);
    CHECK(a.block->prev == NULL);
    CHECK(a.block->capacity == 64);
    CHECK(a.used == 0);
    CHECK(a.total == 64);
    CHECK(a.peak == peak);

    /* The kept block is reused from the start, without a new block */
    int calls = malloc_calls;
    uint8_t* p = arena_alloc(&a, uint8_t, 64);
    CHECK(p == block_data(a.block));
    CHECK(malloc_calls == calls);

    arena_free(&a);
}

void test_reset_oversized_oldest()
{
    arena a = arena_make(64, 0);
    arena_block* first = a.block;

    /* The oversized block goes behind the only block, so it is the oldest */
    arena_alloc(&a, uint8_t, 200);
    CHECK(a.block == first);
    CHECK(first->prev != NULL);
    CHECK(first->prev->prev == NULL);

    /* Used to keep the oldest block, which was the oversized one */
    arena_reset(&a);
    CHECK(a.block == first);
    CHECK(a.block->prev == NULL);
    CHECK(a.total == 64);

    arena_free(&a);
}

void test_reset_no_regular_block()
{
    /* With blocksize 0, every block is sized for its allocation, and none of
     * them are kept */
    arena a = arena_make(0, 0);
    arena_alloc(&a, int, 4);
    arena_alloc(&a, int, 4);

    int frees = free_calls;
    arena_reset(&a);
    CHECK(free_calls == frees + 2);
    CHECK(a.block == NULL);
    CHECK(a.total == 0);

    /* Still usable afterwards */
    CHECK(arena_alloc(&a, int, 4) != NULL);

    arena_free(&a);
}

void test_reset_empty()
{
    arena a = arena_make(0, 0);

    arena_reset(&a);
    CHECK(a.block == NULL);
    CHECK(a.used == 0);
    CHECK(a.total == 0);

    arena_free(&a);
}

void test_rewind()
{
    arena a = arena_make(64, 0);

    int* p = arena_alloc(&a, int, 2);
    p[0] = 1;
    p[1] = 2;

    arena_mark m = arena_save(&a);
    CHECK(m.block == a.block);
    CHECK(m.used == 2 * sizeof(int));

    int* q = arena_alloc(&a, int, 4);
    memset(q, 0xff, 4 * sizeof(int));

    arena_rewind(&a, m);
    CHECK(a.used == 2 * sizeof(int));

    /* Allocations from before the mark are kept */
    CHECK(p[0] == 1);
    CHECK(p[1] == 2);

    /* The rewound memory is handed out again, zeroed */
    int* r = arena_alloc(&a, int, 4);
    CHECK(r == q);
    for (int i = 0; i < 4; i++) {
        CHECK(r[i] == 0);
    }

    arena_free(&a);
}

void test_rewind_frees_blocks()
{
    arena a = arena_make(64, 0);
    arena_block* first = a.block;

    arena_alloc(&a, uint8_t, 8);
    arena_mark m = arena_save(&a);

    /* Grow to four blocks, one of them oversized */
    arena_alloc(&a, uint8_t, 64);
    arena_alloc(&a, uint8_t, 64);
    arena_alloc(&a, uint8_t, 200);
    arena_alloc(&a, uint8_t, 64);
    size_t peak = a.total;

    int frees = free_calls;
    arena_rewind(&a, m);
    CHECK(free_calls == frees + 4);
    CHECK(a.block == first);
    CHECK(a.block->prev == NULL);
    CHECK(a.used == 8);
    CHECK(a.total == 64);
    CHECK(a.peak == peak);

    arena_free(&a);
}

void test_rewind_oversized_behind()
{
    arena a = arena_make(64, 0);
    arena_block* first = a.block;

    /* Grow to two blocks, so the marked block has one behind it */
    arena_alloc(&a, uint8_t, 64);
    arena_alloc(&a, uint8_t, 8);
    arena_block* second = a.block;
    CHECK(second->prev == first);

    arena_mark m = arena_save(&a);

    /* Oversized blocks go directly behind the marked block, which stays
     * current, so they are not in front of it */
    arena_alloc(&a, uint8_t, 200);
    arena_alloc(&a, uint8_t, 300);
    CHECK(a.block == second);
    CHECK(second->prev != first);

    int frees = free_calls;
    arena_rewind(&a, m);
    CHECK(free_calls == frees + 2);
    CHECK(a.block == second);
    CHECK(second->prev == first);
    CHECK(a.used == 8);
    CHECK(a.total == 2 * 64);

    arena_free(&a);
}

void test_rewind_nested()
{
    arena a = arena_make(64, 0);

    arena_alloc(&a, uint8_t, 8);
    arena_mark outer = arena_save(&a);

    arena_alloc(&a, uint8_t, 40);
    arena_mark inner = arena_save(&a);

    arena_alloc(&a, uint8_t, 100);
    arena_alloc(&a, uint8_t, 40);

    /* Rewind the inner mark, and reuse it */
    arena_rewind(&a, inner);
    CHECK(a.used == 48);
    CHECK(a.total == 64);

    arena_alloc(&a, uint8_t, 40);
    arena_rewind(&a, inner);
    CHECK(a.used == 48);
    CHECK(a.total == 64);

    /* Rewinding the outer mark also frees what the inner mark kept */
    arena_rewind(&a, outer);
    CHECK(a.used == 8);
    CHECK(a.total == 64);

    arena_free(&a);
}

void test_rewind_empty()
{
    /* A mark saved before the arena has any block rewinds to no blocks */
    arena a = arena_make(0, 0);
    arena_mark m = arena_save(&a);
    CHECK(m.block == NULL);

    arena_alloc(&a, int, 4);
    arena_alloc(&a, int, 4);

    int frees = free_calls;
    arena_rewind(&a, m);
    CHECK(free_calls == frees + 2);
    CHECK(a.block == NULL);
    CHECK(a.used == 0);
    CHECK(a.total == 0);

    arena_free(&a);
}

void test_rewind_invalid()
{
    arena a = arena_make(64, 0);

    arena_mark outer = arena_save(&a);
    arena_alloc(&a, uint8_t, 40);

    /* Rewinding to the outer mark moves back past the inner mark, within the
     * same block */
    arena_mark inner = arena_save(&a);
    arena_rewind(&a, outer);
    CHECK_ASSERTS(arena_rewind(&a, inner));
    CHECK(a.used == 0);

    /* The outer mark frees the block the inner mark was saved in */
    arena_alloc(&a, uint8_t, 64);
    arena_alloc(&a, uint8_t, 8);
    inner = arena_save(&a);
    arena_rewind(&a, outer);
    CHECK_ASSERTS(arena_rewind(&a, inner));

    /* A mark from another arena */
    arena b = arena_make(64, 0);
    arena_mark other = arena_save(&b);
    CHECK_ASSERTS(arena_rewind(&a, other));
    arena_free(&b);

    /* Reset drops the block that was behind the marked block */
    arena_alloc(&a, uint8_t, 64);
    arena_alloc(&a, uint8_t, 8);
    arena_mark m = arena_save(&a);
    arena_reset(&a);
    arena_alloc(&a, uint8_t, 200);
    CHECK_ASSERTS(arena_rewind(&a, m));

    /* Nothing was changed by the failed calls */
    CHECK(a.block->prev != NULL);
    CHECK(a.total == 64 + 200);

    arena_free(&a);
}

void test_free_blocks()
{
    arena a = arena_make(16, 0);

    for (int i = 0; i < 10; i++) {
        arena_alloc(&a, uint8_t, 16);
    }

    int frees = free_calls;
    arena_free(&a);
    CHECK(free_calls == frees + 10);
}

int main()
{
    printf("Testing arena_make() and arena_free()\n");
    test_make();
    test_make_zero();
    test_make_failure();
    test_make_over_limit();
    test_make_size_overflow();
    test_free_blocks();

    printf("Testing arena_alloc()\n");
    test_alloc();
    test_alloc_zero_init();
    test_alloc_zero_count();
    test_alloc_alignment();
    test_alloc_bad_alignment();
    test_alloc_size_overflow();
    test_alloc_struct();

    printf("Testing arena_alloc() growth\n");
    test_grow();
    test_grow_leftover();
    test_grow_failure();
    test_oversized();
    test_oversized_boundary();
    test_limit();
    test_limit_oversized();

    printf("Testing arena_reset()\n");
    test_reset();
    test_reset_oversized_oldest();
    test_reset_no_regular_block();
    test_reset_empty();

    printf("Testing arena_save() and arena_rewind()\n");
    test_rewind();
    test_rewind_frees_blocks();
    test_rewind_oversized_behind();
    test_rewind_nested();
    test_rewind_empty();
    test_rewind_invalid();

    if (failures) {
        printf("=== ARENA TESTS FAILED: %d check(s) ===\n", failures);
        return 1;
    }

    printf("=== ARENA TESTS COMPLETED ===\n");

    return 0;
}
