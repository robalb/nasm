/*
 * x86-64-playground: bump allocator replacing the libc (musl mallocng)
 * allocator.
 *
 * This nasm build runs inside the Blink x86-64 emulator, compiled to wasm,
 * and only assembles small snippets. Profiling showed that ~55% of the
 * guest instructions executed while assembling an empty file were spent in
 * musl's mallocng (~390 instructions per malloc+free pair, plus mmap/munmap
 * churn that flushes Blink's TLB on every munmap). Replacing it with this
 * allocator halves nasm's instruction count on small inputs.
 *
 * free() is a no-op: memory is never reused, so peak memory grows with the
 * total amount allocated (~29MB for a 3300-instruction file, ~1.5MB for
 * small snippets). Memory comes from fresh anonymous mmap chunks, which are
 * zero-filled, so calloc() needs no memset.
 *
 * Being part of libnasm.a, this object is linked before libc, so its
 * definitions take precedence over musl's malloc family.
 */
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define CHUNK ((size_t)8 << 20)

struct hdr { size_t size, pad; };   /* 16 bytes: keeps 16-byte alignment */
static char *cur, *end;

static void *bump(size_t n, size_t align)
{
    uintptr_t p;
    size_t need;
    if (n > SIZE_MAX / 2) { errno = ENOMEM; return NULL; }
    for (;;) {
        p = (uintptr_t)cur + sizeof(struct hdr);
        p = (p + align - 1) & ~(uintptr_t)(align - 1);
        need = (p - (uintptr_t)cur) + ((n + 15) & ~(size_t)15);
        if (cur && need <= (size_t)(end - cur)) break;
        size_t sz = need + align > CHUNK ? (need + align + 4095) & ~(size_t)4095 : CHUNK;
        void *m = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { errno = ENOMEM; return NULL; }
        cur = m;
        end = cur + sz;
    }
    ((struct hdr *)p)[-1].size = n;
    cur += need;
    return (void *)p;
}

void *malloc(size_t n) { return bump(n, 16); }
void free(void *p) { (void)p; }
void *calloc(size_t a, size_t b)
{
    size_t n;
    if (__builtin_mul_overflow(a, b, &n)) { errno = ENOMEM; return NULL; }
    return bump(n, 16);
}
void *realloc(void *p, size_t n)
{
    void *q;
    size_t old;
    if (!p) return bump(n, 16);
    old = ((struct hdr *)p)[-1].size;
    if (n <= old) return p;
    if ((q = bump(n, 16))) memcpy(q, p, old);
    return q;
}
void *aligned_alloc(size_t al, size_t n) { return bump(n, al < 16 ? 16 : al); }
void *memalign(size_t al, size_t n) { return bump(n, al < 16 ? 16 : al); }
int posix_memalign(void **r, size_t al, size_t n)
{
    void *p = bump(n, al < 16 ? 16 : al);
    if (!p) return ENOMEM;
    *r = p;
    return 0;
}
size_t malloc_usable_size(void *p) { return p ? ((struct hdr *)p)[-1].size : 0; }
