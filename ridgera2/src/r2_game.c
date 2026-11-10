/* Ridge Racer 2 (MAME `ridgera2`, World RRS2, 1994-06-21) on the shared System 22 runtime (../raverace/src) -- the ONLY thing this game
 * says about itself: which ROM chip is where, and the handful of board behaviours MAME's namcos22.cpp gives it. */
#include "rr_game.h"
#include "rr_mem.h"
#include "rr_hw.h"
#include "rr_lifted.h"

/* A player's first run: every chip taken out of MAME's zips by name, size and CRC (engine/romzip.c), the list as
 * ../acedriver/tools/setup_roms.py's GAMES["ridgera2"] plus the sound BIOS. */
static const char *const r2_zips[] = { "ridgera2.zip", "namcoc74.zip" };
static const eng_rom_t r2_roms[] = {
    { "rrs2_prgll.4d", 0x80000, NULL, NULL },
    { "rrs2_prglm.2d", 0x80000, NULL, NULL },
    { "rrs2_prgum.8d", 0x80000, NULL, NULL },
    { "rrs2_prguu.6d", 0x80000, NULL, NULL },
    { "rrs1data.6r", 0x80000, NULL, NULL },
    { "rrs1cg0.1a", 0x200000, NULL, NULL },
    { "rrs1cg1.2a", 0x200000, NULL, NULL },
    { "rrs1cg2.3a", 0x200000, NULL, NULL },
    { "rrs1cg3.5a", 0x200000, NULL, NULL },
    { "rrs1ccrl.5a", 0x200000, NULL, NULL },
    { "rrs1ccrh.5c", 0x80000, NULL, NULL },
    { "rrs1pol0.5b", 0x80000, NULL, NULL },
    { "rrs1pol1.4b", 0x80000, NULL, NULL },
    { "rrs1pom0.5c", 0x80000, NULL, NULL },
    { "rrs1pom1.4c", 0x80000, NULL, NULL },
    { "rrs1pou0.5d", 0x80000, NULL, NULL },
    { "rrs1pou1.4d", 0x80000, NULL, NULL },
    { "rrs1wav0.10r", 0x100000, NULL, NULL },
    { "rrs1wav1.10p", 0x100000, NULL, NULL },
    { "rrs1wav2.10n", 0x100000, NULL, NULL },
    { "rrs1wav3.10l", 0x100000, NULL, NULL },
    { "rr1gam.2d", 0x100, NULL, NULL },
    { "rr1gam.3d", 0x100, NULL, NULL },
    { "rr1gam.4d", 0x100, NULL, NULL },
    { "c74.bin", 0x4000, NULL, NULL },        /* the sound MCU BIOS: namcoc74.zip */
};

/* WIDESCREEN HUD: the race HUD is up while these text cells hold these tiles -- the TIME and POSITION labels (row 1) and the tacho
 * (row 22). Measured over text dumps of a coined race, two driven races and attract + four test-mode pages (2026-10-25): present on
 * every race frame from the start line to GAME OVER, on no other screen. */
static const uint16_t r2_hud_marks[][3] = { { 1, 3, 0x5331 }, { 1, 31, 0x5337 }, { 22, 29, 0x22C4 } };

/* THE OPTIONS BLOCK. EEPROM logical block 2 (30 data bytes + a BE16 checksum = ~ the sum of the 15 data WORDS, ROM 0x2041E), loaded at
 * boot into work RAM 0x10006840 (0x1FFEA: 16 blocks, 0x20 bytes each from 0x10006800). The EEPROM keeps a directory at 0x400: byte k =
 * where logical block k lives (0x500 + byte * 0x10), so the block is found through it. Measured byte by byte (test mode walks, pokes):
 *   0 GAME COST coins  6 FREE PLAY (1 = on)  0x10..0x14 difficulty  0x15..0x19 laps  0x1A SPEED  0x1B SOUND IN ATTRACT
 *   0x1C CPU NO. (& 7: this cabinet's link number)  0x1D CAR TYPE (0..7 = 1 BLUE .. 8 GREEN)  0x1E LINK
 * A change goes into work RAM (the running game reads it there) and into the EEPROM block with a fresh checksum (the next boot), as
 * the test menu itself does; an EEPROM the game has not initialised yet (a first run) is left alone. */
static void r2_opt_set(int byte, uint8_t v)
{
    g_rr.wram[0x6840 + byte] = v;
    const uint32_t blk = 0x500u + (uint32_t)g_rr.eeprom[0x402] * 0x10u;
    if (blk + 32 > RR_EEPROM_SIZE) return;
    uint8_t *b = g_rr.eeprom + blk;
    unsigned sum = 0;
    for (int i = 0; i < 30; i += 2) sum += (unsigned)(b[i] << 8 | b[i + 1]);
    if ((uint16_t)~sum != (uint16_t)(b[30] << 8 | b[31])) return;        /* not a valid block: the game will set it up itself */
    b[byte] = v;
    sum = 0;
    for (int i = 0; i < 30; i += 2) sum += (unsigned)(b[i] << 8 | b[i + 1]);
    b[30] = (uint8_t)((uint16_t)~sum >> 8); b[31] = (uint8_t)~sum;
}
static void r2_freeplay_set(bool on) { r2_opt_set(6, on ? 1 : 0); }
static bool r2_freeplay_get(void) { return g_rr.wram[0x6846] != 0; }
/* the link number, and the latches 0x1C110 makes of it at boot: A6W(0x1800) = n, 0x1810 = n * 2, 0x1812 = n * 16, 0x1814 = n * 64 */
static void r2_link_cabinet_set(int n)
{
    n &= 7;
    r2_opt_set(0x1C, (uint8_t)n);
    const uint16_t v[4] = { (uint16_t)n, (uint16_t)(n * 2), (uint16_t)(n * 16), (uint16_t)(n * 64) };
    const uint32_t at[4] = { 0x9800, 0x9810, 0x9812, 0x9814 };
    for (int i = 0; i < 4; i++) { g_rr.wram[at[i]] = (uint8_t)(v[i] >> 8); g_rr.wram[at[i] + 1] = (uint8_t)v[i]; }
}
static const char *const r2_cars[] = { "BLUE", "YELLOW", "RED", "GREEN", "BLUE (white)", "YELLOW (2)", "RED (orange)", "GREEN (dark)" };
static void r2_car_set(int i) { if (i >= 0 && i < 8) r2_opt_set(0x1D, (uint8_t)i); }
static int r2_car_get(void) { return g_rr.wram[0x685D] & 7; }

/* FORCE FEEDBACK. Ridge Racer 2's cabinet has no wheel motor (MAME's handle_driving_io copies no drive byte for Ridge Racer 1/2), so the
 * force is made from the game's state (rr_game_t.ffb_synth), only while the race HUD is up:
 *   - centring: a pull back toward the middle proportional to the wheel's offset, from 0.15 at a standstill to 0.70 at 150 and over;
 *   - impacts: the speed word 0x10009E9C (measured 2026-10-25 over a gas/brake race: smooth through the automatic's five gears,
 *     correlation 0.93 with the gear, steps of a few units a frame) falling faster than the brakes can take it -- 6 a frame and
 *     more -- starts a 12-frame shake whose size follows the loss (a wall hit took 135 -> 70 in two frames).
 * The speed is read once per game frame, so a paused or slowed window does not see a fake impact. */
static double r2_ffb(void)
{
    extern uint32_t rr_frame;
    static uint32_t last = ~0u; static int prev_speed, shake_left; static double shake_amp;
    const bool racing = (g_rr.text[(1 * 64 + 3) * 2] << 8 | g_rr.text[(1 * 64 + 3) * 2 + 1]) == 0x5331;
    int speed = (int16_t)rr_read(0x10009E9C, 2);
    if (speed < 0) speed = 0;
    if (rr_frame != last) {
        last = rr_frame;
        const int loss = prev_speed - speed;
        if (racing && loss >= 6) { shake_left = 12; shake_amp = loss >= 40 ? 0.9 : 0.3 + 0.6 * (loss - 6) / 34.0; }
        else if (shake_left > 0) shake_left--;
        prev_speed = speed;
    }
    if (!racing) { shake_left = 0; return 0.0; }
    const double off = ((int)g_hw.steer - 0x800) / (double)(0xD80 - 0x800);          /* -1..1, + = the higher A-D side */
    double f = (0.15 + 0.55 * (speed > 150 ? 1.0 : speed / 150.0)) * off;          /* + pushes toward the lower side: back to the middle */
    if (shake_left > 0) f += ((shake_left >> 1) & 1 ? shake_amp : -shake_amp) * shake_left / 12.0;
    return f > 1 ? 1 : f < -1 ? -1 : f;
}

/* DEVELOPER SCREENS (the Debug page). The main loop's mode dispatcher (0xD830) runs mode 2, SELECT MODE (0xDAAC, its states through the
 * table at 0xDAC0), whenever the Test switch is OFF and DIP switch bit 7 of 0x50000000 is ON -- nothing else enters it. Its cursor
 * (0x10000808) picks an item and gas writes cursor * 2 + 1 into the state word (0x10000806); state 2 is the menu waiting. */
static const rr_debug_t r2_debug[] = {
    { "SELECT MODE (developer menu)", 0,  "The game's own developer menu: wheel moves the cursor, gas picks. Found behind DIP switch 8." },
    { "Polygon viewer",               3,  "SELECT MODE > POLYGON (0x1734E): the object viewer (FRAME / WINDOW / OBJECT / LIGHT)." },
    { "Character viewer",             5,  "SELECT MODE > CHARACTER (0x21DF6): colour bars." },
    { "Switch test",                  7,  "SELECT MODE > SWITCH (0x73C0): the cabinet inputs." },
    { "RAM test",                     9,  "SELECT MODE > RAM TEST: this revision points it at the same handler as SWITCH (0x73C0)." },
    { "Area test",                    11, "SELECT MODE > AREA (0x70F6): AREA TEST with X / Y / Z." },
    { "Sound test",                   13, "SELECT MODE > SOUND (0x206B4)." },
    { "Adjustment (hidden)",          15, "State 15/16 (0x20BAA): HANDLE / GAS / BRAKE calibration. No menu item reaches it (the cursor wraps at SOUND)." },
    { "Leave (DIP switch off)",       DEBUG_DSW_LEAVE, "Turn the DIP switch off again: the game goes back to attract." },
};

static const rr_game_t r2 = {
    .name = "ridgera2", .title = "Ridge Racer 2", .cfg_file = "r2_controls.cfg", .nv_file = "r2_eeprom.nv", .entry = L_CCD0,
    .prg = { "rrs2_prguu.6d", "rrs2_prgum.8d", "rrs2_prglm.2d", "rrs2_prgll.4d" },  /* ROM_LOAD32_BYTE offsets 0, 1, 2, 3 */
    .cg = { NULL, NULL, NULL, NULL, "rrs1cg0.1a", "rrs1cg1.2a", "rrs1cg2.3a", "rrs1cg3.5a" },   /* the upper 8 MB, as Ace Driver */
    .ccrl = "rrs1ccrl.5a", .ccrh = "rrs1ccrh.5c", .tex_fixup = 1,           /* MAME init_tables: Ridge Racer 1/2 */
    .pot = { { "rrs1pol0.5b", "rrs1pol1.4b" }, { "rrs1pom0.5c", "rrs1pom1.4c" }, { "rrs1pou0.5d", "rrs1pou1.4d" } },
    .pot_chips = 2,
    .snd_data = "rrs1data.6r",
    .wav = { { "rrs1wav0.10r", 0x000000 }, { "rrs1wav2.10n", 0x100000 }, { "rrs1wav1.10p", 0x200000 }, { "rrs1wav3.10l", 0x300000 } },
    .gamma = { "rr1gam.2d", "rr1gam.3d", "rr1gam.4d" },
    .eeprom = NULL,                                   /* no default image in the set: the game initialises a blank EEPROM */
    .keycus_off = 0, .keycus_val = 0x0172,            /* namcos22_keycus_r: NAMCOS22_RIDGE_RACER2, offset 0 */
    .steer_add = 0x160, .gas_add = 884, .brake_add = 809,   /* handle_driving_io: Ridge Racer 1/2 */
    .inputs_idle = 0xFEFF,                            /* INPUT_PORTS_START(ridgera2): cabinet field 0x2100 = 0x2000 Standard, the rest active-low 1 */
    .steer_min = 0x280, .steer_max = 0xD80,           /* ADC.0 PORT_MINMAX(0x280, 0xd80) */
    .gas_max = 0x610, .brake_max = 0x610,             /* ADC.1 / ADC.2 (0, 0x610) */
    .gas_delta = 160, .brake_delta = 160,             /* PORT_KEYDELTA(160) */
    .spin_pc = 0, .spin_op = 0,                       /* the master's busy-wait poll is not measured for this game yet */
    .zips = r2_zips, .nzips = 2, .roms = r2_roms, .nroms = (int)(sizeof r2_roms / sizeof r2_roms[0]),
    .log_file = "ridgeracer2.log",
    .hud_marks = r2_hud_marks, .n_hud_marks = 3,
    .ffb_synth = r2_ffb,
    .link_payload = 0x22, .link_tx_reg6 = 1, .link_ring = 1,          /* the C139 sender 0x1BF38 / receiver 0x1BFEC (frame 0x22 + length 0x24 + checksum) */
    .link_id_addr = 0x1000685C,                       /* & 7: this cabinet's link number (0x1C110) */
    .freeplay_set = r2_freeplay_set, .freeplay_get = r2_freeplay_get, .link_cabinet_set = r2_link_cabinet_set,
    .cars = r2_cars, .ncars = 8, .car_set = r2_car_set, .car_get = r2_car_get,
    .debug = r2_debug, .ndebug = (int)(sizeof r2_debug / sizeof r2_debug[0]),
    .debug_dsw = 0x80000000u,                         /* the byte at 0x50000000, bit 7 */
    .debug_mode_addr = 0x10000804, .debug_state_addr = 0x10000806, .debug_cursor_addr = 0x10000808,
    .debug_mode = 2, .debug_menu_state = 2,
};
const rr_game_t *g_rr_game = &r2;
