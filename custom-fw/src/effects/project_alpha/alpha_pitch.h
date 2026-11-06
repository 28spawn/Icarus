/* custom-fw/src/effects/project_alpha/alpha_pitch.h — Hybrid Transient-Assisted YIN Guitar Pitch Tracker */
#ifndef ALPHA_PITCH_H
#define ALPHA_PITCH_H

#include <stdint.h>

/* 2048 full-rate samples decimated 2:1 into a 1024-sample ring (@ 22,050 Hz)
 * plus a 64-sample raw ring for exact fractional lag interpolation at high frets. */
#define ALPHA_PITCH_DEC       2u
#define ALPHA_PITCH_RING_LEN  1024u
#define ALPHA_PITCH_WIN_LEN   512u
#define ALPHA_PITCH_TAU_MIN   14u   /* 22050 / 14 = 1575 Hz */
#define ALPHA_PITCH_TAU_MAX   512u  /* 22050 / 512 = 43.07 Hz */
#define ALPHA_PITCH_NEED      (ALPHA_PITCH_WIN_LEN + ALPHA_PITCH_TAU_MAX)

typedef struct {
    /* Public tracking outputs (Section 4.3 & 10.4) */
    float   frequency;          /* Smoothed valid fundamental frequency in Hz (default 110.0 Hz) */
    float   raw_frequency;      /* Latest unsmoothed YIN pitch estimate in Hz */
    float   confidence;         /* 0.0 .. 1.0 pitch periodicity confidence */
    float   note;               /* Continuous MIDI note number (69.0 = A4 = 440 Hz) */
    float   stability;          /* 0.0 .. 1.0 pitch stability metric */
    uint8_t midi_note;          /* Quantized MIDI note with hysteresis (36..96) */
    uint8_t valid;              /* 1 = tracking valid / note gated on, 0 = released/uncertain */
    uint8_t onset;              /* 1-block pulse when a new note onset or legato transition is detected */
    uint8_t release;            /* 1-block pulse when note release is detected */
    uint8_t legato;             /* 1 = current note transition was connected (legato), 0 = re-picked from silence */

    /* Tracker configuration */
    float   sensitivity;        /* 0.0 .. 1.0 note gate sensitivity (default 0.50) */
    float   smoothing;          /* 0.0 .. 1.0 base tracking smoothing (default 0.35) */

    /* Internal signal conditioning & ring buffer */
    float   lp_z1, lp_z2;       /* 2-pole 1.6 kHz anti-aliasing & harmonic-taming filter */
    float   hp_x1, hp_y1;       /* 38 Hz sub-rumble high-pass filter */
    float   dec_acc;
    uint8_t dec_cnt;
    uint16_t ring_w;
    uint16_t samples_since_upd;
    int16_t ring[ALPHA_PITCH_RING_LEN];

    /* Incremental / background YIN state machine */
    uint8_t  det_state;         /* 0 = idle, 1 = accumulating lags */
    uint16_t det_w0;            /* Ring write snapshot at start of window */
    uint16_t det_tau;           /* Next lag to evaluate */
    int8_t   det_rsh, det_lsh;  /* Window block-floating-point scaling */
    float    det_hf_ratio;      /* High-frequency vs fundamental energy ratio for palm-mute rejection */
    float    det_rms;           /* Window RMS level */
    uint32_t det_d[ALPHA_PITCH_TAU_MAX + 2u];

    /* History & octave/transient rejection state */
    float    hist[3];           /* 3-point median history */
    uint8_t  hist_n, hist_i;
    uint8_t  octave_jump_cnt;   /* Consecutive frames suggesting a 2x/0.5x octave jump */
    uint8_t  hold_uncertain;    /* Frames spent holding last valid pitch during transient/low confidence */
    uint16_t quiet_blocks;      /* Consecutive blocks below release threshold */
    float    env_fast;
    float    env_slow;
    float    prev_valid_freq;
    uint8_t  was_valid;
} alpha_pitch_t;

void alpha_pitch_init(alpha_pitch_t *p);
void alpha_pitch_reset(alpha_pitch_t *p);
void alpha_pitch_set_params(alpha_pitch_t *p, float sensitivity_01, float smoothing_01);

/* Fast audio-path sample feeder: filters input, updates onset/release envelope,
 * pushes decimated samples into circular buffer, and runs a small bounded lag slice. */
void alpha_pitch_feed_block(alpha_pitch_t *p, const float *in, int n);

/* Optional background / between-block runner to finish any pending lag slice */
void alpha_pitch_poll_background(alpha_pitch_t *p);

#endif /* ALPHA_PITCH_H */
