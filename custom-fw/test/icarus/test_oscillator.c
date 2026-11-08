/* custom-fw/test/icarus/test_oscillator.c — Unit & Audio Quality Tests for Icarus Synth Oscillators */
#include <stdio.h>
#include <math.h>
#include "../../src/effects/icarus/icarus_osc.h"
#include "pa_config.h"

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
        fails++; \
    } else { \
        printf("  ok   "); printf(__VA_ARGS__); printf("\n"); \
    } \
} while (0)

/* Compute single-bin DFT power at target_hz over n samples */
static double dft_bin_power(const float *buf, int n, float target_hz) {
    double re = 0.0, im = 0.0;
    double w = 2.0 * 3.14159265358979323846 * (double)target_hz / 44100.0;
    for (int i = 0; i < n; i++) {
        /* Hann window to suppress spectral leakage */
        double win = 0.5 * (1.0 - cos(2.0 * 3.14159265358979323846 * (double)i / (double)(n - 1)));
        double s = (double)buf[i] * win;
        re += s * cos(w * (double)i);
        im -= s * sin(w * (double)i);
    }
    return (re * re + im * im) / ((double)n * (double)n);
}

int main(void) {
    printf("=== Icarus Synth: Band-Limited Oscillator Engine Tests ===\n");

    /* 1. Waveform generation, RMS, and zero DC offset across Saw, Square, Triangle */
    icarus_osc_t osc;
    for (uint8_t w = 0; w < ICARUS_WAVE_COUNT; w++) {
        icarus_osc_init(&osc);
        osc.waveform    = w;
        osc.level       = 1.0f;
        osc.pulse_width = (w == ICARUS_WAVE_SQUARE) ? 0.25f : 0.50f;
        icarus_osc_set_freq(&osc, 440.0f, 0.0f);

        double sum = 0.0, sum_sq = 0.0;
        const int N = 44100;
        for (int i = 0; i < N; i++) {
            float y = icarus_osc_step(&osc, 0.0f);
            if (i >= 4410) {
                sum    += (double)y;
                sum_sq += (double)y * (double)y;
            }
        }
        int cnt = N - 4410;
        float rms = (float)sqrt(sum_sq / (double)cnt);
        float dc  = (float)fabs(sum / (double)cnt);
        CHECK(rms > 0.25f && rms < 1.05f && dc < 0.025f,
              "waveform %u @ 440 Hz produces healthy signal with near-zero DC (rms=%.3f, dc=%.4f)",
              (unsigned)w, rms, dc);
    }

    /* 2. PolyBLEP vs Naive Aliasing Suppression at high guitar-synth pitch (f0 = 1396.91 Hz = F6)
     * 16th harmonic is at 22,350.6 Hz, which aliases to 44100 - 22350.6 = 21,749.4 Hz,
     * and 31st harmonic (43,304.2 Hz) aliases right down to 795.8 Hz (below the fundamental!). */
    const int N_SPEC = 8192;
    static float blep_buf[8192];
    static float naive_buf[8192];
    float f0 = 1396.91f;
    icarus_osc_init(&osc);
    osc.waveform = ICARUS_WAVE_SAW;
    osc.level    = 1.0f;
    icarus_osc_set_freq(&osc, f0, 0.0f);

    float ph = 0.0f, dt = f0 / 44100.0f;
    for (int i = 0; i < N_SPEC; i++) {
        blep_buf[i]  = icarus_osc_step(&osc, 0.0f);
        naive_buf[i] = 2.0f * ph - 1.0f;
        ph += dt;
        if (ph >= 1.0f) ph -= 1.0f;
    }
    double p_fund_blep  = dft_bin_power(blep_buf,  N_SPEC, f0);
    double p_alias_blep = dft_bin_power(blep_buf,  N_SPEC, 44100.0f - 31.0f * f0); /* ~795.8 Hz sub-fundamental alias */
    double p_alias_naiv = dft_bin_power(naive_buf, N_SPEC, 44100.0f - 31.0f * f0);

    double alias_db_blep = 10.0 * log10((p_alias_blep + 1e-18) / p_fund_blep);
    double alias_db_naiv = 10.0 * log10((p_alias_naiv + 1e-18) / p_fund_blep);

    CHECK(alias_db_blep < alias_db_naiv - 15.0 && alias_db_blep < -45.0,
          "PolyBLEP saw suppresses sub-fundamental alias by >15 dB over naive (PolyBLEP=%.1f dBc vs Naive=%.1f dBc)",
          alias_db_blep, alias_db_naiv);

    /* 3. Phase continuity during pitch transitions and click-free gate transitions */
    icarus_osc_engine_t eng;
    icarus_osc_engine_init(&eng);
    eng.osc_a.waveform = ICARUS_WAVE_TRIANGLE;
    eng.osc_b.waveform = ICARUS_WAVE_TRIANGLE;
    eng.sub_mode       = ICARUS_SUB_OFF;

    float prev_sample = 0.0f;
    float max_step_jump = 0.0f;
    for (int blk = 0; blk < 30; blk++) {
        float out[64];
        /* Toggle gate on at blk 2, jump pitch from 220 Hz to 330 Hz at blk 12, gate off at blk 22 */
        int gate = (blk >= 2 && blk < 22) ? 1 : 0;
        float freq = (blk < 12) ? 220.0f : 330.0f;
        icarus_osc_engine_process_block(&eng, freq, gate, 0.0f, 0.0f, 0.0f, out, 64);
        for (int i = 0; i < 64; i++) {
            float jump = fabsf(out[i] - prev_sample);
            if (jump > max_step_jump) max_step_jump = jump;
            prev_sample = out[i];
        }
    }
    CHECK(max_step_jump < 0.15f,
          "oscillator engine maintains phase continuity and click-free gating (max sample-to-sample step = %.4f)",
          max_step_jump);

    if (fails) {
        printf("FAILED (%d errors)\n", fails);
        return 1;
    }
    printf("ALL OSCILLATOR TESTS PASSED\n");
    return 0;
}
