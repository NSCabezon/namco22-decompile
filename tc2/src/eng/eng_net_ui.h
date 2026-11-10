/* TIME CRISIS 2'S OWN COPY of namco-2x-systems engine/eng_net_ui.h (copied 2026-10-21). TC2 is independent of the System 22 project: change it here. */
/* eng_net_ui.h -- the Online page of the shared menu (engine/eng_net_ui.c). After eng_cfg_load() and eng_net_init():
 *     eng_ui_add_page(eng_net_ui_page());
 * It reads/saves net_server (default zonesync.net) and net_name in the game's cfg; connecting is always a menu action. */
#ifndef ENG_NET_UI_H
#define ENG_NET_UI_H
#include "eng_ui.h"
const eng_ui_page *eng_net_ui_page(void);
#include <stdbool.h>
#include <stddef.h>
/* a game's one-click "second player on this computer" (Time Crisis 2): the hook starts it and says what happened in msg; shown as a
 * row of the page and a button in the Online window while set */
void eng_net_ui_set_local2p(bool (*start)(char *msg, size_t n), const char *label, const char *note);
#endif
