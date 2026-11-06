/* custom-fw/src/ui/project_alpha_ui.h — Project Alpha Vintage Analog Laboratory UI */
#ifndef PROJECT_ALPHA_UI_H
#define PROJECT_ALPHA_UI_H

#include "pa_ui.h"
#include "pa_fx.h"

/* Vintage Analog Laboratory RGB565 Palette (Section 12.1) */
#define ALPHA_COL_BG        0x0841u /* #0B0D0F Near-black charcoal background */
#define ALPHA_COL_PANEL     0x18E3u /* #181C1F Dark graphite panel */
#define ALPHA_COL_PANEL_HI  0x2145u /* #22282C Highlighted panel */
#define ALPHA_COL_LINE      0x31A6u /* #313638 Panel border / divider */
#define ALPHA_COL_AMBER     0xFD60u /* #FFAE00 Warm Amber (Primary accent / Osc A) */
#define ALPHA_COL_CYAN      0x3679u /* #35CECA Muted Cyan (Secondary accent / Osc B) */
#define ALPHA_COL_ORANGE    0xFA60u /* #FF4D00 Warm Orange (Filter visualization) */
#define ALPHA_COL_VIOLET    0xB39Fu /* #B473FF Soft Violet (Modulation) */
#define ALPHA_COL_RED       0xFA27u /* #FF453A Soft Red (Warning / Clip) */
#define ALPHA_COL_DISABLED  0x4A69u /* #4A4D4A Dark Grey (Disabled module) */
#define ALPHA_COL_TEXT      0xF7BEu /* #F5F7F5 Warm off-white laboratory text */
#define ALPHA_COL_MUTE      0x8C71u /* #8C8E8C Muted label grey */
#define ALPHA_COL_SCOPE_BG  0x0020u /* #040804 Deep CRT oscilloscope screen background */
#define ALPHA_COL_GRID      0x2124u /* #202420 Subtle CRT graticule line */
#define ALPHA_COL_ORANGE_DIM 0x6180u /* #663000 Dim passband shaded fill */
#define ALPHA_COL_AMBER_DIM 0x6260u /* #664C00 Dim amber fill */
#define ALPHA_COL_CYAN_DIM  0x1268u /* #124C48 Dim cyan fill */
#define ALPHA_COL_VIOLET_DIM 0x3989u /* #3A304A Dim violet fill */

void alpha_ui_enter(pa_ui_ctx_t *ui, pa_fx_chain_t *chain);
void alpha_ui_exit(pa_ui_ctx_t *ui, pa_fx_chain_t *chain);
void alpha_ui_handle_inputs(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                            uint32_t buttons_pressed, int32_t enc_delta);
void alpha_ui_render(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain);
void alpha_ui_set_debug_page(uint8_t screen, uint8_t cat, uint8_t row);

#endif /* PROJECT_ALPHA_UI_H */
