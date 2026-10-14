# Time Crisis core for the native visionOS app (`tc_core`)

The game as a static library that the visionOS app (`~/Apps/tc-visionos/app`) links. The library contains the
engine, the lifted program, the translated DSP and sound programs, and the glmetal renderer. In place of the SDL
window it has an external host. Its C API is `tc_core.h`. This is a **personal build**, because the app bundles
your ROMs.

## Files

| File | Purpose |
|---|---|
| `tc_core.h` | The C API. The app keeps an identical copy at `app/Bridge/tc_core.h`; change both together. |
| `tc_core.m` | The external host (`ENG_EXTERNAL_HOST`). It implements `ss22_host.h` and `ss22_input.h` in place of `engine/ss22_host.c` and `engine/ss22_input.c`, and implements the API. |
| `tc_game_core.c` | `src/tc_game.c` with `main` renamed `tc_main`. |
| `nosdl/SDL2/SDL.h` | Not SDL. It supplies only the SDL types and constants that the engine headers' tables name. It declares no SDL functions. |
| `tc_core_test.m` | A macOS harness for the same API. |

## How it runs

- **Game thread.** `tc_core_start` spawns the game thread: a 64 MB stack at user-interactive QoS. The thread runs
  the unchanged scheduler (`ss22_main` as a `--window` session).
- **Each emulated frame** ends in `ss22_host_frame()`. That function:
  1. applies the app's input;
  2. calls `ss22_draw` into glmetal's target, on the app's `MTLDevice` (`glmetal_set_device`);
  3. calls `glmetal_end_frame`, whose hook blits the target into one of three `RGBA8Unorm` textures and publishes
     that texture when its command buffer completes (`tc_core_latest_frame`);
  4. paces to 59.906 Hz with `mach_wait_until`.
- **Input.** The setters write atomics. The game thread reads them once per frame:
  - The gun maps to the ports, 68..694 and 43..284; off screen makes the ports read 0.
  - The trigger, the pedal (down = out of cover) and the coin go to the game's INPUTS bits.
  - A trigger press is held for at least 3 frames.
  - "Start" pulses the trigger, because Time Crisis starts on a shot.
- **The shot event.** `engine/ss22_out.c` calls `ss22_input_rumble` on each rising edge of the recoil solenoid
  (mcuout1). That call becomes `TC_EVENT_SHOT`. It fires only for shots the game accepted, not on an empty gun.
- **Sound.** `engine/audio_out.c` has an `ENG_AUDIO_COREAUDIO` backend: a RemoteIO output unit (the default output
  on macOS) pulls the same lock-free ring that the SDL callback did, at 48 kHz stereo s16.
- **External sound (the app).** `tc_core_set_external_audio(true)`, called before the first `tc_core_start`, opens
  no output unit, so visionOS does not place the sound at the app's window. The app pulls the same ring from its own
  audio thread instead: `tc_core_audio_pull` gives both channels, and `tc_core_audio_pull_channel` serves one emitter per
  channel through a small splitter. The output is float32 at 48 kHz (`TC_AUDIO_SAMPLE_RATE`). The ring, resampler,
  limiter and gain are the same as with the output unit. Pulls write silence while the core is paused. The
  `[TC_CORE]` stats line prints the app's pull rate, which should read about 48000 Hz. To test it on macOS, run
  `TC_TEST_EXTAUDIO=1 tc_core_test ...`.
- **Files.** The working directory is set to `Application Support/TimeCrisis`, so `tc_eeprom.nv` is saved there.
  The MAME output socket is off (`SS22_OUT_PORT=0`), and there is no `tc_last.rec`.
- **Stop and resume.** `tc_core_stop` parks the game thread: audio is closed and the EEPROM saved. A later
  `tc_core_start` resumes the same game.
- **A fatal trap** in the lifted program ends the game thread, not the app.
- **Not compiled in:** `ss22_host.c`, `ss22_input.c`, `eng_ui.c` (the Nuklear menu), `eng_display.c`, `eng_ffb.c`,
  `render_target.c`, `win_startup.c`.

## Build

The ROMs must be in `timecris/extracted/`. `gen/` must already hold the generated C.

```sh
cd ~/Apps/namco22-decompile/timecris
PY=$HOME/Apps/namco22-decompile/.venv/bin/python
for v in xros:xros xrsim:xrsimulator; do d=${v%%:*}; sr=${v##*:}
  cmake -S . -B build-$d -G Ninja -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=$sr -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DTC_ALLOW_CLANG=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPython3_EXECUTABLE=$PY
  cmake --build build-$d --target tc_core
done
rm -rf build-vision/TCCore.xcframework
xcodebuild -create-xcframework -library build-xros/libtc_core.a -library build-xrsim/libtc_core.a \
  -output build-vision/TCCore.xcframework
```

The app's `project.yml` links `build-vision/TCCore.xcframework`, plus `libz` and AudioToolbox. Rebuild the app after
you rebuild the xcframework.

## macOS harness

```sh
CC=clang cmake -S . -B build-coremac -G Ninja -DTC_ALLOW_CLANG=ON -DTC_CORE_MAC=ON -DPython3_EXECUTABLE=$PY
cmake --build build-coremac --target tc_core_test
./build-coremac/tc_core_test extracted /tmp/coreshots 60
```

The harness runs attract mode, then stop and resume, then three coins and start, then play with a trigger every 0.5 s.
It saves frames as PPM files and counts `TC_EVENT_SHOT`.

## Stereo 3D (tc-visionos PLAN.md level 2a)

- `tc_core_set_stereo(sep, zc)`: sep <= 0 is mono, which is the same single render as before. With stereo on, each
  update is walked once (`ss22_prepare`), then drawn twice (`ss22_draw_eye`): the right eye into glmetal target 1, the
  left eye into target 0 (`glmetal_set_target`). The hook publishes both images in one slot, and
  `tc_core_latest_frame_stereo` returns them as a pair.
- The eyes differ only in the polygons' screen x. `geo_hw_quad_eye` shifts each vertex by `e*(sep/2)*K*(1/zc - 1/z)`.
  That is the off-axis projection `X - e*sep/2`, recentred at zc. It covers rv and the guard-band dv, and keeps the
  mono numbers so eye 0 restores them exactly. Crossed disparity is capped at 3 % of the width.
- These run once per update, so both eyes share them: the walk, the sort, the texture bakes and their budget (keyed on
  `g_eng_frame`), the sprites, the text, the fade and the gamma.
- Defaults: sep 1000, zc 100000. They were measured with `ENG_ZHIST=<n>` (the area-weighted view depth on screen) over
  attract mode and stage 1. Far objects end up 1.2 % of the width behind the screen. `tc_core.h` has the numbers.
- Checks:
  - `ENG_STEREO=<sep>,<zc> tc_metal ... --shots DIR N` also writes `_L` and `_R` PPMs. The mono shot is unchanged.
  - `TC_TEST_STEREO=1 tc_core_test` saves pairs.
  - M1 Max: draw+encode is 2.6 ms mono and 3.4 ms stereo.

## Panorama (tc-visionos PLAN.md)

- `tc_core_set_panorama(1, w, h)` adds a side view on each side of the picture; 0 turns it off (the frame is exactly as before).
  `tc_core_latest_frame_side(-1 | +1, ...)` returns them like `tc_core_latest_frame_stereo` (per eye with stereo on), and
  `tc_core_view_fov()` the yaw step: the picture's horizontal FOV, `2 atan(320 / K)` with K = zoom_mant / 2^zoom_shift px.
  Time Crisis's focal length is 772.6 px, so 45.0 degrees. The core takes the zoom most full-frame quads of the update use.
- Side views are 4:3 at 2/3 of the centre's size by default. They show the 3D world alone: no sprites, no text layer (so no
  HUD), but the background, the screen fade and the gamma. Quads in a sub-window viewport are left out.
- How: the walk stays once per update and unchanged. Each walked quad now keeps its view-space vertices, UVs, brightness and
  zsort inputs (`geo_quad.wx..zs_ap`); `geo_hw_quad_view` rotates them about the vertical axis through the eye and runs the
  walk's own zsort, near clip, projection and guard band again. Back-face culling is unchanged by a rotation about the eye (a
  triple product), and so is the lighting. `ss22_draw_view(vw, vh, eye, yaw, layers2d)` builds and sorts the turned quads once
  per update and yaw, culls the ones outside the turned picture, then draws them; the stereo eye shift applies in the side's
  own frame. glmetal has up to 8 targets, each with its own size (`glmetal_set_target_size`): 2/3 are the sides, 4/5 their
  right eyes.
- Checks: `ENG_PANO=1 tc ... --shots DIR N` also writes `_Lside` / `_Rside` (with `ENG_STEREO`, per eye). The default shots
  stay byte-identical. With ENG_PANO on, a few centre pixels can differ, because a texture baked at a higher size tier for a
  side view is reused (tex_bake.c tier fallback). `TC_TEST_PANORAMA=1 tc_core_test` saves the core's side views.
- Cost (M-series Mac, in play): draw+encode goes from 2.5 to 3.0 ms mono, and from 3.4 to 3.9 ms stereo.
- Known limit: Time Crisis draws its explosions and effects as sprites, which have no 3D position, so they appear only in the
  centre picture and are cut at its edge.

## Notes

- **The simulator GPU** reports the Apple1 family but cannot read the render target. glmetal probes the
  framebuffer-fetch pipeline and falls back to the ping-pong gamma pass.
- **The log** goes to stderr. Every 300 frames it prints a `[TC_CORE]` line: the rate, the work per frame (the
  emulation plus draw and encode), late frames and shots.
