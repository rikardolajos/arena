# arena

This is a small header-only library that implements a memory arena in C11.
Memory is allocated by bumping an offset into a large block, which is fast, and everything allocated from the arena is freed at once instead of one allocation at a time.
The arena grows by adding more blocks when it is full, never moves what has already been allocated, and can be given a limit on how large it may grow.


## Usage

Copy the header file `arena.h` to your repository and include it in the files where you want to use it.
In **exactly one** C file you will have to define `ARENA_IMPLEMENTATION` before including the header file:

```C
#define ARENA_IMPLEMENTATION
#include "arena.h"
```

The library requires C11 (for `_Alignof`), so with MSVC compile with `/std:c11` or later.
The allocator and assertion can be replaced by defining `ARENA_MALLOC(sz)`, `ARENA_FREE(p)`, and `ARENA_ASSERT(cond)` before including the implementation.

Make an arena with `arena_make()`, allocate from it with `arena_alloc()`, and free all its memory with `arena_free()` after use.
Access the fields of the `arena` struct to see the details of the arena:

```C
typedef struct {
    arena_block* block; /* Current block, NULL if there is none */
    size_t used;        /* Bytes used in the current block */
    size_t blocksize;   /* Capacity of new blocks in bytes */
    size_t limit;       /* Maximum total capacity in bytes, 0 for no limit */
    size_t total;       /* Total capacity of all blocks in bytes */
    size_t peak;        /* Highest total capacity reached, kept on reset */
} arena;
```

These fields should not be written to by the user.

### Blocks and growth

`arena_make(blocksize, limit)` allocates a first block of `blocksize` bytes.
When the current block is full, a new block of the same size is added, so the arena grows one block at a time rather than doubling.
An allocation that is too large for a block gets a block of its own, sized to fit.
An allocation is never split across blocks, and never moves, so pointers stay valid until the arena is reset, rewound, or freed.

If `limit` is not 0, the total capacity of all blocks never grows past it, and an allocation that would need more fails and returns `NULL`.
Use `arena_make(size, size)` for an arena with a fixed capacity that never grows.
The `peak` field keeps the highest total capacity reached, even after a reset, which helps to see how close to its budget an arena gets.

`arena_reset()` makes the memory of the arena reusable.
It keeps one block to allocate from again and frees any others, so a single large peak does not keep its memory forever.

### Example usage
```C
typedef struct {
    float x, y;
    const char* name;
} entity;

/* Level data lives until the next level is loaded. Blocks of 1 MiB, and never
 * more than 64 MiB in total. */
arena level = arena_make(1 << 20, 64 << 20);

/* Scratch data lives for one frame. Blocks of 64 KiB, with no limit. */
arena frame = arena_make(64 << 10, 0);

/* Load the level (zero-initialized) */
entity* entities = arena_alloc(&level, entity, 3);
entities[0] = (entity){1.0f, 2.0f, arena_strdup(&level, "player")};
entities[1] = (entity){5.0f, 0.5f, arena_strdup(&level, "goblin")};
entities[2] = (entity){8.0f, 3.0f, arena_strdup(&level, "chest")};

for (int f = 0; f < 2; f++) {
    for (int i = 0; i < 3; i++) {
        /* Temporary strings, no need to free them one by one */
        char* label = arena_sprintf(&frame, "frame %d: %s at (%.1f, %.1f)", f,
                                    entities[i].name, entities[i].x,
                                    entities[i].y);
        printf("%s\n", label);
        entities[i].x += 1.0f;
    }

    /* Free everything allocated during the frame at once */
    arena_reset(&frame);
}

/* Free the arenas when we are done with them */
arena_free(&frame);
arena_free(&level);
```

Expected output:
```
frame 0: player at (1.0, 2.0)
frame 0: goblin at (5.0, 0.5)
frame 0: chest at (8.0, 3.0)
frame 1: player at (2.0, 2.0)
frame 1: goblin at (6.0, 0.5)
frame 1: chest at (9.0, 3.0)
```

### Scratch allocations

`arena_save()` saves the current position of an arena, and `arena_rewind()` frees everything allocated after it, including any blocks that were added.
This gives temporary memory in the middle of a frame, without resetting the whole arena:

```C
arena_mark m = arena_save(scratch);

int* tmp = arena_alloc(scratch, int, 1000);
/* ... use tmp ... */

arena_rewind(scratch, m); /* tmp is freed, anything before m is kept */
```

Marks can be nested, and a mark can be rewound to more than once.
Rewinding to an earlier mark invalidates the marks saved after it, and `arena_reset()` and `arena_free()` invalidate all marks.

### Copies and strings

`arena_copy()` copies elements into the arena, and the string helpers copy or format null-terminated strings into it, sized to fit:

```C
const char* line = "move 10 20";
char* cmd = arena_strndup(&a, line, 4);              /* "move" */
char* path = arena_sprintf(&a, "maps/%s.map", "dungeon");
int* copy = arena_copy(&a, int, ((int[]){1, 2, 3}), 3);
```

`arena_copy()` checks that the type of the source pointer matches the element type, and GCC and Clang check the format string of `arena_sprintf()`, both as compiler warnings.
Note that a macro argument containing a comma, like the compound literal above, has to be wrapped in parentheses.

For details of the implementation and which functions are available, check the header file.

More examples of usage can be found in the `test.c` file.


## Safety

No error messages are returned by `arena`, but failures can be detected: every function that allocates returns `NULL` when it fails, and leaves the arena unchanged.
Allocating from an arena fails when the limit would be exceeded or `malloc()` fails, and allocating zero elements also returns `NULL`.
If `arena_make()` fails, `block` is `NULL`, and the arena can still be allocated from (which tries again) and freed.

Memory from `arena_alloc()` is zero-initialized and aligned for its type.
Using a pointer after the arena was reset, rewound past it, or freed is not detected, and neither is writing past the end of an allocation.
Some misuse is caught by assertions, like an alignment that is not a power of two, or rewinding to a mark that is no longer valid, but not all of it.

An arena is not thread-safe. Use one arena per thread, or guard it with a lock.


## License

`arena` is released under the MIT License.
