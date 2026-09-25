/* Copyright 2026 Rikard Olajos
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Lets GCC and Clang check format strings against their arguments */
#if defined(__GNUC__) || defined(__clang__)
#define ARENA_PRINTF_(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define ARENA_PRINTF_(fmt, args)
#endif

#ifndef ARENA_MALLOC
#define ARENA_MALLOC(sz) malloc(sz)
#endif

#ifndef ARENA_FREE
#define ARENA_FREE(p) free(p)
#endif

#ifndef ARENA_ASSERT
#define ARENA_ASSERT(cond) assert(cond)
#endif

/* When ARENA_POISON is 1, memory given back by arena_reset(), arena_rewind()
 * and arena_free() is filled with ARENA_POISON_BYTE, so that reading through a
 * stale pointer gives obvious garbage instead of the old data. Like assert(),
 * it is on unless NDEBUG is defined.
 */
#ifndef ARENA_POISON
#ifdef NDEBUG
#define ARENA_POISON 0
#else
#define ARENA_POISON 1
#endif
#endif

#ifndef ARENA_POISON_BYTE
#define ARENA_POISON_BYTE 0xdd
#endif

/* A block of memory in an arena. The data follows directly after this header
 * in the same allocation. Blocks are chained from the newest to the oldest.
 */
typedef struct arena_block arena_block;
struct arena_block {
    arena_block* prev; /* Previous (older) block, NULL for the oldest */
    size_t capacity;   /* Capacity of the data in bytes */
};

/* A memory arena. Allocations are made by bumping an offset into the current
 * block, and a new block is added when it is full. Allocations are never moved,
 * and are all freed at once with arena_reset() or arena_free(). The fields
 * should not be written to by the user.
 *
 * Capacities count the data of the blocks, not the small header of each block.
 */
typedef struct {
    arena_block* block; /* Current block, NULL if there is none */
    size_t used;        /* Bytes used in the current block */
    size_t blocksize;   /* Capacity of new blocks in bytes */
    size_t limit;       /* Maximum total capacity in bytes, 0 for no limit */
    size_t total;       /* Total capacity of all blocks in bytes */
    size_t peak;        /* Highest total capacity reached, kept on reset */
} arena;

/* Make a new arena, starting with one block. An arena has to be freed using
 * arena_free().
 *
 * When the current block is full a new block is added. An allocation that
 * might not fit in a new block, counting the worst case alignment padding, gets
 * a block of its own, sized to fit. An allocation is never split across blocks.
 * The total capacity never grows past limit, an allocation that would need that
 * fails instead.
 *
 * On failure block is NULL. The arena can still be allocated from (which tries
 * to add a block again) and freed.
 *
 *  blocksize   is the capacity of each block in bytes
 *  limit       is the maximum total capacity in bytes, or 0 for no limit
 */
arena arena_make(size_t blocksize, size_t limit);

/* Free all blocks of the arena and reset fields to zero. Every pointer
 * returned by arena_alloc() is invalidated.
 *
 *  a       is the arena to free
 */
void arena_free(arena* a);

/* Reset the arena so that its memory can be reused. One block of blocksize is
 * kept, if there is one, and every other block is freed, so memory used by a
 * single large peak is given back. Every pointer returned by arena_alloc() is
 * invalidated.
 *
 *  a       is the arena to reset
 */
void arena_reset(arena* a);

/* Allocate count elements of a type from the arena, aligned for that type.
 * Returns a pointer to the first element, or NULL if a new block was needed
 * and could not be added, in which case the arena is left unchanged.
 * Allocating zero elements also returns NULL. Elements are zero-initialized.
 *
 *  a       is the arena to allocate from
 *  type    is the type of the elements
 *  count   is the number of elements
 */
#define arena_alloc(a, type, count)                                            \
    ((type*)arena_alloc_((a), sizeof(type), (count), _Alignof(type)))
void* arena_alloc_(arena* a, size_t elemsize, size_t count, size_t align);

/* Copy count elements of a type into the arena, aligned for that type. Returns
 * a pointer to the copy, or NULL on failure like arena_alloc(). The source can
 * be memory from the same arena.
 *
 * The pointer type of src is checked against the type, as far as the compiler
 * warns about mismatched pointer types in a conditional expression. A void
 * pointer is not checked.
 *
 *  a       is the arena to copy into
 *  type    is the type of the elements
 *  src     is a pointer to the elements to copy
 *  count   is the number of elements
 */
#define arena_copy(a, type, src, count)                                        \
    ((type*)arena_copy_((a), (1 ? (src) : (const type*)NULL), sizeof(type),    \
                        (count), _Alignof(type)))
void* arena_copy_(arena* a, const void* src, size_t elemsize, size_t count,
                  size_t align);

/* Grow, or shrink, the last allocation of the arena in place. Returns p, or
 * NULL if p is not the last allocation in the current block or the new size
 * does not fit in it, in which case the arena is left unchanged. New elements
 * are zero-initialized.
 *
 * On failure, the elements have to be moved by the caller if more room is
 * needed, e.g. with arena_alloc() and memcpy(). The last allocation is not
 * growable if it got a block of its own.
 *
 * Growing after arena_save() counts as allocating after the mark, and is
 * undone by arena_rewind(). Shrinking below a mark invalidates it.
 *
 *  a           is the arena that p was allocated from
 *  type        is the type of the elements
 *  p           is the last allocation
 *  oldcount    is the number of elements p currently has
 *  newcount    is the new number of elements, must not be 0
 */
#define arena_grow_last(a, type, p, oldcount, newcount)                        \
    ((type*)arena_grow_last_((a), (1 ? (p) : (type*)NULL), sizeof(type),       \
                             (oldcount), (newcount)))
void* arena_grow_last_(arena* a, void* p, size_t elemsize, size_t oldcount,
                       size_t newcount);

/* Copy a null-terminated string into the arena. Returns a pointer to the copy,
 * or NULL on failure.
 *
 *  a       is the arena to copy into
 *  s       is the string to copy
 */
char* arena_strdup(arena* a, const char* s);

/* Copy at most n chars of a string into the arena, and null-terminate the
 * copy. Stops early at the terminator of s, so s only has to be null-terminated
 * if it is shorter than n. Returns a pointer to the copy, or NULL on failure.
 *
 *  a       is the arena to copy into
 *  s       is the string to copy
 *  n       is the maximum number of chars to copy, excluding the terminator
 */
char* arena_strndup(arena* a, const char* s, size_t n);

/* Format a string like sprintf() into the arena, sized to fit. Returns a
 * pointer to the string, or NULL on failure, also when the formatting fails.
 *
 *  a       is the arena to format into
 *  fmt     is the printf() format string
 */
char* arena_sprintf(arena* a, const char* fmt, ...) ARENA_PRINTF_(2, 3);

/* Format a string like vsprintf() into the arena, sized to fit. See
 * arena_sprintf().
 *
 *  a       is the arena to format into
 *  fmt     is the printf() format string
 *  args    are the arguments for the format string
 */
char* arena_vsprintf(arena* a, const char* fmt, va_list args)
    ARENA_PRINTF_(2, 0);

/* A saved position in an arena, to rewind to with arena_rewind(). The fields
 * should not be written to by the user.
 */
typedef struct {
    arena_block* block; /* Current block when saved */
    arena_block* prev;  /* Block behind it when saved */
    size_t used;        /* Bytes used in the current block when saved */
} arena_mark;

/* Save the current position of the arena, so that every allocation made after
 * it can be freed at once with arena_rewind().
 *
 * Marks can be nested. A mark is invalidated by rewinding to an earlier mark,
 * and by arena_reset() and arena_free().
 *
 *  a       is the arena to save the position of
 */
arena_mark arena_save(arena* a);

/* Rewind the arena to a position saved with arena_save(). Every allocation made
 * after it is freed, including any blocks that were added. Pointers returned by
 * arena_alloc() before the mark stay valid, pointers returned after it are
 * invalidated. The mark stays valid and can be rewound to again.
 *
 *  a       is the arena to rewind
 *  mark    is the saved position, has to be valid for this arena
 */
void arena_rewind(arena* a, arena_mark mark);

#ifdef ARENA_IMPLEMENTATION

/* Only the implementation needs vsnprintf() */
#include <stdio.h>

/* Allocate a new block with the given capacity, and count it towards the total
 * of the arena. The block is not linked into the arena. Returns NULL if the
 * limit would be exceeded or the allocation fails.
 */
static arena_block* arena_newblock_(arena* a, size_t capacity)
{
    if (capacity > SIZE_MAX - sizeof(arena_block)) {
        return NULL;
    }

    /* The total never exceeds the limit, so this cannot wrap around */
    if (a->limit != 0 && capacity > a->limit - a->total) {
        return NULL;
    }

    arena_block* b = ARENA_MALLOC(sizeof(arena_block) + capacity);
    if (!b) {
        return NULL;
    }

    b->prev = NULL;
    b->capacity = capacity;

    a->total += capacity;
    if (a->total > a->peak) {
        a->peak = a->total;
    }

    return b;
}

/* Poison size bytes of memory that is given back, if ARENA_POISON is on */
static void arena_poison_(void* p, size_t size)
{
#if ARENA_POISON
    memset(p, ARENA_POISON_BYTE, size);
#else
    (void)p;
    (void)size;
#endif
}

/* Free a block that is no longer linked into the arena, and remove it from the
 * total of the arena.
 */
static void arena_freeblock_(arena* a, arena_block* b)
{
    arena_poison_(b + 1, b->capacity);
    a->total -= b->capacity;
    ARENA_FREE(b);
}

/* Find where size bytes with the given alignment fit in a block, when used
 * bytes of it are already taken. Returns NULL if they do not fit.
 */
static uint8_t* arena_fit_(arena_block* b, size_t used, size_t size,
                           size_t align)
{
    uint8_t* data = (uint8_t*)(b + 1);

    /* Pad from the actual address rather than the offset, so any alignment
     * works regardless of how the block itself is aligned */
    uintptr_t cur = (uintptr_t)data + used;
    size_t misalign = (size_t)(cur & (align - 1));
    size_t pad = (align - misalign) & (align - 1);

    /* Written as subtractions so nothing can wrap around */
    size_t left = b->capacity - used;
    if (pad > left || size > left - pad) {
        return NULL;
    }

    return data + used + pad;
}

arena arena_make(size_t blocksize, size_t limit)
{
    arena a = {.blocksize = blocksize, .limit = limit};

    if (blocksize == 0) {
        return a;
    }

    a.block = arena_newblock_(&a, blocksize);

    return a;
}

void arena_free(arena* a)
{
    ARENA_ASSERT(a);

    arena_block* b = a->block;
    while (b) {
        arena_block* prev = b->prev;
        arena_freeblock_(a, b);
        b = prev;
    }

    memset(a, 0, sizeof(*a));
}

void arena_reset(arena* a)
{
    ARENA_ASSERT(a);

    /* Keep the newest block of the regular size and free every other block.
     * Oversized blocks can be anywhere in the chain, so check them all. */
    arena_block* keep = NULL;
    arena_block* b = a->block;
    while (b) {
        arena_block* prev = b->prev;
        if (!keep && b->capacity == a->blocksize) {
            keep = b;
        } else {
            arena_freeblock_(a, b);
        }
        b = prev;
    }

    if (keep) {
        /* New regular blocks are always added in front, so the newest one is
         * the current block, and only its used part has to be poisoned */
        ARENA_ASSERT(keep == a->block);
        keep->prev = NULL;
        arena_poison_(keep + 1, a->used);
    }
    a->block = keep;
    a->used = 0;
}

/* Allocate count elements of elemsize bytes with the given alignment, like
 * arena_alloc_(), but leave the memory uninitialized. For callers that
 * overwrite all of it right away.
 */
static void* arena_take_(arena* a, size_t elemsize, size_t count, size_t align)
{
    ARENA_ASSERT(a);
    /* Alignment has to be a power of two */
    ARENA_ASSERT(align != 0 && (align & (align - 1)) == 0);

    if (elemsize == 0 || count == 0 || count > SIZE_MAX / elemsize) {
        return NULL;
    }
    size_t size = elemsize * count;

    /* Fast path, it fits in the current block */
    if (a->block) {
        uint8_t* dst = arena_fit_(a->block, a->used, size, align);
        if (dst) {
            a->used = (size_t)(dst - (uint8_t*)(a->block + 1)) + size;
            return dst;
        }
    }

    /* A new block needs room for the worst case padding as well */
    if (size > SIZE_MAX - (align - 1)) {
        return NULL;
    }
    size_t capacity = size + (align - 1);
    int oversized = capacity > a->blocksize;
    if (!oversized) {
        capacity = a->blocksize;
    }

    arena_block* b = arena_newblock_(a, capacity);
    if (!b) {
        return NULL;
    }

    uint8_t* dst = arena_fit_(b, 0, size, align);
    ARENA_ASSERT(dst);

    if (oversized && a->block) {
        /* The oversized block is filled by this allocation alone. Put it
         * behind the current block, which may still have room left. */
        b->prev = a->block->prev;
        a->block->prev = b;
    } else {
        b->prev = a->block;
        a->block = b;
        a->used = (size_t)(dst - (uint8_t*)(b + 1)) + size;
    }

    return dst;
}

void* arena_alloc_(arena* a, size_t elemsize, size_t count, size_t align)
{
    void* dst = arena_take_(a, elemsize, count, align);
    if (!dst) {
        return NULL;
    }

    /* The size cannot overflow, arena_take_() succeeded */
    return memset(dst, 0, elemsize * count);
}

void* arena_copy_(arena* a, const void* src, size_t elemsize, size_t count,
                  size_t align)
{
    ARENA_ASSERT(src || count == 0);

    void* dst = arena_take_(a, elemsize, count, align);
    if (!dst) {
        return NULL;
    }

    return memcpy(dst, src, elemsize * count);
}

void* arena_grow_last_(arena* a, void* p, size_t elemsize, size_t oldcount,
                       size_t newcount)
{
    ARENA_ASSERT(a);
    ARENA_ASSERT(p);
    ARENA_ASSERT(oldcount != 0 && newcount != 0);

    if (elemsize == 0 || oldcount > SIZE_MAX / elemsize ||
        newcount > SIZE_MAX / elemsize) {
        return NULL;
    }
    size_t oldsize = elemsize * oldcount;
    size_t newsize = elemsize * newcount;

    /* Allocations never overlap, so only the last allocation in the current
     * block ends exactly where the used part of the block ends */
    if (!a->block || oldsize > a->used) {
        return NULL;
    }
    size_t start = a->used - oldsize;
    uint8_t* data = (uint8_t*)(a->block + 1);
    if ((uint8_t*)p != data + start) {
        return NULL;
    }

    if (newsize > a->block->capacity - start) {
        return NULL;
    }

    if (newsize > oldsize) {
        memset((uint8_t*)p + oldsize, 0, newsize - oldsize);
    } else {
        arena_poison_((uint8_t*)p + newsize, oldsize - newsize);
    }
    a->used = start + newsize;

    return p;
}

/* Copy len chars of s into the arena, and terminate the copy */
static char* arena_strcopy_(arena* a, const char* s, size_t len)
{
    if (len == SIZE_MAX) {
        return NULL;
    }

    char* dst = arena_take_(a, 1, len + 1, 1);
    if (!dst) {
        return NULL;
    }

    memcpy(dst, s, len);
    dst[len] = '\0';

    return dst;
}

char* arena_strdup(arena* a, const char* s)
{
    ARENA_ASSERT(s);

    return arena_strcopy_(a, s, strlen(s));
}

char* arena_strndup(arena* a, const char* s, size_t n)
{
    ARENA_ASSERT(s);

    /* Like strnlen(), which is not in C11. memchr() stops at the first match,
     * so it never reads past the terminator of a string shorter than n. */
    const char* end = memchr(s, '\0', n);
    size_t len = end ? (size_t)(end - s) : n;

    return arena_strcopy_(a, s, len);
}

char* arena_sprintf(arena* a, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char* dst = arena_vsprintf(a, fmt, args);
    va_end(args);

    return dst;
}

char* arena_vsprintf(arena* a, const char* fmt, va_list args)
{
    ARENA_ASSERT(a);
    ARENA_ASSERT(fmt);

    /* Measure first, the arguments are used twice so they have to be copied */
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (len < 0) {
        return NULL;
    }

    char* dst = arena_take_(a, 1, (size_t)len + 1, 1);
    if (!dst) {
        return NULL;
    }

    vsnprintf(dst, (size_t)len + 1, fmt, args);

    return dst;
}

arena_mark arena_save(arena* a)
{
    ARENA_ASSERT(a);

    return (arena_mark){
        .block = a->block,
        .prev = a->block ? a->block->prev : NULL,
        .used = a->used,
    };
}

void arena_rewind(arena* a, arena_mark mark)
{
    ARENA_ASSERT(a);

    /* Check the mark before freeing anything. The marked block, and the block
     * that was behind it, have to still be in the chain. Otherwise the mark
     * was invalidated by an earlier rewind or reset, or belongs to another
     * arena. */
    arena_block* b = a->block;
    while (b && b != mark.block) {
        b = b->prev;
    }
    ARENA_ASSERT(b == mark.block);
    if (mark.block) {
        b = mark.block->prev;
        while (b && b != mark.prev) {
            b = b->prev;
        }
        ARENA_ASSERT(b == mark.prev);
    }
    /* Rewinding within the current block can only go backwards */
    ARENA_ASSERT(mark.block != a->block || mark.used <= a->used);

    /* How far the marked block was used, to poison what is given back of it.
     * If blocks were added in front of it, that offset was not kept. */
    size_t end = 0;
    if (mark.block) {
        end = mark.block == a->block ? a->used : mark.block->capacity;
    }

    /* Free the blocks added in front of the marked block */
    b = a->block;
    while (b != mark.block) {
        arena_block* prev = b->prev;
        arena_freeblock_(a, b);
        b = prev;
    }

    /* Oversized blocks allocated while the marked block was current were put
     * directly behind it. Free them too, up to the block that was behind it
     * when the mark was saved. */
    if (mark.block) {
        b = mark.block->prev;
        while (b != mark.prev) {
            arena_block* prev = b->prev;
            arena_freeblock_(a, b);
            b = prev;
        }
        mark.block->prev = mark.prev;

        arena_poison_((uint8_t*)(mark.block + 1) + mark.used, end - mark.used);
    }

    a->block = mark.block;
    a->used = mark.used;
}

#endif /* ARENA_IMPLEMENTATION */
