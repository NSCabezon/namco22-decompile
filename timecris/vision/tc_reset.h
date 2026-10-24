/* tc_reset.h -- forced (-include) into the C sources of the tc_core library and into glmetal.m (timecris/CMakeLists.txt, target
 * tc_core), so that tc_core_quit can put the whole game back to how a new process finds it (vision/README.md, "Quit"):
 *   - every global and static variable those files define (the 68K's RAM images and register file, the DSP and sound MCU, the
 *     scheduler, the renderer's caches, `static` locals) lands in two sections of their own: __DATA,__tcg_data (initialised) and
 *     __DATA,__tcg_bss (zero). tc_reset.c copies __tcg_data when the image loads and puts both back on a quit;
 *   - in the C sources (not glmetal.m, whose few buffers glmetal_destroy frees), malloc/calloc/realloc/free/strdup go through
 *     tc_reset.c, which remembers every block still live, so a quit frees them all.
 * vision/tc_core.m does not include it: its state (the API's locks, the app's settings) outlives a game.
 * Nothing changes in how the code runs: only where its variables live, and that each allocation is also noted. */
#ifndef TC_RESET_H
#define TC_RESET_H
#ifndef TC_RESET_IMPL
#if defined(__MACH__)
#pragma clang section bss="__DATA,__tcg_bss,zerofill" data="__DATA,__tcg_data"
#else
/* ELF (Android / Meta Quest): a named section whose name is a valid C identifier, so the linker (lld / GNU ld) provides the
 * boundary symbols __start___tcg_data / __stop___tcg_data (and the bss pair) automatically; tc_reset.c reads them. */
#pragma clang section bss="__tcg_bss" data="__tcg_data"
#endif
#endif
#ifndef __OBJC__
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
void *tcg_malloc(size_t n);
void *tcg_calloc(size_t n, size_t size);
void *tcg_realloc(void *p, size_t n);
void  tcg_free(void *p);
char *tcg_strdup(const char *s);
#ifndef TC_RESET_IMPL
#define malloc(n)      tcg_malloc(n)
#define calloc(n, s)   tcg_calloc(n, s)
#define realloc(p, n)  tcg_realloc(p, n)
#define free(p)        tcg_free(p)
#define strdup(s)      tcg_strdup(s)
#endif
#endif
#endif
