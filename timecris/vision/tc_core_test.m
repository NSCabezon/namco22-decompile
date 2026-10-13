/*
 * tc_core_test.m -- a macOS harness for the visionOS core (tc_core.h), built with -DTC_CORE_MAC=ON. It does what the app does,
 * without the app: starts the core on the system Metal device, reads frames back through tc_core_latest_frame, feeds the cabinet,
 * counts TC_EVENT_SHOT, pauses and resumes. Sound plays through the Mac's default output.
 *
 *   tc_core_test <rom_dir> <out_dir> [seconds=60]
 * Writes <out_dir>/core_<t>s.ppm at a few moments: attract mode, after coins + start, in play (trigger every 0.5 s, pedal held).
 * TC_TEST_STEREO=1 (or =<sep>,<zc>): stereo on (tc_core_set_stereo, the header's defaults); each moment is then saved as a
 * <name>_L.ppm / <name>_R.ppm pair from tc_core_latest_frame_stereo.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include "tc_core.h"

static atomic_int shots;
static void on_event(void *user, tc_core_event e, int32_t v) { (void)user; (void)v; if (e == TC_EVENT_SHOT) atomic_fetch_add(&shots, 1); }

static void sleep_s(double s) { [NSThread sleepForTimeInterval:s]; }

static void save_tex(id<MTLDevice> dev, id<MTLCommandQueue> q, const char *dir, const char *name, id<MTLTexture> t, uint64_t fid);
static void save(id<MTLDevice> dev, id<MTLCommandQueue> q, const char *dir, const char *name)
{
    uint64_t fid = 0; void *rp = NULL;
    id<MTLTexture> t = (__bridge id<MTLTexture>)tc_core_latest_frame_stereo(&fid, &rp);
    if (!t) { fprintf(stderr, "[TEST] no frame yet\n"); return; }
    id<MTLTexture> r = (__bridge id<MTLTexture>)rp;
    if (r == t) { save_tex(dev, q, dir, name, t, fid); return; }
    char n[256];
    snprintf(n, sizeof n, "%s_L", name); save_tex(dev, q, dir, n, t, fid);
    snprintf(n, sizeof n, "%s_R", name); save_tex(dev, q, dir, n, r, fid);
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
        const int rc = tc_core_start(rom_abs, (__bridge void *)dev, 640, 480);
        fprintf(stderr, "[TEST] tc_core_start -> %d\n", rc);
        if (rc) return 1;
        tc_core_set_gun(0.5f, 0.5f, true);
        tc_core_set_pedal(true);
        double t = 0;
        sleep_s(20); t += 20; save(dev, q, out_abs, "core_20s_attract");
        tc_core_stop(); fprintf(stderr, "[TEST] stopped\n"); sleep_s(1);
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
            if (k % 40 == 0) { char n[32]; snprintf(n, sizeof n, "core_%.0fs_play", 28 + k * 0.5); save(dev, q, out_abs, n); }
        }
        fprintf(stderr, "[TEST] done: %d shots (TC_EVENT_SHOT) for %d trigger pulls, %.1f fps\n", atomic_load(&shots), k, tc_core_fps());
        tc_core_stop();
        return 0;
    }
}
