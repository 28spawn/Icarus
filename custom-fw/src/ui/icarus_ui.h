/* custom-fw/src/ui/icarus_ui.h — Icarus Vintage Analog Laboratory UI */
#ifndef ICARUS_UI_H
#define ICARUS_UI_H

#include "pa_ui.h"
#include "pa_fx.h"

/* Vintage Analog Laboratory RGB565 Palette (Section 12.1) */
#define ICARUS_COL_BG        0x0841u /* #0B0D0F Near-black charcoal background */
#define ICARUS_COL_PANEL     0x18E3u /* #181C1F Dark graphite panel */
#define ICARUS_COL_PANEL_HI  0x2145u /* #22282C Highlighted panel */
#define ICARUS_COL_LINE      0x31A6u /* #313638 Panel border / divider */
#define ICARUS_COL_AMBER     0xFD60u /* #FFAE00 Warm Amber (Primary accent / Osc A) */
#define ICARUS_COL_CYAN      0x3679u /* #35CECA Muted Cyan (Secondary accent / Osc B) */
#define ICARUS_COL_ORANGE    0xFA60u /* #FF4D00 Warm Orange (Filter visualization) */
#define ICARUS_COL_VIOLET    0xB39Fu /* #B473FF Soft Violet (Modulation) */
#define ICARUS_COL_RED       0xFA27u /* #FF453A Soft Red (Warning / Clip) */
#define ICARUS_COL_DISABLED  0x4A69u /* #4A4D4A Dark Grey (Disabled module) */
#define ICARUS_COL_TEXT      0xF7BEu /* #F5F7F5 Warm off-white laboratory text */
#define ICARUS_COL_MUTE      0x8C71u /* #8C8E8C Muted label grey */
#define ICARUS_COL_SCOPE_BG  0x0020u /* #040804 Deep CRT oscilloscope screen background */
#define ICARUS_COL_GRID      0x2124u /* #202420 Subtle CRT graticule line */
#define ICARUS_COL_ORANGE_DIM 0x6180u /* #663000 Dim passband shaded fill */
#define ICARUS_COL_AMBER_DIM 0x6260u /* #664C00 Dim amber fill */
#define ICARUS_COL_CYAN_DIM  0x1268u /* #124C48 Dim cyan fill */
#define ICARUS_COL_VIOLET_DIM 0x3989u /* #3A304A Dim violet fill */

void icarus_ui_enter(pa_ui_ctx_t *ui, pa_fx_chain_t *chain);
void icarus_ui_exit(pa_ui_ctx_t *ui, pa_fx_chain_t *chain);
void icarus_ui_handle_inputs(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                            uint32_t buttons_pressed, int32_t enc_delta);
void icarus_ui_render(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain);
void icarus_ui_set_debug_page(uint8_t screen, uint8_t cat, uint8_t row);


/* Backward compatibility aliases */
#define ALPHA_COL_AMBER ICARUS_COL_AMBER
#define ALPHA_COL_TEXT ICARUS_COL_TEXT
#define ALPHA_COL_MUTE ICARUS_COL_MUTE
#define ALPHA_COL_BG ICARUS_COL_BG
#define ALPHA_COL_PANEL ICARUS_COL_PANEL
#define ALPHA_COL_PANEL_HI ICARUS_COL_PANEL_HI
#define ALPHA_COL_LINE ICARUS_COL_LINE
#define ALPHA_COL_CYAN ICARUS_COL_CYAN
#define ALPHA_COL_ORANGE ICARUS_COL_ORANGE
#define ALPHA_COL_VIOLET ICARUS_COL_VIOLET
#define ALPHA_COL_RED ICARUS_COL_RED
#define ALPHA_COL_DISABLED ICARUS_COL_DISABLED
#define ALPHA_COL_SCOPE_BG ICARUS_COL_SCOPE_BG
#define ALPHA_COL_GRID ICARUS_COL_GRID
#define ALPHA_COL_ORANGE_DIM ICARUS_COL_ORANGE_DIM
#define ALPHA_COL_AMBER_DIM ICARUS_COL_AMBER_DIM
#define ALPHA_COL_CYAN_DIM ICARUS_COL_CYAN_DIM
#define ALPHA_COL_VIOLET_DIM ICARUS_COL_VIOLET_DIM
#define alpha_ui_enter icarus_ui_enter
#define alpha_ui_exit icarus_ui_exit
#define alpha_ui_handle_inputs icarus_ui_handle_inputs
#define alpha_ui_render icarus_ui_render

#endif /* ICARUS_UI_H */
