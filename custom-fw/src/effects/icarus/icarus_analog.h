/* custom-fw/src/effects/icarus/icarus_analog.h — Icarus Engine A: Polyphonic Analog Processing */
#ifndef ICARUS_ANALOG_H
#define ICARUS_ANALOG_H

#include <stdint.h>
#include "icarus_env.h"
#include "icarus_filter.h"

typedef enum {
    ICARUS_DIST_SOFT_CLIP = 0, /* Smooth analog-like saturation */
    ICARUS_DIST_HARD_CLIP = 1, /* Aggressive synth-like distortion */
    ICARUS_DIST_FOLDBACK  = 2, /* Harmonic-rich oscillator-like texture */
    ICARUS_DIST_HEXA      = 3, /* Dense, compressed, GR-inspired distortion */
    ICARUS_DIST_FUZZ      = 4, /* Vintage guitar synth character */
    ICARUS_DIST_MODE_COUNT = 5
} icarus_dist_mode_t;

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
    uint8_t dist_mode;      /* icarus_dist_mode_t */
    float drive;            /* 0.0 .. 1.0 */
    float cur_drive;        /* Smoothed drive */
    float follow;           /* 0..1: how much the engine's OUTPUT LEVEL follows the note envelope (the preset's expression). The saturators below hold the level nearly constant whatever is played. */

    /* Hexa-inspired multi-band crossover state (split @ 320 Hz for chord clarity) */
    float hex_lp_z1;
    float hex_lp_z2;

    /* Dedicated Analog Engine Resonant Filter */
    icarus_filter_t filter;
} icarus_analog_t;

void icarus_analog_init(icarus_analog_t *a);
void icarus_analog_reset(icarus_analog_t *a);
void icarus_analog_set_conditioning(icarus_analog_t *a, float gain_linear, uint8_t pre_emphasis, uint8_t soft_clip);
void icarus_analog_set_distortion(icarus_analog_t *a, uint8_t mode, float drive_01);

/* Step 1: Condition raw guitar input (DC removal, gain, transient-preserving soft clip)
 * and fill env_out[0..n-1] with the envelope follower output. */
void icarus_input_condition_block(icarus_analog_t *a, icarus_env_follower_t *ef,
                                 const float *in, float *cond_out, float *env_out, int n);

/* Step 2: Run Engine A (Distortion + Resonant Filter + Envelope Modulation) on conditioned guitar. */
void icarus_analog_process_block(icarus_analog_t *a, const float *cond_in, const float *env_in,
                                float *analog_out, int n, float pitch_hz, float lfo_octaves);

#endif /* ICARUS_ANALOG_H */
