/* twin_stick.c -- see twin_stick.h. Shared by Cyber Sled (cybsled/src/host/s21_input.c) and Cyber Commando (raverace/src/rr_host.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "twin_stick.h"

enum { S_NONE, S_KEY, S_PBTN, S_PAXIS, S_JBTN, S_JHAT, S_JAXIS };
typedef struct {
    int type, idx, sign, rest;           /* sign: an axis's direction (+1 / -1), a hat's direction mask */
    int ord;                             /* a joystick's: the first (0) or second (1) ... connected device with this GUID */
    char guid[40];
} src_t;
#define NKEY 2
#define NPAD 2
#define NJOY 2
#define NSRC (NKEY + NPAD + NJOY)
typedef struct { src_t s[NSRC]; } ctl_t;                  /* s[0..1] keys, s[2..3] gamepad, s[4..5] joysticks */
#define KEYS(k) (&(k)->s[0])
#define PADS(k) (&(k)->s[NKEY])
#define JOYS(k) (&(k)->s[NKEY + NPAD])

#define MAXC 32
#define MAXD 16
#define MAXA 16
#define MAXB 64
#define MAXH 4
static const ts_def *defs;
static int nctl;
static ctl_t ctl[MAXC];
static void (*save_fn)(const char *, const char *);
static int deadzone = 15;                /* percent; a worn pad rests near 10 % */
static int capture = -1;

static struct {
    SDL_Joystick *js; SDL_GameController *gc; SDL_JoystickID id; char guid[40];
    int base[MAXA]; bool held[MAXB]; Uint8 hat[MAXH];              /* where everything was when a binding began */
} dv[MAXD];

/* ---- devices ------------------------------------------------------------------------------------------------------------------- */
static int dev_find(SDL_JoystickID id) { for (int d = 0; d < MAXD; d++) if (dv[d].js && dv[d].id == id) return d; return -1; }
static void dev_add(int index)
{
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    if (id < 0 || dev_find(id) >= 0) return;
    for (int d = 0; d < MAXD; d++) if (!dv[d].js) {
        if (!(dv[d].js = SDL_JoystickOpen(index))) return;
        dv[d].gc = SDL_IsGameController(index) ? SDL_GameControllerOpen(index) : NULL;
        dv[d].id = id;
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(dv[d].js), dv[d].guid, sizeof dv[d].guid);
        fprintf(stderr, "[INPUT] %s %d: %s (%d axes, %d buttons, %d hats)\n", dv[d].gc ? "gamepad" : "joystick", d, SDL_JoystickName(dv[d].js),
                SDL_JoystickNumAxes(dv[d].js), SDL_JoystickNumButtons(dv[d].js), SDL_JoystickNumHats(dv[d].js));
        return;
    }
}
static void dev_remove(SDL_JoystickID id)
{
    const int d = dev_find(id);
    if (d < 0) return;
    if (dv[d].gc) SDL_GameControllerClose(dv[d].gc);
    SDL_JoystickClose(dv[d].js);
    memset(&dv[d], 0, sizeof dv[d]);
}
/* two sticks of one model share a GUID: they are told apart by the order they were connected in (SDL's instance ids rise) */
static int dev_ord(int d)
{
    int n = 0;
    for (int j = 0; j < MAXD; j++) if (dv[j].js && j != d && !strcmp(dv[j].guid, dv[d].guid) && dv[j].id < dv[d].id) n++;
    return n;
}
static int dev_of(const src_t *s)
{
    for (int d = 0; d < MAXD; d++) if (dv[d].js && !dv[d].gc && !strcmp(dv[d].guid, s->guid) && dev_ord(d) == s->ord) return d;
    return -1;
}
void ts_event(const SDL_Event *e)
{
    if (e->type == SDL_JOYDEVICEADDED) dev_add(e->jdevice.which);
    else if (e->type == SDL_JOYDEVICEREMOVED) dev_remove(e->jdevice.which);
}

/* ---- sources: text, defaults, the config line ---------------------------------------------------------------------------------- */
static const char *pad_axis_name(int a) { return SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)a); }
static bool parse_pad(const char *t, src_t *s)           /* "a", "dpup", "-lefty", "+righttrigger" */
{
    memset(s, 0, sizeof *s);
    if (*t == '+' || *t == '-') {
        const SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(t + 1);
        if (a == SDL_CONTROLLER_AXIS_INVALID) return false;
        s->type = S_PAXIS; s->idx = a; s->sign = *t == '+' ? 1 : -1;
        return true;
    }
    const SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(t);
    if (b == SDL_CONTROLLER_BUTTON_INVALID) return false;
    s->type = S_PBTN; s->idx = b;
    return true;
}
static char *trim(char *s) { while (*s == ' ' || *s == '\t') s++; char *e = s + strlen(s); while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0; return s; }
static void set_defaults(int c)
{
    ctl_t *k = &ctl[c]; char tmp[128]; int n;
    memset(k, 0, sizeof *k);
    snprintf(tmp, sizeof tmp, "%s", defs[c].key ? defs[c].key : ""); n = 0;
    for (char *t = strtok(tmp, ","); t && n < NKEY; t = strtok(NULL, ",")) {
        const SDL_Scancode sc = SDL_GetScancodeFromName(trim(t));
        if (sc != SDL_SCANCODE_UNKNOWN) { KEYS(k)[n].type = S_KEY; KEYS(k)[n++].idx = sc; }
    }
    snprintf(tmp, sizeof tmp, "%s", defs[c].pad ? defs[c].pad : ""); n = 0;
    for (char *t = strtok(tmp, ","); t && n < NPAD; t = strtok(NULL, ",")) if (parse_pad(trim(t), &PADS(k)[n])) n++;
}
/* one source as config text: kn:<scancode> | p:<pad button or +/-axis> | jb:<button> | jh:<hat>.<mask> | ja:<+/-axis>.<rest>, a
 * joystick's followed by @<ord>@<guid>. No '#' (the configs' comment) and no ';' (the list separator). */
static int src_text(const src_t *s, char *o, size_t n)
{
    switch (s->type) {
    case S_KEY:   return snprintf(o, n, "kn:%d", s->idx);
    case S_PBTN:  return snprintf(o, n, "p:%s", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)s->idx));
    case S_PAXIS: return snprintf(o, n, "p:%c%s", s->sign > 0 ? '+' : '-', pad_axis_name(s->idx));
    case S_JBTN:  return snprintf(o, n, "jb:%d@%d@%s", s->idx, s->ord, s->guid);
    case S_JHAT:  return snprintf(o, n, "jh:%d.%d@%d@%s", s->idx, s->sign, s->ord, s->guid);
    case S_JAXIS: return snprintf(o, n, "ja:%c%d.%d@%d@%s", s->sign > 0 ? '+' : '-', s->idx, s->rest, s->ord, s->guid);
    }
    return 0;
}
static bool src_parse(char *t, src_t *s)
{
    memset(s, 0, sizeof *s);
    char *at = strchr(t, '@');
    if (at) { *at++ = 0; char *g = strchr(at, '@'); if (!g) return false; *g++ = 0; s->ord = atoi(at); snprintf(s->guid, sizeof s->guid, "%s", g); }
    if (!strncmp(t, "kn:", 3)) { s->type = S_KEY; s->idx = atoi(t + 3); return s->idx > 0 && s->idx < SDL_NUM_SCANCODES; }
    if (!strncmp(t, "p:", 2)) return parse_pad(t + 2, s);
    if (!at) return false;
    if (!strncmp(t, "jb:", 3)) { s->type = S_JBTN; s->idx = atoi(t + 3); return s->idx >= 0 && s->idx < MAXB; }
    if (!strncmp(t, "jh:", 3)) { s->type = S_JHAT; return sscanf(t + 3, "%d.%d", &s->idx, &s->sign) == 2 && s->idx >= 0 && s->idx < MAXH; }
    if (!strncmp(t, "ja:", 3)) {
        s->type = S_JAXIS; s->sign = t[3] == '-' ? -1 : 1;
        return sscanf(t + 4, "%d.%d", &s->idx, &s->rest) == 2 && s->idx >= 0 && s->idx < MAXA;
    }
    return false;
}
static void save(int c)
{
    if (!save_fn) return;
    char key[64], v[512] = ""; size_t n = 0;
    snprintf(key, sizeof key, "ts_%s", defs[c].name);
    const src_t *all = ctl[c].s;
    for (int i = 0; i < NSRC; i++) if (all[i].type != S_NONE && n < sizeof v - 96) {
        if (n) v[n++] = ';';
        n += (size_t)src_text(&all[i], v + n, sizeof v - n);
    }
    save_fn(key, n ? v : "none");
}
static void load(int c, const char *val)
{
    set_defaults(c);
    if (!val || !*val) return;                           /* empty: the defaults */
    ctl_t *k = &ctl[c]; char tmp[512];
    memset(k, 0, sizeof *k);
    if (!strcmp(val, "none")) return;
    snprintf(tmp, sizeof tmp, "%s", val);
    int nk = 0, np = 0, nj = 0;
    for (char *t = strtok(tmp, ";"); t; t = strtok(NULL, ";")) {
        src_t s;
        if (!src_parse(trim(t), &s)) { fprintf(stderr, "[INPUT] ts_%s: cannot read '%s'\n", defs[c].name, t); continue; }
        if (s.type == S_KEY) { if (nk < NKEY) KEYS(k)[nk++] = s; }
        else if (s.type == S_PBTN || s.type == S_PAXIS) { if (np < NPAD) PADS(k)[np++] = s; }
        else if (nj < NJOY) JOYS(k)[nj++] = s;
    }
}

void ts_init(const ts_def *d, int n, void (*save_cb)(const char *, const char *))
{
    defs = d; nctl = n > MAXC ? MAXC : n; save_fn = save_cb; capture = -1;
    for (int c = 0; c < nctl; c++) set_defaults(c);
    SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
    for (int i = 0; i < SDL_NumJoysticks(); i++) dev_add(i);
}
bool ts_load_line(const char *key, const char *val)
{
    if (strncmp(key, "ts_", 3)) return false;
    if (!strcmp(key, "ts_deadzone")) { int v = atoi(val); deadzone = v < 0 ? 0 : v > 50 ? 50 : v; return true; }
    for (int c = 0; c < nctl; c++) if (!strcmp(key + 3, defs[c].name)) { load(c, val); return true; }
    return false;
}
void ts_load_from(const char *(*get)(const char *))
{
    char key[64];
    for (int c = 0; c < nctl; c++) { snprintf(key, sizeof key, "ts_%s", defs[c].name); load(c, get(key)); }
    const char *v = get("ts_deadzone");
    if (v && *v) ts_load_line("ts_deadzone", v);
}

/* ---- the state ------------------------------------------------------------------------------------------------------------------ */
static double axis_frac(int v, int rest, int sign)       /* 0..1 past the deadzone, the way `sign` points from `rest` */
{
    const int range = sign > 0 ? 32767 - rest : rest + 32768, d = (v - rest) * sign;
    if (range <= 0 || d <= 0) return 0;
    double f = (double)d / range;
    const double dz = deadzone / 100.0;
    f = f > dz ? (f - dz) / (1 - dz) : 0;
    return f > 1 ? 1 : f;
}
static double src_analog(const src_t *s)
{
    double best = 0;
    if (s->type == S_PAXIS) {
        for (int d = 0; d < MAXD; d++) if (dv[d].gc) {
            const double f = axis_frac(SDL_GameControllerGetAxis(dv[d].gc, (SDL_GameControllerAxis)s->idx), 0, s->sign);
            if (f > best) best = f;
        }
    } else if (s->type == S_JAXIS) {
        const int d = dev_of(s);
        if (d >= 0 && s->idx < SDL_JoystickNumAxes(dv[d].js)) best = axis_frac(SDL_JoystickGetAxis(dv[d].js, s->idx), s->rest, s->sign);
    }
    return best;
}
static bool keys_on = true;
static bool src_digital(const src_t *s)
{
    switch (s->type) {
    case S_KEY: { int n; const Uint8 *k = SDL_GetKeyboardState(&n); return s->idx < n && k[s->idx]; }
    case S_PBTN:
        for (int d = 0; d < MAXD; d++) if (dv[d].gc && SDL_GameControllerGetButton(dv[d].gc, (SDL_GameControllerButton)s->idx)) return true;
        return false;
    case S_JBTN: { const int d = dev_of(s); return d >= 0 && s->idx < SDL_JoystickNumButtons(dv[d].js) && SDL_JoystickGetButton(dv[d].js, s->idx); }
    case S_JHAT: { const int d = dev_of(s); return d >= 0 && s->idx < SDL_JoystickNumHats(dv[d].js) && (SDL_JoystickGetHat(dv[d].js, s->idx) & s->sign); }
    }
    return false;
}
double ts_analog(int c)
{
    if (c < 0 || c >= nctl) return 0;
    double best = 0;
    const src_t *all = ctl[c].s;
    for (int i = 0; i < NSRC; i++) { const double f = src_analog(&all[i]); if (f > best) best = f; }
    return best;
}
bool ts_digital(int c)
{
    if (c < 0 || c >= nctl) return false;
    const src_t *all = ctl[c].s;
    for (int i = 0; i < NSRC; i++) if ((keys_on || all[i].type != S_KEY) && src_digital(&all[i])) return true;
    return false;
}
void ts_keys(bool on) { keys_on = on; }
bool ts_uses_joystick(SDL_Joystick *js)
{
    const SDL_JoystickID id = js ? SDL_JoystickInstanceID(js) : -1;
    for (int c = 0; c < nctl; c++) for (int i = 0; i < NJOY; i++) {
        const src_t *s = &JOYS(&ctl[c])[i];
        if (s->type == S_JAXIS) { const int d = dev_of(s); if (d >= 0 && dv[d].id == id) return true; }
    }
    return false;
}
bool ts_held(int c) { return ts_digital(c) || ts_analog(c) > 0.5; }
double ts_lever(int neg, int pos, bool *digital)
{
    const int d = (ts_digital(pos) ? 1 : 0) - (ts_digital(neg) ? 1 : 0);
    *digital = d != 0;
    if (d) return d;
    return ts_analog(pos) - ts_analog(neg);
}

/* ---- the Controls page ---------------------------------------------------------------------------------------------------------- */
int ts_count(void) { return nctl; }
const char *ts_label(int c) { return c >= 0 && c < nctl ? defs[c].label : "?"; }
int ts_capturing(void) { return capture; }
int ts_deadzone(void) { return deadzone; }
void ts_set_deadzone(int pct)
{
    deadzone = pct < 0 ? 0 : pct > 50 ? 50 : pct;
    if (save_fn) { char v[16]; snprintf(v, sizeof v, "%d", deadzone); save_fn("ts_deadzone", v); }
}

static const char *pad_btn_text(int b)
{
    static const char *const n[SDL_CONTROLLER_BUTTON_MAX] = { "A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right" };
    return b >= 0 && b < SDL_CONTROLLER_BUTTON_MAX && n[b] ? n[b] : SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
}
static void pad_axis_text(int a, int sign, char *o, size_t n)
{
    switch (a) {
    case SDL_CONTROLLER_AXIS_LEFTX:  snprintf(o, n, "LS %s", sign < 0 ? "left" : "right"); break;
    case SDL_CONTROLLER_AXIS_LEFTY:  snprintf(o, n, "LS %s", sign < 0 ? "up" : "down"); break;
    case SDL_CONTROLLER_AXIS_RIGHTX: snprintf(o, n, "RS %s", sign < 0 ? "left" : "right"); break;
    case SDL_CONTROLLER_AXIS_RIGHTY: snprintf(o, n, "RS %s", sign < 0 ? "up" : "down"); break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  snprintf(o, n, "LT"); break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: snprintf(o, n, "RT"); break;
    default: snprintf(o, n, "%c%s", sign > 0 ? '+' : '-', pad_axis_name(a)); break;
    }
}
static void joy_text(const src_t *s, char *o, size_t n)
{
    const int d = dev_of(s);
    char dn[24];
    if (d < 0) snprintf(dn, sizeof dn, "(unplugged)");
    else { snprintf(dn, sizeof dn, "%.14s", SDL_JoystickName(dv[d].js) ? SDL_JoystickName(dv[d].js) : "Joystick"); }
    char sfx[16] = ""; if (s->ord) snprintf(sfx, sizeof sfx, " #%d", s->ord + 1);
    if (s->type == S_JBTN) snprintf(o, n, "%s%s btn %d", dn, sfx, s->idx);
    else if (s->type == S_JHAT) snprintf(o, n, "%s%s hat %s", dn, sfx, s->sign == SDL_HAT_UP ? "up" : s->sign == SDL_HAT_DOWN ? "down" : s->sign == SDL_HAT_LEFT ? "left" : "right");
    else snprintf(o, n, "%s%s axis %d%c", dn, sfx, s->idx, s->sign > 0 ? '+' : '-');
}
void ts_value_text(int c, char *v, size_t vn)
{
    if (c < 0 || c >= nctl || !vn) return;
    if (capture == c) { snprintf(v, vn, "press a key / button or move a stick ... (Esc cancels)"); return; }
    size_t n = 0; v[0] = 0;
    const src_t *all = ctl[c].s;
    for (int i = 0; i < NSRC && n + 4 < vn; i++) {
        const src_t *s = &all[i];
        char t[64];
        if (s->type == S_NONE) continue;
        if (s->type == S_KEY) snprintf(t, sizeof t, "%s", SDL_GetScancodeName((SDL_Scancode)s->idx));
        else if (s->type == S_PBTN) snprintf(t, sizeof t, "Pad %s", pad_btn_text(s->idx));
        else if (s->type == S_PAXIS) { char a[24]; pad_axis_text(s->idx, s->sign, a, sizeof a); snprintf(t, sizeof t, "Pad %s", a); }
        else joy_text(s, t, sizeof t);
        n += (size_t)snprintf(v + n, vn - n, "%s%s", n ? " / " : "", t);
        if (n >= vn) n = vn - 1;
    }
    if (!n) snprintf(v, vn, "(none)  Enter to bind");
}

static void capture_snapshot(void)
{
    for (int d = 0; d < MAXD; d++) if (dv[d].js) {
        for (int a = 0; a < MAXA; a++)
            dv[d].base[a] = dv[d].gc ? (a < SDL_CONTROLLER_AXIS_MAX ? SDL_GameControllerGetAxis(dv[d].gc, (SDL_GameControllerAxis)a) : 0)
                                     : (a < SDL_JoystickNumAxes(dv[d].js) ? SDL_JoystickGetAxis(dv[d].js, a) : 0);
        for (int b = 0; b < MAXB; b++)
            dv[d].held[b] = dv[d].gc ? (b < SDL_CONTROLLER_BUTTON_MAX && SDL_GameControllerGetButton(dv[d].gc, (SDL_GameControllerButton)b))
                                     : (b < SDL_JoystickNumButtons(dv[d].js) && SDL_JoystickGetButton(dv[d].js, b));
        for (int h = 0; h < MAXH; h++) dv[d].hat[h] = h < SDL_JoystickNumHats(dv[d].js) ? SDL_JoystickGetHat(dv[d].js, h) : 0;
    }
}
void ts_change(int c, int dir)
{
    if (c < 0 || c >= nctl) return;
    capture = -1;
    if (dir < 0) { memset(&ctl[c], 0, sizeof ctl[c]); save(c); fprintf(stderr, "[INPUT] %s: cleared\n", defs[c].label); return; }
    if (dir > 0) { set_defaults(c); if (save_fn) { char key[64]; snprintf(key, sizeof key, "ts_%s", defs[c].name); save_fn(key, ""); } return; }
    SDL_JoystickUpdate(); SDL_GameControllerUpdate();
    capture_snapshot();
    capture = c;
    fprintf(stderr, "[INPUT] binding %s: press a key or button, or move a stick\n", defs[c].label);
}
static void bound(const src_t *s)
{
    ctl_t *k = &ctl[capture];
    if (s->type == S_KEY) { memset(KEYS(k), 0, NKEY * sizeof *s); KEYS(k)[0] = *s; }
    else if (s->type == S_PBTN || s->type == S_PAXIS) { memset(PADS(k), 0, NPAD * sizeof *s); PADS(k)[0] = *s; }
    else {                                               /* the slot of the same joystick, else the first free one, else the second */
        src_t *j = JOYS(k); int slot = -1;
        for (int i = 0; i < NJOY && slot < 0; i++) if (j[i].type != S_NONE && j[i].ord == s->ord && !strcmp(j[i].guid, s->guid)) slot = i;
        for (int i = 0; i < NJOY && slot < 0; i++) if (j[i].type == S_NONE) slot = i;
        j[slot < 0 ? NJOY - 1 : slot] = *s;
    }
    const int c = capture; char t[128];
    capture = -1;
    save(c); ts_value_text(c, t, sizeof t);
    fprintf(stderr, "[INPUT] %s = %s\n", defs[c].label, t);
}
bool ts_capture_event(const SDL_Event *e)
{
    if (capture < 0) return true;
    if (!e) { capture = -1; return true; }
    src_t s; memset(&s, 0, sizeof s);
    int d;
    switch (e->type) {
    case SDL_KEYDOWN:
        if (e->key.repeat) return false;
        if (e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) { capture = -1; return true; }
        s.type = S_KEY; s.idx = e->key.keysym.scancode; bound(&s); return true;
    case SDL_CONTROLLERBUTTONDOWN:
        if ((d = dev_find(e->cbutton.which)) < 0 || !dv[d].gc || dv[d].held[e->cbutton.button]) return false;   /* the press that opened the row */
        s.type = S_PBTN; s.idx = e->cbutton.button; bound(&s); return true;
    case SDL_CONTROLLERBUTTONUP:
        if ((d = dev_find(e->cbutton.which)) >= 0) dv[d].held[e->cbutton.button] = false;
        return false;
    case SDL_CONTROLLERAXISMOTION: {
        if ((d = dev_find(e->caxis.which)) < 0 || !dv[d].gc || e->caxis.axis >= MAXA) return false;
        const int delta = e->caxis.value - dv[d].base[e->caxis.axis];
        if (abs(delta) < 16000) return false;
        s.type = S_PAXIS; s.idx = e->caxis.axis; s.sign = delta > 0 ? 1 : -1; bound(&s); return true; }
    case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: case SDL_JOYHATMOTION: case SDL_JOYAXISMOTION: break;
    default: return false;
    }
    /* a joystick SDL has no gamepad mapping for (a gamepad's own joystick events are left to the gamepad ones above) */
    const SDL_JoystickID which = e->type == SDL_JOYAXISMOTION ? e->jaxis.which : e->type == SDL_JOYHATMOTION ? e->jhat.which : e->jbutton.which;
    if ((d = dev_find(which)) < 0 || dv[d].gc) return false;
    s.ord = dev_ord(d); snprintf(s.guid, sizeof s.guid, "%s", dv[d].guid);
    if (e->type == SDL_JOYBUTTONUP) { if (e->jbutton.button < MAXB) dv[d].held[e->jbutton.button] = false; return false; }
    if (e->type == SDL_JOYBUTTONDOWN) {
        if (e->jbutton.button >= MAXB || dv[d].held[e->jbutton.button]) return false;
        s.type = S_JBTN; s.idx = e->jbutton.button;
    } else if (e->type == SDL_JOYHATMOTION) {
        const Uint8 v = e->jhat.value;
        if (e->jhat.hat >= MAXH || (v != SDL_HAT_UP && v != SDL_HAT_DOWN && v != SDL_HAT_LEFT && v != SDL_HAT_RIGHT) || v == dv[d].hat[e->jhat.hat]) return false;
        s.type = S_JHAT; s.idx = e->jhat.hat; s.sign = v;
    } else {
        if (e->jaxis.axis >= MAXA) return false;
        const int rest = dv[d].base[e->jaxis.axis], delta = e->jaxis.value - rest;
        if (abs(delta) < 16000) return false;
        s.type = S_JAXIS; s.idx = e->jaxis.axis; s.sign = delta > 0 ? 1 : -1;
        s.rest = abs(rest) < 4096 ? 0 : rest;            /* a centred stick rests at 0 (its noise is not its centre); a throttle at its end */
    }
    bound(&s);
    return true;
}
