/* custom-fw/src/effects/icarus/icarus_env.c — Icarus Envelope Followers & ADSR Generator */
#include "icarus_env.h"
#include "pa_config.h"
#include <math.h>
#include "icarus_fastmath.h"

#define ICARUS_FS ((float)PA_SAMPLE_RATE)

static float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* Compute 1-pole exponential coefficient for a time constant in ms */
static float ms_to_coeff(float ms) {
    if (ms <= 0.05f) return 1.0f;
    float samples = (ms * 0.001f) * ICARUS_FS;
    if (samples < 1.0f) return 1.0f;
    /* 1 - exp(-1 / samples) approximated accurately */
    float c = 1.0f - expf(-1.0f / samples);
    return clampf(c, 0.000005f, 1.0f);
}

void icarus_env_follower_init(icarus_env_follower_t *ef) {
    if (!ef) return;
    ef->level      = 0.0f;
    ef->fast_level = 0.0f;
    ef->slow_ref   = 0.0f;
    ef->holdoff    = 0u;
    ef->onset_trig = 0u;
    icarus_env_follower_set_times(ef, 2.5f, 65.0f);
}

void icarus_env_follower_set_times(icarus_env_follower_t *ef, float attack_ms, float release_ms) {
    if (!ef) return;
    ef->attack_ms  = clampf(attack_ms, 0.1f, 500.0f);
    ef->release_ms = clampf(release_ms, 5.0f, 2000.0f);
    ef->att_coeff  = ms_to_coeff(ef->attack_ms);
    ef->rel_coeff  = ms_to_coeff(ef->release_ms);
}

void icarus_env_follower_reset(icarus_env_follower_t *ef) {
    if (!ef) return;
    ef->level      = 0.0f;
    ef->fast_level = 0.0f;
    ef->slow_ref   = 0.0f;
    ef->holdoff    = 0u;
    ef->onset_trig = 0u;
}

float icarus_env_follower_step(icarus_env_follower_t *ef, float abs_in) {
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
    } else if (ef->fast_level > 0.0055f && ef->fast_level > ef->slow_ref * 1.40f + 0.003f) {
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

void icarus_env_init(icarus_env_t *env) {
    if (!env) return;
    env->level    = 0.0f;
    env->velocity = 1.0f;
    env->stage    = ICARUS_ENV_IDLE;
    env->gate     = 0u;
    icarus_env_set_params(env, 10.0f, 300.0f, 0.70f, 250.0f);
}

void icarus_env_set_params(icarus_env_t *env, float attack_ms, float decay_ms, float sustain_01, float release_ms) {
    if (!env) return;
    env->attack  = clampf(attack_ms, 0.0f, 2000.0f);
    env->decay   = clampf(decay_ms, 0.0f, 3000.0f);
    env->sustain = clampf(sustain_01, 0.0f, 1.0f);
    env->release = clampf(release_ms, 0.0f, 5000.0f);

    /* Linear-to-asymptotic attack step so 0 ms is ~0.5 ms click-free ramp */
    float att_samps = ((env->attack < 0.5f ? 0.5f : env->attack) * 0.001f) * ICARUS_FS;
    env->att_step  = 1.0f / att_samps;
    /* Decay and release reach ~99% completion in the specified time (tau = T / 4.6) */
    env->dec_coeff = ms_to_coeff((env->decay < 1.0f ? 1.0f : env->decay) * 0.22f);
    env->rel_coeff = ms_to_coeff((env->release < 1.0f ? 1.0f : env->release) * 0.22f);
    env->att_step8 = env->att_step * 8.0f;
    env->dec8      = 1.0f - powf(1.0f - env->dec_coeff, 8.0f);
    env->rel8      = 1.0f - powf(1.0f - env->rel_coeff, 8.0f);
    env->sus8      = 1.0f - powf(1.0f - 0.005f, 8.0f);
}

void icarus_env_reset(icarus_env_t *env) {
    if (!env) return;
    env->level    = 0.0f;
    env->velocity = 1.0f;
    env->stage    = ICARUS_ENV_IDLE;
    env->gate     = 0u;
}

void icarus_env_trigger(icarus_env_t *env, float velocity) {
    if (!env) return;
    env->velocity = clampf(velocity, 0.15f, 1.0f);
    env->gate     = 1u;
    env->stage    = ICARUS_ENV_ATTACK;
    /* Note: env->level is preserved (legato retrigger from current level, zero click) */
}

void icarus_env_release(icarus_env_t *env) {
    if (!env) return;
    env->gate = 0u;
    if (env->stage != ICARUS_ENV_IDLE) {
        env->stage = ICARUS_ENV_RELEASE;
    }
}

float icarus_env_step(icarus_env_t *env) {
    switch (env->stage) {
        case ICARUS_ENV_IDLE:
            env->level = 0.0f;
            break;

        case ICARUS_ENV_ATTACK:
            /* Analog RC-like attack curve aiming slightly above peak (1.2x) for musical punch */
            env->level += env->att_step * (1.25f - env->level * 0.25f);
            if (env->level >= 1.0f) {
                env->level = 1.0f;
                env->stage = ICARUS_ENV_DECAY;
            }
            break;

        case ICARUS_ENV_DECAY:
            env->level += env->dec_coeff * (env->sustain - env->level);
            if (fabsf(env->level - env->sustain) < 0.0005f) {
                env->level = env->sustain;
                env->stage = ICARUS_ENV_SUSTAIN;
            }
            break;

        case ICARUS_ENV_SUSTAIN:
            env->level += 0.005f * (env->sustain - env->level);
            break;

        case ICARUS_ENV_RELEASE:
            env->level -= env->rel_coeff * env->level;
            if (env->level < 0.0002f) {
                env->level = 0.0f;
                env->stage = ICARUS_ENV_IDLE;
            }
            break;

        default:
            env->stage = ICARUS_ENV_IDLE;
            env->level = 0.0f;
            break;
    }
    return env->level * env->velocity;
}

void icarus_env_follower_block(icarus_env_follower_t *ef, const float *in, float *out, int n) {
    if (!ef || !in || !out) return;
    float fast = ef->fast_level, slow = ef->slow_ref, lvl = ef->level;
    const float att = ef->att_coeff, rel = ef->rel_coeff;
    uint16_t hold = ef->holdoff;
    uint8_t trig_any = 0u;      /* ANY onset in the block: the flag used to hold only the LAST sample's result, so a pick was seen about 1 time in 64 (the ADSR of the ANALOG presets almost never triggered) */
    for (int i = 0; i < n; i++) {
        const float a = (in[i] < 0.0f) ? -in[i] : in[i];
        fast += ((a > fast) ? 0.020f : 0.0015f) * (a - fast);
        slow += ((a > slow) ? 0.0010f : 0.0005f) * (a - slow);
        if (hold > 0u) {
            hold--;
        } else if (fast > 0.0055f && fast > slow * 1.40f + 0.003f) {
            slow = fast;
            hold = 1102u;                                   /* 25 ms holdoff */
            trig_any = 1u;
        }
        lvl += ((a > lvl) ? att : rel) * (a - lvl);
        if (lvl < 1.0e-7f) lvl = 0.0f;
        if (lvl > 1.0f)    lvl = 1.0f;
        out[i] = lvl;
    }
    if (fast < 1.0e-20f) fast = 0.0f;                         /* no denormals in the trackers after the note dies away */
    if (slow < 1.0e-20f) slow = 0.0f;
    ef->fast_level = fast; ef->slow_ref = slow; ef->level = lvl; ef->holdoff = hold; ef->onset_trig = trig_any;
}

float icarus_env_step8(icarus_env_t *env) {
    switch (env->stage) {
        case ICARUS_ENV_IDLE:
            env->level = 0.0f;
            break;
        case ICARUS_ENV_ATTACK:
            env->level += env->att_step8 * (1.25f - env->level * 0.25f);
            if (env->level >= 1.0f) {
                env->level = 1.0f;
                env->stage = ICARUS_ENV_DECAY;
            }
            break;
        case ICARUS_ENV_DECAY:
            env->level += env->dec8 * (env->sustain - env->level);
            if (fabsf(env->level - env->sustain) < 0.0005f) {
                env->level = env->sustain;
                env->stage = ICARUS_ENV_SUSTAIN;
            }
            break;
        case ICARUS_ENV_SUSTAIN:
            env->level += env->sus8 * (env->sustain - env->level);
            break;
        case ICARUS_ENV_RELEASE:
            env->level -= env->rel8 * env->level;
            if (env->level < 0.0002f) {
                env->level = 0.0f;
                env->stage = ICARUS_ENV_IDLE;
            }
            break;
        default:
            env->stage = ICARUS_ENV_IDLE;
            env->level = 0.0f;
            break;
    }
    return env->level * env->velocity;
}
