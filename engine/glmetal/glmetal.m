/*
 * glmetal.m -- the engine's OpenGL subset (glmetal/gl.h) on Metal. Objective-C with ARC; Metal + Foundation only
 * (macOS, iOS, visionOS).
 *
 * MODEL
 *   - The default framebuffer is an RGBA8 MTLTexture (tgt[cur]). GL window rows map 1:1 onto texture rows when
 *     `bottom_up` (GLMETAL_BOTTOMUP=1): the vertex stage negates y, viewport and scissor keep GL's y, and
 *     glReadPixels / glCopyTexSubImage2D copy rows straight. The default is TOP-DOWN (row 0 = the picture's top, what a
 *     presenter wants): rasterisation ties then fall exactly as Apple's OpenGL makes them (measured: identical frames),
 *     and readbacks flip rows.
 *   - Every draw (glEnd, glDrawArrays, glDrawElements) is turned into a triangle / line / point list in a per-frame
 *     vertex arena and described by a POD `drawstate` (pipeline key, depth key, viewport, scissor, textures,
 *     samplers, vertex and fragment uniforms). A draw whose state equals the pending one and whose vertices follow
 *     it extends it; anything else encodes the pending draw first. One uber-shader (fs_main) does the fixed-function
 *     work from the uniforms: two texture units with the full GL 1.3 texenv/combine set, then the alpha test.
 *   - One command buffer per segment: a segment ends at glmetal_end_frame, glFinish/glFlush or a readback.
 *     glClear over the whole target becomes the next render pass's load action; a partial clear is a quad.
 *   - TEXTURE UPLOADS keep GL's ordering: a texture no uncompleted GPU work references is written by the CPU at once
 *     (replaceRegion); otherwise the pixels go through the arena and a blit encoded in stream order (the render pass
 *     is ended and resumed with a load), so draws issued before the upload still see the old texels.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <simd/simd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gl.h"

/* ------------------------------------------------------------------------------------------------ shaders */
static NSString *const kShaderSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Vtx { float4 pos; float4 col; float4 tc; };\n"
"struct VSU { float4x4 mvp; float4x4 tm; float4 misc; };\n"
"struct VOut { float4 pos [[position]]; float4 col; float4 tc; float psize [[point_size]]; };\n"
"vertex VOut vs_main(uint vid [[vertex_id]], const device Vtx *v [[buffer(0)]], constant VSU &u [[buffer(1)]]) {\n"
"    VOut o;\n"
"    float4 p = u.mvp * v[vid].pos;\n"
"    p.y *= u.misc.x;                       /* -1: the target is stored bottom-up */\n"
"    p.z = (p.z + p.w) * 0.5;               /* GL's [-1,1] depth to Metal's [0,1] */\n"
"    o.pos = p;\n"
"    o.col = saturate(v[vid].col);           /* fixed function clamps the vertex colour */\n"
"    o.tc = u.tm * v[vid].tc;\n"
"    o.psize = 1.0;\n"
"    return o;\n"
"}\n"
"struct UnitU { int4 cfg; int4 src_rgb; int4 op_rgb; int4 src_a; int4 op_a; float4 env; float4 scale; };\n"
"struct FSU { UnitU u[2]; int4 misc; float4 miscf; };\n"
"static float4 pick(int src, int unit, float4 t0, float4 t1, float4 prim, float4 prev, float4 env) {\n"
"    switch (src) {\n"
"    case 0x1702: return unit == 0 ? t0 : t1;   /* GL_TEXTURE */\n"
"    case 0x84C0: return t0;                    /* GL_TEXTURE0 */\n"
"    case 0x84C1: return t1;                    /* GL_TEXTURE1 */\n"
"    case 0x8576: return env;                   /* GL_CONSTANT */\n"
"    case 0x8577: return prim;                  /* GL_PRIMARY_COLOR */\n"
"    default:     return prev;                  /* GL_PREVIOUS */\n"
"    }\n"
"}\n"
"static float3 op_rgb(float4 s, int op) {\n"
"    switch (op) {\n"
"    case 0x0300: return s.rgb;\n"
"    case 0x0301: return 1.0 - s.rgb;\n"
"    case 0x0302: return float3(s.a);\n"
"    default:     return float3(1.0 - s.a);\n"
"    }\n"
"}\n"
"static float op_a(float4 s, int op) { return op == 0x0303 ? 1.0 - s.a : s.a; }\n"
"static float3 comb3(int f, float3 a0, float3 a1, float3 a2) {\n"
"    switch (f) {\n"
"    case 0x1E01: return a0;\n"
"    case 0x2100: return a0 * a1;\n"
"    case 0x0104: return a0 + a1;\n"
"    case 0x8574: return a0 + a1 - 0.5;\n"
"    case 0x8575: return a0 * a2 + a1 * (1.0 - a2);\n"
"    case 0x84E7: return a0 - a1;\n"
"    default:     return a0 * a1;\n"
"    }\n"
"}\n"
"static float comb1(int f, float a0, float a1, float a2) {\n"
"    switch (f) {\n"
"    case 0x1E01: return a0;\n"
"    case 0x2100: return a0 * a1;\n"
"    case 0x0104: return a0 + a1;\n"
"    case 0x8574: return a0 + a1 - 0.5;\n"
"    case 0x8575: return a0 * a2 + a1 * (1.0 - a2);\n"
"    case 0x84E7: return a0 - a1;\n"
"    default:     return a0 * a1;\n"
"    }\n"
"}\n"
"fragment float4 fs_main(VOut in [[stage_in]], constant FSU &u [[buffer(0)]],\n"
"                        texture2d<float> tx0 [[texture(0)]], texture2d<float> tx1 [[texture(1)]],\n"
"                        sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]]) {\n"
"    const float4 prim = in.col;\n"
"    float4 prev = prim;\n"
"    float4 t0 = float4(1.0), t1 = float4(1.0);\n"
"    if (u.u[0].cfg.x != 0) t0 = tx0.sample(s0, in.tc.xy / in.tc.w);\n"
"    if (u.u[1].cfg.x != 0) t1 = tx1.sample(s1, float2(0.0));       /* unit 1 has no coordinate array: (0,0,0,1) */\n"
"    for (int i = 0; i < 2; i++) {\n"
"        const int4 cfg = u.u[i].cfg;\n"
"        if (cfg.x == 0) continue;\n"
"        const float4 t = i == 0 ? t0 : t1;\n"
"        const float4 env = u.u[i].env;\n"
"        float4 r;\n"
"        switch (cfg.y) {\n"
"        case 0x1E01: r = t; break;                                               /* REPLACE */\n"
"        case 0x2100: r = prev * t; break;                                        /* MODULATE */\n"
"        case 0x2101: r = float4(mix(prev.rgb, t.rgb, t.a), prev.a); break;       /* DECAL */\n"
"        case 0x0BE2: r = float4(mix(prev.rgb, env.rgb, t.rgb), prev.a * t.a); break;  /* BLEND */\n"
"        case 0x0104: r = float4(prev.rgb + t.rgb, prev.a * t.a); break;          /* ADD */\n"
"        default: {                                                               /* COMBINE */\n"
"            const int4 sr = u.u[i].src_rgb, orr = u.u[i].op_rgb, sa = u.u[i].src_a, oa = u.u[i].op_a;\n"
"            float3 a0 = op_rgb(pick(sr.x, i, t0, t1, prim, prev, env), orr.x);\n"
"            float3 a1 = op_rgb(pick(sr.y, i, t0, t1, prim, prev, env), orr.y);\n"
"            float3 a2 = op_rgb(pick(sr.z, i, t0, t1, prim, prev, env), orr.z);\n"
"            float b0 = op_a(pick(sa.x, i, t0, t1, prim, prev, env), oa.x);\n"
"            float b1 = op_a(pick(sa.y, i, t0, t1, prim, prev, env), oa.y);\n"
"            float b2 = op_a(pick(sa.z, i, t0, t1, prim, prev, env), oa.z);\n"
"            r = float4(comb3(cfg.z, a0, a1, a2) * u.u[i].scale.x, comb1(cfg.w, b0, b1, b2) * u.u[i].scale.y);\n"
"        }\n"
"        }\n"
"        prev = saturate(r);\n"
"    }\n"
"    const float a = prev.a, ref = u.miscf.x;\n"
"    switch (u.misc.x) {                     /* the alpha test; 0 = off */\n"
"    case 0x0200: discard_fragment(); break;\n"
"    case 0x0201: if (!(a <  ref)) discard_fragment(); break;\n"
"    case 0x0202: if (!(a == ref)) discard_fragment(); break;\n"
"    case 0x0203: if (!(a <= ref)) discard_fragment(); break;\n"
"    case 0x0204: if (!(a >  ref)) discard_fragment(); break;\n"
"    case 0x0205: if (!(a != ref)) discard_fragment(); break;\n"
"    case 0x0206: if (!(a >= ref)) discard_fragment(); break;\n"
"    default: break;\n"
"    }\n"
"    return prev;\n"
"}\n"
"static float4 lut_apply(float4 c, constant uchar4 *lut) {\n"
"    const uint3 i = uint3(rint(saturate(c.rgb) * 255.0));\n"
"    return float4(float(lut[i.r].r), float(lut[i.g].g), float(lut[i.b].b), 255.0) / 255.0;\n"
"}\n"
"fragment float4 fs_post_read(VOut in [[stage_in]], texture2d<float> src [[texture(0)]], constant uchar4 *lut [[buffer(0)]]) {\n"
"    return lut_apply(src.read(uint2(in.pos.xy)), lut);\n"
"}\n";

/* framebuffer fetch: Apple GPUs only, so its own library */
static NSString *const kFetchSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float4 col; float4 tc; float psize [[point_size]]; };\n"
"fragment float4 fs_post_fetch(VOut in [[stage_in]], float4 dst [[color(0)]], constant uchar4 *lut [[buffer(0)]]) {\n"
"    const uint3 i = uint3(rint(saturate(dst.rgb) * 255.0));\n"
"    return float4(float(lut[i.r].r), float(lut[i.g].g), float(lut[i.b].b), 255.0) / 255.0;\n"
"}\n";

/* ------------------------------------------------------------------------------------------------ CPU-side structs */
typedef struct { float pos[4]; float col[4]; float tc[4]; } Vtx;
typedef struct { simd_float4x4 mvp; simd_float4x4 tm; simd_float4 misc; } VSU;
typedef struct { simd_int4 cfg, src_rgb, op_rgb, src_a, op_a; simd_float4 env; simd_float4 scale; } UnitU;
typedef struct { UnitU u[2]; simd_int4 misc; simd_float4 miscf; } FSU;

enum { PRIM_TRI = 0, PRIM_LINE = 1, PRIM_POINT = 2 };
enum { VAR_MAIN = 0, VAR_POST_FETCH = 1, VAR_POST_READ = 2 };

typedef struct {
    uint32_t pipe, ds;
    int32_t  prim;
    int32_t  vp[4];          /* Metal viewport, target pixels */
    int32_t  sc[4];          /* Metal scissor, clamped to the target */
    void    *tex[2];         /* MTLTexture of each unit (NULL = unit off) */
    uint32_t samp[2];
    uint32_t pad;
    VSU vsu;
    FSU fsu;
} drawstate;

typedef struct {
    GLenum mode, comb_rgb, comb_a;
    GLenum src_rgb[3], op_rgb[3], src_a[3], op_a[3];
    float rgb_scale, a_scale, color[4];
} texenv;

/* server state that glPushAttrib can save */
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
    void *mtl;                         /* id<MTLTexture>, retained */
    int w, h;
    bool rgb_only;                     /* internal format without alpha: alpha reads 1 */
    GLenum minf, magf, ws, wt;
    uint64_t serial;                   /* the last segment whose GPU work references it */
} gtex;

typedef struct { void *buf; size_t size; uint64_t serial; } chunk;
typedef struct { bool on; int size; GLenum type; int stride; const void *ptr; } carray;

/* ------------------------------------------------------------------------------------------------ globals */
static id<MTLDevice>              dev;
static id<MTLCommandQueue>        queue;
static id<MTLLibrary>             lib, lib_fetch;
static id<MTLFunction>            fn_vs, fn_fs, fn_post_read, fn_post_fetch;
static id<MTLTexture>             tgt[2], depth_tex, dummy_tex;
static id<MTLCommandBuffer>       cb;
static id<MTLRenderCommandEncoder> enc;
static id<MTLBlitCommandEncoder>  blit;
static id<MTLBuffer>              readbuf;
static id<MTLCommandBuffer>       inflight[2];
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pipes;
static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>>   *dstates;
static NSMutableDictionary<NSNumber *, id<MTLSamplerState>>        *samplers;

static int TW, TH, cur;                 /* target size, which of tgt[] is the framebuffer */
/* STEREO / PANORAMA (glmetal_set_target): more framebuffers -- each its own pair (the gamma ping-pong flips within a pair), its own size
 * and depth buffer -- parked in slot[] while another is active. tgt/cur/tgt_fresh/TW/TH/depth_tex/depth_fresh always describe the
 * ACTIVE one; switching saves them into its slot and loads the new one's. Target 0 is glmetal_create's; the others default to its
 * size (glmetal_set_target_size gives one its own). */
typedef struct { id<MTLTexture> t[2], depth; int cur, w, h, req_w, req_h; bool fresh, depth_fresh; } tslot;
static tslot slot[GLMETAL_MAX_TARGETS];
static int cur_set;
static int base_w, base_h;              /* target 0's size (glmetal_create) */
static int bottom_up = 0;
static bool fetch_ok;
static bool tgt_fresh, clear_pending, depth_on, depth_fresh, depth_clear_pending;
static float clear_rgba[4], depth_clear_val = 1.0f;
static uint64_t cur_serial;             /* the open segment's serial (0 = none open) */
static uint64_t next_serial = 1;
static _Atomic uint64_t done_serial;

static chunk *chunks; static int nchunks, cur_chunk = -1; static size_t chunk_off;

static gtex *texs; static int ntexs;     /* index = GL name */
static GLuint *freenames; static int nfree, capfree;

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

static drawstate pend; static bool pend_on; static void *pend_buf; static size_t pend_off; static int pend_n;
static drawstate encst; static bool enc_has; static void *enc_vbuf;

static char renderer_str[160];

/* GLMETAL_STATS=1: per-segment averages every 600 segments (a segment = a frame, or what a readback cut off) */
static long st_draws, st_passes, st_blitup, st_cpuup, st_seg, st_verts;
static void stats_tick(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("GLMETAL_STATS"); on = e && *e == '1'; }
    if (!on || ++st_seg < 600) return;
    fprintf(stderr, "[GLMETAL] per segment: %.1f draws, %.1f render passes, %.0f vertices, uploads %.1f by blit / %.1f by CPU\n",
            st_draws / (double)st_seg, st_passes / (double)st_seg, st_verts / (double)st_seg, st_blitup / (double)st_seg, st_cpuup / (double)st_seg);
    st_draws = st_passes = st_blitup = st_cpuup = st_seg = st_verts = 0;
}

/* ------------------------------------------------------------------------------------------------ helpers */
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
static simd_float4x4 to_simd(const float *m)
{
    simd_float4x4 r;
    memcpy(&r, m, sizeof r);
    return r;
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

/* ------------------------------------------------------------------------------------------------ segments */
static void ensure_cb(void)
{
    if (cb) return;
    cb = [queue commandBuffer];
    cur_serial = next_serial++;
    const uint64_t s = cur_serial;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        (void)b;
        uint64_t d = atomic_load(&done_serial);
        while (d < s && !atomic_compare_exchange_weak(&done_serial, &d, s)) {}
    }];
}

static void end_blit(void)
{
    if (blit) { [blit endEncoding]; blit = nil; }
}
static void end_pass(void)
{
    if (enc) { [enc endEncoding]; enc = nil; enc_has = false; enc_vbuf = NULL; }
}

static void begin_pass(void)
{
    if (enc) return;
    ensure_cb();
    end_blit();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tgt[cur];
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (clear_pending || tgt_fresh) {
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        const float *c = clear_pending ? clear_rgba : (const float[4]){ 0, 0, 0, 0 };
        rp.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
    } else rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    clear_pending = false; tgt_fresh = false;
    if (depth_on) {
        rp.depthAttachment.texture = depth_tex;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (depth_clear_pending || depth_fresh) {
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = depth_clear_pending ? depth_clear_val : 1.0;
        } else rp.depthAttachment.loadAction = MTLLoadActionLoad;
        depth_clear_pending = false; depth_fresh = false;
    }
    enc = [cb renderCommandEncoderWithDescriptor:rp];
    enc_has = false; enc_vbuf = NULL;
    st_passes++;
}

static void encode_pending(void);

static void ensure_blit(void)
{
    encode_pending();
    end_pass();
    ensure_cb();
    if (!blit) blit = [cb blitCommandEncoder];
}

/* the whole-target clears still owed become a pass of their own */
static void resolve_clears(void)
{
    if (clear_pending || depth_clear_pending || tgt_fresh) { begin_pass(); end_pass(); }
}

static void commit(bool wait)
{
    encode_pending();
    if (!cb && !clear_pending && !depth_clear_pending) return;
    resolve_clears();
    end_pass();
    end_blit();
    [cb commit];
    if (wait) [cb waitUntilCompleted];
    cb = nil;
    cur_serial = 0;
    stats_tick();
}

/* ------------------------------------------------------------------------------------------------ arena */
static void *arena_alloc(size_t n, size_t align, void **out_buf, size_t *out_off)
{
    ensure_cb();
    if (cur_chunk >= 0) {
        size_t o = (chunk_off + align - 1) & ~(align - 1);
        if (o + n <= chunks[cur_chunk].size) {
            chunk_off = o + n;
            chunks[cur_chunk].serial = cur_serial;
            *out_buf = chunks[cur_chunk].buf; *out_off = o;
            return (uint8_t *)[(__bridge id<MTLBuffer>)chunks[cur_chunk].buf contents] + o;
        }
    }
    const uint64_t done = atomic_load(&done_serial);
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != cur_chunk && chunks[i].serial <= done && chunks[i].size >= n) { pick = i; break; }
    if (pick < 0) {
        size_t sz = 8u << 20;
        while (sz < n) sz *= 2;
        id<MTLBuffer> b = [dev newBufferWithLength:sz options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        if (!b) return NULL;
        chunks = realloc(chunks, sizeof(chunk) * (size_t)(nchunks + 1));
        chunks[nchunks].buf = (__bridge_retained void *)b;
        chunks[nchunks].size = sz;
        chunks[nchunks].serial = 0;
        pick = nchunks++;
    }
    cur_chunk = pick; chunk_off = n;
    chunks[pick].serial = cur_serial;
    *out_buf = chunks[pick].buf; *out_off = 0;
    return [(__bridge id<MTLBuffer>)chunks[pick].buf contents];
}

/* ------------------------------------------------------------------------------------------------ textures */
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
    if (!t || !t->mtl) return false;
    return t->minf == GL_NEAREST || t->minf == GL_LINEAR;      /* a mipmap filter on one level: incomplete */
}
static bool tex_busy(const gtex *t) { return t->serial > atomic_load(&done_serial); }

static uint32_t samp_key(const gtex *t)
{
    const uint32_t lin = t->magf == GL_LINEAR, minl = t->minf == GL_LINEAR;
    uint32_t ws = t->ws == GL_REPEAT ? 0 : t->ws == GL_MIRRORED_REPEAT ? 2 : 1;
    uint32_t wt = t->wt == GL_REPEAT ? 0 : t->wt == GL_MIRRORED_REPEAT ? 2 : 1;
    return lin | minl << 1 | ws << 2 | wt << 4;
}
static id<MTLSamplerState> sampler_for(uint32_t k)
{
    NSNumber *key = @(k);
    id<MTLSamplerState> s = samplers[key];
    if (s) return s;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.magFilter = (k & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = (k & 2) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    const MTLSamplerAddressMode am[3] = { MTLSamplerAddressModeRepeat, MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeMirrorRepeat };
    d.sAddressMode = am[(k >> 2) & 3];
    d.tAddressMode = am[(k >> 4) & 3];
    d.normalizedCoordinates = YES;
    s = [dev newSamplerStateWithDescriptor:d];
    samplers[key] = s;
    return s;
}

static id<MTLTexture> new_rgba_tex(int w, int h, MTLTextureUsage usage, MTLStorageMode sm)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    d.usage = usage;
    d.storageMode = sm;
    return [dev newTextureWithDescriptor:d];
}

static int fmt_bpp(GLenum format)
{
    switch (format) {
    case GL_RGBA: case GL_BGRA: return 4;
    case GL_RGB: case GL_BGR: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;
    }
}
/* one row of `w` pixels in `format` to RGBA8 */
static void row_to_rgba(uint8_t *dst, const uint8_t *src, int w, GLenum format, bool force_alpha)
{
    switch (format) {
    case GL_RGBA: memcpy(dst, src, (size_t)w * 4); break;
    case GL_BGRA: for (int i = 0; i < w; i++) { dst[i*4] = src[i*4+2]; dst[i*4+1] = src[i*4+1]; dst[i*4+2] = src[i*4]; dst[i*4+3] = src[i*4+3]; } break;
    case GL_RGB:  for (int i = 0; i < w; i++) { dst[i*4] = src[i*3]; dst[i*4+1] = src[i*3+1]; dst[i*4+2] = src[i*3+2]; dst[i*4+3] = 255; } break;
    case GL_BGR:  for (int i = 0; i < w; i++) { dst[i*4] = src[i*3+2]; dst[i*4+1] = src[i*3+1]; dst[i*4+2] = src[i*3]; dst[i*4+3] = 255; } break;
    case GL_LUMINANCE: for (int i = 0; i < w; i++) { dst[i*4] = dst[i*4+1] = dst[i*4+2] = src[i]; dst[i*4+3] = 255; } break;
    case GL_LUMINANCE_ALPHA: for (int i = 0; i < w; i++) { dst[i*4] = dst[i*4+1] = dst[i*4+2] = src[i*2]; dst[i*4+3] = src[i*2+1]; } break;
    case GL_ALPHA: for (int i = 0; i < w; i++) { dst[i*4] = dst[i*4+1] = dst[i*4+2] = 0; dst[i*4+3] = src[i]; } break;
    default: memset(dst, 0, (size_t)w * 4); break;
    }
    if (force_alpha) for (int i = 0; i < w; i++) dst[i*4+3] = 255;
}

static void tex_upload(gtex *t, int x, int y, int w, int h, GLenum format, const void *pixels)
{
    if (!t || !t->mtl || !pixels || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > t->w || y + h > t->h) { gl_err = GL_INVALID_VALUE; return; }
    encode_pending();                                  /* draws issued before the upload see the old texels */
    const int bpp = fmt_bpp(format);
    const size_t rowpix = unpack_row_len > 0 ? (size_t)unpack_row_len : (size_t)w;
    const size_t a = (size_t)(unpack_align > 0 ? unpack_align : 1);
    const size_t stride = (rowpix * (size_t)bpp + a - 1) / a * a;
    const bool direct = format == GL_RGBA && !t->rgb_only;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)t->mtl;
    if (!tex_busy(t)) {
        if (direct) {
            [tex replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0
                     withBytes:pixels bytesPerRow:stride];
            st_cpuup++;
        } else {
            uint8_t *tmp = malloc((size_t)w * (size_t)h * 4);
            if (!tmp) return;
            for (int r = 0; r < h; r++) row_to_rgba(tmp + (size_t)r * w * 4, (const uint8_t *)pixels + (size_t)r * stride, w, format, t->rgb_only);
            [tex replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0
                     withBytes:tmp bytesPerRow:(NSUInteger)w * 4];
            free(tmp);
            st_cpuup++;
        }
        return;
    }
    /* in use: through the arena and a blit, in stream order */
    void *buf; size_t off;
    uint8_t *dst = arena_alloc((size_t)w * (size_t)h * 4, 256, &buf, &off);
    if (!dst) return;
    for (int r = 0; r < h; r++) {
        const uint8_t *src = (const uint8_t *)pixels + (size_t)r * stride;
        if (direct) memcpy(dst + (size_t)r * w * 4, src, (size_t)w * 4);
        else row_to_rgba(dst + (size_t)r * w * 4, src, w, format, t->rgb_only);
    }
    ensure_blit();
    [blit copyFromBuffer:(__bridge id<MTLBuffer>)buf sourceOffset:off sourceBytesPerRow:(NSUInteger)w * 4
              sourceBytesPerImage:(NSUInteger)w * (NSUInteger)h * 4 sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                        toTexture:tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)];
    t->serial = cur_serial;
    st_blitup++;
}

/* ------------------------------------------------------------------------------------------------ pipelines */
static MTLBlendFactor blend_factor(GLenum f)
{
    switch (f) {
    case GL_ZERO: return MTLBlendFactorZero;
    case GL_ONE: return MTLBlendFactorOne;
    case GL_SRC_COLOR: return MTLBlendFactorSourceColor;
    case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case GL_DST_COLOR: return MTLBlendFactorDestinationColor;
    case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    default: return MTLBlendFactorOne;
    }
}
static uint32_t blend_index(GLenum f)                  /* 4 bits */
{
    if (f == GL_ZERO) return 0;
    if (f == GL_ONE) return 1;
    if (f >= GL_SRC_COLOR && f <= GL_SRC_ALPHA_SATURATE) return 2 + (f - GL_SRC_COLOR);
    return 1;
}
static const GLenum blend_enum[11] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                       GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE };

/* key: variant 2 | blend 1 | src 4 | dst 4 | mask 4 | depth 1 */
static uint32_t pipe_key(int variant, bool blend, GLenum s, GLenum d, const bool *mask, bool depth)
{
    uint32_t m = (mask[0] ? 1u : 0) | (mask[1] ? 2u : 0) | (mask[2] ? 4u : 0) | (mask[3] ? 8u : 0);
    return (uint32_t)variant | (blend ? 1u : 0) << 2 | (blend ? blend_index(s) : 0) << 3 | (blend ? blend_index(d) : 0) << 7 | m << 11 | (depth ? 1u : 0) << 15;
}
static id<MTLRenderPipelineState> pipeline_for(uint32_t k)
{
    NSNumber *key = @(k);
    id<MTLRenderPipelineState> p = pipes[key];
    if (p) return p;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = fn_vs;
    const int variant = k & 3;
    d.fragmentFunction = variant == VAR_POST_FETCH ? fn_post_fetch : variant == VAR_POST_READ ? fn_post_read : fn_fs;
    MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
    c.pixelFormat = MTLPixelFormatRGBA8Unorm;
    if ((k >> 2) & 1) {
        c.blendingEnabled = YES;
        c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = blend_factor(blend_enum[(k >> 3) & 15]);
        c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = blend_factor(blend_enum[(k >> 7) & 15]);
        c.rgbBlendOperation = c.alphaBlendOperation = MTLBlendOperationAdd;
    }
    const uint32_t m = (k >> 11) & 15;
    c.writeMask = ((m & 1) ? MTLColorWriteMaskRed : 0) | ((m & 2) ? MTLColorWriteMaskGreen : 0) |
                  ((m & 4) ? MTLColorWriteMaskBlue : 0) | ((m & 8) ? MTLColorWriteMaskAlpha : 0);
    if ((k >> 15) & 1) d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *err = nil;
    p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) { fprintf(stderr, "[GLMETAL] pipeline %08x: %s\n", k, err.localizedDescription.UTF8String); return nil; }
    pipes[key] = p;
    return p;
}
static MTLCompareFunction cmp_func(GLenum f)
{
    switch (f) {
    case GL_NEVER: return MTLCompareFunctionNever;
    case GL_LESS: return MTLCompareFunctionLess;
    case GL_EQUAL: return MTLCompareFunctionEqual;
    case GL_LEQUAL: return MTLCompareFunctionLessEqual;
    case GL_GREATER: return MTLCompareFunctionGreater;
    case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
    case GL_GEQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}
static id<MTLDepthStencilState> dstate_for(uint32_t k)        /* bit 0 test, bit 1 write, bits 2.. GL func - GL_NEVER */
{
    NSNumber *key = @(k);
    id<MTLDepthStencilState> s = dstates[key];
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = (k & 1) ? cmp_func(GL_NEVER + (k >> 2)) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = (k & 1) && (k & 2);                 /* GL: no depth writes while the test is off */
    s = [dev newDepthStencilStateWithDescriptor:d];
    dstates[key] = s;
    return s;
}

/* ------------------------------------------------------------------------------------------------ draw state */
static void gl_rect_to_metal(const int *r, int32_t *out)         /* GL window rect (x, y bottom, w, h) to target rows */
{
    out[0] = r[0];
    out[1] = bottom_up ? r[1] : TH - (r[1] + r[3]);
    out[2] = r[2]; out[3] = r[3];
}
static void clamp_scissor(int32_t *s)
{
    int x0 = s[0], y0 = s[1], x1 = s[0] + s[2], y1 = s[1] + s[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > TW) x1 = TW;
    if (y1 > TH) y1 = TH;
    s[0] = x0; s[1] = y0; s[2] = x1 > x0 ? x1 - x0 : 0; s[3] = y1 > y0 ? y1 - y0 : 0;
}

static void build_unit(UnitU *u, int i)
{
    memset(u, 0, sizeof *u);
    const gtex *t = tex_get(S.bound[i]);
    if (!S.tex2d[i] || !tex_complete(t)) return;
    const texenv *e = &S.env[i];
    u->cfg = (simd_int4){ 1, (int)e->mode, 0, 0 };
    if (e->mode == GL_COMBINE) {
        u->cfg.z = (int)e->comb_rgb; u->cfg.w = (int)e->comb_a;
        u->src_rgb = (simd_int4){ (int)e->src_rgb[0], (int)e->src_rgb[1], (int)e->src_rgb[2], 0 };
        u->op_rgb  = (simd_int4){ (int)e->op_rgb[0],  (int)e->op_rgb[1],  (int)e->op_rgb[2], 0 };
        u->src_a   = (simd_int4){ (int)e->src_a[0],   (int)e->src_a[1],   (int)e->src_a[2], 0 };
        u->op_a    = (simd_int4){ (int)e->op_a[0],    (int)e->op_a[1],    (int)e->op_a[2], 0 };
        u->scale   = (simd_float4){ e->rgb_scale, e->a_scale, 0, 0 };
    }
    if (e->mode == GL_COMBINE || e->mode == GL_BLEND)
        u->env = (simd_float4){ e->color[0], e->color[1], e->color[2], e->color[3] };
}

/* clear: the colour quad of a scissored / masked glClear */
static void build_state(drawstate *st, int prim, bool clear)
{
    memset(st, 0, sizeof *st);
    st->prim = prim;
    const bool blend = !clear && S.blend;
    st->pipe = pipe_key(VAR_MAIN, blend, S.bsrc, S.bdst, S.cmask, depth_on);
    st->ds = clear ? 0 : ((S.depth_test ? 1u : 0) | (S.depth_mask ? 2u : 0) | (uint32_t)(S.depth_func - GL_NEVER) << 2);
    if (clear) { st->vp[0] = 0; st->vp[1] = 0; st->vp[2] = TW; st->vp[3] = TH; }
    else gl_rect_to_metal(S.vp, st->vp);
    if (S.scissor_test) { gl_rect_to_metal(S.sc, st->sc); clamp_scissor(st->sc); }
    else { st->sc[0] = 0; st->sc[1] = 0; st->sc[2] = TW; st->sc[3] = TH; }
    if (clear) {
        float ident[16]; mat_identity(ident);
        st->vsu.mvp = to_simd(ident); st->vsu.tm = to_simd(ident);
    } else {
        float m[16]; mat_mul(m, pj[pj_n], mv[mv_n]);
        st->vsu.mvp = to_simd(m);
        st->vsu.tm = to_simd(tmx[0][tm_n[0]]);
        for (int i = 0; i < 2; i++) {
            build_unit(&st->fsu.u[i], i);
            if (st->fsu.u[i].cfg.x) {
                const gtex *t = tex_get(S.bound[i]);
                st->tex[i] = t->mtl;
                st->samp[i] = samp_key(t);
            }
        }
        if (S.alpha_test) { st->fsu.misc.x = (int)S.alpha_func; st->fsu.miscf.x = fminf(fmaxf(S.alpha_ref, 0.0f), 1.0f); }
    }
    st->vsu.misc = (simd_float4){ bottom_up ? -1.0f : 1.0f, 0, 0, 0 };
}

static void apply_state(const drawstate *st)
{
    if (!enc_has || st->pipe != encst.pipe) {
        id<MTLRenderPipelineState> p = pipeline_for(st->pipe);
        if (p) [enc setRenderPipelineState:p];
    }
    if (!enc_has || st->ds != encst.ds) [enc setDepthStencilState:dstate_for(st->ds)];
    if (!enc_has || memcmp(st->vp, encst.vp, sizeof st->vp)) {
        MTLViewport v = { st->vp[0], st->vp[1], st->vp[2], st->vp[3], 0.0, 1.0 };
        [enc setViewport:v];
    }
    if (!enc_has || memcmp(st->sc, encst.sc, sizeof st->sc)) {
        MTLScissorRect r = { (NSUInteger)st->sc[0], (NSUInteger)st->sc[1], (NSUInteger)st->sc[2], (NSUInteger)st->sc[3] };
        [enc setScissorRect:r];
    }
    if (!enc_has || memcmp(&st->vsu, &encst.vsu, sizeof st->vsu)) [enc setVertexBytes:&st->vsu length:sizeof st->vsu atIndex:1];
    if (!enc_has || memcmp(&st->fsu, &encst.fsu, sizeof st->fsu)) [enc setFragmentBytes:&st->fsu length:sizeof st->fsu atIndex:0];
    for (int i = 0; i < 2; i++) {
        if (!enc_has || st->tex[i] != encst.tex[i])
            [enc setFragmentTexture:st->tex[i] ? (__bridge id<MTLTexture>)st->tex[i] : dummy_tex atIndex:(NSUInteger)i];
        if (!enc_has || st->samp[i] != encst.samp[i])
            [enc setFragmentSamplerState:sampler_for(st->samp[i]) atIndex:(NSUInteger)i];
    }
    encst = *st;
    enc_has = true;
}

static const MTLPrimitiveType prim_mtl[3] = { MTLPrimitiveTypeTriangle, MTLPrimitiveTypeLine, MTLPrimitiveTypePoint };

static void encode_pending(void)
{
    if (!pend_on || !pend_n) { pend_on = false; return; }
    begin_pass();
    apply_state(&pend);
    if (pend_buf != enc_vbuf) { [enc setVertexBuffer:(__bridge id<MTLBuffer>)pend_buf offset:pend_off atIndex:0]; enc_vbuf = pend_buf; }
    else [enc setVertexBufferOffset:pend_off atIndex:0];
    [enc drawPrimitives:prim_mtl[pend.prim] vertexStart:0 vertexCount:(NSUInteger)pend_n];
    st_draws++; st_verts += pend_n;
    pend_on = false; pend_n = 0;
}

/* textures referenced by a draw are marked when the draw is queued (their MTLTexture cannot change before it is
 * encoded: every texture change encodes the pending draw first) */
static void mark_textures(const drawstate *st)
{
    for (int i = 0; i < 2; i++) {
        if (!st->tex[i]) continue;
        gtex *t = tex_get(S.bound[i]);
        if (t && t->mtl == st->tex[i]) t->serial = cur_serial;
    }
}

static void submit_state(const drawstate *st, const Vtx *v, int n)
{
    if (n <= 0 || !dev) return;
    if (st->sc[2] <= 0 || st->sc[3] <= 0) return;           /* an empty scissor draws nothing */
    void *buf; size_t off;
    if (pend_on && memcmp(st, &pend, sizeof *st) == 0) {
        /* extend the pending draw when the arena hands back the very next bytes */
        void *b2; size_t o2;
        Vtx *dst = arena_alloc((size_t)n * sizeof(Vtx), 16, &b2, &o2);
        if (!dst) return;
        memcpy(dst, v, (size_t)n * sizeof(Vtx));
        if (b2 == pend_buf && o2 == pend_off + (size_t)pend_n * sizeof(Vtx)) { pend_n += n; mark_textures(st); return; }
        encode_pending();
        pend = *st; pend_on = true; pend_buf = b2; pend_off = o2; pend_n = n;
        mark_textures(st);
        return;
    }
    encode_pending();
    Vtx *dst = arena_alloc((size_t)n * sizeof(Vtx), 16, &buf, &off);
    if (!dst) return;
    memcpy(dst, v, (size_t)n * sizeof(Vtx));
    pend = *st; pend_on = true; pend_buf = buf; pend_off = off; pend_n = n;
    mark_textures(st);
}

static void submit(int prim, const Vtx *v, int n)
{
    if (n <= 0 || !dev) return;
    drawstate st;
    build_state(&st, prim, false);
    submit_state(&st, v, n);
}

static Vtx *tmp_vtx(int n)
{
    if (n > tmpv_cap) {
        int c = tmpv_cap ? tmpv_cap : 1024;
        while (c < n) c *= 2;
        tmpv = realloc(tmpv, sizeof(Vtx) * (size_t)c);
        tmpv_cap = c;
    }
    return tmpv;
}

/* any GL primitive list to triangles / lines / points */
static void draw_prims(GLenum mode, const Vtx *v, int n)
{
    switch (mode) {
    case GL_TRIANGLES: submit(PRIM_TRI, v, n - n % 3); break;
    case GL_LINES: submit(PRIM_LINE, v, n & ~1); break;
    case GL_POINTS: submit(PRIM_POINT, v, n); break;
    case GL_QUADS: {
        const int q = n / 4;
        Vtx *o = tmp_vtx(q * 6);
        for (int i = 0; i < q; i++) {
            const Vtx *s = v + i * 4;
            o[i*6+0] = s[0]; o[i*6+1] = s[1]; o[i*6+2] = s[2];
            o[i*6+3] = s[0]; o[i*6+4] = s[2]; o[i*6+5] = s[3];
        }
        submit(PRIM_TRI, o, q * 6);
        break;
    }
    case GL_QUAD_STRIP: {
        const int q = n >= 4 ? (n - 2) / 2 : 0;
        Vtx *o = tmp_vtx(q * 6);
        for (int i = 0; i < q; i++) {
            const Vtx *s = v + i * 2;           /* v0 v1 v3 v2 */
            o[i*6+0] = s[0]; o[i*6+1] = s[1]; o[i*6+2] = s[3];
            o[i*6+3] = s[0]; o[i*6+4] = s[3]; o[i*6+5] = s[2];
        }
        submit(PRIM_TRI, o, q * 6);
        break;
    }
    case GL_TRIANGLE_FAN: case GL_POLYGON: {
        const int t = n >= 3 ? n - 2 : 0;
        Vtx *o = tmp_vtx(t * 3);
        for (int i = 0; i < t; i++) { o[i*3] = v[0]; o[i*3+1] = v[i+1]; o[i*3+2] = v[i+2]; }
        submit(PRIM_TRI, o, t * 3);
        break;
    }
    case GL_TRIANGLE_STRIP: {
        const int t = n >= 3 ? n - 2 : 0;
        Vtx *o = tmp_vtx(t * 3);
        for (int i = 0; i < t; i++) {
            if (i & 1) { o[i*3] = v[i+1]; o[i*3+1] = v[i]; o[i*3+2] = v[i+2]; }
            else       { o[i*3] = v[i];   o[i*3+1] = v[i+1]; o[i*3+2] = v[i+2]; }
        }
        submit(PRIM_TRI, o, t * 3);
        break;
    }
    case GL_LINE_STRIP: case GL_LINE_LOOP: {
        const int segs = n < 2 ? 0 : (mode == GL_LINE_LOOP ? n : n - 1);
        Vtx *o = tmp_vtx(segs * 2);
        for (int i = 0; i < segs; i++) { o[i*2] = v[i]; o[i*2+1] = v[(i + 1) % n]; }
        submit(PRIM_LINE, o, segs * 2);
        break;
    }
    default: gl_err = GL_INVALID_ENUM; break;
    }
}

/* ------------------------------------------------------------------------------------------------ host API */
static id<MTLDevice> preset_dev;              /* glmetal_set_device: the embedding app's device (its textures must be usable there) */
void glmetal_set_device(void *mtl_device)
{
    if (dev) { if ((__bridge id<MTLDevice>)mtl_device != dev) fprintf(stderr, "[GLMETAL] glmetal_set_device after start: ignored\n"); return; }
    preset_dev = (__bridge id<MTLDevice>)mtl_device;
}

static bool init_device(void)
{
    if (dev) return true;
    dev = preset_dev ? preset_dev : MTLCreateSystemDefaultDevice();
    if (!dev) { fprintf(stderr, "[GLMETAL] no Metal device\n"); return false; }
    queue = [dev newCommandQueue];
    NSError *err = nil;
    lib = [dev newLibraryWithSource:kShaderSrc options:nil error:&err];
    if (!lib) { fprintf(stderr, "[GLMETAL] shaders: %s\n", err.localizedDescription.UTF8String); dev = nil; return false; }
    fn_vs = [lib newFunctionWithName:@"vs_main"];
    fn_fs = [lib newFunctionWithName:@"fs_main"];
    fn_post_read = [lib newFunctionWithName:@"fs_post_read"];
    const char *nf = getenv("GLMETAL_NO_FETCH");
    if ([dev supportsFamily:MTLGPUFamilyApple1] && !(nf && *nf == '1')) {
        lib_fetch = [dev newLibraryWithSource:kFetchSrc options:nil error:&err];
        if (lib_fetch) fn_post_fetch = [lib_fetch newFunctionWithName:@"fs_post_fetch"];
        if (fn_post_fetch) {                                     /* the visionOS / iOS SIMULATOR reports Apple1 but cannot read the render target: probe */
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = fn_vs; pd.fragmentFunction = fn_post_fetch;
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            if (![dev newRenderPipelineStateWithDescriptor:pd error:&err]) {
                fprintf(stderr, "[GLMETAL] no framebuffer fetch here (%s): the gamma pass ping-pongs\n", err.localizedDescription.UTF8String);
                fn_post_fetch = nil;
            }
        }
    }
    fetch_ok = fn_post_fetch != nil;
    /* top-down by default: Apple's own OpenGL breaks rasterisation ties that way (byte-identical frames) */
    const char *bu = getenv("GLMETAL_BOTTOMUP");
    bottom_up = bu && *bu == '1';
    pipes = [NSMutableDictionary new];
    dstates = [NSMutableDictionary new];
    samplers = [NSMutableDictionary new];
    dummy_tex = new_rgba_tex(1, 1, MTLTextureUsageShaderRead, MTLStorageModeShared);
    const uint32_t white = 0xFFFFFFFFu;
    [dummy_tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
    state_defaults();
    snprintf(renderer_str, sizeof renderer_str, "glmetal on %s", dev.name.UTF8String);
    fprintf(stderr, "[GLMETAL] %s; gamma pass by %s; target rows %s\n", renderer_str,
            fetch_ok ? "framebuffer fetch" : "ping-pong", bottom_up ? "bottom-up" : "top-down");
    return true;
}

bool glmetal_create(int w, int h)
{
    if (!init_device()) return false;
    if (w < 1 || h < 1) return false;
    if (tgt[0] && w == base_w && h == base_h) return true;
    commit(true);
    if (cur_set != 0) glmetal_set_target(0);
    TW = w; TH = h; cur = 0; base_w = w; base_h = h;
    for (int i = 1; i < GLMETAL_MAX_TARGETS; i++) {               /* the other targets are made again on demand (at the new size, unless their own) */
        slot[i].t[0] = slot[i].t[1] = slot[i].depth = nil; slot[i].cur = 0;
    }
    const MTLTextureUsage u = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    tgt[0] = new_rgba_tex(w, h, u, MTLStorageModePrivate);
    tgt[1] = fetch_ok ? nil : new_rgba_tex(w, h, u, MTLStorageModePrivate);
    depth_tex = nil; depth_fresh = true;
    if (depth_on) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
        depth_tex = [dev newTextureWithDescriptor:d];
    }
    tgt_fresh = true;
    S.vp[0] = 0; S.vp[1] = 0; S.vp[2] = w; S.vp[3] = h;
    S.sc[0] = 0; S.sc[1] = 0; S.sc[2] = w; S.sc[3] = h;
    return tgt[0] != nil;
}
static void make_slot_textures(tslot *t, int w, int h)
{
    const MTLTextureUsage u = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    t->t[0] = new_rgba_tex(w, h, u, MTLStorageModePrivate);
    t->t[1] = fetch_ok ? nil : new_rgba_tex(w, h, u, MTLStorageModePrivate);
    t->depth = nil;
    if (depth_on) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
        t->depth = [dev newTextureWithDescriptor:d];
    }
    t->cur = 0; t->w = w; t->h = h; t->fresh = true; t->depth_fresh = true;
}

void glmetal_set_target_size(int i, int w, int h)
{
    if (i <= 0 || i >= GLMETAL_MAX_TARGETS) return;
    if (w < 1 || h < 1) w = h = 0;                                /* back to target 0's size */
    slot[i].req_w = w; slot[i].req_h = h;
}

void glmetal_set_target(int i)
{
    if (i < 0 || i >= GLMETAL_MAX_TARGETS) i = 0;
    if (!tgt[0]) return;
    const int want_w = i == 0 ? base_w : slot[i].req_w ? slot[i].req_w : base_w;
    const int want_h = i == 0 ? base_h : slot[i].req_h ? slot[i].req_h : base_h;
    if (i == cur_set && TW == want_w && TH == want_h) return;
    encode_pending();
    resolve_clears();                                    /* whatever this target still owes goes to it, not to the next one */
    end_pass();
    tslot *o = &slot[cur_set];                           /* park the active one */
    o->t[0] = tgt[0]; o->t[1] = tgt[1]; o->cur = cur; o->fresh = tgt_fresh; o->w = TW; o->h = TH;
    o->depth = depth_tex; o->depth_fresh = depth_fresh;
    tslot *n = &slot[i];
    if (!n->t[0] || n->w != want_w || n->h != want_h) make_slot_textures(n, want_w, want_h);
    else if (depth_on && !n->depth) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:(NSUInteger)n->w height:(NSUInteger)n->h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
        n->depth = [dev newTextureWithDescriptor:d]; n->depth_fresh = true;
    }
    tgt[0] = n->t[0]; tgt[1] = n->t[1]; cur = n->cur; tgt_fresh = n->fresh; TW = n->w; TH = n->h;
    depth_tex = n->depth; depth_fresh = n->depth_fresh;
    cur_set = i;
}
int glmetal_current_target(void) { return cur_set; }
void *glmetal_target_texture_n(int i)
{
    if (i < 0 || i >= GLMETAL_MAX_TARGETS) return NULL;
    if (i == cur_set) return (__bridge void *)tgt[cur];
    return slot[i].t[0] ? (__bridge void *)slot[i].t[slot[i].cur] : NULL;
}
int glmetal_width(void) { return TW; }
int glmetal_height(void) { return TH; }
void *glmetal_target_texture(void) { return (__bridge void *)tgt[cur]; }
int glmetal_target_bottom_up(void) { return bottom_up; }
void *glmetal_device(void) { return (__bridge void *)dev; }
void *glmetal_command_queue(void) { return (__bridge void *)queue; }

void glmetal_end_frame(void (*encode)(void *cmd_buffer, void *target_texture, void *user), void *user)
{
    if (!dev) return;
    encode_pending();
    ensure_cb();
    resolve_clears();
    end_pass();
    end_blit();
    if (encode) encode((__bridge void *)cb, (__bridge void *)tgt[cur], user);
    id<MTLCommandBuffer> c = cb;
    [cb commit];
    cb = nil; cur_serial = 0;
    if (inflight[0]) [inflight[0] waitUntilCompleted];         /* at most two frames queued */
    inflight[0] = inflight[1]; inflight[1] = c;
    stats_tick();
}

static void enable_depth(void)
{
    if (depth_on) return;
    encode_pending();
    end_pass();
    depth_on = true; depth_fresh = true;
    if (TW > 0) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:(NSUInteger)TW height:(NSUInteger)TH mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
        depth_tex = [dev newTextureWithDescriptor:d];
    }
}

/* the fullscreen pair of triangles in NDC (with an identity matrix, no y negation applied twice: the vertex stage flips) */
static void push_fullscreen(void **buf, size_t *off)
{
    Vtx *v = arena_alloc(6 * sizeof(Vtx), 16, buf, off);
    const float xy[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int i = 0; i < 6; i++) {
        Vtx *p = &v[i];
        p->pos[0] = xy[i][0]; p->pos[1] = xy[i][1]; p->pos[2] = 0; p->pos[3] = 1;
        p->col[0] = p->col[1] = p->col[2] = p->col[3] = 1;
        p->tc[0] = p->tc[1] = p->tc[2] = 0; p->tc[3] = 1;
    }
}

void glmetal_post_lut(const uint8_t lut[3][256], int vw, int vh)
{
    if (!dev || !tgt[0]) return;
    encode_pending();
    uint8_t l4[256][4];
    int identity = 1;
    for (int v = 0; v < 256; v++) {
        l4[v][0] = lut[0][v]; l4[v][1] = lut[1][v]; l4[v][2] = lut[2][v]; l4[v][3] = 255;
        identity &= lut[0][v] == v && lut[1][v] == v && lut[2][v] == v;
    }
    static int said = -1;
    if (said != identity) { fprintf(stderr, "[GLMETAL] gamma table %s\n", identity ? "is identity" : "changes colours"); said = identity; }
    const int r[4] = { 0, 0, vw, vh };
    int32_t vp[4]; gl_rect_to_metal(r, vp);
    int32_t sc[4] = { vp[0], vp[1], vp[2], vp[3] }; clamp_scissor(sc);
    if (sc[2] <= 0 || sc[3] <= 0) return;
    float ident[16]; mat_identity(ident);
    VSU vsu = { to_simd(ident), to_simd(ident), (simd_float4){ bottom_up ? -1.0f : 1.0f, 0, 0, 0 } };
    void *buf; size_t off;
    if (fetch_ok) {
        begin_pass();
        push_fullscreen(&buf, &off);
        static const bool all[4] = { true, true, true, true };
        [enc setRenderPipelineState:pipeline_for(pipe_key(VAR_POST_FETCH, false, GL_ONE, GL_ZERO, all, depth_on))];
        [enc setDepthStencilState:dstate_for(0)];
    } else {
        /* ping-pong: draw the table into the other target, reading this one; outside the rectangle the old frame is copied over */
        resolve_clears();
        const int src = cur, dst = cur ^ 1;
        if (sc[0] > 0 || sc[1] > 0 || sc[2] < TW || sc[3] < TH) {
            ensure_blit();
            [blit copyFromTexture:tgt[src] toTexture:tgt[dst]];
        }
        encode_pending();
        end_pass();
        cur = dst;
        begin_pass();
        push_fullscreen(&buf, &off);
        static const bool all[4] = { true, true, true, true };
        [enc setRenderPipelineState:pipeline_for(pipe_key(VAR_POST_READ, false, GL_ONE, GL_ZERO, all, depth_on))];
        [enc setDepthStencilState:dstate_for(0)];
        [enc setFragmentTexture:tgt[src] atIndex:0];
    }
    MTLViewport v = { vp[0], vp[1], vp[2], vp[3], 0.0, 1.0 };
    [enc setViewport:v];
    MTLScissorRect s = { (NSUInteger)sc[0], (NSUInteger)sc[1], (NSUInteger)sc[2], (NSUInteger)sc[3] };
    [enc setScissorRect:s];
    [enc setVertexBytes:&vsu length:sizeof vsu atIndex:1];
    [enc setVertexBuffer:(__bridge id<MTLBuffer>)buf offset:off atIndex:0];
    [enc setFragmentBytes:l4 length:sizeof l4 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    enc_has = false; enc_vbuf = NULL;          /* the encoder's state no longer matches encst */
    if (!fetch_ok) end_pass();
}

/* ------------------------------------------------------------------------------------------------ GL: immediate mode */
static void imm_push(float x, float y, float z)
{
    if (imm_n == imm_cap) { imm_cap = imm_cap ? imm_cap * 2 : 1024; imm = realloc(imm, sizeof(Vtx) * (size_t)imm_cap); }
    Vtx *v = &imm[imm_n++];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z; v->pos[3] = 1.0f;
    memcpy(v->col, S.color, sizeof v->col);
    memcpy(v->tc, S.tc, sizeof v->tc);
}
void glBegin(GLenum mode) { in_begin = true; imm_mode = mode; imm_n = 0; }
void glEnd(void)
{
    if (!in_begin) { gl_err = GL_INVALID_OPERATION; return; }
    in_begin = false;
    draw_prims(imm_mode, imm, imm_n);
    imm_n = 0;
}
void glVertex2f(GLfloat x, GLfloat y) { if (in_begin) imm_push(x, y, 0.0f); }
void glVertex2i(GLint x, GLint y) { if (in_begin) imm_push((float)x, (float)y, 0.0f); }
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { if (in_begin) imm_push(x, y, z); }
void glTexCoord2f(GLfloat s, GLfloat t) { S.tc[0] = s; S.tc[1] = t; S.tc[2] = 0.0f; S.tc[3] = 1.0f; }
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { S.tc[0] = s; S.tc[1] = t; S.tc[2] = r; S.tc[3] = q; }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { S.color[0] = r; S.color[1] = g; S.color[2] = b; S.color[3] = 1.0f; }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.color[0] = r; S.color[1] = g; S.color[2] = b; S.color[3] = a; }
void glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }

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
static void set_array(carray *c, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)
{
    c->size = size; c->type = type; c->stride = stride; c->ptr = ptr;
}
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { set_array(&ca_v, size, type, stride, ptr); }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { set_array(&ca_c, size, type, stride, ptr); }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { if (client_active == 0) set_array(&ca_t, size, type, stride, ptr); }

static int type_size(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
    }
}
static void fetch(const carray *c, int idx, float *out, bool normalize)
{
    const int st = c->stride ? c->stride : c->size * type_size(c->type);
    const uint8_t *p = (const uint8_t *)c->ptr + (size_t)idx * (size_t)st;
    for (int k = 0; k < c->size && k < 4; k++) {
        switch (c->type) {
        case GL_FLOAT: { float f; memcpy(&f, p + k * 4, 4); out[k] = f; break; }
        case GL_DOUBLE: { double d; memcpy(&d, p + k * 8, 8); out[k] = (float)d; break; }
        case GL_UNSIGNED_BYTE: out[k] = normalize ? p[k] / 255.0f : p[k]; break;
        case GL_BYTE: out[k] = normalize ? ((int8_t)p[k]) / 127.0f : (int8_t)p[k]; break;
        case GL_SHORT: { int16_t s; memcpy(&s, p + k * 2, 2); out[k] = normalize ? s / 32767.0f : s; break; }
        case GL_UNSIGNED_SHORT: { uint16_t s; memcpy(&s, p + k * 2, 2); out[k] = normalize ? s / 65535.0f : s; break; }
        case GL_INT: { int32_t s; memcpy(&s, p + k * 4, 4); out[k] = (float)s; break; }
        default: { uint32_t s; memcpy(&s, p + k * 4, 4); out[k] = (float)s; break; }
        }
    }
}
static void gather(Vtx *v, int idx)
{
    v->pos[0] = v->pos[1] = v->pos[2] = 0.0f; v->pos[3] = 1.0f;
    if (ca_v.on && ca_v.ptr) fetch(&ca_v, idx, v->pos, false);
    if (ca_c.on && ca_c.ptr) { v->col[3] = 1.0f; fetch(&ca_c, idx, v->col, true); } else memcpy(v->col, S.color, sizeof v->col);
    if (ca_t.on && ca_t.ptr) { v->tc[0] = v->tc[1] = v->tc[2] = 0.0f; v->tc[3] = 1.0f; fetch(&ca_t, idx, v->tc, false); }
    else memcpy(v->tc, S.tc, sizeof v->tc);
}
static Vtx *gat; static int gat_cap;
static Vtx *gat_buf(int n)
{
    if (n > gat_cap) { int c = gat_cap ? gat_cap : 4096; while (c < n) c *= 2; gat = realloc(gat, sizeof(Vtx) * (size_t)c); gat_cap = c; }
    return gat;
}
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
        int idx = type == GL_UNSIGNED_SHORT ? ((const uint16_t *)indices)[i] :
                  type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] : (int)((const uint32_t *)indices)[i];
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
    default: return NULL;            /* CULL_FACE, FOG, LIGHTING, DITHER...: accepted, nothing to do */
    }
}
void glEnable(GLenum cap)
{
    bool *f = cap_flag(cap);
    if (f) *f = true;
    if (cap == GL_DEPTH_TEST && dev) enable_depth();
}
void glDisable(GLenum cap) { bool *f = cap_flag(cap); if (f) *f = false; }
GLboolean glIsEnabled(GLenum cap) { bool *f = cap_flag(cap); return f && *f; }

void glMatrixMode(GLenum mode) { S.matrix_mode = mode; }
void glLoadIdentity(void) { mat_identity(cur_matrix()); }
void glLoadMatrixf(const GLfloat *m) { memcpy(cur_matrix(), m, 16 * sizeof(float)); }
void glMultMatrixf(const GLfloat *m) { float *c = cur_matrix(); mat_mul(c, c, m); }
void glPushMatrix(void)
{
    switch (S.matrix_mode) {
    case GL_PROJECTION: if (pj_n < 31) { memcpy(pj[pj_n + 1], pj[pj_n], sizeof pj[0]); pj_n++; } break;
    case GL_TEXTURE: if (tm_n[S.active] < 7) { memcpy(tmx[S.active][tm_n[S.active] + 1], tmx[S.active][tm_n[S.active]], sizeof tmx[0][0]); tm_n[S.active]++; } break;
    default: if (mv_n < 31) { memcpy(mv[mv_n + 1], mv[mv_n], sizeof mv[0]); mv_n++; } break;
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
    m[0] = (float)(2.0 / (r - l));
    m[5] = (float)(2.0 / (t - b));
    m[10] = (float)(-2.0 / (f - n));
    m[12] = (float)(-(r + l) / (r - l));
    m[13] = (float)(-(t + b) / (t - b));
    m[14] = (float)(-(f + n) / (f - n));
    m[15] = 1.0f;
    glMultMatrixf(m);
}
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16] = { 0 };
    m[0] = (float)(2.0 * n / (r - l));
    m[5] = (float)(2.0 * n / (t - b));
    m[8] = (float)((r + l) / (r - l));
    m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n));
    m[11] = -1.0f;
    m[14] = (float)(-2.0 * f * n / (f - n));
    glMultMatrixf(m);
}
void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    float m[16]; mat_identity(m); m[12] = x; m[13] = y; m[14] = z; glMultMatrixf(m);
}
void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    float m[16]; mat_identity(m); m[0] = x; m[5] = y; m[10] = z; glMultMatrixf(m);
}
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    const float len = sqrtf(x * x + y * y + z * z);
    if (len <= 0.0f) return;
    x /= len; y /= len; z /= len;
    const float a = angle * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a), k = 1.0f - c;
    float m[16] = { x*x*k + c,   y*x*k + z*s, x*z*k - y*s, 0,
                    x*y*k - z*s, y*y*k + c,   y*z*k + x*s, 0,
                    x*z*k + y*s, y*z*k - x*s, z*z*k + c,   0,
                    0, 0, 0, 1 };
    glMultMatrixf(m);
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h; }
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    S.clear[0] = fminf(fmaxf(r, 0), 1); S.clear[1] = fminf(fmaxf(g, 0), 1); S.clear[2] = fminf(fmaxf(b, 0), 1); S.clear[3] = fminf(fmaxf(a, 0), 1);
}
void glClearDepth(GLclampd d) { S.clear_depth = (float)fmin(fmax(d, 0.0), 1.0); }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { S.cmask[0] = r; S.cmask[1] = g; S.cmask[2] = b; S.cmask[3] = a; }
void glDepthMask(GLboolean flag) { S.depth_mask = flag; }
void glDepthFunc(GLenum func) { S.depth_func = func; }
void glBlendFunc(GLenum s, GLenum d) { S.bsrc = s; S.bdst = d; }
void glAlphaFunc(GLenum func, GLclampf ref) { S.alpha_func = func; S.alpha_ref = ref; }
void glLineWidth(GLfloat w) { S.line_width = w; }   /* Metal draws 1-pixel lines only */
void glShadeModel(GLenum mode) { (void)mode; }
void glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }

void glClear(GLbitfield mask)
{
    if (!dev || !tgt[0]) return;
    encode_pending();
    const bool full = !S.scissor_test || (S.sc[0] <= 0 && S.sc[1] <= 0 && S.sc[0] + S.sc[2] >= TW && S.sc[1] + S.sc[3] >= TH);
    if ((mask & GL_DEPTH_BUFFER_BIT) && depth_on && S.depth_mask) {
        /* a scissored depth clear clears the whole depth buffer (approximation; the engine never scissors one) */
        end_pass();
        depth_clear_pending = true; depth_clear_val = S.clear_depth;
    }
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;
    if (!S.cmask[0] && !S.cmask[1] && !S.cmask[2] && !S.cmask[3]) return;
    if (full && S.cmask[0] && S.cmask[1] && S.cmask[2] && S.cmask[3]) {
        end_pass();
        clear_pending = true;
        memcpy(clear_rgba, S.clear, sizeof clear_rgba);
        return;
    }
    /* scissored or masked: a quad of the clear colour */
    drawstate st;
    build_state(&st, PRIM_TRI, true);
    Vtx v[6];
    const float xy[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int i = 0; i < 6; i++) {
        v[i].pos[0] = xy[i][0]; v[i].pos[1] = xy[i][1]; v[i].pos[2] = 0; v[i].pos[3] = 1;
        memcpy(v[i].col, S.clear, sizeof v[i].col);
        v[i].tc[0] = v[i].tc[1] = v[i].tc[2] = 0; v[i].tc[3] = 1;
    }
    submit_state(&st, v, 6);
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
    if (pname == GL_TEXTURE_ENV_COLOR) {
        for (int i = 0; i < 4; i++) S.env[S.active].color[i] = fminf(fmaxf(params[i], 0.0f), 1.0f);
    } else glTexEnvf(target, pname, params[0]);
}

void glGenTextures(GLsizei n, GLuint *out)
{
    static GLuint next = 1;
    for (int i = 0; i < n; i++) {
        GLuint name = nfree ? freenames[--nfree] : next++;
        tex_get(name);
        out[i] = name;
    }
}
void glDeleteTextures(GLsizei n, const GLuint *names)
{
    encode_pending();
    for (int i = 0; i < n; i++) {
        const GLuint name = names[i];
        if (name == 0 || (int)name >= ntexs || !texs[name].used) continue;
        gtex *t = &texs[name];
        if (t->mtl) CFRelease(t->mtl);
        memset(t, 0, sizeof *t);
        for (int u = 0; u < 2; u++) if (S.bound[u] == name) S.bound[u] = 0;
        if (nfree == capfree) { capfree = capfree ? capfree * 2 : 1024; freenames = realloc(freenames, sizeof(GLuint) * (size_t)capfree); }
        freenames[nfree++] = name;
    }
}
void glBindTexture(GLenum target, GLuint name)
{
    if (target != GL_TEXTURE_2D) return;
    if (name) tex_get(name);
    S.bound[S.active] = name;
}
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
}
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)border; (void)type;
    if (target != GL_TEXTURE_2D || level != 0 || !dev) return;     /* no mipmaps */
    gtex *t = tex_get(S.bound[S.active]);
    if (!t || w <= 0 || h <= 0) return;
    encode_pending();
    if (!t->mtl || t->w != w || t->h != h || tex_busy(t)) {     /* new storage; the old texture lives on in the GPU work that uses it */
        if (t->mtl) CFRelease(t->mtl);
        id<MTLTexture> nt = new_rgba_tex(w, h, MTLTextureUsageShaderRead, MTLStorageModeShared);
        t->mtl = nt ? (__bridge_retained void *)nt : NULL;
        t->w = w; t->h = h; t->serial = 0;
    }
    t->rgb_only = internalformat == GL_RGB || internalformat == GL_RGB8 || internalformat == 3 || internalformat == GL_LUMINANCE;
    if (pixels) tex_upload(t, 0, 0, w, h, format, pixels);
}
void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)type;
    if (target != GL_TEXTURE_2D || level != 0) return;
    tex_upload(tex_get(S.bound[S.active]), x, y, w, h, format, pixels);
}
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (target != GL_TEXTURE_2D || level != 0 || !dev || !tgt[0]) return;
    gtex *t = tex_get(S.bound[S.active]);
    if (!t || !t->mtl) return;
    /* clip the source to the target and the destination to the texture */
    if (x < 0) { xoff -= x; w += x; x = 0; }
    if (y < 0) { yoff -= y; h += y; y = 0; }
    if (x + w > TW) w = TW - x;
    if (y + h > TH) h = TH - y;
    if (xoff + w > t->w) w = t->w - xoff;
    if (yoff + h > t->h) h = t->h - yoff;
    if (w <= 0 || h <= 0 || xoff < 0 || yoff < 0) return;
    encode_pending();
    resolve_clears();
    ensure_blit();
    id<MTLTexture> dst = (__bridge id<MTLTexture>)t->mtl;
    if (bottom_up)
        [blit copyFromTexture:tgt[cur] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)
                   sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1) toTexture:dst destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake((NSUInteger)xoff, (NSUInteger)yoff, 0)];
    else
        for (int r = 0; r < h; r++)                   /* GL row y + r is target row TH-1-(y+r) */
            [blit copyFromTexture:tgt[cur] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)(TH - 1 - (y + r)), 0)
                       sourceSize:MTLSizeMake((NSUInteger)w, 1, 1) toTexture:dst destinationSlice:0 destinationLevel:0
                destinationOrigin:MTLOriginMake((NSUInteger)xoff, (NSUInteger)(yoff + r), 0)];
    t->serial = cur_serial;
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
    if (!dev || !tgt[0] || !pixels || w <= 0 || h <= 0 || type != GL_UNSIGNED_BYTE) return;
    const int bpp = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB || format == GL_BGR ? 3 : 1;
    const size_t a = (size_t)(pack_align > 0 ? pack_align : 1);
    const size_t rowpix = pack_row_len > 0 ? (size_t)pack_row_len : (size_t)w;
    const size_t stride = (rowpix * (size_t)bpp + a - 1) / a * a;
    /* the part inside the target; pixels outside it are left as they are */
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > TW ? TW : x + w, y1 = y + h > TH ? TH : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    const int cw = x1 - x0, ch = y1 - y0;
    const size_t need = (size_t)cw * (size_t)ch * 4;
    if (!readbuf || readbuf.length < need) readbuf = [dev newBufferWithLength:need options:MTLResourceStorageModeShared];
    encode_pending();
    resolve_clears();
    ensure_blit();
    const int ty = bottom_up ? y0 : TH - y1;
    [blit copyFromTexture:tgt[cur] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)x0, (NSUInteger)ty, 0)
               sourceSize:MTLSizeMake((NSUInteger)cw, (NSUInteger)ch, 1) toBuffer:readbuf destinationOffset:0
      destinationBytesPerRow:(NSUInteger)cw * 4 destinationBytesPerImage:need];
    commit(true);
    const uint8_t *src = readbuf.contents;
    for (int r = 0; r < ch; r++) {                  /* GL row y0 + r */
        const uint8_t *s = src + (size_t)(bottom_up ? r : ch - 1 - r) * (size_t)cw * 4;
        uint8_t *d = (uint8_t *)pixels + (size_t)(y0 - y + r) * stride + (size_t)(x0 - x) * (size_t)bpp;
        for (int i = 0; i < cw; i++, s += 4, d += bpp) {
            switch (format) {
            case GL_RGBA: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; break;
            case GL_BGRA: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; break;
            case GL_RGB:  d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; break;
            case GL_BGR:  d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; break;
            case GL_ALPHA: d[0] = s[3]; break;
            default: d[0] = s[0]; break;            /* GL_RED / LUMINANCE */
            }
        }
    }
}

void glGetIntegerv(GLenum pname, GLint *p)
{
    switch (pname) {
    case GL_VIEWPORT: memcpy(p, S.vp, sizeof S.vp); break;
    case GL_SCISSOR_BOX: memcpy(p, S.sc, sizeof S.sc); break;
    case GL_MAX_TEXTURE_SIZE: *p = 16384; break;
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
    case GL_VENDOR: return (const GLubyte *)"glmetal";
    case GL_RENDERER: return (const GLubyte *)(renderer_str[0] ? renderer_str : "glmetal");
    case GL_VERSION: return (const GLubyte *)"1.3 glmetal";
    case GL_EXTENSIONS: return (const GLubyte *)"GL_ARB_multitexture GL_ARB_texture_env_combine GL_EXT_bgra";
    default: return NULL;
    }
}
GLenum glGetError(void) { const GLenum e = gl_err; gl_err = GL_NO_ERROR; return e; }
void glFinish(void) { if (dev) commit(true); }
void glFlush(void) { if (dev) commit(false); }

void glPushAttrib(GLbitfield mask)
{
    if (attr_n >= 16) return;
    attr_stack[attr_n] = S; attr_mask[attr_n] = mask; attr_n++;
}
void glPopAttrib(void)
{
    if (attr_n <= 0) return;
    attr_n--;
    const srvstate *o = &attr_stack[attr_n];
    const GLbitfield m = attr_mask[attr_n];
    if (m & GL_ENABLE_BIT) {
        S.tex2d[0] = o->tex2d[0]; S.tex2d[1] = o->tex2d[1]; S.alpha_test = o->alpha_test; S.blend = o->blend;
        S.scissor_test = o->scissor_test; S.depth_test = o->depth_test;
    }
    if (m & GL_COLOR_BUFFER_BIT) {
        S.alpha_test = o->alpha_test; S.alpha_func = o->alpha_func; S.alpha_ref = o->alpha_ref;
        S.blend = o->blend; S.bsrc = o->bsrc; S.bdst = o->bdst;
        memcpy(S.cmask, o->cmask, sizeof S.cmask); memcpy(S.clear, o->clear, sizeof S.clear);
    }
    if (m & GL_DEPTH_BUFFER_BIT) { S.depth_test = o->depth_test; S.depth_func = o->depth_func; S.depth_mask = o->depth_mask; S.clear_depth = o->clear_depth; }
    if (m & GL_VIEWPORT_BIT) memcpy(S.vp, o->vp, sizeof S.vp);
    if (m & GL_SCISSOR_BIT) { S.scissor_test = o->scissor_test; memcpy(S.sc, o->sc, sizeof S.sc); }
    if (m & GL_TEXTURE_BIT) {
        memcpy(S.env, o->env, sizeof S.env); memcpy(S.bound, o->bound, sizeof S.bound); S.active = o->active;
        S.tex2d[0] = o->tex2d[0]; S.tex2d[1] = o->tex2d[1];
    }
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
void *glmetal_GetProcAddress(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof procs / sizeof procs[0]; i++)
        if (!strcmp(procs[i].name, name)) return procs[i].fn;
    return NULL;                       /* shaders, programs, framebuffer objects: not provided */
}
