# Time Crisis on iPad / Apple Vision Pro (experimental)

An iPad app build of `tc`. Apple Vision Pro runs it as a compatible iPad app (a floating window). It is not a
native visionOS app, because visionOS marks OpenGL ES `API_UNAVAILABLE` for native apps; iPad apps keep it.

This is a **personal build**. The app bundle carries your ROM folder (`timecris/extracted/`), and the build turns
parts of the ROMs into C. Never distribute the result.

## How it works

- `ios/tc_ios.c` is the entry point (`SDL_main`). It switches to the app's writable preferences folder, because the
  engine writes its settings and EEPROM to relative paths. Then it calls `tc_game.c`'s `main`, which is compiled as
  `tc_main`, with the bundle's `extracted/` folder and `--window`.
- The engine draws with desktop OpenGL 2.1. [gl4es](https://github.com/ptitSeb/gl4es) provides that API on top of
  the OpenGL ES 2 context SDL creates. On iOS, `engine/eng_gl.h` takes `<GL/gl.h>` from gl4es and sends
  `SDL_GL_GetProcAddress` lookups to gl4es. The offscreen render target (`engine/render_target.c`) is off, so the
  game draws straight into SDL's framebuffer.
- Input (`ENG_TOUCH`, in `engine/ss22_input.c`):
  - A tap aims and shoots. On Vision Pro, that is looking at a spot and pinching.
  - The pedal is held, so you are out of cover, until two fingers are down (both hands pinching). Then you duck and
    reload.
  - With a game controller connected, the usual pad mapping applies instead.

## Dependencies (built once per SDK slice)

Static SDL2 2.32.x and gl4es, for `iphonesimulator` and `iphoneos`, arm64, with an iOS 17.0 deployment target.
Each slice is installed into a prefix with `include/`, `include/gl4es/` and `lib/` (`libSDL2.a`, `libSDL2main.a`,
`libgl4es.a`).

- **SDL2** needs a patch to adopt the UIScene life cycle (the iOS 27 SDK refuses to launch apps without it).
- **gl4es** needs a small Apple-only patch in `src/gl/directstate.c`, because Mach-O has no `__attribute__((alias))`.

## Build

```
cmake -S timecris -B timecris/build-iossim -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DTC_ALLOW_CLANG=ON -DTC_IOS_PREFIX=<prefix-iossim> -DPython3_EXECUTABLE=<python with numpy>
cmake --build timecris/build-iossim --target tc --config RelWithDebInfo
```

For a device, use `iphoneos` with the device prefix, and set a signing team in the generated Xcode project. The ROMs
must already be in `timecris/extracted/`; `tools/setup_roms.py` puts them there.
