/*
 * glgles.c -- the engine's OpenGL subset (glgles/gl.h) on OpenGL ES 3.0 (GLSL ES 3.00), for the Android / Meta Quest build
 * (ENG_GL_GLES). A port of engine/glmetal/glmetal.m: the SAME backend-neutral CPU half -- CPU matrix stacks (mv/pj/tmx),
 * an immediate-mode vertex chunk buffer, the 2-unit GL_COMBINE texenv state machine, a GL-name texture table, and
 * glPushAttrib/glPopAttrib -- with the Metal objects replaced by GLES ones:
 *   - ONE uber-shader (vs_main/fs_main below, GLSL ES 3.00) reproducing glmetal's uber-shader exactly: mvp * pos in the
 *     vertex stage, the vertex colour clamped, the texture matrix on the coordinates; then two texture units with the full
 *     GL 1.3 texenv/combine set (REPLACE/MODULATE/DECAL/BLEND/ADD + COMBINE with every source/operand) and the alpha test;
 *   - a VBO ring for the immediate-mode vertex stream (orphaned per flush, like m3_render_gles.c);
 *   - FBO colour + depth render targets, GLGLES_MAX_TARGETS of them (per-eye stereo + panorama slots, as glmetal's
 *     GLMETAL_MAX_TARGETS), each a GL_RGBA8 texture + a DEPTH_COMPONENT24 renderbuffer; the host publishes the colour
 *     textures (glgles_host.h);
 *   - the mixer's gamma pass (glgles_post_lut) through a 256-entry per-channel table, GL_EXT_shader_framebuffer_fetch in
 *     place when present (as model3's GLES backend), else a copy-and-redraw.
 *
 * ORIENTATION: the targets are native GL framebuffers, so row 0 (t = 0) is the BOTTOM of the picture. The engine's
 * glOrtho(x0, x1, NH, 0, ...) already orients the scene; no y flip is applied (glgles_target_bottom_up() == 1). This is the
 * orientation m3_core_latest_frame_gl publishes and an OpenXR GLES swapchain image wants.
 *
 * The real GLES entry points are loaded by name (dlsym on libGLESv3/2, eglGetProcAddress as a fallback) into R_* pointers,
 * so the engine-facing names (glViewport, glBindTexture, glClear...) this file DEFINES never clash with the driver's.
 */
#include "glgles/gl.h"

#include <EGL/egl.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ real-GL constants */
#define RGL_FRAMEBUFFER            0x8D40
#define RGL_RENDERBUFFER           0x8D41
#define RGL_COLOR_ATTACHMENT0      0x8CE0
#define RGL_DEPTH_ATTACHMENT       0x8D00
#define RGL_FRAMEBUFFER_COMPLETE   0x8CD5
#define RGL_DEPTH_COMPONENT24      0x81A6
#define RGL_ARRAY_BUFFER           0x8892
#define RGL_STREAM_DRAW            0x88E0
#define RGL_FRAGMENT_SHADER        0x8B30
#define RGL_VERTEX_SHADER          0x8B31
#define RGL_COMPILE_STATUS         0x8B81
#define RGL_LINK_STATUS            0x8B82
#define RGL_FUNC_ADD               0x8006
#define RGL_NUM_EXTENSIONS         0x821D
#define RGL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define RGL_MAJOR_VERSION          0x821B
#define RGL_MINOR_VERSION          0x821C

typedef void *GLsync_;
typedef unsigned long long GLuint64_;

/* ------------------------------------------------------------------------------------------------ real-GL pointers */
#define RGL_LIST(X) \
    X(void,  GenTextures,       (GLsizei, GLuint *)) \
    X(void,  DeleteTextures,    (GLsizei, const GLuint *)) \
    X(void,  BindTexture,       (GLenum, GLuint)) \
    X(void,  TexImage2D,        (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void,  TexSubImage2D,     (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void,  TexParameteri,     (GLenum, GLenum, GLint)) \
    X(void,  ActiveTexture,     (GLenum)) \
    X(void,  PixelStorei,       (GLenum, GLint)) \
    X(void,  CopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
    X(void,  GenFramebuffers,   (GLsizei, GLuint *)) \
    X(void,  BindFramebuffer,   (GLenum, GLuint)) \
    X(void,  FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void,  GenRenderbuffers,  (GLsizei, GLuint *)) \
    X(void,  BindRenderbuffer,  (GLenum, GLuint)) \
    X(void,  RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void,  FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void,  DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void,  DeleteRenderbuffers, (GLsizei, const GLuint *)) \
    X(void,  Enable,            (GLenum)) \
    X(void,  Disable,           (GLenum)) \
    X(void,  Viewport,          (GLint, GLint, GLsizei, GLsizei)) \
    X(void,  Scissor,           (GLint, GLint, GLsizei, GLsizei)) \
    X(void,  BlendFunc,         (GLenum, GLenum)) \
    X(void,  BlendEquation,     (GLenum)) \
    X(void,  DepthFunc,         (GLenum)) \
    X(void,  DepthMask,         (GLboolean)) \
    X(void,  ColorMask,         (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void,  Clear,             (GLbitfield)) \
    X(void,  ClearColor,        (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,  ClearDepthf,       (GLfloat)) \
    X(void,  DrawArrays,        (GLenum, GLint, GLsizei)) \
    X(void,  GenBuffers,        (GLsizei, GLuint *)) \
    X(void,  BindBuffer,        (GLenum, GLuint)) \
    X(void,  BufferData,        (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void,  BufferSubData,     (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void,  DeleteBuffers,     (GLsizei, const GLuint *)) \
    X(void,  GenVertexArrays,   (GLsizei, GLuint *)) \
    X(void,  BindVertexArray,   (GLuint)) \
    X(void,  DeleteVertexArrays, (GLsizei, const GLuint *)) \
    X(void,  EnableVertexAttribArray, (GLuint)) \
    X(void,  VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(GLuint, CreateShader,     (GLenum)) \
    X(void,  ShaderSource,      (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void,  CompileShader,     (GLuint)) \
    X(void,  GetShaderiv,       (GLuint, GLenum, GLint *)) \
    X(void,  GetShaderInfoLog,  (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, CreateProgram,    (void)) \
    X(void,  AttachShader,      (GLuint, GLuint)) \
    X(void,  BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void,  LinkProgram,       (GLuint)) \
    X(void,  GetProgramiv,      (GLuint, GLenum, GLint *)) \
    X(void,  GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,  UseProgram,        (GLuint)) \
    X(void,  DeleteShader,      (GLuint)) \
    X(void,  DeleteProgram,     (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void,  Uniform1i,         (GLint, GLint)) \
    X(void,  Uniform1f,         (GLint, GLfloat)) \
    X(void,  Uniform2f,         (GLint, GLfloat, GLfloat)) \
    X(void,  Uniform4iv,        (GLint, GLsizei, const GLint *)) \
    X(void,  Uniform4fv,        (GLint, GLsizei, const GLfloat *)) \
    X(void,  UniformMatrix4fv,  (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(GLsync_, FenceSync,       (GLenum, GLbitfield)) \
    X(void,  DeleteSync,        (GLsync_)) \
    X(void,  Flush,             (void)) \
    X(void,  Finish,            (void)) \
    X(void,  ReadPixels,        (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(GLenum, GetError,         (void)) \
    X(const GLubyte *, GetString, (GLenum)) \
    X(const GLubyte *, GetStringi, (GLenum, GLuint)) \
    X(void,  GetIntegerv,       (GLenum, GLint *))

#define X(ret, name, args) typedef ret (APIENTRY *pfn_##name) args; static pfn_##name R_##name;
RGL_LIST(X)
#undef X

static void *s_libgl;
static void *gl_load(const char *n)
{
    void *p = s_libgl ? dlsym(s_libgl, n) : NULL;
    if (!p) { void (*q)(void) = eglGetProcAddress(n); p = (void *)q; }
    return p;
}
static bool load_gl(void)
{
    if (!s_libgl) s_libgl = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);
    if (!s_libgl) s_libgl = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    bool ok = true;
#define X(ret, name, args) R_##name = (pfn_##name)gl_load("gl" #name); if (!R_##name) { fprintf(stderr, "[GLGLES] missing gl%s\n", #name); ok = false; }
    RGL_LIST(X)
#undef X
    return ok;
}

/* ------------------------------------------------------------------------------------------------ CPU-side structs */
/* bias: STEREO off-axis horizontal shift for the +1 (right) eye, in pre-mvp screen pixels, 0 for the 2D layers and in mono.
 * The vertex shader adds u_eye * bias to pos.x before u_mvp (u_eye = -1 left, +1 right, 0 mono). See glgles_stereo_begin. */
typedef struct { float pos[4]; float col[4]; float tc[4]; float bias; } Vtx;

typedef struct {
    GLenum mode, comb_rgb, comb_a;
    GLenum src_rgb[3], op_rgb[3], src_a[3], op_a[3];
    float rgb_scale, a_scale, color[4];
} texenv;

typedef struct {
    bool tex2d[2], alpha_test, blend, scissor_test, depth_test;
    GLenum alpha_func; float alpha_ref;
    GLenum bsrc, bdst;
    bool cmask[4];
    float clear[4];
    GLenum depth_func; bool depth_mask; float clear_depth;
    int vp[4], sc[4];
    texenv env[2]; GLuint bound[2]; int active;
    float color[4], tc[4];
    GLenum matrix_mode;
    float line_width;
} srvstate;

typedef struct {
    bool used;
    GLuint tex;                 /* the real GL texture name (0 = no storage yet) */
    int w, h;
    bool rgb_only;              /* internal format without alpha: alpha reads 1 */
    GLenum minf, magf, ws, wt;
} gtex;

typedef struct { bool on; int size; GLenum type; int stride; const void *ptr; } carray;

enum { PRIM_TRI = 0, PRIM_LINE = 1, PRIM_POINT = 2 };

/* the per-draw GPU state (POD; memcmp decides whether a draw extends the pending one) */
typedef struct {
    int prim;
    int vp[4], sc[4]; int scissor;
    int depth_test, depth_mask; GLenum depth_func;
    int blend; GLenum bsrc, bdst;
    int cmask;                       /* 4 bits */
    float mvp[16], tm[16];
    int cfg[2][4], srcrgb[2][4], oprgb[2][4], srca[2][4], opa[2][4];
    float env[2][4], scale[2][4];
    GLuint tex[2];
    int alpha_func; float alpha_ref;
} drawstate;

/* ------------------------------------------------------------------------------------------------ targets */
typedef struct {
    GLuint fbo, color[2], depth;     /* color[1] only for the no-fetch gamma ping-pong */
    int cur, w, h, req_w, req_h;
    int fb_attached;                 /* which color[] is attached to fbo now (-1 = none): skip the per-flush re-attach */
    bool made;
} target;

/* ------------------------------------------------------------------------------------------------ globals */
static bool created;
static int base_w, base_h;
static target tgt[GLGLES_MAX_TARGETS];
static int cur_set;                   /* the active target index */

/* STEREO: when st_mode, every flush / clear / gamma pass runs TWICE from the one emitted vertex stream -- target st_L with
 * u_eye = -1 and target st_R with u_eye = +1 -- so the per-vertex Vtx.bias becomes the off-axis parallax on the GPU. The CPU
 * geometry (quad walk, bake, clip, batch) is emitted ONCE. See glgles_stereo_begin / glgles_stereo_end. */
static int st_mode, st_L, st_R;
/* STEREO: the per-vertex bias source for the client-array draw path (quad_gl's bb_b). NULL = no bias (0), as in mono and for
 * the immediate-mode 2D layers. Set by glgles_set_bias_array. */
static const float *bias_arr;

static GLuint prog;                   /* the uber-program */
static struct { GLint mvp, tm, yflip, eye, cfg, srcrgb, oprgb, srca, opa, env, scale, afunc, aref, tex0, tex1; } U;
static GLuint prog_post;              /* framebuffer-fetch gamma */
static GLuint prog_post_copy;         /* copy-and-redraw gamma (no fetch) */
static struct { GLint lut; } UP;
static struct { GLint lut, src; } UPC;
static bool fetch_ok;
static GLuint vao;
/* ONE streaming vertex buffer. The attrib layout is bound to it ONCE at create (it is the only GL_ARRAY_BUFFER glgles ever
 * binds), so flush_pending no longer re-issues the 4 VertexAttribPointer calls every flush -- it only orphans + uploads + draws.
 * Orphaning (BufferData NULL) per flush keeps the no-stall streaming of the old 4-deep ring while dropping the per-flush setup. */
static GLuint vbo; static size_t vbo_cap;
static GLuint lut_tex, scratch_tex; static int scratch_w, scratch_h;

static srvstate S;
static float mv[32][16], pj[32][16], tmx[2][8][16];
static int mv_n, pj_n, tm_n[2];
static carray ca_v, ca_c, ca_t;
static int client_active;
static int unpack_row_len, unpack_align = 4, pack_align = 4, pack_row_len;
static GLenum gl_err;
static srvstate attr_stack[16]; static GLbitfield attr_mask[16]; static int attr_n;

static Vtx *imm; static int imm_n, imm_cap; static GLenum imm_mode; static bool in_begin;
static Vtx *tmpv; static int tmpv_cap;
static Vtx *gat; static int gat_cap;

/* the pending (batched) draw: a run of same-state primitives accumulated on the CPU, one glDrawArrays at flush */
static drawstate pend; static bool pend_on;
static Vtx *pend_v; static int pend_n, pend_cap;
static drawstate applied; static bool have_applied; static GLuint applied_prog;

static char renderer_str[192];

/* ------------------------------------------------------------------------------------------------ matrices */
static void mat_identity(float *m) { memset(m, 0, 16 * sizeof(float)); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static void mat_mul(float *r, const float *a, const float *b)      /* r = a * b, column-major */
{
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            t[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
    memcpy(r, t, sizeof t);
}
static float *cur_matrix(void)
{
    switch (S.matrix_mode) {
    case GL_PROJECTION: return pj[pj_n];
    case GL_TEXTURE:    return tmx[S.active][tm_n[S.active]];
    default:            return mv[mv_n];
    }
}

static void env_default(texenv *e)
{
    e->mode = GL_MODULATE; e->comb_rgb = GL_MODULATE; e->comb_a = GL_MODULATE;
    e->src_rgb[0] = GL_TEXTURE; e->src_rgb[1] = GL_PREVIOUS; e->src_rgb[2] = GL_CONSTANT;
    e->src_a[0] = GL_TEXTURE;   e->src_a[1] = GL_PREVIOUS;   e->src_a[2] = GL_CONSTANT;
    e->op_rgb[0] = GL_SRC_COLOR; e->op_rgb[1] = GL_SRC_COLOR; e->op_rgb[2] = GL_SRC_ALPHA;
    e->op_a[0] = e->op_a[1] = e->op_a[2] = GL_SRC_ALPHA;
    e->rgb_scale = e->a_scale = 1.0f;
    memset(e->color, 0, sizeof e->color);
}
static void state_defaults(void)
{
    memset(&S, 0, sizeof S);
    S.alpha_func = GL_ALWAYS; S.alpha_ref = 0.0f;
    S.bsrc = GL_ONE; S.bdst = GL_ZERO;
    S.cmask[0] = S.cmask[1] = S.cmask[2] = S.cmask[3] = true;
    S.depth_func = GL_LESS; S.depth_mask = true; S.clear_depth = 1.0f;
    env_default(&S.env[0]); env_default(&S.env[1]);
    S.color[0] = S.color[1] = S.color[2] = S.color[3] = 1.0f;
    S.tc[3] = 1.0f;
    S.matrix_mode = GL_MODELVIEW;
    S.line_width = 1.0f;
    mv_n = pj_n = 0; tm_n[0] = tm_n[1] = 0;
    mat_identity(mv[0]); mat_identity(pj[0]); mat_identity(tmx[0][0]); mat_identity(tmx[1][0]);
}

/* ------------------------------------------------------------------------------------------------ textures */
static gtex *texs; static int ntexs;
static gtex *tex_get(GLuint name)
{
    if (name == 0) return NULL;
    if ((int)name >= ntexs) {
        int n = ntexs ? ntexs : 256;
        while (n <= (int)name) n *= 2;
        texs = realloc(texs, sizeof(gtex) * (size_t)n);
        memset(texs + ntexs, 0, sizeof(gtex) * (size_t)(n - ntexs));
        ntexs = n;
    }
    gtex *t = &texs[name];
    if (!t->used) {
        memset(t, 0, sizeof *t);
        t->used = true;
        t->minf = GL_NEAREST_MIPMAP_LINEAR; t->magf = GL_LINEAR; t->ws = GL_REPEAT; t->wt = GL_REPEAT;
    }
    return t;
}
static bool tex_complete(const gtex *t)
{
    if (!t || !t->tex) return false;
    return t->minf == GL_NEAREST || t->minf == GL_LINEAR;   /* a mipmap filter on one level: incomplete -> unit off */
}
static GLint wrap_gl(GLenum w) { return w == GL_REPEAT ? GL_REPEAT : w == GL_MIRRORED_REPEAT ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE; }
static GLint minf_gl(GLenum f) { return f == GL_LINEAR ? GL_LINEAR : GL_NEAREST; }
static GLint magf_gl(GLenum f) { return f == GL_LINEAR ? GL_LINEAR : GL_NEAREST; }

static int fmt_bpp(GLenum format)
{
    switch (format) {
    case GL_RGBA: case GL_BGRA: return 4;
    case GL_RGB: case GL_BGR: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;
    }
}
/* one row of `w` pixels in `format` to RGBA8 (as glmetal's row_to_rgba: GLES has no GL_BGRA/LUMINANCE storage, so every
 * upload is converted to RGBA8 on the CPU and uploaded as GL_RGBA) */
static void row_to_rgba(uint8_t *dst, const uint8_t *src, int w, GLenum format, bool force_alpha)
{
    switch (format) {
    case GL_RGBA: memcpy(dst, src, (size_t)w * 4); break;
    case GL_BGRA: for (int i = 0; i < w; i++) { dst[i*4]=src[i*4+2]; dst[i*4+1]=src[i*4+1]; dst[i*4+2]=src[i*4]; dst[i*4+3]=src[i*4+3]; } break;
    case GL_RGB:  for (int i = 0; i < w; i++) { dst[i*4]=src[i*3]; dst[i*4+1]=src[i*3+1]; dst[i*4+2]=src[i*3+2]; dst[i*4+3]=255; } break;
    case GL_BGR:  for (int i = 0; i < w; i++) { dst[i*4]=src[i*3+2]; dst[i*4+1]=src[i*3+1]; dst[i*4+2]=src[i*3]; dst[i*4+3]=255; } break;
    case GL_LUMINANCE: for (int i = 0; i < w; i++) { dst[i*4]=dst[i*4+1]=dst[i*4+2]=src[i]; dst[i*4+3]=255; } break;
    case GL_LUMINANCE_ALPHA: for (int i = 0; i < w; i++) { dst[i*4]=dst[i*4+1]=dst[i*4+2]=src[i*2]; dst[i*4+3]=src[i*2+1]; } break;
    case GL_ALPHA: for (int i = 0; i < w; i++) { dst[i*4]=dst[i*4+1]=dst[i*4+2]=0; dst[i*4+3]=src[i]; } break;
    default: memset(dst, 0, (size_t)w * 4); break;
    }
    if (force_alpha) for (int i = 0; i < w; i++) dst[i*4+3] = 255;
}
static void flush_pending(void);
static void tex_upload(gtex *t, int x, int y, int w, int h, GLenum format, const void *pixels, bool full)
{
    if (!t || !t->tex || !pixels || w <= 0 || h <= 0) return;
    const int bpp = fmt_bpp(format);
    const size_t rowpix = unpack_row_len > 0 ? (size_t)unpack_row_len : (size_t)w;
    const size_t a = (size_t)(unpack_align > 0 ? unpack_align : 1);
    const size_t stride = (rowpix * (size_t)bpp + a - 1) / a * a;
    uint8_t *tmp = malloc((size_t)w * (size_t)h * 4);
    if (!tmp) { gl_err = GL_OUT_OF_MEMORY; return; }
    for (int r = 0; r < h; r++)
        row_to_rgba(tmp + (size_t)r * w * 4, (const uint8_t *)pixels + (size_t)r * stride, w, format, t->rgb_only);
    R_ActiveTexture(GL_TEXTURE0);
    R_BindTexture(GL_TEXTURE_2D, t->tex);
    R_PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (full) R_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    else      R_TexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    R_PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    free(tmp);
    have_applied = false;                 /* we disturbed the real texture binding on unit 0 */
}

/* ------------------------------------------------------------------------------------------------ shaders */
static const char *VS_SRC =
"#version 300 es\n"
"layout(location=0) in vec4 a_pos;\n"
"layout(location=1) in vec4 a_col;\n"
"layout(location=2) in vec4 a_tc;\n"
"layout(location=3) in float a_bias;\n"
"uniform mat4 u_mvp;\n"
"uniform mat4 u_tm;\n"
"uniform float u_yflip;\n"
"uniform float u_eye;\n"               /* STEREO: -1 left, +1 right, 0 mono (bias has no effect) */
"out vec4 v_col;\n"
"out vec4 v_tc;\n"
"void main(){\n"
"    vec4 pp = a_pos;\n"
"    pp.x += u_eye * a_bias;\n"        /* off-axis eye shift, in the same pre-mvp screen space as pos.x */
"    vec4 p = u_mvp * pp;\n"
"    p.y *= u_yflip;\n"                 /* +1 here: native bottom-up targets */
"    gl_Position = p;\n"
"    gl_PointSize = 1.0;\n"
"    v_col = clamp(a_col, 0.0, 1.0);\n" /* fixed function clamps the vertex colour */
"    v_tc = u_tm * a_tc;\n"
"}\n";

static const char *FS_SRC =
"#version 300 es\n"
"precision highp float;\n"
"precision highp int;\n"
"precision highp sampler2D;\n"
"in vec4 v_col;\n"
"in vec4 v_tc;\n"
"uniform sampler2D u_tex0;\n"
"uniform sampler2D u_tex1;\n"
"uniform ivec4 u_cfg[2];\n"             /* x enabled, y mode, z comb_rgb, w comb_a */
"uniform ivec4 u_srcrgb[2];\n"
"uniform ivec4 u_oprgb[2];\n"
"uniform ivec4 u_srca[2];\n"
"uniform ivec4 u_opa[2];\n"
"uniform vec4 u_env[2];\n"
"uniform vec4 u_scale[2];\n"
"uniform int u_afunc;\n"                /* 0 = off */
"uniform float u_aref;\n"
"out vec4 o_frag;\n"
"vec4 pick(int src,int unit,vec4 t0,vec4 t1,vec4 prim,vec4 prev,vec4 env){\n"
"    switch(src){\n"
"    case 0x1702: return unit==0?t0:t1;\n"
"    case 0x84C0: return t0;\n"
"    case 0x84C1: return t1;\n"
"    case 0x8576: return env;\n"
"    case 0x8577: return prim;\n"
"    default: return prev;\n"
"    }\n"
"}\n"
"vec3 oprgb(vec4 s,int op){ switch(op){ case 0x0300: return s.rgb; case 0x0301: return 1.0-s.rgb; case 0x0302: return vec3(s.a); default: return vec3(1.0-s.a);} }\n"
"float opa(vec4 s,int op){ return op==0x0303 ? 1.0-s.a : s.a; }\n"
"vec3 comb3(int f,vec3 a0,vec3 a1,vec3 a2){ switch(f){ case 0x1E01: return a0; case 0x2100: return a0*a1; case 0x0104: return a0+a1; case 0x8574: return a0+a1-0.5; case 0x8575: return a0*a2+a1*(1.0-a2); case 0x84E7: return a0-a1; default: return a0*a1;} }\n"
"float comb1(int f,float a0,float a1,float a2){ switch(f){ case 0x1E01: return a0; case 0x2100: return a0*a1; case 0x0104: return a0+a1; case 0x8574: return a0+a1-0.5; case 0x8575: return a0*a2+a1*(1.0-a2); case 0x84E7: return a0-a1; default: return a0*a1;} }\n"
"void main(){\n"
"    vec4 prim = v_col;\n"
"    vec4 prev = prim;\n"
"    vec4 t0 = vec4(1.0), t1 = vec4(1.0);\n"
"    if (u_cfg[0].x != 0) t0 = texture(u_tex0, v_tc.xy / v_tc.w);\n"
"    if (u_cfg[1].x != 0) t1 = texture(u_tex1, vec2(0.0));\n"
"    for (int i = 0; i < 2; i++) {\n"
"        ivec4 cfg = (i==0)?u_cfg[0]:u_cfg[1];\n"
"        if (cfg.x == 0) continue;\n"
"        vec4 t = (i==0)?t0:t1;\n"
"        vec4 env = (i==0)?u_env[0]:u_env[1];\n"
"        vec4 r;\n"
"        switch (cfg.y) {\n"
"        case 0x1E01: r = t; break;\n"
"        case 0x2100: r = prev * t; break;\n"
"        case 0x2101: r = vec4(mix(prev.rgb, t.rgb, t.a), prev.a); break;\n"
"        case 0x0BE2: r = vec4(mix(prev.rgb, env.rgb, t.rgb), prev.a * t.a); break;\n"
"        case 0x0104: r = vec4(prev.rgb + t.rgb, prev.a * t.a); break;\n"
"        default: {\n"
"            ivec4 sr=(i==0)?u_srcrgb[0]:u_srcrgb[1];\n"
"            ivec4 orr=(i==0)?u_oprgb[0]:u_oprgb[1];\n"
"            ivec4 sa=(i==0)?u_srca[0]:u_srca[1];\n"
"            ivec4 oa=(i==0)?u_opa[0]:u_opa[1];\n"
"            vec4 sc=(i==0)?u_scale[0]:u_scale[1];\n"
"            vec3 a0=oprgb(pick(sr.x,i,t0,t1,prim,prev,env),orr.x);\n"
"            vec3 a1=oprgb(pick(sr.y,i,t0,t1,prim,prev,env),orr.y);\n"
"            vec3 a2=oprgb(pick(sr.z,i,t0,t1,prim,prev,env),orr.z);\n"
"            float b0=opa(pick(sa.x,i,t0,t1,prim,prev,env),oa.x);\n"
"            float b1=opa(pick(sa.y,i,t0,t1,prim,prev,env),oa.y);\n"
"            float b2=opa(pick(sa.z,i,t0,t1,prim,prev,env),oa.z);\n"
"            r = vec4(comb3(cfg.z,a0,a1,a2) * sc.x, comb1(cfg.w,b0,b1,b2) * sc.y);\n"
"        }\n"
"        }\n"
"        prev = clamp(r, 0.0, 1.0);\n"
"    }\n"
"    float a = prev.a, ref = u_aref;\n"
"    switch (u_afunc) {\n"
"    case 0x0200: discard;\n"
"    case 0x0201: if (!(a <  ref)) discard; break;\n"
"    case 0x0202: if (!(a == ref)) discard; break;\n"
"    case 0x0203: if (!(a <= ref)) discard; break;\n"
"    case 0x0204: if (!(a >  ref)) discard; break;\n"
"    case 0x0205: if (!(a != ref)) discard; break;\n"
"    case 0x0206: if (!(a >= ref)) discard; break;\n"
"    default: break;\n"
"    }\n"
"    o_frag = prev;\n"
"}\n";

/* gamma: a big triangle from gl_VertexID, the 256-entry per-channel table (post_gl.c's texel centres) */
static const char *POST_VS =
"#version 300 es\n"
"void main(){ vec2 p = vec2((gl_VertexID==1)?3.0:-1.0, (gl_VertexID==2)?3.0:-1.0); gl_Position = vec4(p,0.0,1.0); }\n";
static const char *POST_FS_FETCH =
"#version 300 es\n"
"#extension GL_EXT_shader_framebuffer_fetch : require\n"
"precision highp float;\n"
"uniform sampler2D u_lut;\n"
"layout(location=0) inout vec4 o_c;\n"
"void main(){\n"
"    vec3 c = o_c.rgb;\n"
"    vec3 k = c * (255.0/256.0) + 0.5/256.0;\n"
"    o_c = vec4(texture(u_lut, vec2(k.r,0.5)).r, texture(u_lut, vec2(k.g,0.5)).g, texture(u_lut, vec2(k.b,0.5)).b, 1.0);\n"
"}\n";
static const char *POST_FS_COPY =
"#version 300 es\n"
"precision highp float;\n"
"uniform sampler2D u_lut;\n"
"uniform sampler2D u_src;\n"
"out vec4 o_c;\n"
"void main(){\n"
"    vec3 c = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).rgb;\n"
"    vec3 k = c * (255.0/256.0) + 0.5/256.0;\n"
"    o_c = vec4(texture(u_lut, vec2(k.r,0.5)).r, texture(u_lut, vec2(k.g,0.5)).g, texture(u_lut, vec2(k.b,0.5)).b, 1.0);\n"
"}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = R_CreateShader(type);
    R_ShaderSource(s, 1, &src, NULL);
    R_CompileShader(s);
    GLint ok = 0; R_GetShaderiv(s, RGL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024] = ""; R_GetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[GLGLES] shader: %s\n", log); R_DeleteShader(s); return 0; }
    return s;
}
static GLuint link_prog(const char *vs, const char *fs, bool bind_attrs)
{
    GLuint v = compile(RGL_VERTEX_SHADER, vs);
    GLuint f = v ? compile(RGL_FRAGMENT_SHADER, fs) : 0;
    if (!v || !f) { if (v) R_DeleteShader(v); if (f) R_DeleteShader(f); return 0; }
    GLuint p = R_CreateProgram();
    R_AttachShader(p, v); R_AttachShader(p, f);
    if (bind_attrs) { R_BindAttribLocation(p, 0, "a_pos"); R_BindAttribLocation(p, 1, "a_col"); R_BindAttribLocation(p, 2, "a_tc"); R_BindAttribLocation(p, 3, "a_bias"); }
    R_LinkProgram(p);
    R_DeleteShader(v); R_DeleteShader(f);
    GLint ok = 0; R_GetProgramiv(p, RGL_LINK_STATUS, &ok);
    if (!ok) { char log[1024] = ""; R_GetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "[GLGLES] program: %s\n", log); R_DeleteProgram(p); return 0; }
    return p;
}
static bool has_ext(const char *name)
{
    GLint n = 0; R_GetIntegerv(RGL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) { const char *e = (const char *)R_GetStringi(GL_EXTENSIONS, (GLuint)i); if (e && !strcmp(e, name)) return true; }
    return false;
}

/* ------------------------------------------------------------------------------------------------ targets */
static bool make_target(target *t, int w, int h)
{
    R_GenTextures(1, &t->color[0]);
    R_BindTexture(GL_TEXTURE_2D, t->color[0]);
    R_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    t->color[1] = 0;
    if (!fetch_ok) {                      /* the gamma ping-pong needs a second colour texture */
        R_GenTextures(1, &t->color[1]);
        R_BindTexture(GL_TEXTURE_2D, t->color[1]);
        R_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    R_GenRenderbuffers(1, &t->depth);
    R_BindRenderbuffer(RGL_RENDERBUFFER, t->depth);
    R_RenderbufferStorage(RGL_RENDERBUFFER, RGL_DEPTH_COMPONENT24, w, h);
    R_BindRenderbuffer(RGL_RENDERBUFFER, 0);
    R_GenFramebuffers(1, &t->fbo);
    R_BindFramebuffer(RGL_FRAMEBUFFER, t->fbo);
    R_FramebufferTexture2D(RGL_FRAMEBUFFER, RGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color[0], 0);
    R_FramebufferRenderbuffer(RGL_FRAMEBUFFER, RGL_DEPTH_ATTACHMENT, RGL_RENDERBUFFER, t->depth);
    const GLenum st = R_CheckFramebufferStatus(RGL_FRAMEBUFFER);
    if (st != RGL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[GLGLES] target %dx%d incomplete (0x%x)\n", w, h, st); return false; }
    t->cur = 0; t->fb_attached = 0; t->w = w; t->h = h; t->made = true;   /* color[0] attached just above */
    return true;
}
static void bind_target_idx(int i)
{
    target *t = &tgt[i];
    R_BindFramebuffer(RGL_FRAMEBUFFER, t->fbo);
    if (t->fb_attached != t->cur) {    /* the attachment is per-FBO and only changes on the gamma ping-pong: skip the re-attach otherwise (a per-flush call, 2x in stereo) */
        R_FramebufferTexture2D(RGL_FRAMEBUFFER, RGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color[t->cur], 0);
        t->fb_attached = t->cur;
    }
}
static void bind_target(void) { bind_target_idx(cur_set); }

/* ------------------------------------------------------------------------------------------------ draw state */
static void clamp_scissor(int *s, int W, int H)
{
    int x0 = s[0], y0 = s[1], x1 = s[0] + s[2], y1 = s[1] + s[3];
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    s[0] = x0; s[1] = y0; s[2] = x1 > x0 ? x1 - x0 : 0; s[3] = y1 > y0 ? y1 - y0 : 0;
}
static void build_unit(drawstate *st, int i)
{
    st->cfg[i][0] = 0;
    const gtex *t = tex_get(S.bound[i]);
    if (!S.tex2d[i] || !tex_complete(t)) { st->tex[i] = 0; return; }
    const texenv *e = &S.env[i];
    st->tex[i] = t->tex;
    st->cfg[i][0] = 1; st->cfg[i][1] = (int)e->mode; st->cfg[i][2] = st->cfg[i][3] = 0;
    if (e->mode == GL_COMBINE) {
        st->cfg[i][2] = (int)e->comb_rgb; st->cfg[i][3] = (int)e->comb_a;
        for (int k = 0; k < 3; k++) { st->srcrgb[i][k] = (int)e->src_rgb[k]; st->oprgb[i][k] = (int)e->op_rgb[k]; st->srca[i][k] = (int)e->src_a[k]; st->opa[i][k] = (int)e->op_a[k]; }
        st->scale[i][0] = e->rgb_scale; st->scale[i][1] = e->a_scale;
    }
    if (e->mode == GL_COMBINE || e->mode == GL_BLEND)
        for (int k = 0; k < 4; k++) st->env[i][k] = e->color[k];
}
static void build_state(drawstate *st, int prim)
{
    memset(st, 0, sizeof *st);
    st->prim = prim;
    const int W = tgt[cur_set].w, H = tgt[cur_set].h;
    st->vp[0] = S.vp[0]; st->vp[1] = S.vp[1]; st->vp[2] = S.vp[2]; st->vp[3] = S.vp[3];
    if (S.scissor_test) { st->scissor = 1; st->sc[0]=S.sc[0]; st->sc[1]=S.sc[1]; st->sc[2]=S.sc[2]; st->sc[3]=S.sc[3]; clamp_scissor(st->sc, W, H); }
    else { st->scissor = 0; st->sc[0]=0; st->sc[1]=0; st->sc[2]=W; st->sc[3]=H; }
    st->depth_test = S.depth_test; st->depth_mask = S.depth_mask; st->depth_func = S.depth_func;
    st->blend = S.blend; st->bsrc = S.bsrc; st->bdst = S.bdst;
    st->cmask = (S.cmask[0]?1:0)|(S.cmask[1]?2:0)|(S.cmask[2]?4:0)|(S.cmask[3]?8:0);
    mat_mul(st->mvp, pj[pj_n], mv[mv_n]);
    memcpy(st->tm, tmx[0][tm_n[0]], sizeof st->tm);
    build_unit(st, 0); build_unit(st, 1);
    if (S.alpha_test) { st->alpha_func = (int)S.alpha_func; st->alpha_ref = fminf(fmaxf(S.alpha_ref, 0.0f), 1.0f); }
}

static void apply_gl_state(const drawstate *st)
{
    if (applied_prog != prog) { R_UseProgram(prog); applied_prog = prog; have_applied = false; }
    const bool all = !have_applied;
    if (all || memcmp(st->vp, applied.vp, sizeof st->vp)) R_Viewport(st->vp[0], st->vp[1], st->vp[2], st->vp[3]);
    if (all || st->scissor != applied.scissor) { if (st->scissor) R_Enable(GL_SCISSOR_TEST); else R_Disable(GL_SCISSOR_TEST); }
    if (st->scissor && (all || memcmp(st->sc, applied.sc, sizeof st->sc))) R_Scissor(st->sc[0], st->sc[1], st->sc[2], st->sc[3]);
    if (all || st->depth_test != applied.depth_test) { if (st->depth_test) R_Enable(GL_DEPTH_TEST); else R_Disable(GL_DEPTH_TEST); }
    if (all || st->depth_func != applied.depth_func) R_DepthFunc(st->depth_func);
    if (all || st->depth_mask != applied.depth_mask) R_DepthMask(st->depth_mask ? GL_TRUE : GL_FALSE);
    if (all || st->blend != applied.blend) { if (st->blend) R_Enable(GL_BLEND); else R_Disable(GL_BLEND); }
    if (st->blend && (all || st->bsrc != applied.bsrc || st->bdst != applied.bdst)) R_BlendFunc(st->bsrc, st->bdst);
    if (all || st->cmask != applied.cmask)
        R_ColorMask((st->cmask&1)?GL_TRUE:GL_FALSE, (st->cmask&2)?GL_TRUE:GL_FALSE, (st->cmask&4)?GL_TRUE:GL_FALSE, (st->cmask&8)?GL_TRUE:GL_FALSE);
    if (all || memcmp(st->mvp, applied.mvp, sizeof st->mvp)) R_UniformMatrix4fv(U.mvp, 1, GL_FALSE, st->mvp);
    if (all || memcmp(st->tm, applied.tm, sizeof st->tm)) R_UniformMatrix4fv(U.tm, 1, GL_FALSE, st->tm);
    if (all || memcmp(st->cfg, applied.cfg, sizeof st->cfg)) R_Uniform4iv(U.cfg, 2, &st->cfg[0][0]);
    if (all || memcmp(st->srcrgb, applied.srcrgb, sizeof st->srcrgb)) R_Uniform4iv(U.srcrgb, 2, &st->srcrgb[0][0]);
    if (all || memcmp(st->oprgb, applied.oprgb, sizeof st->oprgb)) R_Uniform4iv(U.oprgb, 2, &st->oprgb[0][0]);
    if (all || memcmp(st->srca, applied.srca, sizeof st->srca)) R_Uniform4iv(U.srca, 2, &st->srca[0][0]);
    if (all || memcmp(st->opa, applied.opa, sizeof st->opa)) R_Uniform4iv(U.opa, 2, &st->opa[0][0]);
    if (all || memcmp(st->env, applied.env, sizeof st->env)) R_Uniform4fv(U.env, 2, &st->env[0][0]);
    if (all || memcmp(st->scale, applied.scale, sizeof st->scale)) R_Uniform4fv(U.scale, 2, &st->scale[0][0]);
    if (all || st->alpha_func != applied.alpha_func) R_Uniform1i(U.afunc, st->alpha_func);
    if (all || st->alpha_ref != applied.alpha_ref) R_Uniform1f(U.aref, st->alpha_ref);
    for (int i = 0; i < 2; i++)
        if (all || st->tex[i] != applied.tex[i]) { R_ActiveTexture(GL_TEXTURE0 + i); R_BindTexture(GL_TEXTURE_2D, st->tex[i]); }
    applied = *st; have_applied = true;
}

static const GLenum prim_gl[3] = { GL_TRIANGLES, GL_LINES, GL_POINTS };

static void flush_pending(void)
{
    if (!pend_on || pend_n == 0) { pend_on = false; return; }
    /* All of the per-draw GL state (program, viewport, scissor, uniforms, textures) is eye-independent: apply it once, upload
     * the vertices once. In stereo only the target FBO and u_eye repeat -- the parallax lives in each vertex's bias. */
    apply_gl_state(&pend);
    const size_t bytes = (size_t)pend_n * sizeof(Vtx);
    if (bytes > vbo_cap) { while (vbo_cap < bytes) vbo_cap *= 2; R_BufferData(RGL_ARRAY_BUFFER, (GLsizeiptr)vbo_cap, NULL, RGL_STREAM_DRAW); }
    else R_BufferData(RGL_ARRAY_BUFFER, (GLsizeiptr)vbo_cap, NULL, RGL_STREAM_DRAW);  /* orphan: fresh storage, no stall; the attrib layout stays bound to the buffer object */
    R_BufferSubData(RGL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, pend_v);
    if (st_mode) {
        R_Uniform1f(U.eye, -1.0f); bind_target_idx(st_L); R_DrawArrays(prim_gl[pend.prim], 0, pend_n);
        R_Uniform1f(U.eye,  1.0f); bind_target_idx(st_R); R_DrawArrays(prim_gl[pend.prim], 0, pend_n);
    } else {
        bind_target();
        R_DrawArrays(prim_gl[pend.prim], 0, pend_n);
    }
    pend_on = false; pend_n = 0;
}

static void submit_state(const drawstate *st, const Vtx *v, int n)
{
    if (n <= 0 || !created) return;
    if (st->sc[2] <= 0 || st->sc[3] <= 0) return;                       /* an empty scissor draws nothing */
    if (pend_on && memcmp(st, &pend, sizeof *st) == 0) {
        if (pend_n + n > pend_cap) { int c = pend_cap ? pend_cap : 4096; while (c < pend_n + n) c *= 2; pend_v = realloc(pend_v, sizeof(Vtx) * (size_t)c); pend_cap = c; }
        memcpy(pend_v + pend_n, v, (size_t)n * sizeof(Vtx)); pend_n += n; return;
    }
    flush_pending();
    if (n > pend_cap) { int c = pend_cap ? pend_cap : 4096; while (c < n) c *= 2; pend_v = realloc(pend_v, sizeof(Vtx) * (size_t)c); pend_cap = c; }
    memcpy(pend_v, v, (size_t)n * sizeof(Vtx));
    pend = *st; pend_on = true; pend_n = n;
}
static void submit(int prim, const Vtx *v, int n)
{
    if (n <= 0 || !created) return;
    drawstate st; build_state(&st, prim); submit_state(&st, v, n);
}
static Vtx *tmp_vtx(int n)
{
    if (n > tmpv_cap) { int c = tmpv_cap ? tmpv_cap : 1024; while (c < n) c *= 2; tmpv = realloc(tmpv, sizeof(Vtx) * (size_t)c); tmpv_cap = c; }
    return tmpv;
}
/* any GL primitive list to triangles / lines / points (verbatim from glmetal's draw_prims) */
static void draw_prims(GLenum mode, const Vtx *v, int n)
{
    switch (mode) {
    case GL_TRIANGLES: submit(PRIM_TRI, v, n - n % 3); break;
    case GL_LINES: submit(PRIM_LINE, v, n & ~1); break;
    case GL_POINTS: submit(PRIM_POINT, v, n); break;
    case GL_QUADS: {
        const int q = n / 4; Vtx *o = tmp_vtx(q * 6);
        for (int i = 0; i < q; i++) { const Vtx *s = v + i*4; o[i*6+0]=s[0]; o[i*6+1]=s[1]; o[i*6+2]=s[2]; o[i*6+3]=s[0]; o[i*6+4]=s[2]; o[i*6+5]=s[3]; }
        submit(PRIM_TRI, o, q * 6); break;
    }
    case GL_QUAD_STRIP: {
        const int q = n >= 4 ? (n - 2) / 2 : 0; Vtx *o = tmp_vtx(q * 6);
        for (int i = 0; i < q; i++) { const Vtx *s = v + i*2; o[i*6+0]=s[0]; o[i*6+1]=s[1]; o[i*6+2]=s[3]; o[i*6+3]=s[0]; o[i*6+4]=s[3]; o[i*6+5]=s[2]; }
        submit(PRIM_TRI, o, q * 6); break;
    }
    case GL_TRIANGLE_FAN: case GL_POLYGON: {
        const int t = n >= 3 ? n - 2 : 0; Vtx *o = tmp_vtx(t * 3);
        for (int i = 0; i < t; i++) { o[i*3]=v[0]; o[i*3+1]=v[i+1]; o[i*3+2]=v[i+2]; }
        submit(PRIM_TRI, o, t * 3); break;
    }
    case GL_TRIANGLE_STRIP: {
        const int t = n >= 3 ? n - 2 : 0; Vtx *o = tmp_vtx(t * 3);
        for (int i = 0; i < t; i++) { if (i & 1) { o[i*3]=v[i+1]; o[i*3+1]=v[i]; o[i*3+2]=v[i+2]; } else { o[i*3]=v[i]; o[i*3+1]=v[i+1]; o[i*3+2]=v[i+2]; } }
        submit(PRIM_TRI, o, t * 3); break;
    }
    case GL_LINE_STRIP: case GL_LINE_LOOP: {
        const int segs = n < 2 ? 0 : (mode == GL_LINE_LOOP ? n : n - 1); Vtx *o = tmp_vtx(segs * 2);
        for (int i = 0; i < segs; i++) { o[i*2]=v[i]; o[i*2+1]=v[(i+1)%n]; }
        submit(PRIM_LINE, o, segs * 2); break;
    }
    default: gl_err = GL_INVALID_ENUM; break;
    }
}

/* ------------------------------------------------------------------------------------------------ GL: immediate mode */
static void imm_push(float x, float y, float z)
{
    if (imm_n == imm_cap) { imm_cap = imm_cap ? imm_cap * 2 : 1024; imm = realloc(imm, sizeof(Vtx) * (size_t)imm_cap); }
    Vtx *v = &imm[imm_n++];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z; v->pos[3] = 1.0f;
    memcpy(v->col, S.color, sizeof v->col);
    memcpy(v->tc, S.tc, sizeof v->tc);
    v->bias = 0.0f;                         /* immediate mode = the 2D layers: zero parallax */
}
void glBegin(GLenum mode) { in_begin = true; imm_mode = mode; imm_n = 0; }
void glEnd(void) { if (!in_begin) { gl_err = GL_INVALID_OPERATION; return; } in_begin = false; draw_prims(imm_mode, imm, imm_n); imm_n = 0; }
void glVertex2f(GLfloat x, GLfloat y) { if (in_begin) imm_push(x, y, 0.0f); }
void glVertex2i(GLint x, GLint y) { if (in_begin) imm_push((float)x, (float)y, 0.0f); }
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { if (in_begin) imm_push(x, y, z); }
void glTexCoord2f(GLfloat s, GLfloat t) { S.tc[0] = s; S.tc[1] = t; S.tc[2] = 0.0f; S.tc[3] = 1.0f; }
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { S.tc[0] = s; S.tc[1] = t; S.tc[2] = r; S.tc[3] = q; }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { S.color[0]=r; S.color[1]=g; S.color[2]=b; S.color[3]=1.0f; }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.color[0]=r; S.color[1]=g; S.color[2]=b; S.color[3]=a; }
void glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { glColor4f(r/255.0f, g/255.0f, b/255.0f, a/255.0f); }

/* ------------------------------------------------------------------------------------------------ GL: client arrays */
static carray *array_for(GLenum a)
{
    switch (a) {
    case GL_VERTEX_ARRAY: return &ca_v;
    case GL_COLOR_ARRAY: return &ca_c;
    case GL_TEXTURE_COORD_ARRAY: return client_active == 0 ? &ca_t : NULL;
    default: return NULL;
    }
}
void glEnableClientState(GLenum a) { carray *c = array_for(a); if (c) c->on = true; }
void glDisableClientState(GLenum a) { carray *c = array_for(a); if (c) c->on = false; }
static void set_array(carray *c, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { c->size = size; c->type = type; c->stride = stride; c->ptr = ptr; }
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { set_array(&ca_v, size, type, stride, ptr); }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { set_array(&ca_c, size, type, stride, ptr); }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { if (client_active == 0) set_array(&ca_t, size, type, stride, ptr); }

static int type_size(GLenum t)
{
    switch (t) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2; case GL_DOUBLE: return 8; default: return 4; }
}
static void fetch(const carray *c, int idx, float *out, bool normalize)
{
    const int st = c->stride ? c->stride : c->size * type_size(c->type);
    const uint8_t *p = (const uint8_t *)c->ptr + (size_t)idx * (size_t)st;
    for (int k = 0; k < c->size && k < 4; k++) {
        switch (c->type) {
        case GL_FLOAT: { float f; memcpy(&f, p + k*4, 4); out[k] = f; break; }
        case GL_DOUBLE: { double d; memcpy(&d, p + k*8, 8); out[k] = (float)d; break; }
        case GL_UNSIGNED_BYTE: out[k] = normalize ? p[k] / 255.0f : p[k]; break;
        case GL_BYTE: out[k] = normalize ? ((int8_t)p[k]) / 127.0f : (int8_t)p[k]; break;
        case GL_SHORT: { int16_t s; memcpy(&s, p + k*2, 2); out[k] = normalize ? s / 32767.0f : s; break; }
        case GL_UNSIGNED_SHORT: { uint16_t s; memcpy(&s, p + k*2, 2); out[k] = normalize ? s / 65535.0f : s; break; }
        case GL_INT: { int32_t s; memcpy(&s, p + k*4, 4); out[k] = (float)s; break; }
        default: { uint32_t s; memcpy(&s, p + k*4, 4); out[k] = (float)s; break; }
        }
    }
}
static void gather(Vtx *v, int idx)
{
    v->pos[0] = v->pos[1] = v->pos[2] = 0.0f; v->pos[3] = 1.0f;
    if (ca_v.on && ca_v.ptr) fetch(&ca_v, idx, v->pos, false);
    if (ca_c.on && ca_c.ptr) { v->col[3] = 1.0f; fetch(&ca_c, idx, v->col, true); } else memcpy(v->col, S.color, sizeof v->col);
    if (ca_t.on && ca_t.ptr) { v->tc[0]=v->tc[1]=v->tc[2]=0.0f; v->tc[3]=1.0f; fetch(&ca_t, idx, v->tc, false); } else memcpy(v->tc, S.tc, sizeof v->tc);
    v->bias = bias_arr ? bias_arr[idx] : 0.0f;   /* STEREO: quad_gl's per-vertex off-axis shift (0 when none) */
}
static Vtx *gat_buf(int n) { if (n > gat_cap) { int c = gat_cap ? gat_cap : 4096; while (c < n) c *= 2; gat = realloc(gat, sizeof(Vtx) * (size_t)c); gat_cap = c; } return gat; }
void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (count <= 0 || !ca_v.on) return;
    Vtx *v = gat_buf(count);
    for (int i = 0; i < count; i++) gather(&v[i], first + i);
    draw_prims(mode, v, count);
}
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
    if (count <= 0 || !ca_v.on || !indices) return;
    Vtx *v = gat_buf(count);
    for (int i = 0; i < count; i++) {
        int idx = type == GL_UNSIGNED_SHORT ? ((const uint16_t *)indices)[i] : type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] : (int)((const uint32_t *)indices)[i];
        gather(&v[i], idx);
    }
    draw_prims(mode, v, count);
}

/* ------------------------------------------------------------------------------------------------ GL: state */
static bool *cap_flag(GLenum cap)
{
    switch (cap) {
    case GL_TEXTURE_2D: return &S.tex2d[S.active];
    case GL_ALPHA_TEST: return &S.alpha_test;
    case GL_BLEND: return &S.blend;
    case GL_SCISSOR_TEST: return &S.scissor_test;
    case GL_DEPTH_TEST: return &S.depth_test;
    default: return NULL;             /* CULL_FACE, FOG, LIGHTING, DITHER...: accepted, nothing to do */
    }
}
void glEnable(GLenum cap) { bool *f = cap_flag(cap); if (f) *f = true; }
void glDisable(GLenum cap) { bool *f = cap_flag(cap); if (f) *f = false; }
GLboolean glIsEnabled(GLenum cap) { bool *f = cap_flag(cap); return f && *f; }

void glMatrixMode(GLenum mode) { S.matrix_mode = mode; }
void glLoadIdentity(void) { mat_identity(cur_matrix()); }
void glLoadMatrixf(const GLfloat *m) { memcpy(cur_matrix(), m, 16 * sizeof(float)); }
void glMultMatrixf(const GLfloat *m) { float *c = cur_matrix(); mat_mul(c, c, m); }
void glPushMatrix(void)
{
    switch (S.matrix_mode) {
    case GL_PROJECTION: if (pj_n < 31) { memcpy(pj[pj_n+1], pj[pj_n], sizeof pj[0]); pj_n++; } break;
    case GL_TEXTURE: if (tm_n[S.active] < 7) { memcpy(tmx[S.active][tm_n[S.active]+1], tmx[S.active][tm_n[S.active]], sizeof tmx[0][0]); tm_n[S.active]++; } break;
    default: if (mv_n < 31) { memcpy(mv[mv_n+1], mv[mv_n], sizeof mv[0]); mv_n++; } break;
    }
}
void glPopMatrix(void)
{
    switch (S.matrix_mode) {
    case GL_PROJECTION: if (pj_n > 0) pj_n--; break;
    case GL_TEXTURE: if (tm_n[S.active] > 0) tm_n[S.active]--; break;
    default: if (mv_n > 0) mv_n--; break;
    }
}
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16] = { 0 };
    m[0] = (float)(2.0/(r-l)); m[5] = (float)(2.0/(t-b)); m[10] = (float)(-2.0/(f-n));
    m[12] = (float)(-(r+l)/(r-l)); m[13] = (float)(-(t+b)/(t-b)); m[14] = (float)(-(f+n)/(f-n)); m[15] = 1.0f;
    glMultMatrixf(m);
}
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16] = { 0 };
    m[0] = (float)(2.0*n/(r-l)); m[5] = (float)(2.0*n/(t-b)); m[8] = (float)((r+l)/(r-l)); m[9] = (float)((t+b)/(t-b));
    m[10] = (float)(-(f+n)/(f-n)); m[11] = -1.0f; m[14] = (float)(-2.0*f*n/(f-n));
    glMultMatrixf(m);
}
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { float m[16]; mat_identity(m); m[12]=x; m[13]=y; m[14]=z; glMultMatrixf(m); }
void glScalef(GLfloat x, GLfloat y, GLfloat z) { float m[16]; mat_identity(m); m[0]=x; m[5]=y; m[10]=z; glMultMatrixf(m); }
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    const float len = sqrtf(x*x + y*y + z*z); if (len <= 0.0f) return;
    x /= len; y /= len; z /= len;
    const float a = angle * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a), k = 1.0f - c;
    float m[16] = { x*x*k+c, y*x*k+z*s, x*z*k-y*s, 0,  x*y*k-z*s, y*y*k+c, y*z*k+x*s, 0,  x*z*k+y*s, y*z*k-x*s, z*z*k+c, 0,  0,0,0,1 };
    glMultMatrixf(m);
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { S.vp[0]=x; S.vp[1]=y; S.vp[2]=w; S.vp[3]=h; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { S.sc[0]=x; S.sc[1]=y; S.sc[2]=w; S.sc[3]=h; }
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { S.clear[0]=fminf(fmaxf(r,0),1); S.clear[1]=fminf(fmaxf(g,0),1); S.clear[2]=fminf(fmaxf(b,0),1); S.clear[3]=fminf(fmaxf(a,0),1); }
void glClearDepth(GLclampd d) { S.clear_depth = (float)fmin(fmax(d,0.0),1.0); }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { S.cmask[0]=r; S.cmask[1]=g; S.cmask[2]=b; S.cmask[3]=a; }
void glDepthMask(GLboolean flag) { S.depth_mask = flag; }
void glDepthFunc(GLenum func) { S.depth_func = func; }
void glBlendFunc(GLenum s, GLenum d) { S.bsrc = s; S.bdst = d; }
void glAlphaFunc(GLenum func, GLclampf ref) { S.alpha_func = func; S.alpha_ref = ref; }
void glLineWidth(GLfloat w) { S.line_width = w; }
void glShadeModel(GLenum mode) { (void)mode; }
void glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }

void glClear(GLbitfield mask)
{
    if (!created) return;
    flush_pending();
    const int W = tgt[cur_set].w, H = tgt[cur_set].h;
    if (S.scissor_test) { int sc[4] = { S.sc[0], S.sc[1], S.sc[2], S.sc[3] }; clamp_scissor(sc, W, H); R_Enable(GL_SCISSOR_TEST); R_Scissor(sc[0], sc[1], sc[2], sc[3]); }
    else R_Disable(GL_SCISSOR_TEST);
    GLbitfield gm = 0;
    if (mask & GL_COLOR_BUFFER_BIT) {
        R_ColorMask(S.cmask[0]?GL_TRUE:GL_FALSE, S.cmask[1]?GL_TRUE:GL_FALSE, S.cmask[2]?GL_TRUE:GL_FALSE, S.cmask[3]?GL_TRUE:GL_FALSE);
        R_ClearColor(S.clear[0], S.clear[1], S.clear[2], S.clear[3]);
        gm |= GL_COLOR_BUFFER_BIT;
    }
    if (mask & GL_DEPTH_BUFFER_BIT) { R_DepthMask(S.depth_mask ? GL_TRUE : GL_FALSE); R_ClearDepthf(S.clear_depth); if (S.depth_mask) gm |= GL_DEPTH_BUFFER_BIT; }
    if (gm) {
        if (st_mode) { bind_target_idx(st_L); R_Clear(gm); bind_target_idx(st_R); R_Clear(gm); }   /* STEREO: clear both eyes */
        else { bind_target(); R_Clear(gm); }
    }
    have_applied = false;                 /* scissor / masks / clear state no longer match `applied` */
}

/* ------------------------------------------------------------------------------------------------ GL: textures */
void glActiveTexture(GLenum t) { const int u = (int)t - GL_TEXTURE0; if (u >= 0 && u < 2) S.active = u; else gl_err = GL_INVALID_ENUM; }
void glClientActiveTexture(GLenum t) { const int u = (int)t - GL_TEXTURE0; if (u >= 0 && u < 2) client_active = u; }

void glTexEnvi(GLenum target, GLenum pname, GLint param)
{
    if (target != GL_TEXTURE_ENV) return;
    texenv *e = &S.env[S.active];
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: e->mode = (GLenum)param; break;
    case GL_COMBINE_RGB: e->comb_rgb = (GLenum)param; break;
    case GL_COMBINE_ALPHA: e->comb_a = (GLenum)param; break;
    case GL_SOURCE0_RGB: case GL_SOURCE1_RGB: case GL_SOURCE2_RGB: e->src_rgb[pname - GL_SOURCE0_RGB] = (GLenum)param; break;
    case GL_SOURCE0_ALPHA: case GL_SOURCE1_ALPHA: case GL_SOURCE2_ALPHA: e->src_a[pname - GL_SOURCE0_ALPHA] = (GLenum)param; break;
    case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB: e->op_rgb[pname - GL_OPERAND0_RGB] = (GLenum)param; break;
    case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA: e->op_a[pname - GL_OPERAND0_ALPHA] = (GLenum)param; break;
    case GL_RGB_SCALE: e->rgb_scale = (float)param; break;
    case GL_ALPHA_SCALE: e->a_scale = (float)param; break;
    default: gl_err = GL_INVALID_ENUM; break;
    }
}
void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    if (target != GL_TEXTURE_ENV) return;
    texenv *e = &S.env[S.active];
    if (pname == GL_RGB_SCALE) e->rgb_scale = param;
    else if (pname == GL_ALPHA_SCALE) e->a_scale = param;
    else glTexEnvi(target, pname, (GLint)param);
}
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{
    if (target != GL_TEXTURE_ENV) return;
    if (pname == GL_TEXTURE_ENV_COLOR) for (int i = 0; i < 4; i++) S.env[S.active].color[i] = fminf(fmaxf(params[i], 0.0f), 1.0f);
    else glTexEnvf(target, pname, params[0]);
}

void glGenTextures(GLsizei n, GLuint *out)
{
    if (!created) { for (int i = 0; i < n; i++) out[i] = 0; return; }
    R_GenTextures(n, out);
    for (int i = 0; i < n; i++) { gtex *t = tex_get(out[i]); if (t) t->tex = out[i]; }
}
void glDeleteTextures(GLsizei n, const GLuint *names)
{
    flush_pending();
    for (int i = 0; i < n; i++) {
        const GLuint name = names[i];
        if (name == 0 || (int)name >= ntexs || !texs[name].used) continue;
        if (texs[name].tex) R_DeleteTextures(1, &texs[name].tex);
        memset(&texs[name], 0, sizeof(gtex));
        for (int u = 0; u < 2; u++) if (S.bound[u] == name) S.bound[u] = 0;
    }
    have_applied = false;
}
void glBindTexture(GLenum target, GLuint name) { if (target != GL_TEXTURE_2D) return; if (name) tex_get(name); S.bound[S.active] = name; }
void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    if (target != GL_TEXTURE_2D) return;
    gtex *t = tex_get(S.bound[S.active]);
    if (!t) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->minf = (GLenum)param; break;
    case GL_TEXTURE_MAG_FILTER: t->magf = (GLenum)param; break;
    case GL_TEXTURE_WRAP_S: t->ws = (GLenum)param; break;
    case GL_TEXTURE_WRAP_T: t->wt = (GLenum)param; break;
    default: break;
    }
    if (t->tex) {
        R_ActiveTexture(GL_TEXTURE0); R_BindTexture(GL_TEXTURE_2D, t->tex);
        R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf_gl(t->minf));
        R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf_gl(t->magf));
        R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_gl(t->ws));
        R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_gl(t->wt));
        have_applied = false;
    }
}
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)border; (void)type;
    if (target != GL_TEXTURE_2D || level != 0 || !created) return;
    gtex *t = tex_get(S.bound[S.active]);
    if (!t || w <= 0 || h <= 0) return;
    flush_pending();
    if (!t->tex) { R_GenTextures(1, &t->tex); }
    t->w = w; t->h = h;
    t->rgb_only = internalformat == GL_RGB || internalformat == GL_RGB8 || internalformat == 3 || internalformat == GL_LUMINANCE;
    R_ActiveTexture(GL_TEXTURE0); R_BindTexture(GL_TEXTURE_2D, t->tex);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf_gl(t->minf));
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf_gl(t->magf));
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_gl(t->ws));
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_gl(t->wt));
    if (pixels) tex_upload(t, 0, 0, w, h, format, pixels, true);
    else { R_PixelStorei(GL_UNPACK_ALIGNMENT, 1); R_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL); R_PixelStorei(GL_UNPACK_ALIGNMENT, 4); }
    have_applied = false;
}
void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)type;
    if (target != GL_TEXTURE_2D || level != 0) return;
    flush_pending();
    gtex *t = tex_get(S.bound[S.active]);
    if (!t || !t->tex) return;
    if (x < 0 || y < 0 || x + w > t->w || y + h > t->h) { gl_err = GL_INVALID_VALUE; return; }
    tex_upload(t, x, y, w, h, format, pixels, false);
    have_applied = false;
}
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (target != GL_TEXTURE_2D || level != 0 || !created) return;
    gtex *t = tex_get(S.bound[S.active]);
    if (!t || !t->tex) return;
    flush_pending();
    bind_target();                        /* read from the current target's FBO */
    R_ActiveTexture(GL_TEXTURE0); R_BindTexture(GL_TEXTURE_2D, t->tex);
    R_CopyTexSubImage2D(GL_TEXTURE_2D, 0, xoff, yoff, x, y, w, h);
    have_applied = false;
}
void glPixelStorei(GLenum pname, GLint param)
{
    switch (pname) {
    case GL_UNPACK_ROW_LENGTH: unpack_row_len = param; break;
    case GL_UNPACK_ALIGNMENT: unpack_align = param; break;
    case GL_PACK_ALIGNMENT: pack_align = param; break;
    case GL_PACK_ROW_LENGTH: pack_row_len = param; break;
    default: break;
    }
}
void glPixelTransferi(GLenum pname, GLint param) { (void)pname; (void)param; }
void glPixelMapfv(GLenum map, GLsizei mapsize, const GLfloat *values) { (void)map; (void)mapsize; (void)values; }

/* ------------------------------------------------------------------------------------------------ GL: readback, queries */
void glReadBuffer(GLenum mode) { (void)mode; }
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels)
{
    if (!created || !pixels || w <= 0 || h <= 0 || type != GL_UNSIGNED_BYTE) return;
    flush_pending();
    bind_target();
    const int bpp = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB || format == GL_BGR ? 3 : 1;
    const size_t a = (size_t)(pack_align > 0 ? pack_align : 1);
    const size_t rowpix = pack_row_len > 0 ? (size_t)pack_row_len : (size_t)w;
    const size_t stride = (rowpix * (size_t)bpp + a - 1) / a * a;
    uint8_t *tmp = malloc((size_t)w * (size_t)h * 4);
    if (!tmp) { gl_err = GL_OUT_OF_MEMORY; return; }
    R_PixelStorei(GL_PACK_ALIGNMENT, 4);
    R_ReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);   /* GL rows: bottom-up already */
    for (int r = 0; r < h; r++) {
        const uint8_t *s = tmp + (size_t)r * (size_t)w * 4;
        uint8_t *d = (uint8_t *)pixels + (size_t)r * stride;
        for (int i = 0; i < w; i++, s += 4, d += bpp) {
            switch (format) {
            case GL_RGBA: d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; break;
            case GL_BGRA: d[0]=s[2]; d[1]=s[1]; d[2]=s[0]; d[3]=s[3]; break;
            case GL_RGB:  d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; break;
            case GL_BGR:  d[0]=s[2]; d[1]=s[1]; d[2]=s[0]; break;
            case GL_ALPHA: d[0]=s[3]; break;
            default: d[0]=s[0]; break;
            }
        }
    }
    free(tmp);
    have_applied = false;
}
void glGetIntegerv(GLenum pname, GLint *p)
{
    switch (pname) {
    case GL_VIEWPORT: memcpy(p, S.vp, sizeof S.vp); break;
    case GL_SCISSOR_BOX: memcpy(p, S.sc, sizeof S.sc); break;
    case GL_MAX_TEXTURE_SIZE: *p = 8192; break;
    case GL_TEXTURE_BINDING_2D: *p = (GLint)S.bound[S.active]; break;
    case GL_MATRIX_MODE: *p = (GLint)S.matrix_mode; break;
    case GL_ACTIVE_TEXTURE: *p = GL_TEXTURE0 + S.active; break;
    case GL_CLIENT_ACTIVE_TEXTURE: *p = GL_TEXTURE0 + client_active; break;
    case GL_MAX_TEXTURE_UNITS: *p = 2; break;
    case GL_BLEND_SRC: *p = (GLint)S.bsrc; break;
    case GL_BLEND_DST: *p = (GLint)S.bdst; break;
    case GL_UNPACK_ROW_LENGTH: *p = unpack_row_len; break;
    case GL_UNPACK_ALIGNMENT: *p = unpack_align; break;
    case GL_PACK_ALIGNMENT: *p = pack_align; break;
    default: *p = 0; break;
    }
}
const GLubyte *glGetString(GLenum name)
{
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"glgles";
    case GL_RENDERER: return (const GLubyte *)(renderer_str[0] ? renderer_str : "glgles");
    case GL_VERSION: return (const GLubyte *)"1.3 glgles (OpenGL ES 3.0)";
    case GL_EXTENSIONS: return (const GLubyte *)"GL_ARB_multitexture GL_ARB_texture_env_combine GL_EXT_bgra";
    default: return NULL;
    }
}
GLenum glGetError(void) { const GLenum e = gl_err; gl_err = GL_NO_ERROR; return e; }
void glFinish(void) { if (created) { flush_pending(); R_Finish(); } }
void glFlush(void) { if (created) { flush_pending(); R_Flush(); } }

void glPushAttrib(GLbitfield mask) { if (attr_n >= 16) return; attr_stack[attr_n] = S; attr_mask[attr_n] = mask; attr_n++; }
void glPopAttrib(void)
{
    if (attr_n <= 0) return;
    attr_n--;
    const srvstate *o = &attr_stack[attr_n];
    const GLbitfield m = attr_mask[attr_n];
    if (m & GL_ENABLE_BIT) { S.tex2d[0]=o->tex2d[0]; S.tex2d[1]=o->tex2d[1]; S.alpha_test=o->alpha_test; S.blend=o->blend; S.scissor_test=o->scissor_test; S.depth_test=o->depth_test; }
    if (m & GL_COLOR_BUFFER_BIT) { S.alpha_test=o->alpha_test; S.alpha_func=o->alpha_func; S.alpha_ref=o->alpha_ref; S.blend=o->blend; S.bsrc=o->bsrc; S.bdst=o->bdst; memcpy(S.cmask, o->cmask, sizeof S.cmask); memcpy(S.clear, o->clear, sizeof S.clear); }
    if (m & GL_DEPTH_BUFFER_BIT) { S.depth_test=o->depth_test; S.depth_func=o->depth_func; S.depth_mask=o->depth_mask; S.clear_depth=o->clear_depth; }
    if (m & GL_VIEWPORT_BIT) memcpy(S.vp, o->vp, sizeof S.vp);
    if (m & GL_SCISSOR_BIT) { S.scissor_test=o->scissor_test; memcpy(S.sc, o->sc, sizeof S.sc); }
    if (m & GL_TEXTURE_BIT) { memcpy(S.env, o->env, sizeof S.env); memcpy(S.bound, o->bound, sizeof S.bound); S.active=o->active; S.tex2d[0]=o->tex2d[0]; S.tex2d[1]=o->tex2d[1]; }
    if (m & GL_CURRENT_BIT) { memcpy(S.color, o->color, sizeof S.color); memcpy(S.tc, o->tc, sizeof S.tc); }
    if (m & GL_TRANSFORM_BIT) S.matrix_mode = o->matrix_mode;
    if (m & GL_LINE_BIT) S.line_width = o->line_width;
}

/* ------------------------------------------------------------------------------------------------ proc lookup */
typedef struct { const char *name; void *fn; } procent;
#define P(n) { #n, (void *)n }
static const procent procs[] = {
    P(glBegin), P(glEnd), P(glVertex2f), P(glVertex2i), P(glVertex3f), P(glTexCoord2f), P(glTexCoord4f), P(glColor3f), P(glColor4f),
    P(glColor4ub), P(glColor4fv), P(glEnable), P(glDisable), P(glIsEnabled), P(glEnableClientState), P(glDisableClientState),
    P(glVertexPointer), P(glColorPointer), P(glTexCoordPointer), P(glDrawArrays), P(glDrawElements), P(glMatrixMode),
    P(glLoadIdentity), P(glPushMatrix), P(glPopMatrix), P(glLoadMatrixf), P(glMultMatrixf), P(glOrtho), P(glFrustum),
    P(glTranslatef), P(glScalef), P(glRotatef), P(glViewport), P(glScissor), P(glClearColor), P(glClearDepth), P(glClear),
    P(glColorMask), P(glDepthMask), P(glDepthFunc), P(glBlendFunc), P(glAlphaFunc), P(glLineWidth), P(glShadeModel), P(glHint),
    P(glActiveTexture), P(glClientActiveTexture), P(glTexEnvi), P(glTexEnvf), P(glTexEnvfv), P(glGenTextures),
    P(glDeleteTextures), P(glBindTexture), P(glTexParameteri), P(glTexImage2D), P(glTexSubImage2D), P(glCopyTexSubImage2D),
    P(glPixelStorei), P(glPixelTransferi), P(glPixelMapfv), P(glReadBuffer), P(glReadPixels), P(glGetIntegerv), P(glGetString),
    P(glGetError), P(glFinish), P(glFlush), P(glPushAttrib), P(glPopAttrib),
    { "glActiveTextureARB", (void *)glActiveTexture }, { "glClientActiveTextureARB", (void *)glClientActiveTexture },
};
#undef P
void *glgles_GetProcAddress(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof procs / sizeof procs[0]; i++) if (!strcmp(procs[i].name, name)) return procs[i].fn;
    return NULL;                           /* shaders, programs, framebuffer objects: not provided (as glmetal) */
}

/* ------------------------------------------------------------------------------------------------ host API */
int glgles_create(int w, int h)
{
    if (created) return (w == base_w && h == base_h);
    if (w < 1 || h < 1) return 0;
    if (!load_gl()) return 0;
    GLint major = 0, minor = 0;
    R_GetIntegerv(RGL_MAJOR_VERSION, &major); R_GetIntegerv(RGL_MINOR_VERSION, &minor);
    if (major < 3) { fprintf(stderr, "[GLGLES] OpenGL ES 3.0 needed (context %d.%d)\n", major, minor); return 0; }
    while (R_GetError() != GL_NO_ERROR) {}

    fetch_ok = has_ext("GL_EXT_shader_framebuffer_fetch");
    const char *nf = getenv("GLGLES_NO_FETCH"); if (nf && *nf == '1') fetch_ok = false;

    prog = link_prog(VS_SRC, FS_SRC, true);
    if (!prog) return 0;
    U.mvp = R_GetUniformLocation(prog, "u_mvp"); U.tm = R_GetUniformLocation(prog, "u_tm"); U.yflip = R_GetUniformLocation(prog, "u_yflip"); U.eye = R_GetUniformLocation(prog, "u_eye");
    U.cfg = R_GetUniformLocation(prog, "u_cfg"); U.srcrgb = R_GetUniformLocation(prog, "u_srcrgb"); U.oprgb = R_GetUniformLocation(prog, "u_oprgb");
    U.srca = R_GetUniformLocation(prog, "u_srca"); U.opa = R_GetUniformLocation(prog, "u_opa"); U.env = R_GetUniformLocation(prog, "u_env");
    U.scale = R_GetUniformLocation(prog, "u_scale"); U.afunc = R_GetUniformLocation(prog, "u_afunc"); U.aref = R_GetUniformLocation(prog, "u_aref");
    U.tex0 = R_GetUniformLocation(prog, "u_tex0"); U.tex1 = R_GetUniformLocation(prog, "u_tex1");
    R_UseProgram(prog);
    R_Uniform1i(U.tex0, 0); R_Uniform1i(U.tex1, 1); R_Uniform1f(U.yflip, 1.0f); R_Uniform1f(U.eye, 0.0f);

    prog_post = fetch_ok ? link_prog(POST_VS, POST_FS_FETCH, false) : 0;
    if (prog_post) UP.lut = R_GetUniformLocation(prog_post, "u_lut");
    else fetch_ok = false;
    prog_post_copy = link_prog(POST_VS, POST_FS_COPY, false);
    if (prog_post_copy) { UPC.lut = R_GetUniformLocation(prog_post_copy, "u_lut"); UPC.src = R_GetUniformLocation(prog_post_copy, "u_src"); }
    if (!prog_post && !prog_post_copy) fprintf(stderr, "[GLGLES] no gamma program (gamma pass will be skipped)\n");

    R_GenVertexArrays(1, &vao); R_BindVertexArray(vao);
    R_EnableVertexAttribArray(0); R_EnableVertexAttribArray(1); R_EnableVertexAttribArray(2); R_EnableVertexAttribArray(3);
    R_GenBuffers(1, &vbo);
    R_BindBuffer(RGL_ARRAY_BUFFER, vbo);
    vbo_cap = 1u << 16;
    R_BufferData(RGL_ARRAY_BUFFER, (GLsizeiptr)vbo_cap, NULL, RGL_STREAM_DRAW);
    R_VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (const void *)0);   /* bound once: flush_pending never re-issues these */
    R_VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (const void *)16);
    R_VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (const void *)32);
    R_VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), (const void *)48);

    R_GenTextures(1, &lut_tex);
    R_BindTexture(GL_TEXTURE_2D, lut_tex);
    R_PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    R_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    R_PixelStorei(GL_UNPACK_ALIGNMENT, 4);

    R_Disable(GL_CULL_FACE); R_Disable(GL_DITHER); R_BlendEquation(RGL_FUNC_ADD);

    base_w = w; base_h = h; cur_set = 0;
    for (int i = 0; i < GLGLES_MAX_TARGETS; i++) { memset(&tgt[i], 0, sizeof tgt[i]); }
    if (!make_target(&tgt[0], w, h)) return 0;
    state_defaults();
    S.vp[0]=0; S.vp[1]=0; S.vp[2]=w; S.vp[3]=h;
    S.sc[0]=0; S.sc[1]=0; S.sc[2]=w; S.sc[3]=h;
    have_applied = false; applied_prog = 0;
    created = true;
    snprintf(renderer_str, sizeof renderer_str, "glgles on %s (%s gamma)", (const char *)R_GetString(GL_RENDERER), fetch_ok ? "framebuffer-fetch" : "copy");
    fprintf(stderr, "[GLGLES] %dx%d, %s; gamma by %s\n", w, h, renderer_str, fetch_ok ? "framebuffer fetch" : "copy-and-redraw");
    return 1;
}

void glgles_set_target_size(int i, int w, int h)
{
    if (i <= 0 || i >= GLGLES_MAX_TARGETS) return;
    if (w < 1 || h < 1) w = h = 0;
    tgt[i].req_w = w; tgt[i].req_h = h;
}
void glgles_set_target(int i)
{
    if (!created) return;
    if (i < 0 || i >= GLGLES_MAX_TARGETS) i = 0;
    const int want_w = i == 0 ? base_w : (tgt[i].req_w ? tgt[i].req_w : base_w);
    const int want_h = i == 0 ? base_h : (tgt[i].req_h ? tgt[i].req_h : base_h);
    if (i == cur_set && tgt[i].made && tgt[i].w == want_w && tgt[i].h == want_h) return;
    flush_pending();
    if (tgt[i].made && (tgt[i].w != want_w || tgt[i].h != want_h)) {
        R_DeleteFramebuffers(1, &tgt[i].fbo); R_DeleteTextures(1, &tgt[i].color[0]);
        if (tgt[i].color[1]) R_DeleteTextures(1, &tgt[i].color[1]); R_DeleteRenderbuffers(1, &tgt[i].depth);
        memset(&tgt[i], 0, sizeof tgt[i]); tgt[i].req_w = (i == 0 ? 0 : want_w); tgt[i].req_h = (i == 0 ? 0 : want_h);
    }
    if (!tgt[i].made) make_target(&tgt[i], want_w, want_h);
    cur_set = i;
    have_applied = false;
}
int glgles_current_target(void) { return cur_set; }
unsigned glgles_target_texture_n(int i) { if (i < 0 || i >= GLGLES_MAX_TARGETS || !tgt[i].made) return 0; return tgt[i].color[tgt[i].cur]; }
unsigned glgles_target_texture(void) { return glgles_target_texture_n(cur_set); }
int glgles_width(void) { return created ? tgt[cur_set].w : 0; }
int glgles_height(void) { return created ? tgt[cur_set].h : 0; }
int glgles_target_bottom_up(void) { return 1; }

/* STEREO: draw the next frame to BOTH `left` and `right` targets from one emitted stream (see the Vtx.bias note). Each is made
 * at target 0's size on first use. Until glgles_stereo_end, every flush / clear / gamma pass runs twice, with u_eye -1 then +1. */
void glgles_stereo_begin(int left, int right)
{
    if (!created) return;
    flush_pending();
    if (left  < 0 || left  >= GLGLES_MAX_TARGETS) left  = 0;
    if (right < 0 || right >= GLGLES_MAX_TARGETS) right = 0;
    glgles_set_target(right);                 /* make `right` (and flush), then settle on `left` as the representative */
    glgles_set_target(left);
    st_L = left; st_R = right; st_mode = 1;
}
void glgles_stereo_end(void)
{
    if (!created) return;
    flush_pending();
    st_mode = 0;
    R_UseProgram(prog); applied_prog = prog;
    R_Uniform1f(U.eye, 0.0f);                 /* back to mono: bias has no effect */
    have_applied = false;
}
/* STEREO: the per-vertex off-axis shift for the next client-array draws (quad_gl's bb_b), one float a vertex; NULL = none. */
void glgles_set_bias_array(const float *bias) { bias_arr = bias; }

void *glgles_end_frame(void)
{
    if (!created) return NULL;
    flush_pending();
    GLsync_ f = R_FenceSync(RGL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    R_Flush();
    return f;
}

static void ensure_scratch(int w, int h)
{
    if (scratch_tex && scratch_w == w && scratch_h == h) return;
    if (scratch_tex) R_DeleteTextures(1, &scratch_tex);
    R_GenTextures(1, &scratch_tex);
    R_BindTexture(GL_TEXTURE_2D, scratch_tex);
    R_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    R_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    scratch_w = w; scratch_h = h;
}
static void post_lut_one(int idx, int vw, int vh)
{
    if (fetch_ok && prog_post) {
        bind_target_idx(idx);
        R_UseProgram(prog_post);
        R_Uniform1i(UP.lut, 1);
        R_DrawArrays(GL_TRIANGLES, 0, 3);
    } else if (prog_post_copy) {
        /* copy the current colour into scratch, then redraw it through the table */
        bind_target_idx(idx);
        ensure_scratch(tgt[idx].w, tgt[idx].h);
        R_ActiveTexture(GL_TEXTURE0); R_BindTexture(GL_TEXTURE_2D, scratch_tex);
        R_CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, vw, vh);
        R_UseProgram(prog_post_copy);
        R_Uniform1i(UPC.src, 0); R_Uniform1i(UPC.lut, 1);
        R_DrawArrays(GL_TRIANGLES, 0, 3);
    }
}
void glgles_post_lut(const uint8_t lut[3][256], int vw, int vh)
{
    if (!created) return;
    flush_pending();
    target *t = &tgt[st_mode ? st_L : cur_set];       /* both eyes are the same size */
    if (vw > t->w) vw = t->w; if (vh > t->h) vh = t->h;
    if (vw <= 0 || vh <= 0) return;
    uint8_t l4[256][4];
    for (int v = 0; v < 256; v++) { l4[v][0]=lut[0][v]; l4[v][1]=lut[1][v]; l4[v][2]=lut[2][v]; l4[v][3]=255; }
    R_ActiveTexture(GL_TEXTURE1);
    R_BindTexture(GL_TEXTURE_2D, lut_tex);
    R_PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    R_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, l4);
    R_PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    R_Disable(GL_DEPTH_TEST); R_Disable(GL_BLEND); R_Disable(GL_SCISSOR_TEST);
    R_DepthMask(GL_FALSE); R_ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    R_Viewport(0, 0, vw, vh);
    if (st_mode) { post_lut_one(st_L, vw, vh); post_lut_one(st_R, vw, vh); }   /* STEREO: gamma each eye */
    else post_lut_one(cur_set, vw, vh);
    applied_prog = 0; have_applied = false;   /* program/state no longer match the uber-shader's cache */
}

void glgles_destroy(void)
{
    if (s_libgl && created) {
        flush_pending();
        R_Finish();
        for (int i = 0; i < GLGLES_MAX_TARGETS; i++) if (tgt[i].made) {
            R_DeleteFramebuffers(1, &tgt[i].fbo); R_DeleteTextures(1, &tgt[i].color[0]);
            if (tgt[i].color[1]) R_DeleteTextures(1, &tgt[i].color[1]); R_DeleteRenderbuffers(1, &tgt[i].depth);
        }
        for (int i = 0; i < ntexs; i++) if (texs[i].used && texs[i].tex) R_DeleteTextures(1, &texs[i].tex);
        if (vbo) R_DeleteBuffers(1, &vbo);
        if (vao) R_DeleteVertexArrays(1, &vao);
        if (lut_tex) R_DeleteTextures(1, &lut_tex);
        if (scratch_tex) R_DeleteTextures(1, &scratch_tex);
        if (prog) R_DeleteProgram(prog);
        if (prog_post) R_DeleteProgram(prog_post);
        if (prog_post_copy) R_DeleteProgram(prog_post_copy);
    }
    free(texs); texs = NULL; ntexs = 0;
    free(imm); imm = NULL; imm_cap = imm_n = 0;
    free(tmpv); tmpv = NULL; tmpv_cap = 0;
    free(gat); gat = NULL; gat_cap = 0;
    free(pend_v); pend_v = NULL; pend_cap = pend_n = 0; pend_on = false;
    memset(tgt, 0, sizeof tgt);
    vbo = 0; vbo_cap = 0;
    vao = lut_tex = scratch_tex = 0; scratch_w = scratch_h = 0;
    prog = prog_post = prog_post_copy = 0; applied_prog = 0; have_applied = false;
    st_mode = 0; st_L = st_R = 0; bias_arr = NULL;
    created = false;
    if (s_libgl) { dlclose(s_libgl); s_libgl = NULL; }
}
