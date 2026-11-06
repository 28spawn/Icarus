/* custom-fw/src/effects/project_alpha/alpha.c — Project Alpha Flagship Guitar Synthesis System */
#include "alpha.h"
#include "pa_config.h"
#include "../../dsp/fixedpoint.h"
#include <math.h>
#include <string.h>

extern void *pa_alloc(uint32_t);
extern void  pa_free(void *);

#define ALPHA_FS ((float)PA_SAMPLE_RATE)

static alpha_state_t *s_alpha_active = 0;

static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static inline float db_to_lin(float db) {
    return powf(10.0f, db * 0.05f);
}

static void alpha_gate_init(alpha_gate_t *g, float thresh_db, float att_ms, float hold_ms, float rel_ms, float hyst_db) {
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->threshold_db  = thresh_db;
    g->attack_ms     = att_ms;
    g->hold_ms       = hold_ms;
    g->release_ms    = rel_ms;
    g->hysteresis_db = hyst_db;
    g->open_lin      = db_to_lin(thresh_db);
    g->close_lin     = db_to_lin(thresh_db - hyst_db);
    g->att_coeff     = 1.0f - expf(-1000.0f / (clampf(att_ms, 0.2f, 100.0f) * ALPHA_FS));
    g->rel_coeff     = 1.0f - expf(-1000.0f / (clampf(rel_ms, 5.0f, 2000.0f) * ALPHA_FS));
    g->hold_samples  = (uint32_t)(clampf(hold_ms, 0.0f, 1000.0f) * 0.001f * ALPHA_FS);
}

static inline float alpha_gate_step(alpha_gate_t *g, float abs_in) {
    g->env += ((abs_in > g->env) ? 0.05f : 0.002f) * (abs_in - g->env);
    if (!g->open) {
        if (g->env >= g->open_lin) {
            g->open     = 1u;
            g->hold_cnt = g->hold_samples;
        }
    } else {
        if (g->env >= g->close_lin) {
            g->hold_cnt = g->hold_samples;
        } else if (g->hold_cnt > 0u) {
            g->hold_cnt--;
        } else {
            g->open = 0u;
        }
    }
    float target = g->open ? 1.0f : 0.0f;
    float coeff  = g->open ? g->att_coeff : g->rel_coeff;
    g->gain += coeff * (target - g->gain);
    return g->gain;
}

/* ============================================================================
 * 12 Factory Presets (Section 13 — Stored in XIP Flash .rodata)
 * ============================================================================ */
static const alpha_preset_t k_factory_presets[ALPHA_FACTORY_PRESET_COUNT] = {
    /* 1. Metheny Lead — Oscillator: Bright analog GR-300 style lead with legato glide & delayed vibrato */
    {
        .name            = "Metheny Lead",
        .mode            = ALPHA_MODE_OSCILLATOR,
        .dist_mode       = ALPHA_DIST_HEXA,
        .analog_drive    = 0.45f,
        .input_gain      = 1.15f,
        .pre_emphasis    = 1u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 4,
        .level_a         = 0.90f,
        .level_b         = 0.55f,
        .detune_cents    = 7.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.35f,
        .filter_mode     = ALPHA_FILTER_LP24,
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
        .lfo_wave        = ALPHA_LFO_SINE,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
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
        .mode            = ALPHA_MODE_HYBRID,
        .dist_mode       = ALPHA_DIST_HEXA,
        .analog_drive    = 0.55f,
        .input_gain      = 1.10f,
        .pre_emphasis    = 1u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 0,
        .level_a         = 0.85f,
        .level_b         = 0.80f,
        .detune_cents    = 14.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OCT1,
        .sub_level       = 0.22f,
        .osc_mix         = 0.48f,
        .filter_mode     = ALPHA_FILTER_LP24,
        .cutoff_hz       = 680.0f,
        .resonance       = 0.54f,
        .env_amount      = 0.74f,
        .key_tracking    = 0.65f,
        .filter_sat      = 0.42f,
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
        .lfo_wave        = ALPHA_LFO_TRI,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
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
        .mode            = ALPHA_MODE_OSCILLATOR,
        .dist_mode       = ALPHA_DIST_SOFT_CLIP,
        .analog_drive    = 0.35f,
        .input_gain      = 1.05f,
        .pre_emphasis    = 0u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = -3,
        .level_a         = 0.85f,
        .level_b         = 0.65f,
        .detune_cents    = 11.0f,
        .pulse_width     = 0.42f,
        .sub_mode        = ALPHA_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.42f,
        .filter_mode     = ALPHA_FILTER_LP24,
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
        .lfo_wave        = ALPHA_LFO_TRI,
        .lfo_dest        = ALPHA_LFO_DEST_PULSE_WIDTH,
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
        .mode            = ALPHA_MODE_OSCILLATOR,
        .dist_mode       = ALPHA_DIST_HARD_CLIP,
        .analog_drive    = 0.50f,
        .input_gain      = 1.20f,
        .pre_emphasis    = 0u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_SQUARE,
        .pitch_a         = -12,
        .pitch_b         = 0,
        .fine_tune       = 0,
        .level_a         = 0.90f,
        .level_b         = 0.55f,
        .detune_cents    = 5.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OCT1,
        .sub_level       = 0.75f,
        .osc_mix         = 0.35f,
        .filter_mode     = ALPHA_FILTER_LP24,
        .cutoff_hz       = 360.0f,
        .resonance       = 0.58f,
        .env_amount      = 0.68f,
        .key_tracking    = 0.50f,
        .filter_sat      = 0.48f,
        .env_attack_ms   = 3.0f,
        .env_decay_ms    = 190.0f,
        .env_sustain     = 0.25f,
        .env_release_ms  = 120.0f,
        .glide_ms        = 28.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 4.5f,
        .vib_depth_cents = 4.0f,
        .vib_delay_ms    = 500.0f,
        .lfo_rate_hz     = 0.5f,
        .lfo_depth       = 0.00f,
        .lfo_wave        = ALPHA_LFO_SINE,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
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
        .mode            = ALPHA_MODE_OSCILLATOR,
        .dist_mode       = ALPHA_DIST_SOFT_CLIP,
        .analog_drive    = 0.40f,
        .input_gain      = 1.10f,
        .pre_emphasis    = 1u,
        .wave_a          = ALPHA_WAVE_TRIANGLE,
        .wave_b          = ALPHA_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 2,
        .level_a         = 0.80f,
        .level_b         = 0.75f,
        .detune_cents    = 9.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OCT1,
        .sub_level       = 0.25f,
        .osc_mix         = 0.50f,
        .filter_mode     = ALPHA_FILTER_LP24,
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
        .lfo_wave        = ALPHA_LFO_SINE,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
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
        .mode            = ALPHA_MODE_ANALOG,
        .dist_mode       = ALPHA_DIST_FUZZ,
        .analog_drive    = 0.82f,
        .input_gain      = 1.35f,
        .pre_emphasis    = 1u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = -12,
        .fine_tune       = 0,
        .level_a         = 0.70f,
        .level_b         = 0.50f,
        .detune_cents    = 8.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.40f,
        .filter_mode     = ALPHA_FILTER_LP12,
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
        .lfo_wave        = ALPHA_LFO_TRI,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
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
        .mode            = ALPHA_MODE_HYBRID,
        .dist_mode       = ALPHA_DIST_HEXA,
        .analog_drive    = 0.68f,
        .input_gain      = 1.20f,
        .pre_emphasis    = 1u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 12,
        .fine_tune       = 0,
        .level_a         = 0.80f,
        .level_b         = 0.45f,
        .detune_cents    = 12.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OCT1,
        .sub_level       = 0.30f,
        .osc_mix         = 0.38f,
        .filter_mode     = ALPHA_FILTER_LP24,
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
        .lfo_wave        = ALPHA_LFO_SINE,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
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
        .mode            = ALPHA_MODE_HYBRID,
        .dist_mode       = ALPHA_DIST_SOFT_CLIP,
        .analog_drive    = 0.30f,
        .input_gain      = 1.05f,
        .pre_emphasis    = 0u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_TRIANGLE,
        .pitch_a         = 0,
        .pitch_b         = 12,
        .fine_tune       = 0,
        .level_a         = 0.75f,
        .level_b         = 0.65f,
        .detune_cents    = 18.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OCT1,
        .sub_level       = 0.35f,
        .osc_mix         = 0.48f,
        .filter_mode     = ALPHA_FILTER_LP24,
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
        .lfo_wave        = ALPHA_LFO_SINE,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
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
        .mode            = ALPHA_MODE_OSCILLATOR,
        .dist_mode       = ALPHA_DIST_SOFT_CLIP,
        .analog_drive    = 0.35f,
        .input_gain      = 1.10f,
        .pre_emphasis    = 1u,
        .wave_a          = ALPHA_WAVE_SQUARE,
        .wave_b          = ALPHA_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 0,
        .level_a         = 0.88f,
        .level_b         = 0.60f,
        .detune_cents    = 8.0f,
        .pulse_width     = 0.36f,
        .sub_mode        = ALPHA_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.40f,
        .filter_mode     = ALPHA_FILTER_LP24,
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
        .lfo_wave        = ALPHA_LFO_TRI,
        .lfo_dest        = ALPHA_LFO_DEST_PULSE_WIDTH,
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
        .mode            = ALPHA_MODE_ANALOG,
        .dist_mode       = ALPHA_DIST_FOLDBACK,
        .analog_drive    = 0.64f,
        .input_gain      = 1.25f,
        .pre_emphasis    = 1u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = -12,
        .fine_tune       = 0,
        .level_a         = 0.80f,
        .level_b         = 0.50f,
        .detune_cents    = 10.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OCT1,
        .sub_level       = 0.40f,
        .osc_mix         = 0.40f,
        .filter_mode     = ALPHA_FILTER_LP24,
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
        .lfo_wave        = ALPHA_LFO_SH,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.12f,
        .analog_level    = 0.90f,
        .synth_level     = 0.00f,
        .master_level    = 0.85f,
        .stereo_width    = 0.40f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 11. Octave Machine — Hybrid: Guitar + Upper & Sub Octave Synthesizer */
    {
        .name            = "Octave Machine",
        .mode            = ALPHA_MODE_HYBRID,
        .dist_mode       = ALPHA_DIST_HARD_CLIP,
        .analog_drive    = 0.58f,
        .input_gain      = 1.15f,
        .pre_emphasis    = 1u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_SQUARE,
        .pitch_a         = 0,
        .pitch_b         = 12,
        .fine_tune       = 0,
        .level_a         = 0.75f,
        .level_b         = 0.70f,
        .detune_cents    = 6.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OCT1,
        .sub_level       = 0.70f,
        .osc_mix         = 0.48f,
        .filter_mode     = ALPHA_FILTER_LP24,
        .cutoff_hz       = 2200.0f,
        .resonance       = 0.36f,
        .env_amount      = 0.45f,
        .key_tracking    = 0.65f,
        .filter_sat      = 0.38f,
        .env_attack_ms   = 5.0f,
        .env_decay_ms    = 300.0f,
        .env_sustain     = 0.60f,
        .env_release_ms  = 180.0f,
        .glide_ms        = 15.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.2f,
        .vib_depth_cents = 8.0f,
        .vib_delay_ms    = 300.0f,
        .lfo_rate_hz     = 1.0f,
        .lfo_depth       = 0.08f,
        .lfo_wave        = ALPHA_LFO_SINE,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
        .guitar_level    = 0.45f,
        .analog_level    = 0.55f,
        .synth_level     = 0.80f,
        .master_level    = 0.85f,
        .stereo_width    = 0.45f,
        .nam_enable      = 1u,
        .cab_enable      = 1u,
        .gate_enable     = 1u
    },
    /* 12. Pure GR — Analog: GR-inspired direct polyphonic Hexa + resonant filter */
    {
        .name            = "Pure GR",
        .mode            = ALPHA_MODE_ANALOG,
        .dist_mode       = ALPHA_DIST_HEXA,
        .analog_drive    = 0.74f,
        .input_gain      = 1.25f,
        .pre_emphasis    = 1u,
        .wave_a          = ALPHA_WAVE_SAW,
        .wave_b          = ALPHA_WAVE_SAW,
        .pitch_a         = 0,
        .pitch_b         = 0,
        .fine_tune       = 0,
        .level_a         = 0.80f,
        .level_b         = 0.40f,
        .detune_cents    = 6.0f,
        .pulse_width     = 0.50f,
        .sub_mode        = ALPHA_SUB_OFF,
        .sub_level       = 0.00f,
        .osc_mix         = 0.35f,
        .filter_mode     = ALPHA_FILTER_LP24,
        .cutoff_hz       = 980.0f,
        .resonance       = 0.60f,
        .env_amount      = 0.68f,
        .key_tracking    = 0.50f,
        .filter_sat      = 0.45f,
        .env_attack_ms   = 8.0f,
        .env_decay_ms    = 380.0f,
        .env_sustain     = 0.58f,
        .env_release_ms  = 250.0f,
        .glide_ms        = 0.0f,
        .glide_legato    = 1u,
        .vib_rate_hz     = 5.5f,
        .vib_depth_cents = 0.0f,
        .vib_delay_ms    = 300.0f,
        .lfo_rate_hz     = 0.75f,
        .lfo_depth       = 0.10f,
        .lfo_wave        = ALPHA_LFO_SINE,
        .lfo_dest        = ALPHA_LFO_DEST_FILTER_CUTOFF,
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

const alpha_preset_t *alpha_get_factory_preset(uint8_t index) {
    if (index >= ALPHA_FACTORY_PRESET_COUNT) index = 0u;
    return &k_factory_presets[index];
}

void alpha_apply_params(alpha_state_t *s) {
    if (!s) return;
    const alpha_preset_t *p = &s->params;

    /* Engine A: Analog conditioning, distortion & filter */
    alpha_analog_set_conditioning(&s->analog, p->input_gain, p->pre_emphasis, 1u);
    alpha_analog_set_distortion(&s->analog, p->dist_mode, p->analog_drive);
    alpha_filter_set_params(&s->analog.filter, p->cutoff_hz, p->resonance,
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

    alpha_filter_set_params(&s->synth_filter, p->cutoff_hz, p->resonance,
                            p->env_amount, p->key_tracking, p->filter_sat, p->filter_mode);

    /* Envelopes */
    alpha_env_follower_set_times(&s->env_follower,
                                 clampf(p->env_attack_ms * 0.25f + 2.0f, 1.5f, 80.0f),
                                 clampf(p->env_release_ms * 0.50f + 40.0f, 30.0f, 600.0f));
    alpha_env_set_params(&s->adsr, p->env_attack_ms, p->env_decay_ms,
                         p->env_sustain, p->env_release_ms);

    /* Glide, Vibrato & Modulation LFO */
    alpha_glide_set_params(&s->glide, p->glide_ms,
                           p->glide_legato ? ALPHA_GLIDE_LEGATO : ALPHA_GLIDE_ALWAYS,
                           ALPHA_GLIDE_CURVE_EXP, ALPHA_GLIDE_DIR_BOTH, 0.85f);
    alpha_vibrato_set_params(&s->vibrato, p->vib_rate_hz, p->vib_depth_cents,
                             p->vib_delay_ms, 300.0f, ALPHA_VIB_SINE, 0.35f);
    alpha_lfo_set_params(&s->lfo, p->lfo_rate_hz, p->lfo_depth,
                         (alpha_lfo_wave_t)p->lfo_wave, (alpha_lfo_dest_t)p->lfo_dest);

    /* Mixer */
    alpha_mix_set_params(&s->mixer, p->mode, p->guitar_level, p->analog_level,
                         p->synth_level, p->master_level, 0.0f, p->stereo_width);

    if (s->live_cutoff_hz < 80.0f) {
        s->live_cutoff_hz = p->cutoff_hz;
    }
    if (s->live_pwm < 0.05f) {
        s->live_pwm = p->pulse_width;
    }
}

void alpha_init(alpha_state_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    alpha_env_follower_init(&s->env_follower);
    alpha_env_init(&s->adsr);
    alpha_analog_init(&s->analog);
    alpha_osc_engine_init(&s->osc_eng);
    alpha_filter_init(&s->synth_filter);
    alpha_pitch_init(&s->pitch);
    alpha_glide_init(&s->glide);
    alpha_vibrato_init(&s->vibrato);
    alpha_lfo_init(&s->lfo);
    alpha_mix_init(&s->mixer);

    alpha_gate_init(&s->in_gate,  -56.0f, 1.5f,  45.0f, 90.0f,  5.0f);
    alpha_gate_init(&s->out_gate, -62.0f, 2.0f,  60.0f, 140.0f, 6.0f);

    s->xfade_gain = 1.0f;
    s->xfade_step = 0.0f;
    s->enabled    = 1u;

    alpha_load_preset(s, 0u);
}

void alpha_reset(alpha_state_t *s) {
    if (!s) return;
    alpha_env_follower_reset(&s->env_follower);
    alpha_env_reset(&s->adsr);
    alpha_analog_reset(&s->analog);
    alpha_osc_engine_reset(&s->osc_eng);
    alpha_filter_reset(&s->synth_filter);
    alpha_pitch_reset(&s->pitch);
    alpha_mix_reset(&s->mixer);
    s->xfade_gain = 0.0f;
    s->xfade_step = 1.0f / (float)(4 * PA_BLOCK_N); /* 4-block smooth ramp-in */
}

void alpha_load_preset(alpha_state_t *s, uint8_t index) {
    if (!s) return;
    if (index >= ALPHA_FACTORY_PRESET_COUNT) index = 0u;
    s->preset_index = index;
    s->params       = *alpha_get_factory_preset(index);
    alpha_apply_params(s);
    /* Trigger a brief click-free ramp-in on preset switch */
    s->xfade_gain = 0.15f;
    s->xfade_step = (1.0f - 0.15f) / (float)(2 * PA_BLOCK_N);
}

void alpha_set_mode(alpha_state_t *s, uint8_t mode) {
    if (!s) return;
    if (mode >= ALPHA_MODE_COUNT) mode = ALPHA_MODE_HYBRID;
    s->params.mode = mode;
    /* Ensure sensible default levels if switching into a mode whose level was 0 */
    if ((mode == ALPHA_MODE_OSCILLATOR || mode == ALPHA_MODE_HYBRID) && s->params.synth_level < 0.15f) {
        s->params.synth_level = 0.80f;
    }
    if ((mode == ALPHA_MODE_ANALOG || mode == ALPHA_MODE_HYBRID) && s->params.analog_level < 0.15f) {
        s->params.analog_level = 0.75f;
    }
    if (mode == ALPHA_MODE_GUITAR && s->params.guitar_level < 0.25f) {
        s->params.guitar_level = 0.85f;
    }
    alpha_apply_params(s);
}

alpha_state_t *alpha_create(void) {
    alpha_state_t *s = (alpha_state_t *)pa_alloc((uint32_t)sizeof(alpha_state_t));
    if (!s) return 0;
    alpha_init(s);
    s_alpha_active = s;
    return s;
}

void alpha_destroy(alpha_state_t *s) {
    if (!s) return;
    if (s_alpha_active == s) s_alpha_active = 0;
    pa_free(s);
}

alpha_state_t *alpha_get_active(void) {
    return s_alpha_active;
}

void alpha_set_active_enable(int enable) {
    if (enable) {
        if (!s_alpha_active) {
            alpha_create();
        } else {
            s_alpha_active->enabled = 1u;
        }
    } else {
        if (s_alpha_active) {
            alpha_destroy(s_alpha_active);
        }
    }
}

int alpha_is_active(void) {
    return (s_alpha_active && s_alpha_active->enabled) ? 1 : 0;
}

void alpha_sync_chain_routing(alpha_state_t *s, pa_fx_chain_t *chain) {
    if (!s || !chain) return;
    /* In the focused 5-block chain (DYN -> SYNTH -> AMP -> CAB -> EQ), DYN and EQ
     * can run simultaneously alongside SYNTH, AMP, and CAB. Only sync NAM & CAB
     * bypass when modified from the Project Alpha Stage Integration menu. */
    pa_fx_chain_set_slot_enabled(chain, PA_SLOT_AMP, s->params.nam_enable ? 1 : 0);
    pa_fx_chain_set_slot_enabled(chain, PA_SLOT_CAB, s->params.cab_enable ? 1 : 0);
}

void alpha_enter_synth_mode(pa_fx_chain_t *chain) {
    if (chain) {
        pa_fx_chain_set_slot_enabled(chain, PA_SLOT_SYNTH, 1);
    } else {
        alpha_set_active_enable(1);
    }
    if (s_alpha_active && chain) {
        s_alpha_active->params.nam_enable = (chain->slots[PA_SLOT_AMP] && chain->slots[PA_SLOT_AMP]->active) ? 1u : 0u;
        s_alpha_active->params.cab_enable = (chain->slots[PA_SLOT_CAB] && chain->slots[PA_SLOT_CAB]->active) ? 1u : 0u;
    }
}

void alpha_exit_synth_mode(pa_fx_chain_t *chain) {
    /* Leaving the Project Alpha UI returns to the Home Screen while keeping the
     * SYNTH block active in the signal chain (use Stomp/Menu to bypass SYNTH if desired). */
    (void)chain;
}

/* ===================================================================== */
/* First-Class FX Node Interface (fx_synth_new for PA_SLOT_SYNTH)        */
/* ===================================================================== */
static void fx_synth_create(fx_node_t *nd) {
    if (!nd) return;
    if (nd->state) {
        s_alpha_active = (alpha_state_t *)nd->state;
        s_alpha_active->enabled = 1u;
        return;
    }
    alpha_state_t *s = alpha_create();
    nd->state = s;
}

static void fx_synth_destroy(fx_node_t *nd) {
    if (!nd || !nd->state) return;
    alpha_destroy((alpha_state_t *)nd->state);
    nd->state = 0;
}

static void fx_synth_reset(fx_node_t *nd) {
    if (!nd || !nd->state) return;
    alpha_reset((alpha_state_t *)nd->state);
}

static void fx_synth_run_mono(fx_node_t *nd, int32_t *buf, int n) {
    if (!nd || !nd->state) return;
    alpha_state_t *s = (alpha_state_t *)nd->state;
    if (!s->enabled) return;
    alpha_process_block_q21(s, buf, buf, 0, n);
}

static void fx_synth_run_stereo(fx_node_t *nd, int32_t *buf_L, int32_t *buf_R, int n) {
    if (!nd || !nd->state) return;
    alpha_state_t *s = (alpha_state_t *)nd->state;
    if (!s->enabled) return;
    alpha_process_block_q21(s, buf_L, buf_L, buf_R, n);
}

static void fx_synth_set_param(fx_node_t *nd, int id, int32_t val) {
    if (!nd || !nd->state) return;
    alpha_state_t *s = (alpha_state_t *)nd->state;
    /* id == 10 (or legacy id == 3 if large/small): explicit preset/model selection */
    if (id == 10) {
        uint8_t p_idx = (uint8_t)((val < 0 ? 0 : val) % ALPHA_FACTORY_PRESET_COUNT);
        if (s->preset_index != p_idx) {
            alpha_load_preset(s, p_idx);
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
    alpha_apply_params(s);
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

void alpha_poll_background(alpha_state_t *s) {
    if (!s || !s->enabled) return;
    if (s->params.mode == ALPHA_MODE_OSCILLATOR || s->params.mode == ALPHA_MODE_HYBRID) {
        alpha_pitch_poll_background(&s->pitch);
    }
}

void alpha_process_block_f32(alpha_state_t *s, const float *in_mono,
                             float *out_L, float *out_R, int n) {
    if (!s || !in_mono || !out_L || !out_R || n <= 0) return;

    float gated_in[PA_BLOCK_N];
    float cond_buf[PA_BLOCK_N];
    float env_buf[PA_BLOCK_N];
    float analog_buf[PA_BLOCK_N];
    float synth_buf[PA_BLOCK_N];
    float adsr_buf[PA_BLOCK_N];

    if (n > PA_BLOCK_N) n = PA_BLOCK_N;

    /* 1. Optional Input Synth Noise Gate */
    if (s->params.gate_enable) {
        for (int i = 0; i < n; i++) {
            float g = alpha_gate_step(&s->in_gate, fabsf(in_mono[i]));
            gated_in[i] = in_mono[i] * g;
        }
    } else {
        memcpy(gated_in, in_mono, (size_t)n * sizeof(float));
    }

    /* 2. Input Conditioning & Envelope Follower (Always runs — feeds Direct, Analog & Expression) */
    alpha_input_condition_block(&s->analog, &s->env_follower, gated_in, cond_buf, env_buf, n);
    float block_env = env_buf[n - 1];

    /* 3. Pitch Analysis & Modulation Frame */
    const uint8_t mode = s->params.mode;
    const int need_osc = (mode == ALPHA_MODE_OSCILLATOR || mode == ALPHA_MODE_HYBRID ||
                          (mode == ALPHA_MODE_GUITAR && s->params.synth_level > 0.01f));
    const int need_ana = (mode == ALPHA_MODE_ANALOG || mode == ALPHA_MODE_HYBRID ||
                          (mode == ALPHA_MODE_GUITAR && s->params.analog_level > 0.01f));

    if (need_osc) {
        alpha_pitch_feed_block(&s->pitch, cond_buf, n);
    } else {
        /* Even in pure Analog mode, step LFO & ADSR from picking envelope onset */
        s->pitch.onset   = s->env_follower.onset_trig;
        s->pitch.legato  = 0u;
        s->pitch.valid   = (block_env > 0.015f) ? 1u : 0u;
        s->pitch.release = (!s->pitch.valid && s->adsr.gate) ? 1u : 0u;
    }

    /* Trigger / release ADSR envelope from pitch/onset detector */
    if (s->pitch.onset || (s->env_follower.onset_trig && !s->adsr.gate)) {
        float vel = clampf(s->env_follower.fast_level * 2.2f, 0.35f, 1.0f);
        alpha_env_trigger(&s->adsr, vel);
    } else if (s->pitch.release || (!s->pitch.valid && s->adsr.gate)) {
        alpha_env_release(&s->adsr);
    }

    for (int i = 0; i < n; i++) {
        float a_env = alpha_env_step(&s->adsr);
        /* Blend ADSR envelope with natural guitar picking envelope so dynamics remain alive */
        adsr_buf[i] = clampf(0.65f * a_env + 0.55f * env_buf[i], 0.0f, 1.0f);
    }

    alpha_mod_frame_t mod;
    alpha_compute_mod_frame(&mod,
                            &s->glide,
                            &s->vibrato,
                            &s->lfo,
                            s->pitch.frequency,
                            s->pitch.onset,
                            s->pitch.legato,
                            block_env,
                            s->adsr.velocity,
                            n);

    /* 4. Engine A — Polyphonic Analog Processing */
    if (need_ana) {
        alpha_analog_process_block(&s->analog, cond_buf, adsr_buf, analog_buf, n,
                                   s->pitch.frequency, mod.filter_cutoff_oct);
    } else {
        memset(analog_buf, 0, (size_t)n * sizeof(float));
    }

    /* 5. Engine B — Pitch-Tracked Dual Oscillator + Sub + Resonant Synth Filter */
    if (need_osc) {
        int osc_gate = (s->pitch.valid || s->adsr.stage != ALPHA_ENV_IDLE);
        float amp_env = adsr_buf[n >> 1] * mod.osc_amp_scale;
        alpha_osc_engine_process_block(&s->osc_eng,
                                       mod.glided_freq_hz,
                                       osc_gate,
                                       0.0f,
                                       mod.pulse_width_mod,
                                       amp_env,
                                       synth_buf,
                                       n);
        alpha_filter_process_block(&s->synth_filter,
                                   synth_buf,
                                   n,
                                   adsr_buf,
                                   mod.glided_freq_hz,
                                   mod.filter_cutoff_oct);
    } else {
        memset(synth_buf, 0, (size_t)n * sizeof(float));
    }

    /* 6. Constant-Power 3-Way Mixer Stage (with >= 6 dB headroom before NAM) */
    s->mixer.pan = mod.stereo_pan_offset;
    alpha_mix_process_block(&s->mixer, cond_buf, analog_buf, synth_buf, out_L, out_R, n);

    /* 7. Optional Output Gate & Smooth Preset Crossfade */
    float pk_gtr = 0.0f, pk_ana = 0.0f, pk_osc = 0.0f, pk_out = 0.0f;
    for (int i = 0; i < n; i++) {
        float g = 1.0f;
        if (s->params.gate_enable) {
            float pk = fabsf(out_L[i]);
            float pkr = fabsf(out_R[i]);
            if (pkr > pk) pk = pkr;
            g = alpha_gate_step(&s->out_gate, pk);
        }
        if (s->xfade_gain < 1.0f) {
            s->xfade_gain += s->xfade_step;
            if (s->xfade_gain > 1.0f) s->xfade_gain = 1.0f;
            g *= s->xfade_gain;
        }
        out_L[i] *= g;
        out_R[i] *= g;

        float ag = fabsf(cond_buf[i]);
        float aa = fabsf(analog_buf[i]);
        float ao = fabsf(synth_buf[i]);
        float am = fabsf(out_L[i]);
        if (ag > pk_gtr) pk_gtr = ag;
        if (aa > pk_ana) pk_ana = aa;
        if (ao > pk_osc) pk_osc = ao;
        if (am > pk_out) pk_out = am;
    }

    /* 8. Real-time Visual UI & Wavelength Oscilloscope Telemetry Update */
    s->ui_anim_tick++;
    s->live_env_level = adsr_buf[n - 1];
    s->live_lfo_val   = s->lfo.out_bipolar;
    s->live_pwm       = clampf(s->params.pulse_width + mod.pulse_width_mod, 0.05f, 0.95f);
    s->live_vib_semi  = s->vibrato.current_cents * 0.01f;

    /* Compute instantaneous modulated filter cutoff frequency */
    {
        float track_oct = 0.0f;
        float track_f   = (mod.glided_freq_hz > 30.0f) ? mod.glided_freq_hz : s->pitch.frequency;
        if (track_f > 30.0f && s->params.key_tracking > 0.001f) {
            track_oct = s->params.key_tracking * log2f(track_f / 220.0f);
        }
        float env_oct = s->params.env_amount * s->live_env_level * 4.0f;
        float tot_oct = clampf(env_oct + track_oct + mod.filter_cutoff_oct, -4.5f, 4.5f);
        s->live_cutoff_hz = clampf(s->params.cutoff_hz * powf(2.0f, tot_oct), 80.0f, 18000.0f);
    }

    /* Compute fine pitch deviation in cents (-50..+50) from nearest MIDI semitone */
    if (s->pitch.valid && s->pitch.frequency > 30.0f && s->pitch.midi_note >= 24u) {
        float exact_midi = 69.0f + 12.0f * log2f(s->pitch.frequency / 440.0f);
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
}

void alpha_process_block_q21(alpha_state_t *s, const int32_t *in_mono_q21,
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

    alpha_process_block_f32(s, in_f, out_l_f, out_r_f, n);

    for (int i = 0; i < n; i++) {
        out_L_q21[i] = sat32((int64_t)(out_l_f[i] * k_f_to_q21));
        if (out_R_q21 && out_R_q21 != out_L_q21) {
            out_R_q21[i] = sat32((int64_t)(out_r_f[i] * k_f_to_q21));
        }
    }
}

void alpha_post_output_gate_q21(alpha_state_t *s, int32_t *buf_L_q21,
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
