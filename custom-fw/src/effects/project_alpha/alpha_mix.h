/* custom-fw/src/effects/project_alpha/alpha_mix.h — Constant-Power 3-Way Mixer & Headroom Scaling */
#ifndef ALPHA_MIX_H
#define ALPHA_MIX_H

#include <stdint.h>

typedef enum {
    ALPHA_MODE_ANALOG     = 0, /* Polyphonic GR-inspired guitar processing */
    ALPHA_MODE_OSCILLATOR = 1, /* Pitch-tracked synthesizer */
    ALPHA_MODE_HYBRID     = 2, /* Flagship: Analog processing combined with oscillator synthesis */
    ALPHA_MODE_GUITAR     = 3, /* Processed original guitar with optional synth coloration */
    ALPHA_MODE_COUNT      = 4
} alpha_op_mode_t;

typedef struct {
    float guitar_level;     /* 0.0 .. 1.0 */
    float analog_level;     /* 0.0 .. 1.0 */
    float synth_level;      /* 0.0 .. 1.0 */
    float master_level;     /* 0.0 .. 1.0 */
    float pan;              /* -1.0 (Left) .. +1.0 (Right), 0.0 = Center */
    float stereo_width;     /* 0.0 .. 1.0 */
    uint8_t mode;           /* alpha_op_mode_t */

    /* Smoothed per-sample gains to prevent clicks during mode or knob changes */
    float cur_g_gtr;
    float cur_g_ana;
    float cur_g_syn;
    float cur_g_mst;

    /* Stereo Haas / allpass decorrelation state for optional stereo width */
    float ap_z1, ap_z2;
} alpha_mix_t;

void alpha_mix_init(alpha_mix_t *m);
void alpha_mix_reset(alpha_mix_t *m);
void alpha_mix_set_params(alpha_mix_t *m, uint8_t mode,
                          float guitar_01, float analog_01, float synth_01,
                          float master_01, float pan_m1_p1, float width_01);

/* Mix Direct Guitar, Analog Engine, and Oscillator Engine with constant-power weighting
 * and >= 6 dB internal headroom (0.5x nominal scale) before the NAM amplifier stage. */
void alpha_mix_process_block(alpha_mix_t *m,
                             const float *direct_in,
                             const float *analog_in,
                             const float *synth_in,
                             float *out_L, float *out_R, int n);

#endif /* ALPHA_MIX_H */
