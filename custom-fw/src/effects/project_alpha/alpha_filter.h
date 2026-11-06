/* custom-fw/src/effects/project_alpha/alpha_filter.h — Topology-Preserving Zero-Delay-Feedback Resonant Filter */
#ifndef ALPHA_FILTER_H
#define ALPHA_FILTER_H

#include <stdint.h>

typedef enum {
    ALPHA_FILTER_LP12 = 0, /* 2-pole (12 dB/oct) low-pass */
    ALPHA_FILTER_LP24 = 1, /* 4-pole (24 dB/oct) low-pass */
    ALPHA_FILTER_HP12 = 2, /* 2-pole (12 dB/oct) high-pass */
    ALPHA_FILTER_BP12 = 3, /* 2-pole (12 dB/oct) band-pass */
    ALPHA_FILTER_MODE_COUNT = 4
} alpha_filter_mode_t;

typedef struct {
    float cutoff;           /* Base cutoff in Hz (80 Hz .. 18000 Hz) */
    float resonance;        /* 0.0 .. 1.0 (0..100%) */
    float envelope_amount;  /* -1.0 .. +1.0 (-100%..+100%, up to +-4 octaves) */
    float tracking_amount;  /* 0.0 .. 1.0 (0..100% pitch tracking relative to A3 = 220 Hz) */
    float saturation;       /* 0.0 .. 1.0 (internal feedback/integrator drive) */
    float hpf_cutoff;       /* Optional pre-filter high-pass component (20 .. 400 Hz, 0 = off) */
    uint8_t mode;           /* alpha_filter_mode_t (LP12 / LP24 / HP12 / BP12) */

    /* Smoothed control state & TPT SVF state variables */
    float cur_cutoff_hz;
    float g;                /* tan(pi * fc / fs) */
    float k;                /* damping factor 2 - 1.92 * res */
    float a1, a2, a3;       /* Cytomic TPT SVF coefficients */

    /* Stage 1 integrators (12 dB/oct) */
    float ic1eq_1;
    float ic2eq_1;
    /* Stage 2 integrators (cascaded for 24 dB/oct LP24) */
    float ic1eq_2;
    float ic2eq_2;
    /* Optional 1-pole high-pass state */
    float hpf_x1;
    float hpf_y1;
    float hpf_r;
} alpha_filter_t;

void  alpha_filter_init(alpha_filter_t *f);
void  alpha_filter_reset(alpha_filter_t *f);
void  alpha_filter_set_params(alpha_filter_t *f, float cutoff_hz, float res_01,
                              float env_amt_m1_p1, float track_01, float sat_01, uint8_t mode);
void  alpha_filter_set_hpf(alpha_filter_t *f, float hpf_hz);

/* Update filter coefficients for the current sub-block given envelope (0..1), pitch (Hz), and LFO (-1..+1) */
void  alpha_filter_update_mod(alpha_filter_t *f, float env_val, float pitch_hz, float lfo_octaves);

/* Process a single sample (-1.0 .. +1.0 nominal) */
float alpha_filter_step(alpha_filter_t *f, float x);

/* Process a block of samples with control-rate coefficient interpolation every 8 samples */
void  alpha_filter_process_block(alpha_filter_t *f, float *buf, int n,
                                 const float *env_buf, float pitch_hz, float lfo_octaves);

#endif /* ALPHA_FILTER_H */
