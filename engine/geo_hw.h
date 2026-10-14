/*
 * geo_hw.h — the System 22 / Super 22 geometry stage (see engine/geo_hw.c).
 *
 * Screen coordinates come out in 1/16 pixel, exactly as the hardware and
 * the oracle (pc_geo_fixed.py) produce them. Divide by 16.0 for float.
 */
#ifndef GEO_HW_H
#define GEO_HW_H

#include <stdint.h>

typedef struct {
    int32_t  sx16, sy16;   /* screen position, 1/16 pixel */
    int32_t  z;            /* view-space depth */
    uint32_t u, v;         /* 12-bit texture coords */
    int      bri;          /* per-vertex brightness */
    int      valid;        /* 0 = behind the eye, needs z-clip */
    /* Same u/v/bri carried at UF=16 fraction bits, i.e. BEFORE the >>UF
     * truncation. The oracle's golden stream compares these, so keeping
     * them is what lets tools/frame_gate.py check a whole frame rather
     * than one model. */
    int32_t  uf, vf, bf;
} geo_vert;

typedef struct {
    /* The oracle keeps BOTH, and so must this: `v` is the raw per-vertex
     * projection (its scr16, valid=0 when behind the eye) and is what the
     * gate compares; `rv` is the near-plane-clipped POLYGON that actually
     * gets drawn (3-6 verts). Collapsing them into one broke the gate. */
    geo_vert v[4];
    geo_vert rv[10];
    int      nrv;
    /* DRAW polygon, only when the clipped polygon reaches past the screen
     * guard band: rv clipped in 3D against the four guard planes as well, so
     * no coordinate saturates. `rv` stays the oracle-exact near-clipped polygon
     * the gates compare; the live rasteriser swaps dv in (register row 187). */
    geo_vert dv[10];
    int      ndv;
    uint32_t color;      /* PALETTE SELECTOR: (color>>8)&0x7F picks a
                          * 256-entry group. Not RGB -- painting it as
                          * RGB is what made every quad flat olive. */
    int      texbank, cmode;
    int      cz_type;    /* (flags >> 10) & 3 -- selects the czram bank for
                          * depth fog via czattr[6]. Needed by the fog stage;
                          * see FogState.quad_fog in pc_raster_model.py. */
    int32_t  zsort;
    int32_t  cz_adjust;    /* bit 23 set = fog OFF for this quad
                            * (pc_raster_model.py: BIT(cz_adjust,23)) */
    int32_t  flags_raw;    /* packet flags & 0xffffff (golden stream field) */
    int32_t  cz_value;     /* min(0x1fffff, zmax) >> 8  (golden stream field) */
    int      behind;       /* at least one vertex behind the eye */
    int      order;        /* emission index; tiebreak for the stable sort */
    int      pick_code;    /* the object this quad came from, for the picker.
                            * Not part of the hardware model and not compared
                            * by any gate -- it rides along so the sort keeps
                            * quad and owner together. */
    /* Scene clip window in screen pixels (l, r, t, b), from the viewport
     * record. The rasteriser rejects everything outside it; without it,
     * geometry the hardware never shows gets drawn. */
    int32_t  clip[4];
    /* The quad's WHOLE UV box (min_u, max_u, min_v, max_v) from its four ROM
     * UVs, whether or not a vertex is behind the eye. Not part of the hardware
     * model and not compared by any gate (like pick_code). The live
     * rasteriser keys its baked texture on this rather than on the clipped
     * polygon, whose interpolated UVs move every frame and made every
     * near-clipped quad a new texture every frame. */
    uint16_t uvbox[4];
    /* System 22 object flags from the 0x10 (233002) record, bits 21-23 of its
     * third word: non-zero draws the quad UNTEXTURED in one palette pen
     * (namcos22_v.cpp poly3d_drawquad). Super 22 never sets them. */
    int      objectflags;
    int      direct;       /* a quad the master sent straight to the renderer */
    /* STEREO (tc-visionos PLAN.md, level 2a). Not part of the hardware model and not compared by any gate: the zoom the quad
     * was projected with, which eye rv/dv's sx16 currently hold (0 = the mono projection, as emitted), and the mono sx16
     * saved the first time geo_hw_quad_eye moves them, so any eye -- 0 included -- is recomputed from the original. */
    int32_t  zoom_mant;
    int      zoom_shift;
    int      eye;
    int32_t  mono_rx[10], mono_dx[10];
    /* PANORAMA (tc-visionos PLAN.md). Not part of the hardware model and not compared by any gate: what the projection was made
     * from -- the four view-space vertices, their 12-bit UVs and brightness, the projection centre, and the zsort inputs -- so
     * geo_hw_quad_view can project the same quad again for a view turned about the vertical axis, after the walk. */
    int32_t  wx[4], wy[4], wz[4];
    uint16_t wu[4], wv[4];
    int32_t  wb[4];
    int32_t  pcx, pcy;
    int32_t  zs_flags, zs_poly, zs_obj, zs_ap;
} geo_quad;

typedef struct {
    int32_t m[3][3];       /* Q15 rotation: view * object */
    int32_t t[3];          /* translation, view space */
    int32_t zoom_mant;     /* focal length mantissa (1920 = the game's 0x780) */
    int     zoom_shift;    /* right shift paired with the mantissa */
    int32_t vx, vy;        /* viewport centre offset from (320,240) */
    float   cl, cr, cu, cd; /* scene clip window: dspfloat(src[7..10])*zoom-0.5 */
    int     have_clip;
    int32_t objectshift;
    int32_t cz_adjust;
    int32_t absolute_priority;
    int     cullflip;
    /* Lighting (GeoFixed.register_normals_fixed). The normals dot the
     * light against the VIEW matrix, not the combined object*view one in
     * m[][], so the view has to be carried separately. */
    int32_t viewq[3][3];   /* view matrix alone, Q15, reflection applied */
    int32_t light[3];      /* light vector, Q15 */
    int32_t ambient, power;
    int     objectflags;   /* System 22 only, see geo_quad */
} geo_view;

typedef struct {
    int emitted, part_clip;
    int rej_behind, rej_cull, rej_pktlen, rej_chunk;
    int32_t zmin_seen, zmax_seen;
} geo_stats;
extern geo_stats g_geo_stats;
extern unsigned  g_eng_frame;   /* set by the game each frame; diagnostics only */
extern int       g_bbox_cur;    /* object code being walked, -1 = none */

typedef void (*geo_quad_cb)(const geo_quad *q, void *user);

void geo_hw_set_view(const geo_view *v);
void geo_hw_object(int32_t code, geo_quad_cb cb, void *user);
void geo_hw_zoom_from_dspfloat(uint32_t word, int32_t *mant, int *shift);

/* STEREO 3D (off-axis, tc-visionos PLAN.md level 2a). `sep` is the eye separation in view-space units, `zc` the convergence
 * depth (view-space z drawn at zero parallax), `max_crossed` the largest crossed (in front of the screen) disparity, as a
 * fraction of the 640 px picture width; nearer polygons keep that disparity. sep <= 0 turns stereo off. Eye e (-1 left,
 * +1 right) sees screen_x = c + (X - e*sep/2)*K/Z + e*(sep/2)*K/zc (Y unchanged) -- applied per vertex to an emitted quad
 * by geo_hw_quad_eye, from its depth: x += e*(sep/2)*K*(1/zc - 1/Z). The walk itself stays mono and byte-identical. */
void geo_hw_set_stereo(float sep, float zc, float max_crossed);
int  geo_hw_stereo_on(void);
/* Move q's drawn screen x (rv, and dv when present) to eye `eye`'s projection; eye 0 restores the mono numbers exactly. */
void geo_hw_quad_eye(geo_quad *q, int eye);

/* PANORAMA (tc-visionos PLAN.md): a view turned by `yaw` radians about the vertical axis through the eye (> 0 = to the right).
 * `dst` becomes `src` projected for that view: its view-space vertices rotated (x' = x cos - z sin, z' = x sin + z cos), then the
 * same zsort, near clip, projection and guard band as the walk's, from the rotated depths. Back-face culling needs no redo: the
 * walk's test is a triple product of the vertices, which a rotation about the eye does not change. The lighting does not change
 * either (normals and light turn together). Returns 0 when the quad is wholly behind the turned eye (dst is then unusable).
 * dst comes out mono (eye 0); geo_hw_quad_eye moves it to an eye like any walked quad. The walk itself never calls this, so the
 * centre view stays byte-identical. A view at yaw = +-geo_hw_hfov(...) shows the world exactly beside the centre's 4:3 picture. */
int   geo_hw_quad_view(geo_quad *dst, const geo_quad *src, double yaw);
/* The horizontal field of view, in radians, of a quad's 640 px picture: 2 atan(320 / K), K = zoom_mant / 2^zoom_shift pixels. */
double geo_hw_hfov(int32_t zoom_mant, int zoom_shift);

#endif /* GEO_HW_H */
