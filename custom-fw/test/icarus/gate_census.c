/* custom-fw/test/icarus/gate_census.c — libm census for the synth gate: libm calls per 64-sample block in steady state, per preset.
 * Built with -include gate_countlib.h so every synth source counts its calls. Output: one JSON object on stdout. */
#include "icarus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

unsigned long g_lc[C_N];
void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p) { free(p); }
void  pa_fx_chain_set_slot_enabled(pa_fx_chain_t *c, pa_fx_slot_id_t s, int on) { (void)c; (void)s; (void)on; }

static const char *nm[C_N] = { "floorf", "sinf", "cosf", "expf", "logf", "log2f", "powf", "tanhf", "fmodf", "atan2f", "tanf" };

int main(void) {
    printf("{\"presets\": [\n");
    for (int preset = 0; preset < (int)ICARUS_FACTORY_PRESET_COUNT; preset++) {
        icarus_state_t *s = icarus_create();
        icarus_load_preset(s, (uint8_t)preset);
        s->enabled = 1;
        double ph = 0.0;
        float in[64], L[64], R[64];
        const int warm = 500, blocks = 500;
        unsigned long acc[C_N], poll[C_N];
        memset(acc, 0, sizeof(acc)); memset(poll, 0, sizeof(poll));
        for (int b = 0; b < warm + blocks; b++) {
            for (int i = 0; i < 64; i++) {
                ph += 196.0 / 44100.0; if (ph >= 1.0) ph -= 1.0;
                in[i] = 0.2f * (float)__builtin_sin(6.2831853 * ph) + 0.1f * (float)__builtin_sin(12.566371 * ph);
            }
            unsigned long before[C_N], mid[C_N];
            memcpy(before, g_lc, sizeof(before));
            icarus_process_block_f32(s, in, L, R, 64);          /* the audio-interrupt side */
            memcpy(mid, g_lc, sizeof(mid));
            icarus_poll_background(s);                           /* the main-loop side, counted apart */
            if (b >= warm) for (int k = 0; k < C_N; k++) { acc[k] += mid[k] - before[k]; poll[k] += g_lc[k] - mid[k]; }
        }
        printf("  {\"preset\": %d, \"name\": \"%s\", \"calls_per_block\": {", preset + 1, s->params.name);
        int first = 1;
        for (int k = 0; k < C_N; k++) { printf("%s\"%s\": %.2f", first ? "" : ", ", nm[k], (double)acc[k] / blocks); first = 0; }
        printf("}, \"poll_calls_per_block\": {");
        first = 1;
        for (int k = 0; k < C_N; k++) { printf("%s\"%s\": %.2f", first ? "" : ", ", nm[k], (double)poll[k] / blocks); first = 0; }
        printf("}}%s\n", preset + 1 < (int)ICARUS_FACTORY_PRESET_COUNT ? "," : "");
        icarus_destroy(s);
    }
    printf("]}\n");
    return 0;
}
