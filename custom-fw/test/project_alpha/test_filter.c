/* custom-fw/test/project_alpha/test_filter.c — Unit & Stability Tests for Project Alpha Filter & Analog Engine */
#include <stdio.h>
#include <math.h>
#include "../../src/effects/project_alpha/alpha_filter.h"
#include "../../src/effects/project_alpha/alpha_analog.h"
#include "../../src/effects/project_alpha/alpha_mix.h"
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

static float measure_sine_rms(alpha_filter_t *f, float freq_hz) {
    alpha_filter_reset(f);
    double sum_sq = 0.0;
    const int n_settle = 2048;
    const int n_meas   = 4096;
    for (int i = 0; i < n_settle + n_meas; i++) {
        float x = 0.25f * sinf(2.0f * 3.14159265f * freq_hz * (float)i / 44100.0f);
        float y = alpha_filter_step(f, x);
        if (i >= n_settle) sum_sq += (double)y * (double)y;
    }
    return (float)sqrt(sum_sq / (double)n_meas);
}

int main(void) {
    printf("=== Project Alpha: Resonant Filter & Analog Engine Tests ===\n");

    /* 1. Low-pass attenuation check (LP12 vs LP24) */
    alpha_filter_t flt;
    alpha_filter_init(&flt);
    alpha_filter_set_params(&flt, 1000.0f, 0.1f, 0.0f, 0.0f, 0.0f, ALPHA_FILTER_LP12);

    float rms_pass_12 = measure_sine_rms(&flt, 200.0f);
    float rms_stop_12 = measure_sine_rms(&flt, 4000.0f);

    alpha_filter_set_params(&flt, 1000.0f, 0.1f, 0.0f, 0.0f, 0.0f, ALPHA_FILTER_LP24);
    float rms_pass_24 = measure_sine_rms(&flt, 200.0f);
    float rms_stop_24 = measure_sine_rms(&flt, 4000.0f);

    CHECK(rms_pass_12 > 0.14f && rms_stop_12 < rms_pass_12 * 0.22f,
          "LP12 passes 200 Hz (%.4f) and attenuates 4 kHz by >12 dB (%.4f)", rms_pass_12, rms_stop_12);
    CHECK(rms_pass_24 > 0.14f && rms_stop_24 < rms_stop_12 * 0.25f,
          "LP24 steeper slope attenuates 4 kHz further than LP12 (LP24=%.5f vs LP12=%.5f)", rms_stop_24, rms_stop_12);

    /* 2. High-resonance stability across full cutoff sweep (80 Hz -> 18 kHz) */
    int finite_ok = 1;
    float max_abs = 0.0f;
    alpha_filter_reset(&flt);
    for (int blk = 0; blk < 200; blk++) {
        float fc = 80.0f * powf(18000.0f / 80.0f, (float)blk / 199.0f);
        alpha_filter_set_params(&flt, fc, 1.0f, 0.8f, 0.5f, 0.5f, ALPHA_FILTER_LP24);
        float buf[64], env[64];
        for (int i = 0; i < 64; i++) {
            int t = blk * 64 + i;
            buf[i] = 0.8f * sinf(2.0f * 3.14159265f * 110.0f * (float)t / 44100.0f)
                   + 0.4f * cosf(2.0f * 3.14159265f * 880.0f * (float)t / 44100.0f);
            env[i] = 0.7f;
        }
        alpha_filter_process_block(&flt, buf, 64, env, 440.0f, 0.0f);
        for (int i = 0; i < 64; i++) {
            if (!isfinite(buf[i])) finite_ok = 0;
            float a = fabsf(buf[i]);
            if (a > max_abs) max_abs = a;
        }
    }
    CHECK(finite_ok && max_abs > 0.05f && max_abs <= 1.55f,
          "100%% resonance sweep 80 Hz..18 kHz stays stable and bounded (max |y| = %.3f)", max_abs);

    /* 3. Polyphonic chord processing through Analog Engine across all 5 distortion modes */
    alpha_analog_t ana;
    alpha_env_follower_t ef;
    alpha_analog_init(&ana);
    alpha_env_follower_init(&ef);

    for (uint8_t m = 0; m < ALPHA_DIST_MODE_COUNT; m++) {
        alpha_analog_reset(&ana);
        alpha_env_follower_reset(&ef);
        alpha_analog_set_distortion(&ana, m, 0.75f);
        double energy = 0.0, dc_sum = 0.0;
        for (int b = 0; b < 80; b++) {
            float raw[64], cond[64], env[64], out[64];
            for (int i = 0; i < 64; i++) {
                int t = b * 64 + i;
                /* 3-note guitar triad (A2 = 110 Hz, E3 = 164.81 Hz, C#4 = 277.18 Hz) + +0.15 DC offset */
                raw[i] = 0.15f
                       + 0.25f * sinf(2.0f * 3.14159265f * 110.0f * (float)t / 44100.0f + 0.1f)
                       + 0.22f * sinf(2.0f * 3.14159265f * 164.81f * (float)t / 44100.0f + 1.3f)
                       + 0.20f * sinf(2.0f * 3.14159265f * 277.18f * (float)t / 44100.0f + 2.7f);
            }
            alpha_input_condition_block(&ana, &ef, raw, cond, env, 64);
            alpha_analog_process_block(&ana, cond, env, out, 64, 220.0f, 0.0f);
            if (b >= 30) {
                for (int i = 0; i < 64; i++) {
                    energy += (double)out[i] * (double)out[i];
                    dc_sum += (double)out[i];
                }
            }
        }
        float rms = (float)sqrt(energy / (double)(50 * 64));
        float dc  = (float)fabs(dc_sum / (double)(50 * 64));
        CHECK(rms > 0.05f && rms < 1.2f && dc < 0.03f,
              "Analog distortion mode %u processes polyphonic chord cleanly (rms=%.3f, dc=%.4f)",
              (unsigned)m, rms, dc);
    }

    if (fails) {
        printf("FAILED (%d errors)\n", fails);
        return 1;
    }
    printf("ALL FILTER & ANALOG ENGINE TESTS PASSED\n");
    return 0;
}
