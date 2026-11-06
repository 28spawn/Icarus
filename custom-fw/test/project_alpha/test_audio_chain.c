/* custom-fw/test/project_alpha/test_audio_chain.c — Integration tests for Project Alpha + NAM + IR + Gate */
#include "../../src/effects/project_alpha/alpha.h"
#include "../../include/pa_config.h"
#include "../../include/pa_fx.h"
#include "../../include/pa_model.h"
#include "../../include/pa_wavenet.h"
#include "../../src/dsp/fixedpoint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static pa_nam_model_t     s_nam;
static uint32_t           s_pool_live_bytes = 0;

void *pa_alloc(uint32_t n) {
    uint32_t *p = (uint32_t *)calloc(1, n + 8u);
    if (!p) return 0;
    p[0] = n;
    s_pool_live_bytes += n;
    return (void *)(p + 2);
}

void pa_free(void *q) {
    if (!q) return;
    uint32_t *p = ((uint32_t *)q) - 2;
    s_pool_live_bytes -= p[0];
    free(p);
}

static void amp_test_run(fx_node_t *nd, int32_t *buf, int n) {
    pa_nam_model_t *m = (pa_nam_model_t *)nd->state;
    if (m && m->active) {
        nam_gru_run_block(m, buf, n);
    }
}

static void amp_test_reset(fx_node_t *nd) {
    pa_nam_model_t *m = (pa_nam_model_t *)nd->state;
    if (m) pa_nam_reset(m);
}

static fx_node_t *create_test_amp_node(pa_nam_model_t *gru) {
    static fx_node_t node;
    memset(&node, 0, sizeof(node));
    nam_model_init(gru);
    node.active  = 1;
    node.frame   = PA_BLOCK_N;
    node.state   = gru;
    node.tag     = "amp";
    node.run     = amp_test_run;
    node.reset   = amp_test_reset;
    node.wet_q21 = PA_Q21_ONE;
    return &node;
}

static void generate_guitar_block_q21(int32_t *buf_L, int32_t *buf_R, float freq_hz, float amp, int n) {
    static double ph = 0.0;
    for (int i = 0; i < n; i++) {
        ph += (double)freq_hz / (double)PA_SAMPLE_RATE;
        if (ph >= 1.0) ph -= 1.0;
        float s = amp * (sinf((float)(2.0 * M_PI * ph))
                       + 0.45f * sinf((float)(4.0 * M_PI * ph + 0.3))
                       + 0.20f * sinf((float)(6.0 * M_PI * ph + 0.8)));
        int32_t v = (int32_t)(s * (float)PA_Q21_ONE);
        buf_L[i] = v;
        buf_R[i] = v;
    }
}

static void test_pool_allocation_and_all_12_presets(void) {
    pa_fx_chain_t chain;
    fx_node_t *amp_nd = create_test_amp_node(&s_nam);
    fx_node_t *cab_nd = fx_cab_ir_new();
    pa_fx_chain_init(&chain, amp_nd, cab_nd);

    /* Enable all heavy effects first, then enter Project Alpha synth mode and verify SRAM reclaim works */
    for (int s = 0; s < PA_ACTIVE_SLOT_COUNT; s++) {
        pa_fx_chain_set_slot_enabled_now(&chain, (pa_fx_slot_id_t)s, 1);
    }

    alpha_enter_synth_mode(&chain);
    alpha_state_t *alpha = alpha_get_active();
    assert(alpha != NULL);
    assert(alpha_is_active() == 1);
    printf("[test_audio_chain]sizeof(alpha_state_t) = %zu bytes\n", sizeof(alpha_state_t));

    /* Verify conventional stomp slots are disabled while AMP and CAB remain active */
    /* 5-block chain: DYN + SYNTH + AMP + CAB + EQ all run together */
    assert(chain.slots[PA_SLOT_SYNTH]->active == 1);
    assert(chain.slots[PA_SLOT_DYNAMICS]->active == 1);
    assert(chain.slots[PA_SLOT_EQ]->active == 1);
    assert(chain.slots[PA_SLOT_MOD] == NULL && chain.slots[PA_SLOT_DELAY] == NULL && chain.slots[PA_SLOT_REVERB] == NULL);
    assert(chain.slots[PA_SLOT_AMP]->active == 1);
    assert(chain.slots[PA_SLOT_CAB]->active == 1);

    /* Run all 12 Factory Presets through the full chain (Project Alpha -> NAM -> IR Cabinet -> Gate) */
    int32_t buf_L[PA_BLOCK_N], buf_R[PA_BLOCK_N];
    for (uint8_t p = 0; p < ALPHA_FACTORY_PRESET_COUNT; p++) {
        alpha_load_preset(alpha, p);
        alpha_sync_chain_routing(alpha, &chain);

        double energy = 0.0;
        int32_t max_step = 0;
        int32_t prev_s = 0;

        for (int b = 0; b < 65; b++) {
            generate_guitar_block_q21(buf_L, buf_R, 196.0f, 0.25f, PA_BLOCK_N);
            pa_fx_chain_run_stereo(&chain, buf_L, buf_R, PA_BLOCK_N);
            alpha_poll_background(alpha);

            for (int i = 0; i < PA_BLOCK_N; i++) {
                double fl = (double)buf_L[i] / (double)PA_Q21_ONE;
                double fr = (double)buf_R[i] / (double)PA_Q21_ONE;
                assert(isfinite(fl) && isfinite(fr));
                assert(fabs(fl) <= 1.05 && fabs(fr) <= 1.05);
                if (b >= 20) {
                    energy += fl * fl + fr * fr;
                }
                int32_t d = abs(buf_L[i] - prev_s);
                if (b > 0 && d > max_step) max_step = d;
                prev_s = buf_L[i];
            }
        }

        double rms = sqrt(energy / (double)(45 * PA_BLOCK_N * 2));
        printf("[test_audio_chain] Preset %2u %-16s (mode=%u): RMS=%.4f, peak_step=%.3f FS, live_fc=%.0f Hz, peak_out=%.3f\n",
               p + 1u, alpha->params.name, alpha->params.mode,
               rms, (double)max_step / (double)PA_Q21_ONE,
               (double)alpha->live_cutoff_hz, (double)alpha->live_peak_out);
        assert(rms > 0.015 && rms < 0.95);
        assert(alpha->live_cutoff_hz >= 80.0f && alpha->live_cutoff_hz <= 18000.0f);
        assert(alpha->live_peak_out > 0.01f);
        int scope_nz = 0;
        for (int k = 0; k < 64; k++) {
            if (alpha->scope_wave[k] != 0) scope_nz++;
        }
        assert(scope_nz > 8);
    }

    /* Test NAM & Cab bypass combinations in Project Alpha */
    alpha_load_preset(alpha, 1u); /* GR Brass (HYBRID) */
    for (int cfg = 0; cfg < 4; cfg++) {
        alpha->params.nam_enable = (cfg & 1) ? 1u : 0u;
        alpha->params.cab_enable = (cfg & 2) ? 1u : 0u;
        alpha_sync_chain_routing(alpha, &chain);
        pa_fx_chain_collect_garbage(&chain);

        double energy = 0.0;
        for (int b = 0; b < 30; b++) {
            generate_guitar_block_q21(buf_L, buf_R, 220.0f, 0.25f, PA_BLOCK_N);
            pa_fx_chain_run_stereo(&chain, buf_L, buf_R, PA_BLOCK_N);
            for (int i = 0; i < PA_BLOCK_N; i++) {
                double fl = (double)buf_L[i] / (double)PA_Q21_ONE;
                energy += fl * fl;
            }
        }
        double rms = sqrt(energy / (double)(30 * PA_BLOCK_N));
        printf("[test_audio_chain] Routing NAM=%u CAB=%u -> RMS=%.4f\n",
               alpha->params.nam_enable, alpha->params.cab_enable, rms);
        assert(rms > 0.02);
    }

    /* A LIVE bypass must stop Project Alpha immediately, even before the UI's garbage
     * collector frees the node state. The old standalone fallback in chain_condition()
     * keyed on alpha->enabled alone, so a bypassed SYNTH kept processing (and kept its
     * CPU cost) until collect_garbage() ran - and forever in hosts that never call it. */
    pa_fx_chain_set_slot_enabled(&chain, PA_SLOT_SYNTH, 0);
    for (int b = 0; b < 4; b++) {                       /* let the one-block fade finish */
        generate_guitar_block_q21(buf_L, buf_R, 220.0f, 0.25f, PA_BLOCK_N);
        pa_fx_chain_run_stereo(&chain, buf_L, buf_R, PA_BLOCK_N);
    }
    assert(chain.slots[PA_SLOT_SYNTH]->active == 0);
    const uint32_t alpha_ticks_bypassed = alpha->ui_anim_tick;
    const float    alpha_peak_bypassed  = alpha->live_peak_out;
    double bypass_energy = 0.0;
    for (int b = 0; b < 8; b++) {                       /* no collect_garbage() on purpose */
        generate_guitar_block_q21(buf_L, buf_R, 220.0f, 0.25f, PA_BLOCK_N);
        pa_fx_chain_run_stereo(&chain, buf_L, buf_R, PA_BLOCK_N);
        for (int i = 0; i < PA_BLOCK_N; i++) {
            double fl = (double)buf_L[i] / (double)PA_Q21_ONE;
            bypass_energy += fl * fl;
        }
    }
    assert(alpha->ui_anim_tick == alpha_ticks_bypassed);   /* the synth did not run */
    assert(alpha->live_peak_out == alpha_peak_bypassed);
    /* ...and the dry guitar still passes: the synth's post-output gate must not mute it. */
    assert(sqrt(bypass_energy / (double)(8 * PA_BLOCK_N)) > 0.02);

    /* Exit synth mode and verify clean teardown */
    alpha_exit_synth_mode(&chain);
    assert(chain.slots[PA_SLOT_SYNTH]->active == 0);   /* bypassed above */
    pa_fx_chain_collect_garbage(&chain);
    assert(alpha_is_active() == 0);
}

int main(void) {
    test_pool_allocation_and_all_12_presets();
    printf("ALL AUDIO CHAIN INTEGRATION TESTS PASSED\n");
    return 0;
}
