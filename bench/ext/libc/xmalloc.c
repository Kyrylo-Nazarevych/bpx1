/* BPX-1 bench/ext: malloc() and free() for the external benchmarks that allocate (MiBench dijkstra).
   Written for BPX-1. A fixed arena; freed blocks go to a free list per size (last in, first out)
   and are reused for requests of the same rounded size; blocks above SMALL_MAX bytes share one
   list that is searched first-fit. Blocks are neither split nor merged. */
#include <stdlib.h>

#define ARENA_BYTES (1u << 16)                  /* 64 KiB; crt0 clears it at start-up with the rest of .bss */
#define SMALL_MAX 256u                          /* largest size with its own free list */

struct header {
    size_t size;                                /* payload bytes, a multiple of 8 */
    struct header *next;                        /* next free block of the list (while free) */
};

static union {
    unsigned char bytes[ARENA_BYTES];
    long long align;
} arena;
static size_t arena_used;
static struct header *small_free[SMALL_MAX / 8];
static struct header *large_free;

static struct header **list_of(size_t size)
{
    return size <= SMALL_MAX ? &small_free[size / 8 - 1] : &large_free;
}

void *malloc(size_t n)
{
    size_t size = (n + 7u) & ~(size_t)7u;
    struct header **link, *h;

    if (size < n)
        return NULL;                            /* n is close to SIZE_MAX */
    if (size == 0)
        size = 8;
    for (link = list_of(size); (h = *link) != NULL; link = &h->next) {
        if (h->size >= size) {
            *link = h->next;
            return h + 1;
        }
    }
    if (size > ARENA_BYTES - sizeof *h || arena_used > ARENA_BYTES - sizeof *h - size)
        return NULL;
    h = (struct header *)(arena.bytes + arena_used);
    arena_used += sizeof *h + size;
    h->size = size;
    return h + 1;
}

void free(void *p)
{
    if (p != NULL) {
        struct header *h = (struct header *)p - 1;
        struct header **link = list_of(h->size);
        h->next = *link;
        *link = h;
    }
}
