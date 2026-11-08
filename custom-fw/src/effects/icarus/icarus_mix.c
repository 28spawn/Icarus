/* custom-fw/src/effects/icarus/icarus_mix.c — Constant-Power 3-Way Mixer & Headroom Scaling */
#include "icarus_mix.h"
#include <math.h>
#include "icarus_fastmath.h"

/* Nominal -6.02 dB (0.50f) headroom target before the NAM amplifier stage */
#define ICARUS_HEADROOM_SCALE 0.50f

static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static inline float fast_tanhf(float x) {
    if (x > 3.0f)  return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static void compute_target_gains(const icarus_mix_t *m, float *t_gtr, float *t_ana, float *t_syn) {
    float g = m->guitar_level;
    float a = m->analog_level;
    float s = m->synth_level;

    switch (m->mode) {
        case ICARUS_MODE_ANALOG:
            /* Polyphonic GR-inspired guitar processing + optional direct blend */
            *t_gtr = g * 0.65f;
            *t_ana = a;
            *t_syn = 0.0f;
            break;
        case ICARUS_MODE_OSCILLATOR:
            /* Pitch-tracked synthesizer + optional direct blend */
            *t_gtr = g * 0.50f;
            *t_ana = 0.0f;
            *t_syn = s;
            break;
        case ICARUS_MODE_HYBRID:
            /* Flagship: Analog processing combined with oscillator synthesis + direct guitar */
            *t_gtr = g;
            *t_ana = a;
            *t_syn = s;
            break;
        case ICARUS_MODE_GUITAR:
        default:
            /* Processed original guitar with optional subtle synth/analog coloration */
            *t_gtr = (g < 0.2f) ? 0.8f : g;
            *t_ana = a * 0.40f;
            *t_syn = s * 0.35f;
            break;
    }

    /* Constant-power normalization when multiple engines are active simultaneously,
     * ensuring mode switching never produces unexpected gain jumps. */
    float pwr = (*t_gtr) * (*t_gtr) + (*t_ana) * (*t_ana) + (*t_syn) * (*t_syn);
    if (pwr > 1.0f) {
        float norm = 1.0f / sqrtf(pwr);
        *t_gtr *= norm;
        *t_ana *= norm;
        *t_syn *= norm;
    }
}

void icarus_mix_init(icarus_mix_t *m) {
    if (!m) return;
    m->guitar_level = 0.25f;
    m->analog_level = 0.75f;
    m->synth_level  = 0.85f;
    m->master_level = 0.85f;
    m->pan          = 0.0f;
    m->stereo_width = 0.30f;
    m->mode         = ICARUS_MODE_HYBRID;
    icarus_mix_reset(m);
}

void icarus_mix_reset(icarus_mix_t *m) {
    if (!m) return;
    float tg, ta, ts;
    compute_target_gains(m, &tg, &ta, &ts);
    m->cur_g_gtr = tg;
    m->cur_g_ana = ta;
    m->cur_g_syn = ts;
    m->cur_g_mst = m->master_level * (2.0f * ICARUS_HEADROOM_SCALE);
    m->ap_z1     = 0.0f;
    m->ap_z2     = 0.0f;
}

void icarus_mix_set_params(icarus_mix_t *m, uint8_t mode,
                          float guitar_01, float analog_01, float synth_01,
                          float master_01, float pan_m1_p1, float width_01) {
    if (!m) return;
    m->mode         = (mode < ICARUS_MODE_COUNT) ? mode : ICARUS_MODE_HYBRID;
    m->guitar_level = clampf(guitar_01, 0.0f, 1.0f);
    m->analog_level = clampf(analog_01, 0.0f, 1.0f);
    m->synth_level  = clampf(synth_01, 0.0f, 1.0f);
    m->master_level = clampf(master_01, 0.0f, 1.0f);
    m->pan          = clampf(pan_m1_p1, -1.0f, 1.0f);
    m->stereo_width = clampf(width_01, 0.0f, 1.0f);
}

void icarus_mix_process_block(icarus_mix_t *m,
                             const float *direct_in,
                             const float *analog_in,
                             const float *synth_in,
                             float *out_L, float *out_R, int n) {
    if (!m || !out_L || n <= 0) return;

    float tg, ta, ts;
    compute_target_gains(m, &tg, &ta, &ts);
    /* Master level 0..100% where 85% = -6 dB headroom (0.5x full scale), 100% = -4.4 dB (0.60f) */
    float tm = m->master_level * (1.20f * ICARUS_HEADROOM_SCALE);

    float inv_n = 1.0f / (float)n;
    float dg = (tg - m->cur_g_gtr) * inv_n;
    float da = (ta - m->cur_g_ana) * inv_n;
    float ds = (ts - m->cur_g_syn) * inv_n;
    float dm = (tm - m->cur_g_mst) * inv_n;

    /* Constant-power pan law */
    float pan_norm = 0.5f * (m->pan + 1.0f); /* 0..1 */
    float pan_l = sqrtf(1.0f - pan_norm) * 1.41421356f;
    float pan_r = sqrtf(pan_norm) * 1.41421356f;
    if (pan_l > 1.2f) pan_l = 1.2f;
    if (pan_r > 1.2f) pan_r = 1.2f;

    const float width = m->stereo_width;
    const int   stereo = (out_R && out_R != out_L && width > 0.01f);
    const float side_k = 0.35f * width;

    /* the ramped gains and the all-pass state in locals for the block (struct read-modify-writes cost ~20 ns each on the unit) */
    float g_gtr = m->cur_g_gtr, g_ana = m->cur_g_ana, g_syn = m->cur_g_syn, g_mst = m->cur_g_mst;
    float z1 = m->ap_z1, z2 = m->ap_z2;

    for (int i = 0; i < n; i++) {
        g_gtr += dg;
        g_ana += da;
        g_syn += ds;
        g_mst += dm;

        const float d_in = direct_in ? direct_in[i] : 0.0f;
        const float a_in = analog_in ? analog_in[i] : 0.0f;
        const float s_in = synth_in  ? synth_in[i]  : 0.0f;

        float sum = d_in * g_gtr + a_in * g_ana + s_in * g_syn;
        sum *= g_mst;

        /* Soft-knee headroom protection so summing never hard-clips */
        if (sum > 0.70f) {
            sum = 0.70f + 0.25f * fast_tanhf((sum - 0.70f) * 4.0f);
        } else if (sum < -0.70f) {
            sum = -0.70f + 0.25f * fast_tanhf((sum + 0.70f) * 4.0f);
        }

        if (stereo) {
            /* Subtle phase-coherent stereo side difference between Analog & Synth engines */
            const float diff = (s_in * g_syn - a_in * g_ana) * g_mst;
            const float ap1 = 0.65f * (diff - z1) + z2;
            z2 = diff;
            z1 = ap1;
            const float side = ap1 * side_k;
            out_L[i] = (sum + side) * pan_l;
            out_R[i] = (sum - side) * pan_r;
        } else {
            out_L[i] = sum * pan_l;
            if (out_R && out_R != out_L) {
                out_R[i] = sum * pan_r;
            }
        }
    }
    m->cur_g_gtr = g_gtr; m->cur_g_ana = g_ana; m->cur_g_syn = g_syn; m->cur_g_mst = g_mst;
    m->ap_z1 = z1; m->ap_z2 = z2;
}
