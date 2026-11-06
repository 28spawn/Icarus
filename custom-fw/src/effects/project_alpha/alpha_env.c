/* custom-fw/src/effects/project_alpha/alpha_env.c — Project Alpha Envelope Followers & ADSR Generator */
#include "alpha_env.h"
#include "pa_config.h"
#include <math.h>

#define ALPHA_FS ((float)PA_SAMPLE_RATE)

static float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* Compute 1-pole exponential coefficient for a time constant in ms */
static float ms_to_coeff(float ms) {
    if (ms <= 0.05f) return 1.0f;
    float samples = (ms * 0.001f) * ALPHA_FS;
    if (samples < 1.0f) return 1.0f;
    /* 1 - exp(-1 / samples) approximated accurately */
    float c = 1.0f - expf(-1.0f / samples);
    return clampf(c, 0.000005f, 1.0f);
}

void alpha_env_follower_init(alpha_env_follower_t *ef) {
    if (!ef) return;
    ef->level      = 0.0f;
    ef->fast_level = 0.0f;
    ef->slow_ref   = 0.0f;
    ef->holdoff    = 0u;
    ef->onset_trig = 0u;
    alpha_env_follower_set_times(ef, 2.5f, 65.0f);
}

void alpha_env_follower_set_times(alpha_env_follower_t *ef, float attack_ms, float release_ms) {
    if (!ef) return;
    ef->attack_ms  = clampf(attack_ms, 0.1f, 500.0f);
    ef->release_ms = clampf(release_ms, 5.0f, 2000.0f);
    ef->att_coeff  = ms_to_coeff(ef->attack_ms);
    ef->rel_coeff  = ms_to_coeff(ef->release_ms);
}

void alpha_env_follower_reset(alpha_env_follower_t *ef) {
    if (!ef) return;
    ef->level      = 0.0f;
    ef->fast_level = 0.0f;
    ef->slow_ref   = 0.0f;
    ef->holdoff    = 0u;
    ef->onset_trig = 0u;
}

float alpha_env_follower_step(alpha_env_follower_t *ef, float abs_in) {
    if (abs_in < 0.0f) abs_in = -abs_in;

    /* Fast transient tracker (~1.2 ms attack, ~15 ms release) vs slower baseline (~25 ms attack, ~45 ms release) */
    const float k_fast_att = 0.020f;
    const float k_fast_rel = 0.0015f;
    const float k_slow_att = 0.0010f;
    const float k_slow_rel = 0.0005f;
    if (abs_in > ef->fast_level) {
        ef->fast_level += k_fast_att * (abs_in - ef->fast_level);
    } else {
        ef->fast_level += k_fast_rel * (abs_in - ef->fast_level);
    }
    if (abs_in > ef->slow_ref) {
        ef->slow_ref += k_slow_att * (abs_in - ef->slow_ref);
    } else {
        ef->slow_ref += k_slow_rel * (abs_in - ef->slow_ref);
    }

    /* Detect repeated pick attack: when fast envelope surges well above slow reference */
    ef->onset_trig = 0u;
    if (ef->holdoff > 0u) {
        ef->holdoff--;
    } else if (ef->fast_level > 0.025f && ef->fast_level > ef->slow_ref * 1.45f + 0.015f) {
        ef->onset_trig = 1u;
        ef->slow_ref   = ef->fast_level;
        ef->holdoff    = 1102u; /* 25 ms holdoff */
    }

    /* Smooth musical envelope without pumping:
     * When input exceeds current level, track up with attack coefficient;
     * when input is below, decay smoothly toward abs_in with release coefficient. */
    if (abs_in > ef->level) {
        ef->level += ef->att_coeff * (abs_in - ef->level);
    } else {
        ef->level += ef->rel_coeff * (abs_in - ef->level);
    }
    if (ef->level < 1.0e-7f) ef->level = 0.0f;
    if (ef->level > 1.0f)    ef->level = 1.0f;
    return ef->level;
}

void alpha_env_init(alpha_env_t *env) {
    if (!env) return;
    env->level    = 0.0f;
    env->velocity = 1.0f;
    env->stage    = ALPHA_ENV_IDLE;
    env->gate     = 0u;
    alpha_env_set_params(env, 10.0f, 300.0f, 0.70f, 250.0f);
}

void alpha_env_set_params(alpha_env_t *env, float attack_ms, float decay_ms, float sustain_01, float release_ms) {
    if (!env) return;
    env->attack  = clampf(attack_ms, 0.0f, 2000.0f);
    env->decay   = clampf(decay_ms, 0.0f, 3000.0f);
    env->sustain = clampf(sustain_01, 0.0f, 1.0f);
    env->release = clampf(release_ms, 0.0f, 5000.0f);

    /* Linear-to-asymptotic attack step so 0 ms is ~0.5 ms click-free ramp */
    float att_samps = ((env->attack < 0.5f ? 0.5f : env->attack) * 0.001f) * ALPHA_FS;
    env->att_step  = 1.0f / att_samps;
    /* Decay and release reach ~99% completion in the specified time (tau = T / 4.6) */
    env->dec_coeff = ms_to_coeff((env->decay < 1.0f ? 1.0f : env->decay) * 0.22f);
    env->rel_coeff = ms_to_coeff((env->release < 1.0f ? 1.0f : env->release) * 0.22f);
}

void alpha_env_reset(alpha_env_t *env) {
    if (!env) return;
    env->level    = 0.0f;
    env->velocity = 1.0f;
    env->stage    = ALPHA_ENV_IDLE;
    env->gate     = 0u;
}

void alpha_env_trigger(alpha_env_t *env, float velocity) {
    if (!env) return;
    env->velocity = clampf(velocity, 0.15f, 1.0f);
    env->gate     = 1u;
    env->stage    = ALPHA_ENV_ATTACK;
    /* Note: env->level is preserved (legato retrigger from current level, zero click) */
}

void alpha_env_release(alpha_env_t *env) {
    if (!env) return;
    env->gate = 0u;
    if (env->stage != ALPHA_ENV_IDLE) {
        env->stage = ALPHA_ENV_RELEASE;
    }
}

float alpha_env_step(alpha_env_t *env) {
    switch (env->stage) {
        case ALPHA_ENV_IDLE:
            env->level = 0.0f;
            break;

        case ALPHA_ENV_ATTACK:
            /* Analog RC-like attack curve aiming slightly above peak (1.2x) for musical punch */
            env->level += env->att_step * (1.25f - env->level * 0.25f);
            if (env->level >= 1.0f) {
                env->level = 1.0f;
                env->stage = ALPHA_ENV_DECAY;
            }
            break;

        case ALPHA_ENV_DECAY:
            env->level += env->dec_coeff * (env->sustain - env->level);
            if (fabsf(env->level - env->sustain) < 0.0005f) {
                env->level = env->sustain;
                env->stage = ALPHA_ENV_SUSTAIN;
            }
            break;

        case ALPHA_ENV_SUSTAIN:
            env->level += 0.005f * (env->sustain - env->level);
            break;

        case ALPHA_ENV_RELEASE:
            env->level -= env->rel_coeff * env->level;
            if (env->level < 0.0002f) {
                env->level = 0.0f;
                env->stage = ALPHA_ENV_IDLE;
            }
            break;

        default:
            env->stage = ALPHA_ENV_IDLE;
            env->level = 0.0f;
            break;
    }
    return env->level * env->velocity;
}
