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

## Notes

- **The simulator GPU** reports the Apple1 family but cannot read the render target. glmetal probes the
  framebuffer-fetch pipeline and falls back to the ping-pong gamma pass.
- **The log** goes to stderr. Every 300 frames it prints a `[TC_CORE]` line: the rate, the work per frame (the
  emulation plus draw and encode), late frames and shots.
