/*
 * tc_core.m -- Time Crisis as a library for the visionOS shell (tc_core.h): the "external host" of the shared Super 22 engine.
 *
 * The engine's own host (engine/ss22_host.c: an SDL window, its GL context, events, menu, vsync) and its input module
 * (engine/ss22_input.c: SDL keyboard, mouse and pads) are NOT compiled into this build (ENG_EXTERNAL_HOST); this file implements
 * their interfaces (ss22_host.h, ss22_input.h) instead:
 *   - the game thread runs the unchanged scheduler (engine/ss22_run.c's ss22_main via tc_game.c's main, built as tc_main) as a
 *     "--window" session; every emulated frame ends in ss22_host_frame() here: the shell's input -> the cabinet, ss22_draw into
 *     glmetal's offscreen target, glmetal_end_frame with a hook that blits the target into one of three output textures and
 *     publishes it when the GPU has completed it, then the pacing to the board's 59.906 Hz (mach_wait_until);
 *   - the shell's setters (any thread) write atomics; the game thread reads them once per frame (ss22_input_update);
 *   - the gun's recoil solenoid (engine/ss22_out.c calls ss22_input_rumble on each rising edge) becomes TC_EVENT_SHOT;
 *   - sound: engine/audio_out.c's CoreAudio backend (ENG_AUDIO_COREAUDIO) pulls the same ring the SDL callback did -- or, with
 *     tc_core_set_external_audio, the app pulls it (tc_core_audio_pull / _channel) and plays it from a RealityKit entity.
 * tc_core_stop parks the game thread inside ss22_host_frame (audio closed, EEPROM saved); tc_core_start resumes it.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <os/lock.h>
#include "tc_core.h"
#include "ss22_game.h"
#include "ss22_host.h"
#include "ss22_gl.h"
#include "eng_gl.h"
#include "audio_out.h"
#include "tex_bake.h"
#include "lift_cpu.h"
#include "geo_hw.h"

#ifndef ENG_EXTERNAL_HOST
#error "tc_core.m is the ENG_EXTERNAL_HOST host: build it with -DENG_EXTERNAL_HOST (timecris/CMakeLists.txt, target tc_core)"
#endif

int tc_main(int argc, char **argv);                          /* src/tc_game.c's main, renamed for this build */

/* ------------------------------------------------------------------------------------------------ shared state */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static pthread_t       game_thread;
static bool            thread_made;
static int             start_state;                         /* 0 starting, 1 running, -1 failed (guarded by mu) */
static bool            want_pause, parked;                   /* guarded by mu */

static id<MTLDevice>   app_dev;
static char           *rom_path;
static int             W = 640, H = 480;

static _Atomic float   gun_u = 0.5f, gun_v = 0.5f;
static atomic_bool     gun_on, trig_down, pedal_down;
static atomic_uint     trig_presses, coins_pending, starts_pending;
static _Atomic(tc_core_event_fn) ev_fn;
static _Atomic(void *) ev_user;
static _Atomic double  fps_now;

/* frames: three output textures (and three for the right eye, made when stereo is first on); `published` =
 * (frame id << 4) | (panorama << 3) | (stereo << 2) | slot of the newest one the GPU has finished */
static id<MTLTexture>  outs[3], outs_r[3];
static uint64_t        frame_seq;
static _Atomic uint64_t published;
static _Atomic float   st_sep, st_zc;                /* tc_core_set_stereo: sep <= 0 = mono */
/* PANORAMA (tc_core_set_panorama): the side views' output textures, [slot][side: 0 left, 1 right][eye: 0 left or mono, 1 right],
 * made at the first panorama frame and again if the side size changes (the previous set is kept one more generation, so a texture
 * the app still holds stays alive); each slot's yaw step (the view's horizontal FOV). glmetal targets: 0/1 the centre's eyes,
 * 2/3 the left/right side (left eye or mono), 4/5 the same for the right eye. */
static id<MTLTexture>  outs_side[3][2][2], outs_side_old[3][2][2];
static int             side_tw, side_th;
static float           slot_fov[3];
static atomic_int      pano_n, pano_w, pano_h;
static _Atomic float   fov_now = 0.785398163f;

static void emit(tc_core_event e, int32_t v)
{
    void *u = atomic_load(&ev_user);
    tc_core_event_fn f = atomic_load(&ev_fn);
    if (f) f(u, e, v);
}

/* ------------------------------------------------------------------------------------------------ the public API */
static void *game_main(void *arg)
{
    (void)arg;
    pthread_setname_np("tc.core.game");
    char *argv[] = { "tc", rom_path, "--window", NULL };
    const int rc = tc_main(3, argv);                       /* returns only if the game could not start (entry_reset never returns) */
    fprintf(stderr, "[TC_CORE] the game stopped (%d)\n", rc);
    pthread_mutex_lock(&mu);
    start_state = -1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* Application Support/TimeCrisis: the engine writes its EEPROM image (and any settings) to relative paths */
static void enter_data_dir(void)
{
    @autoreleasepool {
        NSArray<NSString *> *d = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
        NSString *dir = [(d.firstObject ?: NSTemporaryDirectory()) stringByAppendingPathComponent:@"TimeCrisis"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        if (chdir(dir.fileSystemRepresentation) != 0) fprintf(stderr, "[TC_CORE] cannot enter %s\n", dir.fileSystemRepresentation);
        else fprintf(stderr, "[TC_CORE] data folder %s\n", dir.fileSystemRepresentation);
    }
}

int tc_core_start(const char *rom_dir, void *mtl_device, int width, int height)
{
    pthread_mutex_lock(&mu);
    if (thread_made) {                                     /* after tc_core_stop: resume the same game */
        const int st = start_state;
        want_pause = false;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
        return st == 1 ? 0 : -1;
    }
    if (!rom_dir || !mtl_device) { pthread_mutex_unlock(&mu); return -2; }
    app_dev = (__bridge id<MTLDevice>)mtl_device;
    rom_path = strdup(rom_dir);
    if (width >= 320 && height >= 240 && width <= 4096 && height <= 4096) { W = width; H = height; }
    enter_data_dir();
    setenv("SS22_OUT_PORT", "0", 1);                       /* engine/ss22_out.c: no MAME network-output socket in the app */
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);              /* the lifted 68K program recurses as the original did: a main-thread-sized stack */
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    start_state = 0;
    if (pthread_create(&game_thread, &a, game_main, NULL) != 0) { pthread_attr_destroy(&a); pthread_mutex_unlock(&mu); return -3; }
    pthread_attr_destroy(&a);
    thread_made = true;
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 20;   /* ROMs, DSP, sound, video: well under a second normally */
    while (start_state == 0) if (pthread_cond_timedwait(&cv, &mu, &dl) != 0) break;
    const int st = start_state;
    pthread_mutex_unlock(&mu);
    return st == 1 ? 0 : -1;
}

void tc_core_stop(void)
{
    pthread_mutex_lock(&mu);
    if (thread_made && start_state == 1) {
        want_pause = true;
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 1;
        while (!parked) if (pthread_cond_timedwait(&cv, &mu, &dl) != 0) break;
    }
    pthread_mutex_unlock(&mu);
}

void *tc_core_latest_frame(uint64_t *frame_id)
{
    const uint64_t v = atomic_load(&published);
    if (!v) return NULL;
    if (frame_id) *frame_id = v >> 4;
    return (__bridge void *)outs[v & 3];
}

void *tc_core_latest_frame_stereo(uint64_t *frame_id, void **right)
{
    const uint64_t v = atomic_load(&published);
    if (right) *right = NULL;
    if (!v) return NULL;
    if (frame_id) *frame_id = v >> 4;
    if (right) *right = (__bridge void *)((v & 4) ? outs_r[v & 3] : outs[v & 3]);
    return (__bridge void *)outs[v & 3];
}

void tc_core_set_panorama(int side_views_each_side, int side_width, int side_height)
{
    int n = side_views_each_side;
    if (n < 0) n = 0;
    if (n > TC_PANORAMA_MAX_SIDE_VIEWS) n = TC_PANORAMA_MAX_SIDE_VIEWS;
    atomic_store(&pano_w, side_width > 0 ? side_width : 0);
    atomic_store(&pano_h, side_height > 0 ? side_height : 0);
    atomic_store(&pano_n, n);
}

void *tc_core_latest_frame_side(int index, uint64_t *frame_id, void **right)
{
    const uint64_t v = atomic_load(&published);
    if (right) *right = NULL;
    if (!v || !(v & 8) || (index != -1 && index != 1)) return NULL;
    if (frame_id) *frame_id = v >> 4;
    const unsigned slot = (unsigned)(v & 3), side = index < 0 ? 0 : 1;
    id<MTLTexture> l = outs_side[slot][side][0];
    if (right) *right = (__bridge void *)((v & 4) ? outs_side[slot][side][1] : l);
    return (__bridge void *)l;
}

float tc_core_view_fov(void)
{
    const uint64_t v = atomic_load(&published);
    if (v && (v & 8)) return slot_fov[v & 3];
    return atomic_load(&fov_now);
}

void tc_core_set_stereo(float sep, float zc)
{
    if (!(sep > 0.0f) || !(zc > 0.0f)) sep = 0.0f;
    atomic_store(&st_zc, zc);
    atomic_store(&st_sep, sep);
}

void tc_core_set_gun(float u, float v, bool on_screen)
{
    atomic_store(&gun_u, u); atomic_store(&gun_v, v); atomic_store(&gun_on, on_screen);
}
void tc_core_set_trigger(bool down)
{
    const bool was = atomic_exchange(&trig_down, down);
    if (down && !was) atomic_fetch_add(&trig_presses, 1);
}
void tc_core_set_pedal(bool down) { atomic_store(&pedal_down, down); }
void tc_core_insert_coin(void) { atomic_fetch_add(&coins_pending, 1); }
void tc_core_press_start(void) { atomic_fetch_add(&starts_pending, 1); }
void tc_core_set_event_callback(tc_core_event_fn fn, void *user)
{
    atomic_store(&ev_fn, NULL);
    atomic_store(&ev_user, user);
    atomic_store(&ev_fn, fn);
}
double tc_core_fps(void) { return atomic_load(&fps_now); }

/* ------------------------------------------------------------------------------------------------ external audio */
static atomic_ullong audio_pulled;                     /* frames the app has pulled (the stats line shows the rate: ~48000/s) */
static unsigned long long pulled_seen;                  /* audio_pulled at the last stats line (or the resume) */

void tc_core_set_external_audio(bool on) { eng_audio_set_external(on); }

bool tc_core_audio_pull(float *left, float *right, int frames)
{
    if (frames <= 0) return false;
    atomic_fetch_add_explicit(&audio_pulled, (unsigned long long)frames, memory_order_relaxed);
    return eng_audio_pull(left, right, frames);
}

/* The splitter for two per-channel consumers: the stereo frames pulled so far live in sp[][] (frame k at k & (SPLIT-1));
 * consumed[c] is how far channel c has read. A channel that needs more pulls a block of stereo for both; the other channel's
 * half waits there for its own callback. If one channel stops asking, the other drops its unread frames rather than block. */
#define SPLIT 8192                                      /* frames, power of two (170 ms) */
#define SPLIT_BLOCK 256
static os_unfair_lock sp_lock = OS_UNFAIR_LOCK_INIT;
static float sp[2][SPLIT];
static uint64_t sp_produced, sp_consumed[2];
static bool sp_live;

bool tc_core_audio_pull_channel(int channel, float *out, int frames)
{
    if (frames <= 0 || (channel != 0 && channel != 1)) return false;
    const int me = channel, other = 1 - channel;
    os_unfair_lock_lock(&sp_lock);
    /* this channel far behind the other (it was stopped, or started late): realign it to the newest frames */
    const uint64_t lag = sp_produced - sp_consumed[me];
    if (lag > (uint64_t)frames + 2048) sp_consumed[me] = sp_produced - (uint64_t)(frames < SPLIT ? frames : SPLIT);
    while (frames > 0) {
        const int n = frames > SPLIT_BLOCK ? SPLIT_BLOCK : frames;
        while (sp_produced - sp_consumed[me] < (uint64_t)n) {
            float l[SPLIT_BLOCK], r[SPLIT_BLOCK];
            sp_live = tc_core_audio_pull(l, r, SPLIT_BLOCK);
            if (sp_produced + SPLIT_BLOCK - sp_consumed[other] > SPLIT) sp_consumed[other] = sp_produced + SPLIT_BLOCK - SPLIT;
            for (int i = 0; i < SPLIT_BLOCK; i++) {
                const unsigned k = (unsigned)((sp_produced + (uint64_t)i) & (SPLIT - 1));
                sp[0][k] = l[i]; sp[1][k] = r[i];
            }
            sp_produced += SPLIT_BLOCK;
        }
        for (int i = 0; i < n; i++) out[i] = sp[me][(unsigned)((sp_consumed[me] + (uint64_t)i) & (SPLIT - 1))];
        sp_consumed[me] += (uint64_t)n;
        out += n; frames -= n;
    }
    const bool live = sp_live;
    os_unfair_lock_unlock(&sp_lock);
    return live;
}

/* ------------------------------------------------------------------------------------------------ the cabinet (ss22_input.h) */
static const ss22_input_game *ig;
static uint16_t bit_trigger, bit_pedal, bit_coin;

static uint16_t action_bit(const char *key)
{
    for (int i = 0; ig && i < ig->n; i++) if (ig->actions[i].key && !strcmp(ig->actions[i].key, key)) return ig->actions[i].bit;
    return 0;
}

void ss22_input_init(const ss22_input_game *g)
{
    ig = g;
    bit_trigger = action_bit("trigger"); bit_pedal = action_bit("pedal"); bit_coin = action_bit("coin");
    if (!bit_trigger || !bit_pedal || !bit_coin) fprintf(stderr, "[TC_CORE] the game's table lacks trigger/pedal/coin\n");
}
const eng_ui_page *ss22_input_page(void) { return NULL; }
void ss22_input_event(const SDL_Event *e) { (void)e; }
void ss22_input_motor(uint8_t b) { (void)b; }
void ss22_input_kick(void) {}
void ss22_input_kick_frame(void) {}
void ss22_input_close(void) {}
bool ss22_input_aim(float *nx, float *ny)
{
    if (!atomic_load(&gun_on)) return false;
    *nx = atomic_load(&gun_u); *ny = atomic_load(&gun_v);
    return true;
}
static unsigned shots;
void ss22_input_rumble(uint16_t low, uint16_t high, uint32_t ms)    /* engine/ss22_out.c: the recoil solenoid's rising edge */
{
    (void)low; (void)high; (void)ms;
    shots++;
    emit(TC_EVENT_SHOT, 1);
}

static unsigned wheel_centre(void) { return ig ? (unsigned)((ig->wheel_min + ig->wheel_max) / 2) : 0x200; }

void ss22_input_neutral(void)
{
    if (ig) ig->send(0, wheel_centre(), 0, 0);
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

void ss22_input_update(void)
{
    if (!ig) return;
    static unsigned seen_presses, seen_starts, seen_coins;
    static int trig_hold, start_hold, coin_frames;
    uint16_t p = 0;

    const unsigned pr = atomic_load(&trig_presses);
    if (pr != seen_presses) { seen_presses = pr; trig_hold = 3; }           /* a pinch shorter than a frame still fires */
    const unsigned sp = atomic_load(&starts_pending);
    if (sp != seen_starts && start_hold == 0) { seen_starts++; start_hold = 6; }
    const unsigned cp = atomic_load(&coins_pending);
    if (cp != seen_coins && coin_frames == 0) { seen_coins++; coin_frames = 12; }     /* 6 frames on, 6 off: each coin is its own edge */

    if (atomic_load(&trig_down) || trig_hold > 0 || start_hold > 0) p |= bit_trigger;
    if (atomic_load(&pedal_down)) p |= bit_pedal;
    if (coin_frames > 6) p |= bit_coin;
    if (trig_hold > 0) trig_hold--;
    if (start_hold > 0) start_hold--;
    if (coin_frames > 0) coin_frames--;

    const bool on = atomic_load(&gun_on);
    g_ss22_gun_off = !on;
    g_ss22_gun_x = (uint16_t)(68 + clamp01(atomic_load(&gun_u)) * 626 + 0.5f);   /* the cabinet's port ranges (engine/ss22_board.h) */
    g_ss22_gun_y = (uint16_t)(43 + clamp01(atomic_load(&gun_v)) * 241 + 0.5f);
    ig->send(p, wheel_centre(), 0, 0);
}

/* ------------------------------------------------------------------------------------------------ the host (ss22_host.h) */
static const ss22_host_game *hg;
static bool host_open, headless_open, audio_on;
static uint64_t period_mt, next_mt;                           /* the frame period and the next deadline, in mach_absolute_time units */
static mach_timebase_info_data_t tb;
static double mt_ms(uint64_t d) { return (double)d * tb.numer / tb.denom * 1e-6; }

bool ss22_host_active(void) { return host_open; }
bool ss22_host_restart_requested(void) { return false; }
bool ss22_host_pointer(float *nx, float *ny, bool *inside) { (void)nx; (void)ny; (void)inside; return false; }

static void signal_started(int st)
{
    pthread_mutex_lock(&mu);
    start_state = st;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

bool ss22_host_open(const ss22_host_game *g, int scale, bool fs)
{
    (void)scale; (void)fs;
    hg = g;
    glmetal_set_device((__bridge void *)app_dev);
    if (!glmetal_create(W, H)) { fprintf(stderr, "[TC_CORE] glmetal: no Metal\n"); signal_started(-1); return false; }
    id<MTLDevice> d = (__bridge id<MTLDevice>)glmetal_device();
    id<MTLTexture> t = (__bridge id<MTLTexture>)glmetal_target_texture();
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:(NSUInteger)W height:(NSUInteger)H mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    for (int i = 0; i < 3; i++) { outs[i] = [d newTextureWithDescriptor:td]; outs[i].label = [NSString stringWithFormat:@"tc_core frame %d", i]; }
    fprintf(stderr, "[TC_CORE] %dx%d frames on %s (%s), triple buffered\n", W, H, d.name.UTF8String,
            t.pixelFormat == MTLPixelFormatRGBA8Unorm ? "RGBA8Unorm" : "other format");
    g->input_init();
    tex_bake_window_defaults();                               /* a per-frame budget for cold texture bakes, as a window has */
    eng_audio_set_gain(g->out_gain);
    audio_on = eng_audio_open();
    if (audio_on) g->snd_set_output(true);
    mach_timebase_info(&tb);
    period_mt = (uint64_t)(1e9 / 59.906 * tb.denom / tb.numer);
    next_mt = 0;
    host_open = true;
    signal_started(1);
    return true;
}

bool ss22_host_open_headless(void)
{
    glmetal_set_device((__bridge void *)app_dev);
    headless_open = eng_gl_open_headless(W, H);
    return headless_open;
}

void ss22_host_shot(const char *path)
{
    ss22_draw(W, H);
    if (eng_gl_write_ppm(path, W, H)) fprintf(stderr, "[HOST] saved %s\n", path);
}

/* user: bit 0 a stereo frame -- the left eye is glmetal target 0 (the `target` passed here), the right eye target 1; bit 1 a panorama
 * frame -- the side views are targets 2 (left) and 3 (right), and with stereo 4 and 5 for the right eye */
static void publish_hook(void *cmd_buffer, void *target, void *user)
{
    const bool stereo = ((uintptr_t)user & 1) != 0, pano = ((uintptr_t)user & 2) != 0;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmd_buffer;
    id<MTLTexture> src = (__bridge id<MTLTexture>)(stereo ? glmetal_target_texture_n(0) : target);
    const uint64_t fid = ++frame_seq;
    const unsigned slot = (unsigned)(fid % 3);
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake((NSUInteger)W, (NSUInteger)H, 1)
             toTexture:outs[slot] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    if (stereo) {
        id<MTLTexture> rs = (__bridge id<MTLTexture>)glmetal_target_texture_n(1);
        [b copyFromTexture:rs sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake((NSUInteger)W, (NSUInteger)H, 1)
                 toTexture:outs_r[slot] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    if (pano)
        for (int side = 0; side < 2; side++)
            for (int eye = 0; eye < (stereo ? 2 : 1); eye++) {
                id<MTLTexture> ss = (__bridge id<MTLTexture>)glmetal_target_texture_n(2 + side + 2 * eye);
                [b copyFromTexture:ss sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake((NSUInteger)side_tw, (NSUInteger)side_th, 1)
                         toTexture:outs_side[slot][side][eye] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            }
    [b endEncoding];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        (void)c;
        const uint64_t v = fid << 4 | (pano ? 8u : 0u) | (stereo ? 4u : 0u) | slot;
        uint64_t cur = atomic_load(&published);
        while ((cur >> 4) < fid && !atomic_compare_exchange_weak(&published, &cur, v)) {}
    }];
}

/* the right eye's three output textures, like outs[] (on the game thread, the first time stereo is on) */
static bool make_right_outs(void)
{
    if (outs_r[0]) return true;
    id<MTLTexture> t = (__bridge id<MTLTexture>)glmetal_target_texture();
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:(NSUInteger)W height:(NSUInteger)H mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLDevice> d = (__bridge id<MTLDevice>)glmetal_device();
    for (int i = 0; i < 3; i++) { outs_r[i] = [d newTextureWithDescriptor:td]; outs_r[i].label = [NSString stringWithFormat:@"tc_core right frame %d", i]; }
    return outs_r[0] && outs_r[1] && outs_r[2];
}

/* the side views' output textures at w x h (on the game thread, at the first panorama frame and when the size changes) */
static bool make_side_outs(int w, int h, bool stereo)
{
    if (outs_side[0][0][0] && side_tw == w && side_th == h && (!stereo || outs_side[0][0][1])) return true;
    id<MTLTexture> t = (__bridge id<MTLTexture>)glmetal_target_texture();
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLDevice> d = (__bridge id<MTLDevice>)glmetal_device();
    const bool resize = outs_side[0][0][0] && (side_tw != w || side_th != h);
    for (int i = 0; i < 3; i++)
        for (int s = 0; s < 2; s++)
            for (int e = 0; e < 2; e++) {
                if (resize) outs_side_old[i][s][e] = outs_side[i][s][e];
                else if (outs_side[i][s][e]) continue;                    /* same size: only the eyes not made yet */
                if (e == 1 && !stereo) { if (resize) outs_side[i][s][e] = nil; continue; }
                outs_side[i][s][e] = [d newTextureWithDescriptor:td];
                outs_side[i][s][e].label = [NSString stringWithFormat:@"tc_core side %c%s frame %d", s ? 'R' : 'L', e ? " right eye" : "", i];
                if (!outs_side[i][s][e]) return false;
            }
    side_tw = w; side_th = h;
    fprintf(stderr, "[TC_CORE] panorama side views %dx%d\n", w, h);
    return true;
}

/* ENG-style statistics: every 300 frames, the rate and where a frame's time goes */
static uint64_t st_t0, st_work, st_work_max, st_draw, st_draw_max, st_last_end;
static unsigned st_n, st_late;

static void park_if_asked(void)
{
    pthread_mutex_lock(&mu);
    if (!want_pause) { pthread_mutex_unlock(&mu); return; }
    pthread_mutex_unlock(&mu);
    if (audio_on) { hg->snd_set_output(false); eng_audio_close(); audio_on = false; }
    ss22_eeprom_save();
    fprintf(stderr, "[TC_CORE] paused at frame %u\n", rr_frame);
    pthread_mutex_lock(&mu);
    parked = true;
    pthread_cond_broadcast(&cv);
    while (want_pause) pthread_cond_wait(&cv, &mu);
    parked = false;
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "[TC_CORE] resumed\n");
    audio_on = eng_audio_open();
    if (audio_on) hg->snd_set_output(true);
    next_mt = 0; st_t0 = 0;
    pulled_seen = atomic_load(&audio_pulled);                 /* the pull rate's window starts now, not at the pause */
}

bool ss22_host_frame(void)
{
    if (!host_open) return true;
    const uint64_t t0 = mach_absolute_time();
    hg->input_update();                                       /* ss22_run.c's in_update -> ss22_input_update above */
    /* STEREO (tc_core_set_stereo): the same prepared update drawn twice, once per eye into its own glmetal target -- the right eye
     * into target 1, then the left into target 0 (left as the default target). Off: the mono draw, exactly as before. */
    const float sep = atomic_load(&st_sep), zc = atomic_load(&st_zc);
    const bool stereo = sep > 0.0f && zc > 0.0f && make_right_outs();
    if (stereo) {
        geo_hw_set_stereo(sep, zc, TC_STEREO_MAX_CROSSED);
        glmetal_set_target(1);
        ss22_draw_eye(W, H, +1);
        glmetal_set_target(0);
        ss22_draw_eye(W, H, -1);
    } else {
        if (geo_hw_stereo_on()) geo_hw_set_stereo(0.0f, 0.0f, 0.0f);
        ss22_draw_eye(W, H, 0);                               /* = ss22_draw (a quad left at an eye goes back to mono) */
    }
    /* PANORAMA (tc_core_set_panorama): after the centre (which keeps the texture-bake budget's first claim), the world alone turned by
     * -+ the view's horizontal FOV into targets 2 and 3 (4 and 5: the right eye), 4:3 at the side size; then target 0 again, whose
     * texture the hook is handed. Off: nothing here, the frame is exactly as before. */
    bool pano = false;
    const int pn = atomic_load(&pano_n);
    const float fov = (float)ss22_view_fov();
    atomic_store(&fov_now, fov);
    if (pn > 0) {
        int sw = atomic_load(&pano_w), sh = atomic_load(&pano_h);
        if (sw <= 0) sw = (W * 2 + 2) / 3;
        if (sh <= 0 || sh != (sw * 3 + 2) / 4) sh = (sw * 3 + 2) / 4;    /* 4:3: the side view's picture is the centre's, turned */
        if (sw > 4096) { sw = 4096; sh = 3072; }
        if (sw < 64)   { sw = 64; sh = 48; }
        pano = make_side_outs(sw, sh, stereo);
        if (pano) {
            for (int side = 0; side < 2; side++)
                for (int eye = 0; eye < (stereo ? 2 : 1); eye++) {
                    const int t = 2 + side + 2 * eye;
                    glmetal_set_target_size(t, sw, sh);
                    glmetal_set_target(t);
                    ss22_draw_view(sw, sh, stereo ? (eye ? +1 : -1) : 0, side ? fov : -fov, false);
                }
            glmetal_set_target(0);
            slot_fov[(frame_seq + 1) % 3] = fov;              /* the slot publish_hook is about to fill */
        }
    }
    glmetal_end_frame(publish_hook, (void *)(uintptr_t)((stereo ? 1u : 0u) | (pano ? 2u : 0u)));
    const uint64_t t1 = mach_absolute_time();

    /* the work since the last frame ended (the 68K, the DSP, the sound MCU, the video walk) plus this draw */
    if (st_last_end) { const uint64_t w = t1 - st_last_end; st_work += w; if (w > st_work_max) st_work_max = w; }
    st_draw += t1 - t0; if (t1 - t0 > st_draw_max) st_draw_max = t1 - t0;
    if (!st_t0) { st_t0 = t1; st_n = 0; st_work = st_work_max = st_draw = st_draw_max = 0; st_late = 0; }
    else if (++st_n == 300) {
        const double secs = mt_ms(t1 - st_t0) * 1e-3, fps = st_n / secs;
        atomic_store(&fps_now, fps);
        const unsigned long long pulled = atomic_load_explicit(&audio_pulled, memory_order_relaxed);
        char arate[48] = "";
        if (pulled != pulled_seen) snprintf(arate, sizeof arate, "; audio pulled at %.0f Hz", (pulled - pulled_seen) / secs);
        pulled_seen = pulled;
        fprintf(stderr, "[TC_CORE] frame %u: %.2f fps; work %.2f ms avg %.1f max (draw+encode %.2f avg %.1f max); %u late; %u shots; frame id %llu; %s%s%s\n",
                rr_frame, fps, mt_ms(st_work) / st_n, mt_ms(st_work_max), mt_ms(st_draw) / st_n, mt_ms(st_draw_max), st_late, shots,
                (unsigned long long)(atomic_load(&published) >> 4), stereo ? "stereo" : "mono", pano ? " + panorama" : "", arate);
        st_t0 = t1; st_n = 0; st_work = st_work_max = st_draw = st_draw_max = 0; st_late = 0;
    }

    /* pace: one emulated frame per 1/59.906 s; more than two frames behind (a stall) starts the clock again */
    const uint64_t now = mach_absolute_time();
    if (!next_mt) next_mt = now;
    next_mt += period_mt;
    if (now > next_mt + 2 * period_mt) { next_mt = now; st_late++; }
    else if (now < next_mt) mach_wait_until(next_mt);
    else st_late++;
    st_last_end = mach_absolute_time();

    park_if_asked();
    return true;
}

void ss22_host_close(void)
{
    if (audio_on) { eng_audio_close(); audio_on = false; }
    host_open = false;
}

/* ------------------------------------------------------------------------------------------------ the menu (eng_ui.h): none here */
void eng_ui_set_mixer(const eng_ui_mixer *m) { (void)m; }

/* engine/ss22_run.c's last word (rr_fatal_hook): the lifted program reached code it does not have. The app must not exit with it:
 * stop the sound, keep the last picture, end the game thread. */
void ss22_host_fatal(const char *why)
{
    fprintf(stderr, "[TC_CORE] fatal: %s -- the game thread ends here\n", why);
    if (audio_on) { eng_audio_close(); audio_on = false; }
    pthread_mutex_lock(&mu);
    start_state = -1;
    parked = true;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    pthread_exit(NULL);
}
