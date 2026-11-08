/* custom-fw/src/effects/icarus/icarus_lfo.h — Glide, Independent Vibrato, Modulation LFO & Expression */
#ifndef ICARUS_LFO_H
#define ICARUS_LFO_H

#include <stdint.h>

typedef enum {
    ICARUS_GLIDE_LEGATO = 0,     /* Default: portamento only on legato connected notes */
    ICARUS_GLIDE_ALWAYS = 1      /* Portamento on every note transition */
} icarus_glide_mode_t;

typedef enum {
    ICARUS_GLIDE_CURVE_EXP = 0,  /* Natural RC exponential pitch glide */
    ICARUS_GLIDE_CURVE_LIN = 1   /* Constant-semitones-per-second linear glide */
} icarus_glide_curve_t;

typedef enum {
    ICARUS_GLIDE_DIR_BOTH = 0,
    ICARUS_GLIDE_DIR_UP   = 1,
    ICARUS_GLIDE_DIR_DOWN = 2
} icarus_glide_dir_t;

typedef struct {
    float               time_ms;        /* 0 .. 2000 ms */
    icarus_glide_mode_t  mode;
    icarus_glide_curve_t curve;
    icarus_glide_dir_t   direction;
    float               sensitivity;    /* 0.0 .. 1.0 */
    float               current_hz;     /* Current glided pitch in Hz */
    float               current_oct;    /* log2(current_hz) */
    uint8_t             initialized;
    float               vel_scale;      /* glide time multiplier from the pick strength (0 = not set = 1): a harder pick glides a little faster */
} icarus_glide_t;

typedef enum {
    ICARUS_VIB_SINE = 0,
    ICARUS_VIB_TRI  = 1
} icarus_vib_wave_t;

typedef struct {
    float            rate_hz;        /* 0.1 .. 12.0 Hz */
    float            depth_cents;    /* 0.0 .. 100.0 cents */
    float            delay_ms;       /* 0.0 .. 2000.0 ms after note onset */
    float            fade_ms;        /* 0.0 .. 2000.0 ms fade-in time */
    icarus_vib_wave_t waveform;       /* Sine / Triangle */
    float            expr_sens;      /* 0.0 .. 1.0 picking/envelope expression sensitivity */

    float            phase;          /* 0.0 .. 1.0 */
    float            note_age_ms;    /* Elapsed ms since note onset */
    float            fade_env;       /* 0.0 .. 1.0 fade-in envelope */
    float            current_cents;  /* Current vibrato offset in cents */
    float            pitch_ratio;    /* 2^(current_cents / 1200) */
} icarus_vibrato_t;

typedef enum {
    ICARUS_LFO_SINE   = 0,
    ICARUS_LFO_TRI    = 1,
    ICARUS_LFO_SQUARE = 2,
    ICARUS_LFO_SAW    = 3,
    ICARUS_LFO_SH     = 4   /* Random stepped Sample & Hold */
} icarus_lfo_wave_t;

typedef enum {
    ICARUS_LFO_DEST_FILTER_CUTOFF = 0,
    ICARUS_LFO_DEST_OSC_PITCH     = 1,
    ICARUS_LFO_DEST_PULSE_WIDTH   = 2,
    ICARUS_LFO_DEST_OSC_AMP       = 3,
    ICARUS_LFO_DEST_STEREO_PAN    = 4
} icarus_lfo_dest_t;

typedef struct {
    float            rate_hz;        /* 0.05 .. 20.0 Hz */
    float            depth_01;       /* 0.0 .. 1.0 modulation depth */
    icarus_lfo_wave_t waveform;
    icarus_lfo_dest_t dest;

    float            phase;          /* 0.0 .. 1.0 */
    float            sh_val;         /* Current S&H value (-1 .. +1) */
    float            sh_smooth;      /* Lightly slewed S&H to prevent zipper clicks */
    uint32_t         rng_state;
    float            out_bipolar;    /* -1.0 .. +1.0 */
    float            out_unipolar;   /*  0.0 .. +1.0 */
} icarus_lfo_t;

/* Expression modulation outputs computed once per audio block */
typedef struct {
    float glided_freq_hz;       /* Pitch after glide + vibrato + LFO pitch mod */
    float filter_cutoff_oct;    /* Additional filter cutoff offset in octaves */
    float pulse_width_mod;      /* Pulse width modulation offset (-0.40 .. +0.40) */
    float osc_amp_scale;        /* Oscillator amplitude scale (0.0 .. 1.5) */
    float distortion_boost;     /* Analog drive boost from picking strength */
    float synth_blend_offset;   /* Dynamic synth/guitar blend shift */
    float stereo_pan_offset;    /* Stereo pan modulation (-0.5 .. +0.5) */
} icarus_mod_frame_t;

void icarus_glide_init(icarus_glide_t *g);
void icarus_glide_set_params(icarus_glide_t *g, float time_ms, icarus_glide_mode_t mode,
                            icarus_glide_curve_t curve, icarus_glide_dir_t dir, float sens_01);
float icarus_glide_step_block(icarus_glide_t *g, float target_hz, int note_onset, int is_legato, int block_samples);

void icarus_vibrato_init(icarus_vibrato_t *v);
void icarus_vibrato_set_params(icarus_vibrato_t *v, float rate_hz, float depth_cents,
                              float delay_ms, float fade_ms, icarus_vib_wave_t wave, float expr_sens_01);
void icarus_vibrato_trigger(icarus_vibrato_t *v);
float icarus_vibrato_step_block(icarus_vibrato_t *v, float input_env_01, int block_samples);

void icarus_lfo_init(icarus_lfo_t *l);
void icarus_lfo_set_params(icarus_lfo_t *l, float rate_hz, float depth_01,
                          icarus_lfo_wave_t wave, icarus_lfo_dest_t dest);
void icarus_lfo_step_block(icarus_lfo_t *l, int block_samples);

void icarus_compute_mod_frame(icarus_mod_frame_t *out,
                             icarus_glide_t *glide,
                             icarus_vibrato_t *vib,
                             icarus_lfo_t *lfo,
                             float tracked_freq_hz,
                             int note_onset,
                             int is_legato,
                             float input_env_01,
                             float pick_velocity_01,
                             int block_samples);

#endif /* ICARUS_LFO_H */
