/* tc_reset.c -- the core's "new process" for tc_core_quit (tc_reset.h says what is gathered where). Built with TC_RESET_IMPL: its
 * own state is ordinary, so it outlives the reset it does. */
#include "tc_reset.h"
#include "tc_reset_core.h"
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unistd.h>

/* Platform shims: Mach-O (Apple) uses os_unfair_lock and malloc_size; ELF (Android / Meta Quest) uses a pthread mutex and
 * malloc_usable_size. The rest of the file is identical. */
#if defined(__MACH__)
#include <os/lock.h>
#include <malloc/malloc.h>
#define TCG_LOCK_T       os_unfair_lock
#define TCG_LOCK_INIT    OS_UNFAIR_LOCK_INIT
#define TCG_LOCK(p)      os_unfair_lock_lock(p)
#define TCG_UNLOCK(p)    os_unfair_lock_unlock(p)
#define TCG_MALLOC_SIZE(p) malloc_size(p)
#else
#include <pthread.h>
#include <malloc.h>
#define TCG_LOCK_T       pthread_mutex_t
#define TCG_LOCK_INIT    PTHREAD_MUTEX_INITIALIZER
#define TCG_LOCK(p)      pthread_mutex_lock(p)
#define TCG_UNLOCK(p)    pthread_mutex_unlock(p)
#define TCG_MALLOC_SIZE(p) malloc_usable_size(p)
#endif

/* the sections' bounds, made by the linker; arrays of unknown size, so _FORTIFY_SOURCE does not take them for one char */
#if defined(__MACH__)
/* ld64 / ld-prime: section$start$SEG$SECT */
extern char tcg_data_start[]__asm("section$start$__DATA$__tcg_data");
extern char tcg_data_end[]  __asm("section$end$__DATA$__tcg_data");
extern char tcg_bss_start[] __asm("section$start$__DATA$__tcg_bss");
extern char tcg_bss_end[]   __asm("section$end$__DATA$__tcg_bss");
#else
/* ELF (lld / GNU ld): __start_<name> / __stop_<name> for a section whose name is a C identifier (tc_reset.h names the
 * sections __tcg_data / __tcg_bss for this reason) */
extern char __start___tcg_data[], __stop___tcg_data[];
extern char __start___tcg_bss[],  __stop___tcg_bss[];
#define tcg_data_start __start___tcg_data
#define tcg_data_end   __stop___tcg_data
#define tcg_bss_start  __start___tcg_bss
#define tcg_bss_end    __stop___tcg_bss
#endif

static void  *data0;                 /* __tcg_data as the image was loaded */
static size_t data_n;

/* when the image loads, before any of the game's code has run: the initial values of every initialised variable */
__attribute__((constructor)) static void tcg_snapshot(void)
{
    data_n = (size_t)(tcg_data_end - tcg_data_start);
    data0 = malloc(data_n ? data_n : 1);
    if (data0) memcpy(data0, tcg_data_start, data_n);
}

/* ---- the live blocks: an open-addressed set of pointers (0 empty, 1 deleted) */
static TCG_LOCK_T lk = TCG_LOCK_INIT;
static uintptr_t *set;
static size_t cap, used, live;       /* slots, slots not empty (live + deleted), live blocks */

static size_t slot_of(uintptr_t p) { p ^= p >> 33; p *= 0xFF51AFD7ED558CCDull; p ^= p >> 33; return (size_t)p & (cap - 1); }
static void put_locked(uintptr_t p);
static void grow_locked(void)
{
    uintptr_t *old = set; const size_t oc = cap;
    cap = cap ? (live * 4 > cap ? cap * 2 : cap) : 1024;
    set = calloc(cap, sizeof *set);
    if (!set) abort();
    used = live = 0;
    for (size_t i = 0; i < oc; i++) if (old[i] > 1) put_locked(old[i]);
    free(old);
}
static void put_locked(uintptr_t p)
{
    if ((used + 1) * 2 > cap) grow_locked();
    size_t i = slot_of(p);
    while (set[i] > 1) i = (i + 1) & (cap - 1);
    if (set[i] == 0) used++;
    set[i] = p; live++;
}
static bool take_locked(uintptr_t p)
{
    if (!cap) return false;
    for (size_t i = slot_of(p);; i = (i + 1) & (cap - 1)) {
        if (set[i] == 0) return false;
        if (set[i] == p) { set[i] = 1; live--; return true; }
    }
}
static void *noted(void *p)
{
    if (p) { TCG_LOCK(&lk); put_locked((uintptr_t)p); TCG_UNLOCK(&lk); }
    return p;
}

/* BIG blocks (the ROM images, the texture and wave memories: tens of MB each) are mapped pages of their own, so that a quit gives
 * them back to the system at once: freed through malloc they would stay in its cache of large blocks, dirty, counted in the app's
 * footprint. Mapped pages read as zeros, which is also what calloc promises. */
#define BIG (1u << 20)
#define BIG_MAX 64
static struct { void *p; size_t n; } big[BIG_MAX];
static int nbig;
static void *big_map(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    TCG_LOCK(&lk);
    if (nbig == BIG_MAX) { TCG_UNLOCK(&lk); munmap(p, n); return NULL; }
    big[nbig].p = p; big[nbig].n = n; nbig++;
    TCG_UNLOCK(&lk);
    return p;
}
static size_t big_take_locked(void *p)              /* its size if p is a big block (then forgotten), else 0 */
{
    for (int i = 0; i < nbig; i++)
        if (big[i].p == p) { const size_t n = big[i].n; big[i] = big[--nbig]; return n; }
    return 0;
}

void *tcg_malloc(size_t n) { if (n >= BIG) { void *p = big_map(n); if (p) return p; } return noted(malloc(n)); }
void *tcg_calloc(size_t n, size_t s)
{
    if (s && n <= SIZE_MAX / s && n * s >= BIG) { void *p = big_map(n * s); if (p) return p; }
    return noted(calloc(n, s));
}
char *tcg_strdup(const char *s) { return noted(strdup(s)); }
void tcg_free(void *p)
{
    if (!p) return;
    TCG_LOCK(&lk);
    const size_t bn = big_take_locked(p);
    if (!bn) take_locked((uintptr_t)p);
    TCG_UNLOCK(&lk);
    if (bn) munmap(p, bn);
    else free(p);                    /* a block the C library made (realpath's) was never noted: freed all the same */
}
void *tcg_realloc(void *p, size_t n)
{
    if (!p) return tcg_malloc(n);
    TCG_LOCK(&lk);
    size_t bn = 0;
    for (int i = 0; i < nbig; i++) if (big[i].p == p) bn = big[i].n;
    TCG_UNLOCK(&lk);
    if (bn || n >= BIG) {                           /* to, from or between big blocks: a new block and a copy */
        size_t old = bn;
        if (!bn) old = TCG_MALLOC_SIZE(p);
        void *q = tcg_malloc(n);
        if (!q) return NULL;
        memcpy(q, p, old < n ? old : n);
        tcg_free(p);
        return q;
    }
    TCG_LOCK(&lk);
    const bool mine = take_locked((uintptr_t)p);
    TCG_UNLOCK(&lk);
    void *q = realloc(p, n);
    if (!q) { if (mine) noted(p); return NULL; }  /* failed: the old block is still live */
    return mine ? noted(q) : q;
}

bool tcg_reset(size_t *blocks_freed)
{
    TCG_LOCK(&lk);
    size_t n = 0;
    for (size_t i = 0; i < cap; i++) if (set[i] > 1) { free((void *)set[i]); n++; }
    if (set) memset(set, 0, cap * sizeof *set);
    used = live = 0;
    for (int i = 0; i < nbig; i++) { munmap(big[i].p, big[i].n); n++; }
    nbig = 0;
    TCG_UNLOCK(&lk);
    if (blocks_freed) *blocks_freed = n;
    if (!data0) return false;
    memcpy(tcg_data_start, data0, data_n);
    /* the zero section: its whole pages mapped anew (zero-fill on demand, so a quit leaves them out of the footprint, as before the
     * first game), the partial pages at its ends cleared; memset all of it if the system will not map there */
    const uintptr_t pg = (uintptr_t)getpagesize(), b0 = (uintptr_t)tcg_bss_start, b1 = (uintptr_t)tcg_bss_end;
    const uintptr_t m0 = (b0 + pg - 1) & ~(pg - 1), m1 = b1 & ~(pg - 1);
    if (m1 > m0 && mmap((void *)m0, m1 - m0, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == (void *)m0) {
        memset(tcg_bss_start, 0, m0 - b0);
        memset((void *)m1, 0, b1 - m1);
    } else memset(tcg_bss_start, 0, b1 - b0);
    return true;
}

size_t tcg_live_blocks(void) { TCG_LOCK(&lk); const size_t n = live + (size_t)nbig; TCG_UNLOCK(&lk); return n; }
size_t tcg_data_size(void) { return data_n; }
size_t tcg_bss_size(void) { return (size_t)(tcg_bss_end - tcg_bss_start); }
