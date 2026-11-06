/* custom-fw/src/effects/project_alpha/alpha_pitch.c — Hybrid Transient-Assisted YIN Guitar Pitch Tracker */
#include "alpha_pitch.h"
#include "pa_config.h"
#include <math.h>
#include <string.h>

#define ALPHA_FS     ((float)PA_SAMPLE_RATE)
#define ALPHA_DEC_FS (ALPHA_FS / (float)ALPHA_PITCH_DEC) /* 22,050 Hz */

/* Evaluate 64 lags per 64-sample audio block -> 500 lags finish in ~8 blocks (256 decimated samples = 512 raw samples),
 * or even faster if background poll runs between blocks. */
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
    /* Scale window so peak sits around 512..1023 -> 256-term sum of squared differences fits 32-bit uint32_t */
    int rsh = 0;
    while ((pk >> rsh) > 1023 && rsh < 10) rsh++;
    int lsh = 0;
    while (rsh == 0 && (pk << (lsh + 1)) <= 1023 && lsh < 5) lsh++;

    p->det_rsh   = (int8_t)rsh;
    p->det_lsh   = (int8_t)lsh;
    p->det_tau   = 1u;
    p->det_state = 1u;
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

static void pitch_det_finish(alpha_pitch_t *p) {
    float run = 0.0f;
    float best_v = 1.0e9f;
    uint32_t best_tau = 0u;
    uint32_t pick = 0u;
    float dn[ALPHA_PITCH_TAU_MAX + 2u];

    dn[0] = 1.0f;
    for (uint32_t tau = 1u; tau <= ALPHA_PITCH_TAU_MAX; tau++) {
        run += (float)p->det_d[tau];
        dn[tau] = (run > 1.0e-9f) ? ((float)p->det_d[tau] * (float)tau / run) : 1.0f;
    }

    for (uint32_t tau = ALPHA_PITCH_TAU_MIN; tau <= ALPHA_PITCH_TAU_MAX - 1u; tau++) {
        if (dn[tau] < best_v) {
            best_v   = dn[tau];
            best_tau = tau;
        }
    }

    /* YIN absolute threshold search: picks the FIRST local minimum below threshold
     * (prevents jumping to sub-harmonics while rejecting upper harmonics via CMND) */
    for (uint32_t tau = ALPHA_PITCH_TAU_MIN; tau <= ALPHA_PITCH_TAU_MAX - 1u; tau++) {
        if (dn[tau] < ALPHA_YIN_THRESH) {
            while (tau + 1u <= ALPHA_PITCH_TAU_MAX - 1u && dn[tau + 1u] < dn[tau]) {
                tau++;
            }
            pick = tau;
            break;
        }
    }

    if (pick == 0u) {
        if (best_v < 0.42f) {
            pick = best_tau;
        } else {
            pitch_commit_estimate(p, 0.0f, 0.0f);
            return;
        }
    }

    /* Octave-harmonic check: if pick/2 also has a very strong dip (within 0.04 of dn[pick]),
     * pick was a sub-octave period (2*T0); prefer pick/2. Conversely, because we searched from
     * TAU_MIN upward, we already found the shortest valid period T0 first! */
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

static void pitch_det_slice(alpha_pitch_t *p, uint32_t max_lags) {
    if (p->det_state == 0u) return;

    const uint32_t base = (uint32_t)p->det_w0 - ALPHA_PITCH_NEED;
    const int rsh = p->det_rsh;
    const int lsh = p->det_lsh;
    uint32_t tau  = p->det_tau;

    /* Stride-2 inner window sum (256 pairs across the 512-sample decimated window = 1024 raw samples = 23.2 ms):
     * covers down to 43 Hz while cutting inner-loop MACs in half. */
    for (uint32_t s = 0; s < max_lags && tau <= ALPHA_PITCH_TAU_MAX; s++, tau++) {
        uint32_t acc = 0u;
        for (uint32_t j = 0; j < ALPHA_PITCH_WIN_LEN; j += 2u) {
            const int32_t a = (ring_at(p, base + j) << lsh) >> rsh;
            const int32_t b = (ring_at(p, base + j + tau) << lsh) >> rsh;
            const int32_t d = a - b;
            acc += (uint32_t)(d * d);
        }
        p->det_d[tau] = acc;
    }
    p->det_tau = (uint16_t)tau;

    if (tau > ALPHA_PITCH_TAU_MAX) {
        p->det_state = 0u;
        pitch_det_finish(p);
    }
}

void alpha_pitch_feed_block(alpha_pitch_t *p, const float *in, int n) {
    if (!p || !in || n <= 0) return;

    p->onset   = 0u;
    p->release = 0u;

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
        if (p->det_state == 0u && p->samples_since_upd >= 64u && p->ring_w >= ALPHA_PITCH_NEED) {
            p->samples_since_upd = 0u;
            pitch_det_start(p, hf_ratio, rms_lp);
        }
        if (p->det_state == 1u) {
            pitch_det_slice(p, ALPHA_PITCH_SLICE_LAGS);
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
    if (!p || p->det_state == 0u) return;
    pitch_det_slice(p, 128u);
}
