/* custom-fw/src/effects/icarus/icarus.c — Icarus Flagship Guitar Synthesis System */
#include "icarus.h"
#include "pa_config.h"
#include "../../dsp/fixedpoint.h"
#include <math.h>
#include "icarus_fastmath.h"
#include <string.h>

extern void *pa_alloc(uint32_t);
extern void  pa_free(void *);

#define ICARUS_FS ((float)PA_SAMPLE_RATE)

static icarus_state_t *s_icarus_active = 0;

/* Set by the audio interrupt (audio_alnk.c) when too many blocks in a row of windows ran past their budget while the synth was on:
 * the synth drops to a dry pass-through so the main loop (UI, USB, Bluetooth) gets its time back instead of the unit freezing.
 * Latched until the synth is switched off and on again. */
volatile uint8_t g_pa_synth_shed = 0;

/* STAGE TIMING. On the unit the synth reports where its block time goes into the three effect-slot timing entries the removed MOD / DLY / REV
 * effects left unused (SRAM 0x01C7C500 + 16 * slot: max, sum / 16, calls, last, in TIMER4 ticks; read by tools/jl_ram_read.py slots, which
 * labels them): slot 5 = the front (gate, input conditioning, pitch feed, envelopes, modulation frame), slot 6 = the engines (analog stage
 * and oscillators + filter), slot 7 = the back (mixer, output gate, meters, scope). On the host, -DICARUS_PROFILE gives ten finer stages
 * through icarus_prof_begin / icarus_prof_end (supplied by the profiling harness). */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
extern uint32_t plat_ticks24(void);
#define AP_DEV_BEGIN(t)       uint32_t t = plat_ticks24()
#define AP_DEV_RESTART(t)     (t) = plat_ticks24()
#define AP_DEV_END(t, slot)   do { \
        volatile uint32_t *w_ = (volatile uint32_t *)(uintptr_t)(0x01C7C500u + 16u * (uint32_t)(slot)); \
        const uint32_t d_ = plat_ticks24() - (t); \
        if (d_ > w_[0]) w_[0] = d_; \
        w_[1] += d_ >> 4; w_[2]++; w_[3] = d_; if (w_[1] >= 0xC0000000u) { w_[1] >>= 1; w_[2] >>= 1; } } while (0)
#else
#define AP_DEV_BEGIN(t)       (void)0
#define AP_DEV_RESTART(t)     (void)0
#define AP_DEV_END(t, slot)   (void)0
#endif
#if defined(ICARUS_PROFILE)
extern void icarus_prof_begin(int id);
extern void icarus_prof_end(int id);
#define AP_BEGIN(id) icarus_prof_begin(id)
#define AP_END(id)   icarus_prof_end(id)
#else
#define AP_BEGIN(id) ((void)0)
#define AP_END(id)   ((void)0)
#endif

static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* Input sanitiser: a guitar into an ADC never carries a NaN or an Inf, but one such sample in a filter or oscillator state stays there for ever
 * (every later sample is NaN) and a NaN frequency reaching logf / floorf in the main loop is a hang risk on the unit. +-4 is 12 dB over full scale. */
static inline float icarus_san(float x) {
    return (x > 4.0f) ? 4.0f : ((x < -4.0f) ? -4.0f : ((x == x) ? x : 0.0f));
}

/* Cheap 2^x and log2(x) for the per-block telemetry (display cutoff, cents): a libm powf/log2f is thousands of cycles in software
 * float, and these ran every audio block. Accuracy ~1e-4 octave, far finer than the screen shows. */
static inline float fast_exp2f_a(float x) {
    if (x < -20.0f) x = -20.0f;
    if (x >  20.0f) x =  20.0f;
    const int32_t i = (int32_t)floorf(x);
    const float   f = x - (float)i;
    const float   p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * 0.00961813f)));
    union { uint32_t u; float f; } sc;
    sc.u = (uint32_t)((i + 127) << 23);
    return sc.f * p;
}

/* ICARUS_CP(n): the audio interrupt's position inside the synth (SRAM 0x01C7C9B0): after a reset that left no fault record, the last stage reached is the answer. */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
#define ICARUS_CP(n) ((void)(*(volatile uint32_t *)0x01C7C9B0u = (uint32_t)(n)))
#else
#define ICARUS_CP(n) ((void)0)
#endif

static inline float fast_log2f_a(float x) {
    if (x < 1.0e-6f) x = 1.0e-6f;
    union { float f; uint32_t u; } v;
    v.f = x;
    const int32_t e = (int32_t)((v.u >> 23) & 0xFFu) - 127;
    v.u = (v.u & 0x7FFFFFu) | (127u << 23);
    const float m = v.f - 1.0f;
    return (float)e + m * (1.4425449f - m * (0.7181452f - m * 0.2756003f));   /* the three terms sum to 1: continuous across octaves */
}

static inline float db_to_lin(float db) {
    return powf(10.0f, db * 0.05f);
}

static void icarus_gate_init(icarus_gate_t *g, float thresh_db, float att_ms, float hold_ms, float rel_ms, float hyst_db) {
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->threshold_db  = thresh_db;
    g->attack_ms     = att_ms;
    g->hold_ms       = hold_ms;
    g->release_ms    = rel_ms;
    g->hysteresis_db = hyst_db;
    g->open_lin      = db_to_lin(thresh_db);
    g->close_lin     = db_to_lin(thresh_db - hyst_db);
    g->att_coeff     = 1.0f - expf(-1000.0f / (clampf(att_ms, 0.2f, 100.0f) * ICARUS_FS));
    g->rel_coeff     = 1.0f - expf(-1000.0f / (clampf(rel_ms, 5.0f, 2000.0f) * ICARUS_FS));
    g->hold_samples  = (uint32_t)(clampf(hold_ms, 0.0f, 1000.0f) * 0.001f * ICARUS_FS);
    g->env_att_blk   = 1.0f - powf(1.0f - 0.05f,  (float)PA_BLOCK_N);
    g->env_rel_blk   = 1.0f - powf(1.0f - 0.002f, (float)PA_BLOCK_N);
    g->att_blk       = 1.0f - powf(1.0f - g->att_coeff, (float)PA_BLOCK_N);
    g->rel_blk       = 1.0f - powf(1.0f - g->rel_coeff, (float)PA_BLOCK_N);
}

/* The noise gate, once per BLOCK (it used to run per sample: a function call with ~8 struct read-modify-writes at 20 ns each, 64 times a block).
 * `mean_abs` is the block's mean rectified level (what the per-sample envelope tracked); the envelope and gain smoothers use their whole-block
 * coefficients; the caller ramps the gain linearly from its old value to the one returned. The hold counter counts blocks' worth of samples. */
static inline float icarus_gate_block(icarus_gate_t *g, float mean_abs, int n) {
    g->env += ((mean_abs > g->env) ? g->env_att_blk : g->env_rel_blk) * (mean_abs - g->env);
    if (g->env < 1.0e-20f) g->env = 0.0f;                                  /* no denormals in the gate's envelope during long silences */
    if (!g->open) {
        if (g->env >= g->open_lin) {
            g->open     = 1u;
            g->hold_cnt = g->hold_samples;
        }
    } else {
        if (g->env >= g->close_lin) {
            g->hold_cnt = g->hold_samples;
        } else if (g->hold_cnt > (uint32_t)n) {
            g->hold_cnt -= (uint32_t)n;
        } else {
            g->hold_cnt = 0u;
            g->open     = 0u;
        }
    }
    const float target = g->open ? 1.0f : 0.0f;
    g->gain += (g->open ? g->att_blk : g->rel_blk) * (target - g->gain);
    return g->gain;
}

/* ============================================================================
 * 12 Factory Presets (Section 13 — Stored in XIP Flash .rodata)
 * ============================================================================ */
static const icarus_preset_t k_factory_presets[ICARUS_FACTORY_PRESET_COUNT] = {
    /* 1. Metheny Lead — Oscillator: Bright analog GR-300 style lead with legato glide & delayed vibrato */
    {
        .name            = "Metheny Lead",
        .mode            = ICARUS_MODE_OSCILLATOR,
        .dist_mode       = ICARUS_DIST_HEXA,
        .analog_drive    = 0.45f,
        .input_gain      = 1.15f,
        .pre_emphasis    = 1u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 4,
        .level_a         = 0.90f,
        .level_b         = 0.55f,
        .detune_cents    = 7.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ICARUS_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.35f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 2100.0f,
        .resonance       = 0.38f,
        .env_amount      = 0.52f,
        .key_tracking    = 0.75f,
        .filter_sat      = 0.35f,
        .env_attack_ms   = 8.0f,
        .env_decay_ms    = 420.0f,
        .env_sustain     = 0.68f,
        .env_release_ms  = 240.0f,
        .glide_ms        = 55.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.6f,
        .vib_depth_cents = 16.0f,
        .vib_delay_ms    = 260.0f,
        .lfo_rate_hz     = 1.2f,
        .lfo_depth       = 0.08f,
        .lfo_wave        = ICARUS_LFO_SINE,
        .lfo_dest        = ICARUS_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.12f,
        .analog_level    = 0.20f,
        .synth_level     = 0.92f,
        .master_level    = 0.85f,
        .stereo_width    = 0.35f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 2. GR Brass — Hybrid: Dual sawtooth + fifth/detune with resonant filter envelope sweep */
    {
        .name            = "GR Brass",
        .mode            = ICARUS_MODE_HYBRID,
        .dist_mode       = ICARUS_DIST_HEXA,
        .analog_drive    = 0.55f,
        .input_gain      = 1.10f,
        .pre_emphasis    = 1u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 0,
        .level_a         = 0.85f,
        .level_b         = 0.80f,
        .detune_cents    = 14.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ICARUS_SUB_OCT1,
        .sub_level       = 0.22f,
        .osc_mix         = 0.48f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 680.0f,
        .resonance       = 0.44f,
        .env_amount      = 0.74f,
        .key_tracking    = 0.65f,
        .filter_sat      = 0.30f,
        .env_attack_ms   = 38.0f,
        .env_decay_ms    = 360.0f,
        .env_sustain     = 0.55f,
        .env_release_ms  = 280.0f,
        .glide_ms        = 35.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.2f,
        .vib_depth_cents = 12.0f,
        .vib_delay_ms    = 320.0f,
        .lfo_rate_hz     = 0.8f,
        .lfo_depth       = 0.10f,
        .lfo_wave        = ICARUS_LFO_TRI,
        .lfo_dest        = ICARUS_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.15f,
        .analog_level    = 0.55f,
        .synth_level     = 0.85f,
        .master_level    = 0.85f,
        .stereo_width    = 0.45f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 3. Vintage Solo — Oscillator: Warm detuned oscillator solo voice */
    {
        .name            = "Vintage Solo",
        .mode            = ICARUS_MODE_OSCILLATOR,
        .dist_mode       = ICARUS_DIST_SOFT_CLIP,
        .analog_drive    = 0.35f,
        .input_gain      = 1.05f,
        .pre_emphasis    = 0u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = -3,
        .level_a         = 0.85f,
        .level_b         = 0.65f,
        .detune_cents    = 11.0f,
        .pulse_width     = 0.42f,
        .sub_mode        = ICARUS_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.42f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 1450.0f,
        .resonance       = 0.42f,
        .env_amount      = 0.45f,
        .key_tracking    = 0.70f,
        .filter_sat      = 0.30f,
        .env_attack_ms   = 12.0f,
        .env_decay_ms    = 500.0f,
        .env_sustain     = 0.62f,
        .env_release_ms  = 310.0f,
        .glide_ms        = 70.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.8f,
        .vib_depth_cents = 18.0f,
        .vib_delay_ms    = 220.0f,
        .lfo_rate_hz     = 1.5f,
        .lfo_depth       = 0.15f,
        .lfo_wave        = ICARUS_LFO_TRI,
        .lfo_dest        = ICARUS_LFO_DEST_PULSE_WIDTH,
        .guitar_level    = 0.08f,
        .analog_level    = 0.15f,
        .synth_level     = 0.90f,
        .master_level    = 0.85f,
        .stereo_width    = 0.30f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 4. Synth Bass — Oscillator: Sub-octave punchy low-pass bass synthesizer */
    {
        .name            = "Synth Bass",
        .mode            = ICARUS_MODE_OSCILLATOR,
        .dist_mode       = ICARUS_DIST_HARD_CLIP,
        .analog_drive    = 0.50f,
        .input_gain      = 1.20f,
        .pre_emphasis    = 0u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_SQUARE,
        .pitch_a         = -12,
        .pitch_b         = 0,
        .fine_tune       = 0,
        .level_a         = 0.90f,
        .level_b         = 0.55f,
        .detune_cents    = 5.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ICARUS_SUB_OCT1,
        .sub_level       = 0.75f,
        .osc_mix         = 0.35f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 430.0f,
        .resonance       = 0.48f,
        .env_amount      = 0.68f,
        .key_tracking    = 0.50f,
        .filter_sat      = 0.32f,
        .env_attack_ms   = 3.0f,
        .env_decay_ms    = 190.0f,
        .env_sustain     = 0.38f,
        .env_release_ms  = 120.0f,
        .glide_ms        = 28.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 4.5f,
        .vib_depth_cents = 4.0f,
        .vib_delay_ms    = 500.0f,
        .lfo_rate_hz     = 0.5f,
        .lfo_depth       = 0.00f,
        .lfo_wave        = ICARUS_LFO_SINE,
        .lfo_dest        = ICARUS_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.10f,
        .analog_level    = 0.20f,
        .synth_level     = 0.95f,
        .master_level    = 0.88f,
        .stereo_width    = 0.10f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 5. Liquid Lead — Oscillator: Expressive vibrato and fluid filter movement */
    {
        .name            = "Liquid Lead",
        .mode            = ICARUS_MODE_OSCILLATOR,
        .dist_mode       = ICARUS_DIST_SOFT_CLIP,
        .analog_drive    = 0.40f,
        .input_gain      = 1.10f,
        .pre_emphasis    = 1u,
        .wave_a          = ICARUS_WAVE_TRIANGLE,
        .wave_b          = ICARUS_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 2,
        .level_a         = 0.80f,
        .level_b         = 0.75f,
        .detune_cents    = 9.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ICARUS_SUB_OCT1,
        .sub_level       = 0.25f,
        .osc_mix         = 0.50f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 1150.0f,
        .resonance       = 0.66f,
        .env_amount      = 0.58f,
        .key_tracking    = 0.80f,
        .filter_sat      = 0.35f,
        .env_attack_ms   = 18.0f,
        .env_decay_ms    = 460.0f,
        .env_sustain     = 0.60f,
        .env_release_ms  = 290.0f,
        .glide_ms        = 85.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 6.0f,
        .vib_depth_cents = 22.0f,
        .vib_delay_ms    = 180.0f,
        .lfo_rate_hz     = 2.4f,
        .lfo_depth       = 0.28f,
        .lfo_wave        = ICARUS_LFO_SINE,
        .lfo_dest        = ICARUS_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.05f,
        .analog_level    = 0.18f,
        .synth_level     = 0.92f,
        .master_level    = 0.85f,
        .stereo_width    = 0.45f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 6. Analog Fuzz — Analog: Aggressive harmonic polyphonic synth distortion */
    {
        .name            = "Analog Fuzz",
        .mode            = ICARUS_MODE_ANALOG,
        .dist_mode       = ICARUS_DIST_FUZZ,
        .analog_drive    = 0.82f,
        .input_gain      = 1.35f,
        .pre_emphasis    = 1u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = -12,
        .fine_tune       = 0,
        .level_a         = 0.70f,
        .level_b         = 0.50f,
        .detune_cents    = 8.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ICARUS_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.40f,
        .filter_mode     = ICARUS_FILTER_LP12,
        .cutoff_hz       = 1600.0f,
        .resonance       = 0.52f,
        .env_amount      = 0.62f,
        .key_tracking    = 0.30f,
        .filter_sat      = 0.55f,
        .env_attack_ms   = 5.0f,
        .env_decay_ms    = 320.0f,
        .env_sustain     = 0.50f,
        .env_release_ms  = 220.0f,
        .glide_ms        = 0.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.0f,
        .vib_depth_cents = 0.0f,
        .vib_delay_ms    = 300.0f,
        .lfo_rate_hz     = 0.9f,
        .lfo_depth       = 0.12f,
        .lfo_wave        = ICARUS_LFO_TRI,
        .lfo_dest        = ICARUS_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.15f,
        .analog_level    = 0.92f,
        .synth_level     = 0.00f,
        .master_level    = 0.84f,
        .stereo_width    = 0.25f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 7. GR Texture — Hybrid: Dense Hexa-inspired guitar + oscillator blend */
    {
        .name            = "GR Texture",
        .mode            = ICARUS_MODE_HYBRID,
        .dist_mode       = ICARUS_DIST_HEXA,
        .analog_drive    = 0.68f,
        .input_gain      = 1.20f,
        .pre_emphasis    = 1u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 12,
        .fine_tune       = 0,
        .level_a         = 0.80f,
        .level_b         = 0.45f,
        .detune_cents    = 12.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ICARUS_SUB_OCT1,
        .sub_level       = 0.30f,
        .osc_mix         = 0.38f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 1350.0f,
        .resonance       = 0.48f,
        .env_amount      = 0.56f,
        .key_tracking    = 0.60f,
        .filter_sat      = 0.40f,
        .env_attack_ms   = 15.0f,
        .env_decay_ms    = 480.0f,
        .env_sustain     = 0.65f,
        .env_release_ms  = 340.0f,
        .glide_ms        = 42.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.4f,
        .vib_depth_cents = 14.0f,
        .vib_delay_ms    = 250.0f,
        .lfo_rate_hz     = 1.1f,
        .lfo_depth       = 0.16f,
        .lfo_wave        = ICARUS_LFO_SINE,
        .lfo_dest        = ICARUS_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.25f,
        .analog_level    = 0.72f,
        .synth_level     = 0.75f,
        .master_level    = 0.85f,
        .stereo_width    = 0.50f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 8. Soft Pad — Hybrid: Slow attack, long release atmospheric synth pad */
    {
        .name            = "Soft Pad",
        .mode            = ICARUS_MODE_HYBRID,
        .dist_mode       = ICARUS_DIST_SOFT_CLIP,
        .analog_drive    = 0.30f,
        .input_gain      = 1.05f,
        .pre_emphasis    = 0u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_TRIANGLE,
        .pitch_a         = 0,
        .pitch_b         = 12,
        .fine_tune       = 0,
        .level_a         = 0.75f,
        .level_b         = 0.65f,
        .detune_cents    = 18.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ICARUS_SUB_OCT1,
        .sub_level       = 0.35f,
        .osc_mix         = 0.48f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 520.0f,
        .resonance       = 0.32f,
        .env_amount      = 0.60f,
        .key_tracking    = 0.55f,
        .filter_sat      = 0.22f,
        .env_attack_ms   = 380.0f,
        .env_decay_ms    = 900.0f,
        .env_sustain     = 0.78f,
        .env_release_ms  = 1100.0f,
        .glide_ms        = 110.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 4.8f,
        .vib_depth_cents = 15.0f,
        .vib_delay_ms    = 350.0f,
        .lfo_rate_hz     = 0.45f,
        .lfo_depth       = 0.25f,
        .lfo_wave        = ICARUS_LFO_SINE,
        .lfo_dest        = ICARUS_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.30f,
        .analog_level    = 0.45f,
        .synth_level     = 0.82f,
        .master_level    = 0.85f,
        .stereo_width    = 0.75f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 9. Square Lead — Oscillator: Hollow woody square-wave lead with PWM */
    {
        .name            = "Square Lead",
        .mode            = ICARUS_MODE_OSCILLATOR,
        .dist_mode       = ICARUS_DIST_SOFT_CLIP,
        .analog_drive    = 0.35f,
        .input_gain      = 1.10f,
        .pre_emphasis    = 1u,
        .wave_a          = ICARUS_WAVE_SQUARE,
        .wave_b          = ICARUS_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 0,
        .level_a         = 0.88f,
        .level_b         = 0.60f,
        .detune_cents    = 8.0f,
        .pulse_width     = 0.36f,
        .sub_mode        = ICARUS_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.40f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 1750.0f,
        .resonance       = 0.45f,
        .env_amount      = 0.48f,
        .key_tracking    = 0.75f,
        .filter_sat      = 0.28f,
        .env_attack_ms   = 6.0f,
        .env_decay_ms    = 350.0f,
        .env_sustain     = 0.65f,
        .env_release_ms  = 210.0f,
        .glide_ms        = 40.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.7f,
        .vib_depth_cents = 15.0f,
        .vib_delay_ms    = 240.0f,
        .lfo_rate_hz     = 1.8f,
        .lfo_depth       = 0.35f,
        .lfo_wave        = ICARUS_LFO_TRI,
        .lfo_dest        = ICARUS_LFO_DEST_PULSE_WIDTH,
        .guitar_level    = 0.08f,
        .analog_level    = 0.12f,
        .synth_level     = 0.92f,
        .master_level    = 0.85f,
        .stereo_width    = 0.35f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 10. Dark Sequence — Analog: Foldback + Sample-and-Hold LFO filtered texture */
    {
        .name            = "Dark Sequence",
        .mode            = ICARUS_MODE_ANALOG,
        .dist_mode       = ICARUS_DIST_FOLDBACK,
        .analog_drive    = 0.64f,
        .input_gain      = 1.25f,
        .pre_emphasis    = 1u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = -12,
        .fine_tune       = 0,
        .level_a         = 0.80f,
        .level_b         = 0.50f,
        .detune_cents    = 10.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ICARUS_SUB_OCT1,
        .sub_level       = 0.40f,
        .osc_mix         = 0.40f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 780.0f,
        .resonance       = 0.68f,
        .env_amount      = 0.35f,
        .key_tracking    = 0.40f,
        .filter_sat      = 0.45f,
        .env_attack_ms   = 10.0f,
        .env_decay_ms    = 260.0f,
        .env_sustain     = 0.45f,
        .env_release_ms  = 200.0f,
        .glide_ms        = 0.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.0f,
        .vib_depth_cents = 0.0f,
        .vib_delay_ms    = 300.0f,
        .lfo_rate_hz     = 6.5f,
        .lfo_depth       = 0.58f,
        .lfo_wave        = ICARUS_LFO_SH,
        .lfo_dest        = ICARUS_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.12f,
        .analog_level    = 0.90f,
        .synth_level     = 0.00f,
        .master_level    = 0.85f,
        .stereo_width    = 0.40f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 11. Pipe Organ — Hybrid: Perfect Strangers cathedral organ (16' Sub + 8' Principal Saw + 4' Asymmetric Reed/Flute + Hexa) */
    {
        .name            = "Pipe Organ",
        .mode            = ICARUS_MODE_HYBRID,
        .dist_mode       = ICARUS_DIST_HEXA,
        .analog_drive    = 0.42f,
        .input_gain      = 1.25f,
        .pre_emphasis    = 1u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = 12,
        .fine_tune       = 0,
        .level_a         = 0.92f,
        .level_b         = 0.80f,
        .detune_cents    = 14.0f,
        .pulse_width     = 0.36f,
        .sub_mode        = ICARUS_SUB_OCT1,
        .sub_level       = 0.75f,
        .osc_mix         = 0.45f,
        .filter_mode     = ICARUS_FILTER_LP12,
        .cutoff_hz       = 3800.0f,
        .resonance       = 0.24f,
        .env_amount      = 0.14f,
        .key_tracking    = 0.85f,
        .filter_sat      = 0.16f,
        .env_attack_ms   = 10.0f,
        .env_decay_ms    = 1200.0f,
        .env_sustain     = 0.94f,
        .env_release_ms  = 340.0f,
        .glide_ms        = 0.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 6.1f,
        .vib_depth_cents = 14.0f,
        .vib_delay_ms    = 0.0f,
        .lfo_rate_hz     = 0.85f,
        .lfo_depth       = 0.22f,
        .lfo_wave        = ICARUS_LFO_SINE,
        .lfo_dest        = ICARUS_LFO_DEST_PULSE_WIDTH,
        .guitar_level    = 0.14f,
        .analog_level    = 0.48f,
        .synth_level     = 0.95f,
        .master_level    = 0.90f,
        .stereo_width    = 0.80f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u,
        .expression      = 0.22f,
        .drift_cents     = 3.5f
    },
    /* 12. Pure GR — Analog: GR-inspired direct polyphonic Hexa + resonant filter */
    {
        .name            = "Pure GR",
        .mode            = ICARUS_MODE_ANALOG,
        .dist_mode       = ICARUS_DIST_HEXA,
        .analog_drive    = 0.66f,
        .input_gain      = 1.25f,
        .pre_emphasis    = 1u,
        .wave_a          = ICARUS_WAVE_SAW,
        .wave_b          = ICARUS_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 0,
        .level_a         = 0.80f,
        .level_b         = 0.40f,
        .detune_cents    = 6.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ICARUS_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.35f,
        .filter_mode     = ICARUS_FILTER_LP24,
        .cutoff_hz       = 980.0f,
        .resonance       = 0.50f,
        .env_amount      = 0.68f,
        .key_tracking    = 0.50f,
        .filter_sat      = 0.36f,
        .env_attack_ms   = 8.0f,
        .env_decay_ms    = 310.0f,
        .env_sustain     = 0.54f,
        .env_release_ms  = 220.0f,
        .glide_ms        = 0.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.5f,
        .vib_depth_cents = 0.0f,
        .vib_delay_ms    = 300.0f,
        .lfo_rate_hz     = 0.75f,
        .lfo_depth       = 0.10f,
        .lfo_wave        = ICARUS_LFO_SINE,
        .lfo_dest        = ICARUS_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.18f,
        .analog_level    = 0.92f,
        .synth_level     = 0.00f,
        .master_level    = 0.86f,
        .stereo_width    = 0.35f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    }
};

const icarus_preset_t *icarus_get_factory_preset(uint8_t index) {
    if (index >= ICARUS_FACTORY_PRESET_COUNT) index = 0u;
    return &k_factory_presets[index];
}

void icarus_apply_params(icarus_state_t *s) {
    if (!s) return;
    const icarus_preset_t *p = &s->params;

    /* Engine A: Analog conditioning, distortion & filter */
    icarus_analog_set_conditioning(&s->analog, p->input_gain, p->pre_emphasis, 1u);
    icarus_analog_set_distortion(&s->analog, p->dist_mode, p->analog_drive);
    s->analog.follow = clampf(p->expression, 0.0f, 1.0f);
    icarus_filter_set_params(&s->analog.filter, p->cutoff_hz, p->resonance,
                            p->env_amount, p->key_tracking, p->filter_sat, p->filter_mode);

    /* Engine B: Dual Oscillator + Sub + Synth Filter */
    s->osc_eng.osc_a.waveform    = p->wave_a;
    s->osc_eng.osc_a.pitch_semi  = p->pitch_a;
    s->osc_eng.osc_a.fine_cents  = p->fine_tune;
    s->osc_eng.osc_a.level       = p->level_a;
    s->osc_eng.osc_a.detune      = -0.5f * p->detune_cents;
    s->osc_eng.osc_a.pulse_width = p->pulse_width;

    s->osc_eng.osc_b.waveform    = p->wave_b;
    s->osc_eng.osc_b.pitch_semi  = p->pitch_b;
    s->osc_eng.osc_b.fine_cents  = p->fine_tune;
    s->osc_eng.osc_b.level       = p->level_b;
    s->osc_eng.osc_b.detune      = +0.5f * p->detune_cents;
    s->osc_eng.osc_b.pulse_width = p->pulse_width;

    s->osc_eng.sub_mode          = p->sub_mode;
    s->osc_eng.sub_level         = p->sub_level;
    s->osc_eng.osc_mix           = p->osc_mix;

    icarus_filter_set_params(&s->synth_filter, p->cutoff_hz, p->resonance,
                            p->env_amount, p->key_tracking, p->filter_sat, p->filter_mode);

    /* Envelopes */
    icarus_env_follower_set_times(&s->env_follower,
                                 clampf(p->env_attack_ms * 0.25f + 2.0f, 1.5f, 80.0f),
                                 clampf(p->env_release_ms * 0.50f + 40.0f, 30.0f, 600.0f));
    icarus_env_set_params(&s->adsr, p->env_attack_ms, p->env_decay_ms,
                         p->env_sustain, p->env_release_ms);

    /* Glide, Vibrato & Modulation LFO */
    icarus_glide_set_params(&s->glide, p->glide_ms,
                           p->glide_legato ? ICARUS_GLIDE_LEGATO : ICARUS_GLIDE_ALWAYS,
                           ICARUS_GLIDE_CURVE_EXP, ICARUS_GLIDE_DIR_BOTH, 0.85f);
    icarus_vibrato_set_params(&s->vibrato, p->vib_rate_hz, p->vib_depth_cents,
                             p->vib_delay_ms, 300.0f, ICARUS_VIB_SINE, 0.35f);
    icarus_lfo_set_params(&s->lfo, p->lfo_rate_hz, p->lfo_depth,
                         (icarus_lfo_wave_t)p->lfo_wave, (icarus_lfo_dest_t)p->lfo_dest);

    /* Mixer */
    icarus_mix_set_params(&s->mixer, p->mode, p->guitar_level, p->analog_level,
                         p->synth_level, p->master_level, 0.0f, p->stereo_width);

    if (s->live_cutoff_hz < 80.0f) {
        s->live_cutoff_hz = p->cutoff_hz;
    }
    if (s->live_pwm < 0.05f) {
        s->live_pwm = p->pulse_width;
    }
}

#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
static void ptrk_init(void);
#endif

void icarus_init(icarus_state_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
    ptrk_init();
#endif
    icarus_env_follower_init(&s->env_follower);
    icarus_env_init(&s->adsr);
    icarus_analog_init(&s->analog);
    icarus_osc_engine_init(&s->osc_eng);
    icarus_filter_init(&s->synth_filter);
    icarus_pitch_init(&s->pitch);
    icarus_glide_init(&s->glide);
    icarus_vibrato_init(&s->vibrato);
    icarus_lfo_init(&s->lfo);
    icarus_mix_init(&s->mixer);

    icarus_gate_init(&s->in_gate,  -52.0f, 1.5f,  45.0f, 90.0f,  5.0f);
    icarus_gate_init(&s->out_gate, -58.0f, 2.0f,  60.0f, 140.0f, 6.0f);

    s->xfade_gain = 1.0f;
    s->xfade_step = 0.0f;
    s->enabled    = 1u;

    icarus_load_preset(s, 0u);
}

void icarus_reset(icarus_state_t *s) {
    if (!s) return;
    icarus_env_follower_reset(&s->env_follower);
    icarus_env_reset(&s->adsr);
    icarus_analog_reset(&s->analog);
    icarus_osc_engine_reset(&s->osc_eng);
    icarus_filter_reset(&s->synth_filter);
    icarus_pitch_reset(&s->pitch);
    icarus_mix_reset(&s->mixer);
    s->xfade_gain = 0.0f;
    s->xfade_step = 1.0f / (float)(4 * PA_BLOCK_N); /* 4-block smooth ramp-in */
}

void icarus_load_preset(icarus_state_t *s, uint8_t index) {
    if (!s) return;
    if (index >= ICARUS_FACTORY_PRESET_COUNT) index = 0u;
    s->preset_index = index;
    s->params       = *icarus_get_factory_preset(index);
    if (s->params.expression <= 0.0f) {                       /* a factory preset that does not say: the default for its mode */
        switch (s->params.mode) {
            case ICARUS_MODE_OSCILLATOR: s->params.expression = 0.80f; break;   /* leads and basses follow the string, but the ADSR keeps its shape */
            case ICARUS_MODE_HYBRID:     s->params.expression = 0.80f; break;
            case ICARUS_MODE_GUITAR:     s->params.expression = 0.90f; break;
            default:                    s->params.expression = 0.85f; break;   /* ANALOG: the saturators flatten the level, the follow stage puts it back */
        }
    }
    if (s->params.drift_cents <= 0.0f) s->params.drift_cents = (s->params.mode == ICARUS_MODE_ANALOG) ? 0.0f : 2.0f;
    icarus_apply_params(s);
    g_pa_synth_shed = 0u;                                   /* preset reload clears any overload shed */
    /* Trigger a brief click-free ramp-in on preset switch */
    s->xfade_gain = 0.15f;
    s->xfade_step = (1.0f - 0.15f) / (float)(2 * PA_BLOCK_N);
}

void icarus_set_mode(icarus_state_t *s, uint8_t mode) {
    if (!s) return;
    if (mode >= ICARUS_MODE_COUNT) mode = ICARUS_MODE_HYBRID;
    s->params.mode = mode;
    /* Ensure sensible default levels if switching into a mode whose level was 0 */
    if ((mode == ICARUS_MODE_OSCILLATOR || mode == ICARUS_MODE_HYBRID) && s->params.synth_level < 0.15f) {
        s->params.synth_level = 0.80f;
    }
    if ((mode == ICARUS_MODE_ANALOG || mode == ICARUS_MODE_HYBRID) && s->params.analog_level < 0.15f) {
        s->params.analog_level = 0.75f;
    }
    if (mode == ICARUS_MODE_GUITAR && s->params.guitar_level < 0.25f) {
        s->params.guitar_level = 0.85f;
    }
    icarus_apply_params(s);
}

icarus_state_t *icarus_create(void) {
    icarus_state_t *s = (icarus_state_t *)pa_alloc((uint32_t)sizeof(icarus_state_t));
    if (!s) return 0;
    icarus_init(s);
    s_icarus_active = s;
    g_pa_synth_shed = 0u;                                   /* a fresh synth starts un-shed */
    return s;
}

void icarus_destroy(icarus_state_t *s) {
    if (!s) return;
    if (s_icarus_active == s) s_icarus_active = 0;
    pa_free(s);
}

icarus_state_t *icarus_get_active(void) {
    return s_icarus_active;
}

void icarus_set_active_enable(int enable) {
    if (enable) {
        g_pa_synth_shed = 0u;
        if (!s_icarus_active) {
            icarus_create();
        } else {
            s_icarus_active->enabled = 1u;
        }
    } else {
        if (s_icarus_active) {
            s_icarus_active->enabled = 0u;
        }
    }
}

int icarus_is_active(void) {
    return (s_icarus_active && s_icarus_active->enabled) ? 1 : 0;
}

void icarus_sync_chain_routing(icarus_state_t *s, pa_fx_chain_t *chain) {
    if (!s || !chain) return;
    /* In the focused 5-block chain (DYN -> SYNTH -> AMP -> CAB -> EQ), DYN and EQ
     * can run simultaneously alongside SYNTH, AMP, and CAB. Only sync NAM & CAB
     * bypass when modified from the Icarus Stage Integration menu. */
    pa_fx_chain_set_slot_enabled(chain, PA_SLOT_AMP, s->params.nam_enable ? 1 : 0);
    pa_fx_chain_set_slot_enabled(chain, PA_SLOT_CAB, s->params.cab_enable ? 1 : 0);
}

void icarus_enter_synth_mode(pa_fx_chain_t *chain) {
    g_pa_synth_shed = 0u;                                   /* entering synth mode clears any overload shed */
    if (chain) {
        fx_node_t *nd = chain->slots[PA_SLOT_SYNTH];
        if (nd && !nd->state && nd->create) {
            nd->create(nd);
        }
    } else {
        if (!s_icarus_active) icarus_create();
    }
    if (s_icarus_active && chain) {
        s_icarus_active->params.nam_enable = (chain->slots[PA_SLOT_AMP] && chain->slots[PA_SLOT_AMP]->active) ? 1u : 0u;
        s_icarus_active->params.cab_enable = (chain->slots[PA_SLOT_CAB] && chain->slots[PA_SLOT_CAB]->active) ? 1u : 0u;
    }
}

void icarus_exit_synth_mode(pa_fx_chain_t *chain) {
    /* Leaving the Icarus UI clears the UI telemetry TTL. If SYNTH is bypassed,
     * garbage collection can reclaim its state. */
    if (s_icarus_active) {
        s_icarus_active->ui_ttl = 0u;
    }
    (void)chain;
}

/* ===================================================================== */
/* First-Class FX Node Interface (fx_synth_new for PA_SLOT_SYNTH)        */
/* ===================================================================== */
static void fx_synth_create(fx_node_t *nd) {
    if (!nd) return;
    if (nd->state) {
        s_icarus_active = (icarus_state_t *)nd->state;
        s_icarus_active->enabled = 1u;
        return;
    }
    icarus_state_t *s = icarus_create();
    nd->state = s;
}

static void fx_synth_destroy(fx_node_t *nd) {
    if (!nd || !nd->state) return;
    icarus_destroy((icarus_state_t *)nd->state);
    nd->state = 0;
}

static void fx_synth_reset(fx_node_t *nd) {
    if (!nd || !nd->state) return;
    icarus_reset((icarus_state_t *)nd->state);
}

static void fx_synth_run_mono(fx_node_t *nd, int32_t *buf, int n) {
    if (!nd || !nd->state) return;
    icarus_state_t *s = (icarus_state_t *)nd->state;
    if (!s->enabled) return;
    icarus_process_block_q21(s, buf, buf, 0, n);
}

static void fx_synth_run_stereo(fx_node_t *nd, int32_t *buf_L, int32_t *buf_R, int n) {
    if (!nd || !nd->state) return;
    icarus_state_t *s = (icarus_state_t *)nd->state;
    if (!s->enabled) return;
    icarus_process_block_q21(s, buf_L, buf_L, buf_R, n);
}

static void fx_synth_set_param(fx_node_t *nd, int id, int32_t val) {
    if (!nd || !nd->state) return;
    icarus_state_t *s = (icarus_state_t *)nd->state;
    /* id == 10 (or legacy id == 3 if large/small): explicit preset/model selection */
    if (id == 10) {
        uint8_t p_idx = (uint8_t)((val < 0 ? 0 : val) % ICARUS_FACTORY_PRESET_COUNT);
        if (s->preset_index != p_idx) {
            icarus_load_preset(s, p_idx);
        }
        return;
    }
    float k01 = clampf((float)val / (float)PA_Q21_ONE, 0.0f, 1.0f);
    switch (id) {
        case 0: /* CUTOFF (120 .. 8000 Hz exponential-ish) */
            s->params.cutoff_hz = 120.0f + k01 * k01 * 7880.0f;
            break;
        case 1: /* RESONANCE (0 .. 95%) */
            s->params.resonance = k01 * 0.95f;
            break;
        case 2: /* ENV DEPTH (-100% .. +100%, 50% = +50% default sweep) */
            s->params.env_amount = k01;
            break;
        case 3: /* SYNTH MIX (0 .. 100%) */
            s->params.synth_level = k01;
            break;
        case 4: /* ANALOG DRIVE (0 .. 100%) */
            s->params.analog_drive = k01;
            break;
        case 5: /* GLIDE TIME (0 .. 400 ms) */
            s->params.glide_ms = k01 * 400.0f;
            break;
        default:
            return;
    }
    icarus_apply_params(s);
}

static fx_node_t g_synth_node = {
    .next        = 0,
    .active      = 0,
    .frame       = 64,
    .state       = 0,
    .tag         = "synth",
    .run         = fx_synth_run_mono,
    .run_stereo  = fx_synth_run_stereo,
    .create      = fx_synth_create,
    .destroy     = fx_synth_destroy,
    .set_param   = fx_synth_set_param,
    .reset       = fx_synth_reset
};

fx_node_t *fx_synth_new(void) {
    /* Static singleton like the other nodes: never touch state here, or a second
     * call would orphan a live pool allocation. */
    return &g_synth_node;
}

/* PITCH TELEMETRY (device only): every change of what the tracker reports goes into a small ring in RAM, so tools/pitch_capture_pc.py can read it over USB while the guitar is played and
 * compare it with what was played. The pointer to the ring is at SRAM 0x01C7C9B4 (0 = no ring yet). One event is 12 bytes: the block number, the tracked and the raw frequency (Hz x 32), the flags
 * (1 valid, 2 onset, 4 release, 8 legato, bits 4-5 the tracker state), the confidence (x 255), the MIDI note, the level of the string (log scale, 0.5 dB per step from -96 dB). */
#if !defined(PA_HOST_BUILD) && !defined(PA_HOST_SIM)
typedef struct { uint32_t blk; uint16_t f, raw; uint8_t flags, conf, midi, lvl; } pa_ptrk_ev_t;
#define PA_PTRK_N 80u
static struct { uint32_t magic, wr, blocks, n; pa_ptrk_ev_t ev[PA_PTRK_N]; } s_ptrk __attribute__((section(".noinit")));
static uint8_t  s_ptrk_flags;
static uint16_t s_ptrk_f;
static void ptrk_init(void) {
    s_ptrk.magic = 0x4B525450u; s_ptrk.wr = 0u; s_ptrk.blocks = 0u; s_ptrk.n = PA_PTRK_N;
    *(volatile uint32_t *)0x01C7C9B4u = (uint32_t)(uintptr_t)&s_ptrk;
}
static void ptrk_log(const icarus_pitch_t *p) {
    if (s_ptrk.magic != 0x4B525450u) ptrk_init();
    s_ptrk.blocks++;
    const uint8_t flags = (uint8_t)((p->valid ? 1u : 0u) | (p->onset ? 2u : 0u) | (p->release ? 4u : 0u) | (p->legato ? 8u : 0u) | ((p->state & 3u) << 4));
    const float fq = p->frequency * 32.0f;
    const uint16_t f16 = (uint16_t)((fq > 65000.0f) ? 65000.0f : fq);
    const int32_t df = (int32_t)f16 - (int32_t)s_ptrk_f;
    if (flags == s_ptrk_flags && (df < 0 ? -df : df) * 200 <= (int32_t)f16) return;       /* nothing new: the same note, within 0.5 % */
    s_ptrk_flags = flags; s_ptrk_f = f16;
    pa_ptrk_ev_t *e = &s_ptrk.ev[s_ptrk.wr % PA_PTRK_N];
    const float r32 = p->raw_frequency * 32.0f;
    int32_t lv = (int32_t)((fast_log2f_a(p->env_fast + 1.0e-5f) * 6.0206f + 96.0f) * 2.0f);
    e->blk = s_ptrk.blocks; e->f = f16; e->raw = (uint16_t)((r32 > 65000.0f) ? 65000.0f : (r32 < 0.0f ? 0.0f : r32));
    e->flags = flags; e->conf = (uint8_t)(p->confidence * 255.0f); e->midi = p->midi_note; e->lvl = (uint8_t)((lv < 0) ? 0 : (lv > 255 ? 255 : lv));
    __asm__ volatile("" ::: "memory");
    s_ptrk.wr++;
}
#define PTRK_LOG(p) ptrk_log(p)
#else
#define PTRK_LOG(p) ((void)0)
#endif

void icarus_poll_background(icarus_state_t *s) {
    if (!s || !s->enabled) return;
    /* Same condition as need_osc in icarus_process_block_f32: whenever the pitch tracker is fed, its lag search runs HERE (main loop),
     * not in the audio interrupt. GUITAR mode feeds it too when the oscillator branch is audible. */
    const uint8_t mode = s->params.mode;
    if (mode == ICARUS_MODE_OSCILLATOR || mode == ICARUS_MODE_HYBRID ||
        (mode == ICARUS_MODE_GUITAR && s->params.synth_level > 0.01f)) {
        icarus_pitch_poll_background(&s->pitch);
    }
}

void icarus_process_block_f32(icarus_state_t *s, const float *in_mono,
                             float *out_L, float *out_R, int n) {
    if (!s || !in_mono || !out_L || !out_R || n <= 0) return;

    float gated_in[PA_BLOCK_N];
    float cond_buf[PA_BLOCK_N];
    float env_buf[PA_BLOCK_N];
    float analog_buf[PA_BLOCK_N];
    float synth_buf[PA_BLOCK_N];
    float adsr_buf[PA_BLOCK_N];

    if (n > PA_BLOCK_N) n = PA_BLOCK_N;

    AP_DEV_BEGIN(ap_t);
    AP_BEGIN(0);
    if (g_pa_synth_shed) {                                   /* overload protection: the guitar, dry, on both channels */
        for (int i = 0; i < n; i++) { out_L[i] = in_mono[i]; out_R[i] = in_mono[i]; }
        return;
    }

    /* 1. Optional Input Synth Noise Gate */
    if (s->params.gate_enable) {
        float sum_abs = 0.0f;
        for (int i = 0; i < n; i++) {
            const float x = icarus_san(in_mono[i]) + ((i & 1) ? 1.0e-18f : -1.0e-18f);   /* anti-denormal floor, -360 dB: states that decay never reach denormal range */
            gated_in[i] = x;
            sum_abs += fabsf(x);
        }
        const float g0 = s->in_gate.gain;
        const float g1 = icarus_gate_block(&s->in_gate, sum_abs / (float)n, n);
        const float dg = (g1 - g0) / (float)n;
        float g = g0;
        for (int i = 0; i < n; i++) { g += dg; gated_in[i] *= g; }
    } else {
        for (int i = 0; i < n; i++) gated_in[i] = icarus_san(in_mono[i]) + ((i & 1) ? 1.0e-18f : -1.0e-18f);
    }

    ICARUS_CP(0x01);
    AP_END(0); AP_BEGIN(1);
    /* 2. Input Conditioning & Envelope Follower (Always runs — feeds Direct, Analog & Expression) */
    icarus_input_condition_block(&s->analog, &s->env_follower, gated_in, cond_buf, env_buf, n);
    float block_env = env_buf[n - 1];

    ICARUS_CP(0x02);
    AP_END(1); AP_BEGIN(2);
    /* 3. Pitch Analysis & Modulation Frame */
    const uint8_t mode = s->params.mode;
    const int need_osc = (mode == ICARUS_MODE_OSCILLATOR || mode == ICARUS_MODE_HYBRID ||
                          (mode == ICARUS_MODE_GUITAR && s->params.synth_level > 0.01f));
    const int need_ana = (mode == ICARUS_MODE_ANALOG || mode == ICARUS_MODE_HYBRID ||
                          (mode == ICARUS_MODE_GUITAR && s->params.analog_level > 0.01f));

    if (need_osc) {
        icarus_pitch_feed_block(&s->pitch, cond_buf, n);
        PTRK_LOG(&s->pitch);
    } else {
        /* Even in pure Analog mode, step LFO & ADSR from picking envelope onset (lower threshold so -40 dB soft picks trigger cleanly) */
        s->pitch.onset   = s->env_follower.onset_trig;
        s->pitch.legato  = 0u;
        s->pitch.valid   = (block_env > 0.004f || s->env_follower.fast_level > 0.005f) ? 1u : 0u;
        s->pitch.release = (!s->pitch.valid && s->adsr.gate) ? 1u : 0u;
    }

    ICARUS_CP(0x03);
    AP_END(2); AP_BEGIN(3);
    /* Trigger / release ADSR envelope from pitch/onset detector */
    if (s->pitch.onset || (s->env_follower.onset_trig && !s->adsr.gate)) {
        /* Picking strength in dB, not a linear gain with a floor: the old clamp(fast * 2.2, 0.35, 1) put every note of a DI at -33 dBFS rms on the floor (soft and hard picking sounded the same).
         * 0.22 (about -13 dBFS on the follower) is a hard pick = 1.0; the exponent 0.55 gives a dynamic range of about 2:1 in amplitude per 4:1 in picking level. */
        const float fl  = s->env_follower.fast_level;
        float vel = fast_exp2f_a(0.55f * fast_log2f_a(fl * (1.0f / 0.22f)));
        vel = clampf(vel, 0.16f, 1.0f);
        s->expr_ref = (fl > 0.006f) ? fl : 0.006f;
        icarus_env_trigger(&s->adsr, vel);
    } else if (s->adsr.gate && s->env_follower.holdoff > 500u && s->env_follower.fast_level > s->expr_ref) {
        /* Peak-track the pick transient over the first ~10 ms after an early threshold crossing so hard picks aren't latched at their initial ramp level */
        const float fl = s->env_follower.fast_level;
        s->expr_ref = fl;
        float vel = fast_exp2f_a(0.55f * fast_log2f_a(fl * (1.0f / 0.22f)));
        vel = clampf(vel, 0.16f, 1.0f);
        if (vel > s->adsr.velocity) s->adsr.velocity = vel;
    } else if (s->pitch.release || (!s->pitch.valid && s->adsr.gate)) {
        icarus_env_release(&s->adsr);
    }

    ICARUS_CP(0x04);
    const float ex_pow = 0.9f * clampf(s->params.expression, 0.0f, 1.0f);
    const float expr_ref_inv = 1.0f / ((s->expr_ref > 0.005f) ? s->expr_ref : 0.2f);
    for (int base = 0; base < n; base += 8) {                      /* the ADSR is read at 5.5 kHz (filter updates, amplitude): step it once per 8 samples */
        const float a_env = icarus_env_step8(&s->adsr);
        const int end = (base + 8 < n) ? base + 8 : n;
        /* EXPRESSION: the voice follows the string. The follower level relative to the note's own trigger level (r), to the power 0.9 x the preset's expression (0.72 by default), scales the ADSR: a note that rings out fades
         * with the guitar, a note picked harder than the trigger swells, and the ADSR keeps its shape (attack, decay, sustain, release). The old blend was 0.65 * ADSR + 0.55 * level:
         * with a sustain near 0.7 the first term decided everything, so the voice sat at one level whatever was played (level-vs-input correlation about 0 on real captures). */
        const int mid = (base + 4 < n) ? base + 4 : n - 1;
        const float r = env_buf[mid] * expr_ref_inv;
        float ex = (r > 1.0e-4f) ? fast_exp2f_a(ex_pow * fast_log2f_a(r)) : 0.0f;
        if (ex > 1.15f) ex = 1.15f;
        const float v = 0.65f * a_env * ex;
        const float vc = (v > 1.0f) ? 1.0f : ((v < 0.0f) ? 0.0f : v);
        for (int i = base; i < end; i++) adsr_buf[i] = vc;
    }

    ICARUS_CP(0x05);
    icarus_mod_frame_t mod;
    icarus_compute_mod_frame(&mod,
                            &s->glide,
                            &s->vibrato,
                            &s->lfo,
                            s->pitch.frequency,
                            s->pitch.onset,
                            s->pitch.legato,
                            block_env,
                            s->adsr.velocity,
                            n);

    ICARUS_CP(0x06);
    AP_END(3);
    AP_DEV_END(ap_t, 5); AP_DEV_RESTART(ap_t);
    AP_BEGIN(4);
    /* 4. Engine A — Polyphonic Analog Processing */
    if (need_ana) {
        icarus_analog_process_block(&s->analog, cond_buf, adsr_buf, analog_buf, n,
                                   mod.glided_freq_hz, mod.filter_cutoff_oct);
    } else {
        memset(analog_buf, 0, (size_t)n * sizeof(float));
    }

    ICARUS_CP(0x07);
    AP_END(4); AP_BEGIN(5);
    /* 5. Engine B — Pitch-Tracked Dual Oscillator + Sub + Resonant Synth Filter */
    if (need_osc) {
        {   /* analog instability: two slow random walks (a few cents), updated every ~90 ms and smoothed over ~1.4 s */
            const float dc = s->params.drift_cents;
            if ((s->ui_anim_tick & 63u) == 0u) {
                s->drift_rng = s->drift_rng * 1664525u + 1013904223u;
                s->drift_tgt = dc * ((float)((s->drift_rng >> 8) & 0xFFFFu) * (1.0f / 32767.5f) - 1.0f);
                s->drift_rng = s->drift_rng * 1664525u + 1013904223u;
                s->drift_dif_tgt = dc * ((float)((s->drift_rng >> 8) & 0xFFFFu) * (1.0f / 32767.5f) - 1.0f);
            }
            s->drift_val += 0.01f * (s->drift_tgt - s->drift_val);
            s->drift_dif += 0.01f * (s->drift_dif_tgt - s->drift_dif);
            s->osc_eng.drift_diff_cents = s->drift_dif;
        }
        int osc_gate = (s->pitch.valid || s->adsr.stage != ICARUS_ENV_IDLE);
        float amp_env = adsr_buf[n >> 1] * mod.osc_amp_scale;
        icarus_osc_engine_process_block(&s->osc_eng,
                                       mod.glided_freq_hz,
                                       osc_gate,
                                       s->drift_val,
                                       mod.pulse_width_mod,
                                       amp_env,
                                       synth_buf,
                                       n);
        ICARUS_CP(0x08);
        AP_END(5); AP_BEGIN(6);
        icarus_filter_process_block(&s->synth_filter,
                                   synth_buf,
                                   n,
                                   adsr_buf,
                                   mod.glided_freq_hz,
                                   mod.filter_cutoff_oct);
    } else {
        memset(synth_buf, 0, (size_t)n * sizeof(float));
    }

#if (defined(PA_ICARUS_BISECT_SILENT) || defined(PA_ALPHA_BISECT_SILENT)) && !defined(PA_HOST_SIM)
    memset(synth_buf, 0, (size_t)n * sizeof(float));              /* BISECT build: the pitch search and the whole synth run, but the oscillator branch is muted */
#endif
    ICARUS_CP(0x09);
    AP_END(5); AP_END(6);
    AP_DEV_END(ap_t, 6); AP_DEV_RESTART(ap_t);
    AP_BEGIN(7);
    /* 6. Constant-Power 3-Way Mixer Stage (with >= 6 dB headroom before NAM) */
    s->mixer.pan = mod.stereo_pan_offset;
    icarus_mix_process_block(&s->mixer, cond_buf, analog_buf, synth_buf, out_L, out_R, n);

    ICARUS_CP(0x0A);
    AP_END(7); AP_BEGIN(8);
    /* Self-heal: if anything inside went non-finite (a filter pushed into instability, say) the poison would persist in its state and the output
     * would stay NaN for ever. One sample of each channel is enough to see it: reset every DSP state (the preset and its parameters stay) and
     * play silence for this block; the ramp-in of icarus_reset brings the voice back. */
    {
        const float chk = out_L[n - 1] + out_R[n - 1];
        if (!(fabsf(chk) < 64.0f)) {
            icarus_reset(s);
            s->in_gate.env = 0.0f;  s->in_gate.gain = 1.0f;
            s->out_gate.env = 0.0f; s->out_gate.gain = 1.0f;
            for (int i = 0; i < n; i++) { out_L[i] = 0.0f; out_R[i] = 0.0f; }
        }
    }

    /* 7. Optional Output Gate & Smooth Preset Crossfade */
    if (s->params.gate_enable) {                                  /* the output gate, once per block, ramped across it */
        float sum_abs = 0.0f;
        for (int i = 0; i < n; i++) {
            const float a = fabsf(out_L[i]), b = fabsf(out_R[i]);
            sum_abs += (b > a) ? b : a;
        }
        const float g0 = s->out_gate.gain;
        float lvl_out = sum_abs / (float)n;
        if (s->adsr.stage != ICARUS_ENV_IDLE && lvl_out < s->out_gate.open_lin * 1.2f)
            lvl_out = s->out_gate.open_lin * 1.2f;               /* the output gate follows the NOTE (the ADSR, release included), not only the level after the filter */
        const float g1 = icarus_gate_block(&s->out_gate, lvl_out, n);
        const float dg = (g1 - g0) / (float)n;
        float g = g0;
        for (int i = 0; i < n; i++) { g += dg; out_L[i] *= g; out_R[i] *= g; }
    }
    if (s->xfade_gain < 1.0f) {                                   /* the preset ramp-in: only for the first few blocks after a load */
        for (int i = 0; i < n; i++) {
            s->xfade_gain += s->xfade_step;
            if (s->xfade_gain > 1.0f) s->xfade_gain = 1.0f;
            out_L[i] *= s->xfade_gain;
            out_R[i] *= s->xfade_gain;
        }
    }

    /* the four branch meters (UI only): every 4th sample is plenty for a bar that decays by 0.92 per block; skipped when nobody is looking */
    const int ui_on = (s->ui_ttl > 0u);
    if (ui_on) s->ui_ttl--;
    float pk_gtr = 0.0f, pk_ana = 0.0f, pk_osc = 0.0f, pk_out = 0.0f;
    for (int i = 0; ui_on && i < n; i += 4) {
        const float ag = fabsf(cond_buf[i]);
        const float aa = fabsf(analog_buf[i]);
        const float ao = fabsf(synth_buf[i]);
        const float am = fabsf(out_L[i]);
        if (ag > pk_gtr) pk_gtr = ag;
        if (aa > pk_ana) pk_ana = aa;
        if (ao > pk_osc) pk_osc = ao;
        if (am > pk_out) pk_out = am;
    }

    AP_END(8); AP_BEGIN(9);
    /* 8. Real-time Visual UI & Wavelength Oscilloscope Telemetry Update */
    s->ui_anim_tick++;
    if (!ui_on) {                                              /* the Icarus screen is not open: no meters, scope or live cutoff to keep */
        AP_END(9);
        AP_DEV_END(ap_t, 7);
        return;
    }
    s->live_env_level = adsr_buf[n - 1];
    s->live_lfo_val   = s->lfo.out_bipolar;
    s->live_pwm       = clampf(s->params.pulse_width + mod.pulse_width_mod, 0.05f, 0.95f);
    s->live_vib_semi  = s->vibrato.current_cents * 0.01f;

    /* Compute instantaneous modulated filter cutoff frequency */
    {
        float track_oct = 0.0f;
        float track_f   = (mod.glided_freq_hz > 30.0f) ? mod.glided_freq_hz : s->pitch.frequency;
        if (track_f > 30.0f && s->params.key_tracking > 0.001f) {
            track_oct = s->params.key_tracking * fast_log2f_a(track_f * (1.0f / 220.0f));
        }
        float env_oct = s->params.env_amount * s->live_env_level * 4.0f;
        float tot_oct = clampf(env_oct + track_oct + mod.filter_cutoff_oct, -4.5f, 4.5f);
        s->live_cutoff_hz = clampf(s->params.cutoff_hz * fast_exp2f_a(tot_oct), 80.0f, 18000.0f);
    }

    /* Compute fine pitch deviation in cents (-50..+50) from nearest MIDI semitone */
    if (s->pitch.valid && s->pitch.frequency > 30.0f && s->pitch.midi_note >= 24u) {
        float exact_midi = 69.0f + 12.0f * fast_log2f_a(s->pitch.frequency * (1.0f / 440.0f));
        float diff_semi  = exact_midi - (float)s->pitch.midi_note;
        int   cents      = (int)(diff_semi * 100.0f);
        if (cents < -50) cents = -50;
        if (cents >  50) cents =  50;
        s->live_pitch_cents = (int8_t)cents;
    } else {
        s->live_pitch_cents = 0;
    }

    /* Smooth branch peak meters (fast attack, smooth decay) */
    s->live_peak_gtr = (pk_gtr > s->live_peak_gtr) ? pk_gtr : (s->live_peak_gtr * 0.92f);
    s->live_peak_ana = (pk_ana > s->live_peak_ana) ? pk_ana : (s->live_peak_ana * 0.92f);
    s->live_peak_osc = (pk_osc > s->live_peak_osc) ? pk_osc : (s->live_peak_osc * 0.92f);
    s->live_peak_out = (pk_out > s->live_peak_out) ? pk_out : (s->live_peak_out * 0.92f);

    /* Multi-block 8:1 decimated rising zero-crossing triggered 64-point wavelength oscilloscope
     * 64 points at 44100/8 = 5512.5 Hz spans 512 audio samples (11.6 ms) -> 2..3 full guitar cycles! */
    float norm_scale = (s->live_peak_out > 0.04f) ? clampf(27.0f / (s->live_peak_out + 0.03f), 20.0f, 110.0f) : 30.0f;
    float in_scale   = (s->live_peak_gtr > 0.04f) ? clampf(27.0f / (s->live_peak_gtr + 0.03f), 20.0f, 110.0f) : 30.0f;

    if (s->scope_hold_blocks > 0u && !s->scope_armed) {
        s->scope_hold_blocks--;
    }

    for (int i = 0; i < n; i++) {
        float cur_s = out_L[i];
        if (!s->scope_armed && s->scope_hold_blocks == 0u) {
            if (s->scope_prev_s <= 0.0f && cur_s > 0.0f) {
                s->scope_armed     = 1u;
                s->scope_wr_idx    = 0u;
                s->scope_decim_cnt = 0u;
            }
        }
        if (s->scope_armed) {
            if (s->scope_decim_cnt == 0u) {
                int v_out = (int)(cur_s * norm_scale);
                int v_in  = (int)(cond_buf[i] * in_scale);
                if (v_out < -31) v_out = -31;
                if (v_out >  31) v_out =  31;
                if (v_in  < -31) v_in  = -31;
                if (v_in  >  31) v_in  =  31;
                s->scope_accum[s->scope_wr_idx]    = (int8_t)v_out;
                s->scope_in_accum[s->scope_wr_idx] = (int8_t)v_in;
                s->scope_wr_idx++;
                if (s->scope_wr_idx >= 64u) {
                    memcpy(s->scope_wave,    s->scope_accum,    64u);
                    memcpy(s->scope_in_wave, s->scope_in_accum, 64u);
                    s->scope_armed       = 0u;
                    s->scope_hold_blocks = 2u;
                }
            }
            s->scope_decim_cnt = (uint8_t)((s->scope_decim_cnt + 1u) & 7u);
        }
        s->scope_prev_s = cur_s;
    }
    AP_END(9);
    AP_DEV_END(ap_t, 7);
}

void icarus_process_block_q21(icarus_state_t *s, const int32_t *in_mono_q21,
                             int32_t *out_L_q21, int32_t *out_R_q21, int n) {
    if (!s || !in_mono_q21 || !out_L_q21 || n <= 0) return;
    if (n > PA_BLOCK_N) n = PA_BLOCK_N;

    float in_f[PA_BLOCK_N];
    float out_l_f[PA_BLOCK_N];
    float out_r_f[PA_BLOCK_N];
    const float k_q21_to_f = 1.0f / (float)PA_Q21_ONE;
    const float k_f_to_q21 = (float)PA_Q21_ONE;

    for (int i = 0; i < n; i++) {
        in_f[i] = (float)in_mono_q21[i] * k_q21_to_f;
    }

    icarus_process_block_f32(s, in_f, out_l_f, out_r_f, n);

    /* float -> int32 with the clamp done in the float domain (the old path went through an int64 and a saturate call per sample) */
    const float lim = 2147483520.0f;                                 /* the largest float below 2^31 */
    const int stereo_out = (out_R_q21 && out_R_q21 != out_L_q21);
    for (int i = 0; i < n; i++) {
        float vl = out_l_f[i] * k_f_to_q21;
        vl = (vl > lim) ? lim : ((vl < -lim) ? -lim : vl);
        out_L_q21[i] = (int32_t)vl;
        if (stereo_out) {
            float vr = out_r_f[i] * k_f_to_q21;
            vr = (vr > lim) ? lim : ((vr < -lim) ? -lim : vr);
            out_R_q21[i] = (int32_t)vr;
        }
    }
}

void icarus_post_output_gate_q21(icarus_state_t *s, int32_t *buf_L_q21,
                                int32_t *buf_R_q21, int n) {
    if (!s || !s->params.gate_enable || !buf_L_q21 || n <= 0) return;
    /* Use the synthesizer's smoothed output gate gain to suppress post-NAM/IR idle hiss when note is silent */
    int32_t g_q21 = (int32_t)(s->out_gate.gain * (float)PA_Q21_ONE);
    if (g_q21 >= PA_Q21_ONE - 64) return;
    for (int i = 0; i < n; i++) {
        buf_L_q21[i] = q21_mul(buf_L_q21[i], g_q21);
        if (buf_R_q21 && buf_R_q21 != buf_L_q21) {
            buf_R_q21[i] = q21_mul(buf_R_q21[i], g_q21);
        }
    }
}
