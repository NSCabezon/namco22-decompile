/* eng_rawptr.c -- see eng_rawptr.h: pointer devices read directly (Linux evdev, Windows Raw Input). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "eng_rawptr.h"

#define NDEV 16
typedef struct {
    eng_rawptr_info info;
    int   fd;                     /* Linux */
    void *handle;                 /* Windows: the HANDLE raw input names the device by */
    int   explicit_path;          /* Linux: opened by its path, not found by the scan (kept across rescans) */
    char  path[80];
    int   xmin, xmax, ymin, ymax; /* abs ranges */
    float x, y;                   /* abs: 0..1 */
    int   dx, dy;                 /* rel: since the last take */
    uint32_t btn;
    unsigned ev, taken;           /* events seen; events already reported through eng_rawptr_state */
} rp_dev;
static rp_dev dev[NDEV];
static int ndev, sel = -1;
static char sel_id[128];          /* the selection, by id: survives a rescan */
static char err[200];
static int dbg = -1;

static void dlog(void) { if (dbg < 0) dbg = getenv("TC2_GUNLOG") != NULL; }
int  eng_rawptr_count(void) { return ndev; }
const eng_rawptr_info *eng_rawptr_at(int i) { return i >= 0 && i < ndev ? &dev[i].info : NULL; }
int  eng_rawptr_selected(void) { return sel; }
const char *eng_rawptr_selected_id(void) { return sel >= 0 ? dev[sel].info.id : ""; }
const char *eng_rawptr_error(void) { return err; }
unsigned eng_rawptr_activity(int i) { return i >= 0 && i < ndev ? dev[i].ev : 0; }
uint32_t eng_rawptr_buttons(void) { return sel >= 0 ? dev[sel].btn : 0; }
bool eng_rawptr_state(float *x, float *y, int *dx, int *dy, uint32_t *buttons, unsigned *events)
{
    if (sel < 0) return false;
    rp_dev *d = &dev[sel];
    *x = d->x; *y = d->y; *dx = d->dx; *dy = d->dy; d->dx = d->dy = 0;
    *buttons = d->btn; *events = d->ev - d->taken; d->taken = d->ev;
    return true;
}
static void set_button(rp_dev *d, uint32_t mask, int down) { if (down) d->btn |= mask; else d->btn &= ~mask; d->ev++; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int ci_contains(const char *hay, const char *needle)
{
    const size_t n = strlen(needle); if (!n) return 0;
    for (const char *p = hay; *p; p++) {
        size_t k = 0; while (k < n && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == n) return 1;
    }
    return 0;
}

/* ================================================================ Linux: evdev ================================================================ */
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#define BITS(n) (((n) + 7) / 8)
static int bit(const uint8_t *b, int i) { return (b[i / 8] >> (i % 8)) & 1; }
static void dev_close(rp_dev *d) { if (d->fd >= 0) close(d->fd); d->fd = -1; d->info.open = 0; }

/* open one event node; a pointer = it moves (abs or rel x/y) and has a button, and is not a pad / joystick. explicit = a path the
 * player named: taken whatever it looks like, and a node that cannot be asked (a FIFO) is an absolute device over 0..65535 */
static int add_evdev(const char *path, int explicit)
{
    for (int i = 0; i < ndev; i++) if (!strcmp(dev[i].path, path)) return i;
    if (ndev >= NDEV) return -1;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if ((errno == EACCES || errno == EPERM) && !err[0])
            snprintf(err, sizeof err, "no permission to read %s: add your user to the 'input' group (sudo usermod -aG input $USER, then log in again)", path);
        else if (explicit) snprintf(err, sizeof err, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    uint8_t ev[BITS(EV_MAX + 1)], key[BITS(KEY_MAX + 1)], ab[BITS(ABS_MAX + 1)], rel[BITS(REL_MAX + 1)];
    memset(ev, 0, sizeof ev); memset(key, 0, sizeof key); memset(ab, 0, sizeof ab); memset(rel, 0, sizeof rel);
    char name[64] = "", phys[64] = "", uniq[64] = "";
    const int asked = ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) >= 0;
    if (asked) {
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key); ioctl(fd, EVIOCGBIT(EV_ABS, sizeof ab), ab); ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name); ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys); ioctl(fd, EVIOCGUNIQ(sizeof uniq - 1), uniq);
    }
    const int is_abs = asked && bit(ev, EV_ABS) && bit(ab, ABS_X) && bit(ab, ABS_Y);
    const int is_rel = asked && bit(ev, EV_REL) && bit(rel, REL_X) && bit(rel, REL_Y);
    const int has_btn = asked && bit(ev, EV_KEY) && (bit(key, BTN_LEFT) || bit(key, BTN_TOUCH) || bit(key, BTN_TOOL_PEN));
    const int is_pad = asked && bit(ev, EV_KEY) && (bit(key, BTN_JOYSTICK) || bit(key, BTN_GAMEPAD) || bit(key, BTN_SOUTH) || bit(key, BTN_TRIGGER));
    if (!explicit && (!(is_abs || is_rel) || !has_btn || is_pad)) { close(fd); return -1; }
    rp_dev *d = &dev[ndev]; memset(d, 0, sizeof *d);
    d->fd = fd; d->explicit_path = explicit; snprintf(d->path, sizeof d->path, "%s", path);
    d->info.open = 1; d->info.abs = asked ? is_abs : 1; d->info.space = 0;
    d->xmin = d->ymin = 0; d->xmax = d->ymax = 65535;
    if (is_abs) {
        struct input_absinfo ax, ay;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ax) >= 0 && ax.maximum > ax.minimum) { d->xmin = ax.minimum; d->xmax = ax.maximum; d->x = (float)(ax.value - ax.minimum) / (ax.maximum - ax.minimum); }
        if (ioctl(fd, EVIOCGABS(ABS_Y), &ay) >= 0 && ay.maximum > ay.minimum) { d->ymin = ay.minimum; d->ymax = ay.maximum; d->y = (float)(ay.value - ay.minimum) / (ay.maximum - ay.minimum); }
    } else { d->x = d->y = 0.5f; }
    if (!name[0]) { const char *b = strrchr(path, '/'); snprintf(name, sizeof name, "%s", b ? b + 1 : path); }
    snprintf(d->info.name, sizeof d->info.name, "%s", name);
    if (explicit && !asked) snprintf(d->info.id, sizeof d->info.id, "%s", path);
    else snprintf(d->info.id, sizeof d->info.id, "evdev:%s", uniq[0] ? uniq : phys[0] ? phys : name);
    dlog();
    if (dbg) fprintf(stderr, "[GUN] %s: \"%s\" %s%s id %s (%s)\n", path, d->info.name, d->info.abs ? "absolute" : "relative", asked ? "" : " (assumed)", d->info.id, explicit ? "named" : "found");
    return ndev++;
}
static int cmp_path(const void *a, const void *b) { return strcmp(((const rp_dev *)a)->path, ((const rp_dev *)b)->path); }
void eng_rawptr_scan(void)
{
    char keep[NDEV][80]; int nkeep = 0;                /* paths the player named stay */
    for (int i = 0; i < ndev; i++) { if (dev[i].explicit_path && nkeep < NDEV) snprintf(keep[nkeep++], 80, "%s", dev[i].path); dev_close(&dev[i]); }
    ndev = 0; sel = -1; err[0] = 0;
    DIR *dir = opendir("/dev/input");
    if (dir) {
        char paths[64][40]; int np = 0;
        struct dirent *e;
        while ((e = readdir(dir)) && np < 64) if (!strncmp(e->d_name, "event", 5)) snprintf(paths[np++], 40, "/dev/input/%s", e->d_name);
        closedir(dir);
        qsort(paths, (size_t)np, sizeof paths[0], (int (*)(const void *, const void *))strcmp);
        for (int i = 0; i < np; i++) add_evdev(paths[i], 0);
    }
    for (int i = 0; i < nkeep; i++) add_evdev(keep[i], 1);
    (void)cmp_path;
    if (ndev && err[0]) err[0] = 0;                    /* some could be read: no complaint */
    if (sel_id[0]) eng_rawptr_select(sel_id);
}
void eng_rawptr_attach(SDL_Window *win) { (void)win; }
void eng_rawptr_poll(void)
{
    for (int i = 0; i < ndev; i++) {
        rp_dev *d = &dev[i]; if (d->fd < 0) continue;
        struct input_event ie[32];
        for (int guard = 0; guard < 64; guard++) {
            const ssize_t n = read(d->fd, ie, sizeof ie);
            if (n <= 0) {
                if (n < 0 && errno != EAGAIN && errno != EINTR) { dlog(); if (dbg) fprintf(stderr, "[GUN] %s: %s (unplugged?)\n", d->path, strerror(errno)); dev_close(d); }
                break;                                 /* a FIFO with no writer reads 0: nothing now */
            }
            for (int k = 0; k < (int)(n / sizeof ie[0]); k++) {
                const struct input_event *e = &ie[k];
                if (e->type == EV_ABS) {
                    if (e->code == ABS_X) { d->x = d->xmax > d->xmin ? (float)(e->value - d->xmin) / (d->xmax - d->xmin) : 0.5f; d->ev++; }
                    else if (e->code == ABS_Y) { d->y = d->ymax > d->ymin ? (float)(e->value - d->ymin) / (d->ymax - d->ymin) : 0.5f; d->ev++; }
                    d->x = clamp01(d->x); d->y = clamp01(d->y);
                } else if (e->type == EV_REL) {
                    if (e->code == REL_X) { d->dx += e->value; d->ev++; } else if (e->code == REL_Y) { d->dy += e->value; d->ev++; }
                } else if (e->type == EV_KEY) {
                    switch (e->code) {
                    case BTN_LEFT: case BTN_TOUCH: set_button(d, SDL_BUTTON_LMASK, e->value != 0); break;
                    case BTN_RIGHT: set_button(d, SDL_BUTTON_RMASK, e->value != 0); break;
                    case BTN_MIDDLE: set_button(d, SDL_BUTTON_MMASK, e->value != 0); break;
                    case BTN_SIDE: set_button(d, SDL_BUTTON_X1MASK, e->value != 0); break;
                    case BTN_EXTRA: set_button(d, SDL_BUTTON_X2MASK, e->value != 0); break;
                    default: break;
                    }
                }
            }
        }
    }
}
static int select_path(const char *spec) { const int i = add_evdev(spec, 1); return i; }

/* ================================================================ Windows: Raw Input ================================================================ */
#elif defined(_WIN32)
#include <windows.h>
#include <SDL2/SDL_syswm.h>
static HWND hwnd;
static int find_handle(HANDLE h) { for (int i = 0; i < ndev; i++) if (dev[i].handle == (void *)h) return i; return -1; }
static int add_raw(HANDLE h)
{
    int i = find_handle(h); if (i >= 0) return i;
    if (ndev >= NDEV) return -1;
    rp_dev *d = &dev[ndev]; memset(d, 0, sizeof *d);
    d->handle = (void *)h; d->fd = -1; d->info.open = 1; d->info.abs = 0; d->x = d->y = 0.5f;
    char path[256] = ""; UINT n = sizeof path;
    if (GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, path, &n) == (UINT)-1) path[0] = 0;
    snprintf(d->info.id, sizeof d->info.id, "%s", path[0] ? path : "?");
    /* the name: the HID path's VID / PID, the only name Windows gives without SetupAPI */
    const char *v = strstr(path, "VID_"), *p = strstr(path, "PID_");
    if (v && p) snprintf(d->info.name, sizeof d->info.name, "Mouse %.8s %.8s", v, p);
    else snprintf(d->info.name, sizeof d->info.name, "%s", strstr(path, "RDP_MOU") ? "Remote desktop mouse" : "Mouse");
    dlog();
    if (dbg) fprintf(stderr, "[GUN] raw input device \"%s\" id %s\n", d->info.name, d->info.id);
    return ndev++;
}
static void SDLCALL hook(void *u, void *hWnd, unsigned int msg, Uint64 wParam, Sint64 lParam)
{
    (void)u; (void)hWnd; (void)wParam;
    if (msg != WM_INPUT) return;
    static uint8_t buf[256]; UINT sz = sizeof buf;
    if (GetRawInputData((HRAWINPUT)(LPARAM)lParam, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    const RAWINPUT *ri = (const RAWINPUT *)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE) return;
    const int i = add_raw(ri->header.hDevice); if (i < 0) return;
    rp_dev *d = &dev[i]; const RAWMOUSE *m = &ri->data.mouse;
    if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
        d->info.abs = 1; d->info.space = (m->usFlags & MOUSE_VIRTUAL_DESKTOP) ? 0 : 1;
        d->x = clamp01(m->lLastX / 65535.0f); d->y = clamp01(m->lLastY / 65535.0f);
        d->ev++;
    } else if (m->lLastX || m->lLastY) { d->dx += m->lLastX; d->dy += m->lLastY; d->ev++; }
    static const struct { USHORT down, up; uint32_t mask; } bt[5] = {
        { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, SDL_BUTTON_LMASK }, { RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, SDL_BUTTON_RMASK },
        { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, SDL_BUTTON_MMASK }, { RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, SDL_BUTTON_X1MASK },
        { RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, SDL_BUTTON_X2MASK } };
    for (int b = 0; b < 5; b++) {
        if (m->usButtonFlags & bt[b].down) set_button(d, bt[b].mask, 1);
        if (m->usButtonFlags & bt[b].up) set_button(d, bt[b].mask, 0);
    }
}
void eng_rawptr_scan(void)
{
    ndev = 0; sel = -1; err[0] = 0;
    UINT n = 0;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1 || !n) return;
    RAWINPUTDEVICELIST *l = calloc(n, sizeof *l); if (!l) return;
    n = GetRawInputDeviceList(l, &n, sizeof(RAWINPUTDEVICELIST));
    for (UINT i = 0; i < n && i < 64; i++) if (l[i].dwType == RIM_TYPEMOUSE) add_raw(l[i].hDevice);
    free(l);
    if (sel_id[0]) eng_rawptr_select(sel_id);
}
void eng_rawptr_attach(SDL_Window *win)
{
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    hwnd = info.info.win.window;
    RAWINPUTDEVICE rid = { 1, 2, RIDEV_INPUTSINK, hwnd };        /* generic desktop, mouse; also while another window is in front */
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) { snprintf(err, sizeof err, "RegisterRawInputDevices failed (%lu)", (unsigned long)GetLastError()); return; }
    SDL_SetWindowsMessageHook(hook, NULL);
}
void eng_rawptr_poll(void) {}
static int select_path(const char *spec) { (void)spec; return -1; }

/* ================================================================ others: nothing ================================================================ */
#else
void eng_rawptr_scan(void) { ndev = 0; sel = -1; snprintf(err, sizeof err, "no direct device input on this system"); }
void eng_rawptr_attach(SDL_Window *win) { (void)win; }
void eng_rawptr_poll(void) {}
static int select_path(const char *spec) { (void)spec; return -1; }
#endif

int eng_rawptr_select(const char *spec)
{
    dlog();
    if (!spec || !*spec) { sel = -1; sel_id[0] = 0; return -1; }
    int i = -1;
    if (spec[0] == '/' || spec[0] == '\\' || (isalpha((unsigned char)spec[0]) && spec[1] == ':')) i = select_path(spec);
    else {
        char *end; const long n = strtol(spec, &end, 10);
        if (*end == 0 && n >= 1) i = n <= ndev ? (int)n - 1 : -1;
        else {
            for (int k = 0; k < ndev && i < 0; k++) if (!strcmp(dev[k].info.id, spec)) i = k;
            for (int k = 0; k < ndev && i < 0; k++) if (ci_contains(dev[k].info.name, spec)) i = k;
            for (int k = 0; k < ndev && i < 0; k++) if (ci_contains(dev[k].info.id, spec)) i = k;
        }
    }
    sel = i;
    if (i >= 0) { snprintf(sel_id, sizeof sel_id, "%s", dev[i].info.id); if (dbg) fprintf(stderr, "[GUN] using \"%s\" (%s)\n", dev[i].info.name, dev[i].info.id); }
    else { snprintf(sel_id, sizeof sel_id, "%s", spec); if (dbg) fprintf(stderr, "[GUN] no device matches \"%s\": the system pointer\n", spec); }   /* kept: it may be plugged in later */
    return i;
}
