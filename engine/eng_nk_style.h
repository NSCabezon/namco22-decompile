/* eng_nk_style.h -- the look every Nuklear menu in the tree shares (the engine's eng_ui.c, Rave Racer's rr_ui.c, Prop Cycle's ui_menu.c).
 * Include AFTER nuklear.h. Header-only because each of those menus is its own Nuklear implementation. */
#ifndef ENG_NK_STYLE_H
#define ENG_NK_STYLE_H
/* THE SELECTED ROW / TAB (GitHub #43): Nuklear's default marks the selected item with a grey a few shades off the window's, hard to see
 * (worst with a dark desktop theme). Amber with black text; a hovered row is lifted a little. */
static inline void eng_nk_style_highlight(struct nk_context *c)
{
    struct nk_style_selectable *s = &c->style.selectable;
    s->normal_active  = nk_style_item_color(nk_rgb(240, 170, 40));
    s->hover_active   = nk_style_item_color(nk_rgb(255, 196, 80));
    s->pressed_active = nk_style_item_color(nk_rgb(255, 196, 80));
    s->text_normal_active = s->text_hover_active = s->text_pressed_active = nk_rgb(0, 0, 0);
    s->hover = nk_style_item_color(nk_rgb(70, 70, 78));
}
#endif
