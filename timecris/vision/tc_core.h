// tc_core.h -- the C API the Time Crisis core (ss22 + glmetal, timecris/vision) exposes to the visionOS shell.
//
// THE SAME FILE lives in the app as ~/Apps/tc-visionos/app/Bridge/tc_core.h: change both together (README.md here).
// The shell's Swift `GameCore` protocol maps 1:1 onto these calls (app: Sources/Core/CGameCore.swift, TC_REAL_CORE).
//
// Threading: tc_core_start spawns the core's own game thread (paced at the board's 59.906 Hz; it owns the audio output
// and renders with the Metal device it was given). Every other call is thread-safe and non-blocking (tc_core_stop waits
// briefly for the game thread to park); the shell calls them from the main thread once per display frame.
#ifndef TC_CORE_H
#define TC_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Output-port events (the sound MCU's output latches, MAME's mcuout bits; engine/ss22_out.c).
typedef enum tc_core_event {
    TC_EVENT_SHOT   = 1,   ///< gun recoil solenoid fired (mcuout1 rising edge: one shot the game accepted)
    TC_EVENT_LAMP   = 2,   ///< reserved (start lamp); not emitted yet
} tc_core_event;

typedef void (*tc_core_event_fn)(void *user, tc_core_event event, int32_t value);

/// Starts the core. `mtl_device` is an `id<MTLDevice>` (bridged, unretained) that the core must render with, so its
/// textures are usable by RealityKit. `rom_dir` holds the ROM set (the MAME `timecris` chips, unzipped). `width` x
/// `height` is the render size (native 640x480). Writable files (the EEPROM image `tc_eeprom.nv`) go to
/// Application Support/TimeCrisis in the app's container (the process's working directory is set there).
/// Returns 0 once the game is running, nonzero if it could not start (missing ROMs, no Metal).
/// After tc_core_stop, tc_core_start RESUMES the same game (a powered cabinet that was paused); the arguments are
/// then ignored. One game per process.
int  tc_core_start(const char *rom_dir, void *mtl_device, int width, int height);
/// Pauses the game thread (emulation, audio output and frames stop; the EEPROM is saved). Returns once parked.
void tc_core_stop(void);

/// The newest *completed* frame as an `id<MTLTexture>` (bridged, unretained). Triple buffered: it stays untouched for
/// at least two more frames. `frame_id` increments per finished frame; returns NULL before the first frame.
/// Pixel format: MTLPixelFormatRGBA8Unorm holding sRGB-encoded values (display-ready, gamma already applied by the
/// game's mixer); row 0 is the top of the picture. Sample it (format-agnostic); the shell decodes sRGB itself.
void *tc_core_latest_frame(uint64_t *frame_id);

/// Gun position, normalized: u 0..1 left->right, v 0..1 top->bottom of the 4:3 image.
/// The core maps it to the gun port: x = 68 + u*(694-68), y = 43 + v*(284-43).
/// on_screen = false means the gun points away from the screen (the ports read 0: off-screen).
void tc_core_set_gun(float u, float v, bool on_screen);
/// Trigger state. A press shorter than a frame is still seen (held for 3 emulated frames).
void tc_core_set_trigger(bool down);
/// Pedal down = out of cover (the arcade pedal: release it to hide and reload).
void tc_core_set_pedal(bool down);
/// One coin (a 100 ms pulse on the coin switch; several calls queue).
void tc_core_insert_coin(void);
/// Time Crisis has no start button: the trigger starts a credited game. This pulses the trigger for 100 ms.
void tc_core_press_start(void);

/// Event callback (called on the game thread; keep it short). NULL clears it.
void tc_core_set_event_callback(tc_core_event_fn fn, void *user);

/// Measured core rate over the last ~5 s (frames per second, 0 before), for status displays.
double tc_core_fps(void);

#ifdef __cplusplus
}
#endif

#endif // TC_CORE_H
