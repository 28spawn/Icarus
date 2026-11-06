/* custom-fw/src/effects/project_alpha/alpha_env.h — Project Alpha Envelope Followers & ADSR Generator */
#ifndef ALPHA_ENV_H
#define ALPHA_ENV_H

#include <stdint.h>

typedef enum {
    ALPHA_ENV_IDLE    = 0,
    ALPHA_ENV_ATTACK  = 1,
    ALPHA_ENV_DECAY   = 2,
    ALPHA_ENV_SUSTAIN = 3,
    ALPHA_ENV_RELEASE = 4
} alpha_env_stage_t;

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
} alpha_env_follower_t;

/* Dedicated ADSR filter/modulation envelope generator */
typedef struct {
    float attack;       /* 0 .. 2000 ms */
    float decay;        /* 0 .. 3000 ms */
    float sustain;      /* 0.0 .. 1.0 (0..100%) */
    float release;      /* 0 .. 5000 ms */
    float att_step;
    float dec_coeff;
    float rel_coeff;
    float level;        /* Current envelope output 0.0 .. 1.0 */
    float velocity;     /* Picking strength scaling 0.2 .. 1.0 */
    uint8_t stage;      /* alpha_env_stage_t */
    uint8_t gate;       /* Current gate state */
} alpha_env_t;

void  alpha_env_follower_init(alpha_env_follower_t *ef);
void  alpha_env_follower_set_times(alpha_env_follower_t *ef, float attack_ms, float release_ms);
void  alpha_env_follower_reset(alpha_env_follower_t *ef);
float alpha_env_follower_step(alpha_env_follower_t *ef, float abs_in);

void  alpha_env_init(alpha_env_t *env);
void  alpha_env_set_params(alpha_env_t *env, float attack_ms, float decay_ms, float sustain_01, float release_ms);
void  alpha_env_reset(alpha_env_t *env);
void  alpha_env_trigger(alpha_env_t *env, float velocity);
void  alpha_env_release(alpha_env_t *env);
float alpha_env_step(alpha_env_t *env);

#endif /* ALPHA_ENV_H */
