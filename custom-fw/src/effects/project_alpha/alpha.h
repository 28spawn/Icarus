/* custom-fw/src/effects/project_alpha/alpha.h — Project Alpha Flagship Guitar Synthesis System */
#ifndef PROJECT_ALPHA_H
#define PROJECT_ALPHA_H

#include <stdint.h>
#include "pa_fx.h"
#include "alpha_env.h"
#include "alpha_filter.h"
#include "alpha_analog.h"
#include "alpha_osc.h"
#include "alpha_pitch.h"
#include "alpha_lfo.h"
#include "alpha_mix.h"

#define ALPHA_FACTORY_PRESET_COUNT 12u

/* Dedicated Synthesizer Noise Gate (Input / Output) with hysteresis (Section 7.3) */
typedef struct {
    float    threshold_db;  /* -85.0 .. -20.0 dB */
    float    attack_ms;     /* 0.5 .. 50.0 ms */
    float    hold_ms;       /* 5.0 .. 500.0 ms */
    float    release_ms;    /* 10.0 .. 1000.0 ms */
    float    hysteresis_db; /* 2.0 .. 12.0 dB */

    float    open_lin;
    float    close_lin;
    float    att_coeff;
    float    rel_coeff;
    uint32_t hold_samples;
    uint32_t hold_cnt;
    float    env;
    float    gain;
    uint8_t  open;
} alpha_gate_t;

/* Complete serializable preset definition (Section 11 & Section 13) */
typedef struct {
    const char *name;
    uint8_t     mode;           /* alpha_op_mode_t: ANALOG / OSCILLATOR / HYBRID / GUITAR */

    /* Engine A: Analog Processing */
    uint8_t     dist_mode;      /* alpha_dist_mode_t */
    float       analog_drive;   /* 0.0 .. 1.0 */
    float       input_gain;     /* 0.5 .. 3.0 */
    uint8_t     pre_emphasis;   /* 0 / 1 */

    /* Engine B: Dual Oscillator + Sub */
    uint8_t     wave_a;         /* alpha_waveform_t */
    uint8_t     wave_b;         /* alpha_waveform_t */
    int8_t      pitch_a;        /* -24 .. +24 semitones */
    int8_t      pitch_b;        /* -24 .. +24 semitones */
    int8_t      fine_tune;      /* -50 .. +50 cents */
    float       level_a;        /* 0.0 .. 1.0 */
    float       level_b;        /* 0.0 .. 1.0 */
    float       detune_cents;   /* 0.0 .. 50.0 cents */
    float       pulse_width;    /* 0.05 .. 0.95 */
    uint8_t     sub_mode;       /* alpha_sub_mode_t: Off / -1 oct / -2 oct */
    float       sub_level;      /* 0.0 .. 1.0 */
    float       osc_mix;        /* 0.0 .. 1.0 */

    /* Resonant Filter (shared/linked across Analog & Synth paths) */
    uint8_t     filter_mode;    /* alpha_filter_mode_t: LP12 / LP24 / HP12 / BP12 */
    float       cutoff_hz;      /* 80.0 .. 18000.0 Hz */
    float       resonance;      /* 0.0 .. 1.0 */
    float       env_amount;     /* -1.0 .. +1.0 */
    float       key_tracking;   /* 0.0 .. 1.0 */
    float       filter_sat;     /* 0.0 .. 1.0 */

    /* ADSR Filter & Amplitude Envelope */
    float       env_attack_ms;  /* 0 .. 2000 ms */
    float       env_decay_ms;   /* 0 .. 3000 ms */
    float       env_sustain;    /* 0.0 .. 1.0 */
    float       env_release_ms; /* 0 .. 5000 ms */

    /* Glide & Vibrato */
    float       glide_ms;       /* 0 .. 2000 ms */
    uint8_t     glide_legato;   /* 1 = legato-only, 0 = always-on */
    float       vib_rate_hz;    /* 0.1 .. 12.0 Hz */
    float       vib_depth_cents;/* 0.0 .. 100.0 cents */
    float       vib_delay_ms;   /* 0.0 .. 2000.0 ms */

    /* Modulation LFO */
    float       lfo_rate_hz;    /* 0.05 .. 20.0 Hz */
    float       lfo_depth;      /* 0.0 .. 1.0 */
    uint8_t     lfo_wave;       /* alpha_lfo_wave_t */
    uint8_t     lfo_dest;       /* alpha_lfo_dest_t */

    /* Mixer & Stage Integration */
    float       guitar_level;   /* 0.0 .. 1.0 */
    float       analog_level;   /* 0.0 .. 1.0 */
    float       synth_level;    /* 0.0 .. 1.0 */
    float       master_level;   /* 0.0 .. 1.0 */
    float       stereo_width;   /* 0.0 .. 1.0 */
    uint8_t     nam_enable;     /* 1 = route through NAM amplifier, 0 = bypass NAM */
    uint8_t     cab_enable;     /* 1 = route through IR cabinet, 0 = bypass IR */
    uint8_t     gate_enable;    /* 1 = enable synth noise gate */
} alpha_preset_t;

/* Complete runtime DSP state (~3.2 KiB, allocated dynamically from .pool via pa_alloc) */
typedef struct {
    alpha_preset_t       params;
    uint8_t              preset_index;   /* 0 .. 11 */
    uint8_t              enabled;

    alpha_env_follower_t env_follower;
    alpha_env_t          adsr;
    alpha_analog_t       analog;
    alpha_osc_engine_t   osc_eng;
    alpha_filter_t       synth_filter;
    alpha_pitch_t        pitch;
    alpha_glide_t        glide;
    alpha_vibrato_t      vibrato;
    alpha_lfo_t          lfo;
    alpha_mix_t          mixer;
    alpha_gate_t         in_gate;
    alpha_gate_t         out_gate;

    /* Smooth crossfade ramp for click-free preset/mode changes */
    float                xfade_gain;
    float                xfade_step;

    /* Real-time Visual UI & Wavelength Oscilloscope Telemetry (stored in .pool, 0 static .bss) */
    int8_t               scope_wave[64];      /* Decimated 4:1 triggered output wavelength trace (-31..+31 px) */
    int8_t               scope_in_wave[64];   /* Decimated 4:1 triggered input wavelength trace (-31..+31 px) */
    int8_t               scope_accum[64];     /* Multi-block decimated accumulation buffer */
    int8_t               scope_in_accum[64];  /* Multi-block decimated input accumulation buffer */
    float                scope_prev_s;        /* Previous sample for rising zero-crossing trigger */
    uint8_t              scope_wr_idx;        /* Write index (0..64) into decimated scope_accum */
    uint8_t              scope_decim_cnt;     /* 4:1 decimation counter (0..3) */
    uint8_t              scope_armed;         /* 1 = triggered and capturing 64 decimated samples */
    float                live_cutoff_hz;      /* Instantaneous modulated filter cutoff (80..18000 Hz) */
    float                live_env_level;      /* Combined envelope level (0.0 .. 1.0) */
    float                live_lfo_val;        /* Instantaneous LFO output (-1.0 .. +1.0) */
    float                live_pwm;            /* Instantaneous modulated pulse width (0.05 .. 0.95) */
    float                live_vib_semi;       /* Instantaneous vibrato offset in semitones */
    float                live_peak_gtr;       /* Smoothed Direct Guitar branch meter (0.0 .. 1.0) */
    float                live_peak_ana;       /* Smoothed Analog Synth branch meter (0.0 .. 1.0) */
    float                live_peak_osc;       /* Smoothed Oscillator Synth branch meter (0.0 .. 1.0) */
    float                live_peak_out;       /* Smoothed Master Stereo output meter (0.0 .. 1.0) */
    int8_t               live_pitch_cents;    /* Fine pitch deviation (-50 .. +50 cents) */
    uint8_t              scope_hold_blocks;   /* Persistence counter for triggered scope trace */
    uint16_t             ui_anim_tick;        /* Monotonic block counter for UI phase animation */
} alpha_state_t;

/* Lifecycle & Pool Allocation */
alpha_state_t *alpha_create(void);
void           alpha_destroy(alpha_state_t *s);
void           alpha_init(alpha_state_t *s);
void           alpha_reset(alpha_state_t *s);

/* Preset & Parameter Control */
const alpha_preset_t *alpha_get_factory_preset(uint8_t index);
void                  alpha_load_preset(alpha_state_t *s, uint8_t index);
void                  alpha_apply_params(alpha_state_t *s);
void                  alpha_set_mode(alpha_state_t *s, uint8_t mode);

/* Real-Time Audio Processing (Float & Q21 Fixed-Point Entry Points) */
void alpha_process_block_f32(alpha_state_t *s, const float *in_mono,
                             float *out_L, float *out_R, int n);
void alpha_process_block_q21(alpha_state_t *s, const int32_t *in_mono_q21,
                             int32_t *out_L_q21, int32_t *out_R_q21, int n);
void alpha_post_output_gate_q21(alpha_state_t *s, int32_t *buf_L_q21,
                                int32_t *buf_R_q21, int n);

/* Background / Low-Priority Pitch Analysis Slice */
void alpha_poll_background(alpha_state_t *s);

/* Global active instance accessor for UI / Audio Core integration */
alpha_state_t *alpha_get_active(void);
void           alpha_set_active_enable(int enable);
int            alpha_is_active(void);
void           alpha_enter_synth_mode(pa_fx_chain_t *chain);
void           alpha_exit_synth_mode(pa_fx_chain_t *chain);
void           alpha_sync_chain_routing(alpha_state_t *s, pa_fx_chain_t *chain);

#endif /* PROJECT_ALPHA_H */
