/* custom-fw/src/effects/project_alpha/alpha_analog.h — Project Alpha Engine A: Polyphonic Analog Processing */
#ifndef ALPHA_ANALOG_H
#define ALPHA_ANALOG_H

#include <stdint.h>
#include "alpha_env.h"
#include "alpha_filter.h"

typedef enum {
    ALPHA_DIST_SOFT_CLIP = 0, /* Smooth analog-like saturation */
    ALPHA_DIST_HARD_CLIP = 1, /* Aggressive synth-like distortion */
    ALPHA_DIST_FOLDBACK  = 2, /* Harmonic-rich oscillator-like texture */
    ALPHA_DIST_HEXA      = 3, /* Dense, compressed, GR-inspired distortion */
    ALPHA_DIST_FUZZ      = 4, /* Vintage guitar synth character */
    ALPHA_DIST_MODE_COUNT = 5
} alpha_dist_mode_t;

typedef struct {
    /* Input conditioning parameters */
    float input_gain;       /* Linear gain (0.25 .. 4.0, default 1.0) */
    uint8_t pre_emphasis;   /* 1 = enable mild high-mid emphasis before distortion */
    uint8_t soft_clip_in;   /* 1 = enable input soft ceiling */

    /* Input conditioning state */
    float dc_x1, dc_y1;     /* Input 20 Hz DC blocker */
    uint8_t dc_primed;
    float out_dc_x1, out_dc_y1; /* Post-shaper DC blocker (removes asymmetric distortion DC) */
    float pre_emp_z1;       /* 1-pole pre-emphasis state */
    float de_emp_z1;        /* Post-distortion de-emphasis state */

    /* Distortion parameters */
    uint8_t dist_mode;      /* alpha_dist_mode_t */
    float drive;            /* 0.0 .. 1.0 */
    float cur_drive;        /* Smoothed drive */

    /* Hexa-inspired multi-band crossover state (split @ 320 Hz for chord clarity) */
    float hex_lp_z1;
    float hex_lp_z2;

    /* Dedicated Analog Engine Resonant Filter */
    alpha_filter_t filter;
} alpha_analog_t;

void alpha_analog_init(alpha_analog_t *a);
void alpha_analog_reset(alpha_analog_t *a);
void alpha_analog_set_conditioning(alpha_analog_t *a, float gain_linear, uint8_t pre_emphasis, uint8_t soft_clip);
void alpha_analog_set_distortion(alpha_analog_t *a, uint8_t mode, float drive_01);

/* Step 1: Condition raw guitar input (DC removal, gain, transient-preserving soft clip)
 * and fill env_out[0..n-1] with the envelope follower output. */
void alpha_input_condition_block(alpha_analog_t *a, alpha_env_follower_t *ef,
                                 const float *in, float *cond_out, float *env_out, int n);

/* Step 2: Run Engine A (Distortion + Resonant Filter + Envelope Modulation) on conditioned guitar. */
void alpha_analog_process_block(alpha_analog_t *a, const float *cond_in, const float *env_in,
                                float *analog_out, int n, float pitch_hz, float lfo_octaves);

#endif /* ALPHA_ANALOG_H */
