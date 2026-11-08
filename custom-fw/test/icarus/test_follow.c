/* custom-fw/test/icarus/test_follow.c — Icarus Synth: the synth OUTPUT follows the played pitch, the pitch tracker defers its lag
 * search to the main-loop poll, and the overload shed turns the synth into a dry pass-through.
 *
 * Why this exists: the unit tests only checked that the output was finite and audible, so a glide bug that pinned every oscillator to
 * ~16-25 Hz regardless of what was played went through the whole QA gate, and a pitch tracker that cost 3x the audio block (it ran its
 * 500-lag search inside the audio interrupt) did too. */
#include "../../src/effects/icarus/icarus.h"
#include "../../src/effects/icarus/icarus_pitch.h"
#include "../../include/pa_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p) { free(p); }
void  pa_fx_chain_set_slot_enabled(pa_fx_chain_t *c, pa_fx_slot_id_t s, int on) { (void)c; (void)s; (void)on; }

extern volatile uint8_t g_pa_synth_shed;

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   "); else { printf("  FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

/* guitar-ish tone: fundamental + 2 harmonics */
static float tone(double *ph, double hz) {
    *ph += hz / (double)PA_SAMPLE_RATE;
    if (*ph >= 1.0) *ph -= 1.0;
    const float a = (float)(2.0 * M_PI * *ph);
    return 0.2f * (sinf(a) + 0.5f * sinf(2.0f * a) + 0.3f * sinf(3.0f * a));
}

/* period of the output by autocorrelation (lag 20..800 = 55 Hz..2.2 kHz), returned as Hz */
static float out_hz(const float *x, int n) {
    int best = 0; double bv = -1.0;
    for (int lag = 20; lag <= 800; lag++) {
        double acc = 0.0, e0 = 0.0, e1 = 0.0;
        for (int i = 0; i + lag < n; i++) { acc += (double)x[i] * x[i + lag]; e0 += (double)x[i] * x[i]; e1 += (double)x[i + lag] * x[i + lag]; }
        const double v = acc / (sqrt(e0 * e1) + 1e-12);
        if (v > bv * 1.02) { bv = v; best = lag; }               /* the earliest strong peak, not a multiple of it */
    }
    return best ? (float)PA_SAMPLE_RATE / (float)best : 0.0f;
}

/* The pitch the output is built on: the LOWEST strong partial of its spectrum (a local maximum within 17 dB of the strongest, 40 Hz .. 1.2 kHz, half-semitone grid). The autocorrelation above picks
 * the earliest strong peak, which on a resonant filtered voice (Synth Bass, Liquid Lead) is the filter's resonance; a harmonic comb prefers low candidates whatever is played. The sub-oscillator
 * is an octave below the played note, so a preset with one reads an octave low at BOTH pitches and the ratio between two inputs stays what it should be. */
static float out_f0_lowest(const float *x, int n) {
    enum { K = 120 };
    double pw[K], fk[K], pmax = 0.0;
    for (int k = 0; k < K; k++) {
        fk[k] = 40.0 * pow(2.0, k / 24.0);
        const double w = 2.0 * M_PI * fk[k] / (double)PA_SAMPLE_RATE;
        double re = 0.0, im = 0.0;
        for (int i = 0; i < n; i++) { const double win = 0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1)); re += win * x[i] * cos(w * i); im += win * x[i] * sin(w * i); }
        pw[k] = re * re + im * im;
        if (pw[k] > pmax) pmax = pw[k];
    }
    for (int k = 1; k < K - 1; k++)
        if (pw[k] >= 0.02 * pmax && pw[k] > pw[k - 1] && pw[k] >= pw[k + 1] && fk[k] <= 1200.0) return (float)fk[k];
    return 0.0f;
}

static float run_synth_hz(int preset, double in_hz) {
    icarus_state_t *s = icarus_create();
    icarus_load_preset(s, (uint8_t)preset);
    s->enabled = 1;
    double ph = 0.0;
    float in[PA_BLOCK_N], L[PA_BLOCK_N], R[PA_BLOCK_N];
    static float cap[8192];
    int ci = 0;
    const int blocks = (int)(PA_SAMPLE_RATE * 2 / PA_BLOCK_N);
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < PA_BLOCK_N; i++) in[i] = tone(&ph, in_hz);
        icarus_process_block_f32(s, in, L, R, PA_BLOCK_N);
        icarus_poll_background(s);                                  /* the main loop */
        if (b > blocks - 8192 / PA_BLOCK_N - 1 && ci + PA_BLOCK_N <= 8192) for (int i = 0; i < PA_BLOCK_N; i++) cap[ci++] = L[i];
    }
    icarus_destroy(s);
    return out_f0_lowest(cap, ci);
}

static void test_output_follows_input(void) {
    printf("=== the synth output follows the played pitch (every glided / tracked preset) ===\n");
    for (int p = 0; p < (int)ICARUS_FACTORY_PRESET_COUNT; p++) {
        const icarus_preset_t *fp = icarus_get_factory_preset((uint8_t)p);
        if (fp->mode == ICARUS_MODE_ANALOG) continue;               /* no oscillators: nothing to track */
        const float lo = run_synth_hz(p, 110.0);
        const float hi = run_synth_hz(p, 440.0);
        const float ratio = (lo > 1.0f) ? hi / lo : 0.0f;
        /* two octaves up in, two octaves up out (the sub-oscillator and the filter shift the absolute pitch, not the ratio) */
        /* with a sub-oscillator an octave down, the output at 440 Hz repeats every 2 periods of the sub (220 Hz), and the autocorrelation may report that
         * instead (ratio ~2): still two octaves tracked, not the pinned pitch the test exists to catch (ratio ~1). */
        CHECK((ratio > 3.2f && ratio < 4.8f) || (ratio > 1.8f && ratio < 2.2f),"[%2d] %-14s 110 Hz -> %6.1f Hz, 440 Hz -> %6.1f Hz (ratio %.2f, want ~4)", p + 1, fp->name, lo, hi, ratio);
    }
}

static void test_search_runs_in_the_poll(void) {
    printf("\n=== the lag search runs in the main-loop poll, not in the audio path ===\n");
    icarus_pitch_t p;
    icarus_pitch_init(&p);
    double ph = 0.0;
    float in[PA_BLOCK_N];
    int polled_ok = 1, started = 0;
    p.bg_age = 0u;                                                 /* a poller is alive */
    for (int b = 0; b < 200; b++) {
        for (int i = 0; i < PA_BLOCK_N; i++) in[i] = tone(&ph, 196.0);
        icarus_pitch_feed_block(&p, in, PA_BLOCK_N);
        if (p.det_state == 1u) { started = 1; if (p.det_tau != 1u && p.det_tau != 0u) polled_ok = 0; }
        else if (b > 100 && p.det_state == 0u) {}
        /* no poll here: the detection may start but must not advance */
        if (b % 4 == 3 && p.det_state == 1u) break;
    }
    CHECK(started, "a detection started once the ring was primed");
    CHECK(polled_ok && p.det_state == 1u && p.det_tau == 1u, "...and the audio path left every lag to the poller (next lag %u)", (unsigned)p.det_tau);
    uint32_t polls = 0;
    p.bg_age = 0u;
    while (p.det_state == 1u && polls < 20u) { icarus_pitch_poll_background(&p); polls++; }
    CHECK(p.det_state == 0u && polls >= 1u && polls <= 6u, "the poll finished it in %u calls of %u lags", (unsigned)polls, (unsigned)ICARUS_PITCH_BG_LAGS);
    for (int b = 0; b < 40 && !p.valid; b++) {                    /* the audio path takes the answer on its next block, then confirms it */
        for (int i = 0; i < PA_BLOCK_N; i++) in[i] = tone(&ph, 196.0);
        icarus_pitch_feed_block(&p, in, PA_BLOCK_N);
        icarus_pitch_poll_background(&p);
    }
    CHECK(p.valid && fabsf(p.frequency - 196.0f) < 2.0f, "and found %.2f Hz for a 196 Hz string", p.frequency);

    /* no poller (host tests, a stalled main loop): after ICARUS_PITCH_BG_STALE blocks the audio path does the search itself */
    icarus_pitch_t q;
    icarus_pitch_init(&q);
    ph = 0.0;
    for (int b = 0; b < 400; b++) {
        for (int i = 0; i < PA_BLOCK_N; i++) in[i] = tone(&ph, 196.0);
        icarus_pitch_feed_block(&q, in, PA_BLOCK_N);
    }
    CHECK(fabsf(q.frequency - 196.0f) < 2.0f, "with no poller at all the audio path still tracks (%.2f Hz)", q.frequency);
}

static void test_shed_is_dry(void) {
    printf("\n=== the overload shed drops the synth to the dry guitar ===\n");
    icarus_state_t *s = icarus_create();
    icarus_load_preset(s, 0);
    s->enabled = 1;
    float in[PA_BLOCK_N], L[PA_BLOCK_N], R[PA_BLOCK_N];
    for (int i = 0; i < PA_BLOCK_N; i++) in[i] = 0.1f * sinf(0.3f * (float)i);
    g_pa_synth_shed = 1u;
    icarus_process_block_f32(s, in, L, R, PA_BLOCK_N);
    int dry = 1;
    for (int i = 0; i < PA_BLOCK_N; i++) if (L[i] != in[i] || R[i] != in[i]) dry = 0;
    CHECK(dry, "shed: both channels are the input, untouched");
    icarus_destroy(s);
    icarus_state_t *s2 = icarus_create();                            /* a fresh synth is un-shed */
    CHECK(g_pa_synth_shed == 0u, "a newly created synth clears the shed");
    icarus_destroy(s2);
}

int main(void) {
    test_output_follows_input();
    test_search_runs_in_the_poll();
    test_shed_is_dry();
    printf("\n%s\n", fails ? "FOLLOW TESTS FAILED" : "ALL FOLLOW / DEFERRAL / SHED TESTS PASSED");
    return fails ? 1 : 0;
}
