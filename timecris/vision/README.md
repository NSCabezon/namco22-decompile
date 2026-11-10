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
| `tc_reset.h`, `tc_reset.c`, `tc_reset_core.h` | `tc_core_quit`'s reset: the game's variables in two sections of their own, its heap blocks noted. |

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
  `tc_core_start` resumes the same game (the app's Home).
- **Quit.** `tc_core_quit` ends the game (the app's Exit); the next `tc_core_start` boots a new one from power-on, exactly as
  a new process would. See "Quit" below.
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

The xcframework also carries the **iPhone** slices (gun-games-visionos `app/README.md` "iPhone": the same core in the
iPhone app). `-DTC_CORE_IOS=ON` builds this `tc_core` for iOS instead of the SDL + gl4es iPad app (`ios/`); nothing in the
sources is iOS-specific (glmetal probes the simulator's missing framebuffer fetch as on visionOS). Build both and pack the
four slices (the visionOS ones as built above):

```sh
for v in iosv:iphoneos iossimv:iphonesimulator; do d=${v%%:*}; sr=${v##*:}
  cmake -S . -B build-$d -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sr -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 -DTC_ALLOW_CLANG=ON -DTC_CORE_IOS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DPython3_EXECUTABLE=$PY
  cmake --build build-$d --target tc_core
done
rm -rf build-vision/TCCore.xcframework
xcodebuild -create-xcframework -library build-xros/libtc_core.a -library build-xrsim/libtc_core.a \
  -library build-iosv/libtc_core.a -library build-iossimv/libtc_core.a -output build-vision/TCCore.xcframework
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

## Quit (`tc_core_quit`)

`tc_core_stop` + `tc_core_start` pause and resume one game. `tc_core_quit` ends it, and the next `tc_core_start` (with its
arguments read again) boots the cabinet from power-on.

- **How the game thread leaves.** The decompiled program never returns (`entry_reset` runs the game's own loop). The only place
  the game thread waits is `park_if_asked`, at the end of `ss22_host_frame`, the frame boundary. `tc_core_quit` sets `want_quit`
  and `want_pause`. The thread parks there exactly as for `tc_core_stop` (sound closed, EEPROM saved), then `_longjmp`s back to
  `game_main`. `game_main` runs the engine's own close sequence for a closed window (`ss22_eeprom_save`, `ss22_snd_close`,
  `ss22_out_close`, `ss22_input_close`, `ss22_host_close`) and returns; `tc_core_quit` joins it. The jump skips `rr_tick`, the
  lifted 68K's C frames, `ss22_main` and `tc_main`. These hold no lock (`mu` is released first), no autorelease pool, no ObjC
  object and no open file. All their state is global and every heap block is noted, so the reset below covers it. A game thread
  that already ended (a fatal trap, or a game that could not start) is simply joined.
- **What is freed.** glmetal (`glmetal_destroy`: waits for the GPU, then releases the textures, buffers, pipelines, targets,
  queue and device), the frame and side-view textures of `tc_core.m`, and every heap block the engine's C code still holds.
- **How the globals are reset.** `tc_reset.h` is force-included (`-include`) into every C source of `tc_core` and into
  `glmetal.m`. It puts each global and `static` variable of those files, `static` locals included, into `__DATA,__tcg_data`
  (initialised) or `__DATA,__tcg_bss` (zero-fill) with `#pragma clang section`. That covers the 68K RAM images and register file,
  the DSP, the sound MCU, the scheduler, the renderer's caches, glmetal's state and the `getenv` caches. In the C sources
  `malloc`, `calloc`, `realloc`, `free` and `strdup` become `tcg_*`. These are the same allocations, and each live block is
  noted; blocks of 1 MB or more are their own mapped pages, so a quit gives them straight back to the system. `tc_reset.c`
  copies `__tcg_data` in a constructor, at image load and before any game code runs. `tcg_reset` frees the noted blocks, copies
  `__tcg_data` back and zero-fills `__tcg_bss` (whole pages are mapped anew). After `tcg_reset`, every variable of the game's
  code holds its load-time value. Only `tc_core.m` (the API's locks, the app's settings, the frame-id counter) and `tc_reset.c`
  sit outside the sections. `tc_core.m` resets its per-game part itself (`host_reset`, `input_reset`), and re-applies
  `tc_core_set_external_audio`.
  `size -m` on the objects shows nothing else in `__DATA,__data` or `__bss`. The sections hold 528 + 20,359,024 bytes. The
  pragma only moves the variables; the code is the same, and the default runs hash identically (below).
- **Threads.** The app's audio thread can keep calling `tc_core_audio_pull*` through a quit. A gate (`pull_gate` and `pulls_in`)
  keeps those calls out of `audio_out.c` while it is reset; they write silence. The frame getters must be called from the same
  thread as `tc_core_quit` (the app's main thread). Frames returned before the quit must not be used after it. The getters
  return NULL until the next game's first frame. The frame ids keep increasing across games.
- **Kept across a quit:** the event callback, the stereo, panorama and external-audio settings, and the gun, pedal and trigger
  state. Presses, starts and coins made before the quit are dropped.

### The proof (macOS, M1 Max)

`TC_TEST_QUIT=<cycles>[:<frames>] tc_core_test <roms> <out>`:
- It boots cold and hashes the first frames.
- Then, `<cycles>` times, it plays (3 coins, start, 16 shots in and out of cover; in the second cycle it also pauses), calls
  `tc_core_quit`, boots again and hashes again. The quit runs once from the paused state.
- Every boot starts from the same EEPROM (`<out>/eeprom0.nv`) and the same cabinet input.
- `TC_CORE_HASHLOG` makes the core log, for each frame, the board memory (`g_ss22` from work RAM on, `g_hw`, the 68K register
  file `R`) at the frame boundary and a hash of the published picture. `TC_CORE_DATA_DIR` keeps the test out of the user's data
  folder. Compare the sessions with `tools/tc_hashlog_cmp.py`.

```sh
mkdir -p /tmp/q && cp <a formatted tc_eeprom.nv> /tmp/q/eeprom0.nv
ENG_TEX_BUDGET=0 TC_TEST_QUIT=3:600 ./build-coremac/tc_core_test extracted /tmp/q
python3 tools/tc_hashlog_cmp.py <cold.log from a separate process> /tmp/q/hash.log
```

- **Hashes.** With `ENG_TEX_BUDGET=0`, all four boots are identical, frame by frame, over 600 frames: memory and pixels. They also
  match a cold boot by a separate process running the core from before this change. The same holds with external audio
  (`TC_TEST_EXTAUDIO=1`), and with stereo and panorama on (`TC_TEST_STEREO=1 TC_TEST_PANORAMA=1`).
- **The bake budget.** With the default budget (the window's, as in the app), the memory hashes are still identical in every
  boot. The pixels are not: the budget adapts to the measured bake speed. That is timing, not state. Two cold processes of the
  old core differ the same way from frame ~300.
- **Memory** (`phys_footprint`, test with hashing on): 4 MB before the first boot, about 500 MB in a game, and 33 to 51 MB
  after each of three quits. It does not grow from one quit to the next. The rest is Metal's and the frameworks' own state.
  A quit takes 8 to 31 ms. A later start takes about 150 ms (the first: 240 to 420 ms).
- **The default paths are unchanged.** The new core's cold boot hashes the same as the old core's. `tc_metal` (with
  `glmetal_destroy` added, never called) replays `tc_last.rec` (`--replay ... --shots DIR 30 --frames 2400`, 80 PPMs) to
  byte-identical shots. The default `tc_core_test` run (attract mode, stop and resume, play) still passes.

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

## Merging upstream

`visionos-port` follows `spacestate1/namco22-decompile` `main` (remote `origin`). The fork is the remote `fork`. To update:
`git fetch origin main:main && git push fork main`, then `git merge origin/main` on `visionos-port`, build the three trees
(`build`, `build-coremac`, `build-xros`) and run the harness (`tc_core_test extracted <out> 60`: 0 traps, about 60 fps,
shots reported).

The core builds without SDL. After a merge, two kinds of break are common:

- **A new engine file calls SDL.** The compile fails with `call to undeclared function 'SDL_...'`. If only the SDL host,
  input or UI (`ss22_host.c`, `ss22_input.c`, `eng_ui.c`) uses that file, add it to the `REMOVE_ITEM` list of `tc_core` in
  `timecris/CMakeLists.txt`. Otherwise guard the call with `ENG_EXTERNAL_HOST`. Don't add SDL functions to `nosdl/SDL2/SDL.h`.
- **`ss22_input.h` gains a function.** The link fails with `Undefined symbols ... _ss22_input_...`. `tc_core.m` replaces
  `ss22_input.c`, so implement the new function there.

Merge of 2026-11-10 (upstream 0.8.0 -> 0.10.3, merge `3987b7c`):

- It brought in the re-lifted Time Crisis 68K (`gen/tc_lifted_*`), with the same ROM, and the sound-translator fixes
  (`tools/gen/snd_dispatch.py`, `snd_indirect.py`, `snd_exits.py`; GitHub #62 and #95). It also brought the `__int128` backface cull
  (`geo_hw.c`), the guard-band draw polygon (`ss22_gl.c`), `g_tex_budget_paused` (full bakes in attract and menus), and `eng_crosshair`.
- Conflicts: `engine/ss22_host.c` (both includes kept) and `engine/tex_bake.c` (our `ENG_TEX_BUDGET_MS` time cap kept beside
  `g_tex_budget_paused`).
- Fixes in the core: `twin_stick.c` and `eng_crosshair.c` were left out of `tc_core` (both are SDL code), and `tc_core.m` gained
  `ss22_input_pad_rumble()`. That function always returns true, because the shell always receives `TC_EVENT_SHOT`.
