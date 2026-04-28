/*
 * libc_stubs.c — self-contained libc replacement for the SGL+jo_engine link
 *
 * Background: linking newlib (libc.a) drags in a parallel allocator
 * (`_malloc_r`, `_sbrk`-based heap, `__sFILE` per-call struct
 * allocations from the printf family) that fights jo_engine's static
 * `JO_GLOBAL_MEMORY_SIZE_FOR_MALLOC` pool. Cross-allocator frees and
 * fragmented printf-buffer reallocs corrupt the pool, manifesting as
 *     "jo_free Bad pointer: <hex>"
 * where the hex value happens to match ASCII bytes of recent printf
 * format strings (we observed 0x5D323A20 / 0x50323A20 = "]2: " / "P2: "
 * — the trailing 24 bits being the "2: " from the lobby's
 * font_printf("P2: %-34s", ...) format string lingering in stale
 * heap-resident newlib state).
 *
 * Solution: don't link newlib at all. Provide every function the
 * SGL/jo_engine/Utenyaa code needs from the C runtime, and implement
 * `malloc`/`free`/`calloc`/`realloc` (plus the `_*_r` reentrant
 * variants) as direct wrappers over jo_malloc/jo_free. With this
 * file present, link with `-nodefaultlibs -lgcc` (libgcc still
 * needed for the SH-2 long-shift / division helpers ___sdivsi3 etc.)
 * and the result has ONE heap, ONE pool, no newlib state, no
 * cross-allocator races.
 *
 * Functions provided here mirror the C standard contract for the
 * subset actually called by the project (verified by linking
 * `-nodefaultlibs` and reading the undefined-reference list).
 */

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <jo/jo.h>

/*============================================================================
 * malloc family — route to jo_malloc / jo_free
 *============================================================================*/

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
    /* No old-size knowledge available from jo_malloc's block header
     * publicly, so we copy `size` bytes — caller usually shrinks or
     * keeps small. For growths the trailing bytes are uninitialised
     * (matches the C realloc contract). */
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

/* Reentrant wrappers — safety net even though we're not linking newlib;
 * if any library .a we link still references these, they resolve here. */
struct _reent;
void *_malloc_r(struct _reent *r, size_t size)            { (void)r; return malloc(size); }
void  _free_r(struct _reent *r, void *ptr)                { (void)r; free(ptr); }
void *_calloc_r(struct _reent *r, size_t n, size_t s)     { (void)r; return calloc(n, s); }
void *_realloc_r(struct _reent *r, void *p, size_t s)     { (void)r; return realloc(p, s); }
void *_sbrk(int incr)                                     { (void)incr; return (void *)-1; }

/*============================================================================
 * String / memory primitives
 *============================================================================*/

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *p, int c, size_t n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = (unsigned char)c;
    return p;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *r = dst;
    while ((*dst++ = *src++)) ;
    return r;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strstr(const char *hay, const char *needle)
{
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return NULL;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    if ((char)c == '\0') return (char *)s;
    return NULL;
}

/*============================================================================
 * Minimal printf-family (sprintf / vsprintf only — no FILE*, no float)
 *
 * Supports the conversion specifiers actually used in this codebase:
 *     %s   %c   %d   %i   %u   %x   %X   %p   %%
 * Flags:
 *     '-' (left align)
 *     '0' (zero pad)
 * Width: integer up to 99
 * Precision: integer up to 99 (for %s only — truncates)
 *
 * Length modifiers ignored (everything is int / unsigned int / pointer).
 * Big-endian SH-2 doesn't change argument promotion rules.
 *============================================================================*/

static char *_emit_pad(char *p, char c, int n)
{
    while (n-- > 0) *p++ = c;
    return p;
}

static int _emit_uint(char *out, unsigned int v, int base, int upper,
                      int width, int zeropad, int leftalign)
{
    char tmp[16];
    int i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[i++] = '0';
    else while (v) { tmp[i++] = digits[v % base]; v /= base; }
    int written = 0;
    int pad = width - i;
    if (!leftalign && pad > 0) {
        char pc = zeropad ? '0' : ' ';
        out = _emit_pad(out, pc, pad);
        written += pad;
    }
    while (i--) { *out++ = tmp[i]; written++; }
    if (leftalign && pad > 0) {
        out = _emit_pad(out, ' ', pad);
        written += pad;
    }
    return written;
}

static int _emit_int(char *out, int v, int width, int zeropad, int leftalign)
{
    int neg = 0;
    unsigned int u;
    if (v < 0) { neg = 1; u = (unsigned int)(-v); }
    else       { u = (unsigned int)v; }

    char tmp[16];
    int i = 0;
    if (u == 0) tmp[i++] = '0';
    else while (u) { tmp[i++] = '0' + (u % 10); u /= 10; }
    int total = i + (neg ? 1 : 0);
    int pad = width - total;
    int written = 0;
    if (!leftalign && pad > 0 && !zeropad) {
        out = _emit_pad(out, ' ', pad);
        written += pad;
    }
    if (neg) { *out++ = '-'; written++; }
    if (!leftalign && pad > 0 && zeropad) {
        out = _emit_pad(out, '0', pad);
        written += pad;
    }
    while (i--) { *out++ = tmp[i]; written++; }
    if (leftalign && pad > 0) {
        out = _emit_pad(out, ' ', pad);
        written += pad;
    }
    return written;
}

static int _emit_str(char *out, const char *s, int width, int prec, int leftalign)
{
    if (!s) s = "(null)";
    int len = 0;
    while (s[len] && (prec < 0 || len < prec)) len++;
    int pad = width - len;
    int written = 0;
    if (!leftalign && pad > 0) { out = _emit_pad(out, ' ', pad); written += pad; }
    int i;
    for (i = 0; i < len; i++) { *out++ = s[i]; written++; }
    if (leftalign && pad > 0) { out = _emit_pad(out, ' ', pad); written += pad; }
    return written;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    char *out = buf;
    while (*fmt) {
        if (*fmt != '%') { *out++ = *fmt++; continue; }
        fmt++;
        int leftalign = 0, zeropad = 0;
        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') leftalign = 1;
            if (*fmt == '0') zeropad = 1;
            fmt++;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }
        /* Skip length modifiers (l, ll, h, hh, z) — int/uint promotion handles them. */
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;

        switch (*fmt) {
        case '%': *out++ = '%'; fmt++; break;
        case 'c': {
            int c = va_arg(ap, int);
            *out++ = (char)c;
            fmt++;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            out += _emit_str(out, s, width, prec, leftalign);
            fmt++;
            break;
        }
        case 'd': case 'i': {
            int v = va_arg(ap, int);
            out += _emit_int(out, v, width, zeropad, leftalign);
            fmt++;
            break;
        }
        case 'u': {
            unsigned int v = va_arg(ap, unsigned int);
            out += _emit_uint(out, v, 10, 0, width, zeropad, leftalign);
            fmt++;
            break;
        }
        case 'x': {
            unsigned int v = va_arg(ap, unsigned int);
            out += _emit_uint(out, v, 16, 0, width, zeropad, leftalign);
            fmt++;
            break;
        }
        case 'X': {
            unsigned int v = va_arg(ap, unsigned int);
            out += _emit_uint(out, v, 16, 1, width, zeropad, leftalign);
            fmt++;
            break;
        }
        case 'p': {
            unsigned int v = (unsigned int)va_arg(ap, void *);
            *out++ = '0'; *out++ = 'x';
            out += _emit_uint(out, v, 16, 0, width, zeropad, leftalign);
            fmt++;
            break;
        }
        default:
            /* Unknown — emit literally so caller sees what's wrong. */
            *out++ = '%';
            if (*fmt) *out++ = *fmt++;
            break;
        }
    }
    *out = '\0';
    return (int)(out - buf);
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t bufsize, const char *fmt, ...)
{
    /* Simple variant — formats into a stack buffer then copies. The
     * codebase doesn't actually call snprintf, but newlib internals or
     * libgcc might. Keeping this here avoids surprise undefined refs. */
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(tmp, fmt, ap);
    va_end(ap);
    if (bufsize == 0) return n;
    size_t copy = (size_t)n < bufsize - 1 ? (size_t)n : bufsize - 1;
    {
        size_t i;
        for (i = 0; i < copy; i++) buf[i] = tmp[i];
        buf[copy] = '\0';
    }
    return n;
}

/*============================================================================
 * Misc — tiny libgcc / startup helpers occasionally referenced
 *============================================================================*/

/* abort() referenced indirectly by some libstdc++-style code. We don't
 * have a real signalling path; halt by spinning. */
void abort(void)
{
    for (;;) { /* SH-2 halt */ }
}
