/*
 * tc_core_test.m -- a macOS harness for the visionOS core (tc_core.h), built with -DTC_CORE_MAC=ON. It does what the app does,
 * without the app: starts the core on the system Metal device, reads frames back through tc_core_latest_frame, feeds the cabinet,
 * counts TC_EVENT_SHOT, pauses and resumes. Sound plays through the Mac's default output.
 *
 *   tc_core_test <rom_dir> <out_dir> [seconds=60]
 * Writes <out_dir>/core_<t>s.ppm at a few moments: attract mode, after coins + start, in play (trigger every 0.5 s, pedal held).
 * TC_TEST_STEREO=1 (or =<sep>,<zc>): stereo on (tc_core_set_stereo, the header's defaults); each moment is then saved as a
 * <name>_L.ppm / <name>_R.ppm pair from tc_core_latest_frame_stereo.
 * TC_TEST_PANORAMA=1 (or =<side width>): the panorama on (tc_core_set_panorama, one view each side); each moment then also saves
 * <name>_Lside / <name>_Rside (with stereo, _Lside_L ...) from tc_core_latest_frame_side, and the log shows tc_core_view_fov.
 * TC_TEST_EXTAUDIO=1: external audio (tc_core_set_external_audio): no sound card; a thread pulls 10 ms per channel every 10 ms
 * through tc_core_audio_pull_channel, as the app's two emitters do, and the log shows each channel's level and live share.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <math.h>
#include <pthread.h>
#include "tc_core.h"

static atomic_int shots;
static void on_event(void *user, tc_core_event e, int32_t v) { (void)user; (void)v; if (e == TC_EVENT_SHOT) atomic_fetch_add(&shots, 1); }

static void sleep_s(double s) { [NSThread sleepForTimeInterval:s]; }

/* TC_TEST_EXTAUDIO: the app's two emitters, as one thread pulling left then right */
static _Atomic double ext_sq[2]; static atomic_long ext_n, ext_live;
static void *ext_puller(void *u)
{
    (void)u;
    float buf[480];
    const double t0 = CFAbsoluteTimeGetCurrent(); long done = 0;     /* paced by the clock, as a sound card is: 48000 frames a second */
    for (;;) {
        if ((CFAbsoluteTimeGetCurrent() - t0) * TC_AUDIO_SAMPLE_RATE < done + 480) { usleep(2000); continue; }
        done += 480;
        for (int ch = 0; ch < 2; ch++) {
            const bool live = tc_core_audio_pull_channel(ch, buf, 480);
            double sq = 0; for (int i = 0; i < 480; i++) sq += (double)buf[i] * buf[i];
            atomic_store(&ext_sq[ch], atomic_load(&ext_sq[ch]) + sq);
            if (ch == 0) { atomic_fetch_add(&ext_n, 480); if (live) atomic_fetch_add(&ext_live, 480); }
        }
    }
    return NULL;
}
static void ext_report(const char *when)
{
    const long n = atomic_exchange(&ext_n, 0), lv = atomic_exchange(&ext_live, 0);
    const double l = atomic_exchange(&ext_sq[0], 0), r = atomic_exchange(&ext_sq[1], 0);
    if (n) fprintf(stderr, "[TEST] external audio %s: %ld frames pulled, %.0f%% live, rms L %.4f R %.4f\n", when, n, 100.0 * lv / n,
                   sqrt(l / n), sqrt(r / n));
}

static void save_tex(id<MTLDevice> dev, id<MTLCommandQueue> q, const char *dir, const char *name, id<MTLTexture> t, uint64_t fid);
static bool test_pano;
static void save_sides(id<MTLDevice> dev, id<MTLCommandQueue> q, const char *dir, const char *name);
static void save(id<MTLDevice> dev, id<MTLCommandQueue> q, const char *dir, const char *name)
{
    if (test_pano) save_sides(dev, q, dir, name);
    uint64_t fid = 0; void *rp = NULL;
    id<MTLTexture> t = (__bridge id<MTLTexture>)tc_core_latest_frame_stereo(&fid, &rp);
    if (!t) { fprintf(stderr, "[TEST] no frame yet\n"); return; }
    id<MTLTexture> r = (__bridge id<MTLTexture>)rp;
    if (r == t) { save_tex(dev, q, dir, name, t, fid); return; }
    char n[256];
    snprintf(n, sizeof n, "%s_L", name); save_tex(dev, q, dir, n, t, fid);
    snprintf(n, sizeof n, "%s_R", name); save_tex(dev, q, dir, n, r, fid);
}

static void save_sides(id<MTLDevice> dev, id<MTLCommandQueue> q, const char *dir, const char *name)
{
    for (int s = -1; s <= 1; s += 2) {
        uint64_t fid = 0; void *rp = NULL;
        id<MTLTexture> t = (__bridge id<MTLTexture>)tc_core_latest_frame_side(s, &fid, &rp);
        if (!t) { fprintf(stderr, "[TEST] no panorama side %d in the newest frame\n", s); continue; }
        id<MTLTexture> r = (__bridge id<MTLTexture>)rp;
        char n[256];
        snprintf(n, sizeof n, "%s_%cside", name, s < 0 ? 'L' : 'R');
        if (r == t) save_tex(dev, q, dir, n, t, fid);
        else {
            char m[300];
            snprintf(m, sizeof m, "%s_L", n); save_tex(dev, q, dir, m, t, fid);
            snprintf(m, sizeof m, "%s_R", n); save_tex(dev, q, dir, m, r, fid);
        }
    }
    fprintf(stderr, "[TEST] view fov %.4f rad (%.2f deg)\n", tc_core_view_fov(), tc_core_view_fov() * 57.29578);
}

static void save_tex(id<MTLDevice> dev, id<MTLCommandQueue> q, const char *dir, const char *name, id<MTLTexture> t, uint64_t fid)
{
    const NSUInteger w = t.width, h = t.height;
    id<MTLBuffer> buf = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    char p[1024]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)w, (unsigned long)h);
    const uint8_t *px = buf.contents;
    const bool bgra = t.pixelFormat == MTLPixelFormatBGRA8Unorm || t.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    for (NSUInteger i = 0; i < w * h; i++) { uint8_t rgb[3] = { px[i*4 + (bgra ? 2 : 0)], px[i*4 + 1], px[i*4 + (bgra ? 0 : 2)] }; fwrite(rgb, 1, 3, f); }
    fclose(f);
    fprintf(stderr, "[TEST] saved %s (frame id %llu, %.1f fps, %d shots)\n", p, (unsigned long long)fid, tc_core_fps(), atomic_load(&shots));
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3) { fprintf(stderr, "usage: %s <rom_dir> <out_dir> [seconds]\n", argv[0]); return 2; }
        const char *roms = argv[1], *out = argv[2];
        const double secs = argc > 3 ? atof(argv[3]) : 60;
        char rom_abs[PATH_MAX]; if (!realpath(roms, rom_abs)) { perror(roms); return 2; }
        char out_abs[PATH_MAX]; mkdir(out, 0755); if (!realpath(out, out_abs)) { perror(out); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        tc_core_set_event_callback(on_event, NULL);
        { const char *e = getenv("TC_TEST_STEREO"); float sep = TC_STEREO_DEFAULT_SEP, zc = TC_STEREO_DEFAULT_ZC;
          if (e && *e && *e != '0') { if (strchr(e, ',')) sscanf(e, "%f,%f", &sep, &zc); tc_core_set_stereo(sep, zc); fprintf(stderr, "[TEST] stereo sep %.0f zc %.0f\n", sep, zc); } }
        { const char *e = getenv("TC_TEST_PANORAMA");
          if (e && *e && *e != '0') { const int w = atoi(e) > 1 ? atoi(e) : 0; tc_core_set_panorama(1, w, 0); test_pano = true;
                                      fprintf(stderr, "[TEST] panorama, side width %d\n", w); } }
        const char *ea = getenv("TC_TEST_EXTAUDIO");
        const bool ext = ea && *ea && *ea != '0';
        if (ext) { tc_core_set_external_audio(true); pthread_t th; pthread_create(&th, NULL, ext_puller, NULL); }
        const int rc = tc_core_start(rom_abs, (__bridge void *)dev, 640, 480);
        fprintf(stderr, "[TEST] tc_core_start -> %d\n", rc);
        if (rc) return 1;
        tc_core_set_gun(0.5f, 0.5f, true);
        tc_core_set_pedal(true);
        double t = 0;
        sleep_s(20); t += 20; save(dev, q, out_abs, "core_20s_attract");
        ext_report("attract");
        tc_core_stop(); fprintf(stderr, "[TEST] stopped\n"); sleep_s(1);
        ext_report("while stopped");
        uint64_t a = 0, b = 0; tc_core_latest_frame(&a); sleep_s(0.5); tc_core_latest_frame(&b);
        fprintf(stderr, "[TEST] while stopped: frame id %llu -> %llu (%s)\n", (unsigned long long)a, (unsigned long long)b, a == b ? "held" : "STILL RUNNING");
        fprintf(stderr, "[TEST] tc_core_start (resume) -> %d\n", tc_core_start(rom_abs, (__bridge void *)dev, 640, 480));
        for (int i = 0; i < 3; i++) { tc_core_insert_coin(); }
        sleep_s(3); t += 3;
        tc_core_press_start(); sleep_s(5); t += 5;
        save(dev, q, out_abs, "core_28s_started");
        tc_core_press_start();              /* the mode / stage choice screens want a shot too */
        int k = 0;
        while (t < secs) {
            const float u = 0.3f + 0.4f * (float)((k * 37) % 100) / 100.0f, v = 0.3f + 0.4f * (float)((k * 61) % 100) / 100.0f;
            tc_core_set_gun(u, v, true);
            tc_core_set_pedal((k / 12) % 4 != 3);   /* out of cover mostly; duck (reload) now and then */
            tc_core_set_trigger(true); sleep_s(0.1); tc_core_set_trigger(false); sleep_s(0.4);
            t += 0.5; k++;
            if (k % 40 == 0) { char n[32]; snprintf(n, sizeof n, "core_%.0fs_play", 28 + k * 0.5); save(dev, q, out_abs, n); ext_report(n); }
        }
        fprintf(stderr, "[TEST] done: %d shots (TC_EVENT_SHOT) for %d trigger pulls, %.1f fps\n", atomic_load(&shots), k, tc_core_fps());
        tc_core_stop();
        return 0;
    }
}
