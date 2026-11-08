/* custom-fw/test/icarus/test_envelope.c — Unit & QA Tests for Icarus Synth Envelopes */
#include <stdio.h>
#include <math.h>
#include "../../src/effects/icarus/icarus_env.h"
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

int main(void) {
    printf("=== Icarus Synth: Envelope Follower & ADSR Tests ===\n");

    /* 1. Envelope Follower response and repeated-note articulation */
    icarus_env_follower_t ef;
    icarus_env_follower_init(&ef);
    icarus_env_follower_set_times(&ef, 2.0f, 80.0f);

    float peak_env = 0.0f;
    for (int i = 0; i < 4410; i++) { /* 100 ms of 0.5 amplitude 220 Hz sine */
        float x = 0.5f * sinf(2.0f * 3.14159265f * 220.0f * (float)i / 44100.0f);
        float e = icarus_env_follower_step(&ef, fabsf(x));
        if (e > peak_env) peak_env = e;
    }
    CHECK(peak_env > 0.35f && peak_env <= 0.52f,
          "envelope follower tracks 0.5 amplitude sine (peak %.3f)", peak_env);

    /* Let it decay for 150 ms, then hit a repeated note and check onset_trig & non-pumping */
    for (int i = 0; i < 6615; i++) {
        icarus_env_follower_step(&ef, 0.02f);
    }
    float decayed = ef.level;
    CHECK(decayed < peak_env * 0.4f && decayed > 0.01f,
          "envelope follower decays smoothly over 150 ms (level %.3f)", decayed);

    int onset_seen = 0;
    for (int i = 0; i < 441; i++) {
        icarus_env_follower_step(&ef, 0.7f);
        if (ef.onset_trig) onset_seen = 1;
    }
    CHECK(onset_seen == 1, "repeated note transient triggers onset re-articulation flag");

    /* 2. ADSR Envelope Generator stages: Attack -> Decay -> Sustain -> Release -> Idle */
    icarus_env_t adsr;
    icarus_env_init(&adsr);
    icarus_env_set_params(&adsr, 10.0f, 50.0f, 0.60f, 80.0f);

    CHECK(adsr.stage == ICARUS_ENV_IDLE && adsr.level == 0.0f, "ADSR starts in IDLE at 0.0");

    icarus_env_trigger(&adsr, 1.0f);
    CHECK(adsr.stage == ICARUS_ENV_ATTACK, "trigger enters ATTACK stage");

    int samples_to_decay = 0;
    while (adsr.stage == ICARUS_ENV_ATTACK && samples_to_decay < 44100) {
        icarus_env_step(&adsr);
        samples_to_decay++;
    }
    CHECK(adsr.stage == ICARUS_ENV_DECAY && samples_to_decay > 200 && samples_to_decay < 800,
          "10 ms attack reaches peak 1.0 and transitions to DECAY in %d samples", samples_to_decay);

    for (int i = 0; i < 15000; i++) {
        icarus_env_step(&adsr);
    }
    CHECK(adsr.stage == ICARUS_ENV_SUSTAIN && fabsf(adsr.level - 0.60f) < 0.01f,
          "decay settles into SUSTAIN at target 0.60 (got %.3f)", adsr.level);

    icarus_env_release(&adsr);
    CHECK(adsr.stage == ICARUS_ENV_RELEASE, "release enters RELEASE stage");

    for (int i = 0; i < 30000; i++) {
        icarus_env_step(&adsr);
    }
    CHECK(adsr.stage == ICARUS_ENV_IDLE && adsr.level == 0.0f,
          "release reaches 0.0 and returns to IDLE (got %.5f)", adsr.level);

    if (fails) {
        printf("FAILED (%d errors)\n", fails);
        return 1;
    }
    printf("ALL ENVELOPE TESTS PASSED\n");
    return 0;
}
