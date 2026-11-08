/* custom-fw/src/effects/icarus/icarus_osc.h — Band-Limited Dual Oscillator + Sub Oscillator Engine */
#ifndef ICARUS_OSC_H
#define ICARUS_OSC_H

#include <stdint.h>

typedef enum {
    ICARUS_WAVE_SAW      = 0,
    ICARUS_WAVE_SQUARE   = 1,
    ICARUS_WAVE_TRIANGLE = 2,
    ICARUS_WAVE_COUNT    = 3
} icarus_waveform_t;

typedef enum {
    ICARUS_SUB_OFF  = 0,
    ICARUS_SUB_OCT1 = 1, /* -1 octave */
    ICARUS_SUB_OCT2 = 2, /* -2 octaves */
    ICARUS_SUB_COUNT = 3
} icarus_sub_mode_t;

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
    uint8_t waveform;       /* icarus_waveform_t */
} icarus_osc_t;

typedef struct {
    icarus_osc_t osc_a;
    icarus_osc_t osc_b;
    float sub_phase;        /* Sub-oscillator phase accumulator [0.0, 1.0) */
    float sub_level;        /* 0.0 .. 1.0 */
    uint8_t sub_mode;       /* icarus_sub_mode_t (Off / -1 oct / -2 oct) */
    float osc_mix;          /* 0.0 (100% A) .. 1.0 (100% B), 0.5 = equal A+B */
    float cur_amp_gate;     /* Click-free activation/deactivation ramp [0.0, 1.0] */
    float dc_x1, dc_y1;     /* 15 Hz DC blocker for PWM asymmetry */
    float drift_diff_cents; /* analog drift between the two oscillators (set per block by icarus.c) */
} icarus_osc_engine_t;

void  icarus_osc_init(icarus_osc_t *osc);
void  icarus_osc_reset(icarus_osc_t *osc);
void  icarus_osc_set_freq(icarus_osc_t *osc, float base_freq_hz, float extra_cents);
float icarus_osc_step(icarus_osc_t *osc, float pwm_mod);

void  icarus_osc_engine_init(icarus_osc_engine_t *eng);
void  icarus_osc_engine_reset(icarus_osc_engine_t *eng);
void  icarus_osc_engine_process_block(icarus_osc_engine_t *eng, float base_freq_hz,
                                     int active_gate, float pitch_mod_cents,
                                     float pwm_mod, float amp_mod,
                                     float *out_buf, int n);

#endif /* ICARUS_OSC_H */
