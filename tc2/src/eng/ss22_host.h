/* ss22_host.h -- TC2's stand-in: where the mouse / absolute-mouse gun points in the picture (src/tc2_window.c) */
#ifndef TC2_SS22_HOST_SHIM_H
#define TC2_SS22_HOST_SHIM_H
#include <stdbool.h>
#include <stdint.h>
bool ss22_host_pointer(float *nx, float *ny, bool *inside);
/* the gun read DIRECTLY from its own device (src/eng/eng_rawptr.h, Controls > Gun device): where it points in the picture, whether
 * that is on the picture, its buttons (SDL_BUTTON_*MASK) and how many events it gave since the last call; false = no device selected */
bool ss22_host_rawptr(float *nx, float *ny, bool *inside, uint32_t *buttons, unsigned *events);
void ss22_host_set_gun_map(int map);            /* Controls > Gun mapping: 0 the whole desktop, 1 the window's monitor, 2 the window */
#endif
