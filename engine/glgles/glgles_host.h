/*
 * glgles/glgles_host.h -- the host-facing API of the OpenGL ES 3.0 backend (engine/glgles/glgles.c), the GLES mirror of
 * glmetal's own glmetal_* API. Plain types only (no GL typedefs), so it can be included anywhere -- in particular by the
 * Quest host (cores/namco22/vision/tc_core_gl.c), which also includes the real <GLES3/gl3.h> and must not pull in
 * glgles/gl.h's GLenum / GL_* definitions.
 *
 * All of these need ONE OpenGL ES 3.0 context current on the calling thread (the game thread), the same context for the
 * backend's whole life -- exactly as m3_render_gles.h requires. glgles_create makes the backend's GL objects; the engine's
 * legacy GL subset (glgles/gl.h) then draws into the current target's FBO; the host publishes the target colour textures
 * (bottom-up rows, as m3_core_latest_frame_gl) with a fence from glgles_end_frame.
 */
#ifndef GLGLES_HOST_H
#define GLGLES_HOST_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* STEREO / PANORAMA: as glmetal, every draw/clear/readback/post goes to the target selected by glgles_set_target (0 = the
 * one glgles_create made). Targets 0..GLGLES_MAX_TARGETS-1, each its own colour texture + depth buffer + FBO, made on first
 * use. The host uses two targets per published frame for stereo (left, right) across a triple-buffered ring of slots. */
#define GLGLES_MAX_TARGETS 8

/* Make the backend: target 0 is a w x h RGBA8 colour texture with a depth buffer, the default framebuffer. Loads the real
 * GLES entry points, builds the uber-shader program and the gamma program, the immediate-mode VBO ring and the vertex
 * array. 1 on success, 0 if the context is not ES 3.0-capable or an object cannot be made (a reason on stderr). */
int  glgles_create(int w, int h);
/* Delete every GL object the backend made (textures, targets, programs, buffers, sync). Needs the context current (call it
 * from the game thread, e.g. in ss22_host_close). The C state is reset by tc_reset on a quit, as glmetal's is. */
void glgles_destroy(void);
int  glgles_width(void);
int  glgles_height(void);

void     glgles_set_target(int i);
void     glgles_set_target_size(int i, int w, int h);  /* w or h < 1: back to target 0's size */
int      glgles_current_target(void);
/* Target i's current colour texture as a GL texture name (GL_TEXTURE_2D, GL_RGBA8), 0 if never made. Row 0 (t = 0) is the
 * BOTTOM of the picture (GL's own orientation: no flip needed into an OpenXR GLES swapchain image), as m3_core_latest_frame_gl. */
unsigned glgles_target_texture_n(int i);
unsigned glgles_target_texture(void);                  /* the active target's colour texture name */
int      glgles_target_bottom_up(void);                /* always 1 here (native GL rows) */

/* End the frame: flush the pending draw, glFlush, and return a fresh GLsync (cast to void *) the caller publishes so the
 * app can glWaitSync on it before sampling the target textures. The backend does not wait. */
void *glgles_end_frame(void);

/* GL entry points by name (what SDL_GL_GetProcAddress is routed to under ENG_GL_GLES); NULL for shaders / framebuffer
 * objects, so render_target.c stays off and post_gl.c takes glgles_post_lut, exactly as it does glmetal_post_lut. */
void *glgles_GetProcAddress(const char *name);

/* The final per-channel colour table over the active target's (0,0,vw,vh) rectangle, in place (framebuffer fetch on Adreno;
 * elsewhere a copy-and-redraw). Alpha becomes 1. The GLES equivalent of glmetal_post_lut / post_gl.c's GLSL pass. */
void glgles_post_lut(const uint8_t lut[3][256], int vw, int vh);

#ifdef __cplusplus
}
#endif
#endif
