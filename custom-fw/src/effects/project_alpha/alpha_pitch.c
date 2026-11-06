/* custom-fw/src/effects/project_alpha/alpha_pitch.c — Hybrid Transient-Assisted YIN Guitar Pitch Tracker */
#include "alpha_pitch.h"
#include "pa_config.h"
#include <math.h>
#include <string.h>

#define ALPHA_FS     ((float)PA_SAMPLE_RATE)
#define ALPHA_DEC_FS (ALPHA_FS / (float)ALPHA_PITCH_DEC) /* 22,050 Hz */

/* Lags the audio path evaluates per block ONLY when no main-loop poller is running (host tests, a stalled main loop). */
#define ALPHA_PITCH_SLICE_LAGS 72u
#define ALPHA_YIN_THRESH       0.16f

static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static inline int32_t ring_at(const alpha_pitch_t *p, uint32_t idx) {
    return p->ring[idx & (ALPHA_PITCH_RING_LEN - 1u)];
}

static float median3(float a, float b, float c) {
    if (a > b) { float t = a; a = b; b = t; }
    if (b > c) { float t = b; b = c; c = t; }
    if (a > b) { float t = a; a = b; b = t; }
    return b;
}

void alpha_pitch_init(alpha_pitch_t *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->frequency       = 110.0f;
    p->raw_frequency   = 110.0f;
    p->prev_valid_freq = 110.0f;
    p->note            = 45.0f;
    p->midi_note       = 45u; /* A2 */
    p->sensitivity     = 0.50f;
    p->smoothing       = 0.35f;
    p->bg_age          = 0xFFFFu;   /* no poller seen yet: the audio path runs the lag search until one shows up */
}

void alpha_pitch_reset(alpha_pitch_t *p) {
    if (!p) return;
    float sens  = p->sensitivity;
    float smth  = p->smoothing;
    float lastf = p->frequency;
    memset(p, 0, sizeof(*p));
    p->frequency       = (lastf >= 40.0f && lastf <= 1500.0f) ? lastf : 110.0f;
    p->raw_frequency   = p->frequency;
    p->prev_valid_freq = p->frequency;
    p->note            = 69.0f + 12.0f * (logf(p->frequency / 440.0f) * 1.44269504f);
    p->midi_note       = (uint8_t)clampf(floorf(p->note + 0.5f), 24.0f, 108.0f);
    p->sensitivity     = sens;
    p->smoothing       = smth;
    p->bg_age          = 0xFFFFu;
}

void alpha_pitch_set_params(alpha_pitch_t *p, float sensitivity_01, float smoothing_01) {
    if (!p) return;
    p->sensitivity = clampf(sensitivity_01, 0.0f, 1.0f);
    p->smoothing   = clampf(smoothing_01, 0.0f, 1.0f);
}

static void pitch_det_start(alpha_pitch_t *p, float hf_ratio, float blk_rms) {
    p->det_w0       = p->ring_w;
    p->det_hf_ratio = hf_ratio;
    p->det_rms      = blk_rms;

    const uint32_t base = (uint32_t)p->det_w0 - ALPHA_PITCH_NEED;
    int32_t pk = 1;
    for (uint32_t i = 0; i < ALPHA_PITCH_NEED; i += 2u) {
        int32_t v = ring_at(p, base + i);
        if (v < 0) v = -v;
        if (v > pk) pk = v;
    }
    /* Scale the window so its peak sits around 512..1023 -> a 256-term sum of squared differences fits 32 bits.
     * Done ONCE here into a flat buffer, so the lag loops below are plain loads with no masking or shifting. */
    int rsh = 0;
    while ((pk >> rsh) > 1023 && rsh < 10) rsh++;
    int lsh = 0;
    while (rsh == 0 && (pk << (lsh + 1)) <= 1023 && lsh < 5) lsh++;
    for (uint32_t i = 0; i < ALPHA_PITCH_NEED; i++) {
        p->det_win[i] = (int16_t)((ring_at(p, base + i) << lsh) >> rsh);
    }

    p->det_tau    = 1u;
    p->det_found  = 0u;
    p->det_cand   = 0u;
    p->det_run    = 0.0f;
    p->det_best_v = 1.0e9f;
    p->det_best_tau = 0u;
    p->det_dn[0]  = 1.0f;
    p->det_state  = 1u;
}

static void pitch_commit_estimate(alpha_pitch_t *p, float f_est, float conf) {
    /* 1. Palm-muting / percussive noise guard:
     * If high-frequency transient/scrape energy dominates and periodicity is weak, do not retune */
    if (p->det_hf_ratio > 2.2f && conf < 0.78f) {
        conf *= 0.35f;
    }

    p->confidence = clampf(conf, 0.0f, 1.0f);

    if (f_est < 40.0f || f_est > 1500.0f || p->confidence < 0.45f) {
        /* Uncertain analysis: maintain last valid oscillator frequency */
        if (p->hold_uncertain < 250u) p->hold_uncertain++;
        p->stability *= 0.85f;
        return;
    }

    p->raw_frequency = f_est;

    /* 2. Octave-error & harmonic-jump rejection:
     * Check if the new estimate is near 2.0x (second harmonic) or 0.5x (sub-octave) of the current tracked frequency.
     * Require higher confidence and multiple consecutive frames before accepting an exact octave jump on a sustained note. */
    if (p->was_valid && p->prev_valid_freq > 40.0f && !p->onset) {
        float ratio = f_est / p->prev_valid_freq;
        int is_oct_up   = (ratio > 1.92f && ratio < 2.08f);
        int is_oct_down = (ratio > 0.48f && ratio < 0.52f);
        int is_fifth_h3 = (ratio > 2.90f && ratio < 3.10f); /* 3rd harmonic (octave + fifth) */

        if (is_fifth_h3 && p->confidence < 0.90f) {
            /* Reject 3rd harmonic overtone slip during decay */
            return;
        }
        if ((is_oct_up || is_oct_down) && p->confidence < 0.88f) {
            p->octave_jump_cnt++;
            if (p->octave_jump_cnt < 3u) {
                /* Hold previous valid pitch until octave change is confirmed across 3 windows */
                return;
            }
        } else {
            p->octave_jump_cnt = 0u;
        }
    } else {
        p->octave_jump_cnt = 0u;
    }

    /* 3. Update 3-point median filter to strip single-window glitches */
    p->hist[p->hist_i] = f_est;
    p->hist_i = (uint8_t)((p->hist_i + 1u) % 3u);
    if (p->hist_n < 3u) p->hist_n++;

    float f_med = f_est;
    if (p->hist_n == 3u) {
        f_med = median3(p->hist[0], p->hist[1], p->hist[2]);
    }

    /* 4. Confidence-dependent smoothing that preserves vibrato and sustained bends:
     * - Large interval step (> 1.2 semitones, i.e. > 7.2% frequency change) with high confidence:
     *   snap immediately to new note so legato/picked notes have zero sluggishness (glide engine handles portamento).
     * - Small/medium pitch movement (< 1.2 semitones, i.e. vibrato and string bends):
     *   track continuously with confidence-weighted responsiveness. */
    float rel_diff = fabsf(f_med - p->frequency) / p->frequency;
    if (!p->was_valid || p->hold_uncertain > 12u || (rel_diff > 0.072f && p->confidence > 0.68f)) {
        /* Genuine note transition or initial acquisition */
        if (p->was_valid && rel_diff > 0.072f) {
            p->onset  = 1u;
            p->legato = 1u;
        }
        p->frequency = f_med;
        /* Seed median history with the new note so old note doesn't pull it back */
        p->hist[0] = p->hist[1] = p->hist[2] = f_med;
    } else {
        /* Continuous tracking for bends & vibrato:
         * High confidence -> fast tracking (alpha ~ 0.55..0.85); lower confidence -> heavier smoothing */
        float conf_w = (p->confidence - 0.40f) * (1.0f / 0.60f);
        conf_w = clampf(conf_w, 0.10f, 1.0f);
        float alpha = conf_w * (1.0f - 0.65f * p->smoothing);
        alpha = clampf(alpha, 0.12f, 0.90f);
        p->frequency += alpha * (f_med - p->frequency);
    }

    p->prev_valid_freq = p->frequency;
    p->hold_uncertain  = 0u;
    p->valid           = 1u;
    p->was_valid       = 1u;
    p->stability       = clampf(1.0f - rel_diff * 8.0f, 0.0f, 1.0f) * p->confidence;

    /* 5. Compute continuous MIDI note and hysteresis-quantized MIDI note (62 cents threshold) */
    float semis = 12.0f * (logf(p->frequency * (1.0f / 440.0f)) * 1.44269504f);
    p->note = 69.0f + semis;
    int cand_midi = (int)floorf(p->note + 0.5f);
    cand_midi = (cand_midi < 24) ? 24 : ((cand_midi > 108) ? 108 : cand_midi);
    if (cand_midi != (int)p->midi_note) {
        float cents_from_cur = (p->note - (float)p->midi_note) * 100.0f;
        if (cents_from_cur > 62.0f || cents_from_cur < -62.0f) {
            p->midi_note = (uint8_t)cand_midi;
        }
    }
}

/* pick: the lag whose dip crossed the YIN threshold and was walked down to its minimum, or 0 when nothing did. */
static void pitch_det_finish(alpha_pitch_t *p, uint32_t pick) {
    const float *dn = p->det_dn;

    if (pick == 0u) {
        if (p->det_best_v < 0.42f) {
            pick = p->det_best_tau;
        } else {
            pitch_commit_estimate(p, 0.0f, 0.0f);
            return;
        }
    }

    float dip_val = dn[pick];
    float conf    = clampf(1.0f - dip_val * 1.4f, 0.0f, 1.0f);

    if (pick <= ALPHA_PITCH_TAU_MIN || pick >= ALPHA_PITCH_TAU_MAX) {
        pitch_commit_estimate(p, 0.0f, 0.0f);
        return;
    }

    /* Parabolic interpolation on normalized difference dn[pick-1..pick+1] for sub-sample accuracy */
    float y0  = dn[pick - 1u];
    float y1  = dn[pick];
    float y2  = dn[pick + 1u];
    float den = y0 - 2.0f * y1 + y2;
    float tau_f = (float)pick;
    if (fabsf(den) > 1.0e-7f) {
        float off = 0.5f * (y0 - y2) / den;
        tau_f += clampf(off, -0.5f, 0.5f);
    }

    float f_hz = ALPHA_DEC_FS / tau_f;
    pitch_commit_estimate(p, f_hz, conf);
}

/* Evaluate up to max_lags lags of the pending detection. The normalised difference is built as the lags come in, so the search stops
 * at the first dip below the YIN threshold (once it has been walked down to its minimum): a high note costs ~50 lags, not 500. */
static void pitch_det_slice(alpha_pitch_t *p, uint32_t max_lags) {
    if (p->det_state == 0u) return;

    const int16_t *w = p->det_win;
    float    *dn  = p->det_dn;
    float     run = p->det_run;
    uint32_t  tau = p->det_tau;

    for (uint32_t s = 0; s < max_lags && tau <= ALPHA_PITCH_TAU_MAX; s++, tau++) {
        /* Stride-2 window sum: 256 terms across the 512-sample decimated window (23.2 ms), unrolled by 4. */
        const int16_t *wt = w + tau;
        uint32_t acc = 0u;
        for (uint32_t j = 0; j < ALPHA_PITCH_WIN_LEN; j += 8u) {
            const int32_t d0 = (int32_t)w[j]      - (int32_t)wt[j];
            const int32_t d1 = (int32_t)w[j + 2u] - (int32_t)wt[j + 2u];
            const int32_t d2 = (int32_t)w[j + 4u] - (int32_t)wt[j + 4u];
            const int32_t d3 = (int32_t)w[j + 6u] - (int32_t)wt[j + 6u];
            acc += (uint32_t)(d0 * d0) + (uint32_t)(d1 * d1) + (uint32_t)(d2 * d2) + (uint32_t)(d3 * d3);
        }
        const float d = (float)acc;
        run += d;
        const float v = (run > 1.0e-9f) ? (d * (float)tau / run) : 1.0f;
        dn[tau] = v;

        if (tau >= ALPHA_PITCH_TAU_MIN && tau <= ALPHA_PITCH_TAU_MAX - 1u) {
            if (v < p->det_best_v) { p->det_best_v = v; p->det_best_tau = (uint16_t)tau; }
            if (!p->det_found) {
                if (v < ALPHA_YIN_THRESH) { p->det_found = 1u; p->det_cand = (uint16_t)tau; }
            } else if (v < dn[p->det_cand]) {
                p->det_cand = (uint16_t)tau;                                  /* still going down: the minimum is further on */
            } else {
                /* dn[cand + 1] (= v) is not lower than dn[cand]: cand is the minimum and the interpolation has its three points */
                p->det_state = 0u;
                p->det_run   = run;
                pitch_det_finish(p, p->det_cand);
                return;
            }
        } else if (p->det_found) {
            p->det_state = 0u;                                                 /* ran off the end of the search range while walking down */
            p->det_run   = run;
            pitch_det_finish(p, p->det_cand);
            return;
        }
    }
    p->det_run = run;
    p->det_tau = (uint16_t)tau;

    if (tau > ALPHA_PITCH_TAU_MAX) {
        p->det_state = 0u;
        pitch_det_finish(p, p->det_found ? p->det_cand : 0u);
    }
}

void alpha_pitch_feed_block(alpha_pitch_t *p, const float *in, int n) {
    if (!p || !in || n <= 0) return;

    p->onset   = 0u;
    p->release = 0u;
    if (p->bg_age < 65535u) p->bg_age++;

    /* Gate thresholds scaled by user sensitivity (0..1) */
    float sens_scale = 1.8f - 1.3f * p->sensitivity; /* 0.5 .. 1.8 */
    float open_thr   = 0.0045f * sens_scale;
    float close_thr  = open_thr * 0.45f;

    /* 1.6 kHz 2-pole low-pass filter (alpha = 0.20) to emphasize fundamental over upper harmonics */
    const float k_lp_a = 0.20f;
    /* 38 Hz 1-pole high-pass filter (R = 0.9946) */
    const float k_hp_r = 0.9946f;

    float sum_sq_lp = 0.0f;
    float sum_sq_hf = 0.0f;

    for (int i = 0; i < n; i++) {
        float x = in[i];
        /* High-pass */
        float y_hp = x - p->hp_x1 + k_hp_r * p->hp_y1;
        p->hp_x1 = x;
        p->hp_y1 = y_hp;

        /* 2-stage low-pass for fundamental extraction */
        p->lp_z1 += k_lp_a * (y_hp - p->lp_z1);
        p->lp_z2 += k_lp_a * (p->lp_z1 - p->lp_z2);
        float y_lp = p->lp_z2;
        float y_hf = y_hp - y_lp;

        sum_sq_lp += y_lp * y_lp;
        sum_sq_hf += y_hf * y_hf;

        /* Fast/slow envelope tracking for note onset & release */
        float a = fabsf(y_hp);
        p->env_fast += ((a > p->env_fast) ? 0.025f : 0.0020f) * (a - p->env_fast);
        p->env_slow += ((a > p->env_slow) ? 0.0015f : 0.0006f) * (a - p->env_slow);

        /* Decimate by 2 into 16-bit circular buffer */
        p->dec_acc += y_lp;
        if (++p->dec_cnt >= ALPHA_PITCH_DEC) {
            float avg = p->dec_acc * (1.0f / (float)ALPHA_PITCH_DEC);
            int32_t q15 = (int32_t)(avg * 32767.0f);
            if (q15 >  32767) q15 =  32767;
            if (q15 < -32768) q15 = -32768;
            p->ring[p->ring_w & (ALPHA_PITCH_RING_LEN - 1u)] = (int16_t)q15;
            p->ring_w++;
            p->samples_since_upd++;
            p->dec_acc = 0.0f;
            p->dec_cnt = 0u;
        }
    }

    float rms_lp   = sqrtf(sum_sq_lp / (float)n);
    float rms_hf   = sqrtf(sum_sq_hf / (float)n);
    float hf_ratio = rms_hf / (rms_lp + 1.0e-5f);

    /* Check note onset & release transitions */
    int sig_active = (p->env_fast >= (p->valid ? close_thr : open_thr));
    if (sig_active && hf_ratio < 2.8f) {
        p->quiet_blocks = 0u;
        if (!p->valid && p->env_fast > open_thr * 1.25f) {
            p->onset  = 1u;
            p->legato = 0u; /* Fresh note from silence -> no legato glide */
        } else if (p->valid && p->env_fast > p->env_slow * 1.65f + open_thr * 2.0f) {
            p->onset  = 1u;
            p->legato = 0u; /* Re-picked note */
            p->env_slow = p->env_fast;
        }

        /* Start a new YIN window every 64 decimated samples (128 input samples = 2.9 ms)
         * once the 1024-sample decimated ring has been primed. */
        if (p->det_state == 0u && p->samples_since_upd >= ALPHA_PITCH_UPD && p->ring_w >= ALPHA_PITCH_NEED) {
            p->samples_since_upd = 0u;
            pitch_det_start(p, hf_ratio, rms_lp);
        }
        if (p->det_state == 1u && p->bg_age > ALPHA_PITCH_BG_STALE) {
            pitch_det_slice(p, ALPHA_PITCH_SLICE_LAGS);                   /* no main-loop poller: do it here, as before */
        }
    } else {
        p->det_state = 0u;
        if (p->quiet_blocks < 65535u) p->quiet_blocks++;
        /* Release note after ~12 ms (8 blocks) below close threshold */
        if (p->valid && p->quiet_blocks >= 8u) {
            p->valid      = 0u;
            p->was_valid  = 0u;
            p->release    = 1u;
            p->confidence = 0.0f;
            p->stability  = 0.0f;
            p->hist_n     = 0u;
        }
    }
}

void alpha_pitch_poll_background(alpha_pitch_t *p) {
    if (!p) return;
    p->bg_age = 0u;                                   /* the main loop is alive: the audio path leaves the lag search to it */
    if (p->det_state == 0u) return;
    pitch_det_slice(p, ALPHA_PITCH_BG_LAGS);
}
