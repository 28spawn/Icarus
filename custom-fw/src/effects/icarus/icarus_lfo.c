/* custom-fw/src/effects/icarus/icarus_lfo.c — Glide, Independent Vibrato, Modulation LFO & Expression */
#include "icarus_lfo.h"
#include "pa_config.h"
#include <math.h>
#include "icarus_fastmath.h"
#include <string.h>

#define ICARUS_FS ((float)PA_SAMPLE_RATE)

static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* Fast parabolic sine approximation on phase p in [0, 1) -> [-1, +1] (zero trig calls) */
static inline float fast_sine_01(float p) {
    p -= floorf(p);
    float x = p * 2.0f - 1.0f; /* -1 .. +1 */
    /* Map to sin(pi * x): 4*x*(1 - |x|) with 0.225 precision refinement */
    float y = -4.0f * x * (1.0f - fabsf(x));
    return y * (0.775f + 0.225f * fabsf(y));
}

/* Fast 2^x. The glide runs in ABSOLUTE octaves (log2 of the pitch in Hz: 110 Hz = 6.8, 2 kHz = 11),
 * so the clamp must cover that range; the old +-4 clamp pinned every glided pitch to ~16 Hz. */
static inline float fast_exp2f_mod(float x) {
    if (x < -8.0f) x = -8.0f;
    if (x > 16.0f) x = 16.0f;
    int32_t i = (int32_t)floorf(x);
    float f = x - (float)i;
    float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * 0.00961813f)));
    union { uint32_t u; float f; } scale;
    scale.u = (uint32_t)((i + 127) << 23);
    return scale.f * p;
}

static inline float fast_log2f_mod(float x) {
    if (x < 1.0f) x = 1.0f;
    union { float f; uint32_t u; } v;
    v.f = x;
    int32_t exp = (int32_t)((v.u >> 23) & 0xFFu) - 127;
    v.u = (v.u & 0x7FFFFFu) | (127u << 23);
    float m = v.f - 1.0f;
    float frac = m * (1.4425449f - m * (0.7181452f - m * 0.2756003f));
    return (float)exp + frac;
}

void icarus_glide_init(icarus_glide_t *g) {
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->time_ms     = 45.0f;
    g->mode        = ICARUS_GLIDE_LEGATO;
    g->curve       = ICARUS_GLIDE_CURVE_EXP;
    g->direction   = ICARUS_GLIDE_DIR_BOTH;
    g->sensitivity = 0.85f;
    g->current_hz  = 110.0f;
    g->current_oct = fast_log2f_mod(110.0f);
    g->initialized = 0u;
}

void icarus_glide_set_params(icarus_glide_t *g, float time_ms, icarus_glide_mode_t mode,
                            icarus_glide_curve_t curve, icarus_glide_dir_t dir, float sens_01) {
    if (!g) return;
    g->time_ms     = clampf(time_ms, 0.0f, 2000.0f);
    g->mode        = mode;
    g->curve       = curve;
    g->direction   = dir;
    g->sensitivity = clampf(sens_01, 0.0f, 1.0f);
}

float icarus_glide_step_block(icarus_glide_t *g, float target_hz, int note_onset, int is_legato, int block_samples) {
    if (!g) return target_hz;
    target_hz = clampf(target_hz, 30.0f, 2000.0f);
    float target_oct = fast_log2f_mod(target_hz);

    if (!g->initialized || g->time_ms < 0.5f) {
        g->current_hz  = target_hz;
        g->current_oct = target_oct;
        g->initialized = 1u;
        return g->current_hz;
    }

    /* Legato-only mode snaps immediately on freshly picked notes from silence */
    if (note_onset && g->mode == ICARUS_GLIDE_LEGATO && !is_legato) {
        g->current_hz  = target_hz;
        g->current_oct = target_oct;
        return g->current_hz;
    }

    /* Direction filtering (Up-only or Down-only) */
    float diff_oct = target_oct - g->current_oct;
    if ((g->direction == ICARUS_GLIDE_DIR_UP   && diff_oct < 0.0f) ||
        (g->direction == ICARUS_GLIDE_DIR_DOWN && diff_oct > 0.0f)) {
        g->current_hz  = target_hz;
        g->current_oct = target_oct;
        return g->current_hz;
    }

    float blk_ms = ((float)block_samples * 1000.0f) * (1.0f / ICARUS_FS);
    float eff_time_ms = g->time_ms * (0.25f + 0.75f * g->sensitivity) * ((g->vel_scale > 0.0f) ? g->vel_scale : 1.0f);
    if (eff_time_ms < 0.5f) {
        g->current_oct = target_oct;
    } else if (g->curve == ICARUS_GLIDE_CURVE_EXP) {
        /* Exponential pitch glide in octave space (reach ~95% in eff_time_ms) */
        float tau_ms = eff_time_ms * 0.33f;
        float alpha  = blk_ms / (tau_ms + blk_ms);
        g->current_oct += alpha * diff_oct;
    } else {
        /* Linear constant-rate glide (1 octave per eff_time_ms) */
        float max_step = blk_ms / eff_time_ms;
        if (fabsf(diff_oct) <= max_step) {
            g->current_oct = target_oct;
        } else {
            g->current_oct += (diff_oct > 0.0f) ? max_step : -max_step;
        }
    }

    g->current_hz = fast_exp2f_mod(g->current_oct);
    return g->current_hz;
}

void icarus_vibrato_init(icarus_vibrato_t *v) {
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->rate_hz     = 5.5f;
    v->depth_cents = 14.0f;
    v->delay_ms    = 220.0f;
    v->fade_ms     = 350.0f;
    v->waveform    = ICARUS_VIB_SINE;
    v->expr_sens   = 0.35f;
    v->pitch_ratio = 1.0f;
}

void icarus_vibrato_set_params(icarus_vibrato_t *v, float rate_hz, float depth_cents,
                              float delay_ms, float fade_ms, icarus_vib_wave_t wave, float expr_sens_01) {
    if (!v) return;
    v->rate_hz     = clampf(rate_hz, 0.1f, 12.0f);
    v->depth_cents = clampf(depth_cents, 0.0f, 100.0f);
    v->delay_ms    = clampf(delay_ms, 0.0f, 2000.0f);
    v->fade_ms     = clampf(fade_ms, 0.0f, 2000.0f);
    v->waveform    = wave;
    v->expr_sens   = clampf(expr_sens_01, 0.0f, 1.0f);
}

void icarus_vibrato_trigger(icarus_vibrato_t *v) {
    if (!v) return;
    v->note_age_ms = 0.0f;
    v->fade_env    = 0.0f;
}

float icarus_vibrato_step_block(icarus_vibrato_t *v, float input_env_01, int block_samples) {
    if (!v) return 1.0f;
    float blk_sec = (float)block_samples * (1.0f / ICARUS_FS);
    float blk_ms  = blk_sec * 1000.0f;

    v->note_age_ms += blk_ms;
    if (v->note_age_ms < v->delay_ms) {
        v->fade_env = 0.0f;
    } else if (v->fade_ms <= 1.0f) {
        v->fade_env = 1.0f;
    } else {
        v->fade_env = clampf((v->note_age_ms - v->delay_ms) / v->fade_ms, 0.0f, 1.0f);
    }

    v->phase += v->rate_hz * blk_sec;
    v->phase -= floorf(v->phase);

    float wave = 0.0f;
    if (v->waveform == ICARUS_VIB_TRI) {
        wave = 1.0f - 4.0f * fabsf(v->phase - 0.5f);
    } else {
        wave = fast_sine_01(v->phase);
    }

    /* Expression sensitivity scales vibrato depth by sustained guitar level */
    float expr_scale = (1.0f - v->expr_sens) + v->expr_sens * clampf(input_env_01 * 1.6f, 0.0f, 1.25f);
    v->current_cents = wave * v->depth_cents * v->fade_env * expr_scale;
    v->pitch_ratio   = fast_exp2f_mod(v->current_cents * (1.0f / 1200.0f));
    return v->pitch_ratio;
}

void icarus_lfo_init(icarus_lfo_t *l) {
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->rate_hz   = 1.2f;
    l->depth_01  = 0.0f;
    l->waveform  = ICARUS_LFO_SINE;
    l->dest      = ICARUS_LFO_DEST_FILTER_CUTOFF;
    l->rng_state = 0x9E3779B9u;
}

void icarus_lfo_set_params(icarus_lfo_t *l, float rate_hz, float depth_01,
                          icarus_lfo_wave_t wave, icarus_lfo_dest_t dest) {
    if (!l) return;
    l->rate_hz  = clampf(rate_hz, 0.05f, 20.0f);
    l->depth_01 = clampf(depth_01, 0.0f, 1.0f);
    l->waveform = wave;
    l->dest     = dest;
}

void icarus_lfo_step_block(icarus_lfo_t *l, int block_samples) {
    if (!l) return;
    float blk_sec = (float)block_samples * (1.0f / ICARUS_FS);
    float prev_ph = l->phase;
    l->phase += l->rate_hz * blk_sec;
    if (l->phase >= 1.0f) {
        l->phase -= floorf(l->phase);
        /* Step Sample & Hold on each cycle wrap */
        l->rng_state = l->rng_state * 1664525u + 1013904223u;
        l->sh_val    = (float)(int32_t)l->rng_state * (1.0f / 2147483648.0f);
    } else if (prev_ph == 0.0f && l->sh_val == 0.0f) {
        l->rng_state = l->rng_state * 1664525u + 1013904223u;
        l->sh_val    = (float)(int32_t)l->rng_state * (1.0f / 2147483648.0f);
    }

    /* Smooth S&H slightly (~3 ms) so stepped filter transitions never click */
    l->sh_smooth += 0.35f * (l->sh_val - l->sh_smooth);

    float w = 0.0f;
    switch (l->waveform) {
        case ICARUS_LFO_SINE:
            w = fast_sine_01(l->phase);
            break;
        case ICARUS_LFO_TRI:
            w = 1.0f - 4.0f * fabsf(l->phase - 0.5f);
            break;
        case ICARUS_LFO_SQUARE:
            w = (l->phase < 0.5f) ? 1.0f : -1.0f;
            break;
        case ICARUS_LFO_SAW:
            w = 2.0f * l->phase - 1.0f;
            break;
        case ICARUS_LFO_SH:
        default:
            w = l->sh_smooth;
            break;
    }

    l->out_bipolar  = w * l->depth_01;
    l->out_unipolar = 0.5f * (w + 1.0f) * l->depth_01;
}

void icarus_compute_mod_frame(icarus_mod_frame_t *out,
                             icarus_glide_t *glide,
                             icarus_vibrato_t *vib,
                             icarus_lfo_t *lfo,
                             float tracked_freq_hz,
                             int note_onset,
                             int is_legato,
                             float input_env_01,
                             float pick_velocity_01,
                             int block_samples) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->osc_amp_scale = 1.0f;

    if (note_onset && vib && !is_legato) {
        icarus_vibrato_trigger(vib);
    }

    if (glide) glide->vel_scale = clampf(1.30f - 0.55f * pick_velocity_01, 0.75f, 1.30f);
    float glided_hz = glide ? icarus_glide_step_block(glide, tracked_freq_hz, note_onset, is_legato, block_samples)
                            : tracked_freq_hz;
    float vib_ratio = vib   ? icarus_vibrato_step_block(vib, input_env_01, block_samples)
                            : 1.0f;

    if (lfo) {
        icarus_lfo_step_block(lfo, block_samples);
        switch (lfo->dest) {
            case ICARUS_LFO_DEST_FILTER_CUTOFF:
                out->filter_cutoff_oct = lfo->out_bipolar * 2.5f; /* +/- 2.5 octaves max */
                break;
            case ICARUS_LFO_DEST_OSC_PITCH:
                vib_ratio *= fast_exp2f_mod(lfo->out_bipolar * (2.0f / 12.0f)); /* +/- 2 semitones max */
                break;
            case ICARUS_LFO_DEST_PULSE_WIDTH:
                out->pulse_width_mod = lfo->out_bipolar * 0.40f;
                break;
            case ICARUS_LFO_DEST_OSC_AMP:
                out->osc_amp_scale = clampf(1.0f - lfo->out_unipolar * 0.85f, 0.10f, 1.25f);
                break;
            case ICARUS_LFO_DEST_STEREO_PAN:
                out->stereo_pan_offset = lfo->out_bipolar * 0.45f;
                break;
        }
    }

    /* Expression matrix: picking strength & input envelope naturally enrich tone */
    out->glided_freq_hz     = clampf(glided_hz * vib_ratio, 25.0f, 4000.0f);
    out->distortion_boost   = clampf(pick_velocity_01 * 0.25f, 0.0f, 0.30f);
    out->synth_blend_offset = clampf((input_env_01 - 0.35f) * 0.15f, -0.10f, 0.15f);
}
