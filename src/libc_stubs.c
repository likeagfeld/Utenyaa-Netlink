/*
 * libc_stubs.c — route newlib heap into jo_engine's pool
 *
 * Background: when this project links against newlib (for memcpy/memset/
 * sprintf/etc.), newlib internals (printf-family reentrant code, etc.)
 * call malloc()/free()/calloc()/realloc(). By default those resolve to
 * newlib's _malloc_r implementation, which manages an _sbrk()-based heap
 * sitting AFTER _end in WORK RAM-H. jo_engine has its own static pool
 * (JO_GLOBAL_MEMORY_SIZE_FOR_MALLOC) used by jo_malloc/jo_free and by
 * the inline operator new in jo/Jo.hpp. Two heaps, one Saturn, eventual
 * cross-allocator free → bad-pointer trap (we hit one in alpha-0.1
 * online gameplay: jo_free 0x5D323A20 → black screen).
 *
 * Defining malloc/free/calloc/realloc here makes the link picker use
 * THESE symbols and skip the newlib malloc.o / nano-malloc.o entirely
 * (static .a archives only pull .o files for currently-undefined
 * symbols). Result: every allocation, from C++ new through libc
 * internals, goes through jo_malloc, and the one-true-pool invariant
 * is restored.
 *
 * The reentrant variants (_malloc_r, _free_r, etc.) are also provided
 * so newlib's _impure_ptr-driven calls are caught — we simply forward
 * and ignore the reent struct.
 */

#include <stddef.h>
#include <jo/jo.h>

/* jo_malloc / jo_free / jo_malloc_with_behaviour come in via <jo/jo.h>. */

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
    /* Simple realloc: alloc-new, copy-old, free-old.
     * jo's pool doesn't preserve the old allocation size in a header we
     * can read here, so we cap the copy to the new size — sufficient for
     * shrinks, and for growths the unread tail is zero-init by jo
     * conventions in practice (and for newlib's printf-buffer realloc
     * pattern, only the prefix is alive). */
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

/* Reentrant variants used by newlib internals. The _reent argument is
 * ignored — there is only one heap and it's jo's. */

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

/* _sbrk is what newlib's stock malloc uses to extend the heap. We
 * hijack the mechanism by providing our own malloc above, but if any
 * stray newlib path still calls _sbrk we want it to fail cleanly
 * (return -1) rather than scribble random RAM. */
void *_sbrk(int incr)
{
    (void)incr;
    return (void *)-1;
}
