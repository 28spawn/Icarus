/* custom-fw/src/effects/icarus/icarus_pitch.h — Icarus guitar pitch tracker (v2: onset-anchored YIN, confidence-gated notes)
 *
 * Who owns what (no field is written by both sides, so nothing needs a lock):
 *   audio interrupt (icarus_pitch_feed_block): the filters, the ring, the level followers, the note state machine and EVERY public output (frequency, valid, onset ...). It also hands the main loop
 *                    a search job (det_state 0 -> 1, window copied into det_win) and takes the answer back (res_seq).
 *   main loop (icarus_pitch_poll_background): the search itself (det_dn, det_tau, det_run ...), the result (res_freq, res_conf, then res_seq++, then det_state = 0).
 * While det_state == 1 the audio path does not touch det_win or the job fields; while it is 0 the main loop does not touch them. */
#ifndef ICARUS_PITCH_H
#define ICARUS_PITCH_H

#include <stdint.h>

#define ICARUS_PITCH_DEC       2u
#define ICARUS_PITCH_RING_LEN  1024u
#define ICARUS_PITCH_WIN_MAX   256u   /* the longest comparison window (11.6 ms @ 22,050 Hz); a young note is searched with a shorter one */
#define ICARUS_PITCH_WIN_MIN   64u    /* the shortest (2.9 ms): enough for a high note a few periods after the pick */
#define ICARUS_PITCH_TAU_MIN   14u    /* 22050 / 14 = 1575 Hz */
#define ICARUS_PITCH_TAU_MAX   480u   /* 22050 / 480 = 45.9 Hz: down to Drop F# (46.2 Hz) and Drop G (49.0 Hz) */   /* 22050 / 420 = 52.5 Hz: low A (55 Hz) with the margin its edge-of-range guard needs; a drop-tuned 7-string B is 61.7 Hz */
#define ICARUS_PITCH_NEED      (ICARUS_PITCH_WIN_MAX + ICARUS_PITCH_TAU_MAX)
#define ICARUS_PITCH_SKIP      48u    /* decimated samples (2.2 ms) skipped after a pick before the analysis window may start: the transient is not periodic */
#define ICARUS_PITCH_UPD       40u    /* decimated samples between search jobs (1.8 ms) while a note is being acquired or tracked */
#define ICARUS_PITCH_BG_LAGS   160u   /* lags one background poll evaluates (main loop, not the audio interrupt) */
#define ICARUS_PITCH_BG_STALE  64u    /* audio blocks without a background poll before the audio path does a small share of the search itself */

typedef struct {
    /* Public tracking outputs, written by the audio path only */
    float   frequency;          /* Tracked fundamental in Hz (default 110 Hz) */
    float   raw_frequency;      /* Latest unsmoothed estimate in Hz */
    float   confidence;         /* 0.0 .. 1.0 periodicity of the latest accepted estimate */
    float   note;               /* Continuous MIDI note number (69.0 = A4 = 440 Hz) */
    float   stability;          /* 0.0 .. 1.0 how steady the pitch has been */
    uint8_t midi_note;          /* Quantized MIDI note with 62-cent hysteresis */
    uint8_t valid;              /* 1 = a note is sounding AND its pitch is known; 0 = nothing (or not yet known) */
    uint8_t onset;              /* 1-block pulse: a new note (its pitch just became known) or a legato change of pitch */
    uint8_t release;            /* 1-block pulse: the note ended */
    uint8_t legato;             /* 1 = this onset was a change of pitch on a ringing string (hammer-on / pull-off / bend step), 0 = a picked note */

    /* Tracker configuration */
    float   sensitivity;        /* 0.0 .. 1.0 note gate sensitivity (default 0.50) */
    float   smoothing;          /* 0.0 .. 1.0 base tracking smoothing (default 0.35) */

    /* Conditioning & ring (audio path) */
    float   lp_z1, lp_z2;       /* 2-pole 1.6 kHz low-pass: the fundamental over the upper harmonics */
    float   hp_x1, hp_y1;       /* 38 Hz high-pass */
    float   dec_acc;
    uint8_t dec_cnt;
    uint16_t ring_w;
    int16_t ring[ICARUS_PITCH_RING_LEN];
    uint32_t ring_n;            /* absolute count of decimated samples written (never wraps in practice: 54 hours) */

    /* Level followers & the note state machine (audio path) */
    float    env_fast, env_slow;
    float    env_floor;         /* the noise floor: hum, hiss, a pickup in a noisy room. Notes must stand well above it */
    float    env_peak;          /* the peak of the current note's envelope, slowly decaying: a note is over when the string is ~29 dB below it */
    uint8_t  state;             /* 0 idle, 1 acquiring (a pick was seen, the pitch is not known yet), 2 locked (valid) */
    uint8_t  was_pick;          /* the pending onset is a new pick (a re-pick of a ringing string) or a plain start */
    uint16_t quiet_blocks;
    uint8_t  job_n;             /* alternates the full and the short job while locked */
    uint16_t fail_run;          /* consecutive searches without a confident result while locked */
    uint32_t onset_n;           /* ring_n at the pick */
    uint32_t last_job_n;        /* ring_n when the last job was handed over */
    uint8_t  res_seen;          /* the result sequence the audio path has consumed */
    uint8_t  cand_n;            /* consecutive results agreeing on a new pitch */
    float    cand_f;
    float    hist_f;            /* last accepted estimate, for continuity */
    float    prev_valid_freq;
    uint8_t  was_valid;
    float    pv_f[12];          /* the last results that came out of several strings (a poll's pick is noisy: the root is the pitch most of them agree on) */
    uint8_t  pv_n, pv_i;
    uint16_t bg_age;            /* audio blocks since the last background poll (saturating) */

    /* The search job (hand-over: det_state) */
    volatile uint8_t det_state; /* 0 = idle (audio path may hand a job over), 1 = job pending (main loop owns det_win and the fields below) */
    uint16_t det_w;             /* comparison window of this job (decimated samples) */
    uint16_t det_t;             /* largest lag of this job */
    uint16_t det_tau;           /* next lag to evaluate */
    uint8_t  det_light;         /* 1 while the AUDIO path runs a share of the search (no poller): the spectrum checks are skipped, they cost too much for the interrupt */
    uint8_t  det_found;
    uint16_t det_cand;
    float    det_run;
    float    det_best_v;
    uint16_t det_best_tau;
    float    det_hf_ratio;
    float    det_dn[ICARUS_PITCH_TAU_MAX + 2u];
    int16_t  det_win[ICARUS_PITCH_NEED];

    /* The answer (main loop writes, audio path reads) */
    volatile uint8_t res_seq;
    float    res_freq;          /* 0 = no confident period in the window */
    float    res_conf;
    volatile uint8_t res_poly;  /* 1 = the result came from the several-strings path (no clean period): it is a vote, not a measurement */
} icarus_pitch_t;

void icarus_pitch_init(icarus_pitch_t *p);
void icarus_pitch_reset(icarus_pitch_t *p);
void icarus_pitch_set_params(icarus_pitch_t *p, float sensitivity_01, float smoothing_01);

/* Audio path: filters the block, follows the levels, runs the note state machine, hands search jobs over and takes the answers. */
void icarus_pitch_feed_block(icarus_pitch_t *p, const float *in, int n);

/* Main loop: evaluates up to ICARUS_PITCH_BG_LAGS lags of the pending job (and marks the background as alive). Call it as often as the loop turns: a job is a few hundred lags. */
void icarus_pitch_poll_background(icarus_pitch_t *p);

#endif /* ICARUS_PITCH_H */
