/* custom-fw/src/ui/project_alpha_ui.c — Project Alpha Vintage Analog Laboratory UI (240x240 RGB565)
 * Interactive Wavelength Oscilloscopes, 2D Filter Bode Curves, ADSR Vector Graph & Waveshaper Curves */
#include "project_alpha_ui.h"
#include "../effects/project_alpha/alpha.h"
#include <string.h>
#include <math.h>
#include "pa_plat.h"

extern void st7735s_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565);

typedef enum {
    ALPHA_SCREEN_MAIN = 0,
    ALPHA_SCREEN_PAGE = 2      /* 1 was the category menu: gone, every page is one < / > press away */
} alpha_screen_t;

typedef enum {
    ALPHA_CAT_MODE        = 0,
    ALPHA_CAT_OSCILLATORS = 1,
    ALPHA_CAT_FILTER      = 2,
    ALPHA_CAT_ENVELOPE    = 3,
    ALPHA_CAT_MODULATION  = 4,
    ALPHA_CAT_MIXER       = 5,
    ALPHA_CAT_ADVANCED    = 6,
    ALPHA_CAT_COUNT       = 7
} alpha_menu_cat_t;

static struct {
    uint8_t screen;         /* alpha_screen_t */
    uint8_t main_focus;     /* what the encoder turns on MAIN: 0=PRESET, 1=MODE, 2=CUTOFF, 3=RES (PARA click cycles) */
    uint8_t main_editing;   /* always 1 on MAIN (the encoder is live); only drives the focus colour */
    uint8_t menu_cat;       /* alpha_menu_cat_t (0..6) */
    uint8_t page_row;       /* Selected parameter row inside category page */
    uint8_t page_editing;   /* 1 = encoder adjusts parameter on category page */
} s_aui;

static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* Freestanding string formatting helpers (no libc snprintf required on pi32v2) */
static char *cat_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    *p = '\0';
    return p;
}

static char *cat_ch(char *p, char c) {
    *p++ = c;
    *p = '\0';
    return p;
}

static char *cat_u(char *p, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0u) {
        *p++ = '0';
        *p = '\0';
        return p;
    }
    while (v > 0u && n < 11) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) *p++ = tmp[--n];
    *p = '\0';
    return p;
}

static char *cat_u2_zero(char *p, uint32_t v) {
    if (v > 99u) v = 99u;
    *p++ = (char)('0' + (v / 10u));
    *p++ = (char)('0' + (v % 10u));
    *p = '\0';
    return p;
}

static char *cat_i_signed(char *p, int v) {
    if (v >= 0) {
        *p++ = '+';
        return cat_u(p, (uint32_t)v);
    } else {
        *p++ = '-';
        return cat_u(p, (uint32_t)(-v));
    }
}

static char *cat_i(char *p, int v) {
    if (v < 0) {
        *p++ = '-';
        return cat_u(p, (uint32_t)(-v));
    }
    return cat_u(p, (uint32_t)v);
}

static char *cat_pct(char *p, int pct) {
    p = cat_i(p, pct);
    return cat_ch(p, '%');
}

static void draw_box_border(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t fill, uint16_t border) {
    st7735s_fill_rect(x, y, w, h, fill);
    st7735s_fill_rect(x, y, w, 1, border);
    st7735s_fill_rect(x, (uint16_t)(y + h - 1u), w, 1, border);
    st7735s_fill_rect(x, y, 1, h, border);
    st7735s_fill_rect((uint16_t)(x + w - 1u), y, 1, h, border);
}

/* Draw a connected vertical segment for continuous oscilloscope / response curve traces */
static void draw_trace_seg(uint16_t x, int y_prev, int y_cur, int y_min, int y_max, uint16_t w, uint16_t col) {
    if (y_prev < y_min) y_prev = y_min;
    if (y_prev > y_max) y_prev = y_max;
    if (y_cur  < y_min) y_cur  = y_min;
    if (y_cur  > y_max) y_cur  = y_max;
    int y0 = (y_prev < y_cur) ? y_prev : y_cur;
    int y1 = (y_prev > y_cur) ? y_prev : y_cur;
    int h  = y1 - y0 + 2;
    if (y0 + h > y_max + 1) h = y_max + 1 - y0;
    if (h < 1) h = 1;
    st7735s_fill_rect(x, (uint16_t)y0, w, (uint16_t)h, col);
}

static const char *mode_short_str(uint8_t m) {
    static const char *const k_m[4] = { "ANA", "OSC", "HYB", "GTR" };
    return k_m[m & 3u];
}

static const char *mode_full_str(uint8_t m) {
    static const char *const k_m[4] = { "ANALOG", "OSCILLATOR", "HYBRID", "GUITAR" };
    return k_m[m & 3u];
}

static const char *wave_str(uint8_t w) {
    static const char *const k_w[3] = { "SAW", "SQR", "TRI" };
    return k_w[w % 3u];
}

static const char *dist_str(uint8_t d) {
    static const char *const k_d[5] = { "SOFT CLIP", "HARD CLIP", "FOLDBACK", "HEXA GR", "FUZZ" };
    return k_d[d % 5u];
}

static const char *fmode_str(uint8_t fm) {
    static const char *const k_f[4] = { "LP 12dB", "LP 24dB", "HP 12dB", "BP 12dB" };
    return k_f[fm & 3u];
}

static const char *sub_str(uint8_t sm) {
    static const char *const k_s[3] = { "OFF", "-1 OCT", "-2 OCT" };
    return k_s[sm % 3u];
}

static const char *lfo_wave_str(uint8_t lw) {
    static const char *const k_lw[5] = { "SINE", "TRIANGLE", "SQUARE", "SAW", "RANDOM S&H" };
    return k_lw[lw % 5u];
}

static const char *lfo_dest_str(uint8_t ld) {
    static const char *const k_ld[5] = { "FILTER FC", "OSC PITCH", "PWM WIDTH", "OSC AMP", "STEREO PAN" };
    return k_ld[ld % 5u];
}

static void fmt_midi_note(uint8_t midi, char out[6]) {
    static const char *const k_n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (midi < 24u || midi > 108u) {
        cat_str(out, "--");
        return;
    }
    char *p = cat_str(out, k_n[midi % 12u]);
    int oct = (int)(midi / 12u) - 1;
    cat_i(p, oct);
}

static uint8_t cat_row_count(uint8_t cat) {
    switch (cat) {
        case ALPHA_CAT_MODE:        return 5u;
        case ALPHA_CAT_OSCILLATORS: return 12u;
        case ALPHA_CAT_FILTER:      return 6u;
        case ALPHA_CAT_ENVELOPE:    return 4u;
        case ALPHA_CAT_MODULATION:  return 9u;
        case ALPHA_CAT_MIXER:       return 5u;
        case ALPHA_CAT_ADVANCED:    return 4u;
        default:                    return 4u;
    }
}

/* ============================================================================
 * Interactive Mathematical Waveform & Filter Response Evaluators for UI Plots
 * ============================================================================ */

/* Evaluate synthesizer oscillator waveform at normalized phase [0..1) */
static float eval_osc_wave(uint8_t wave, float phase, float pw) {
    phase = phase - (float)((int)phase);
    if (phase < 0.0f) phase += 1.0f;
    if (wave == ALPHA_WAVE_SAW) {
        return 2.0f * phase - 1.0f;
    } else if (wave == ALPHA_WAVE_SQUARE) {
        return (phase < pw) ? 0.88f : -0.88f;
    } else {
        /* Triangle */
        return (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    }
}

/* Evaluate nonlinear analog distortion transfer function y = f(x) for x in [-1..+1] */
static float eval_dist_curve(uint8_t dist_mode, float x, float drive) {
    float g = 1.0f + drive * 5.5f;
    float u = x * g;
    switch (dist_mode % 5u) {
        case ALPHA_DIST_SOFT_CLIP:
            return u / (1.0f + 0.85f * fabsf(u));
        case ALPHA_DIST_HARD_CLIP:
            return clampf(u * 1.25f, -0.92f, 0.92f);
        case ALPHA_DIST_FOLDBACK: {
            while (u > 0.85f || u < -0.85f) {
                if (u > 0.85f) u = 1.70f - u;
                if (u < -0.85f) u = -1.70f - u;
            }
            return u * 1.05f;
        }
        case ALPHA_DIST_HEXA: {
            float a = u / (1.0f + 0.65f * fabsf(u));
            float h2 = 0.22f * drive * (2.0f * a * a - 0.5f);
            return clampf(a * 1.15f + h2, -0.95f, 0.95f);
        }
        case ALPHA_DIST_FUZZ:
        default: {
            float asym = (u >= 0.0f) ? (u * 1.6f) : (u * 0.85f);
            return clampf(asym / (1.0f + 0.7f * fabsf(asym)), -0.95f, 0.95f);
        }
    }
}

/* Evaluate resonant filter normalized display height [0.04 .. 0.96] at log-frequency ratio r = f / fc */
static float eval_filter_bode_norm(uint8_t fmode, float f_hz, float fc_hz, float res) {
    float r = f_hz / clampf(fc_hz, 80.0f, 18000.0f);
    float r2 = r * r;
    float q  = 0.707f + res * res * 7.5f; /* Q from 0.707 up to 8.2 */
    float d2 = (1.0f - r2) * (1.0f - r2) + (r / q) * (r / q);
    if (d2 < 0.002f) d2 = 0.002f;

    float mag = 1.0f;
    if (fmode == ALPHA_FILTER_LP12) {
        mag = 1.0f / sqrtf(d2);
    } else if (fmode == ALPHA_FILTER_LP24) {
        mag = 1.0f / d2;
    } else if (fmode == ALPHA_FILTER_HP12) {
        mag = r2 / sqrtf(d2);
    } else {
        /* BP12 */
        mag = (r / (0.7f + res * 1.5f)) / sqrtf(d2);
    }

    /* Convert magnitude to perceptual dB display scale (-28 dB .. +14 dB -> 0.05 .. 0.96) */
    if (mag < 0.03f) mag = 0.03f;
    float db = 8.68589f * logf(mag); /* 20 * log10(mag) */
    float norm = (db + 26.0f) / 40.0f;
    return clampf(norm, 0.04f, 0.96f);
}

/* Draw a miniature oscillator/waveshaper preview inside an OSC card (40x24 px, 20 slices of 2px) */
static void draw_mini_osc_wave(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               uint8_t wave, float pw, float phase_off,
                               int is_analog, uint8_t dist_mode, float drive,
                               uint16_t col, uint16_t col_dim) {
    draw_box_border(x, y, w, h, ALPHA_COL_SCOPE_BG, ALPHA_COL_GRID);
    int yc = (int)y + ((int)h >> 1);
    st7735s_fill_rect((uint16_t)(x + 1u), (uint16_t)yc, (uint16_t)(w - 2u), 1, ALPHA_COL_GRID);

    int slices = ((int)w - 4) / 2;
    int y_prev = yc;
    int y_min  = (int)y + 2;
    int y_max  = (int)(y + h) - 3;
    float amp  = (float)((int)h - 6) * 0.45f;

    for (int i = 0; i < slices; i++) {
        float t = (float)i / (float)(slices - 1);
        float v;
        if (is_analog) {
            float s_in = sinf((t * 1.5f + phase_off) * 6.2831853f);
            v = eval_dist_curve(dist_mode, s_in, drive);
        } else {
            v = eval_osc_wave(wave, t * 1.5f + phase_off, pw);
        }
        int y_cur = yc - (int)(v * amp);
        if (i == 0) y_prev = y_cur;
        uint16_t sx = (uint16_t)(x + 2 + i * 2);
        /* Subtle vertical fill to center line */
        int fy0 = (y_cur < yc) ? y_cur : yc;
        int fh  = (y_cur < yc) ? (yc - y_cur) : (y_cur - yc);
        if (fh > 1 && (i & 1) == 0) {
            st7735s_fill_rect(sx, (uint16_t)fy0, 1, (uint16_t)fh, col_dim);
        }
        draw_trace_seg(sx, y_prev, y_cur, y_min, y_max, 2, col);
        y_prev = y_cur;
    }
}

/* Draw 2D Resonant Filter Bode Frequency-Response Graph with Live Modulated Cutoff Cursor */
static void draw_filter_bode_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 uint8_t fmode, float base_fc, float live_fc,
                                 float res, int ncols) {
    st7735s_fill_rect(x, y, w, h, ALPHA_COL_SCOPE_BG);

    /* Logarithmic frequency decade graticule lines: 100Hz, 1kHz, 10kHz (within 80Hz..18kHz) */
    uint16_t x_100  = (uint16_t)(x + (uint16_t)(0.041f * (float)w));
    uint16_t x_1k   = (uint16_t)(x + (uint16_t)(0.466f * (float)w));
    uint16_t x_10k  = (uint16_t)(x + (uint16_t)(0.892f * (float)w));
    uint16_t y_0db  = (uint16_t)(y + (uint16_t)((1.0f - 0.65f) * (float)h));
    st7735s_fill_rect(x_100, y, 1, h, ALPHA_COL_GRID);
    st7735s_fill_rect(x_1k,  y, 1, h, ALPHA_COL_GRID);
    st7735s_fill_rect(x_10k, y, 1, h, ALPHA_COL_GRID);
    st7735s_fill_rect(x, y_0db, w, 1, ALPHA_COL_GRID);

    if (live_fc < 80.0f) live_fc = base_fc;

    int col_w  = (int)w / ncols;
    if (col_w < 2) col_w = 2;
    int y_min  = (int)y + 1;
    int y_max  = (int)(y + h) - 2;
    int y_prev = y_max;

    for (int i = 0; i < ncols; i++) {
        float norm_x = (float)i / (float)(ncols - 1);
        /* 80 Hz * 2^(norm_x * 7.8138) -> 80 Hz .. 18000 Hz */
        float f_hz   = 80.0f * powf(2.0f, norm_x * 7.8138f);
        float mag_n  = eval_filter_bode_norm(fmode, f_hz, live_fc, res);
        int y_cur    = (int)(y + h - 2) - (int)(mag_n * (float)(h - 3));
        if (y_cur < y_min) y_cur = y_min;
        if (y_cur > y_max) y_cur = y_max;
        if (i == 0) y_prev = y_cur;

        uint16_t cx = (uint16_t)(x + i * col_w);
        /* Shaded passband fill underneath the Bode curve */
        int fill_h = y_max - y_cur;
        if (fill_h > 1) {
            st7735s_fill_rect(cx, (uint16_t)(y_cur + 1), (uint16_t)(col_w - 1),
                              (uint16_t)fill_h, ALPHA_COL_ORANGE_DIM);
        }
        /* Bright Bode response curve trace */
        draw_trace_seg(cx, y_prev, y_cur, y_min, y_max, (uint16_t)col_w, ALPHA_COL_ORANGE);
        y_prev = y_cur;
    }

    /* Base Cutoff marker (Cyan tick at bottom) & Live Modulated Cutoff marker (Amber cursor) */
    float norm_base = clampf(log2f(base_fc / 80.0f) / 7.8138f, 0.01f, 0.98f);
    float norm_live = clampf(log2f(live_fc / 80.0f) / 7.8138f, 0.01f, 0.98f);
    uint16_t x_base = (uint16_t)(x + (uint16_t)(norm_base * (float)(w - 2)));
    uint16_t x_live = (uint16_t)(x + (uint16_t)(norm_live * (float)(w - 2)));
    st7735s_fill_rect(x_base, (uint16_t)(y + h - 4u), 2, 4, ALPHA_COL_CYAN);
    st7735s_fill_rect(x_live, (uint16_t)(y + 1u), 2, (uint16_t)(h - 2u), ALPHA_COL_AMBER);
}

/* Draw a Real-Time Triggered Wavelength Oscilloscope Box (uses live scope_wave or procedural preview when idle) */
static void draw_wavelength_scope_box(const alpha_state_t *s, uint16_t x, uint16_t y,
                                      uint16_t w, uint16_t h, int ncols) {
    const alpha_preset_t *p = &s->params;
    draw_box_border(x, y, w, h, ALPHA_COL_SCOPE_BG, ALPHA_COL_LINE);

    int yc    = (int)y + ((int)h >> 1);
    int y_min = (int)y + 2;
    int y_max = (int)(y + h) - 3;

    /* Oscilloscope CRT Graticule Center Hairlines */
    st7735s_fill_rect((uint16_t)(x + 1u), (uint16_t)yc, (uint16_t)(w - 2u), 1, ALPHA_COL_GRID);
    st7735s_fill_rect((uint16_t)(x + (w >> 1)), (uint16_t)(y + 1u), 1, (uint16_t)(h - 2u), ALPHA_COL_GRID);

    int has_live_audio = (s->live_peak_out > 0.015f);
    int col_w = ((int)w - 4) / ncols;
    if (col_w < 2) col_w = 2;

    float anim_ph = (float)(s->ui_anim_tick & 63u) / 64.0f;
    float pw = (s->live_pwm >= 0.05f) ? s->live_pwm : p->pulse_width;
    float ratio_b = powf(2.0f, ((float)(p->pitch_b - p->pitch_a) + p->detune_cents * 0.01f) / 12.0f);
    ratio_b = clampf(ratio_b, 0.5f, 2.0f);
    float amp_px = (float)((int)h - 6) * 0.44f;

    int y_prev = yc;
    uint16_t trace_col = (p->mode == ALPHA_MODE_ANALOG) ? ALPHA_COL_AMBER : ALPHA_COL_CYAN;

    for (int i = 0; i < ncols; i++) {
        float v = 0.0f;
        if (has_live_audio) {
            int idx = (i * 64) / ncols;
            v = (float)s->scope_wave[idx & 63] / 32.0f;
        } else {
            /* Synthesize exact dual-oscillator / analog waveshaped wavelength preview from current parameters */
            float t = (float)i / (float)(ncols - 1);
            float ph_a = t * 2.0f + anim_ph;
            float ph_b = t * 2.0f * ratio_b + anim_ph * 1.3f;
            if (p->mode == ALPHA_MODE_ANALOG) {
                float gtr_sim = 0.75f * sinf(ph_a * 6.2831853f) + 0.25f * sinf(ph_a * 12.56637f);
                v = eval_dist_curve(p->dist_mode, gtr_sim, p->analog_drive);
            } else {
                float wa = eval_osc_wave(p->wave_a, ph_a, pw) * p->level_a;
                float wb = eval_osc_wave(p->wave_b, ph_b, pw) * p->level_b;
                float ws = (p->sub_mode != ALPHA_SUB_OFF)
                           ? (eval_osc_wave(ALPHA_WAVE_SQUARE, ph_a * 0.5f, 0.5f) * p->sub_level * 0.5f)
                           : 0.0f;
                v = (1.0f - p->osc_mix) * wa + p->osc_mix * wb + ws;
                if (p->mode == ALPHA_MODE_HYBRID) {
                    float ana = eval_dist_curve(p->dist_mode, sinf(ph_a * 6.2831853f), p->analog_drive);
                    v = 0.65f * v + 0.35f * ana;
                }
                v = clampf(v, -1.0f, 1.0f);
            }
        }

        int y_cur = yc - (int)(v * amp_px);
        if (i == 0) y_prev = y_cur;
        uint16_t cx = (uint16_t)(x + 2 + i * col_w);
        draw_trace_seg(cx, y_prev, y_cur, y_min, y_max, (uint16_t)col_w, trace_col);
        y_prev = y_cur;
    }
}

void alpha_ui_enter(pa_ui_ctx_t *ui, pa_fx_chain_t *chain) {
    alpha_enter_synth_mode(chain);
    memset(&s_aui, 0, sizeof(s_aui));
    s_aui.screen     = ALPHA_SCREEN_MAIN;
    s_aui.main_focus = 2u; /* the encoder starts on Filter Cutoff */
    s_aui.main_editing = 1u;
    if (ui) {
        ui->view  = PA_UI_VIEW_PROJECT_ALPHA;
        ui->dirty = 1;
    }
}

void alpha_ui_exit(pa_ui_ctx_t *ui, pa_fx_chain_t *chain) {
    alpha_exit_synth_mode(chain);
    if (ui) {
        ui->view  = PA_UI_VIEW_MAIN_GRID;
        ui->dirty = 1;
    }
}

void alpha_ui_set_debug_page(uint8_t screen, uint8_t cat, uint8_t row) {
    s_aui.screen   = (screen == ALPHA_SCREEN_PAGE) ? ALPHA_SCREEN_PAGE : ALPHA_SCREEN_MAIN;
    s_aui.menu_cat = (uint8_t)(cat % ALPHA_CAT_COUNT);
    s_aui.page_row = row;
}

/* Rows that cycle through a fixed list or flip a switch: PARA click steps them directly, no edit mode. */
static int row_is_discrete(uint8_t cat, uint8_t row) {
    switch (cat) {
        case ALPHA_CAT_MODE:        return row <= 1u || row == 4u;
        case ALPHA_CAT_OSCILLATORS: return row == 0u || row == 3u || row == 10u;
        case ALPHA_CAT_FILTER:      return row == 0u;
        case ALPHA_CAT_MODULATION:  return row == 1u || row == 5u || row == 8u;
        case ALPHA_CAT_ADVANCED:    return 1;
        default:                    return 0;
    }
}

/* Encoder speed -> step multiplier: slow turns are fine steps, fast spins cross a wide range quickly. */
static int32_t accel_delta(int32_t d) {
    static uint32_t s_last_ms;
    static int      s_last_dir;
    if (d == 0) return 0;
    uint32_t now = plat_ms();
    uint32_t dt  = now - s_last_ms;
    int dir = (d > 0) ? 1 : -1;
    int mult = 1;
    if (dir == s_last_dir) {
        if (dt < 30u)       mult = 8;
        else if (dt < 70u)  mult = 4;
        else if (dt < 140u) mult = 2;
    }
    s_last_ms  = now;
    s_last_dir = dir;
    return d * mult;
}

static int wrap_i(int v, int n) {
    v %= n;
    return (v < 0) ? v + n : v;
}

static void adjust_page_param(alpha_state_t *s, pa_fx_chain_t *chain, uint8_t cat, uint8_t row, int delta) {
    if (!s || delta == 0) return;
    alpha_preset_t *p = &s->params;
    float df = (float)delta;
    int   dd = (delta > 0) ? 1 : -1;      /* list and switch rows move one entry per press whatever the spin speed */

    switch (cat) {
        case ALPHA_CAT_MODE:
            if (row == 0u) {
                alpha_set_mode(s, (uint8_t)wrap_i((int)p->mode + dd, 4));
                return;
            } else if (row == 1u) {
                p->dist_mode = (uint8_t)wrap_i((int)p->dist_mode + dd, 5);
            } else if (row == 2u) {
                p->analog_drive = clampf(p->analog_drive + df * 0.02f, 0.0f, 1.0f);
            } else if (row == 3u) {
                p->input_gain = clampf(p->input_gain + df * 0.05f, 0.5f, 2.5f);
            } else if (row == 4u) {
                p->pre_emphasis ^= 1u;
            }
            break;

        case ALPHA_CAT_OSCILLATORS:
            if (row == 0u)       p->wave_a = (uint8_t)wrap_i((int)p->wave_a + dd, 3);
            else if (row == 1u)  p->pitch_a = (int8_t)clampf((float)p->pitch_a + df, -24.0f, 24.0f);
            else if (row == 2u)  p->level_a = clampf(p->level_a + df * 0.02f, 0.0f, 1.0f);
            else if (row == 3u)  p->wave_b = (uint8_t)wrap_i((int)p->wave_b + dd, 3);
            else if (row == 4u)  p->pitch_b = (int8_t)clampf((float)p->pitch_b + df, -24.0f, 24.0f);
            else if (row == 5u)  p->level_b = clampf(p->level_b + df * 0.02f, 0.0f, 1.0f);
            else if (row == 6u)  p->osc_mix = clampf(p->osc_mix + df * 0.02f, 0.0f, 1.0f);
            else if (row == 7u)  p->detune_cents = clampf(p->detune_cents + df, 0.0f, 50.0f);
            else if (row == 8u)  p->fine_tune = (int8_t)clampf((float)p->fine_tune + df, -50.0f, 50.0f);
            else if (row == 9u)  p->pulse_width = clampf(p->pulse_width + df * 0.02f, 0.05f, 0.95f);
            else if (row == 10u) p->sub_mode = (uint8_t)wrap_i((int)p->sub_mode + dd, 3);
            else if (row == 11u) p->sub_level = clampf(p->sub_level + df * 0.02f, 0.0f, 1.0f);
            break;

        case ALPHA_CAT_FILTER:
            if (row == 0u)      p->filter_mode = (uint8_t)wrap_i((int)p->filter_mode + dd, 4);
            else if (row == 1u) p->cutoff_hz = clampf(p->cutoff_hz * powf(1.06f, df), 80.0f, 18000.0f);
            else if (row == 2u) p->resonance = clampf(p->resonance + df * 0.02f, 0.0f, 1.0f);
            else if (row == 3u) p->env_amount = clampf(p->env_amount + df * 0.02f, -1.0f, 1.0f);
            else if (row == 4u) p->key_tracking = clampf(p->key_tracking + df * 0.02f, 0.0f, 1.0f);
            else if (row == 5u) p->filter_sat = clampf(p->filter_sat + df * 0.02f, 0.0f, 1.0f);
            break;

        case ALPHA_CAT_ENVELOPE:
            if (row == 0u)      p->env_attack_ms  = clampf(p->env_attack_ms + df * 10.0f, 1.0f, 2000.0f);
            else if (row == 1u) p->env_decay_ms   = clampf(p->env_decay_ms + df * 20.0f, 10.0f, 3000.0f);
            else if (row == 2u) p->env_sustain    = clampf(p->env_sustain + df * 0.02f, 0.0f, 1.0f);
            else if (row == 3u) p->env_release_ms = clampf(p->env_release_ms + df * 20.0f, 10.0f, 5000.0f);
            break;

        case ALPHA_CAT_MODULATION:
            if (row == 0u)      p->glide_ms        = clampf(p->glide_ms + df * 5.0f, 0.0f, 2000.0f);
            else if (row == 1u) p->glide_legato    ^= 1u;
            else if (row == 2u) p->vib_rate_hz     = clampf(p->vib_rate_hz + df * 0.2f, 0.1f, 12.0f);
            else if (row == 3u) p->vib_depth_cents = clampf(p->vib_depth_cents + df * 1.0f, 0.0f, 100.0f);
            else if (row == 4u) p->vib_delay_ms    = clampf(p->vib_delay_ms + df * 20.0f, 0.0f, 2000.0f);
            else if (row == 5u) p->lfo_wave        = (uint8_t)wrap_i((int)p->lfo_wave + dd, 5);
            else if (row == 6u) p->lfo_rate_hz     = clampf(p->lfo_rate_hz + df * 0.2f, 0.05f, 20.0f);
            else if (row == 7u) p->lfo_depth       = clampf(p->lfo_depth + df * 0.02f, 0.0f, 1.0f);
            else if (row == 8u) p->lfo_dest        = (uint8_t)wrap_i((int)p->lfo_dest + dd, 5);
            break;

        case ALPHA_CAT_MIXER:
            if (row == 0u)      p->guitar_level = clampf(p->guitar_level + df * 0.02f, 0.0f, 1.0f);
            else if (row == 1u) p->analog_level = clampf(p->analog_level + df * 0.02f, 0.0f, 1.0f);
            else if (row == 2u) p->synth_level  = clampf(p->synth_level + df * 0.02f, 0.0f, 1.0f);
            else if (row == 3u) p->master_level = clampf(p->master_level + df * 0.02f, 0.0f, 1.0f);
            else if (row == 4u) p->stereo_width = clampf(p->stereo_width + df * 0.02f, 0.0f, 1.0f);
            break;

        case ALPHA_CAT_ADVANCED:
            if (row == 0u)      p->nam_enable  ^= 1u;
            else if (row == 1u) p->cab_enable  ^= 1u;
            else if (row == 2u) p->gate_enable ^= 1u;
            else if (row == 3u) alpha_load_preset(s, (uint8_t)wrap_i((int)s->preset_index + dd, (int)ALPHA_FACTORY_PRESET_COUNT));
            alpha_sync_chain_routing(s, chain);
            break;
    }

    alpha_apply_params(s);
    s->live_cutoff_hz = p->cutoff_hz;
    s->live_pwm       = p->pulse_width;
}

/* Tab bar: MAIN + the seven parameter pages. < and > walk it from anywhere, so no page is more than a few presses away. */
#define ALPHA_TAB_COUNT 8u
static const char *const k_tab_names[ALPHA_TAB_COUNT] = { "MAIN", "MODE", "OSC", "FLT", "ENV", "MOD", "MIX", "SYS" };

static uint8_t current_tab(void) {
    return (s_aui.screen == ALPHA_SCREEN_PAGE) ? (uint8_t)(1u + s_aui.menu_cat) : 0u;
}

static void goto_tab(uint8_t tab) {
    s_aui.page_editing = 0u;
    if (tab == 0u) {
        s_aui.screen = ALPHA_SCREEN_MAIN;
    } else {
        s_aui.screen   = ALPHA_SCREEN_PAGE;
        s_aui.menu_cat = (uint8_t)(tab - 1u);
        s_aui.page_row = 0u;
    }
}

void alpha_ui_handle_inputs(pa_ui_ctx_t *ui, pa_fx_chain_t *chain,
                            uint32_t buttons_pressed, int32_t enc_delta) {
    if (!ui || !chain) return;
    alpha_state_t *s = alpha_get_active();
    if (!s) {
        alpha_enter_synth_mode(chain);
        s = alpha_get_active();
        if (!s) return;
    }

    /* HOME: from a page it goes straight back to MAIN; from MAIN it leaves Project Alpha. */
    if (buttons_pressed & PA_BTN_HOME) {
        if (s_aui.screen == ALPHA_SCREEN_PAGE) {
            goto_tab(0u);
            ui->dirty = 1;
        } else {
            alpha_ui_exit(ui, chain);
        }
        return;
    }

    /* < / > : previous / next tab, on every screen. */
    if (buttons_pressed & (PA_BTN_LEFT | PA_BTN_RIGHT)) {
        int step = (buttons_pressed & PA_BTN_RIGHT) ? 1 : -1;
        goto_tab((uint8_t)wrap_i((int)current_tab() + step, (int)ALPHA_TAB_COUNT));
        ui->dirty = 1;
        return;
    }

    if (s_aui.screen == ALPHA_SCREEN_MAIN) {
        s_aui.main_editing = 1u;
        /* VOL- / VOL+ : master output level */
        if (buttons_pressed & (PA_BTN_VOL_MINUS | PA_BTN_VOL_PLUS)) {
            float dv = (buttons_pressed & PA_BTN_VOL_PLUS) ? 0.05f : -0.05f;
            s->params.master_level = clampf(s->params.master_level + dv, 0.0f, 1.0f);
            alpha_apply_params(s);
            ui->dirty = 1;
        }
        /* PARA click: choose what the encoder turns (CUTOFF -> RES -> PRESET -> MODE) */
        if (buttons_pressed & PA_BTN_PARA_CLICK) {
            static const uint8_t k_next[4] = { 1u, 2u, 3u, 0u };   /* focus ids: 0 preset, 1 mode, 2 cutoff, 3 res */
            s_aui.main_focus = k_next[s_aui.main_focus & 3u];
            ui->dirty = 1;
        }
        if (enc_delta != 0) {
            int f = s_aui.main_focus;
            int32_t d = (f == 2 || f == 3) ? accel_delta(enc_delta) : ((enc_delta > 0) ? 1 : -1);
            if (f == 0) {
                alpha_load_preset(s, (uint8_t)wrap_i((int)s->preset_index + (int)d, (int)ALPHA_FACTORY_PRESET_COUNT));
                alpha_sync_chain_routing(s, chain);
            } else if (f == 1) {
                alpha_set_mode(s, (uint8_t)wrap_i((int)s->params.mode + (int)d, 4));
            } else if (f == 2) {
                s->params.cutoff_hz = clampf(s->params.cutoff_hz * powf(1.06f, (float)d), 80.0f, 18000.0f);
                alpha_apply_params(s);
                s->live_cutoff_hz = s->params.cutoff_hz;
            } else {
                s->params.resonance = clampf(s->params.resonance + (float)d * 0.02f, 0.0f, 1.0f);
                alpha_apply_params(s);
            }
            ui->dirty = 1;
        }
        return;
    }

    /* A parameter page */
    uint8_t cat   = s_aui.menu_cat;
    uint8_t nrows = cat_row_count(cat);
    if (s_aui.page_row >= nrows) s_aui.page_row = 0u;

    /* VOL- / VOL+ : nudge the selected row one step, always (no edit mode needed) */
    if (buttons_pressed & (PA_BTN_VOL_MINUS | PA_BTN_VOL_PLUS)) {
        adjust_page_param(s, chain, cat, s_aui.page_row, (buttons_pressed & PA_BTN_VOL_PLUS) ? 1 : -1);
        ui->dirty = 1;
    }
    /* PARA click: step a switch / list row directly, otherwise toggle encoder edit mode */
    if (buttons_pressed & PA_BTN_PARA_CLICK) {
        if (row_is_discrete(cat, s_aui.page_row)) {
            adjust_page_param(s, chain, cat, s_aui.page_row, 1);
        } else {
            s_aui.page_editing ^= 1u;
        }
        ui->dirty = 1;
    }
    if (enc_delta != 0) {
        if (s_aui.page_editing && !row_is_discrete(cat, s_aui.page_row)) {
            adjust_page_param(s, chain, cat, s_aui.page_row, (int)accel_delta(enc_delta));
        } else {
            s_aui.page_editing = 0u;
            s_aui.page_row = (uint8_t)wrap_i((int)s_aui.page_row + ((enc_delta > 0) ? 1 : -1), nrows);
        }
        ui->dirty = 1;
    }
}

/* ============================================================================
 * 1. Main Synthesizer Screen with Live Waveforms, Bode Plot & Oscilloscope
 * ============================================================================ */
/* The eight tabs, current one lit: shown on MAIN so the whole layout is visible, < and > move along it. */
static void draw_tab_bar(uint16_t y) {
    uint8_t cur = current_tab();
    for (uint8_t t = 0u; t < ALPHA_TAB_COUNT; t++) {
        uint16_t bx = (uint16_t)(4u + t * 29u);
        int sel = (t == cur);
        draw_box_border(bx, y, 28, 14, sel ? ALPHA_COL_AMBER_DIM : ALPHA_COL_PANEL,
                        sel ? ALPHA_COL_AMBER : ALPHA_COL_LINE);
        uint16_t tx = (uint16_t)(bx + 14u - (uint16_t)(strlen(k_tab_names[t]) * 3u));
        ui_draw_str(tx, (uint16_t)(y + 4u), k_tab_names[t], sel ? ALPHA_COL_TEXT : ALPHA_COL_MUTE,
                    sel ? ALPHA_COL_AMBER_DIM : ALPHA_COL_PANEL);
    }
}

static void render_alpha_main(const alpha_state_t *s) {
    const alpha_preset_t *p = &s->params;
    char buf[32];
    char *q;

    /* 1. Top Header Bar (Y: 0..26): PROJECT ALPHA + Operating Mode Badge */
    st7735s_fill_rect(0, 0, 240, 26, ALPHA_COL_PANEL);
    st7735s_fill_rect(0, 26, 240, 1, ALPHA_COL_AMBER);
    ui_draw_str(8, 9, "PROJECT ALPHA", ALPHA_COL_AMBER, ALPHA_COL_PANEL);

    /* Mini signal chain badge in header */
    ui_draw_str(96, 9, p->nam_enable ? "NAM" : "---", p->nam_enable ? ALPHA_COL_VIOLET : ALPHA_COL_DISABLED, ALPHA_COL_PANEL);
    ui_draw_str(122, 9, p->cab_enable ? "IR" : "--",  p->cab_enable ? ALPHA_COL_ORANGE : ALPHA_COL_DISABLED, ALPHA_COL_PANEL);

    uint16_t mode_border = (s_aui.main_focus == 1u)
        ? (s_aui.main_editing ? ONYX_COL_OK : ALPHA_COL_AMBER)
        : ALPHA_COL_LINE;
    draw_box_border(156, 3, 78, 20, ALPHA_COL_BG, mode_border);
    q = cat_str(buf, "MODE:");
    cat_str(q, mode_short_str(p->mode));
    ui_draw_str(166, 9, buf, ALPHA_COL_CYAN, ALPHA_COL_BG);

    /* 2. Dual Oscillator & Analog Cards with Embedded Mini Wavelength Graphs (Y: 30..80) */
    const int osc_active = (p->mode == ALPHA_MODE_OSCILLATOR || p->mode == ALPHA_MODE_HYBRID);
    const int is_pure_ana = (p->mode == ALPHA_MODE_ANALOG);
    float anim_ph = (float)(s->ui_anim_tick & 31u) / 32.0f;
    float pw = (s->live_pwm >= 0.05f) ? s->live_pwm : p->pulse_width;

    /* Left Card: OSC A (or POLY ANALOG WAVESHAPER in pure ANALOG mode) */
    draw_box_border(6, 30, 111, 50, ALPHA_COL_PANEL, (osc_active || is_pure_ana) ? ALPHA_COL_AMBER : ALPHA_COL_DISABLED);
    if (is_pure_ana) {
        ui_draw_str(11, 35, "ANALOG", ALPHA_COL_AMBER, ALPHA_COL_PANEL);
        ui_draw_str(11, 49, dist_str(p->dist_mode), ALPHA_COL_TEXT, ALPHA_COL_PANEL);
        q = cat_str(buf, "DRV ");
        cat_pct(q, (int)(p->analog_drive * 100.0f + 0.5f));
        ui_draw_str(11, 63, buf, ALPHA_COL_MUTE, ALPHA_COL_PANEL);
        draw_mini_osc_wave(72, 34, 40, 42, 0, 0.5f, anim_ph, 1, p->dist_mode, p->analog_drive,
                           ALPHA_COL_AMBER, ALPHA_COL_AMBER_DIM);
    } else {
        ui_draw_str(11, 35, "OSC A", osc_active ? ALPHA_COL_AMBER : ALPHA_COL_DISABLED, ALPHA_COL_PANEL);
        q = cat_str(buf, wave_str(p->wave_a));
        q = cat_ch(q, ' ');
        q = cat_i_signed(q, (int)p->pitch_a);
        ui_draw_str(11, 49, buf, osc_active ? ALPHA_COL_TEXT : ALPHA_COL_DISABLED, ALPHA_COL_PANEL);
        q = cat_str(buf, "LVL ");
        cat_pct(q, (int)(p->level_a * 100.0f + 0.5f));
        ui_draw_str(11, 63, buf, ALPHA_COL_MUTE, ALPHA_COL_PANEL);
        draw_mini_osc_wave(72, 34, 40, 42, p->wave_a, pw, anim_ph, 0, 0, 0.0f,
                           osc_active ? ALPHA_COL_AMBER : ALPHA_COL_DISABLED, ALPHA_COL_AMBER_DIM);
    }

    /* Right Card: OSC B (or ENVELOPE / SUB status) */
    draw_box_border(123, 30, 111, 50, ALPHA_COL_PANEL, osc_active ? ALPHA_COL_CYAN : ALPHA_COL_LINE);
    if (is_pure_ana) {
        ui_draw_str(128, 35, "ENVELOPE", ALPHA_COL_CYAN, ALPHA_COL_PANEL);
        q = cat_str(buf, "ATT ");
        q = cat_i(q, (int)p->env_attack_ms);
        cat_str(q, "m");
        ui_draw_str(128, 49, buf, ALPHA_COL_TEXT, ALPHA_COL_PANEL);
        q = cat_str(buf, "REL ");
        q = cat_i(q, (int)p->env_release_ms);
        cat_str(q, "m");
        ui_draw_str(128, 63, buf, ALPHA_COL_MUTE, ALPHA_COL_PANEL);
        draw_mini_osc_wave(189, 34, 40, 42, ALPHA_WAVE_TRIANGLE, 0.5f, anim_ph, 0, 0, 0.0f,
                           ALPHA_COL_CYAN, ALPHA_COL_CYAN_DIM);
    } else {
        ui_draw_str(128, 35, "OSC B", osc_active ? ALPHA_COL_CYAN : ALPHA_COL_DISABLED, ALPHA_COL_PANEL);
        q = cat_str(buf, wave_str(p->wave_b));
        q = cat_ch(q, ' ');
        q = cat_i_signed(q, (int)p->pitch_b);
        ui_draw_str(128, 49, buf, osc_active ? ALPHA_COL_TEXT : ALPHA_COL_DISABLED, ALPHA_COL_PANEL);
        q = cat_str(buf, "SUB ");
        cat_str(q, sub_str(p->sub_mode));
        ui_draw_str(128, 63, buf, ALPHA_COL_MUTE, ALPHA_COL_PANEL);
        draw_mini_osc_wave(189, 34, 40, 42, p->wave_b, pw, anim_ph + 0.25f, 0, 0, 0.0f,
                           osc_active ? ALPHA_COL_CYAN : ALPHA_COL_DISABLED, ALPHA_COL_CYAN_DIM);
    }

    /* 3. Interactive 2D Resonant Filter Bode Response Panel (Y: 84..144) */
    uint16_t filt_border = (s_aui.main_focus == 2u || s_aui.main_focus == 3u)
        ? (s_aui.main_editing ? ONYX_COL_OK : ALPHA_COL_AMBER)
        : ALPHA_COL_LINE;
    draw_box_border(6, 84, 228, 60, ALPHA_COL_PANEL, filt_border);

    q = cat_str(buf, fmode_str(p->filter_mode));
    q = cat_str(q, " ");
    if (p->cutoff_hz >= 1000.0f) {
        int khz_int  = (int)(p->cutoff_hz / 1000.0f);
        int khz_frac = (int)((p->cutoff_hz - (float)(khz_int * 1000)) / 100.0f);
        q = cat_i(q, khz_int);
        q = cat_ch(q, '.');
        q = cat_i(q, khz_frac);
        cat_str(q, "kHz");
    } else {
        q = cat_i(q, (int)p->cutoff_hz);
        cat_str(q, "Hz");
    }
    ui_draw_str(12, 88, buf, (s_aui.main_focus == 2u) ? ALPHA_COL_AMBER : ALPHA_COL_ORANGE, ALPHA_COL_PANEL);

    q = cat_str(buf, "Q:");
    q = cat_pct(q, (int)(p->resonance * 100.0f + 0.5f));
    q = cat_str(q, " E:");
    q = cat_i_signed(q, (int)(p->env_amount * 100.0f));
    cat_ch(q, '%');
    ui_draw_str(132, 88, buf, (s_aui.main_focus == 3u) ? ALPHA_COL_AMBER : ALPHA_COL_TEXT, ALPHA_COL_PANEL);

    /* 2D Bode Frequency Response Curve (216 x 42 px, 54 columns of 4px) */
    draw_filter_bode_box(12, 98, 216, 42, p->filter_mode,
                         p->cutoff_hz, s->live_cutoff_hz, p->resonance, 54);

    /* 4. Pitch Strobe + Live Wavelength Oscilloscope + Preset Panel (Y: 148..206) */
    draw_box_border(6, 148, 228, 58, ALPHA_COL_PANEL,
                    (s_aui.main_focus == 0u) ? (s_aui.main_editing ? ONYX_COL_OK : ALPHA_COL_AMBER) : ALPHA_COL_LINE);

    /* Left side of Panel 4 (X: 12..118): Pitch / Cents Strobe / Glide / Preset */
    if (p->mode == ALPHA_MODE_ANALOG) {
        ui_draw_str(12, 153, "POLY ANALOG", ALPHA_COL_AMBER, ALPHA_COL_PANEL);
        q = cat_str(buf, "IN:");
        q = cat_pct(q, (int)(p->input_gain * 100.0f + 0.5f));
        q = cat_str(q, " W:");
        cat_pct(q, (int)(p->stereo_width * 100.0f + 0.5f));
        ui_draw_str(12, 166, buf, ALPHA_COL_CYAN, ALPHA_COL_PANEL);
    } else {
        char nstr[6];
        fmt_midi_note(s->pitch.midi_note, nstr);
        q = cat_str(buf, "NOTE ");
        cat_str(q, nstr);
        ui_draw_str(12, 153, buf, s->pitch.valid ? ALPHA_COL_CYAN : ALPHA_COL_MUTE, ALPHA_COL_PANEL);

        /* Center-zero Cents Strobe Bar (-50..+50 ct) at X: 66..116, Y: 154 */
        draw_box_border(66, 153, 50, 8, ALPHA_COL_SCOPE_BG, ALPHA_COL_GRID);
        st7735s_fill_rect(91, 153, 1, 8, ALPHA_COL_TEXT); /* Center 0-cent tick */
        if (s->pitch.valid) {
            int c_off = ((int)s->live_pitch_cents * 22) / 50;
            int nx = 90 + c_off;
            if (nx < 68) nx = 68;
            if (nx > 111) nx = 111;
            st7735s_fill_rect((uint16_t)nx, 154, 3, 6,
                              (c_off >= -4 && c_off <= 4) ? ONYX_COL_OK : ALPHA_COL_AMBER);
        }

        q = cat_str(buf, "GLD ");
        q = cat_i(q, (int)p->glide_ms);
        q = cat_str(q, "ms V:");
        cat_i(q, (int)p->vib_depth_cents);
        ui_draw_str(12, 166, buf, ALPHA_COL_VIOLET, ALPHA_COL_PANEL);
    }

    /* Preset Selector Row (Y: 192) */
    q = cat_str(buf, "P");
    q = cat_u2_zero(q, (uint32_t)(s->preset_index + 1u));
    q = cat_ch(q, ':');
    cat_str(q, p->name);
    ui_draw_str(12, 192, buf, (s_aui.main_focus == 0u) ? ALPHA_COL_AMBER : ALPHA_COL_TEXT, ALPHA_COL_PANEL);

    /* Right side of Panel 4 (X: 122..228, Y: 152..188): Real-Time Triggered Wavelength Oscilloscope! */
    draw_wavelength_scope_box(s, 122, 152, 106, 36, 51);

    /* 5. Tab bar (Y: 208) + what the encoder is on / hints (Y: 226) */
    draw_tab_bar(208);
    static const char *const k_enc[4] = { "PRESET", "MODE", "CUTOFF", "RES" };
    q = cat_str(buf, "ENC:");
    q = cat_str(q, k_enc[s_aui.main_focus & 3u]);
    cat_str(q, "  CLK:NEXT  VOL:MASTER");
    ui_draw_str(6, 226, buf, ALPHA_COL_MUTE, ALPHA_COL_BG);
}

/* ============================================================================
 * 3. Dedicated Interactive Instrument Visualizer for Each Parameter Category
 *    Draws inside (X: 6..234, Y: 30..94, W: 228, H: 64)
 * ============================================================================ */
static void render_category_visualizer(const alpha_state_t *s, uint8_t cat) {
    const alpha_preset_t *p = &s->params;
    draw_box_border(6, 30, 228, 64, ALPHA_COL_SCOPE_BG, ALPHA_COL_LINE);

    switch (cat) {
        case ALPHA_CAT_MODE: {
            /* Left (X: 12..82): Nonlinear Waveshaper Transfer Curve y = f(x)
             * Right (X: 88..228): Live Waveshaped Guitar Output Wavelength Trace */
            draw_box_border(10, 34, 72, 56, ALPHA_COL_BG, ALPHA_COL_GRID);
            st7735s_fill_rect(11, 62, 70, 1, ALPHA_COL_GRID);
            st7735s_fill_rect(46, 35, 1, 54, ALPHA_COL_GRID);
            int y_prev = 62;
            for (int i = 0; i < 34; i++) {
                float x_in = -1.0f + 2.0f * ((float)i / 33.0f);
                float y_out = eval_dist_curve(p->dist_mode, x_in, p->analog_drive);
                int y_cur = 62 - (int)(y_out * 24.0f);
                if (i == 0) y_prev = y_cur;
                draw_trace_seg((uint16_t)(12 + i * 2), y_prev, y_cur, 36, 88, 2, ALPHA_COL_AMBER);
                y_prev = y_cur;
            }
            ui_draw_str(14, 37, "XFER", ALPHA_COL_MUTE, ALPHA_COL_BG);

            /* Right: Resulting Waveshaped Wavelength */
            st7735s_fill_rect(88, 62, 138, 1, ALPHA_COL_GRID);
            int yp2 = 62;
            float ph0 = (float)(s->ui_anim_tick & 31u) / 32.0f;
            for (int i = 0; i < 46; i++) {
                float t = (float)i / 45.0f;
                float s_in = sinf((t * 2.0f + ph0) * 6.2831853f) * clampf(p->input_gain * 0.75f, 0.4f, 1.4f);
                float v = eval_dist_curve(p->dist_mode, s_in, p->analog_drive);
                int yc = 62 - (int)(v * 24.0f);
                if (i == 0) yp2 = yc;
                draw_trace_seg((uint16_t)(88 + i * 3), yp2, yc, 35, 89, 3, ALPHA_COL_CYAN);
                yp2 = yc;
            }
            ui_draw_str(92, 35, dist_str(p->dist_mode), ALPHA_COL_AMBER, ALPHA_COL_SCOPE_BG);
            break;
        }

        case ALPHA_CAT_OSCILLATORS: {
            /* Dual-Beam Wavelength Oscilloscope: OSC A (Amber), OSC B (Cyan), SUB (Violet) */
            st7735s_fill_rect(10, 62, 220, 1, ALPHA_COL_GRID);
            st7735s_fill_rect(120, 32, 1, 60, ALPHA_COL_GRID);

            float pw = (s->live_pwm >= 0.05f) ? s->live_pwm : p->pulse_width;
            float ratio_b = clampf(powf(2.0f, ((float)(p->pitch_b - p->pitch_a) + p->detune_cents * 0.01f) / 12.0f), 0.5f, 2.0f);
            float ph0 = (float)(s->ui_anim_tick & 63u) / 64.0f;

            int ypa = 62, ypb = 62, yps = 62;
            for (int i = 0; i < 55; i++) {
                float t = (float)i / 54.0f;
                float wa = eval_osc_wave(p->wave_a, t * 2.0f + ph0, pw) * p->level_a;
                float wb = eval_osc_wave(p->wave_b, t * 2.0f * ratio_b + ph0 + p->detune_cents * 0.008f, pw) * p->level_b;
                int ya = 62 - (int)(wa * 25.0f);
                int yb = 62 - (int)(wb * 25.0f);
                if (i == 0) { ypa = ya; ypb = yb; }
                uint16_t cx = (uint16_t)(10 + i * 4);

                if (p->sub_mode != ALPHA_SUB_OFF && p->sub_level > 0.02f) {
                    float sub_mul = (p->sub_mode == ALPHA_SUB_OCT2) ? 0.5f : 1.0f;
                    float ws = eval_osc_wave(ALPHA_WAVE_SQUARE, t * sub_mul + ph0 * 0.5f, 0.5f) * p->sub_level;
                    int ys = 62 - (int)(ws * 22.0f);
                    if (i == 0) yps = ys;
                    draw_trace_seg(cx, yps, ys, 34, 90, 2, ALPHA_COL_VIOLET);
                    yps = ys;
                }
                draw_trace_seg(cx, ypb, yb, 34, 90, 3, ALPHA_COL_CYAN);
                draw_trace_seg(cx, ypa, ya, 34, 90, 3, ALPHA_COL_AMBER);
                ypa = ya;
                ypb = yb;
            }
            ui_draw_str(12, 34, "A:AMBER  B:CYAN  SUB:VIOLET", ALPHA_COL_MUTE, ALPHA_COL_SCOPE_BG);
            break;
        }

        case ALPHA_CAT_FILTER: {
            /* High-Resolution 2D Resonant Filter Bode Plot with Envelope Sweep Range Bracket */
            draw_filter_bode_box(10, 34, 220, 56, p->filter_mode,
                                 p->cutoff_hz, s->live_cutoff_hz, p->resonance, 55);
            ui_draw_str(14, 36, fmode_str(p->filter_mode), ALPHA_COL_AMBER, ALPHA_COL_SCOPE_BG);
            ui_draw_str(14, 80, "100Hz     1kHz     10kHz", ALPHA_COL_MUTE, ALPHA_COL_SCOPE_BG);
            break;
        }

        case ALPHA_CAT_ENVELOPE: {
            /* Interactive 4-Stage ADSR Vector Curve + Live Playhead Dot */
            st7735s_fill_rect(12, 86, 216, 1, ALPHA_COL_GRID);
            /* Compute proportional horizontal segment widths for Attack, Decay, Sustain, Release */
            uint16_t w_att = (uint16_t)(14.0f + clampf(p->env_attack_ms / 1500.0f, 0.0f, 1.0f) * 48.0f);
            uint16_t w_dec = (uint16_t)(16.0f + clampf(p->env_decay_ms  / 2000.0f, 0.0f, 1.0f) * 46.0f);
            uint16_t w_rel = (uint16_t)(16.0f + clampf(p->env_release_ms / 3000.0f, 0.0f, 1.0f) * 50.0f);
            uint16_t w_sus = (uint16_t)(210u - (w_att + w_dec + w_rel));

            int y_bot  = 86;
            int y_peak = 36;
            int y_sus  = y_bot - (int)(clampf(p->env_sustain, 0.0f, 1.0f) * (float)(y_bot - y_peak));

            uint16_t x0 = 14u;
            uint16_t x1 = (uint16_t)(x0 + w_att);
            uint16_t x2 = (uint16_t)(x1 + w_dec);
            uint16_t x3 = (uint16_t)(x2 + w_sus);
            uint16_t x4 = (uint16_t)(x3 + w_rel);

            int y_prev = y_bot;
            for (int c = 0; c < 54; c++) {
                uint16_t cx = (uint16_t)(x0 + c * 4);
                int y_cur = y_bot;
                uint16_t seg_col = ALPHA_COL_AMBER;
                if (cx <= x1) {
                    float t = (float)(cx - x0) / (float)(w_att ? w_att : 1u);
                    y_cur = y_bot - (int)(t * (float)(y_bot - y_peak));
                    seg_col = ALPHA_COL_AMBER;
                } else if (cx <= x2) {
                    float t = (float)(cx - x1) / (float)(w_dec ? w_dec : 1u);
                    y_cur = y_peak + (int)(t * (float)(y_sus - y_peak));
                    seg_col = ALPHA_COL_ORANGE;
                } else if (cx <= x3) {
                    y_cur = y_sus;
                    seg_col = ALPHA_COL_CYAN;
                } else if (cx <= x4) {
                    float t = (float)(cx - x3) / (float)(w_rel ? w_rel : 1u);
                    y_cur = y_sus + (int)(t * (float)(y_bot - y_sus));
                    seg_col = ALPHA_COL_VIOLET;
                }
                if (c == 0) y_prev = y_cur;
                if (y_bot - y_cur > 1) {
                    st7735s_fill_rect(cx, (uint16_t)(y_cur + 1), 3, (uint16_t)(y_bot - y_cur), ALPHA_COL_AMBER_DIM);
                }
                draw_trace_seg(cx, y_prev, y_cur, 34, 88, 4, seg_col);
                y_prev = y_cur;
            }
            /* Stage vertex markers */
            st7735s_fill_rect((uint16_t)(x1 - 1u), (uint16_t)(y_peak - 1), 4, 4, ALPHA_COL_TEXT);
            st7735s_fill_rect((uint16_t)(x2 - 1u), (uint16_t)(y_sus - 1),  4, 4, ALPHA_COL_TEXT);
            st7735s_fill_rect((uint16_t)(x3 - 1u), (uint16_t)(y_sus - 1),  4, 4, ALPHA_COL_TEXT);
            ui_draw_str(16, 35, "A      D      S      R", ALPHA_COL_MUTE, ALPHA_COL_SCOPE_BG);
            break;
        }

        case ALPHA_CAT_MODULATION: {
            /* Left (X: 10..124): LFO Wave Graph + Live Phase Cursor
             * Right (X: 130..228): Portamento Glide Exponential Pitch Trajectory */
            st7735s_fill_rect(10, 62, 114, 1, ALPHA_COL_GRID);
            st7735s_fill_rect(126, 32, 1, 60, ALPHA_COL_LINE);

            float ph0 = s->lfo.phase;
            float depth_sc = clampf(0.25f + p->lfo_depth * 0.75f, 0.25f, 1.0f);
            int y_prev = 62;
            for (int i = 0; i < 36; i++) {
                float ph = (float)i / 35.0f + ph0;
                ph = ph - (float)((int)ph);
                float v = 0.0f;
                if (p->lfo_wave == ALPHA_LFO_SINE)          v = sinf(ph * 6.2831853f);
                else if (p->lfo_wave == ALPHA_LFO_TRI)      v = (ph < 0.5f) ? (4.0f * ph - 1.0f) : (3.0f - 4.0f * ph);
                else if (p->lfo_wave == ALPHA_LFO_SQUARE)   v = (ph < 0.5f) ? 0.9f : -0.9f;
                else if (p->lfo_wave == ALPHA_LFO_SAW)      v = 2.0f * ph - 1.0f;
                else                                        v = ((int)(ph * 8.0f) & 1) ? 0.75f : -0.65f;

                int yc = 62 - (int)(v * depth_sc * 22.0f);
                if (i == 0) y_prev = yc;
                draw_trace_seg((uint16_t)(12 + i * 3), y_prev, yc, 35, 88, 3, ALPHA_COL_VIOLET);
                y_prev = yc;
            }
            ui_draw_str(14, 35, "LFO WAVE", ALPHA_COL_VIOLET, ALPHA_COL_SCOPE_BG);

            /* Right: Glide Curve */
            ui_draw_str(132, 35, "GLIDE CURVE", ALPHA_COL_CYAN, ALPHA_COL_SCOPE_BG);
            float tau = clampf(0.12f + (p->glide_ms / 800.0f) * 0.65f, 0.08f, 0.80f);
            int ypg = 84;
            for (int i = 0; i < 30; i++) {
                float t = (float)i / 29.0f;
                float g = 1.0f - expf(-t / tau);
                int yg = 84 - (int)(g * 38.0f);
                if (i == 0) ypg = yg;
                draw_trace_seg((uint16_t)(132 + i * 3), ypg, yg, 38, 88, 3, ALPHA_COL_CYAN);
                ypg = yg;
            }
            break;
        }

        case ALPHA_CAT_MIXER: {
            /* 3-Channel Studio Level & Live Peak Bar Meters + Stereo Spread Stage */
            static const char *const k_ch[3] = { "GTR", "ANA", "OSC" };
            const float lvls[3] = { p->guitar_level, p->analog_level, p->synth_level };
            const uint16_t cols[3] = { ALPHA_COL_TEXT, ALPHA_COL_AMBER, ALPHA_COL_CYAN };

            for (int c = 0; c < 3; c++) {
                uint16_t ry = (uint16_t)(36 + c * 14);
                ui_draw_str(12, ry, k_ch[c], cols[c], ALPHA_COL_SCOPE_BG);
                st7735s_fill_rect(38, ry, 186, 9, ALPHA_COL_BG);
                uint16_t bw = (uint16_t)(clampf(lvls[c], 0.0f, 1.0f) * 186.0f);
                st7735s_fill_rect(38, ry, bw, 9, cols[c]);
            }
            /* Stereo Width Center-Outward Bar */
            ui_draw_str(12, 79, "WIDTH", ALPHA_COL_VIOLET, ALPHA_COL_SCOPE_BG);
            st7735s_fill_rect(48, 80, 176, 7, ALPHA_COL_BG);
            uint16_t hw = (uint16_t)(clampf(p->stereo_width, 0.0f, 1.0f) * 86.0f);
            st7735s_fill_rect((uint16_t)(136u - hw), 80, (uint16_t)(hw * 2u + 2u), 7, ALPHA_COL_VIOLET);
            break;
        }

        case ALPHA_CAT_ADVANCED:
        default: {
            /* Interactive Signal Routing Pipeline Diagram:
             * [GTR] -> [SYNTH] -> [NAM AMP] -> [IR CAB] -> [GATE] */
            static const char *const k_blk[5] = { "GTR", "SYN", "NAM", "CAB", "GAT" };
            const int act[5] = {
                1,
                1,
                (int)p->nam_enable,
                (int)p->cab_enable,
                (int)p->gate_enable
            };
            const uint16_t bcol[5] = {
                ALPHA_COL_TEXT, ALPHA_COL_AMBER, ALPHA_COL_VIOLET, ALPHA_COL_ORANGE, ALPHA_COL_CYAN
            };
            ui_draw_str(14, 36, "SIGNAL ROUTING PIPELINE", ALPHA_COL_MUTE, ALPHA_COL_SCOPE_BG);
            for (int b = 0; b < 5; b++) {
                uint16_t bx = (uint16_t)(12 + b * 44);
                draw_box_border(bx, 50, 36, 26,
                                act[b] ? ALPHA_COL_PANEL_HI : ALPHA_COL_BG,
                                act[b] ? bcol[b] : ALPHA_COL_DISABLED);
                ui_draw_str((uint16_t)(bx + 9), 59, k_blk[b],
                            act[b] ? bcol[b] : ALPHA_COL_DISABLED,
                            act[b] ? ALPHA_COL_PANEL_HI : ALPHA_COL_BG);
                if (b < 4) {
                    st7735s_fill_rect((uint16_t)(bx + 36), 62, 8, 2, ALPHA_COL_AMBER);
                }
            }
            break;
        }
    }
}

static void format_page_row(const alpha_state_t *s, uint8_t cat, uint8_t row,
                            char label[20], char val[20], float *out_norm) {
    const alpha_preset_t *p = &s->params;
    char *q;
    label[0]  = '\0';
    val[0]    = '\0';
    *out_norm = -1.0f; /* -1 means discrete enum/toggle (no continuous bar) */

    switch (cat) {
        case ALPHA_CAT_MODE:
            if (row == 0u)      { cat_str(label, "OP MODE");      cat_str(val, mode_full_str(p->mode)); }
            else if (row == 1u) { cat_str(label, "DISTORTION");   cat_str(val, dist_str(p->dist_mode)); }
            else if (row == 2u) { cat_str(label, "ANALOG DRIVE"); cat_pct(val, (int)(p->analog_drive * 100.0f + 0.5f)); *out_norm = p->analog_drive; }
            else if (row == 3u) { cat_str(label, "INPUT GAIN");   cat_pct(val, (int)(p->input_gain * 100.0f + 0.5f));   *out_norm = (p->input_gain - 0.5f) / 2.0f; }
            else if (row == 4u) { cat_str(label, "PRE-EMPHASIS"); cat_str(val, p->pre_emphasis ? "ON" : "OFF"); }
            break;

        case ALPHA_CAT_OSCILLATORS:
            if (row == 0u)       { cat_str(label, "OSC A WAVE");   cat_str(val, wave_str(p->wave_a)); }
            else if (row == 1u)  { cat_str(label, "OSC A PITCH");  q = cat_i_signed(val, (int)p->pitch_a); cat_str(q, " ST"); *out_norm = ((float)p->pitch_a + 24.0f) / 48.0f; }
            else if (row == 2u)  { cat_str(label, "OSC A LEVEL");  cat_pct(val, (int)(p->level_a * 100.0f + 0.5f)); *out_norm = p->level_a; }
            else if (row == 3u)  { cat_str(label, "OSC B WAVE");   cat_str(val, wave_str(p->wave_b)); }
            else if (row == 4u)  { cat_str(label, "OSC B PITCH");  q = cat_i_signed(val, (int)p->pitch_b); cat_str(q, " ST"); *out_norm = ((float)p->pitch_b + 24.0f) / 48.0f; }
            else if (row == 5u)  { cat_str(label, "OSC B LEVEL");  cat_pct(val, (int)(p->level_b * 100.0f + 0.5f)); *out_norm = p->level_b; }
            else if (row == 6u)  { cat_str(label, "A/B BLEND");    cat_pct(val, (int)(p->osc_mix * 100.0f + 0.5f)); *out_norm = p->osc_mix; }
            else if (row == 7u)  { cat_str(label, "DETUNE");       q = cat_i(val, (int)(p->detune_cents + 0.5f)); cat_str(q, " ct"); *out_norm = p->detune_cents / 50.0f; }
            else if (row == 8u)  { cat_str(label, "FINE TUNE");    q = cat_i_signed(val, (int)p->fine_tune); cat_str(q, " ct"); *out_norm = ((float)p->fine_tune + 50.0f) / 100.0f; }
            else if (row == 9u)  { cat_str(label, "PULSE WIDTH");  cat_pct(val, (int)(p->pulse_width * 100.0f + 0.5f)); *out_norm = p->pulse_width; }
            else if (row == 10u) { cat_str(label, "SUB OCTAVE");   cat_str(val, sub_str(p->sub_mode)); }
            else if (row == 11u) { cat_str(label, "SUB LEVEL");    cat_pct(val, (int)(p->sub_level * 100.0f + 0.5f)); *out_norm = p->sub_level; }
            break;

        case ALPHA_CAT_FILTER:
            if (row == 0u)      { cat_str(label, "FILTER MODE");  cat_str(val, fmode_str(p->filter_mode)); }
            else if (row == 1u) { cat_str(label, "CUTOFF FREQ");  q = cat_i(val, (int)p->cutoff_hz); cat_str(q, " Hz"); *out_norm = log2f(p->cutoff_hz / 80.0f) / 7.8138f; }
            else if (row == 2u) { cat_str(label, "RESONANCE");    cat_pct(val, (int)(p->resonance * 100.0f + 0.5f)); *out_norm = p->resonance; }
            else if (row == 3u) { cat_str(label, "ENV AMOUNT");   q = cat_i_signed(val, (int)(p->env_amount * 100.0f)); cat_ch(q, '%'); *out_norm = (p->env_amount + 1.0f) * 0.5f; }
            else if (row == 4u) { cat_str(label, "KEY TRACK");    cat_pct(val, (int)(p->key_tracking * 100.0f + 0.5f)); *out_norm = p->key_tracking; }
            else if (row == 5u) { cat_str(label, "SATURATION");   cat_pct(val, (int)(p->filter_sat * 100.0f + 0.5f)); *out_norm = p->filter_sat; }
            break;

        case ALPHA_CAT_ENVELOPE:
            if (row == 0u)      { cat_str(label, "ATTACK");       q = cat_i(val, (int)p->env_attack_ms); cat_str(q, " ms");  *out_norm = p->env_attack_ms / 2000.0f; }
            else if (row == 1u) { cat_str(label, "DECAY");        q = cat_i(val, (int)p->env_decay_ms); cat_str(q, " ms");   *out_norm = p->env_decay_ms / 3000.0f; }
            else if (row == 2u) { cat_str(label, "SUSTAIN");      cat_pct(val, (int)(p->env_sustain * 100.0f + 0.5f));       *out_norm = p->env_sustain; }
            else if (row == 3u) { cat_str(label, "RELEASE");      q = cat_i(val, (int)p->env_release_ms); cat_str(q, " ms"); *out_norm = p->env_release_ms / 5000.0f; }
            break;

        case ALPHA_CAT_MODULATION:
            if (row == 0u)      { cat_str(label, "GLIDE TIME");   q = cat_i(val, (int)p->glide_ms); cat_str(q, " ms"); *out_norm = p->glide_ms / 2000.0f; }
            else if (row == 1u) { cat_str(label, "GLIDE MODE");   cat_str(val, p->glide_legato ? "LEGATO" : "ALWAYS"); }
            else if (row == 2u) {
                cat_str(label, "VIB RATE");
                q = cat_i(val, (int)p->vib_rate_hz);
                q = cat_ch(q, '.');
                q = cat_i(q, (int)(p->vib_rate_hz * 10.0f) % 10);
                cat_str(q, " Hz");
                *out_norm = p->vib_rate_hz / 12.0f;
            }
            else if (row == 3u) { cat_str(label, "VIB DEPTH");    q = cat_i(val, (int)p->vib_depth_cents); cat_str(q, " ct"); *out_norm = p->vib_depth_cents / 100.0f; }
            else if (row == 4u) { cat_str(label, "VIB DELAY");    q = cat_i(val, (int)p->vib_delay_ms); cat_str(q, " ms"); *out_norm = p->vib_delay_ms / 2000.0f; }
            else if (row == 5u) { cat_str(label, "LFO WAVE");     cat_str(val, lfo_wave_str(p->lfo_wave)); }
            else if (row == 6u) {
                cat_str(label, "LFO RATE");
                q = cat_i(val, (int)p->lfo_rate_hz);
                q = cat_ch(q, '.');
                q = cat_i(q, (int)(p->lfo_rate_hz * 10.0f) % 10);
                cat_str(q, " Hz");
                *out_norm = p->lfo_rate_hz / 20.0f;
            }
            else if (row == 7u) { cat_str(label, "LFO DEPTH");    cat_pct(val, (int)(p->lfo_depth * 100.0f + 0.5f)); *out_norm = p->lfo_depth; }
            else if (row == 8u) { cat_str(label, "LFO TARGET");   cat_str(val, lfo_dest_str(p->lfo_dest)); }
            break;

        case ALPHA_CAT_MIXER:
            if (row == 0u)      { cat_str(label, "DIRECT GUITAR");cat_pct(val, (int)(p->guitar_level * 100.0f + 0.5f)); *out_norm = p->guitar_level; }
            else if (row == 1u) { cat_str(label, "ANALOG SYNTH"); cat_pct(val, (int)(p->analog_level * 100.0f + 0.5f)); *out_norm = p->analog_level; }
            else if (row == 2u) { cat_str(label, "OSC SYNTH");    cat_pct(val, (int)(p->synth_level * 100.0f + 0.5f));  *out_norm = p->synth_level; }
            else if (row == 3u) { cat_str(label, "MASTER LEVEL"); cat_pct(val, (int)(p->master_level * 100.0f + 0.5f)); *out_norm = p->master_level; }
            else if (row == 4u) { cat_str(label, "STEREO WIDTH"); cat_pct(val, (int)(p->stereo_width * 100.0f + 0.5f)); *out_norm = p->stereo_width; }
            break;

        case ALPHA_CAT_ADVANCED:
            if (row == 0u)      { cat_str(label, "NAM AMPLIFIER");cat_str(val, p->nam_enable ? "ENABLED" : "BYPASSED"); }
            else if (row == 1u) { cat_str(label, "IR CABINET");   cat_str(val, p->cab_enable ? "ENABLED" : "BYPASSED"); }
            else if (row == 2u) { cat_str(label, "SYNTH GATE");   cat_str(val, p->gate_enable ? "ENABLED" : "BYPASSED"); }
            else if (row == 3u) {
                cat_str(label, "PRESET");
                q = cat_u2_zero(val, (uint32_t)(s->preset_index + 1u));
                q = cat_ch(q, ' ');
                cat_str(q, p->name);
            }
            break;
    }
}

static void render_alpha_page(const alpha_state_t *s) {
    static const char *const k_titles[ALPHA_CAT_COUNT] = {
        "MODE & ANALOG", "OSCILLATORS", "SYNTH FILTER",
        "ENVELOPE ADSR", "MODULATION", "3-WAY MIXER", "STAGE & PRESET"
    };
    st7735s_fill_rect(0, 0, 240, 26, ALPHA_COL_PANEL);
    st7735s_fill_rect(0, 26, 240, 1, ALPHA_COL_AMBER);
    ui_draw_str(10, 9, k_titles[s_aui.menu_cat % ALPHA_CAT_COUNT], ALPHA_COL_AMBER, ALPHA_COL_PANEL);
    {
        /* position in the tab strip (8 ticks) and in the row list (n/N) */
        for (uint8_t t = 0u; t < ALPHA_TAB_COUNT; t++) {
            st7735s_fill_rect((uint16_t)(116u + t * 8u), 11, 6, 5,
                              (t == current_tab()) ? ALPHA_COL_AMBER : ALPHA_COL_LINE);
        }
        char rb[12];
        char *rq = cat_u(rb, (uint32_t)s_aui.page_row + 1u);
        rq = cat_ch(rq, '/');
        cat_u(rq, (uint32_t)cat_row_count(s_aui.menu_cat));
        ui_draw_str((uint16_t)(236u - strlen(rb) * 6u), 9, rb, ALPHA_COL_TEXT, ALPHA_COL_PANEL);
    }

    /* Top Interactive Category Visualizer (Y: 30..94) */
    render_category_visualizer(s, s_aui.menu_cat);

    /* 4 Scrollable Parameter Cards Below Visualizer (Y: 98..234) */
    uint8_t nrows = cat_row_count(s_aui.menu_cat);
    uint8_t top = 0u;
    if (s_aui.page_row >= 4u) top = (uint8_t)(s_aui.page_row - 3u);

    for (uint8_t i = 0u; i < 4u && (top + i) < nrows; i++) {
        uint8_t r = (uint8_t)(top + i);
        uint16_t y = (uint16_t)(98 + i * 31);
        int sel = (s_aui.page_row == r);
        uint16_t border = sel ? (s_aui.page_editing ? ONYX_COL_OK : ALPHA_COL_AMBER) : ALPHA_COL_LINE;
        uint16_t bg     = sel ? ALPHA_COL_PANEL_HI : ALPHA_COL_PANEL;

        draw_box_border(6, y, 228, 28, bg, border);
        char lbl[20], val[20];
        float norm_bar = -1.0f;
        format_page_row(s, s_aui.menu_cat, r, lbl, val, &norm_bar);
        ui_draw_str(14, (uint16_t)(y + 5), lbl, sel ? ALPHA_COL_AMBER : ALPHA_COL_MUTE, bg);
        uint16_t vx = (uint16_t)(226 - strlen(val) * 6);
        ui_draw_str(vx, (uint16_t)(y + 5), val, sel ? ALPHA_COL_TEXT : ALPHA_COL_CYAN, bg);

        /* Continuous fill bar along bottom of parameter card */
        if (norm_bar >= 0.0f) {
            norm_bar = clampf(norm_bar, 0.0f, 1.0f);
            st7735s_fill_rect(14, (uint16_t)(y + 18), 212, 4, ALPHA_COL_BG);
            uint16_t fw = (uint16_t)(norm_bar * 212.0f);
            if (fw > 0u) {
                st7735s_fill_rect(14, (uint16_t)(y + 18), fw, 4, sel ? ALPHA_COL_AMBER : ALPHA_COL_CYAN);
            }
        }
    }

    /* Hint line: what the buttons do right now */
    ui_draw_str(6, 224, row_is_discrete(s_aui.menu_cat, s_aui.page_row)
                        ? "CLK:NEXT VOL:+/- <>:PAGE HOME:BACK"
                        : (s_aui.page_editing ? "ENC:ADJUST CLK:DONE VOL:+/-"
                                              : "ENC:ROW CLK:EDIT VOL:+/- <>:PAGE"),
                ALPHA_COL_MUTE, ALPHA_COL_BG);
}

void alpha_ui_render(const pa_ui_ctx_t *ui, const pa_fx_chain_t *chain) {
    (void)ui; (void)chain;
    st7735s_fill_rect(0, 0, 240, 240, ALPHA_COL_BG);
    const alpha_state_t *s = alpha_get_active();
    if (!s) return;

    if (s_aui.screen == ALPHA_SCREEN_PAGE) {
        render_alpha_page(s);
    } else {
        render_alpha_main(s);
    }
}
