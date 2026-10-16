/* tc_reset_core.h -- tc_reset.c's interface for vision/tc_core.m (tc_core_quit) */
#ifndef TC_RESET_CORE_H
#define TC_RESET_CORE_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Frees every block the core's C code still holds and puts all of its global and static variables (and glmetal's) back to their
 * values at image load. Only with no game thread and no other core code running. False if the load-time copy is missing. */
bool   tcg_reset(size_t *blocks_freed);
size_t tcg_live_blocks(void);
size_t tcg_data_size(void);
size_t tcg_bss_size(void);
#ifdef __cplusplus
}
#endif
#endif
