/* custom-fw/src/effects/project_alpha/alpha_lfo.h — Glide, Independent Vibrato, Modulation LFO & Expression */
#ifndef ALPHA_LFO_H
#define ALPHA_LFO_H

#include <stdint.h>

typedef enum {
    ALPHA_GLIDE_LEGATO = 0,     /* Default: portamento only on legato connected notes */
    ALPHA_GLIDE_ALWAYS = 1      /* Portamento on every note transition */
} alpha_glide_mode_t;

typedef enum {
    ALPHA_GLIDE_CURVE_EXP = 0,  /* Natural RC exponential pitch glide */
    ALPHA_GLIDE_CURVE_LIN = 1   /* Constant-semitones-per-second linear glide */
} alpha_glide_curve_t;

typedef enum {
    ALPHA_GLIDE_DIR_BOTH = 0,
    ALPHA_GLIDE_DIR_UP   = 1,
    ALPHA_GLIDE_DIR_DOWN = 2
} alpha_glide_dir_t;

typedef struct {
    float               time_ms;        /* 0 .. 2000 ms */
    alpha_glide_mode_t  mode;
    alpha_glide_curve_t curve;
    alpha_glide_dir_t   direction;
    float               sensitivity;    /* 0.0 .. 1.0 */
    float               current_hz;     /* Current glided pitch in Hz */
    float               current_oct;    /* log2(current_hz) */
    uint8_t             initialized;
} alpha_glide_t;

typedef enum {
    ALPHA_VIB_SINE = 0,
    ALPHA_VIB_TRI  = 1
} alpha_vib_wave_t;

typedef struct {
    float            rate_hz;        /* 0.1 .. 12.0 Hz */
    float            depth_cents;    /* 0.0 .. 100.0 cents */
    float            delay_ms;       /* 0.0 .. 2000.0 ms after note onset */
    float            fade_ms;        /* 0.0 .. 2000.0 ms fade-in time */
    alpha_vib_wave_t waveform;       /* Sine / Triangle */
    float            expr_sens;      /* 0.0 .. 1.0 picking/envelope expression sensitivity */

    float            phase;          /* 0.0 .. 1.0 */
    float            note_age_ms;    /* Elapsed ms since note onset */
    float            fade_env;       /* 0.0 .. 1.0 fade-in envelope */
    float            current_cents;  /* Current vibrato offset in cents */
    float            pitch_ratio;    /* 2^(current_cents / 1200) */
} alpha_vibrato_t;

typedef enum {
    ALPHA_LFO_SINE   = 0,
    ALPHA_LFO_TRI    = 1,
    ALPHA_LFO_SQUARE = 2,
    ALPHA_LFO_SAW    = 3,
    ALPHA_LFO_SH     = 4   /* Random stepped Sample & Hold */
} alpha_lfo_wave_t;

typedef enum {
    ALPHA_LFO_DEST_FILTER_CUTOFF = 0,
    ALPHA_LFO_DEST_OSC_PITCH     = 1,
    ALPHA_LFO_DEST_PULSE_WIDTH   = 2,
    ALPHA_LFO_DEST_OSC_AMP       = 3,
    ALPHA_LFO_DEST_STEREO_PAN    = 4
} alpha_lfo_dest_t;

typedef struct {
    float            rate_hz;        /* 0.05 .. 20.0 Hz */
    float            depth_01;       /* 0.0 .. 1.0 modulation depth */
    alpha_lfo_wave_t waveform;
    alpha_lfo_dest_t dest;

    float            phase;          /* 0.0 .. 1.0 */
    float            sh_val;         /* Current S&H value (-1 .. +1) */
    float            sh_smooth;      /* Lightly slewed S&H to prevent zipper clicks */
    uint32_t         rng_state;
    float            out_bipolar;    /* -1.0 .. +1.0 */
    float            out_unipolar;   /*  0.0 .. +1.0 */
} alpha_lfo_t;

/* Expression modulation outputs computed once per audio block */
typedef struct {
    float glided_freq_hz;       /* Pitch after glide + vibrato + LFO pitch mod */
    float filter_cutoff_oct;    /* Additional filter cutoff offset in octaves */
    float pulse_width_mod;      /* Pulse width modulation offset (-0.40 .. +0.40) */
    float osc_amp_scale;        /* Oscillator amplitude scale (0.0 .. 1.5) */
    float distortion_boost;     /* Analog drive boost from picking strength */
    float synth_blend_offset;   /* Dynamic synth/guitar blend shift */
    float stereo_pan_offset;    /* Stereo pan modulation (-0.5 .. +0.5) */
} alpha_mod_frame_t;

void alpha_glide_init(alpha_glide_t *g);
void alpha_glide_set_params(alpha_glide_t *g, float time_ms, alpha_glide_mode_t mode,
                            alpha_glide_curve_t curve, alpha_glide_dir_t dir, float sens_01);
float alpha_glide_step_block(alpha_glide_t *g, float target_hz, int note_onset, int is_legato, int block_samples);

void alpha_vibrato_init(alpha_vibrato_t *v);
void alpha_vibrato_set_params(alpha_vibrato_t *v, float rate_hz, float depth_cents,
                              float delay_ms, float fade_ms, alpha_vib_wave_t wave, float expr_sens_01);
void alpha_vibrato_trigger(alpha_vibrato_t *v);
float alpha_vibrato_step_block(alpha_vibrato_t *v, float input_env_01, int block_samples);

void alpha_lfo_init(alpha_lfo_t *l);
void alpha_lfo_set_params(alpha_lfo_t *l, float rate_hz, float depth_01,
                          alpha_lfo_wave_t wave, alpha_lfo_dest_t dest);
void alpha_lfo_step_block(alpha_lfo_t *l, int block_samples);

void alpha_compute_mod_frame(alpha_mod_frame_t *out,
                             alpha_glide_t *glide,
                             alpha_vibrato_t *vib,
                             alpha_lfo_t *lfo,
                             float tracked_freq_hz,
                             int note_onset,
                             int is_legato,
                             float input_env_01,
                             float pick_velocity_01,
                             int block_samples);

#endif /* ALPHA_LFO_H */
