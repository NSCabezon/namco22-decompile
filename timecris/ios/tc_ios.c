/*
 * tc_ios.c -- Time Crisis as an iPad app (which is also how it runs on Apple Vision Pro, in its iPad mode).
 *
 * - SDL_main: the app's entry (SDL's UIKit main calls it). tc_game.c's own main() is compiled as tc_main
 *   (CMakeLists.txt) and called with the arguments a desktop launch would give it: the ROM folder inside
 *   the app bundle, and --window so it opens a window rather than running headless.
 * - Everything the engine writes (settings, EEPROM, the last recording) goes to relative paths, so the
 *   working directory moves to the app's writable preferences folder first.
 * - The engine draws with desktop OpenGL 2.1; gl4es provides it over the OpenGL ES 2 context SDL makes
 *   (engine/eng_gl.h routes SDL_GL_GetProcAddress here, ss22_host.c calls the hooks below).
 *
 * This file must not include eng_gl.h: it needs SDL's real SDL_GL_GetProcAddress for gl4es.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>
#include <gl4esinit.h>

void gl4es_pre_swap(void);
void gl4es_post_swap(void);

int tc_main(int argc, char **argv);

static SDL_Window *gl_window;

void *eng_gl_proc(const char *name) { return gl4es_GetProcAddress(name); }

static void *APIENTRY_GL4ES gles_proc(const char *name) { return SDL_GL_GetProcAddress(name); }

static void APIENTRY_GL4ES main_fb_size(int *w, int *h) { SDL_GL_GetDrawableSize(gl_window, w, h); }

void eng_gl_es_ready(void *sdl_window)
{
    gl_window = sdl_window;
    set_getprocaddress(gles_proc);
    set_getmainfbsize(main_fb_size);
    initialize_gl4es();
    int w, h; SDL_GL_GetDrawableSize(gl_window, &w, &h);
    fprintf(stderr, "[IOS] drawable %dx%d\n", w, h);
}

void eng_gl_pre_swap(void) { gl4es_pre_swap(); }
void eng_gl_post_swap(void) { gl4es_post_swap(); }

int SDL_main(int argc, char **argv)
{
    (void)argc;
    static char rom_dir[1024];
    char *base = SDL_GetBasePath();                  /* the app bundle's resources */
    snprintf(rom_dir, sizeof rom_dir, "%sextracted", base ? base : "");
    SDL_free(base);

    char *pref = SDL_GetPrefPath("namco22", "timecris");
    if (pref) {
        if (chdir(pref) != 0) fprintf(stderr, "[IOS] cannot enter %s\n", pref);
        SDL_free(pref);
    }
    fprintf(stderr, "[IOS] ROMs: %s\n", rom_dir);
    /* one picture per game frame: "Auto" follows the display (120 Hz iPad, 90-100 Hz Vision Pro), drawing every
     * frame twice for nothing -- costly through gl4es. The Display menu can still change it. */
    setenv("ENG_FPS", "60", 0);

    char *args[] = { argv[0], rom_dir, "--window", NULL };
    return tc_main(3, args);
}
