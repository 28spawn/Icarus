/* custom-fw/src/effects/project_alpha/alpha_osc.h — Band-Limited Dual Oscillator + Sub Oscillator Engine */
#ifndef ALPHA_OSC_H
#define ALPHA_OSC_H

#include <stdint.h>

typedef enum {
    ALPHA_WAVE_SAW      = 0,
    ALPHA_WAVE_SQUARE   = 1,
    ALPHA_WAVE_TRIANGLE = 2,
    ALPHA_WAVE_COUNT    = 3
} alpha_waveform_t;

typedef enum {
    ALPHA_SUB_OFF  = 0,
    ALPHA_SUB_OCT1 = 1, /* -1 octave */
    ALPHA_SUB_OCT2 = 2, /* -2 octaves */
    ALPHA_SUB_COUNT = 3
} alpha_sub_mode_t;

typedef struct {
    float frequency;        /* Current instantaneous frequency in Hz */
    float phase;            /* Normalized phase accumulator [0.0, 1.0) */
    float phase_increment;  /* frequency / sample_rate */
    float level;            /* 0.0 .. 1.0 (0..100%) */
    float detune;           /* -50.0 .. +50.0 cents */
    float pulse_width;      /* 0.05 .. 0.95 (5..95%, default 0.50) */
    float phase_offset;     /* 0.0 .. 1.0 initial/relative phase offset */
    float tri_state;        /* Leaky integrator state for PolyBLEP triangle */
    int8_t pitch_semi;      /* -24 .. +24 semitones */
    int8_t fine_cents;      /* -50 .. +50 cents */
    uint8_t waveform;       /* alpha_waveform_t */
} alpha_osc_t;

typedef struct {
    alpha_osc_t osc_a;
    alpha_osc_t osc_b;
    float sub_phase;        /* Sub-oscillator phase accumulator [0.0, 1.0) */
    float sub_level;        /* 0.0 .. 1.0 */
    uint8_t sub_mode;       /* alpha_sub_mode_t (Off / -1 oct / -2 oct) */
    float osc_mix;          /* 0.0 (100% A) .. 1.0 (100% B), 0.5 = equal A+B */
    float cur_amp_gate;     /* Click-free activation/deactivation ramp [0.0, 1.0] */
    float dc_x1, dc_y1;     /* 15 Hz DC blocker for PWM asymmetry */
} alpha_osc_engine_t;

void  alpha_osc_init(alpha_osc_t *osc);
void  alpha_osc_reset(alpha_osc_t *osc);
void  alpha_osc_set_freq(alpha_osc_t *osc, float base_freq_hz, float extra_cents);
float alpha_osc_step(alpha_osc_t *osc, float pwm_mod);

void  alpha_osc_engine_init(alpha_osc_engine_t *eng);
void  alpha_osc_engine_reset(alpha_osc_engine_t *eng);
void  alpha_osc_engine_process_block(alpha_osc_engine_t *eng, float base_freq_hz,
                                     int active_gate, float pitch_mod_cents,
                                     float pwm_mod, float amp_mod,
                                     float *out_buf, int n);

#endif /* ALPHA_OSC_H */
