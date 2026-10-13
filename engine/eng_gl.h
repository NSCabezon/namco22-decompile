/* eng_gl.h -- <GL/gl.h> plus the few post-1.1 constants the engine uses.
 * MinGW-w64's gl.h (the Windows build) stops at OpenGL 1.1; the functions
 * themselves are resolved at run time (gl_dyn.c, render_target.c, post_gl.c). */
#ifndef ENG_GL_H
#define ENG_GL_H
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(ENG_GL_METAL)
/* glmetal (engine/glmetal): the GL subset the engine uses, implemented on Metal -- no OpenGL library at all (visionOS
 * has none). The run-time lookups (glActiveTexture, the post shader, framebuffer objects) go to glmetal too, which
 * answers NULL for shaders and FBOs: render_target.c stays off, and post_gl.c takes glmetal_post_lut. */
#include "glmetal/gl.h"
#define SDL_GL_GetProcAddress glmetal_GetProcAddress
#elif defined(__APPLE__) && TARGET_OS_IPHONE
/* iOS / iPadOS (and Vision Pro's iPad mode): no desktop OpenGL. gl4es implements it over OpenGL ES 2; its
 * <GL/gl.h> is on the include path. Every GL entry point the engine looks up at run time must come from
 * gl4es, not from the GLES library SDL loaded, so SDL_GL_GetProcAddress is routed to gl4es (the prototype
 * matches, so SDL_video.h's declaration may come before or after this). See ios/tc_ios.c. */
#include <GL/gl.h>
#include <GL/glext.h>
#ifndef APIENTRY
#define APIENTRY
#endif
void *eng_gl_proc(const char *name);
#define SDL_GL_GetProcAddress eng_gl_proc
void eng_gl_es_ready(void *sdl_window);  /* right after the window's context is current: start gl4es on it */
void eng_gl_pre_swap(void);              /* around SDL_GL_SwapWindow: gl4es flushes its batches */
void eng_gl_post_swap(void);
#define ENG_GL_ES_BACKEND 1
#elif defined(__APPLE__)
/* macOS ships legacy OpenGL 2.1 as a framework, deprecated since 10.14 but still present */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#ifndef APIENTRY
#define APIENTRY
#endif
#else
#include <GL/gl.h>
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_COMBINE
#define GL_COMBINE     0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_RGB_SCALE   0x8573
#endif
/* GL 1.3 multitexture + combiners (the single-pass fog in quad_gl.c) */
#ifndef GL_VERSION_1_3
#define GL_TEXTURE0        0x84C0
#define GL_TEXTURE1        0x84C1
#define GL_COMBINE_ALPHA   0x8572
#define GL_INTERPOLATE     0x8575
#define GL_CONSTANT        0x8576
#define GL_PRIMARY_COLOR   0x8577
#define GL_PREVIOUS        0x8578
#define GL_SOURCE0_RGB     0x8580
#define GL_SOURCE1_RGB     0x8581
#define GL_SOURCE2_RGB     0x8582
#define GL_SOURCE0_ALPHA   0x8588
#define GL_SOURCE1_ALPHA   0x8589
#define GL_SOURCE2_ALPHA   0x858A
#define GL_OPERAND0_RGB    0x8590
#define GL_OPERAND1_RGB    0x8591
#define GL_OPERAND2_RGB    0x8592
#define GL_OPERAND0_ALPHA  0x8598
#define GL_OPERAND1_ALPHA  0x8599
#define GL_OPERAND2_ALPHA  0x859A
#endif
#endif
