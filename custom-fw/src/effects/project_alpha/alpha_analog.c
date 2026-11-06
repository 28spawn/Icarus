/* custom-fw/src/effects/project_alpha/alpha_analog.c — Project Alpha Engine A: Polyphonic Analog Processing */
#include "alpha_analog.h"
#include "pa_config.h"
#include <math.h>

static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static inline float fast_tanhf(float x) {
    if (x > 3.0f)  return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Smooth wavefolder: sin-like triangle foldback without libm overhead */
static inline float foldback_shaper(float x) {
    /* Normalize into period 4 [-2, +2] */
    float u = (x + 1.0f) * 0.25f;
    u -= floorf(u);
    float tri = (u < 0.5f) ? (4.0f * u - 1.0f) : (3.0f - 4.0f * u);
    /* Parabolic shaping for rich odd/even synth harmonics */
    return tri * (1.5f - 0.5f * tri * tri);
}

void alpha_analog_init(alpha_analog_t *a) {
    if (!a) return;
    a->input_gain   = 1.0f;
    a->pre_emphasis = 1u;
    a->soft_clip_in = 1u;
    a->dist_mode    = ALPHA_DIST_HEXA;
    a->drive        = 0.55f;
    a->cur_drive    = 0.55f;
    alpha_filter_init(&a->filter);
    alpha_filter_set_params(&a->filter, 2200.0f, 0.55f, 0.60f, 0.40f, 0.30f, ALPHA_FILTER_LP24);
    alpha_analog_reset(a);
}

void alpha_analog_reset(alpha_analog_t *a) {
    if (!a) return;
    a->dc_x1      = 0.0f;
    a->dc_y1      = 0.0f;
    a->dc_primed  = 0u;
    a->out_dc_x1  = 0.0f;
    a->out_dc_y1  = 0.0f;
    a->pre_emp_z1 = 0.0f;
    a->de_emp_z1  = 0.0f;
    a->hex_lp_z1  = 0.0f;
    a->hex_lp_z2  = 0.0f;
    a->cur_drive  = a->drive;
    alpha_filter_reset(&a->filter);
}

void alpha_analog_set_conditioning(alpha_analog_t *a, float gain_linear, uint8_t pre_emphasis, uint8_t soft_clip) {
    if (!a) return;
    a->input_gain   = clampf(gain_linear, 0.1f, 8.0f);
    a->pre_emphasis = pre_emphasis ? 1u : 0u;
    a->soft_clip_in = soft_clip ? 1u : 0u;
}

void alpha_analog_set_distortion(alpha_analog_t *a, uint8_t mode, float drive_01) {
    if (!a) return;
    a->dist_mode = (mode < ALPHA_DIST_MODE_COUNT) ? mode : ALPHA_DIST_HEXA;
    a->drive     = clampf(drive_01, 0.0f, 1.0f);
}

void alpha_input_condition_block(alpha_analog_t *a, alpha_env_follower_t *ef,
                                 const float *in, float *cond_out, float *env_out, int n) {
    if (!a || !in || !cond_out || n <= 0) return;

    /* 22 Hz DC blocker: R = 1 - (2*pi*22 / 44100) = 0.9968, and zero initial step */
    const float k_dc_r = 0.9968f;
    const float gain   = a->input_gain;

    for (int i = 0; i < n; i++) {
        float x = in[i];
        if (!a->dc_primed) {
            a->dc_x1     = x;
            a->dc_y1     = 0.0f;
            a->dc_primed = 1u;
        }
        float y = x - a->dc_x1 + k_dc_r * a->dc_y1;
        a->dc_x1 = x;
        a->dc_y1 = y;

        y *= gain;

        /* Optional gentle soft ceiling above +-0.85 so picking transients are preserved without hard clipping */
        if (a->soft_clip_in) {
            if (y > 0.85f) {
                y = 0.85f + 0.15f * fast_tanhf((y - 0.85f) * (1.0f / 0.15f));
            } else if (y < -0.85f) {
                y = -0.85f + 0.15f * fast_tanhf((y + 0.85f) * (1.0f / 0.15f));
            }
        }

        cond_out[i] = y;
        if (ef && env_out) {
            env_out[i] = alpha_env_follower_step(ef, fabsf(y));
        }
    }
}

void alpha_analog_process_block(alpha_analog_t *a, const float *cond_in, const float *env_in,
                                float *analog_out, int n, float pitch_hz, float lfo_octaves) {
    if (!a || !cond_in || !analog_out || n <= 0) return;

    const float drv_step = (a->drive - a->cur_drive) / (float)n;
    const uint8_t mode   = a->dist_mode;

    /* 320 Hz crossover coefficient for Hexa-inspired dual-band saturation: 1 - exp(-2*pi*320/44100) = 0.0446 */
    const float k_hex_c  = 0.0446f;
    /* Pre-emphasis high-shelf boost above 700 Hz */
    const float k_pre_c  = 0.095f;

    for (int i = 0; i < n; i++) {
        a->cur_drive += drv_step;
        float d = clampf(a->cur_drive, 0.0f, 1.0f);
        float x = cond_in[i];
        float env = env_in ? env_in[i] : 0.1f;

        /* Optional pre-emphasis: tightens low strings before nonlinear saturation so chords stay articulate */
        if (a->pre_emphasis) {
            a->pre_emp_z1 += k_pre_c * (x - a->pre_emp_z1);
            float hi = x - a->pre_emp_z1;
            x = x + 0.85f * hi;
        }

        float y = x;
        switch (mode) {
            case ALPHA_DIST_SOFT_CLIP: {
                float g = 1.2f + d * 18.0f;
                y = fast_tanhf(x * g) * 0.75f;
                break;
            }
            case ALPHA_DIST_HARD_CLIP: {
                float g = 1.5f + d * 32.0f;
                float u = x * g;
                /* Band-guarded hard clipper with cubic knee */
                if (u > 1.0f)       u = 1.0f;
                else if (u < -1.0f) u = -1.0f;
                else                u = u * (1.5f - 0.5f * u * u);
                y = u * 0.65f;
                break;
            }
            case ALPHA_DIST_FOLDBACK: {
                /* Dynamic wavefolder: picking strength + drive control fold depth */
                float g = 1.0f + d * 6.5f;
                float folded = foldback_shaper(x * g);
                /* Blend clean/soft with folded waveform for musicality at low levels */
                float gate_blend = clampf(env * 8.0f, 0.15f, 1.0f);
                y = (folded * gate_blend + fast_tanhf(x * 2.5f) * (1.0f - gate_blend)) * 0.65f;
                break;
            }
            case ALPHA_DIST_HEXA: {
                /* Hexa-inspired mode: splits lows (<320 Hz) and highs (>320 Hz) into two independent
                 * envelope-normalized saturators so multi-string chords have GR-300-like density
                 * without intermodulation mud between bass and treble strings. */
                a->hex_lp_z1 += k_hex_c * (x - a->hex_lp_z1);
                a->hex_lp_z2 += k_hex_c * (a->hex_lp_z1 - a->hex_lp_z2);
                float lo = a->hex_lp_z2;
                float hi = x - lo;

                /* Envelope-assisted sustain compression + asymmetric GR wave-shaping */
                float comp_g = 1.0f / (0.08f + 0.92f * env);
                comp_g = clampf(comp_g, 0.8f, 6.0f);
                float g = (2.0f + d * 22.0f) * (1.0f + 0.35f * d * (comp_g - 1.0f));

                float y_lo = fast_tanhf((lo + 0.04f * env) * (g * 0.75f));
                float y_hi = fast_tanhf((hi - 0.03f * env) * (g * 1.15f));
                y = (0.55f * y_lo + 0.65f * y_hi) * 0.72f;
                break;
            }
            case ALPHA_DIST_FUZZ: {
                /* Vintage octave-edged transistor fuzz: asymmetric bias + full-wave harmonic edge */
                float g = 2.5f + d * 38.0f;
                float u = x * g;
                float sym  = fast_tanhf(u + 0.25f) - 0.2449f;
                float rect = (fabsf(fast_tanhf(u * 0.8f)) - 0.35f * env);
                y = (0.72f * sym + (0.28f * d) * rect) * 0.70f;
                break;
            }
            default:
                y = x;
                break;
        }

        /* Post-saturation de-emphasis */
        a->de_emp_z1 += 0.38f * (y - a->de_emp_z1);
        analog_out[i] = a->de_emp_z1;
    }

    /* Pass through the Analog Engine's variable resonant filter with input envelope modulation */
    alpha_filter_process_block(&a->filter, analog_out, n, env_in, pitch_hz, lfo_octaves);

    /* Final 30 Hz DC blocker (R = 0.9957) after the resonant filter so asymmetric
     * wave-shapers, intermodulation beats, and filter saturation never output DC offset */
    for (int i = 0; i < n; i++) {
        float u_dc = analog_out[i];
        float y_dc = u_dc - a->out_dc_x1 + 0.9957f * a->out_dc_y1;
        a->out_dc_x1 = u_dc;
        a->out_dc_y1 = y_dc;
        analog_out[i] = y_dc;
    }
}
