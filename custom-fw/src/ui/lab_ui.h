/* custom-fw/src/ui/lab_ui.h — the "laboratory faceplate" drawing kit shared by every screen (home rack, block pages, settings, Project Alpha).
 *
 * The look, in one paragraph: near-black faceplate, SQUARE panels with a 1 px rule, amber corner brackets on whatever has focus (green when
 * it is being edited), legend plates instead of floating captions, tick-ruler gauges with a needle instead of flat progress bars, and LED
 * ladders for levels. One accent colour per block (cyan DYN, amber SYNTH, orange AMP, violet CAB, phosphor EQ) so a block keeps its colour
 * on the rack, on its page and in the signal-path dots.
 *
 * Header-only: every function is static inline over st7735s_fill_rect / ui_draw_str, so it adds no object and no link dependency. Each
 * fill_rect is one display-list command (the frame holds 1372): the helpers say what they cost. */
#ifndef LAB_UI_H
#define LAB_UI_H

#include <stdint.h>
#include "pa_ui.h"
#include "project_alpha_ui.h"          /* ALPHA_COL_* : the laboratory palette */

extern void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565);

#define LAB_COL_PHOS   0x5F11u          /* #5CE08A phosphor green: EQ, "good", edit mode */
#define LAB_COL_FACE   0x18E3u          /* panel face */
#define LAB_COL_FACE_HI 0x2145u         /* focused panel face */

/* A 1 px square outline: 4 commands. */
static inline void lab_outline(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t col) {
    st7735s_fill_rect(x, y, w, 1, col);
    st7735s_fill_rect(x, (uint16_t)(y + h - 1u), w, 1, col);
    st7735s_fill_rect(x, y, 1, h, col);
    st7735s_fill_rect((uint16_t)(x + w - 1u), y, 1, h, col);
}

/* A filled square panel with a rule: 5 commands. */
static inline void lab_panel(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t face, uint16_t rule) {
    st7735s_fill_rect(x, y, w, h, face);
    lab_outline(x, y, w, h, rule);
}

/* Corner brackets, `len` px long, on the box's corners: 8 commands. The focus mark: amber, or green while the thing is being edited. */
static inline void lab_brackets(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t len, uint16_t col) {
    const uint16_t r = (uint16_t)(x + w - 1u), b = (uint16_t)(y + h - 1u);
    st7735s_fill_rect(x, y, len, 2, col);                  st7735s_fill_rect(x, y, 2, len, col);
    st7735s_fill_rect((uint16_t)(r - len + 1u), y, len, 2, col);  st7735s_fill_rect((uint16_t)(r - 1u), y, 2, len, col);
    st7735s_fill_rect(x, (uint16_t)(b - 1u), len, 2, col); st7735s_fill_rect(x, (uint16_t)(b - len + 1u), 2, len, col);
    st7735s_fill_rect((uint16_t)(r - len + 1u), (uint16_t)(b - 1u), len, 2, col);
    st7735s_fill_rect((uint16_t)(r - 1u), (uint16_t)(b - len + 1u), 2, len, col);
}

/* focus state: 0 none, 1 focused (amber), 2 editing (green). A panel with its rule and, when focused, brackets over it: 5 or 13 commands. */
static inline void lab_card(uint16_t x, uint16_t y, uint16_t w, uint16_t h, int focus) {
    const uint16_t rule = (focus == 2) ? LAB_COL_PHOS : (focus == 1) ? ALPHA_COL_AMBER : ONYX_COL_LINE;
    lab_panel(x, y, w, h, focus ? LAB_COL_FACE_HI : LAB_COL_FACE, rule);
    if (focus) lab_brackets(x, y, w, h, 6u, rule);
}

/* A square LED, 6 x 6, lit in `col` (with a 2 px highlight) or dark: 2-3 commands. */
static inline void lab_led(uint16_t x, uint16_t y, int on, uint16_t col) {
    st7735s_fill_rect(x, y, 6, 6, on ? col : ONYX_COL_LINE);
    if (on) st7735s_fill_rect((uint16_t)(x + 1u), (uint16_t)(y + 1u), 2, 2, ALPHA_COL_TEXT);
}

/* Tick-ruler gauge, the signature control: a scale of 11 ticks (the 0, 50 and 100 % ticks taller), a 2 px trace from the left edge to the
 * value in `col`, and a needle block under the scale. `baseline` is the y of the trace; ticks rise 3-5 px above it, the needle hangs 4 px below.
 * Bipolar (centre-zero) scales trace from the middle. 15-17 commands. */
static inline void lab_ruler(uint16_t x, uint16_t baseline, uint16_t w, int32_t pct, uint16_t col, int bipolar, int hot) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    for (int i = 0; i <= 10; i++) {
        const uint16_t tx = (uint16_t)(x + (uint16_t)(((uint32_t)i * (uint32_t)(w - 1u)) / 10u));
        const uint16_t th = (i == 0 || i == 5 || i == 10) ? 5u : 3u;
        st7735s_fill_rect(tx, (uint16_t)(baseline - th), 1, th, ONYX_COL_DIM);
    }
    st7735s_fill_rect(x, baseline, w, 2, ONYX_COL_LINE);                   /* the scale's rail */
    const uint16_t px = (uint16_t)(x + (uint16_t)(((uint32_t)pct * (uint32_t)(w - 1u)) / 100u));
    if (bipolar) {
        const uint16_t mid = (uint16_t)(x + (w >> 1));
        if (px >= mid) st7735s_fill_rect(mid, baseline, (uint16_t)(px - mid + 1u), 2, col);
        else           st7735s_fill_rect(px, baseline, (uint16_t)(mid - px + 1u), 2, col);
    } else if (px > x) {
        st7735s_fill_rect(x, baseline, (uint16_t)(px - x + 1u), 2, col);
    }
    st7735s_fill_rect((uint16_t)(px > 0u ? px - 1u : 0u), (uint16_t)(baseline + 2u), 3, 4, hot ? ALPHA_COL_AMBER : ALPHA_COL_TEXT);   /* the needle */
}

/* LED ladder for a level: segments 3 px wide with a 1 px gap, green to 70 %, amber to 90 %, red above; unlit ones dark. w/4 commands. */
static inline void lab_ladder(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t pct) {
    const uint16_t n = (uint16_t)(w / 4u);
    const uint16_t lit = (uint16_t)(((uint32_t)pct * n + 50u) / 100u);
    for (uint16_t i = 0; i < n; i++) {
        uint16_t col = ONYX_COL_LINE;
        if (i < lit) col = (i * 100u < n * 70u) ? LAB_COL_PHOS : (i * 100u < n * 90u) ? ALPHA_COL_AMBER : ONYX_COL_BAD;
        st7735s_fill_rect((uint16_t)(x + i * 4u), y, 3, h, col);
    }
}

/* The page header: a face-coloured band (0..29) with the amber rule under it (y 30). The title sits after a small square mark in the
 * block's colour. 3 commands + text. */
static inline void lab_header_band(void) {
    st7735s_fill_rect(0, 0, 240, 30, LAB_COL_FACE);
    st7735s_fill_rect(0, 30, 240, 1, ALPHA_COL_AMBER);
}

/* A toggle switch plate (the block's ON / OFF): 48 x 20 at (x, y). A lit LED and the word. */
static inline void lab_switch(uint16_t x, uint16_t y, int on, uint16_t col, int focus) {
    lab_card(x, y, 48, 20, focus);
    lab_led((uint16_t)(x + 7u), (uint16_t)(y + 7u), on, col);
    ui_draw_str((uint16_t)(x + 18u), (uint16_t)(y + 7u), on ? "ON" : "OFF", on ? ALPHA_COL_TEXT : ONYX_COL_MUTE, focus ? LAB_COL_FACE_HI : LAB_COL_FACE);
}

#endif /* LAB_UI_H */
