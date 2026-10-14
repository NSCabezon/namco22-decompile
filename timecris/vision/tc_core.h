// tc_core.h -- the C API the Time Crisis core (ss22 + glmetal, timecris/vision) exposes to the visionOS shell.
//
// THE SAME FILE lives in the app as ~/Apps/tc-visionos/app/Bridge/tc_core.h: change both together (README.md here).
// The shell's Swift `GameCore` protocol maps 1:1 onto these calls (app: Sources/Core/CGameCore.swift, TC_REAL_CORE).
//
// Threading: tc_core_start spawns the core's own game thread (paced at the board's 59.906 Hz; it owns the audio output,
// unless the app pulls it: tc_core_set_external_audio) and renders with the Metal device it was given). Every other call is thread-safe and non-blocking (tc_core_stop waits
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

/// STEREO 3D (a 3D-film screen: off-axis parallax from the game's own geometry; tc-visionos PLAN.md level 2a).
/// `sep` is the eye separation in the game's view-space units ("3D strength"), `zc` the view depth drawn at zero parallax
/// (on the screen plane; nearer comes out of the screen, farther goes behind it). sep <= 0 turns stereo off: one mono
/// render per frame, exactly as without stereo. On: each frame is drawn once per eye (polygons only differ in screen x; the
/// sprites, text and HUD are identical in both eyes, at the screen plane). Takes effect from the next frame; any thread.
/// Measured (ENG_ZHIST: the area-weighted view depth of the polygons on screen, per 200-600 updates): attract mode median
/// ~80000 (scene windows 26000..190000), stage 1 in play ~90000..210000; together ~100000. The zoom is fixed: focal length
/// 772.6 px at 640 px width. The defaults put far objects (z -> infinity) at sep*772.6/zc = 7.7 px = 1.2 % of the width
/// behind the screen; crossed disparity is capped at TC_STEREO_MAX_CROSSED of the width (reached at z ~ 29000; nearer
/// polygons keep that much).
#define TC_STEREO_DEFAULT_SEP   1000.0
#define TC_STEREO_DEFAULT_ZC    100000.0
#define TC_STEREO_MAX_CROSSED   0.03
void tc_core_set_stereo(float sep, float zc);
/// The newest completed frame as a left/right pair of `id<MTLTexture>` (bridged, unretained; same format and lifetime as
/// tc_core_latest_frame). Returns the left eye (the mono frame when that frame was rendered without stereo) and sets
/// *right to the right eye -- or to the same texture as the return value for a mono frame, so `*right == left` means mono.
/// Both come from one published frame. NULL (and *right = NULL) before the first frame.
void *tc_core_latest_frame_stereo(uint64_t *frame_id, void **right);

/// PANORAMA (tc-visionos PLAN.md): extra views of the game's world beside the 4:3 picture, for flat panels around the player.
/// Each side view is the same update's 3D world (no HUD, text or sprites; the background, screen fade and gamma stay) seen from a
/// view turned about the vertical axis through the eye by exactly the picture's horizontal field of view (tc_core_view_fov), so
/// it shows the world immediately beside the centre picture: a panel hinged at the centre panel's edge and turned by that angle
/// about the player's head continues it seamlessly. `side_views_each_side` <= 0 turns it off (the frame is exactly as without it);
/// at most TC_PANORAMA_MAX_SIDE_VIEWS per side. `side_width` x `side_height` is each side view's render size: 0 = 2/3 of the
/// centre's; the picture is always 4:3 (the height follows the width). With stereo on, each side view is drawn per eye, with the
/// same off-axis parallax as the centre (in the side view's own frame). Takes effect from the next frame; any thread.
#define TC_PANORAMA_MAX_SIDE_VIEWS 1
void tc_core_set_panorama(int side_views_each_side, int side_width, int side_height);
/// A side view of the newest completed frame, as tc_core_latest_frame_stereo: `index` -1 = the view left of the centre, +1 = the
/// one right of it. Returns the left eye (or the mono image) and sets *right to the right eye (or the same texture when mono). All
/// views come from the one published frame that tc_core_latest_frame returns. NULL (and *right = NULL) when that frame was drawn
/// without the panorama, or for an index out of range.
void *tc_core_latest_frame_side(int index, uint64_t *frame_id, void **right);
/// The horizontal field of view, in radians, of the newest frame's 4:3 picture (from the game's zoom: 2 atan(320 / focal px));
/// it is also the yaw step between the panorama's views. Time Crisis: 45 degrees (0.7854). Place the panels with it: a panel at
/// distance d from the head is 2 d tan(fov/2) wide, the side panels turned by +-fov about the head.
float tc_core_view_fov(void);

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

/// EXTERNAL AUDIO: the app plays the game's sound itself (a RealityKit audio generator on the screen entity) instead of the
/// core's own CoreAudio output unit, which visionOS would place at the app's window. Call tc_core_set_external_audio(true)
/// BEFORE the first tc_core_start; the core then opens no output unit and the app pulls the sound from its audio thread.
/// The output is identical to the unit's (same ring, resampler, limiter, gain, volume): stereo at TC_AUDIO_SAMPLE_RATE, float32
/// in -1..1. While the core is paused (tc_core_stop) or before it starts, the pulls write silence and return false.
/// The pulls are realtime-safe: no allocation, no I/O, no lock the game thread takes (the core's ring is lock-free, single
/// producer / single consumer). Use ONE of the two pull styles, never both:
///  - tc_core_audio_pull: one consumer gets both channels (left[], right[]); call it from one thread at a time.
///  - tc_core_audio_pull_channel: two consumers (a left and a right emitter, each with its own render callback) share one
///    stream through a small splitter: whichever asks first pulls stereo from the core and the other channel's half waits for
///    its own callback, so the two stay sample-aligned. A channel that stops asking is dropped, not waited for. The splitter
///    holds a short os_unfair_lock (a copy of at most a few hundred frames), contended only if the two run on different threads.
#define TC_AUDIO_SAMPLE_RATE 48000
void tc_core_set_external_audio(bool on);
bool tc_core_audio_pull(float *left, float *right, int frames);
bool tc_core_audio_pull_channel(int channel, float *out, int frames);   ///< channel 0 = left, 1 = right

/// Event callback (called on the game thread; keep it short). NULL clears it.
void tc_core_set_event_callback(tc_core_event_fn fn, void *user);

/// Measured core rate over the last ~5 s (frames per second, 0 before), for status displays.
double tc_core_fps(void);

#ifdef __cplusplus
}
#endif

#endif // TC_CORE_H
