/* ss22_out.h -- the cabinet's OUTPUTS (lamps, solenoids, motors) leaving the emulation: MAME's network-output protocol, and pad rumble.
 *
 * The sound MCU's output latches (ss22_snd_outputs: 16 bits, MAME's mcuout0..15) are the game's own outputs -- Time Crisis's gun solenoid
 * (recoil) is mcuout1, Tokyo Wars' start lamp mcuout1 ... Every frame ss22_out_poll() sends changes to whoever listens, exactly as MAME's
 * `-output network` does, so the tools written for it (MAMEHooker and its Linux ports, gun-recoil drivers) work unchanged:
 *   TCP, default port 8000 (SS22_OUT_PORT=<n>, 0 = off; SS22_OUT_BIND=<addr>, default 127.0.0.1 -- local tools only)
 *   on connect:  "mame_start = <set name>\r"       on every change:  "mcuout<N> = <0|1>\r"
 * A game that declares recoil bits (ss22_game.recoil_mask) also rumbles the game pad on each rising edge (Controls > Pad rumble on recoil, out_rumble= in the cfg; SS22_RUMBLE=0 forces it off).
 * SS22_OUTDBG=1 logs every change.
 */
#ifndef SS22_OUT_H
#define SS22_OUT_H
#include <stdint.h>
void ss22_out_poll(uint16_t outputs);       /* once per emulated frame */
void ss22_out_close(void);
#endif
