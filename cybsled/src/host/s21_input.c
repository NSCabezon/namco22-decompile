/*
 * s21_input.c -- Cyber Sled's cabinet on a keyboard, gamepads and joysticks: two sticks (the sled is driven tank-style, left and right
 * stick forward/back), Gun, Missile, Viewport, coin, start, the Test switch (a TOGGLE, like MAME's PORT_SERVICE) and the Service
 * button. EVERY direction of both sticks and every button is bound on the Controls page (engine/twin_stick.c): a key, a gamepad
 * button or stick, or any joystick's axis, hat or button -- two joysticks of the same model are told apart. The defaults are MAME's
 * keys for cybsled (left stick E/D and S/F, right stick I/K and J/L), the arrows as "tank" controls and a gamepad's two sticks; a
 * held key ramps the stick 16 counts a frame (PORT_KEYDELTA 16) and it springs back when released, an axis sets it directly.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "eng_cfg.h"
#include "eng_ui.h"
#include "twin_stick.h"
#include "s21_host.h"
#include "s21_board.h"
#include "s21_io.h"

/* the Controls page's bindable rows (twin_stick.h: the eight directions first) */
enum { CS_FWD = TS_NDIR, CS_BACK, CS_TURN_L, CS_TURN_R, CS_STRAFE, CS_GUN, CS_MISSILE, CS_VIEW, CS_COIN, CS_START, CS_SERVICE, CS_NCTL };
static const ts_def cs_ctl[CS_NCTL] = {
    [TS_L_UP]    = { "Left stick up",    "left_up",    "E", "-lefty" },  [TS_L_DOWN]  = { "Left stick down",  "left_down",  "D", "+lefty" },
    [TS_L_LEFT]  = { "Left stick left",  "left_left",  "S", "-leftx" },  [TS_L_RIGHT] = { "Left stick right", "left_right", "F", "+leftx" },
    [TS_R_UP]    = { "Right stick up",   "right_up",   "I", "-righty" }, [TS_R_DOWN]  = { "Right stick down", "right_down", "K", "+righty" },
    [TS_R_LEFT]  = { "Right stick left", "right_left", "J", "-rightx" }, [TS_R_RIGHT] = { "Right stick right", "right_right", "L", "+rightx" },
    [CS_FWD]     = { "Both sticks forward", "tank_forward", "Up", "dpup" },   [CS_BACK]   = { "Both sticks back", "tank_back", "Down", "dpdown" },
    [CS_TURN_L]  = { "Turn left",  "tank_left",  "Left", "dpleft" },          [CS_TURN_R] = { "Turn right", "tank_right", "Right", "dpright" },
    [CS_STRAFE]  = { "Strafe (with Turn)", "tank_strafe", "Left Shift,Right Shift", NULL },
    [CS_GUN]     = { "Gun",      "gun",     "Left Ctrl,Z", "a,+righttrigger" }, [CS_MISSILE] = { "Missile", "missile", "Left Alt,X", "b,+lefttrigger" },
    [CS_VIEW]    = { "Viewport", "view",    "Space,C", "y" },
    [CS_COIN]    = { "Coin",     "coin",    "5", "back" },    [CS_START] = { "Start", "start", "1,Return", "start" },
    [CS_SERVICE] = { "Service button", "service", "9", "leftshoulder" },
};
static void cfg_save(const char *k, const char *v) { eng_cfg_set(k, v); }

static int test_on, test_prev;
static int axis[4] = { 0x7F, 0x7F, 0x7F, 0x7F };       /* an0 right Y, an1 left Y, an2 right X, an3 left X */

/* CS_FAKEPAD=1 (TEST): an SDL virtual game controller (Xbox 360 ids) attached at start and driven by a fixed script, so both analog
 * sticks reach the levers through the same SDL game-controller path a real pad uses (the Linux driver is the one part it skips).
 * Script, in frames: Back (coin) 240, Start 330, A x4 from 420 (picks Training), then from 2000: both sticks up 180 f, left down +
 * right up 120 f, both left 120 f, both half up 120 f, centre, D-pad up 120 f. Read the result with CS_INLOG=1. */
static int fake_idx = -1;
static SDL_Joystick *fake_js;
static void fakepad_init(void)
{
    if (!getenv("CS_FAKEPAD")) return;
#if SDL_VERSION_ATLEAST(2, 24, 0)                        /* SDL_JoystickAttachVirtualEx: SDL 2.24 (Ubuntu 22.04 has 2.0.20) */
    SDL_VirtualJoystickDesc d; SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION; d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX; d.nbuttons = SDL_CONTROLLER_BUTTON_MAX; d.vendor_id = 0x045E; d.product_id = 0x028E;
    d.name = "Virtual X-Box 360 pad (CS_FAKEPAD)";
    fake_idx = SDL_JoystickAttachVirtualEx(&d);
    if (fake_idx >= 0) fake_js = SDL_JoystickOpen(fake_idx);
    fprintf(stderr, "[INPUT] CS_FAKEPAD: virtual pad %s\n", fake_js ? "attached" : SDL_GetError());
#else
    fprintf(stderr, "[INPUT] CS_FAKEPAD: needs SDL 2.24 or later (this build: %d.%d.%d)\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
#endif
}
static void fakepad_frame(void)
{
    if (!fake_js) return;
#if SDL_VERSION_ATLEAST(2, 24, 0)
    static int f; f++;
    int lx = 0, ly = 0, rx = 0, ry = 0, hat_up = 0;
    const int t = f - 2000;
    if (t >= 0 && t < 180) { ly = -32767; ry = -32767; }
    else if (t >= 180 && t < 300) { ly = 32767; ry = -32767; }
    else if (t >= 300 && t < 420) { lx = -32767; rx = -32767; }
    else if (t >= 420 && t < 540) { ly = -16000; ry = -16000; }
    else if (t >= 600 && t < 720) hat_up = 1;
    SDL_JoystickSetVirtualAxis(fake_js, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)lx); SDL_JoystickSetVirtualAxis(fake_js, SDL_CONTROLLER_AXIS_LEFTY, (Sint16)ly);
    SDL_JoystickSetVirtualAxis(fake_js, SDL_CONTROLLER_AXIS_RIGHTX, (Sint16)rx); SDL_JoystickSetVirtualAxis(fake_js, SDL_CONTROLLER_AXIS_RIGHTY, (Sint16)ry);
    SDL_JoystickSetVirtualButton(fake_js, SDL_CONTROLLER_BUTTON_DPAD_UP, (Uint8)hat_up);
    SDL_JoystickSetVirtualButton(fake_js, SDL_CONTROLLER_BUTTON_BACK, (Uint8)(f >= 240 && f < 246));
    SDL_JoystickSetVirtualButton(fake_js, SDL_CONTROLLER_BUTTON_START, (Uint8)(f >= 330 && f < 336));
    SDL_JoystickSetVirtualButton(fake_js, SDL_CONTROLLER_BUTTON_A, (Uint8)(f >= 420 && f < 700 && (f - 420) % 72 < 6));
#endif
}

/* ---- A WHEEL AND PEDALS (raw joysticks, the ones SDL has no gamepad mapping for) --------------------------------------------
 * The wheel TURNS the sled (the two levers opposite, as the arrows' Left/Right; outside a battle it moves both levers sideways to
 * pick in the menus), the gas pedal pushes both levers forward, the brake pedal both back. Bound on the Controls page (Enter on a
 * row, then move the wheel / press the pedal) and saved as joy_turn / joy_gas / joy_brake in cs21_controls.cfg. An unbound wheel
 * is axis 0 of any raw joystick that has been turned (more than 1/8 of the range from where it first read, so a pedal set resting
 * off centre does not steer); pedals have no default. Cyber Commando does the same through rr_host.c. */
#define MAXJOY 8
static SDL_Joystick *joys[MAXJOY];
typedef struct { int axis; int invert; int dir; int rest; char guid[40]; } rawbind;   /* axis -1 = unbound; dir/rest: a pedal's travel */
static rawbind b_turn = { 0, 0, 0, 0, "" }, b_gas = { -1, 0, 0, 0, "" }, b_brake = { -1, 0, 0, 0, "" };
static int bind_loaded, learn = -1, learn_base[MAXJOY][32], turn_seen[MAXJOY], turn_rest[MAXJOY], turn_live[MAXJOY];

static void guid_of(SDL_Joystick *j, char *out) { SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), out, 40); }
static void bind_save(const char *key, const rawbind *b)
{
    char v[96]; snprintf(v, sizeof v, "%d,%d,%d,%d,%s", b->axis, b->invert, b->dir, b->rest, b->guid);
    eng_cfg_set(key, v);
}
static void bind_load(const char *key, rawbind *b)
{
    const char *v = eng_cfg_get(key); if (!v) return;
    rawbind t = *b; char g[48] = "";
    if (sscanf(v, "%d,%d,%d,%d,%47s", &t.axis, &t.invert, &t.dir, &t.rest, g) >= 4) { snprintf(t.guid, sizeof t.guid, "%s", g); *b = t; }
}
static void joy_open(int idx)
{
    if (SDL_IsGameController(idx)) return;
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(idx);
    int free_slot = -1;
    for (int i = 0; i < MAXJOY; i++) { if (joys[i] && SDL_JoystickInstanceID(joys[i]) == id) return; if (!joys[i] && free_slot < 0) free_slot = i; }
    if (free_slot < 0) return;
    if ((joys[free_slot] = SDL_JoystickOpen(idx))) {
        turn_seen[free_slot] = turn_live[free_slot] = 0;
        fprintf(stderr, "[INPUT] joystick %d: %s (%d axes, %d buttons) -- wheel / pedals: Controls page, Enter on a row to bind\n", free_slot,
                SDL_JoystickName(joys[free_slot]), SDL_JoystickNumAxes(joys[free_slot]), SDL_JoystickNumButtons(joys[free_slot]));
    }
}
static int joy_axes(int i) { int n = SDL_JoystickNumAxes(joys[i]); return n > 32 ? 32 : n; }
static int bound_to(const rawbind *b, int i) { char g[40]; if (!b->guid[0]) return 1; guid_of(joys[i], g); return !strcmp(g, b->guid); }

/* the wheel, -1 (left) .. +1 (right); *on = it is turned past its deadzone */
static double raw_turn(int *on)
{
    double best = 0;
    for (int i = 0; i < MAXJOY; i++) {
        if (!joys[i] || b_turn.axis < 0 || b_turn.axis >= joy_axes(i) || !bound_to(&b_turn, i)) continue;
        if (!b_turn.guid[0] && ts_uses_joystick(joys[i])) continue;      /* the unbound default is no wheel on a joystick that IS a stick */
        int v = SDL_JoystickGetAxis(joys[i], b_turn.axis);
        if (!turn_seen[i]) { turn_seen[i] = 1; turn_rest[i] = v; }
        if (!turn_live[i] && (b_turn.guid[0] || abs(v - turn_rest[i]) > 4096)) turn_live[i] = 1;
        if (!turn_live[i]) continue;
        if (b_turn.invert) v = -v;
        double f = v / 32767.0;
        f = f > -0.003 && f < 0.003 ? 0 : (f > 0 ? f - 0.003 : f + 0.003) / 0.997;      /* a wheel's own sensor noise and nothing more */
        if (f > 1) f = 1; if (f < -1) f = -1;
        if (fabs(f) > fabs(best)) best = f;
    }
    *on = best != 0;
    return best;
}
static double raw_pedal(const rawbind *b)                /* 0 .. 1 */
{
    double best = 0;
    for (int i = 0; i < MAXJOY; i++) {
        if (!joys[i] || b->axis < 0 || b->axis >= joy_axes(i) || !bound_to(b, i)) continue;
        int r = SDL_JoystickGetAxis(joys[i], b->axis), range = b->dir > 0 ? 32767 - b->rest : b->rest + 32768, d = b->dir > 0 ? r - b->rest : b->rest - r;
        double f = range > 0 ? (double)d / range : 0;
        f = f > 0.03 ? (f - 0.03) / 0.97 : 0;
        if (f > 1) f = 1;
        if (f > best) best = f;
    }
    return best;
}
static void learn_begin(int which)
{
    learn = which;
    for (int i = 0; i < MAXJOY; i++) if (joys[i]) for (int a = 0; a < joy_axes(i); a++) learn_base[i][a] = SDL_JoystickGetAxis(joys[i], a);
    fprintf(stderr, "[INPUT] binding %s: move it now\n", which == 0 ? "the wheel (turn it RIGHT)" : which == 1 ? "the gas pedal (press it)" : "the brake pedal (press it)");
}
static void learn_step(void)
{
    for (int i = 0; i < MAXJOY && learn >= 0; i++) {
        if (!joys[i]) continue;
        for (int a = 0; a < joy_axes(i); a++) {
            int d = SDL_JoystickGetAxis(joys[i], a) - learn_base[i][a];
            if (abs(d) < 16000) continue;
            rawbind *b = learn == 0 ? &b_turn : learn == 1 ? &b_gas : &b_brake;
            b->axis = a; guid_of(joys[i], b->guid);
            if (learn == 0) { b->invert = d < 0; b->dir = b->rest = 0; turn_seen[i] = 0; turn_live[i] = 1; }
            else { b->invert = 0; b->dir = d > 0 ? 1 : -1; b->rest = learn_base[i][a]; }
            bind_save(learn == 0 ? "joy_turn" : learn == 1 ? "joy_gas" : "joy_brake", b);
            fprintf(stderr, "[INPUT] bound to %s axis %d%s\n", SDL_JoystickName(joys[i]), a, learn == 0 && b->invert ? " (inverted)" : "");
            learn = -1;
            break;
        }
    }
}
void s21_input_init(void)
{
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    ts_init(cs_ctl, CS_NCTL, cfg_save);
    ts_load_from(eng_cfg_get);
    fakepad_init();
}

void s21_input_event(const SDL_Event *e)
{
    ts_event(e);
    if (e->type == SDL_JOYDEVICEADDED) joy_open(e->jdevice.which);
    else if (e->type == SDL_JOYDEVICEREMOVED) {
        for (int i = 0; i < MAXJOY; i++) if (joys[i] && SDL_JoystickInstanceID(joys[i]) == e->jdevice.which) { SDL_JoystickClose(joys[i]); joys[i] = NULL; }
    }
}

static void ramp(int *v, int dec, int inc)
{
    const int lo = 0x3F, hi = 0xBF, c = 0x7F;
    if (dec && !inc) *v = *v - 16 < lo ? lo : *v - 16;
    else if (inc && !dec) *v = *v + 16 > hi ? hi : *v + 16;
    else if (*v < c) *v = *v + 16 > c ? c : *v + 16;     /* springs back */
    else if (*v > c) *v = *v - 16 < c ? c : *v - 16;
}

static s21_inputs cur = { .mcub = 0xFF, .mcuc = 0xFF, .mcuh = 0xFF, .dsw = 0xDF, .an = { 0x7F, 0x7F, 0x7F, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF }, .dial = { 0xFF, 0xFF, 0xFF, 0xFF } };

/* move an axis toward a target at the keyboard's 16 counts a frame (MAME's PORT_KEYDELTA(16) for these ports) */
static void toward(int *v, int t) { if (*v < t) *v = *v + 16 > t ? t : *v + 16; else if (*v > t) *v = *v - 16 < t ? t : *v - 16; }

void s21_input_update(void)
{
    fakepad_frame();
    SDL_GameControllerUpdate();
    /* THE TANK CONTROLS DRIVE THE SLED: Cyber Sled is steered like a tank with two levers (AN1 left, AN0 right; a LOWER value is
     * forward -- measured: both held at 0x3F drive up to the wall ahead, both at 0xBF back away). Forward/Back = both levers
     * forward/back, Turn left/right = the levers opposite (turn on the spot; with Forward/Back, a curve), Strafe + Turn = both levers
     * sideways. Defaults: the arrows, Shift and the pad's D-pad. The eight stick directions work each lever on its own. */
    const int up = ts_held(CS_FWD), dn = ts_held(CS_BACK), lt = ts_held(CS_TURN_L), rt = ts_held(CS_TURN_R);
    const int shift = ts_held(CS_STRAFE);
    const int arrows = up || dn || lt || rt;
    /* each lever from its own two directions: an axis sets it, a key / button / hat ramps it (an0 right Y, an1 left Y, an2 right X,
     * an3 left X) */
    static const int lev[4][2] = { { TS_R_UP, TS_R_DOWN }, { TS_L_UP, TS_L_DOWN }, { TS_R_LEFT, TS_R_RIGHT }, { TS_L_LEFT, TS_L_RIGHT } };
    double an[4]; bool dig[4];
    for (int i = 0; i < 4; i++) an[i] = ts_lever(lev[i][0], lev[i][1], &dig[i]);
    if (arrows) {
        /* The menus (course / vehicle / pilot select) read only the levers' SIDEWAYS movement, a battle reads both: master work
         * RAM byte 0x100907 is 0 on the title, attract, course select, round and tutorial screens and 3 while the sled is
         * controllable (measured over a full coin -> Training run, cybsled/PLAN.md). Outside a battle Left/Right move both levers
         * sideways; in a battle they turn (and Strafe + Left/Right strafes). */
        const int battle = ((g_s21.mram[0x907 >> 1] & 0xFF) != 0);       /* odd byte = the low half of the big-endian word */
        const int fwd = up - dn, turn = (shift || !battle) ? 0 : lt - rt, side = (shift || !battle) ? rt - lt : 0;
        int l = fwd - turn, r = fwd + turn;                  /* +1 forward .. -1 back, per lever */
        l = l > 1 ? 1 : l < -1 ? -1 : l; r = r > 1 ? 1 : r < -1 ? -1 : r;
        toward(&axis[1], 0x7F - 0x40 * l); toward(&axis[0], 0x7F - 0x40 * r);
        toward(&axis[3], 0x7F + 0x40 * side); toward(&axis[2], 0x7F + 0x40 * side);
    } else for (int i = 0; i < 4; i++) ramp(&axis[i], dig[i] && an[i] < 0, dig[i] && an[i] > 0);
    s21_inputs in = cur;
    for (int i = 0; i < 4; i++) {                        /* an axis out of its deadzone wins over the keys, as a pad's stick did */
        int v = axis[i];
        if (!dig[i] && an[i] != 0) { v = 0x7F + (int)lround(an[i] * 0x40); v = v < 0x3F ? 0x3F : v > 0xBF ? 0xBF : v; }
        in.an[i] = (uint8_t)v;
    }
    /* the wheel and pedals (analog; they win over the keys / pad only while they are off their rest) */
    if (!bind_loaded) { bind_loaded = 1; bind_load("joy_turn", &b_turn); bind_load("joy_gas", &b_gas); bind_load("joy_brake", &b_brake); }
    SDL_JoystickUpdate();
    learn_step();
    {
        int on; const double w = raw_turn(&on), t = raw_pedal(&b_gas) - raw_pedal(&b_brake);   /* w: right +; t: forward + */
        if (on || t != 0) {
            const int battle = ((g_s21.mram[0x907 >> 1] & 0xFF) != 0);
            const double turn = (shift || !battle) ? 0 : w, side = (shift || !battle) ? w : 0;
            double l = t + turn, r = t - turn;                     /* +1 forward .. -1 back, per lever */
            l = l > 1 ? 1 : l < -1 ? -1 : l; r = r > 1 ? 1 : r < -1 ? -1 : r;
            in.an[1] = (uint8_t)(0x7F - (int)(0x40 * l)); in.an[0] = (uint8_t)(0x7F - (int)(0x40 * r));
            if (side != 0) { in.an[3] = (uint8_t)(0x7F + (int)(0x40 * side)); in.an[2] = in.an[3]; }
        }
    }
    const int gun = ts_held(CS_GUN), missile = ts_held(CS_MISSILE), view = ts_held(CS_VIEW);
    const int coin = ts_held(CS_COIN), start = ts_held(CS_START), service = ts_held(CS_SERVICE);
    const int test = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_F2];
    if (test && !test_prev) { test_on = !test_on; fprintf(stderr, "[INPUT] test switch %s\n", test_on ? "ON" : "OFF"); }
    test_prev = test;
    in.mcuh = (uint8_t)(0xFF & ~(gun ? 0x20 : 0) & ~(missile ? 0x08 : 0) & ~(view ? 0x02 : 0));
    in.mcuc = (uint8_t)(0xFF & ~(coin ? 0x20 : 0) & ~(service ? 0x80 : 0));
    in.mcub = (uint8_t)(0xFF & ~(start ? 0x80 : 0));
    in.dsw = (uint8_t)((cur.dsw & ~1) | (test_on ? 0 : 1));
    { static s21_inputs prev; static int log = -1; if (log < 0) log = getenv("CS_INLOG") != NULL;   /* CS_INLOG=1: every change of the ports */
      s21_debug_frame(&in);
      if (log && memcmp(&prev, &in, sizeof in)) { fprintf(stderr, "[INPUT] b %02X c %02X h %02X dsw %02X an %02X %02X %02X %02X\n", in.mcub, in.mcuc, in.mcuh, in.dsw, in.an[0], in.an[1], in.an[2], in.an[3]); prev = in; } }
    s21_io_set_inputs(&in);
}

void s21_input_set_test(int on) { if (test_on != !!on) fprintf(stderr, "[INPUT] test switch %s\n", on ? "ON" : "OFF"); test_on = !!on; }

void s21_input_neutral(void)
{
    s21_inputs in = cur; in.dsw = (uint8_t)((cur.dsw & ~1) | (test_on ? 0 : 1));
    s21_debug_frame(&in);
    for (int i = 0; i < 4; i++) axis[i] = 0x7F;
    s21_io_set_inputs(&in);
}

/* the Controls page: every control (twin_stick.c), the axis deadzone, the Test switch, the wheel and pedals, a reset */
enum { P_DEADZONE = CS_NCTL, P_TEST, P_WHEEL, P_GAS, P_BRAKE, P_RESET, P_N };
static int nrows(void) { return P_N; }
static bool has_value(int r) { return r != P_RESET; }
static bool capture_cb(const SDL_Event *e, void *u) { (void)u; return ts_capture_event(e); }
static void text(int r, char *l, size_t ln, char *v, size_t vn)
{
    if (r < CS_NCTL) { snprintf(l, ln, "%s", ts_label(r)); ts_value_text(r, v, vn); return; }
    switch (r) {
    case P_DEADZONE: snprintf(l, ln, "Stick deadzone"); snprintf(v, vn, "%d%%", ts_deadzone()); break;
    case P_TEST: snprintf(l, ln, "Test switch"); snprintf(v, vn, "%s (F2)", test_on ? "ON" : "OFF"); break;
    case P_RESET: snprintf(l, ln, "Reset all controls"); snprintf(v, vn, "Enter: every row above back to its defaults"); break;
    default: {
        const int w = r - P_WHEEL;
        const rawbind *b = w == 0 ? &b_turn : w == 1 ? &b_gas : &b_brake;
        snprintf(l, ln, "%s", w == 0 ? "Wheel (turn)" : w == 1 ? "Gas pedal" : "Brake pedal");
        if (learn == w) snprintf(v, vn, "%s", w == 0 ? "turn the wheel RIGHT ..." : "press the pedal ...");
        else if (b->axis < 0) snprintf(v, vn, "not bound (Enter to bind)");
        else snprintf(v, vn, "axis %d%s (Enter to rebind, Left clears)", b->axis, b->invert ? " inverted" : "");
    } }
}
static void change(int r, int dir)
{
    if (r < CS_NCTL) {
        ts_change(r, dir);
        if (dir == 0) eng_ui_capture_input(capture_cb, NULL);
        return;
    }
    switch (r) {
    case P_DEADZONE: { int v = ts_deadzone() + (dir ? dir : 1) * 5; if (!dir && v > 50) v = 0; ts_set_deadzone(v); break; }   /* Enter cycles */
    case P_TEST: test_on = !test_on; fprintf(stderr, "[INPUT] test switch %s\n", test_on ? "ON" : "OFF"); break;
    case P_RESET: if (dir == 0) for (int c = 0; c < CS_NCTL; c++) ts_change(c, +1); break;
    default: {
        const int w = r - P_WHEEL;
        rawbind *b = w == 0 ? &b_turn : w == 1 ? &b_gas : &b_brake;
        if (dir < 0) { b->axis = w == 0 ? 0 : -1; b->guid[0] = 0; b->invert = b->dir = b->rest = 0; bind_save(w == 0 ? "joy_turn" : w == 1 ? "joy_gas" : "joy_brake", b); learn = -1; }
        else if (dir == 0) learn_begin(w);
    } }
}
static void notes(void (*line)(const char *fmt, ...))
{
    line("Enter on a row, then press a key or button, push a hat or move a stick (Esc cancels).");
    line("A key replaces the row's keys, a pad input its pad inputs; a joystick fills one of two joystick slots.");
    line("Left clears a row, Right puts its defaults back. Two joysticks of the same model are told apart (#2).");
}
static const eng_ui_page page = { "Controls", 640, 170, 20, nrows, has_value, NULL, text, change, notes };
const eng_ui_page *s21_input_page(void) { return &page; }
