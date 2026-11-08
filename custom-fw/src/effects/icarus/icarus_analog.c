/* custom-fw/src/effects/icarus/icarus_analog.c — Icarus Engine A: Polyphonic Analog Processing */
#include "icarus_analog.h"
#include "pa_config.h"
#include <math.h>
#include "icarus_fastmath.h"

static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static inline float fast_tanhf(float x) {
    if (x > 3.0f)  return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

void icarus_analog_init(icarus_analog_t *a) {
    if (!a) return;
    a->input_gain   = 1.0f;
    a->pre_emphasis = 1u;
    a->soft_clip_in = 1u;
    a->dist_mode    = ICARUS_DIST_HEXA;
    a->drive        = 0.55f;
    a->cur_drive    = 0.55f;
    icarus_filter_init(&a->filter);
    icarus_filter_set_params(&a->filter, 2200.0f, 0.55f, 0.60f, 0.40f, 0.30f, ICARUS_FILTER_LP24);
    icarus_analog_reset(a);
}

void icarus_analog_reset(icarus_analog_t *a) {
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
    icarus_filter_reset(&a->filter);
}

void icarus_analog_set_conditioning(icarus_analog_t *a, float gain_linear, uint8_t pre_emphasis, uint8_t soft_clip) {
    if (!a) return;
    a->input_gain   = clampf(gain_linear, 0.1f, 8.0f);
    a->pre_emphasis = pre_emphasis ? 1u : 0u;
    a->soft_clip_in = soft_clip ? 1u : 0u;
}

void icarus_analog_set_distortion(icarus_analog_t *a, uint8_t mode, float drive_01) {
    if (!a) return;
    a->dist_mode = (mode < ICARUS_DIST_MODE_COUNT) ? mode : ICARUS_DIST_HEXA;
    a->drive     = clampf(drive_01, 0.0f, 1.0f);
}

void icarus_input_condition_block(icarus_analog_t *a, icarus_env_follower_t *ef,
                                 const float *in, float *cond_out, float *env_out, int n) {
    if (!a || !in || !cond_out || n <= 0) return;

    /* 22 Hz DC blocker: R = 1 - (2*pi*22 / 44100) = 0.9968, and zero initial step */
    const float k_dc_r = 0.9968f;
    const float gain   = a->input_gain;
    const int   sclip  = a->soft_clip_in;
    float dc_x1 = a->dc_x1, dc_y1 = a->dc_y1;                    /* the DC blocker's state in locals: a struct word read-modify-write is ~20 ns on the unit */
    if (!a->dc_primed) {
        dc_x1 = in[0];
        dc_y1 = 0.0f;
        a->dc_primed = 1u;
    }

    for (int i = 0; i < n; i++) {
        const float x = in[i];
        float y = x - dc_x1 + k_dc_r * dc_y1;
        dc_x1 = x;
        dc_y1 = y;

        y *= gain;

        /* Optional gentle soft ceiling above +-0.85 so picking transients are preserved without hard clipping */
        if (sclip) {
            if (y > 0.85f) {
                y = 0.85f + 0.15f * fast_tanhf((y - 0.85f) * (1.0f / 0.15f));
            } else if (y < -0.85f) {
                y = -0.85f + 0.15f * fast_tanhf((y + 0.85f) * (1.0f / 0.15f));
            }
        }
        cond_out[i] = y;
    }
    a->dc_x1 = dc_x1;
    a->dc_y1 = dc_y1;
    if (ef && env_out) icarus_env_follower_block(ef, cond_out, env_out, n);
}

/* floor without the library call (foldback ran floorf once per sample): exact for |u| < 2^31 */
static inline float fast_floorf_a(float u) {
    const int i = (int)u;
    return (float)(((float)i > u) ? i - 1 : i);
}

/* The analog engine's block loop. The distortion mode is chosen ONCE per block (its own tight loop below), the integrator and drive states live
 * in locals (a struct word read-modify-write is ~20 ns on the unit, a float add 8), and the dearer per-sample divides run less often. */
void icarus_analog_process_block(icarus_analog_t *a, const float *cond_in, const float *env_in,
                                float *analog_out, int n, float pitch_hz, float lfo_octaves) {
    if (!a || !cond_in || !analog_out || n <= 0) return;

    const float drv_step = (a->drive - a->cur_drive) / (float)n;
    const uint8_t mode   = a->dist_mode;

    /* 320 Hz crossover coefficient for Hexa-inspired dual-band saturation: 1 - exp(-2*pi*320/44100) = 0.0446 */
    const float k_hex_c  = 0.0446f;
    /* Pre-emphasis high-shelf boost above 700 Hz */
    const float k_pre_c  = 0.095f;
    const int   pre_on   = a->pre_emphasis;

    float cd  = a->cur_drive;
    float pre = a->pre_emp_z1;
    float de  = a->de_emp_z1;
    float hz1 = a->hex_lp_z1, hz2 = a->hex_lp_z2;

/* the per-sample front end shared by every mode: drive ramp, envelope, optional pre-emphasis -> x, d, env */
#define ANALOG_FRONT()                                                         \
    cd += drv_step;                                                            \
    const float d = (cd < 0.0f) ? 0.0f : ((cd > 1.0f) ? 1.0f : cd);            \
    float x = cond_in[i];                                                      \
    const float env = env_in ? env_in[i] : 0.1f;                               \
    if (pre_on) {                                                              \
        pre += k_pre_c * (x - pre);                                            \
        x = x + 0.85f * (x - pre);                                             \
    }
#define ANALOG_BACK(yv) do { de += 0.38f * ((yv) - de); analog_out[i] = de; } while (0)

    switch (mode) {
        case ICARUS_DIST_SOFT_CLIP:
            for (int i = 0; i < n; i++) {
                ANALOG_FRONT();
                (void)env;
                const float g = 1.2f + d * 18.0f;
                ANALOG_BACK(fast_tanhf(x * g) * 0.75f);
            }
            break;
        case ICARUS_DIST_HARD_CLIP:
            for (int i = 0; i < n; i++) {
                ANALOG_FRONT();
                (void)env;
                const float g = 1.5f + d * 32.0f;
                float u = x * g;
                /* Band-guarded hard clipper with cubic knee */
                if (u > 1.0f)       u = 1.0f;
                else if (u < -1.0f) u = -1.0f;
                else                u = u * (1.5f - 0.5f * u * u);
                ANALOG_BACK(u * 0.65f);
            }
            break;
        case ICARUS_DIST_FOLDBACK:
            for (int i = 0; i < n; i++) {
                ANALOG_FRONT();
                /* Dynamic wavefolder: picking strength + drive control fold depth */
                const float g = 1.0f + d * 6.5f;
                float u = (x * g + 1.0f) * 0.25f;                    /* foldback_shaper, with the floor inlined */
                u -= fast_floorf_a(u);
                const float tri = (u < 0.5f) ? (4.0f * u - 1.0f) : (3.0f - 4.0f * u);
                const float folded = tri * (1.5f - 0.5f * tri * tri);
                /* Blend clean/soft with folded waveform for musicality at low levels */
                const float gb = clampf(env * 8.0f, 0.15f, 1.0f);
                ANALOG_BACK((folded * gb + fast_tanhf(x * 2.5f) * (1.0f - gb)) * 0.65f);
            }
            break;
        case ICARUS_DIST_FUZZ:
            for (int i = 0; i < n; i++) {
                ANALOG_FRONT();
                /* Vintage octave-edged transistor fuzz: asymmetric bias + full-wave harmonic edge,
                 * with bias scaled by envelope so idle silence never outputs a DC/rectified floor */
                const float g = 2.5f + d * 38.0f;
                const float u = x * g;
                const float bias_env = clampf(env * 4.0f, 0.0f, 1.0f);
                const float sym  = fast_tanhf(u + 0.25f * bias_env) - 0.2449f * bias_env;
                const float rect = (fabsf(fast_tanhf(u * 0.8f)) - 0.35f * env) * bias_env;
                ANALOG_BACK((0.72f * sym + (0.28f * d) * rect) * 0.70f);
            }
            break;
        case ICARUS_DIST_HEXA: {
            /* Hexa-inspired mode: splits lows (<320 Hz) and highs (>320 Hz) into two independent envelope-normalized saturators so multi-string
             * chords have GR-300-like density without intermodulation mud between bass and treble strings. The envelope-assisted sustain
             * compression (a divide) follows an envelope that moves slowly: it is evaluated once per 4 samples. */
            float comp_g = 1.0f;
            for (int i = 0; i < n; i++) {
                ANALOG_FRONT();
                hz1 += k_hex_c * (x - hz1);
                hz2 += k_hex_c * (hz1 - hz2);
                const float lo = hz2;
                const float hi = x - lo;
                if ((i & 3) == 0) {
                    comp_g = 1.0f / (0.30f + 0.70f * env);        /* gentle sustain assist that preserves soft-vs-hard picking contrast */
                    comp_g = clampf(comp_g, 0.9f, 2.2f);
                }
                const float g = (2.0f + d * 22.0f) * (1.0f + 0.30f * d * (comp_g - 1.0f));
                const float y_lo = fast_tanhf((lo + 0.04f * env) * (g * 0.75f));
                const float y_hi = fast_tanhf((hi - 0.03f * env) * (g * 1.15f));
                ANALOG_BACK((0.55f * y_lo + 0.65f * y_hi) * 0.72f);
            }
            break;
        }
        default:
            for (int i = 0; i < n; i++) {
                ANALOG_FRONT();
                (void)env; (void)d;
                ANALOG_BACK(x);
            }
            break;
    }
#undef ANALOG_FRONT
#undef ANALOG_BACK

    a->cur_drive = cd;
    /* no denormals left in the one-pole states when the input dies away (a denormal operation is dozens of cycles on some cores, and the state would stay there for seconds) */
    if (fabsf(pre) < 1.0e-20f) pre = 0.0f;
    if (fabsf(de)  < 1.0e-20f) de  = 0.0f;
    if (fabsf(hz1) < 1.0e-20f) hz1 = 0.0f;
    if (fabsf(hz2) < 1.0e-20f) hz2 = 0.0f;
    a->pre_emp_z1 = pre;
    a->de_emp_z1 = de;
    a->hex_lp_z1 = hz1; a->hex_lp_z2 = hz2;

    /* LEVEL FOLLOWS THE PLAYING. Every distortion mode above is a saturator (tanh / clip / fold with 12-40x of gain at full drive), so its output sits at nearly one level whatever goes in: a
     * soft note and a hard note came out equally loud (the flat Pure GR / GR Brass / Synth Bass on real captures). The note envelope (ADSR x velocity x the string's own decay, see alpha.c) now
     * scales the saturated signal, the same idea as the oscillator fix: x 1.7 so a full-velocity sustained note stays near its old level, blended by `follow` (the preset's expression). */
    if (a->follow > 0.01f && env_in) {
        const float fl = a->follow, keep = 1.0f - fl;
        for (int i = 0; i < n; i++) {
            float lv = env_in[i] * 1.7f;
            if (lv > 1.4f) lv = 1.4f;
            analog_out[i] *= keep + fl * lv;
        }
    }

    /* Pass through the Analog Engine's variable resonant filter with input envelope modulation */
    icarus_filter_process_block(&a->filter, analog_out, n, env_in, pitch_hz, lfo_octaves);

    /* Final 30 Hz DC blocker (R = 0.9957) after the resonant filter so asymmetric
     * wave-shapers, intermodulation beats, and filter saturation never output DC offset */
    float ox1 = a->out_dc_x1, oy1 = a->out_dc_y1;
    for (int i = 0; i < n; i++) {
        const float u_dc = analog_out[i];
        const float y_dc = u_dc - ox1 + 0.9957f * oy1;
        ox1 = u_dc;
        oy1 = y_dc;
        analog_out[i] = y_dc;
    }
    a->out_dc_x1 = ox1;
    a->out_dc_y1 = oy1;
}
