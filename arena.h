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
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARENA_MALLOC
#define ARENA_MALLOC(sz) malloc(sz)
#endif

#ifndef ARENA_REALLOC
#define ARENA_REALLOC(p, sz) realloc(p, sz)
#endif

#ifndef ARENA_FREE
#define ARENA_FREE(p) free(p)
#endif

#ifndef ARENA_ASSERT
#define ARENA_ASSERT(cond) assert(cond)
#endif

/* A memory arena. Allocations are made by bumping an offset into a single
 * block of memory, and are all freed at once with arena_reset() or
 * arena_free(). The fields should not be written to by the user.
 */
typedef struct {
    uint8_t* data;   /* Memory block */
    size_t used;     /* Bytes used, including alignment padding */
    size_t capacity; /* Capacity in bytes */
} arena;

/* Make a new arena with a fixed capacity. An arena has to be freed using
 * arena_free().
 *
 * On failure data is NULL and capacity is 0. Every allocation from such an
 * arena fails, but it can still be freed.
 *
 *  capacity    is the capacity in bytes
 */
arena arena_make(size_t capacity);

/* Free the memory block of the arena and reset fields to zero. Every pointer
 * returned by arena_alloc() is invalidated.
 *
 *  a       is the arena to free
 */
void arena_free(arena* a);

/* Reset the arena so that its whole capacity can be reused. The memory block
 * is kept, but every pointer returned by arena_alloc() is invalidated.
 *
 *  a       is the arena to reset
 */
void arena_reset(arena* a);

/* Allocate count elements of a type from the arena, aligned for that type.
 * Returns a pointer to the first element, or NULL if there is not enough space
 * left, in which case the arena is left unchanged. Allocating zero elements
 * also returns NULL. Elements are zero-initialized.
 *
 *  a       is the arena to allocate from
 *  type    is the type of the elements
 *  count   is the number of elements
 */
#define arena_alloc(a, type, count)                                            \
    ((type*)arena_alloc_((a), sizeof(type), (count), _Alignof(type)))
void* arena_alloc_(arena* a, size_t elemsize, size_t count, size_t align);

#ifdef ARENA_IMPLEMENTATION

arena arena_make(size_t capacity)
{
    arena empty = {0};

    if (capacity == 0) {
        return empty;
    }

    void* data = ARENA_MALLOC(capacity);
    if (!data) {
        return empty;
    }

    return (arena){
        .data = data,
        .used = 0,
        .capacity = capacity,
    };
}

void arena_free(arena* a)
{
    ARENA_ASSERT(a);

    ARENA_FREE(a->data);
    memset(a, 0, sizeof(*a));
}

void arena_reset(arena* a)
{
    ARENA_ASSERT(a);

    a->used = 0;
}

void* arena_alloc_(arena* a, size_t elemsize, size_t count, size_t align)
{
    ARENA_ASSERT(a);
    /* Alignment has to be a power of two */
    ARENA_ASSERT(align != 0 && (align & (align - 1)) == 0);

    if (elemsize == 0 || count == 0 || count > SIZE_MAX / elemsize) {
        return NULL;
    }
    size_t size = elemsize * count;

    /* Pad from the actual address rather than the offset, so any alignment
     * works regardless of how the block itself is aligned */
    uintptr_t cur = (uintptr_t)a->data + a->used;
    size_t misalign = (size_t)(cur & (align - 1));
    size_t pad = (align - misalign) & (align - 1);

    /* Written as subtractions so nothing can wrap around */
    size_t left = a->capacity - a->used;
    if (pad > left || size > left - pad) {
        return NULL;
    }

    uint8_t* dst = a->data + a->used + pad;
    a->used += pad + size;

    return memset(dst, 0, size);
}

#endif /* ARENA_IMPLEMENTATION */
