/* custom-fw/test/icarus/gate_profile.c — host cost probe for the synth gate (tools/synth_gate.py).
 *
 * Times icarus_process_block_q21 (the audio-interrupt side, the same entry the chain calls) per 64-sample block for every factory preset on
 * three kinds of playing, plus the main-loop poll on its own, plus a fixed float kernel that says how fast THIS host is. x86 has an FPU
 * and wide issue, so the absolute numbers mean nothing for the unit: the gate scales them with a calibration taken from a real hardware
 * readout (qa/synth_cost.json), which is why the speed kernel is here.
 *
 * Output: one JSON object on stdout.
 *   build:  clang -O2 -DPA_HOST_SIM=1 -Icustom-fw/include -Icustom-fw/src/effects/icarus test/icarus/gate_profile.c \
 *           src/effects/icarus/icarus*.c -lm -o gate_profile */
#include "icarus.h"
#include "../../src/dsp/fixedpoint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
static double now_us(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1e6 / (double)f.QuadPart; }
#else
#include <time.h>
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
#endif

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p) { free(p); }
void  pa_fx_chain_set_slot_enabled(pa_fx_chain_t *c, pa_fx_slot_id_t s, int on) { (void)c; (void)s; (void)on; }

#define BLK 64
static uint32_t rng = 0x12345678u;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (float)(int32_t)rng / 2147483648.0f; }

/* the three kinds of playing */
static float gen(int kind, double *ph, int n) {
    switch (kind) {
        case 0: {                                   /* picked single notes: a plucked decay, a new note every 0.4 s, 82..660 Hz */
            static const double nf[6] = { 110.0, 146.83, 196.0, 82.41, 329.63, 246.94 };
            const int note = (n / 17640) % 6;
            const double env = exp(-(double)(n % 17640) / 9000.0);
            *ph += nf[note] / 44100.0; if (*ph >= 1.0) *ph -= 1.0;
            const double a = 6.2831853 * *ph;
            return (float)(0.4 * env * (sin(a) + 0.5 * sin(2 * a) + 0.3 * sin(3 * a)));
        }
        case 1: {                                   /* loud strummed chord with pick noise: ~50 % peaks */
            const double t = (double)n;
            return (float)(0.18 * (sin(t * 0.0623) + sin(t * 0.0831) + sin(t * 0.0991) + sin(t * 0.1247)) + 0.04 * frand());
        }
        default: return 0.0f;                       /* silence */
    }
}

static int cmp_d(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

/* a fixed float kernel: dependent multiply-adds and divides, the same flavour of work as a filter */
static double speed_kernel_us(void) {
    volatile float sink = 0.0f;
    const double t0 = now_us();
    float a = 0.37f, b = 1.0001f, c = 0.0f;
    for (int i = 0; i < 3000000; i++) { c = c * 0.999f + a; a = a * b - 0.0001f * c; if ((i & 63) == 0) c = c / (1.0f + a * a); }
    sink = c + a;
    (void)sink;
    return now_us() - t0;
}

int main(void) {
    static const char *kind_name[3] = { "play", "chord", "silence" };
    double bench = 1.0e30;                                  /* the fastest of 7 runs of the speed kernel: noise only ever adds time */
    for (int i = 0; i < 7; i++) { const double b = speed_kernel_us(); if (b < bench) bench = b; }

    printf("{\"bench_us\": %.1f, \"presets\": [\n", bench);
    for (int preset = 0; preset < (int)ICARUS_FACTORY_PRESET_COUNT; preset++) {
        printf("  {\"preset\": %d, ", preset + 1);
        icarus_state_t *tmp = icarus_create(); icarus_load_preset(tmp, (uint8_t)preset);
        printf("\"name\": \"%s\", \"mode\": %d, \"kinds\": {", tmp->params.name, tmp->params.mode);
        icarus_destroy(tmp);
        for (int kind = 0; kind < 3; kind++) {
            /* five runs, keep the MINIMUM of the per-run means (a scheduler hiccup or a clock dip only ever adds time) */
            double means[5], p99s[5], polls[5];
            for (int run = 0; run < 5; run++) {
                icarus_state_t *s = icarus_create();
                icarus_load_preset(s, (uint8_t)preset);
                s->enabled = 1;
                s->ui_ttl = 0xFFFFu;                           /* worst case: the Icarus Synth screen is up (meters, scope, live cutoff running) */
                double ph = 0.0;
                const int warm = 500, blocks = 1500;
                static double ts[1500];
                double tp = 0.0;
                int32_t in_q[BLK], out_q[BLK];
                int n = 0;
                for (int b = 0; b < warm + blocks; b++) {
                    for (int i = 0; i < BLK; i++) in_q[i] = (int32_t)(gen(kind, &ph, n++) * (float)PA_Q21_ONE);
                    const double t0 = now_us();
                    icarus_process_block_q21(s, in_q, out_q, 0, BLK);
                    const double t1 = now_us();
                    icarus_poll_background(s);
                    const double t2 = now_us();
                    if (b >= warm) { ts[b - warm] = t1 - t0; tp += t2 - t1; }
                }
                double sum = 0.0; for (int i = 0; i < blocks; i++) sum += ts[i];
                qsort(ts, blocks, sizeof(double), cmp_d);
                means[run] = sum / blocks; p99s[run] = ts[(int)(blocks * 0.99)]; polls[run] = tp / blocks;
                icarus_destroy(s);
            }
            qsort(means, 5, sizeof(double), cmp_d); qsort(p99s, 5, sizeof(double), cmp_d); qsort(polls, 5, sizeof(double), cmp_d);
            printf("%s\"%s\": {\"avg_us\": %.3f, \"p99_us\": %.3f, \"poll_us\": %.3f}", kind ? ", " : "", kind_name[kind], means[0], p99s[0], polls[0]);
        }
        printf("}}%s\n", preset + 1 < (int)ICARUS_FACTORY_PRESET_COUNT ? "," : "");
    }
    printf("], \"state_bytes\": %u}\n", (unsigned)sizeof(icarus_state_t));
    return 0;
}
