/*
 * libc_stubs.c — route newlib's malloc family into jo_engine's pool
 *
 * The upstream Utenyaa build (per their README) links against the
 * yaul toolchain with jo-engine's newlib sysroot grafted in. That
 * brings newlib's `malloc` / `free` / `_malloc_r` / `_free_r` etc.
 * into the link, sitting alongside jo_engine's static
 * `JO_GLOBAL_MEMORY_SIZE_FOR_MALLOC` pool used by `jo_malloc` /
 * `jo_free` (and by Jo.hpp's inline `operator new`/`operator delete`).
 *
 * Two heaps in one binary → eventual cross-allocator free →
 * heap-corruption traps. We saw two of these on hardware:
 *   "jo_free Bad pointer: 5D323A20"  (= ASCII "]2: ")
 *   "jo_free Bad pointer: 50323A20"  (= ASCII "P2: ")
 * — bytes from a recent printf format-string fragment lingering in
 * newlib's heap-resident state after libc internals freed them via
 * the wrong allocator.
 *
 * Defining `malloc`/`free`/etc. in this TU before the libc archive
 * is consulted means static link picks our symbols up first and skips
 * newlib's `malloc.o` entirely — leaving newlib's string/printf code
 * intact (we still want `memcpy`, `vsprintf`, etc. from it) but
 * unifying allocations onto jo_engine's pool. The reentrant variants
 * `_malloc_r` / `_free_r` / `_calloc_r` / `_realloc_r` are also
 * exported here so any newlib internal that goes through them lands
 * on jo_malloc too. `_sbrk` returns -1: newlib's stock allocator is
 * unreachable, but if any stray reference survives, _sbrk failing
 * cleanly is much better than scribbling random RAM.
 */

#include <stddef.h>
#include <jo/jo.h>

void *malloc(size_t size)
{
    return jo_malloc((unsigned int)size);
}

void free(void *ptr)
{
    if (ptr) jo_free(ptr);
}

void *calloc(size_t nmemb, size_t size)
{
    unsigned int total = (unsigned int)(nmemb * size);
    void *p = jo_malloc(total);
    if (p) {
        unsigned char *b = (unsigned char *)p;
        unsigned int i;
        for (i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return jo_malloc((unsigned int)size);
    if (size == 0) { jo_free(ptr); return NULL; }
    void *np = jo_malloc((unsigned int)size);
    if (!np) return NULL;
    {
        unsigned char *src = (unsigned char *)ptr;
        unsigned char *dst = (unsigned char *)np;
        unsigned int i;
        for (i = 0; i < (unsigned int)size; i++) dst[i] = src[i];
    }
    jo_free(ptr);
    return np;
}

struct _reent;

void *_malloc_r(struct _reent *r, size_t size)
{
    (void)r;
    return malloc(size);
}

void _free_r(struct _reent *r, void *ptr)
{
    (void)r;
    free(ptr);
}

void *_calloc_r(struct _reent *r, size_t nmemb, size_t size)
{
    (void)r;
    return calloc(nmemb, size);
}

void *_realloc_r(struct _reent *r, void *ptr, size_t size)
{
    (void)r;
    return realloc(ptr, size);
}

void *_sbrk(int incr)
{
    (void)incr;
    return (void *)-1;
}
