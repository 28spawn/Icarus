/* custom-fw/src/effects/icarus/icarus_pitch.c — Icarus guitar pitch tracker, v2.
 *
 * What changed against v1 (measured on known plucked strings and on real dry captures, tools/pitch_eval.py):
 *   LATENCY  v1 took ~125 ms from a pick to a correct pitch: a 512-sample window had to fill with the new note, the search ran 96 lags per 16 ms UI tick, and a 3-point median plus smoothing
 *            needed three results. v2 anchors the analysis window at the pick (after a 2 ms skip) and GROWS it with the note: a high note is searched with a 3 ms window ~8 ms after the pick, a
 *            low E once two periods exist (~20 ms). The main loop is polled every pass, not every tick, and the note is accepted on one confident result or two agreeing ones.
 *   GHOSTS   v1 kept `valid` until the level fell below an absolute threshold (-57 dB), so the long tail of a decaying string, a hum or the noise floor stayed "a note": the voice kept sounding
 *            it. v2 ends a note when the string is ~29 dB below its own peak, or when the periodicity is gone while the level is falling, and measures the noise floor to stay above hum and hiss.
 *   RETRIGGERS  v1 raised an onset whenever the tracked pitch moved 7% (one bad window), re-triggering the envelope inside a note. v2 changes pitch only after two agreeing results (four for an
 *            octave) and reports a re-pick only for a real level surge.
 *   `valid` now means "a note is sounding AND its pitch is known": the voice starts on the first confident result (the old code started it at the pick with a stale pitch).
 *
 * Threading: see icarus_pitch.h (the audio path owns the outputs, the main loop owns the search). */
#include "icarus_pitch.h"
#include "pa_config.h"
#include "pa_plat.h"            /* PA_CP: the main-loop checkpoint store (a no-op on the host) */
#include <math.h>
#include "icarus_fastmath.h"
#include <string.h>

#define ICARUS_DEC_FS (((float)PA_SAMPLE_RATE) / (float)ICARUS_PITCH_DEC) /* 22,050 Hz */
#define ICARUS_YIN_THRESH   0.17f
#define ICARUS_CONF_OK      0.60f      /* a result counts at all above this (a dip of 0.29 or less) */
#define ICARUS_CONF_SURE    0.88f      /* a result this clean is accepted on its own */

static inline float clampf(float x, float lo, float hi) { return (x < lo) ? lo : ((x > hi) ? hi : x); }

/* libm-free log2 (exact at m = 0 and m = 1) and round: the library routines are not used in code the main loop and the interrupt share */
static inline float pitch_log2(float x) {
    union { float f; uint32_t u; } v;
    v.f = x;
    const int32_t e = (int32_t)((v.u >> 23) & 0xFFu) - 127;
    v.u = (v.u & 0x7FFFFFu) | (127u << 23);
    const float m = v.f - 1.0f;
    return (float)e + m * (1.4425449f - m * (0.7181452f - m * 0.2756003f));
}
static inline int pitch_round(float x) { const float y = x + 0.5f; int i = (int)y; if ((float)i > y) i--; return i; }
static inline float cents_between(float a, float b) { return 1200.0f * pitch_log2(a / b); }       /* log2 error ~0.005 -> 6 cents: fine for 35-cent decisions */

static inline int32_t ring_at(const icarus_pitch_t *p, uint32_t idx) { return p->ring[idx & (ICARUS_PITCH_RING_LEN - 1u)]; }

void icarus_pitch_init(icarus_pitch_t *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->frequency = p->raw_frequency = p->prev_valid_freq = p->hist_f = 110.0f;
    p->note = 45.0f;
    p->midi_note = 45u;
    p->sensitivity = 0.50f;
    p->smoothing = 0.35f;
    p->bg_age = 0xFFFFu;
}

void icarus_pitch_reset(icarus_pitch_t *p) {
    if (!p) return;
    const float sens = p->sensitivity, smth = p->smoothing, lastf = p->frequency;
    const uint16_t keep_age = p->bg_age;               /* a reset (the NaN self-heal runs it inside the audio interrupt) must not re-arm the search there */
    const uint8_t keep_seq = p->res_seq;
    memset(p, 0, sizeof(*p));
    p->frequency = (lastf >= 40.0f && lastf <= 1500.0f) ? lastf : 110.0f;
    p->raw_frequency = p->prev_valid_freq = p->hist_f = p->frequency;
    p->note = 69.0f + 12.0f * pitch_log2(p->frequency * (1.0f / 440.0f));
    p->midi_note = (uint8_t)clampf((float)pitch_round(p->note), 24.0f, 108.0f);
    p->sensitivity = sens;
    p->smoothing = smth;
    p->bg_age = keep_age;
    p->res_seq = keep_seq;
    p->res_seen = keep_seq;
}

void icarus_pitch_set_params(icarus_pitch_t *p, float sensitivity_01, float smoothing_01) {
    if (!p) return;
    p->sensitivity = clampf(sensitivity_01, 0.0f, 1.0f);
    p->smoothing = clampf(smoothing_01, 0.0f, 1.0f);
}

/* ------------------------------------------------------------------------------------------------ the search (main loop, or a small share in the interrupt when no poller runs) */

/* Hand a job over: copy the newest W + T decimated samples (the window starts after the pick's transient) into det_win, scaled so a 128-term sum of squared differences fits 32 bits. */
static int pitch_job_start(icarus_pitch_t *p) {
    int32_t d = (int32_t)(p->ring_n - p->onset_n) - (int32_t)ICARUS_PITCH_SKIP;      /* decimated samples of the new note available for analysis */
    uint32_t w, t;
    if (p->state == 1u) {                                                            /* acquiring: the window grows with the note */
        if (d < (int32_t)(2u * ICARUS_PITCH_WIN_MIN)) return 0;
        w = ((uint32_t)d >> 1) & ~7u;
        if (w > ICARUS_PITCH_WIN_MAX) w = ICARUS_PITCH_WIN_MAX;
        if (w < ICARUS_PITCH_WIN_MIN) w = ICARUS_PITCH_WIN_MIN;
        t = (uint32_t)d - w;
        if (t > ICARUS_PITCH_TAU_MAX) t = ICARUS_PITCH_TAU_MAX;
    } else if ((p->job_n++ & 1u) != 0u) {                                            /* locked, every other job: a SHORT window and only the lags around the current period, so a change of pitch shows */
        float tau_c = ICARUS_DEC_FS / p->frequency;                                   /* within ~4 ms of the change instead of after the 256-sample window has refilled */
        w = 96u;
        t = (uint32_t)(tau_c * 1.6f) + 8u;
        if (t > ICARUS_PITCH_TAU_MAX) t = ICARUS_PITCH_TAU_MAX;
        if (t < 2u * ICARUS_PITCH_TAU_MIN) t = 2u * ICARUS_PITCH_TAU_MIN;
    } else {                                                                         /* locked: the steady window, the whole lag range */
        w = ICARUS_PITCH_WIN_MAX;
        t = ICARUS_PITCH_TAU_MAX;
    }
    if (t < ICARUS_PITCH_TAU_MIN + 4u) return 0;
    const uint32_t total = w + t;
    const uint32_t base = p->ring_n - total;
    int32_t pk = 1;
    for (uint32_t i = 0; i < total; i += 2u) {
        int32_t v = ring_at(p, base + i);
        if (v < 0) v = -v;
        if (v > pk) pk = v;
    }
    int rsh = 0;
    while ((pk >> rsh) > 1023 && rsh < 10) rsh++;
    int lsh = 0;
    while (rsh == 0 && (pk << (lsh + 1)) <= 1023 && lsh < 5) lsh++;
    for (uint32_t i = 0; i < total; i++) p->det_win[i] = (int16_t)((ring_at(p, base + i) * (1 << lsh)) >> rsh);
    p->det_w = (uint16_t)w;
    p->det_t = (uint16_t)t;
    p->det_tau = 1u;
    p->det_found = 0u;
    p->det_cand = 0u;
    p->det_run = 0.0f;
    p->det_best_v = 1.0e9f;
    p->det_best_tau = 0u;
    p->det_dn[0] = 1.0f;
    return 1;
}

/* ---- more than one string at once -------------------------------------------------------------------------------------------------------------------------------------------------------
 * A single string repeats exactly, so its difference curve has one clean dip (below 0.15) and the search stops there. Two strings do not: each one's period repeats only its own share of the signal,
 * so the curve has several shallow dips (0.2 .. 0.6, one per string and per multiple of it) and one very deep dip at the COMMON period, a pitch nobody played (E2 + B2 repeat together every 41 Hz).
 * The root of the chord is the lowest string that really sounds: take the dips, throw away the multiples of a shorter dip (a note's own octave dips), throw away the common period (a multiple of two
 * dips that are not multiples of each other), and report the longest period left. */
#define ICARUS_CLEAN_FUND_THR 0.004f                                            /* a clean period is one string until proven otherwise: only a pitch with NO power of its own (-24 dB under its multiples) is a phantom */
#define ICARUS_ROOT_MAXC   14u
#define ICARUS_ROOT_THRESH 0.62f
#define ICARUS_ROOT_TAU_LOW 475u                                                /* 53.1 Hz: covers Drop A (55 Hz, tau=401) and low B (61.7 Hz, tau=357) */
static int root_is_mult(uint32_t c, uint32_t a) {                              /* c is (about) an integer multiple, 2..8, of a */
    if (a < 1u || c < 2u * a - 2u) return 0;
    const uint32_t k = (c + a / 2u) / a;
    if (k < 2u || k > 8u) return 0;
    const int32_t e = (int32_t)c - (int32_t)(k * a);
    const int32_t tol = (int32_t)(c / 40u) + 2;                                   /* 2.5 % and two samples */
    return (e <= tol && e >= -tol);
}
/* cos for 0 <= x <= pi (no libm: this runs in code shared with the interrupt) */
static float pc_cos(float x) {
    float sgn = 1.0f;
    if (x > 1.5707963f) { x = 3.1415927f - x; sgn = -1.0f; }
    const float x2 = x * x;
    return sgn * (1.0f - x2 * (0.5f - x2 * (0.0416667f - x2 * (0.0013889f - x2 * 0.0000248f))));
}

/* The power at f_hz in the newest samples of the job window (every second sample, Hann weighted: ~11 kHz, up to 400 points = 36 ms): a Goertzel resonator with the window folded in. */
static float root_power(const icarus_pitch_t *p, float f_hz) {
    const uint32_t total = (uint32_t)p->det_w + (uint32_t)p->det_t;
    uint32_t n = total >> 1;
    if (n > 400u) n = 400u;
    if (n < 24u) return 0.0f;
    const int16_t *x = p->det_win + (total - 2u * n);
    const float coeff = 2.0f * pc_cos(6.2831853f * f_hz / (ICARUS_DEC_FS * 0.5f));
    const float hk = 2.0f * pc_cos(6.2831853f / (float)n);
    float c0 = 1.0f, c1 = pc_cos(6.2831853f / (float)n);          /* cos(2 pi i / n) for i = 0, 1 */
    float s1 = 0.0f, s2 = 0.0f;
    for (uint32_t i = 0u; i < n; i++) {
        const float h = 0.5f - 0.5f * c0;
        const float s0 = h * (float)x[2u * i] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
        const float c2 = hk * c1 - c0;
        c0 = c1; c1 = c2;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/* A true fundamental has power of its own at f; the common period of two or three strings (a pitch an octave or more under what was played) has none: the strongest of its 2nd..4th multiples
 * stands alone. The root must hold at least -14 dB (power 0.04) of the strongest of those multiples. */
static int root_has_fundamental(const icarus_pitch_t *p, float f_hz, float thr) {
    if (f_hz > 1250.0f || p->det_light) return 1;
    uint32_t n = ((uint32_t)p->det_w + (uint32_t)p->det_t) >> 1;
    if (n > 400u) n = 400u;
    if ((float)n * f_hz < 12000.0f) return (f_hz >= 60.0f);                        /* too short a window to tell its power from its neighbours': take the period at its word down to 50 Hz (Drop A is 55 Hz) */
    const float p1 = root_power(p, f_hz);
    float pm = 0.0f;
    for (uint32_t h = 2u; h <= 4u; h++) { const float q = root_power(p, f_hz * (float)h); if (q > pm) pm = q; }
    return p1 >= thr * pm;
}

/* The period found is nobody's note (the common period of several strings): the notes played are among its multiples. Take the LOWEST multiple that has real power, that is the lowest string. */
static uint32_t pitch_harmonic_root(const icarus_pitch_t *p, float f0) {
    if (p->det_light) return 0u;
    float pw[8];
    float pmax = 0.0f;
    uint32_t hn = 0u;
    for (uint32_t h = 2u; h <= 7u && f0 * (float)h <= 1250.0f; h++) {
        pw[h] = root_power(p, f0 * (float)h);
        if (pw[h] > pmax) pmax = pw[h];
        hn = h;
    }
    for (uint32_t h = 2u; h <= hn; h++) {
        if (pw[h] >= 0.15f * pmax && pmax > 0.0f) return (uint32_t)(ICARUS_DEC_FS / (f0 * (float)h) + 0.5f);
    }
    return 0u;
}

static uint32_t pitch_root_pick(icarus_pitch_t *p, uint32_t last) {            /* last: the largest lag whose difference has been computed */
    const float *dn = p->det_dn;
    uint16_t ct[ICARUS_ROOT_MAXC];
    float cv[ICARUS_ROOT_MAXC];
    uint32_t n = 0u;
    float vmin = 1.0e9f;
    for (uint32_t tau = ICARUS_PITCH_TAU_MIN + 1u; tau + 1u <= last; tau++) {
        const float v = dn[tau];
        if (!(v < ICARUS_ROOT_THRESH && v <= dn[tau - 1u] && v < dn[tau + 1u])) continue;
        if (n > 0u && tau - ct[n - 1u] <= (uint32_t)(ct[n - 1u] / 25u) + 2u) {   /* ripples of one dip: keep the deepest */
            if (v < cv[n - 1u]) { ct[n - 1u] = (uint16_t)tau; cv[n - 1u] = v; }
        } else if (n < ICARUS_ROOT_MAXC) { ct[n] = (uint16_t)tau; cv[n] = v; n++; }
        if (v < vmin) vmin = v;
    }
    if (n == 0u) return 0u;
    uint16_t keep[ICARUS_ROOT_MAXC];
    uint32_t nk = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        const uint32_t c = ct[i];
        int drop = 0;
        for (uint32_t a = 0u; a < i && !drop; a++) {                              /* a multiple of a shorter dip, unless it explains the signal much better than that dip does */
            if (root_is_mult(c, ct[a]) && (cv[a] < 0.28f || cv[i] >= 0.5f * cv[a])) drop = 1;
        }
        for (uint32_t a = 0u; a < i && !drop; a++) {                              /* the common period of two strings */
            if (!root_is_mult(c, ct[a])) continue;
            for (uint32_t b = a + 1u; b < i; b++) if (!root_is_mult(ct[b], ct[a]) && root_is_mult(c, ct[b])) { drop = 1; break; }
        }
        if (drop || cv[i] > vmin + 0.35f || c > ICARUS_ROOT_TAU_LOW) continue;
        keep[nk++] = (uint16_t)c;
    }
#ifdef PA_POLY_DEBUG
    { extern int printf(const char *, ...); printf("cands:"); for (uint32_t i = 0u; i < n; i++) printf(" %u(%.2f)", (unsigned)ct[i], cv[i]); printf(" | keep:"); for (uint32_t i = 0u; i < nk; i++) printf(" %u", (unsigned)keep[i]); printf("\n"); }
#endif
    for (uint32_t i = nk; i > 0u; i--) {                                           /* the longest period that is a real fundamental wins */
        if (root_has_fundamental(p, ICARUS_DEC_FS / (float)keep[i - 1u], 0.04f)) return keep[i - 1u];
    }
    return nk ? pitch_harmonic_root(p, ICARUS_DEC_FS / (float)keep[nk - 1u]) : 0u;     /* every dip is a phantom: look among its multiples */
}

static void pitch_job_finish(icarus_pitch_t *p, uint32_t pick, int poly) {
    float f = 0.0f, conf = 0.0f;
    const float *dn = p->det_dn;
    if (pick > ICARUS_PITCH_TAU_MIN && pick + 6u <= p->det_t) {                /* a dip at the very end of the lags is an edge effect, not a period */
        const float dip = dn[pick];
        conf = poly ? clampf(0.90f - dip * 0.6f, 0.62f, 0.80f) : clampf(1.0f - dip * 1.4f, 0.0f, 1.0f);   /* a root picked out of several strings is never "sure": it has to be confirmed by a second result */
        const float y0 = dn[pick - 1u], y1 = dip, y2 = dn[pick + 1u];
        const float den = y0 - 2.0f * y1 + y2;
        float tau_f = (float)pick;
        if (fabsf(den) > 1.0e-7f) tau_f += clampf(0.5f * (y0 - y2) / den, -0.5f, 0.5f);
        f = ICARUS_DEC_FS / tau_f;
    }
    p->res_freq = f;
    p->res_conf = conf;
    p->res_poly = (uint8_t)(poly && f > 0.0f);
    __asm__ volatile("" ::: "memory");
    p->res_seq = (uint8_t)(p->res_seq + 1u);
    __asm__ volatile("" ::: "memory");
    p->det_state = 0u;                                  /* the audio path owns the job fields again */
}

/* A clean period is not always one string: chords built from the harmonic series (a power chord, a D major triad in equal temperament) repeat exactly at a pitch nobody played. If the pitch found has no
 * power of its own, take the root out of the dips seen so far instead. */
static void pitch_finish_clean(icarus_pitch_t *p, uint32_t cand, uint32_t last) {
    const float f = (cand > ICARUS_PITCH_TAU_MIN) ? ICARUS_DEC_FS / (float)cand : 0.0f;
    if (f > 0.0f && f < 600.0f && !root_has_fundamental(p, f, ICARUS_CLEAN_FUND_THR)) {
        uint32_t r = pitch_root_pick(p, last);
        if (r == 0u) r = pitch_harmonic_root(p, f);
        pitch_job_finish(p, r, 1);
    }
    else pitch_job_finish(p, cand, 0);
}

/* Evaluate up to max_lags lags of the pending job; the normalised difference is built as the lags come in, and the search stops at the first YIN dip once it has been walked to its bottom. */
static void pitch_search(icarus_pitch_t *p, uint32_t max_lags) {
    if (p->det_state == 0u) return;
    const int16_t *w = p->det_win;
    float *dn = p->det_dn;
    float run = p->det_run;
    uint32_t tau = p->det_tau;
    const uint32_t W = p->det_w, T = p->det_t;
    for (uint32_t s = 0; s < max_lags && tau <= T; s++, tau++) {
        const int16_t *wt = w + tau;
        uint32_t acc = 0u;
        for (uint32_t j = 0; j < W; j += 8u) {         /* stride-2 sum: W/2 terms, unrolled by 4 */
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
        if (tau >= ICARUS_PITCH_TAU_MIN && tau < T) {
            if (v < p->det_best_v) { p->det_best_v = v; p->det_best_tau = (uint16_t)tau; }
            if (!p->det_found) {
                if (v < ICARUS_YIN_THRESH) { p->det_found = 1u; p->det_cand = (uint16_t)tau; }
            } else if (v < dn[p->det_cand]) {
                p->det_cand = (uint16_t)tau;           /* still going down: the minimum is further on */
            } else {
                pitch_finish_clean(p, p->det_cand, tau);  /* dn[cand + 1] is not lower than dn[cand]: cand is the minimum */
                return;
            }
        } else if (p->det_found) {
            pitch_finish_clean(p, p->det_cand, tau);      /* ran off the end of the lags while walking down */
            return;
        }
    }
    p->det_run = run;
    p->det_tau = (uint16_t)tau;
    if (tau > T) {
        if (p->det_found) pitch_finish_clean(p, p->det_cand, (uint32_t)p->det_t - 5u);
        else {
            const uint32_t root = pitch_root_pick(p, (uint32_t)p->det_t - 5u);                          /* no clean period: several strings, or a noisy attack */
            pitch_job_finish(p, root, 1);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ the note state machine (audio path) */

static void pitch_set_output(icarus_pitch_t *p, float f) {
    p->frequency = f;
    p->prev_valid_freq = f;
    p->note = 69.0f + 12.0f * pitch_log2(f * (1.0f / 440.0f));
    int cand = pitch_round(p->note);
    cand = (cand < 24) ? 24 : ((cand > 108) ? 108 : cand);
    if (cand != (int)p->midi_note) {
        const float c = (p->note - (float)p->midi_note) * 100.0f;
        if (c > 62.0f || c < -62.0f) p->midi_note = (uint8_t)cand;
    }
}

/* The consensus of the recent several-strings results: the pitch most of them are within 40 cents of (ties go to the lower: the root), or 0 when no pitch has enough support yet. */
static float pitch_poly_vote(icarus_pitch_t *p, float f) {
    p->pv_f[p->pv_i] = f;
    p->pv_i = (uint8_t)((p->pv_i + 1u) % 12u);
    if (p->pv_n < 12u) p->pv_n++;
    float best_f = 0.0f;
    uint32_t best_s = 0u;
    for (uint32_t i = 0u; i < p->pv_n; i++) {
        uint32_t s = 0u;
        float sum = 0.0f;
        for (uint32_t j = 0u; j < p->pv_n; j++) if (fabsf(cents_between(p->pv_f[j], p->pv_f[i])) < 40.0f) { s++; sum += p->pv_f[j]; }
        const float m = sum / (float)s;
        if (s > best_s || (s == best_s && m < best_f)) { best_s = s; best_f = m; }
    }
    return (best_s >= 4u) ? best_f : 0.0f;
}

static void pitch_handle_result(icarus_pitch_t *p, float f, float conf, int poly) {
    int ok = (f > 40.0f && f < 1600.0f && conf >= ICARUS_CONF_OK);
    if (ok && poly) {
        f = pitch_poly_vote(p, f);
        ok = (f > 0.0f);
        conf = 0.70f;
        if (!ok) return;                                                      /* not enough of them agree yet: no decision, and a locked note is not penalised for it */
    } else if (ok) {
        p->pv_n = 0u;                                                         /* a clean period: one string, whatever the votes were */
    }
    if (p->state == 1u) {                                                     /* ACQUIRING: the first confident, agreeing result starts the note */
        if (!ok) return;
        if (p->cand_n > 0u && fabsf(cents_between(f, p->cand_f)) < 45.0f) { p->cand_n++; p->cand_f = 0.5f * (p->cand_f + f); }
        else { p->cand_n = 1u; p->cand_f = f; }
        if (conf >= ICARUS_CONF_SURE || p->cand_n >= 2u) {
            const float f0 = (conf >= ICARUS_CONF_SURE && p->cand_n == 1u) ? f : p->cand_f;
            p->raw_frequency = f;
            pitch_set_output(p, f0);
            p->hist_f = f0;
            p->confidence = conf;
            p->stability = 0.5f;
            p->legato = 0u;
            p->onset = 1u;
            p->valid = 1u;
            p->was_valid = 1u;
            p->state = 2u;
            p->cand_n = 0u;
            p->pv_n = 0u;
            p->fail_run = 0u;
        }
        return;
    }
    if (p->state != 2u) return;
    if (!ok) { if (p->fail_run < 65535u) p->fail_run++; return; }            /* LOCKED, but this window found no period: hold the pitch */
    p->fail_run = 0u;
    p->raw_frequency = f;
    p->confidence = conf;
    const float c = cents_between(f, p->frequency);
    const float ac = fabsf(c);
    if (ac < 35.0f) {                                                        /* the same pitch (or a bend / vibrato step): follow it, firmer on a clean result */
        const float a = clampf((0.75f - 0.55f * p->smoothing) * (0.5f + 0.5f * conf), 0.15f, 0.8f);
        pitch_set_output(p, p->frequency + a * (f - p->frequency));
        p->hist_f = f;
        p->cand_n = 0u;
        p->stability = clampf(p->stability * 0.9f + 0.1f * (1.0f - ac * (1.0f / 35.0f)), 0.0f, 1.0f);
        return;
    }
    /* an octave, two or three octaves, an octave plus a fifth (a harmonic taken for the fundamental, or the other way round): the classic mistakes of a decaying string */
    const int oct = (fabsf(ac - 1200.0f) < 80.0f) || (fabsf(ac - 1902.0f) < 80.0f) || (fabsf(ac - 2400.0f) < 80.0f) || (fabsf(ac - 3102.0f) < 80.0f) || (fabsf(ac - 3600.0f) < 80.0f);
    const uint8_t need = oct ? 4u : 2u;                                      /* a different pitch is believed only when results agree on it */
    if (p->cand_n > 0u && fabsf(cents_between(f, p->cand_f)) < 45.0f) { p->cand_n++; p->cand_f = 0.5f * (p->cand_f + f); }
    else { p->cand_n = 1u; p->cand_f = f; }
    if (p->cand_n >= need) {
        pitch_set_output(p, p->cand_f);
        p->hist_f = p->cand_f;
        p->legato = 1u;                                                      /* a change of pitch on a ringing string: hammer-on, pull-off, a big bend step */
        p->onset = 1u;
        p->cand_n = 0u;
        p->stability = 0.3f;
    }
}

static void pitch_end_note(icarus_pitch_t *p) {
    if (p->valid) { p->release = 1u; }
    p->valid = 0u;
    p->was_valid = 0u;
    p->state = 0u;
    p->confidence = 0.0f;
    p->stability = 0.0f;
    p->env_peak = 0.0f;
    p->quiet_blocks = 0u;
    p->fail_run = 0u;
    p->cand_n = 0u;
    p->pv_n = 0u;
}

void icarus_pitch_feed_block(icarus_pitch_t *p, const float *in, int n) {
    if (!p || !in || n <= 0) return;

    p->onset = 0u;
    p->release = 0u;
    if (p->bg_age < 65535u) p->bg_age++;

    const float sens_scale = 1.8f - 1.3f * p->sensitivity;                   /* 0.5 .. 1.8 */
    const float open_thr = 0.0028f * sens_scale;
    const float close_thr = open_thr * 0.45f;

    const float k_lp_a = 0.20f;                                              /* 1.6 kHz 2-pole low-pass (the fundamental over the harmonics) */
    const float k_hp_r = 0.9946f;                                            /* 38 Hz high-pass */

    float hx1 = p->hp_x1, hy1 = p->hp_y1, z1 = p->lp_z1, z2 = p->lp_z2;
    float ef = p->env_fast, es = p->env_slow, acc = p->dec_acc;
    uint8_t dcnt = p->dec_cnt;
    uint16_t rw = p->ring_w;
    uint32_t rn = p->ring_n;
    for (int i = 0; i < n; i++) {
        const float x = in[i];
        const float y_hp = x - hx1 + k_hp_r * hy1;
        hx1 = x;
        hy1 = y_hp;
        z1 += k_lp_a * (y_hp - z1);
        z2 += k_lp_a * (z1 - z2);
        const float a = (y_hp < 0.0f) ? -y_hp : y_hp;
        ef += ((a > ef) ? 0.025f : 0.0020f) * (a - ef);                      /* fast level: ~1.8 ms attack, ~23 ms release */
        es += ((a > es) ? 0.0015f : 0.0006f) * (a - es);                     /* slow level */
        acc += z2;
        if (++dcnt >= ICARUS_PITCH_DEC) {                                     /* decimate by 2 into the ring */
            int32_t q15 = (int32_t)(acc * (1.0f / (float)ICARUS_PITCH_DEC) * 32767.0f);
            if (q15 > 32767) q15 = 32767;
            if (q15 < -32768) q15 = -32768;
            p->ring[rw & (ICARUS_PITCH_RING_LEN - 1u)] = (int16_t)q15;
            rw++;
            rn++;
            acc = 0.0f;
            dcnt = 0u;
        }
    }
    p->hp_x1 = hx1; p->hp_y1 = hy1; p->lp_z1 = z1; p->lp_z2 = z2;
    p->env_fast = ef; p->env_slow = es; p->dec_acc = acc;
    p->dec_cnt = dcnt; p->ring_w = rw; p->ring_n = rn;

    /* the noise floor: follows the slow level down at once, up very slowly, capped below notes (hum and hiss are steady; a note is not) */
    float fl = p->env_floor;
    fl += ((es < fl) ? 0.20f : 0.0004f) * (es - fl);
    const float fl_max = open_thr * 1.1f;
    if (fl > fl_max) fl = fl_max;
    if (fl < 1.0e-6f) fl = 1.0e-6f;
    p->env_floor = fl;
    const float open_eff = (open_thr > fl * 3.2f) ? open_thr : fl * 3.2f;
    const float close_eff = (close_thr > fl * 2.0f) ? close_thr : fl * 2.0f;

    /* take the main loop's answer (the audio path owns the outputs) */
    if (p->det_state == 0u && p->res_seq != p->res_seen) {
        p->res_seen = p->res_seq;
        if (p->state != 0u) pitch_handle_result(p, p->res_freq, p->res_conf, p->res_poly);
    }

    if (p->state == 0u) {
        if (ef > open_eff && ef > es * 1.25f) {                              /* a pick out of silence */
            p->state = 1u;
            p->onset_n = rn;
            p->last_job_n = rn;
            p->env_peak = ef;
            p->quiet_blocks = 0u;
            p->cand_n = 0u;
            p->pv_n = 0u;
            p->fail_run = 0u;
        }
    } else {
        if (ef > p->env_peak) p->env_peak = ef; else p->env_peak *= 0.9985f; /* ~1.8 s time constant */
        /* a new pick on a ringing string: a real surge of the level, not a wobble of the decay */
        if (ef > es * 1.65f + open_eff * 2.0f && ef > p->env_peak * 0.40f && (rn - p->onset_n) > 1760u) {      /* not within 80 ms of the last pick: the surge of the pick itself is still in the followers */
            p->state = 1u;                                                   /* re-acquire: the voice keeps sounding the old pitch until the new one is known */
            p->onset_n = rn;
            p->last_job_n = rn;
            p->env_peak = ef;
            p->cand_n = 0u;
            p->pv_n = 0u;
            p->fail_run = 0u;
            p->env_slow = ef;
        }
        const float end_lvl = (close_eff > p->env_peak * 0.035f) ? close_eff : p->env_peak * 0.035f;    /* the string is ~29 dB below its own peak */
        if (ef < end_lvl) { if (p->quiet_blocks < 65535u) p->quiet_blocks++; } else p->quiet_blocks = 0u;
        const int lost = (p->state == 2u && p->fail_run >= 14u && ef < p->env_peak * 0.30f)               /* the periodicity is gone while the level falls: a decaying, inharmonic tail */
                      || (p->state == 1u && (rn - p->onset_n) > 3300u && ef < p->env_peak * 0.5f);        /* nothing periodic within 150 ms of the pick */
        if (p->quiet_blocks >= 6u || lost) pitch_end_note(p);
    }

    /* hand the main loop a job: while a note is being acquired or tracked, about every 1.8 ms */
    if (p->state != 0u && p->det_state == 0u && (rn - p->last_job_n) >= (p->pv_n ? 3u * ICARUS_PITCH_UPD : ICARUS_PITCH_UPD)) {   /* several strings: the search is dearer (the whole lag range and the spectrum check), so it runs a third as often */
        if (pitch_job_start(p)) { p->last_job_n = rn; p->det_state = 1u; }
    }
    /* no poller (host tests, a stalled main loop): a small share of the search runs here so the tracker still works */
    if (p->det_state == 1u && p->bg_age > ICARUS_PITCH_BG_STALE) { p->det_light = 1u; pitch_search(p, 24u); p->det_light = 0u; }
}

void icarus_pitch_poll_background(icarus_pitch_t *p) {
    if (!p) return;
    p->bg_age = 0u;                                   /* the main loop is alive: the audio path leaves the search to it */
    if (p->det_state == 0u) return;
    PA_CP(0x79);                                      /* main loop only: inside the lag search */
    pitch_search(p, ICARUS_PITCH_BG_LAGS);
    PA_CP(0x7A);
}
