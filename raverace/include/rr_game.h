/* rr_game.h -- what differs between the System 22 games this runtime hosts (HARD RULE 3: one runtime, tweaks per game).
 * Rave Racer's table is the default (src/rr_game.c); another game (acedriver/) supplies its own table instead of that file. */
#ifndef RR_GAME_H
#define RR_GAME_H
#include <stddef.h>
#include <stdint.h>
#include "romzip.h"

typedef struct { const char *file; uint32_t at; } rr_wave_t;
/* A developer screen left in the program (Ace Driver's object / sprite / character viewers). The program runs its modes as
 * coroutines: the main loop resumes the mode task each frame through the address saved in its slot, the mode code yields back
 * (Ace Driver 0x4C48 / 0x4C6E; its own setter 0x4C36 writes the slot). The menu's Debug page launches a screen the same way:
 * at a frame boundary in the MAIN context (the task stack empty) it writes `entry` into the mode task's slot and turns the Test
 * switch ON (these screens loop while it is on); the main loop then starts the screen, and turning the Test switch OFF is the
 * screen's own exit back into the game (rr_main.c rr_debug_launch). */
typedef struct { const char *name; uint32_t entry; const char *help; } rr_debug_t;
#define DEBUG_DSW_LEAVE 0xFFFFFFFFu

typedef struct {
    const char *name;                       /* "raverace", "acedrive" ... */
    const char *title;                      /* the window title and message boxes: "Rave Racer", "Ace Driver" */
    const char *cfg_file;                   /* the controls / menu settings file beside the binary (rr_controls.cfg, ad_controls.cfg) */
    const char *nv_file;                    /* the persisted EEPROM of a windowed session (rr_eeprom.nv, ad_eeprom.nv): never shared between games */
    int autosetup;                          /* 1: src/rr_romzip.c unpacks this game's zips on first run (its chip table is Rave Racer's); 0: the ROM files must be in extracted/ */
    void (*entry)(void);                    /* the lifted reset routine (the 68020 program's reset PC): never returns */
    const char *prg[4];                     /* the four 68020 program chips: MAME ROM_LOAD32_BYTE offsets 0..3 (uu, um, lm, ll) */
    const char *cg[8];                      /* texture chips, 2 MB each at slot i (NULL = the slot is empty: Ace Driver has slots 4..7 only) */
    const char *ccrl, *ccrh;                /* texture tilemap + attributes */
    const char *pot[3][4];                  /* point ROM: low / mid / upper byte planes, pot_chips chips of 512 KB each (NULL past that) */
    int pot_chips;                          /* 4 (Rave Racer), 2 (Ace Driver), 3 (Victory Lap) */
    const char *snd_data;                   /* the sound MCU's program + data ROM */
    rr_wave_t wav[4];                       /* C352 wave ROMs and where each sits */
    const char *gamma[3];                   /* gamma PROMs */
    const char *eeprom;                     /* default EEPROM image, or NULL (blank: the game initialises it) */
    int keycus_off; uint16_t keycus_val;    /* MAME namcos22_keycus_r: this word offset returns keycus_val, every other read is random; -1 = all random */
    int steer_add, gas_add, brake_add;      /* MAME handle_driving_io: the offsets added to the axes in shared RAM 0x32..0x36 */
    uint16_t inputs_idle;                   /* the INPUTS word with nothing pressed (active-low bits 1; Rave Racer's cabinet field 0x2100 = Standard -> 0xFEFF) */
    int steer_min, steer_max;               /* MAME ADC.0 PORT_MINMAX (centre 0x800) */
    int gas_max, brake_max;                 /* MAME ADC.1 / ADC.2 PORT_MINMAX upper bound */
    int gas_delta, brake_delta;             /* MAME PORT_KEYDELTA of the pedals (ADC counts per frame from a key) */
    int tex_fixup;                          /* 1: MAME's System 22 texture tilemap fix-up (eng_texture_tilemap_sys22_fixup): Ace Driver, Ridge Racer */
    uint16_t spin_pc, spin_op;              /* the master DSP program's busy-wait poll (0 = none: nothing is fast-forwarded) */
    const rr_debug_t *debug; int ndebug;    /* developer screens for the Debug menu page (none: the page is hidden) */
    uint32_t task_sp, task_sp_empty;        /* where the coroutine task stack pointer lives, and its value when empty (main context) */
    uint32_t task_slot;                     /* the mode task's resume slot the Debug page writes */
    /* A DEBUG MENU BEHIND A DIP SWITCH (Ridge Racer 2): debug_dsw != 0 means the game enters its own developer menu while these DIP
     * bits are ON (cleared: active low), and the Debug page works through them instead of the task slot. rr_debug_t.entry is then
     * 0 = open the menu, DEBUG_DSW_LEAVE = the DIP switch off again (back to the game), anything else = the state value the menu's
     * own selection writes: once the mode word reads debug_mode and the state word debug_menu_state (the menu waiting for a pick),
     * entry goes into the state word and (entry - 1) / 2 into the cursor word, as a press of gas on that item does. */
    uint32_t debug_dsw;
    uint32_t debug_mode_addr, debug_state_addr, debug_cursor_addr;   /* 16-bit work-RAM words */
    uint16_t debug_mode, debug_menu_state;
    /* A player's first run: the chips taken out of these MAME zips by the ENGINE's table-driven unpacker (engine/romzip.c) when
     * the ROM folder is incomplete. NULL roms = Rave Racer's own src/rr_romzip.c (autosetup). */
    const char *const *zips; int nzips;
    const eng_rom_t *roms; int nroms;
    const char *log_file;                   /* Windows: stdout/stderr beside the .exe (raveracer.log, acedriver.log) */
    /* TWIN STICKS instead of wheel / gas / brake (Cyber Commando): the I/O answer is MAME's handle_cybrcomm_io -- the four 8-bit stick
     * ADCs x 0x10 at shared RAM 0x32 / 0x34 / 0x36 / 0x38 (right Y, left Y, right X, left X; centre 0x7F, range 0x47..0xB7, forward =
     * low). Keys drive both sticks as "tank" controls through the existing actions (gas = forward, brake = back, steer = turn: the
     * sticks pushed opposite ways); a gamepad's two sticks are the cabinet's two sticks. 0 = the driving model (every other game). */
    int twin_stick;
    /* WIDESCREEN: the game's full-screen effects (a camera-facing quad sized to the 640 x 480 screen, e.g. Cyber Commando's hit flash)
     * reach the wide picture's edges (rr_gl.c wide_screen_quad). 0 = off (every other game). */
    int wide_screen_quads;
    /* WIDESCREEN HUD: text cells {row, col, tile} that hold that tile only while the race HUD is up (any one showing = up); the HUD
     * then moves to the picture's sides (engine/hud_edges.h). NULL = Rave Racer's gear-ladder marks (rr_gl.c). */
    const uint16_t (*hud_marks)[3]; int n_hud_marks;
    /* FORCE FEEDBACK FOR A CABINET WITHOUT A WHEEL MOTOR (Ridge Racer 1/2: MAME copies no drive byte for them): the force, -1..1
     * (negative pushes toward the wheel's higher A-D side, eng_ffb.h's sign), built from the game's state once per host frame.
     * NULL = play the motor byte the game leaves for the I/O board (every other game). */
    double (*ffb_synth)(void);
    /* THE C139 LINK (src/rr_link.c), when it differs from Rave Racer's: link_payload = payload words per frame (0 = 0x26; the frame
     * is payload + a length word + a checksum word); link_tx_reg6 = the sender programs reg 0xE (ring position) and reg 0xA (size)
     * between reg 6 = 1 and reg 6 = 0 (Ridge Racer 2, 0x1BF38) instead of toggling reg 4 1 -> 3; link_id_addr = the byte holding this
     * cabinet's link number (0 = Rave Racer's settings byte 0x10001060). */
    int link_payload, link_tx_reg6;
    /* link_ring: the game's link is a RING (Ridge Racer 2, 0x1C17E): a cabinet counts itself linked only while its OWN frame comes
     * back round, and it forwards every frame it receives. Online (rr_net.c) a cabinet then sends only its own frames -- the server
     * already gives every peer the originals -- and echoes its own frame into its own receive ring, which closes the ring. */
    int link_ring;
    uint32_t link_id_addr;
    /* THE GAME'S OWN SETTINGS, when they are not Rave Racer's (EEPROM settings groups 2 / 3, rr_hw.c): free play on/off and this
     * cabinet's link number (with whatever latches the game copies it into at boot). NULL = Rave Racer's. */
    void (*freeplay_set)(bool on); bool (*freeplay_get)(void);
    void (*link_cabinet_set)(int n);
    /* THE CAR (Controls > Car, saved as `car`): the game's own car / colour option, one name per value (Ridge Racer 2's CAR TYPE,
     * 1 BLUE .. 8 GREEN). ncars 0 = no such row. */
    const char *const *cars; int ncars;
    void (*car_set)(int i); int (*car_get)(void);
    const char *const *act_label;           /* per-action names for the Controls page (NULL entries / NULL table = the action's own name) */
    const struct rr_bind_default { int act; const char *keys, *pad; } *bind_defaults;   /* default keys / pad button replacing the
                                             * driving defaults for these actions (keys: SDL names, comma-separated; pad NULL = none) */
    int nbind_defaults;
    /* THE SELECTION SCREENS (GitHub #35): the game picks AT/MT, a course or a class by where the WHEEL sits, so a self-centring control
     * (keys, D-pad, a thumbstick) fell back to the centre option the moment it was let go -- "the cursor drifts left". While the
     * 16-bit work-RAM word `select_addr` holds one of `select_vals` (0 ends the list) keys and sticks MOVE the wheel and it stays
     * where it is left; a real wheel stays absolute. select_addr 0 = never (the old behaviour). Found by diffing work-RAM dumps of
     * the select screens against attract and the race. */
    uint32_t select_addr; uint16_t select_vals[4];
} rr_game_t;

extern const rr_game_t *g_rr_game;
/* rr_main.c: launch g_rr_game->debug[i] at the next frame boundary (outside interrupts) */
void rr_debug_launch(int i);
#endif
