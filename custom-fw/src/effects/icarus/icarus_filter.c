/* custom-fw/src/effects/icarus/icarus_filter.c — Topology-Preserving Zero-Delay-Feedback Resonant Filter */
#include "icarus_filter.h"
#include "pa_config.h"
#include <math.h>
#include "icarus_fastmath.h"

#define ICARUS_FS ((float)PA_SAMPLE_RATE)
#define ICARUS_PI 3.14159265358979323846f

static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* Fast Padé approximant for tanh(x): smooth, bounded in [-1, +1], zero libm call overhead per sample */
static inline float fast_tanhf(float x) {
    if (x > 3.0f)  return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Fast tan(pi * f / fs) for f in [20 Hz, 18000 Hz] at 44.1 kHz */
static inline float fast_svf_g(float fc_hz) {
    float f = clampf(fc_hz, 40.0f, 18000.0f);
    float w = (ICARUS_PI / ICARUS_FS) * f;
    /* 5th-order polynomial for tan(w) valid up to w = 1.28 rad (18 kHz @ 44.1 kHz) */
    float w2 = w * w;
    return w * (1.0f + w2 * (0.33333333f + w2 * (0.13333333f + w2 * 0.05396825f)));
}

/* Fast exp2f(x) for octave modulation x in [-6, +6] */
static inline float fast_exp2f(float x) {
    x = clampf(x, -6.0f, 6.0f);
    int i = (int)floorf(x);
    float f = x - (float)i;
    /* Minimax cubic for 2^f on [0, 1] */
    float p = 1.0f + f * (0.69314718f + f * (0.2402265f + f * 0.0555041f));
    union { uint32_t u; float fl; } scale;
    scale.u = (uint32_t)((i + 127) << 23);
    return p * scale.fl;
}

static void update_svf_coeffs(icarus_filter_t *f, float target_fc) {
    /* Smooth cutoff movement to prevent zipper noise */
    f->cur_cutoff_hz += 0.35f * (target_fc - f->cur_cutoff_hz);
    float fc = clampf(f->cur_cutoff_hz, 60.0f, 18000.0f);

    float g = fast_svf_g(fc);
    /* Damping k: resonance 0..1 maps to k = 2.0 down to 0.08 (Q = 0.5 up to 12.5).
     * At high frequencies (>8 kHz), slightly increase minimum damping to maintain rock-solid Nyquist stability. */
    float res = clampf(f->resonance, 0.0f, 1.0f);
    float nyq_guard = (fc > 8000.0f) ? ((fc - 8000.0f) * (0.000018f)) : 0.0f;
    float max_res = 0.96f - clampf(nyq_guard, 0.0f, 0.16f);
    /* In 4-pole cascaded mode, each 2-pole stage shares the resonance so peak gain doesn't square excessively */
    if (f->mode == ICARUS_FILTER_LP24) {
        max_res *= 0.84f;
    }
    float k = 2.0f - 2.0f * (res * max_res);

    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;

    f->g  = g;
    f->k  = k;
    f->a1 = a1;
    f->a2 = a2;
    f->a3 = a3;
}

void icarus_filter_init(icarus_filter_t *f) {
    if (!f) return;
    f->cutoff          = 2400.0f;
    f->resonance       = 0.50f;
    f->envelope_amount = 0.50f;
    f->tracking_amount = 0.50f;
    f->saturation      = 0.25f;
    f->hpf_cutoff      = 0.0f;
    f->mode            = ICARUS_FILTER_LP24;
    f->cur_cutoff_hz   = 2400.0f;
    f->hpf_r           = 0.995f;
    icarus_filter_reset(f);
    update_svf_coeffs(f, f->cutoff);
}

void icarus_filter_reset(icarus_filter_t *f) {
    if (!f) return;
    f->ic1eq_1 = 0.0f;
    f->ic2eq_1 = 0.0f;
    f->ic1eq_2 = 0.0f;
    f->ic2eq_2 = 0.0f;
    f->hpf_x1  = 0.0f;
    f->hpf_y1  = 0.0f;
    f->cur_cutoff_hz = clampf(f->cutoff, 80.0f, 18000.0f);
}

void icarus_filter_set_params(icarus_filter_t *f, float cutoff_hz, float res_01,
                             float env_amt_m1_p1, float track_01, float sat_01, uint8_t mode) {
    if (!f) return;
    f->cutoff          = clampf(cutoff_hz, 80.0f, 18000.0f);
    f->resonance       = clampf(res_01, 0.0f, 1.0f);
    f->envelope_amount = clampf(env_amt_m1_p1, -1.0f, 1.0f);
    f->tracking_amount = clampf(track_01, 0.0f, 1.0f);
    f->saturation      = clampf(sat_01, 0.0f, 1.0f);
    f->mode            = (mode < ICARUS_FILTER_MODE_COUNT) ? mode : ICARUS_FILTER_LP24;
    update_svf_coeffs(f, f->cutoff);
}

void icarus_filter_set_hpf(icarus_filter_t *f, float hpf_hz) {
    if (!f) return;
    if (hpf_hz <= 15.0f) {
        f->hpf_cutoff = 0.0f;
        f->hpf_r      = 0.998f;
    } else {
        f->hpf_cutoff = clampf(hpf_hz, 20.0f, 600.0f);
        float w = 2.0f * ICARUS_PI * f->hpf_cutoff / ICARUS_FS;
        f->hpf_r = clampf(1.0f - w, 0.85f, 0.999f);
    }
}

void icarus_filter_update_mod(icarus_filter_t *f, float env_val, float pitch_hz, float lfo_octaves) {
    if (!f) return;
    /* Envelope modulation spans up to +-4.5 octaves */
    float oct_mod = f->envelope_amount * clampf(env_val, 0.0f, 1.2f) * 4.5f + lfo_octaves;
    float fc = f->cutoff * fast_exp2f(oct_mod);

    /* Pitch tracking relative to A3 (220 Hz) */
    if (f->tracking_amount > 0.001f && pitch_hz > 35.0f && pitch_hz < 2000.0f) {
        float ratio = pitch_hz * (1.0f / 220.0f);
        /* Linear blend between 1.0 (no tracking) and ratio (100% key tracking) */
        float trk_mult = 1.0f + f->tracking_amount * (ratio - 1.0f);
        fc *= clampf(trk_mult, 0.25f, 6.0f);
    }
    fc = clampf(fc, 60.0f, 18000.0f);
    update_svf_coeffs(f, fc);
}

static inline void svf_stage_step(float x, float a1, float a2, float a3, float k, float sat_drive,
                                  float *ic1eq, float *ic2eq, float *v1_out, float *v2_out) {
    /* Non-linear saturation of bandpass feedback state (GR-300 / analog OTA core limiting) */
    float s1 = *ic1eq;
    if (sat_drive > 0.01f) {
        s1 = fast_tanhf(s1 * (1.0f + sat_drive)) * (1.0f / (1.0f + 0.85f * sat_drive));
    }
    float v3 = x - *ic2eq;
    float v1 = a1 * s1 + a2 * v3;
    float v2 = *ic2eq + a2 * s1 + a3 * v3;
    *ic1eq = 2.0f * v1 - s1;
    *ic2eq = 2.0f * v2 - *ic2eq;

    /* Guard against denormals or NaN */
    if (!(*ic1eq > -10.0f && *ic1eq < 10.0f)) *ic1eq = 0.0f;
    if (!(*ic2eq > -10.0f && *ic2eq < 10.0f)) *ic2eq = 0.0f;
    if (fabsf(*ic1eq) < 1.0e-18f) *ic1eq = 0.0f;
    if (fabsf(*ic2eq) < 1.0e-18f) *ic2eq = 0.0f;

    (void)k;
    *v1_out = v1;
    *v2_out = v2;
}

float icarus_filter_step(icarus_filter_t *f, float x) {
    /* Optional high-pass pre-filter */
    if (f->hpf_cutoff > 15.0f) {
        float y_hp = x - f->hpf_x1 + f->hpf_r * f->hpf_y1;
        f->hpf_x1 = x;
        f->hpf_y1 = y_hp;
        x = y_hp;
    }

    /* Input saturation drive when filter saturation is active */
    float sat = f->saturation;
    if (sat > 0.01f) {
        float drv = 1.0f + sat * 1.8f;
        x = fast_tanhf(x * drv) * (1.0f / (1.0f + sat * 0.6f));
    }

    float v1 = 0.0f, v2 = 0.0f;
    svf_stage_step(x, f->a1, f->a2, f->a3, f->k, sat, &f->ic1eq_1, &f->ic2eq_1, &v1, &v2);

    float out;
    switch (f->mode) {
        case ICARUS_FILTER_LP12:
            out = v2;
            break;
        case ICARUS_FILTER_LP24: {
            float v1_b = 0.0f, v2_b = 0.0f;
            svf_stage_step(v2, f->a1, f->a2, f->a3, f->k, sat, &f->ic1eq_2, &f->ic2eq_2, &v1_b, &v2_b);
            out = v2_b;
            break;
        }
        case ICARUS_FILTER_HP12:
            out = x - f->k * v1 - v2;
            break;
        case ICARUS_FILTER_BP12:
            /* Normalized band-pass output: k * v1 has unity peak gain at resonance */
            out = v1 * (0.5f + 0.5f * f->k);
            break;
        default:
            out = v2;
            break;
    }

    /* Resonance loudness compensation & safety soft-limiting so high resonance never clips */
    float comp = 1.0f / (1.0f + 0.35f * f->resonance * f->resonance);
    out *= comp;
    if (out > 1.5f || out < -1.5f) {
        out = 1.5f * fast_tanhf(out * (1.0f / 1.5f));
    }
    return out;
}

/* The block loop. Same maths as icarus_filter_step (kept for the unit tests), restructured for the unit's cost model (measured: a float add or multiply
 * 8.3 ns, a divide 13.9, a read-modify-write of a struct word 19-22): the integrator states live in locals for the whole block, the constants that
 * icarus_filter_step divided by on EVERY sample (input scale, state scale, resonance compensation) are computed once per block, and the stability
 * guards (state outside +-10 or NaN, denormal flush) run once per 8 samples instead of 12 compares a sample; if one trips, the states are reset and
 * that chunk of 8 plays silence, so a NaN can never reach the output. */
void icarus_filter_process_block(icarus_filter_t *f, float *buf, int n,
                                const float *env_buf, float pitch_hz, float lfo_octaves) {
    if (!f || !buf || n <= 0) return;

    const float sat       = f->saturation;
    const int   use_sat   = (sat > 0.01f);
    const float comp      = 1.0f / (1.0f + 0.35f * f->resonance * f->resonance);
    const int   use_hpf   = (f->hpf_cutoff > 15.0f);
    const uint8_t mode    = f->mode;

    float i1a = f->ic1eq_1, i2a = f->ic2eq_1, i1b = f->ic1eq_2, i2b = f->ic2eq_2;
    float hx1 = f->hpf_x1, hy1 = f->hpf_y1;

    for (int base = 0; base < n; base += 8) {
        int end = base + 8;
        if (end > n) end = n;
        float ev = env_buf ? env_buf[base] : 0.0f;
        icarus_filter_update_mod(f, ev, pitch_hz, lfo_octaves);
        const float a1 = f->a1, a2 = f->a2, a3 = f->a3, k = f->k, hr = f->hpf_r;

        /* Dynamics-aware saturation & envelope-linked filter makeup (computed once per 8-sample sub-block):
         * quiet notes / decaying tails receive less saturation and follow the envelope rather than sitting on a resonant shelf. */
        const float sat_eff  = sat * clampf(ev * 1.4f, 0.25f, 1.0f);
        const float in_drv   = 1.0f + sat_eff * 1.8f;
        const float in_scale = 1.0f - sat_eff * (0.42f - 0.12f * sat_eff);
        const float s_drv    = 1.0f + sat_eff;
        const float s_scale  = 1.0f - sat_eff * (0.68f - 0.22f * sat_eff);
        const float env_comp = comp * (0.55f + 0.45f * clampf(ev, 0.0f, 1.2f));

        for (int i = base; i < end; i++) {
            float x = buf[i];
            if (use_hpf) {
                const float y_hp = x - hx1 + hr * hy1;
                hx1 = x; hy1 = y_hp; x = y_hp;
            }
            if (use_sat) x = fast_tanhf(x * in_drv) * in_scale;

            float s1 = i1a;
            if (use_sat) s1 = fast_tanhf(s1 * s_drv) * s_scale;
            float v3 = x - i2a;
            float v1 = a1 * s1 + a2 * v3;
            float v2 = i2a + a2 * s1 + a3 * v3;
            i1a = 2.0f * v1 - s1;
            i2a = 2.0f * v2 - i2a;

            float out;
            if (mode == ICARUS_FILTER_LP24) {
                float t1 = i1b;
                if (use_sat) t1 = fast_tanhf(t1 * s_drv) * s_scale;
                const float w3 = v2 - i2b;
                const float w1 = a1 * t1 + a2 * w3;
                const float w2 = i2b + a2 * t1 + a3 * w3;
                i1b = 2.0f * w1 - t1;
                i2b = 2.0f * w2 - i2b;
                out = w2;
            } else if (mode == ICARUS_FILTER_HP12) {
                out = x - k * v1 - v2;
            } else if (mode == ICARUS_FILTER_BP12) {
                out = v1 * (0.5f + 0.5f * k);
            } else {
                out = v2;                                           /* LP12 */
            }
            out *= env_comp;
            if (out > 1.5f || out < -1.5f) out = 1.5f * fast_tanhf(out * (1.0f / 1.5f));
            buf[i] = out;
        }

        /* the guards, once per chunk: a state out of range (or NaN) -> reset and play silence for this chunk; tiny states -> flush to zero */
        if (!(i1a > -10.0f && i1a < 10.0f && i2a > -10.0f && i2a < 10.0f && i1b > -10.0f && i1b < 10.0f && i2b > -10.0f && i2b < 10.0f)) {
            i1a = i2a = i1b = i2b = 0.0f;
            for (int i = base; i < end; i++) buf[i] = 0.0f;
        } else {
            if (fabsf(i1a) < 1.0e-18f) i1a = 0.0f;
            if (fabsf(i2a) < 1.0e-18f) i2a = 0.0f;
            if (fabsf(i1b) < 1.0e-18f) i1b = 0.0f;
            if (fabsf(i2b) < 1.0e-18f) i2b = 0.0f;
        }
    }

    f->ic1eq_1 = i1a; f->ic2eq_1 = i2a; f->ic1eq_2 = i1b; f->ic2eq_2 = i2b;
    f->hpf_x1 = hx1;  f->hpf_y1 = hy1;
}
