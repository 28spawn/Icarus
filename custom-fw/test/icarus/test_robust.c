/* custom-fw/test/icarus/test_robust.c — Icarus Synth under hostile input (the synth gate's SYN-03).
 *
 * A guitar into an ADC is not a clean sine: it can be silent for minutes, decay into denormal range, hit full scale, or sit on a DC step; and a
 * filter that is pushed hard can leave NaN or Inf in its state, where it stays for ever (every later sample is NaN, a library routine fed a NaN
 * may loop or crawl on the unit, and the pitch tracker's tracked frequency feeds straight into logf and floorf in the MAIN loop). The unit
 * froze twice with a stalled main loop and a guitar playing; the emulator cannot reproduce it, so this is the part of that class the host can
 * still catch: for every preset and a set of nasty inputs the output must stay finite and bounded, and the synth must RECOVER (finite, bounded
 * and sounding again) after the input returns to normal.
 *
 *   build: clang -O2 -DPA_HOST_SIM=1 -Icustom-fw/include test/icarus/test_robust.c src/effects/icarus/icarus*.c -lm */
#include "../../src/effects/icarus/icarus.h"
#include "../../include/pa_config.h"
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
static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   "); else { printf("  FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static int finite_bounded(const float *x, int n, float lim) {
    for (int i = 0; i < n; i++) if (!(fabsf(x[i]) <= lim)) return 0;            /* false for NaN too */
    return 1;
}

typedef struct { icarus_state_t *s; double ph; int n; double us; int bad_blocks; float peak; } rig_t;

static rig_t rig_new(int preset) {
    rig_t r; memset(&r, 0, sizeof(r));
    r.s = icarus_create(); icarus_load_preset(r.s, (uint8_t)preset); r.s->enabled = 1;
    return r;
}

/* feed `blocks` blocks of `kind`; returns the largest |out| seen; counts blocks whose output was not finite and bounded (|x| <= 8) */
static void feed(rig_t *r, int kind, int blocks) {
    float in[BLK], L[BLK], R[BLK];
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < BLK; i++) {
            float x = 0.0f;
            r->ph += 196.0 / 44100.0; if (r->ph >= 1.0) r->ph -= 1.0;
            const float a = (float)(6.2831853 * r->ph);
            switch (kind) {
                case 0: x = 0.0f; break;                                        /* silence */
                case 1: x = 0.25f * (sinf(a) + 0.5f * sinf(2.0f * a)); break;   /* a normal note */
                case 2: x = 1.0e-38f * ((r->n & 1) ? 1.0f : -1.0f); break;      /* denormal-range noise */
                case 3: x = (r->ph < 0.5) ? 1.0f : -1.0f; break;                /* full-scale square */
                case 4: x = 0.9f; break;                                        /* DC step */
                case 5: x = (r->ph < 0.5) ? 1.0e6f : -1.0e6f; break;            /* a million times full scale (a stuck / shorted input) */
                case 6: x = (i == 3) ? NAN : 0.1f * sinf(a); break;             /* one NaN in the stream */
                case 7: x = (i == 5) ? INFINITY : 0.1f * sinf(a); break;        /* one Inf */
                case 8: x = 0.5f * (float)((r->n * 2654435761u >> 8) & 0xFFFF) / 32768.0f - 0.5f; break;   /* hash noise */
            }
            in[i] = x;
            r->n++;
        }
        const double t0 = now_us();
        icarus_process_block_f32(r->s, in, L, R, BLK);
        icarus_poll_background(r->s);
        r->us += now_us() - t0;
        if (!finite_bounded(L, BLK, 8.0f) || !finite_bounded(R, BLK, 8.0f)) r->bad_blocks++;
        for (int i = 0; i < BLK; i++) { const float m = fabsf(L[i]); if (m == m && m > r->peak) r->peak = m; }
    }
}

int main(void) {
    static const char *kname[9] = { "silence", "normal note", "denormal noise", "full-scale square", "DC step", "1e6 x full scale", "one NaN", "one Inf", "noise" };
    printf("state size: sizeof(icarus_state_t) = %u bytes\n", (unsigned)sizeof(icarus_state_t));
    CHECK(sizeof(icarus_state_t) <= 12288u, "the synth state fits its allowance (%u of 12288 bytes)", (unsigned)sizeof(icarus_state_t));

    for (int preset = 0; preset < (int)ICARUS_FACTORY_PRESET_COUNT; preset++) {
        const icarus_preset_t *fp = icarus_get_factory_preset((uint8_t)preset);
        printf("=== [%2d] %s ===\n", preset + 1, fp->name);
        /* 1. every hostile input in turn, each followed by normal playing: bounded during, and sounding again after */
        for (int kind = 2; kind <= 8; kind++) {
            rig_t r = rig_new(preset);
            feed(&r, 1, 300);                                                    /* settled on a note */
            rig_t before = r; (void)before;
            feed(&r, kind, 200);
            const int bad_during = r.bad_blocks;
            const float peak_during = r.peak;
            r.bad_blocks = 0; r.peak = 0.0f;
            feed(&r, 1, 400);                                                    /* 9 s of... no: 0.58 s of normal playing */
            const int bad_after = r.bad_blocks;
            const float peak_after = r.peak;
            const int ok_during = (bad_during == 0);
            /* NaN / Inf in the input may not be passed through, but the synth must not be killed by them */
            CHECK(ok_during, "%-18s: output finite and within +-8 while it lasts (%d bad blocks of 200)", kname[kind], bad_during);
            CHECK(bad_after == 0 && peak_after > 0.002f && peak_after <= 4.0f,
                  "%-18s: and sounds normally again afterwards (bad blocks %d, peak %.3f)", kname[kind], bad_after, peak_after);
            (void)peak_during;
            icarus_destroy(r.s);
        }
        /* 2. a denormal-range input must not be dramatically slower than a normal one (a denormal slow-down on the unit is far worse than on x86) */
        {
            rig_t a = rig_new(preset), b = rig_new(preset);
            feed(&a, 1, 200); feed(&b, 1, 200);
            feed(&b, 2, 700);                       /* let the note's own tail (envelopes, filter ring-down: real signal, not denormals) finish before timing the denormal input */
            a.us = 0; b.us = 0;
            feed(&a, 1, 600);
            feed(&b, 2, 600);
            const double ratio = b.us / (a.us > 0.0 ? a.us : 1.0);
            CHECK(ratio < 4.0, "denormal-range input costs %.2fx a normal note (limit 4x)", ratio);
            icarus_destroy(a.s); icarus_destroy(b.s);
        }
        /* 3. the pitch state stays sane: after all that the tracked frequency is finite and inside the tracker's own range */
        {
            rig_t r = rig_new(preset);
            for (int kind = 1; kind <= 8; kind++) feed(&r, kind, 100);
            feed(&r, 1, 300);
            const float f = r.s->pitch.frequency;
            CHECK(f == f && f >= 20.0f && f <= 3000.0f, "tracked frequency after the whole sequence is finite and sane (%.2f Hz)", f);
            icarus_destroy(r.s);
        }
    }
    printf("\n%s\n", fails ? "ROBUSTNESS TESTS FAILED" : "ALL ROBUSTNESS TESTS PASSED");
    return fails ? 1 : 0;
}
