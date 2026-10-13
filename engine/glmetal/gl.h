/*
 * glmetal/gl.h -- the subset of legacy OpenGL (1.x fixed function + 1.3 multitexture/combiners) that the namco22 engine
 * calls, implemented on Metal by glmetal.m. It stands in for <GL/gl.h> when the engine is built with ENG_GL_METAL
 * (engine/eng_gl.h), so the engine sources stay unchanged. The functions carry their real GL names: a glmetal binary
 * links no OpenGL library at all (visionOS has none).
 *
 * What is implemented, and how (see glmetal.m):
 *   - immediate mode (glBegin/glEnd: points, lines, triangles, quads, fans, strips, polygons) and client arrays
 *     (glVertex/Color/TexCoordPointer, glDrawArrays, glDrawElements)
 *   - projection, modelview and texture matrix stacks (glOrtho, glFrustum, glTranslate/Scale/Rotate)
 *   - 2 texture units: glTexEnv MODULATE/REPLACE/DECAL/BLEND/ADD/COMBINE (REPLACE, MODULATE, ADD, ADD_SIGNED,
 *     INTERPOLATE, SUBTRACT with every source/operand), RGB/ALPHA scale, the env colour
 *   - alpha test, blend func, colour mask, depth test/func/mask, scissor, viewport, clear
 *   - textures as RGBA8 (RGB, BGRA, LUMINANCE, ALPHA uploads converted), GL_UNPACK_ROW_LENGTH/ALIGNMENT, NEAREST/LINEAR,
 *     CLAMP/REPEAT; glCopyTexSubImage2D; glReadPixels (bottom-up rows, as GL)
 *   - glPushAttrib/glPopAttrib
 * Not implemented: shaders/programs and framebuffer objects (glmetal_GetProcAddress returns NULL for them, so the
 * engine takes its no-shader / no-FBO paths; the gamma pass has its own entry, glmetal_post_lut), wide lines,
 * glPixelMap/glPixelTransfer (accepted, no effect), mipmaps.
 *
 * The "default framebuffer" is an offscreen RGBA8 MTLTexture made by glmetal_create(). Its rows are stored TOP-DOWN
 * (row 0 = the picture's top, as a presenter wants it) unless glmetal_target_bottom_up() says otherwise
 * (GLMETAL_BOTTOMUP=1); glReadPixels returns GL's bottom-up rows either way.
 */
#ifndef GLMETAL_GL_H
#define GLMETAL_GL_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;
typedef char           GLchar;
typedef intptr_t       GLintptr;
typedef intptr_t       GLsizeiptr;

#ifndef APIENTRY
#define APIENTRY
#endif

#define GL_VERSION_1_1 1
#define GL_VERSION_1_2 1
#define GL_VERSION_1_3 1

#define GL_FALSE 0
#define GL_TRUE  1
#define GL_NO_ERROR          0
#define GL_INVALID_ENUM      0x0500
#define GL_INVALID_VALUE     0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY     0x0505

/* primitives */
#define GL_POINTS         0x0000
#define GL_LINES          0x0001
#define GL_LINE_LOOP      0x0002
#define GL_LINE_STRIP     0x0003
#define GL_TRIANGLES      0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN   0x0006
#define GL_QUADS          0x0007
#define GL_QUAD_STRIP     0x0008
#define GL_POLYGON        0x0009

/* comparison functions */
#define GL_NEVER    0x0200
#define GL_LESS     0x0201
#define GL_EQUAL    0x0202
#define GL_LEQUAL   0x0203
#define GL_GREATER  0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL   0x0206
#define GL_ALWAYS   0x0207

/* blend factors */
#define GL_ZERO                0
#define GL_ONE                 1
#define GL_SRC_COLOR           0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA           0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA           0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR           0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE  0x0308

/* buffers */
#define GL_FRONT              0x0404
#define GL_BACK               0x0405
#define GL_FRONT_AND_BACK     0x0408
#define GL_DEPTH_BUFFER_BIT   0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT   0x00004000

/* capabilities */
#define GL_LINE_SMOOTH    0x0B20
#define GL_CULL_FACE      0x0B44
#define GL_LIGHTING       0x0B50
#define GL_FOG            0x0B60
#define GL_DEPTH_TEST     0x0B71
#define GL_STENCIL_TEST   0x0B90
#define GL_NORMALIZE      0x0BA1
#define GL_ALPHA_TEST     0x0BC0
#define GL_DITHER         0x0BD0
#define GL_BLEND          0x0BE2
#define GL_SCISSOR_TEST   0x0C11
#define GL_TEXTURE_2D     0x0DE1
#define GL_VERTEX_ARRAY        0x8074
#define GL_NORMAL_ARRAY        0x8075
#define GL_COLOR_ARRAY         0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

/* gets */
#define GL_MATRIX_MODE          0x0BA0
#define GL_VIEWPORT             0x0BA2
#define GL_SCISSOR_BOX          0x0C10
#define GL_UNPACK_ROW_LENGTH    0x0CF2
#define GL_UNPACK_SKIP_ROWS     0x0CF3
#define GL_UNPACK_SKIP_PIXELS   0x0CF4
#define GL_UNPACK_ALIGNMENT     0x0CF5
#define GL_PACK_ROW_LENGTH      0x0D02
#define GL_PACK_ALIGNMENT       0x0D05
#define GL_MAX_TEXTURE_SIZE     0x0D33
#define GL_TEXTURE_BINDING_2D   0x8069
#define GL_BLEND_DST            0x0BE0
#define GL_BLEND_SRC            0x0BE1
#define GL_ACTIVE_TEXTURE       0x84E0
#define GL_CLIENT_ACTIVE_TEXTURE 0x84E1
#define GL_MAX_TEXTURE_UNITS    0x84E2

/* strings */
#define GL_VENDOR     0x1F00
#define GL_RENDERER   0x1F01
#define GL_VERSION    0x1F02
#define GL_EXTENSIONS 0x1F03

/* hints */
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_DONT_CARE 0x1100
#define GL_FASTEST   0x1101
#define GL_NICEST    0x1102

/* shading */
#define GL_FLAT   0x1D00
#define GL_SMOOTH 0x1D01

/* matrices */
#define GL_MODELVIEW  0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE    0x1702

/* pixel formats and types */
#define GL_BYTE           0x1400
#define GL_UNSIGNED_BYTE  0x1401
#define GL_SHORT          0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT            0x1404
#define GL_UNSIGNED_INT   0x1405
#define GL_FLOAT          0x1406
#define GL_DOUBLE         0x140A
#define GL_ALPHA           0x1906
#define GL_RGB             0x1907
#define GL_RGBA            0x1908
#define GL_LUMINANCE       0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_RGB8            0x8051
#define GL_RGBA8           0x8058
#define GL_BGR             0x80E0
#define GL_BGRA            0x80E1
#define GL_DEPTH_COMPONENT 0x1902

/* texture parameters */
#define GL_NEAREST                0x2600
#define GL_LINEAR                 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST  0x2701
#define GL_NEAREST_MIPMAP_LINEAR  0x2702
#define GL_LINEAR_MIPMAP_LINEAR   0x2703
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_WRAP_S         0x2802
#define GL_TEXTURE_WRAP_T         0x2803
#define GL_CLAMP                  0x2900
#define GL_REPEAT                 0x2901
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_MIRRORED_REPEAT        0x8370

/* texture environment */
#define GL_TEXTURE_ENV       0x2300
#define GL_TEXTURE_ENV_MODE  0x2200
#define GL_TEXTURE_ENV_COLOR 0x2201
#define GL_MODULATE          0x2100
#define GL_DECAL             0x2101
#define GL_ADD               0x0104
#define GL_REPLACE           0x1E01
#define GL_COMBINE           0x8570
#define GL_COMBINE_RGB       0x8571
#define GL_COMBINE_ALPHA     0x8572
#define GL_RGB_SCALE         0x8573
#define GL_ADD_SIGNED        0x8574
#define GL_INTERPOLATE       0x8575
#define GL_CONSTANT          0x8576
#define GL_PRIMARY_COLOR     0x8577
#define GL_PREVIOUS          0x8578
#define GL_SUBTRACT          0x84E7
#define GL_SOURCE0_RGB       0x8580
#define GL_SOURCE1_RGB       0x8581
#define GL_SOURCE2_RGB       0x8582
#define GL_SOURCE0_ALPHA     0x8588
#define GL_SOURCE1_ALPHA     0x8589
#define GL_SOURCE2_ALPHA     0x858A
#define GL_OPERAND0_RGB      0x8590
#define GL_OPERAND1_RGB      0x8591
#define GL_OPERAND2_RGB      0x8592
#define GL_OPERAND0_ALPHA    0x8598
#define GL_OPERAND1_ALPHA    0x8599
#define GL_OPERAND2_ALPHA    0x859A
#define GL_ALPHA_SCALE       0x0D1C
#define GL_TEXTURE0          0x84C0
#define GL_TEXTURE1          0x84C1

/* pixel transfer (accepted, no effect) */
#define GL_MAP_COLOR          0x0D10
#define GL_PIXEL_MAP_R_TO_R   0x0C76
#define GL_PIXEL_MAP_G_TO_G   0x0C77
#define GL_PIXEL_MAP_B_TO_B   0x0C78
#define GL_PIXEL_MAP_A_TO_A   0x0C79

/* attribute groups */
#define GL_CURRENT_BIT         0x00000001
#define GL_LINE_BIT            0x00000004
#define GL_VIEWPORT_BIT        0x00000800
#define GL_TRANSFORM_BIT       0x00001000
#define GL_ENABLE_BIT          0x00002000
#define GL_HINT_BIT            0x00008000
#define GL_SCISSOR_BIT         0x00080000
#define GL_TEXTURE_BIT         0x00040000
#define GL_ALL_ATTRIB_BITS     0xFFFFFFFF
#define GL_CLIENT_PIXEL_STORE_BIT  0x00000001
#define GL_CLIENT_VERTEX_ARRAY_BIT 0x00000002

/* ---- the GL entry points ---- */
void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex2i(GLint x, GLint y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void glColor4fv(const GLfloat *v);

void glEnable(GLenum cap);
void glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);
void glEnableClientState(GLenum array);
void glDisableClientState(GLenum array);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glPushMatrix(void);
void glPopMatrix(void);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClearDepth(GLclampd d);
void glClear(GLbitfield mask);
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glDepthMask(GLboolean flag);
void glDepthFunc(GLenum func);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glAlphaFunc(GLenum func, GLclampf ref);
void glLineWidth(GLfloat width);
void glShadeModel(GLenum mode);
void glHint(GLenum target, GLenum mode);

void glActiveTexture(GLenum texture);
void glClientActiveTexture(GLenum texture);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                     GLenum format, GLenum type, const GLvoid *pixels);
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
void glPixelStorei(GLenum pname, GLint param);
void glPixelTransferi(GLenum pname, GLint param);
void glPixelMapfv(GLenum map, GLsizei mapsize, const GLfloat *values);

void glReadBuffer(GLenum mode);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);
void glGetIntegerv(GLenum pname, GLint *params);
const GLubyte *glGetString(GLenum name);
GLenum glGetError(void);
void glFinish(void);
void glFlush(void);
void glPushAttrib(GLbitfield mask);
void glPopAttrib(void);

/* ---- glmetal's own API (the host side) ---- */
/* Make (or resize) the target: an offscreen w x h RGBA8 texture that is the default framebuffer. Creates the Metal
 * device and queue on first use. false if there is no Metal device. */
bool glmetal_create(int w, int h);
/* Use this id<MTLDevice> (bridged; retained by glmetal) instead of the system default. Before the first glmetal_create:
 * an embedding app (the visionOS shell) passes its own device so the frames are textures it can sample. */
void glmetal_set_device(void *mtl_device);
int  glmetal_width(void);
int  glmetal_height(void);
/* The target as an id<MTLTexture> (bridged, not retained) -- changes when glmetal_create resizes it, and may alternate
 * between two textures when the device lacks framebuffer fetch (the gamma pass ping-pongs). Read it after
 * glmetal_end_frame's hook or a glFinish. */
void *glmetal_target_texture(void);
int   glmetal_target_bottom_up(void);       /* 1: row 0 of the texture is GL's bottom row (a presenter would flip V); 0 by default */
void *glmetal_device(void);                 /* id<MTLDevice>, bridged */
void *glmetal_command_queue(void);          /* id<MTLCommandQueue>, bridged */
/* End the frame: everything drawn so far is encoded; `encode` (optional) can add its own work to the frame's
 * id<MTLCommandBuffer> (e.g. a blit of the target into a presenter's texture) before it is committed. Keeps at most
 * two frames in flight. */
void glmetal_end_frame(void (*encode)(void *cmd_buffer, void *target_texture, void *user), void *user);
/* GL entry points by name (what SDL_GL_GetProcAddress is routed to under ENG_GL_METAL); NULL for what glmetal lacks */
void *glmetal_GetProcAddress(const char *name);
/* The final per-channel colour table over the frame's (0,0,vw,vh) GL rectangle, in place: the frame is read where it is
 * (framebuffer fetch on Apple GPUs; elsewhere a ping-pong between two targets), with no copy. Alpha becomes 1. */
void glmetal_post_lut(const uint8_t lut[3][256], int vw, int vh);

#ifdef __cplusplus
}
#endif
#endif
