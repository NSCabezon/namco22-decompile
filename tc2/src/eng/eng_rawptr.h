/*
 * eng_rawptr.h -- POINTER DEVICES READ DIRECTLY, one at a time: a light gun (an absolute mouse) or a mouse picked by name, so that
 * two copies of the game on one computer can each take their own gun (the arcade's twin cabinets, LINK PLAY over the LAN), where the
 * system pointer SDL reports merges every pointing device into one.
 *   Linux    /dev/input/event* (evdev): every device that moves (ABS_X/Y or REL_X/Y) and has a button, pads and joysticks left out.
 *            Reading them needs the 'input' group (eng_rawptr_error says so). A path ($TC2_GUN_DEVICE=/dev/input/event7, or a FIFO
 *            of input_event records for tests) is opened as given, its range assumed 0..65535 if the device cannot be asked.
 *   Windows  Raw Input (WM_INPUT through SDL's message hook, RIDEV_INPUTSINK: the window need not be in front): every HID mouse.
 *   Others   none (the system pointer only).
 * The game picks one (Controls > Gun device, `gun_device` in the cfg, $TC2_GUN_DEVICE: "N" = the Nth listed, a name or id fragment,
 * or a path) and reads its position and buttons here instead of SDL's; the host maps the device's space onto the picture
 * (src/tc2_window.c ss22_host_rawptr: Controls > Gun mapping -- the whole desktop, the window's monitor, or the window).
 */
#ifndef ENG_RAWPTR_H
#define ENG_RAWPTR_H
#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>

typedef struct {
    char id[128];                 /* stable across runs: Linux "evdev:<serial>|<usb path>|<name>", Windows the HID device path */
    char name[64];
    int  abs;                     /* 1 = an absolute pointer (a light gun, a touch screen): x, y are 0..1 over its space; 0 = a relative mouse */
    int  space;                   /* abs: 0 = the whole desktop (every monitor), 1 = the primary monitor only (Windows, no MOUSE_VIRTUAL_DESKTOP) */
    int  open;                    /* its events can be read */
} eng_rawptr_info;

void eng_rawptr_scan(void);                          /* (re)list the devices; the selection is kept by id */
void eng_rawptr_attach(SDL_Window *win);             /* once the window exists (Windows: register for raw input to it) */
int  eng_rawptr_count(void);
const eng_rawptr_info *eng_rawptr_at(int i);         /* NULL past the list */
int  eng_rawptr_select(const char *spec);            /* "" = none (the system pointer); "N" 1-based, an id / name fragment, a path. Returns the index, -1 = none */
int  eng_rawptr_selected(void);                      /* -1 = none */
const char *eng_rawptr_selected_id(void);            /* "" if none */
const char *eng_rawptr_error(void);                  /* "" or why devices cannot be read (Linux: no permission) */
void eng_rawptr_poll(void);                          /* once a frame (Linux reads the events here; Windows gets them through the hook) */
/* the selected device: its absolute position (abs devices), the motion since the last call in device counts (relative ones, zeroed
 * by the call), its buttons as SDL_BUTTON_*MASK bits, and how many events it gave since the last call. false = none selected */
bool eng_rawptr_state(float *x, float *y, int *dx, int *dy, uint32_t *buttons, unsigned *events);
uint32_t eng_rawptr_buttons(void);                   /* the selected device's buttons now (0 if none) */
unsigned eng_rawptr_activity(int i);                 /* events seen from device i since the scan (the menu: which gun is which) */
#endif
