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
#define ALPHA_PITCH_UPD       128u  /* decimated samples between detections (5.8 ms): ~170 pitch updates/s is plenty for a guitar */
#define ALPHA_PITCH_BG_LAGS   96u   /* lags one background poll evaluates (main loop, not the audio interrupt) */
#define ALPHA_PITCH_BG_STALE  64u   /* audio blocks without a background poll before the audio path does the work itself */

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
    uint8_t  det_found;         /* 1 = a lag below the YIN threshold was found; now walking down to its minimum */
    uint16_t det_cand;          /* that candidate lag */
    uint16_t det_best_tau;      /* deepest dip so far (fallback when nothing crosses the threshold) */
    uint16_t bg_age;            /* audio blocks since the last background poll (saturating) */
    float    det_run;           /* running sum of the difference function (the CMND denominator) */
    float    det_best_v;
    float    det_hf_ratio;      /* High-frequency vs fundamental energy ratio for palm-mute rejection */
    float    det_rms;           /* Window RMS level */
    float    det_dn[ALPHA_PITCH_TAU_MAX + 2u];   /* cumulative-mean-normalised difference, built lag by lag */
    int16_t  det_win[ALPHA_PITCH_NEED];          /* snapshot of the window, scaled to |x| <= 1023: the ring keeps moving while the lags run */

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

/* Fast audio-path sample feeder: filters input, updates onset/release envelope, pushes decimated samples into the ring and,
 * once per detection, snapshots the window. The lag search itself runs in alpha_pitch_poll_background() (main loop); the audio
 * path only does it when no poller has run for ALPHA_PITCH_BG_STALE blocks (host tests, a stalled main loop). */
void alpha_pitch_feed_block(alpha_pitch_t *p, const float *in, int n);

/* Main-loop runner: evaluates up to ALPHA_PITCH_BG_LAGS lags of the pending detection and marks the background as alive. */
void alpha_pitch_poll_background(alpha_pitch_t *p);

#endif /* ALPHA_PITCH_H */
