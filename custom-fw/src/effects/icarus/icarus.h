/* custom-fw/src/effects/icarus/icarus.h — Icarus Flagship Guitar Synthesis System */
#ifndef ICARUS_H
#define ICARUS_H

#include <stdint.h>
#include "pa_fx.h"
#include "icarus_env.h"
#include "icarus_filter.h"
#include "icarus_analog.h"
#include "icarus_osc.h"
#include "icarus_pitch.h"
#include "icarus_lfo.h"
#include "icarus_mix.h"

#define ICARUS_FACTORY_PRESET_COUNT 12u

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
    float    env_att_blk, env_rel_blk;   /* the envelope's attack / release coefficients for one whole block (1 - (1 - k)^64) */
    float    att_blk, rel_blk;           /* the gain smoother's, likewise */
    uint32_t hold_samples;
    uint32_t hold_cnt;
    float    env;
    float    gain;
    uint8_t  open;
} icarus_gate_t;

/* Complete serializable preset definition (Section 11 & Section 13) */
typedef struct {
    const char *name;
    uint8_t     mode;           /* icarus_op_mode_t: ANALOG / OSCILLATOR / HYBRID / GUITAR */

    /* Engine A: Analog Processing */
    uint8_t     dist_mode;      /* icarus_dist_mode_t */
    float       analog_drive;   /* 0.0 .. 1.0 */
    float       input_gain;     /* 0.5 .. 3.0 */
    uint8_t     pre_emphasis;   /* 0 / 1 */

    /* Engine B: Dual Oscillator + Sub */
    uint8_t     wave_a;         /* icarus_waveform_t */
    uint8_t     wave_b;         /* icarus_waveform_t */
    int8_t      pitch_a;        /* -24 .. +24 semitones */
    int8_t      pitch_b;        /* -24 .. +24 semitones */
    int8_t      fine_tune;      /* -50 .. +50 cents */
    float       level_a;        /* 0.0 .. 1.0 */
    float       level_b;        /* 0.0 .. 1.0 */
    float       detune_cents;   /* 0.0 .. 50.0 cents */
    float       pulse_width;    /* 0.05 .. 0.95 */
    uint8_t     sub_mode;       /* icarus_sub_mode_t: Off / -1 oct / -2 oct */
    float       sub_level;      /* 0.0 .. 1.0 */
    float       osc_mix;        /* 0.0 .. 1.0 */

    /* Resonant Filter (shared/linked across Analog & Synth paths) */
    uint8_t     filter_mode;    /* icarus_filter_mode_t: LP12 / LP24 / HP12 / BP12 */
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
    uint8_t     lfo_wave;       /* icarus_lfo_wave_t */
    uint8_t     lfo_dest;       /* icarus_lfo_dest_t */

    /* Mixer & Stage Integration */
    float       guitar_level;   /* 0.0 .. 1.0 */
    float       analog_level;   /* 0.0 .. 1.0 */
    float       synth_level;    /* 0.0 .. 1.0 */
    float       master_level;   /* 0.0 .. 1.0 */
    float       stereo_width;   /* 0.0 .. 1.0 */
    uint8_t     nam_enable;     /* 1 = route through NAM amplifier, 0 = bypass NAM */
    uint8_t     cab_enable;     /* 1 = route through IR cabinet, 0 = bypass IR */
    uint8_t     gate_enable;    /* 1 = enable synth noise gate */

    /* Touch and analog character (0 = the default for the mode, set by icarus_load_preset) */
    float       expression;     /* 0.0 .. 1.0: how much the voice follows the string. 0 = the ADSR alone (the note holds while the string dies), 1 = the voice fades and swells with the guitar */
    float       drift_cents;    /* 0.0 .. 10.0 cents: slow random analog instability of the two oscillators (common + differential) */
} icarus_preset_t;

/* Complete runtime DSP state (~3.2 KiB, allocated dynamically from .pool via pa_alloc) */
typedef struct {
    icarus_preset_t       params;
    uint8_t              preset_index;   /* 0 .. 11 */
    uint8_t              enabled;

    icarus_env_follower_t env_follower;
    icarus_env_t          adsr;
    icarus_analog_t       analog;
    icarus_osc_engine_t   osc_eng;
    icarus_filter_t       synth_filter;
    icarus_pitch_t        pitch;
    icarus_glide_t        glide;
    icarus_vibrato_t      vibrato;
    icarus_lfo_t          lfo;
    icarus_mix_t          mixer;
    icarus_gate_t         in_gate;
    icarus_gate_t         out_gate;

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
    float                drift_val, drift_tgt, drift_dif, drift_dif_tgt;   /* oscillator drift random walks, cents */
    uint32_t             drift_rng;
    float                expr_ref;            /* Input level (fast follower) at the last note trigger: the string's decay relative to it drives the voice's expression */
    uint16_t             ui_ttl;              /* Blocks the display telemetry (meters, scope, live cutoff / cents) stays on after the Icarus screen last asked for it
                                               * (icarus_ui_handle_inputs refreshes it every tick). 0 = nobody is looking: the audio side skips all of it. */
} icarus_state_t;

/* Lifecycle & Pool Allocation */
icarus_state_t *icarus_create(void);
void           icarus_destroy(icarus_state_t *s);
void           icarus_init(icarus_state_t *s);
void           icarus_reset(icarus_state_t *s);

/* Preset & Parameter Control */
const icarus_preset_t *icarus_get_factory_preset(uint8_t index);
void                  icarus_load_preset(icarus_state_t *s, uint8_t index);
void                  icarus_apply_params(icarus_state_t *s);
void                  icarus_set_mode(icarus_state_t *s, uint8_t mode);

/* Real-Time Audio Processing (Float & Q21 Fixed-Point Entry Points) */
void icarus_process_block_f32(icarus_state_t *s, const float *in_mono,
                             float *out_L, float *out_R, int n);
void icarus_process_block_q21(icarus_state_t *s, const int32_t *in_mono_q21,
                             int32_t *out_L_q21, int32_t *out_R_q21, int n);
void icarus_post_output_gate_q21(icarus_state_t *s, int32_t *buf_L_q21,
                                int32_t *buf_R_q21, int n);

/* Background / Low-Priority Pitch Analysis Slice */
void icarus_poll_background(icarus_state_t *s);

/* Global active instance accessor for UI / Audio Core integration */
icarus_state_t *icarus_get_active(void);
void           icarus_set_active_enable(int enable);
int            icarus_is_active(void);
void           icarus_enter_synth_mode(pa_fx_chain_t *chain);
void           icarus_exit_synth_mode(pa_fx_chain_t *chain);
void           icarus_sync_chain_routing(icarus_state_t *s, pa_fx_chain_t *chain);


/* Backward compatibility aliases */
typedef icarus_state_t alpha_state_t;
typedef icarus_preset_t alpha_preset_t;
typedef icarus_pitch_t alpha_pitch_t;
typedef icarus_osc_t alpha_osc_t;
typedef icarus_filter_t alpha_filter_t;
typedef icarus_env_t alpha_env_t;
typedef icarus_lfo_t alpha_lfo_t;
typedef icarus_analog_t alpha_analog_t;
typedef icarus_mix_t alpha_mix_t;
#define alpha_create icarus_create
#define alpha_destroy icarus_destroy
#define alpha_reset icarus_reset
#define alpha_init icarus_init
#define alpha_process_block_f32 icarus_process_block_f32
#define alpha_process_block_q21 icarus_process_block_q21
#define alpha_post_output_gate_q21 icarus_post_output_gate_q21
#define alpha_poll_background icarus_poll_background
#define alpha_get_active icarus_get_active
#define alpha_is_active icarus_is_active
#define alpha_load_preset icarus_load_preset
#define alpha_apply_params icarus_apply_params
#define alpha_set_mode icarus_set_mode
#define alpha_sync_chain_routing icarus_sync_chain_routing
#define alpha_get_factory_preset icarus_get_factory_preset
#define ALPHA_FACTORY_PRESET_COUNT ICARUS_FACTORY_PRESET_COUNT
#define ALPHA_MODE_ANALOG ICARUS_MODE_ANALOG
#define ALPHA_MODE_OSCILLATOR ICARUS_MODE_OSCILLATOR
#define ALPHA_MODE_HYBRID ICARUS_MODE_HYBRID
#define ALPHA_MODE_GUITAR ICARUS_MODE_GUITAR
#define ALPHA_ENV_IDLE ICARUS_ENV_IDLE

#endif /* ICARUS_H */
