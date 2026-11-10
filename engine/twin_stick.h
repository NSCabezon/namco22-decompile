/*
 * twin_stick.h -- the twin-stick cabinets (Cyber Sled, Cyber Commando): every direction of both sticks and every button bindable,
 * on the Controls page, to a key, a gamepad button or stick, and any joystick's axis, hat or button. Two joysticks of the SAME
 * model share one SDL GUID, so a binding also records which of them it was (the first or second of that GUID, in the order they
 * were connected) -- the left stick stays the left stick.
 *
 * A control holds a key, a gamepad input and two joystick inputs (a trigger on each stick: the cabinets wire both to one bit).
 * The Controls page: Enter on a row, then press a key / button, push a hat or move an axis (Esc cancels). A key replaces the key,
 * a gamepad input the gamepad input, a joystick input the slot of that same joystick or the first free one. Left clears the
 * row, Right puts its defaults back. Saved as ts_<name> = <key>|<pad>|<joy>|<joy> lines through the game's own config.
 */
#ifndef TWIN_STICK_H
#define TWIN_STICK_H
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stddef.h>

/* the eight directions come first in every game's table, in this order */
enum { TS_L_UP, TS_L_DOWN, TS_L_LEFT, TS_L_RIGHT, TS_R_UP, TS_R_DOWN, TS_R_LEFT, TS_R_RIGHT, TS_NDIR };

typedef struct {
    const char *label;                   /* the Controls page's name */
    const char *name;                    /* the config key: ts_<name> */
    const char *key;                     /* default key (SDL key name), NULL = none */
    const char *pad;                     /* default gamepad input: a button name ("a", "dpup") or a stick half ("-lefty", "+righttrigger") */
} ts_def;

/* defs: the game's controls (the eight directions first); save: writes one config line (key, value; value "" = forget it) */
void ts_init(const ts_def *defs, int n, void (*save)(const char *key, const char *val));
bool ts_load_line(const char *key, const char *val);     /* a ts_<name> line from the config; false = not ours */
void ts_load_from(const char *(*get)(const char *key));  /* or: read every control through get (engine/eng_cfg) */
void ts_event(const SDL_Event *e);                       /* every event, before the menu: devices coming and going */

/* the state, read once a frame after SDL has pumped */
double ts_analog(int c);                 /* 0..1: how far an AXIS bound to control c is pushed its way (0 = none, or not an axis) */
bool   ts_digital(int c);                /* a key, button or hat bound to control c is down */
bool   ts_held(int c);                   /* either, past half way */
void   ts_keys(bool on);                 /* false: keys are not read (a chat box has the keyboard) */
bool   ts_uses_joystick(SDL_Joystick *js);   /* an axis of js is bound to a stick: a game's "any joystick's axis 0 is a wheel" default leaves it */
/* one lever of a stick: -1 (up / left) .. +1 (down / right). *digital = a key/button/hat drives it (the caller ramps those as MAME's
 * PORT_KEYDELTA); 0 with *digital false = nothing is touching it */
double ts_lever(int neg, int pos, bool *digital);

/* the Controls page */
int  ts_count(void);
const char *ts_label(int c);
void ts_value_text(int c, char *v, size_t vn);
void ts_change(int c, int dir);          /* 0 = bind (the next input), -1 = clear, +1 = defaults */
bool ts_capture_event(const SDL_Event *e);   /* while binding: true = done (bound or cancelled); NULL e cancels */
int  ts_capturing(void);                 /* the control waiting for an input, or -1 */
int  ts_deadzone(void);                  /* joystick / pad axis deadzone, percent */
void ts_set_deadzone(int pct);
#endif
