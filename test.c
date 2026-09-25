#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void* checked_malloc(size_t sz);
void assert_failed(const char* cond, const char* file, int line);

#define ARENA_IMPLEMENTATION
#define ARENA_MALLOC(sz) checked_malloc(sz)
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

static int is_aligned(const void* p, size_t align)
{
    return ((uintptr_t)p & (align - 1)) == 0;
}

void test_make()
{
    arena a = arena_make(64);

    CHECK(a.data != NULL);
    CHECK(a.used == 0);
    CHECK(a.capacity == 64);

    arena_free(&a);
    CHECK(a.data == NULL);
    CHECK(a.used == 0);
    CHECK(a.capacity == 0);
}

void test_make_zero()
{
    /* Nothing to allocate, so malloc(0) is never called */
    int calls = malloc_calls;
    arena a = arena_make(0);

    CHECK(malloc_calls == calls);
    CHECK(a.data == NULL);
    CHECK(a.capacity == 0);
    CHECK(arena_alloc(&a, int, 1) == NULL);

    arena_free(&a);
}

void test_make_failure()
{
    fail_malloc = 1;
    arena a = arena_make(64);
    fail_malloc = 0;

    CHECK(a.data == NULL);
    CHECK(a.used == 0);
    CHECK(a.capacity == 0);

    /* The failed arena can still be allocated from, which always fails */
    CHECK(arena_alloc(&a, int, 1) == NULL);
    CHECK(a.used == 0);

    arena_free(&a);
}

void test_alloc()
{
    arena a = arena_make(64);

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
    arena a = arena_make(64);

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
    arena a = arena_make(64);

    CHECK(arena_alloc(&a, int, 0) == NULL);
    CHECK(a.used == 0);

    arena_free(&a);
}

void test_alloc_alignment()
{
    arena a = arena_make(256);

    /* Throw the offset off by one, the double must still be aligned */
    char* c = arena_alloc(&a, char, 1);
    double* d = arena_alloc(&a, double, 1);
    CHECK(c != NULL);
    CHECK(d != NULL);
    CHECK(is_aligned(d, _Alignof(double)));
    CHECK((uint8_t*)d - (uint8_t*)c == _Alignof(double));

    /* Alignment larger than malloc() guarantees */
    arena_alloc(&a, char, 1);
    void* p = arena_alloc_(&a, 1, 1, 64);
    CHECK(p != NULL);
    CHECK(is_aligned(p, 64));

    arena_free(&a);
}

void test_alloc_bad_alignment()
{
    arena a = arena_make(64);

    CHECK_ASSERTS(arena_alloc_(&a, 1, 1, 0));
    CHECK_ASSERTS(arena_alloc_(&a, 1, 1, 3));
    CHECK(a.used == 0);

    arena_free(&a);
}

void test_alloc_out_of_space()
{
    arena a = arena_make(4 * sizeof(int));

    /* Exactly fills the arena */
    int* p = arena_alloc(&a, int, 4);
    CHECK(p != NULL);
    CHECK(a.used == a.capacity);

    /* Full, the allocation must fail and leave the arena unchanged */
    CHECK(arena_alloc(&a, int, 1) == NULL);
    CHECK(a.used == a.capacity);

    arena_free(&a);
}

void test_alloc_out_of_space_padding()
{
    /* The block from malloc() is aligned for double */
    arena a = arena_make(16);

    arena_alloc(&a, char, 1);

    /* 7 bytes of padding + 16 bytes does not fit in the 15 bytes left */
    CHECK(arena_alloc(&a, double, 2) == NULL);
    CHECK(a.used == 1);

    /* 7 bytes of padding + 8 bytes fits exactly */
    CHECK(arena_alloc(&a, double, 1) != NULL);
    CHECK(a.used == 16);

    arena_free(&a);
}

void test_alloc_size_overflow()
{
    arena a = arena_make(64);

    /* elemsize * count would wrap around to a tiny size */
    CHECK(arena_alloc_(&a, SIZE_MAX / 2 + 1, 2, 1) == NULL);
    CHECK(a.used == 0);

    arena_free(&a);
}

void test_reset()
{
    arena a = arena_make(64);

    int* p = arena_alloc(&a, int, 16);
    CHECK(p != NULL);
    CHECK(arena_alloc(&a, int, 1) == NULL);

    /* The block is kept and reused from the start */
    uint8_t* data = a.data;
    arena_reset(&a);
    CHECK(a.data == data);
    CHECK(a.used == 0);
    CHECK(a.capacity == 64);
    CHECK(arena_alloc(&a, int, 16) == p);

    arena_free(&a);
}

typedef struct {
    int x;
    double y;
} point;

void test_alloc_struct()
{
    arena a = arena_make(256);

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

int main()
{
    printf("Testing arena_make() and arena_free()\n");
    test_make();
    test_make_zero();
    test_make_failure();

    printf("Testing arena_alloc()\n");
    test_alloc();
    test_alloc_zero_init();
    test_alloc_zero_count();
    test_alloc_alignment();
    test_alloc_bad_alignment();
    test_alloc_out_of_space();
    test_alloc_out_of_space_padding();
    test_alloc_size_overflow();
    test_alloc_struct();

    printf("Testing arena_reset()\n");
    test_reset();

    if (failures) {
        printf("=== ARENA TESTS FAILED: %d check(s) ===\n", failures);
        return 1;
    }

    printf("=== ARENA TESTS COMPLETED ===\n");

    return 0;
}
