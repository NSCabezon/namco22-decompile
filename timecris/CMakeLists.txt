cmake_minimum_required(VERSION 3.16)
project(timecris C)
# Time Crisis (SUPER System 22) on the shared Super 22 host (../engine/ss22_*): the lifted 68K (gen/tc_lifted.c, tools/namco22/lift.py) and ONE table
# (src/tc_game.c). Dirt Dash's build is the template (../dirtdash/CMakeLists.txt). HARD RULE 3: no copies of the engine.
option(TC_ALLOW_CLANG "Experimental: build with clang (needed for Apple platforms other than macOS)" OFF)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" AND NOT TC_ALLOW_CLANG)
  message(FATAL_ERROR "gcc required (the lifted code uses computed labels and statement expressions)")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-fno-strict-aliasing -fwrapv)
option(PORTABLE "Bind glibc symbols to baseline versions (runs on glibc >= 2.34)" ON)
if(PORTABLE AND NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/../include/glibc_compat.h)
endif()
include_directories(include gen)
include(${CMAKE_CURRENT_SOURCE_DIR}/../engine/engine.cmake)

# gen/tc_lifted_NN.c + gen/tc_lifted_tab.c:
#   python3 ../tools/namco22/lift.py decompiled/pcode.txt gen tc timecris_main.bin && python3 ../tools/namco22/lift_split.py gen/tc_lifted.c 16
# 58 MB of goto-heavy C in ONE file needed more than 8 GB of gcc; the parts (shared tool lift_split.py) need about 1 GB each. -O1 is plenty.
file(GLOB TC_LIFTED CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen/tc_lifted_*.c)
set_source_files_properties(${TC_LIFTED} PROPERTIES COMPILE_OPTIONS "-O1;-w")

# The MASTER DSP program (block at ROM 0xABA8C), translated from the ROM by ../tools/gen/c25_translate.py --game tc. tools/gen/c25.cov is EMPTY until
# the oracle build has run the game: every address is then a static one and anything the oracle would need traps.
# --block: the game's program (0xABA8C) and the TEST-MODE master programs the operator menu's DSP tests upload, listed by the 68K's
# table at ROM 0xB91C4 (12 entries; GitHub #30 follow-up, 2026-10-05: without them test mode stopped the translated master at 0x400B..0x413E).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/tc_c25.c
  COMMAND ${Python3_EXECUTABLE} ../tools/gen/c25_translate.py --game tc --roms extracted --main-rom timecris_main.bin
          --block ABA8C --block B91F4 --block B91F8 --block B9212 --block B93AE --block B9536 --block B9626 --block B9804
          --block B9CAE --block B9D3C --block BA11A --block BA580 --block BAE16
          --cov tools/gen/c25.cov --out gen/tc_c25.c --func tc_c25_exec
  DEPENDS ../tools/gen/c25_translate.py tools/gen/c25.cov ../engine/c25/c71_bios.inc
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Translating the master DSP program to C")
set_source_files_properties(gen/tc_c25.c PROPERTIES COMPILE_OPTIONS "-O2;-w")

# visionOS (-DCMAKE_SYSTEM_NAME=visionOS): only the tc_core library the native app links (vision/README.md). No SDL, no OpenGL.
set(TC_VISION OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
  set(TC_VISION ON)
endif()

if(TC_VISION)
  # zlib (engine/romzip.c) comes from the SDK; the app links it (-lz)
elseif(IOS)
  # iPad app (also Apple Vision Pro's iPad mode): static SDL2 + gl4es built for this SDK slice into TC_IOS_PREFIX
  # (include/, include/gl4es/, lib/). The desktop OpenGL 2.1 the engine draws with comes from gl4es over OpenGL ES 2.
  set(TC_IOS_PREFIX "" CACHE PATH "Install prefix holding SDL2 and gl4es for this iOS slice")
  if(NOT TC_IOS_PREFIX)
    message(FATAL_ERROR "set TC_IOS_PREFIX (see ios/README.md)")
  endif()
  set(SDL2_INCLUDE_DIRS ${TC_IOS_PREFIX}/include ${TC_IOS_PREFIX}/include/SDL2 ${TC_IOS_PREFIX}/include/gl4es)
  find_package(ZLIB REQUIRED)
else()
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
# macOS (Homebrew sdl2-compat): no SDL2main library, and the libs live outside the default search path
if(APPLE)
  list(REMOVE_ITEM SDL2_LIBRARIES SDL2main)
  link_directories(${SDL2_LIBRARY_DIRS})
endif()
if(WIN32)
  set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
endif()
include_directories(${SDL2_INCLUDE_DIRS})
# Windows (MinGW-w64): same block as Dirt Dash -- win_compat.h, no console, everything static; OpenGL resolved at run time (gl_dyn.c)
if(WIN32)
  add_compile_options(-include ${CMAKE_CURRENT_SOURCE_DIR}/../include/win_compat.h)
  add_link_options(-mwindows -static-libgcc -static)
  set(SDL2_LIBRARIES ${SDL2_STATIC_LDFLAGS} ws2_32)   # engine/ss22_out.c: the MAME network-output socket (Winsock2)
  set(TC_GL_LIB "")
  set(TC_GL_SRC ${NAMCO22_ENGINE_DIR}/gl_dyn.c)
elseif(IOS OR TC_VISION)
  set(TC_GL_LIB "")
  set(TC_GL_SRC "")
else()
  set(TC_GL_LIB OpenGL::GL)
  set(TC_GL_SRC "")
endif()

# The SOUND MCU program (ts1data.8k), translated to C from the ROM file by the shared tools/gen/snd_translate.py (--game tc). tools/gen/snd.cov: the
# (address, M/X, length) of every instruction the oracle build executed (addresses only, no ROM data).
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/tc_snd_driver.c
  COMMAND ${Python3_EXECUTABLE} ../tools/gen/snd_translate.py tools/gen/snd.cov --game tc --roms extracted
          --out gen/tc_snd_driver.c
  DEPENDS ../tools/gen/snd_translate.py ../tools/gen/snd_rom.py ../tools/gen/snd_spin.inc ../tools/gen/m7700_decode.py tools/gen/snd.cov
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Translating the sound MCU program to C")
set_source_files_properties(gen/tc_snd_driver.c PROPERTIES COMPILE_OPTIONS "-O1;-w")

set(TC_CORE_SRC src/tc_game.c
    ${NAMCO22_ENGINE_LIFT_SRC} ${NAMCO22_ENGINE_C25_SRC} ${NAMCO22_ENGINE_SND_SRC}
    ${NAMCO22_ENGINE_GL_SRC} ${NAMCO22_ENGINE_SS22_SRC} ${NAMCO22_ENGINE_SS22_HOST_SRC} ${NAMCO22_ENGINE_UI_SRC} ${NAMCO22_ENGINE_ROMZIP_SRC} ${TC_GL_SRC}
    ${TC_LIFTED} gen/tc_c25.c)

# THE GAME: the 68K lifted, the master DSP and the sound MCU translated -- no interpreter anywhere (HARD RULE 1/2)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/snd.cov AND IOS)
# The app bundle carries the ROM folder (extracted/) as a resource: a personal build only, never to be distributed.
file(GLOB TC_IOS_ROMS ${CMAKE_CURRENT_SOURCE_DIR}/extracted/*)
set_source_files_properties(${TC_IOS_ROMS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/extracted)
# tc_game.c's main() becomes tc_main, called by ios/tc_ios.c's SDL_main; SDL_MAIN_HANDLED stops SDL_main.h renaming it first
set_source_files_properties(src/tc_game.c PROPERTIES COMPILE_DEFINITIONS "main=tc_main;SDL_MAIN_HANDLED")
add_executable(tc MACOSX_BUNDLE ${TC_CORE_SRC} gen/tc_snd_driver.c ios/tc_ios.c ${TC_IOS_ROMS})
target_compile_options(tc PRIVATE -Wall)
target_compile_definitions(tc PRIVATE ENG_TOUCH)    # engine/ss22_input.c: the pedal on a touch screen
set_target_properties(tc PROPERTIES
    OUTPUT_NAME "TimeCrisis"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER "local.namco22.timecris"
    MACOSX_BUNDLE_BUNDLE_NAME "Time Crisis"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "local.namco22.timecris")
target_link_libraries(tc
    ${TC_IOS_PREFIX}/lib/libSDL2main.a ${TC_IOS_PREFIX}/lib/libSDL2.a ${TC_IOS_PREFIX}/lib/libgl4es.a ZLIB::ZLIB m pthread
    # SDL2's sdl2.pc for iOS
    "-framework CoreVideo" "-framework CoreAudio" "-framework AudioToolbox" "-framework AVFoundation" "-framework CoreBluetooth"
    "-framework CoreGraphics" "-framework CoreMotion" "-framework Foundation" "-framework Metal" "-framework OpenGLES"
    "-framework QuartzCore" "-framework UIKit" "-weak_framework GameController" "-weak_framework CoreHaptics")
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/snd.cov AND NOT TC_VISION)
add_executable(tc ${TC_CORE_SRC} gen/tc_snd_driver.c)
target_compile_options(tc PRIVATE -Wall)
target_link_libraries(tc ${SDL2_LIBRARIES} ${TC_GL_LIB} ZLIB::ZLIB m)
endif()

# tc_metal (macOS): the same game drawn through glmetal (../engine/glmetal), the engine's OpenGL subset on Metal -- the renderer the visionOS
# app will use, where there is no OpenGL. Headless only for now (--shots), checked frame by frame against tc's. Links no OpenGL.
if(APPLE AND NOT IOS AND NOT TC_VISION AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/snd.cov)
  enable_language(OBJC)
  set(TC_GLMETAL_SRC ${NAMCO22_ENGINE_DIR}/glmetal/glmetal.m)
  set_source_files_properties(${TC_GLMETAL_SRC} PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall")
  add_executable(tc_metal ${TC_CORE_SRC} gen/tc_snd_driver.c ${TC_GLMETAL_SRC})
  target_compile_options(tc_metal PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>)
  target_compile_definitions(tc_metal PRIVATE ENG_GL_METAL)
  target_link_libraries(tc_metal ${SDL2_LIBRARIES} ZLIB::ZLIB m "-framework Metal" "-framework Foundation")
endif()

# DEV builds (HARD RULE 2: the DSP and the sound MCU INTERPRETED, never shipped): the window / headless run, and the instruction trace for the T3 proof
if(EXISTS ${NAMCO22_TOOLS_DIR}/sndoracle/sound_oracle.c AND NOT TC_VISION)
add_executable(tc_oracle ${TC_CORE_SRC} ${NAMCO22_C25_ORACLE_SRC}
    ${NAMCO22_TOOLS_DIR}/sndoracle/m37710.c ${NAMCO22_TOOLS_DIR}/sndoracle/sound_oracle.c)
target_compile_options(tc_oracle PRIVATE -Wall)
target_compile_definitions(tc_oracle PRIVATE TC_ORACLE SS22_ORACLE C25_DEV_HOOKS)
target_link_libraries(tc_oracle ${SDL2_LIBRARIES} ${TC_GL_LIB} ZLIB::ZLIB m)

add_executable(tc_trace ${TC_CORE_SRC} ${NAMCO22_C25_ORACLE_SRC}
    ${NAMCO22_TOOLS_DIR}/sndoracle/m37710.c ${NAMCO22_TOOLS_DIR}/sndoracle/sound_oracle.c)
target_compile_options(tc_trace PRIVATE -Wall)
target_compile_definitions(tc_trace PRIVATE RR_TRACE TC_ORACLE SS22_ORACLE C25_DEV_HOOKS)
target_link_libraries(tc_trace ${SDL2_LIBRARIES} ${TC_GL_LIB} ZLIB::ZLIB m)
endif()

# tc_core: Time Crisis as a static library for the NATIVE visionOS app (~/Apps/tc-visionos/app; vision/README.md). The game drawn through
# glmetal into the app's MTLDevice, the engine's SDL window host and input replaced by vision/tc_core.m (ENG_EXTERNAL_HOST: the C API
# vision/tc_core.h), sound through CoreAudio (ENG_AUDIO_COREAUDIO). No SDL anywhere: vision/nosdl/SDL2/SDL.h gives the engine headers
# the few SDL types their tables name. Built for visionOS (device / simulator), or on macOS with -DTC_CORE_MAC=ON together with
# tc_core_test, a headless harness that drives the same API (frames read back, coins, shots, pause/resume).
option(TC_CORE_MAC "macOS: also build the visionOS core library (tc_core) and its harness (tc_core_test)" OFF)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/snd.cov AND (TC_VISION OR (APPLE AND NOT IOS AND TC_CORE_MAC)))
  enable_language(OBJC)
  set(TC_CORE_LIB_SRC ${TC_CORE_SRC} gen/tc_snd_driver.c ${NAMCO22_ENGINE_DIR}/glmetal/glmetal.m vision/tc_core.m vision/tc_game_core.c)
  list(REMOVE_ITEM TC_CORE_LIB_SRC src/tc_game.c       # vision/tc_game_core.c: the same file with main() named tc_main
       ${NAMCO22_ENGINE_DIR}/ss22_host.c ${NAMCO22_ENGINE_DIR}/ss22_input.c ${NAMCO22_ENGINE_DIR}/eng_ffb.c ${NAMCO22_ENGINE_DIR}/render_target.c
       ${NAMCO22_ENGINE_DIR}/eng_display.c ${NAMCO22_ENGINE_DIR}/eng_ui.c ${NAMCO22_ENGINE_DIR}/win_startup.c)
  set_source_files_properties(${NAMCO22_ENGINE_DIR}/glmetal/glmetal.m vision/tc_core.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall")
  add_library(tc_core STATIC ${TC_CORE_LIB_SRC})
  target_include_directories(tc_core BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vision/nosdl ${CMAKE_CURRENT_SOURCE_DIR}/vision)
  target_compile_definitions(tc_core PRIVATE ENG_GL_METAL ENG_EXTERNAL_HOST ENG_AUDIO_COREAUDIO)
  target_compile_options(tc_core PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>)
  if(NOT TC_VISION)
    add_executable(tc_core_test vision/tc_core_test.m)
    set_source_files_properties(vision/tc_core_test.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall")
    target_include_directories(tc_core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vision)
    target_link_libraries(tc_core_test tc_core ZLIB::ZLIB "-framework Metal" "-framework Foundation" "-framework AudioToolbox" "-framework CoreAudio")
  endif()
endif()
