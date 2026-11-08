/* custom-fw/src/effects/icarus/icarus_env.h — Icarus Envelope Followers & ADSR Generator */
#ifndef ICARUS_ENV_H
#define ICARUS_ENV_H

#include <stdint.h>

typedef enum {
    ICARUS_ENV_IDLE    = 0,
    ICARUS_ENV_ATTACK  = 1,
    ICARUS_ENV_DECAY   = 2,
    ICARUS_ENV_SUSTAIN = 3,
    ICARUS_ENV_RELEASE = 4
} icarus_env_stage_t;

/* Input envelope follower: independent attack and release constants,
 * responds naturally to repeated notes without unwanted pumping. */
typedef struct {
    float attack_ms;
    float release_ms;
    float att_coeff;
    float rel_coeff;
    float level;
    float fast_level;   /* Fast transient tracker for re-articulation detection */
    float slow_ref;     /* Slower reference tracker for pick transient ratio */
    uint16_t holdoff;   /* Minimum samples between onset triggers (~25 ms) */
    uint8_t onset_trig; /* 1 when a picking transient re-articulates */
} icarus_env_follower_t;

/* Dedicated ADSR filter/modulation envelope generator */
typedef struct {
    float attack;       /* 0 .. 2000 ms */
    float decay;        /* 0 .. 3000 ms */
    float sustain;      /* 0.0 .. 1.0 (0..100%) */
    float release;      /* 0 .. 5000 ms */
    float att_step;
    float dec_coeff;
    float rel_coeff;
    float att_step8, dec8, rel8, sus8;   /* the same, for stepping 8 samples at a time (icarus_env_step8) */
    float level;        /* Current envelope output 0.0 .. 1.0 */
    float velocity;     /* Picking strength scaling 0.2 .. 1.0 */
    uint8_t stage;      /* icarus_env_stage_t */
    uint8_t gate;       /* Current gate state */
} icarus_env_t;

void  icarus_env_follower_init(icarus_env_follower_t *ef);
void  icarus_env_follower_set_times(icarus_env_follower_t *ef, float attack_ms, float release_ms);
void  icarus_env_follower_reset(icarus_env_follower_t *ef);
float icarus_env_follower_step(icarus_env_follower_t *ef, float abs_in);
/* The follower over a whole block, state in locals: out[i] = level after |in[i]|. The picking-onset flag ends up as the LAST sample's, as it did
 * when this was called once per sample (icarus.c reads it once per block). */
void  icarus_env_follower_block(icarus_env_follower_t *ef, const float *in, float *out, int n);

void  icarus_env_init(icarus_env_t *env);
void  icarus_env_set_params(icarus_env_t *env, float attack_ms, float decay_ms, float sustain_01, float release_ms);
void  icarus_env_reset(icarus_env_t *env);
void  icarus_env_trigger(icarus_env_t *env, float velocity);
void  icarus_env_release(icarus_env_t *env);
float icarus_env_step(icarus_env_t *env);
/* Advance the envelope by 8 samples in one step (first-order, with the 8-sample coefficients): the synth only reads it at 5.5 kHz. */
float icarus_env_step8(icarus_env_t *env);

#endif /* ICARUS_ENV_H */
