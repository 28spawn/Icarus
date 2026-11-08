/* custom-fw/test/icarus/test_pitch.c — Unit tests for Icarus Synth Pitch Tracker */
#include "../../src/effects/icarus/icarus_pitch.h"
#include "../../include/pa_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void feed_guitar_note(icarus_pitch_t *p, float freq_hz, float amp, float h2_ratio, float h3_ratio, int num_blocks) {
    float blk[PA_BLOCK_N];
    static double ph = 0.0;
    for (int b = 0; b < num_blocks; b++) {
        for (int i = 0; i < PA_BLOCK_N; i++) {
            ph += (double)freq_hz / (double)PA_SAMPLE_RATE;
            if (ph >= 1.0) ph -= 1.0;
            float s = sinf((float)(2.0 * M_PI * ph))
                    + h2_ratio * sinf((float)(4.0 * M_PI * ph + 0.3))
                    + h3_ratio * sinf((float)(6.0 * M_PI * ph + 0.7));
            blk[i] = amp * s;
        }
        icarus_pitch_feed_block(p, blk, PA_BLOCK_N);
        icarus_pitch_poll_background(p);
    }
}

static void test_note_range(void) {
    static const struct {
        const char *name;
        float       freq;
        uint8_t     midi;
    } k_notes[] = {
        { "B1 (7-string low B)", 61.74f, 35u },   /* below 58 Hz is not tracked as a note: it is where the common period of a chord lives */
        { "E2 (Open 6th)",     82.41f,   40u },
        { "A2 (Open 5th)",     110.00f,  45u },
        { "D3 (Open 4th)",     146.83f,  50u },
        { "G3 (Open 3rd)",     196.00f,  55u },
        { "B3 (Open 2nd)",     246.94f,  59u },
        { "E4 (Open 1st)",     329.63f,  64u },
        { "A4 (5th Fret 1st)", 440.00f,  69u },
        { "E5 (12th Fret 1st)",659.25f,  76u },
        { "C6 (20th Fret 1st)",1046.50f, 84u },
    };

    for (size_t k = 0; k < sizeof(k_notes) / sizeof(k_notes[0]); k++) {
        icarus_pitch_t p;
        icarus_pitch_init(&p);

        /* Feed ~60 blocks (87 ms) of realistic harmonic guitar tone */
        feed_guitar_note(&p, k_notes[k].freq, 0.25f, 0.50f, 0.25f, 60);

        float cents_err = 1200.0f * log2f(p.frequency / k_notes[k].freq);
        printf("[test_pitch] %-20s target=%7.2f Hz, tracked=%7.2f Hz (err=%+5.1f cents, conf=%.2f, midi=%u, valid=%u)\n",
               k_notes[k].name, k_notes[k].freq, p.frequency, cents_err, p.confidence, p.midi_note, p.valid);

        assert(p.valid == 1u);
        assert(p.confidence > 0.70f);
        assert(fabsf(cents_err) < 25.0f);
        assert(p.midi_note == k_notes[k].midi);
    }
}

static void test_strong_harmonics_no_octave_jump(void) {
    icarus_pitch_t p;
    icarus_pitch_init(&p);

    /* Start on A2 (110 Hz) with strong 2nd (220 Hz) and 3rd (330 Hz) harmonics */
    feed_guitar_note(&p, 110.0f, 0.25f, 0.85f, 0.60f, 65);
    float cents_err = 1200.0f * log2f(p.frequency / 110.0f);
    printf("[test_pitch] Strong harmonics A2 (110 Hz, H2=0.85, H3=0.60): tracked=%.2f Hz (%+.1f cents)\n",
           p.frequency, cents_err);
    assert(p.valid == 1u);
    assert(fabsf(cents_err) < 25.0f);
}

static void test_bends_and_vibrato(void) {
    icarus_pitch_t p;
    icarus_pitch_init(&p);

    /* Establish A3 (220 Hz) */
    feed_guitar_note(&p, 220.0f, 0.30f, 0.4f, 0.2f, 55);
    assert(p.valid == 1u);

    /* Smoothly bend up 2 semitones to B3 (246.94 Hz) over 60 blocks, then sustain at 246.94 Hz for 25 blocks */
    for (int b = 0; b < 60; b++) {
        float f_cur = 220.0f + (246.94f - 220.0f) * ((float)(b + 1) / 60.0f);
        feed_guitar_note(&p, f_cur, 0.30f, 0.4f, 0.2f, 1);
    }
    feed_guitar_note(&p, 246.94f, 0.30f, 0.4f, 0.2f, 25);
    printf("[test_pitch] Whole-step bend 220 -> 246.94 Hz: tracked=%.2f Hz, midi=%u\n",
           p.frequency, p.midi_note);
    assert(fabsf(p.frequency - 246.94f) < 3.0f);
}

static void test_legato_vs_repick_and_palm_mute(void) {
    icarus_pitch_t p;
    icarus_pitch_init(&p);

    /* 1. Fresh note from silence -> onset with legato == 0 */
    int saw_initial_onset = 0;
    int initial_legato = -1;
    for (int b = 0; b < 50; b++) {
        feed_guitar_note(&p, 146.83f, 0.25f, 0.3f, 0.1f, 1);
        if (p.onset && !saw_initial_onset) {
            saw_initial_onset = 1;
            initial_legato = p.legato;
        }
    }
    assert(saw_initial_onset == 1);
    assert(initial_legato == 0);

    /* 2. Connected hammer-on to F#3 (185.00 Hz) without silence -> legato == 1 */
    int saw_legato_onset = 0;
    for (int b = 0; b < 50; b++) {
        feed_guitar_note(&p, 185.00f, 0.25f, 0.3f, 0.1f, 1);
        if (p.onset && p.legato == 1u) {
            saw_legato_onset = 1;
        }
    }
    printf("[test_pitch] Legato hammer-on 146.83 -> 185.00 Hz: tracked=%.2f Hz, saw_legato_onset=%d\n",
           p.frequency, saw_legato_onset);
    assert(saw_legato_onset == 1);
    assert(fabsf(p.frequency - 185.00f) < 4.0f);

    /* 3. High-frequency percussive scratch / noise burst -> must not cause wild pitch jump */
    float prev_f = p.frequency;
    float blk[PA_BLOCK_N];
    uint32_t rng = 0x12345678u;
    float prev_r = 0.0f;
    for (int b = 0; b < 20; b++) {
        for (int i = 0; i < PA_BLOCK_N; i++) {
            rng = rng * 1664525u + 1013904223u;
            float r = ((float)(int32_t)rng / 2147483648.0f) * 0.20f;
            blk[i] = r - prev_r; /* Differentiated high-frequency scrape noise */
            prev_r = r;
        }
        icarus_pitch_feed_block(&p, blk, PA_BLOCK_N);
        icarus_pitch_poll_background(&p);
    }
    printf("[test_pitch] After HF scrape noise: prev=%.2f Hz, after=%.2f Hz\n", prev_f, p.frequency);
    assert(fabsf(p.frequency - prev_f) < 5.0f);
}

/* Several strings at once: the tracker follows the ROOT (the lowest string), not the common period of the chord (a pitch nobody played) nor one of the upper strings.
 * The strings are Karplus-Strong plucks (a noise burst through an averaging delay line): like a real string they are not made of perfectly harmonic partials. */
typedef struct { float buf[1024]; int n, pos; float decay; } ks_t;
static uint32_t ks_rng = 12345u;
static float ks_noise(void) { ks_rng = ks_rng * 1664525u + 1013904223u; return (float)(int32_t)ks_rng * (1.0f / 2147483648.0f); }
static void ks_pluck(ks_t *k, float hz, float decay) {
    k->n = (int)((float)PA_SAMPLE_RATE / hz + 0.5f); k->pos = 0; k->decay = decay;
    float prev = 0.0f;
    for (int i = 0; i < k->n; i++) { prev = 0.5f * ks_noise() + 0.5f * prev; k->buf[i] = prev; }
}
static float ks_tick(ks_t *k) {
    const float y = k->buf[k->pos];
    const int nx = (k->pos + 1 >= k->n) ? 0 : k->pos + 1;
    k->buf[k->pos] = k->decay * 0.5f * (y + k->buf[nx]);
    k->pos = nx;
    return y;
}
static void feed_ks(icarus_pitch_t *p, ks_t *ks, const float *gain, int nn, int num_blocks) {
    float blk[PA_BLOCK_N];
    for (int b = 0; b < num_blocks; b++) {
        for (int i = 0; i < PA_BLOCK_N; i++) { float s = 0.0f; for (int k = 0; k < nn; k++) s += gain[k] * ks_tick(&ks[k]); blk[i] = s; }
        icarus_pitch_feed_block(p, blk, PA_BLOCK_N);
        icarus_pitch_poll_background(p);
    }
}

static void test_chords_follow_the_root(void) {
    static const struct { const char *name; float hz[3]; int n; } k_chords[] = {
        { "E2+B2 (power chord, a fifth)", { 82.41f, 123.47f, 0.0f }, 2 },
        { "E2+E3 (octave)",               { 82.41f, 164.81f, 0.0f }, 2 },
        { "A2+E3+A3 (A5: common period 55 Hz)", { 110.00f, 164.81f, 220.00f }, 3 },
    };
    for (size_t c = 0; c < sizeof(k_chords) / sizeof(k_chords[0]); c++) {
        icarus_pitch_t p;
        icarus_pitch_init(&p);
        ks_t ks[3];
        float gain[3] = { 1.0f, 1.0f, 1.0f };
        for (int k = 0; k < k_chords[c].n; k++) ks_pluck(&ks[k], k_chords[c].hz[k], 0.9985f);
        for (int k = 0; k < 3; k++) gain[k] = 0.25f;
        feed_ks(&p, ks, gain, k_chords[c].n, 200);
        const float err = 1200.0f * log2f(p.frequency / k_chords[c].hz[0]);
        printf("[test_pitch] chord %-40s root=%7.2f Hz tracked=%7.2f Hz (%+.0f cents, valid=%u)\n", k_chords[c].name, k_chords[c].hz[0], p.frequency, err, p.valid);
        assert(p.valid == 1u);
        assert(fabsf(err) < 40.0f);
    }
}

/* A string still ringing sympathetically under a picked note must not take the note over. */
static void test_sympathetic_string_ignored(void) {
    icarus_pitch_t p;
    icarus_pitch_init(&p);
    ks_t ks[2];
    ks_pluck(&ks[0], 164.81f, 0.9992f);                                  /* E3 rings ... */
    float g1[1] = { 0.25f };
    feed_ks(&p, ks, g1, 1, 100);
    icarus_pitch_reset(&p);                                               /* (the tracker has been listening to it; a fresh note out of the ringing level follows) */
    ks_pluck(&ks[1], 110.00f, 0.9985f);                                  /* ... then A2 is picked, 20 dB over it */
    float g2[2] = { 0.025f, 0.25f };
    feed_ks(&p, ks, g2, 2, 150);
    printf("[test_pitch] A2 picked over a ringing E3 (-20 dB): tracked=%.2f Hz valid=%u\n", p.frequency, p.valid);
    assert(p.valid == 1u);
    assert(fabsf(1200.0f * log2f(p.frequency / 110.0f)) < 40.0f);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_note_range();
    test_strong_harmonics_no_octave_jump();
    test_bends_and_vibrato();
    test_legato_vs_repick_and_palm_mute();
    test_chords_follow_the_root();
    test_sympathetic_string_ignored();
    printf("ALL PITCH TRACKER TESTS PASSED\n");
    return 0;
}
