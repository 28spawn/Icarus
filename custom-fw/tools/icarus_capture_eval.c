/* custom-fw/tools/icarus_capture_eval.c - run a dry guitar capture through the REAL Icarus Synth code (host build) the way the unit does, and log what the tracker and the voice do.
 *
 *   icarus_capture_eval <input.f32 mono> <preset 0..11> <out_prefix> [sensitivity 0..1 or -1] [smoothing 0..1 or -1]
 *
 * Writes <out_prefix>.csv (one row per 64-sample audio block: time, tracked frequency, raw frequency, note, valid, onset, release, confidence, output rms L/R, peak) and <out_prefix>.f32
 * (the stereo output, interleaved float). The main-loop poll is called once per UI tick (every 11 blocks), as on the unit. Driven by tools/eval_captures.py. */
#include "../src/effects/icarus/icarus.h"
#include "../src/effects/icarus/icarus_pitch.h"
#include "../include/pa_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void *pa_alloc(uint32_t n) { return calloc(1, n); }
void  pa_free(void *p) { free(p); }
void  pa_fx_chain_set_slot_enabled(pa_fx_chain_t *c, pa_fx_slot_id_t s, int on) { (void)c; (void)s; (void)on; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s in.f32 preset out_prefix [sens] [smooth]\n", argv[0]); return 2; }
    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror("input"); return 2; }
    fseek(fi, 0, SEEK_END); long bytes = ftell(fi); fseek(fi, 0, SEEK_SET);
    const long total = bytes / 4;
    float *x = (float *)malloc((size_t)bytes);
    if (fread(x, 4, (size_t)total, fi) != (size_t)total) return 2;
    fclose(fi);

    icarus_state_t *s = icarus_create();
    icarus_load_preset(s, (uint8_t)atoi(argv[2]));
    s->enabled = 1;
    s->ui_ttl = 0;
    if (argc > 4 && atof(argv[4]) >= 0.0) s->pitch.sensitivity = (float)atof(argv[4]);
    if (argc > 5 && atof(argv[5]) >= 0.0) s->pitch.smoothing = (float)atof(argv[5]);

    char path[1024];
    snprintf(path, sizeof path, "%s.csv", argv[3]);
    FILE *fc = fopen(path, "w");
    snprintf(path, sizeof path, "%s.f32", argv[3]);
    FILE *fo = fopen(path, "wb");
    if (!fc || !fo) { perror("output"); return 2; }
    fprintf(fc, "block,t,freq,raw,note,valid,onset,release,conf,rmsL,rmsR,peak\n");

    const int N = 64;
    const char *pe = getenv("PA_POLL_EVERY");                       /* blocks between main-loop polls: 1 = every pass of a main loop that turns about once per block, 11 = the old once-per-UI-tick call */
    const int poll_every = pe ? atoi(pe) : 1;
    float in[64], L[64], R[64], st[128];
    long blocks = total / N;
    for (long b = 0; b < blocks; b++) {
        memcpy(in, x + b * N, sizeof in);
        s->pitch.onset = 0; /* the audio path clears it itself; keep the log's pulse honest */
        icarus_process_block_f32(s, in, L, R, N);
        const uint8_t onset = s->pitch.onset, release = s->pitch.release;
        if ((b % poll_every) == poll_every - 1) icarus_poll_background(s);   /* the main loop */
        double sl = 0, sr = 0; float pk = 0;
        for (int i = 0; i < N; i++) {
            sl += (double)L[i] * L[i]; sr += (double)R[i] * R[i];
            if (fabsf(L[i]) > pk) pk = fabsf(L[i]);
            if (fabsf(R[i]) > pk) pk = fabsf(R[i]);
            st[2 * i] = L[i]; st[2 * i + 1] = R[i];
        }
        fwrite(st, 4, 128, fo);
        fprintf(fc, "%ld,%.5f,%.3f,%.3f,%.3f,%d,%d,%d,%.3f,%.6f,%.6f,%.4f\n", b, b * N / 44100.0, s->pitch.frequency, s->pitch.raw_frequency, s->pitch.note,
                s->pitch.valid, onset, release, s->pitch.confidence, sqrt(sl / N), sqrt(sr / N), pk);
    }
    fclose(fc); fclose(fo);
    icarus_destroy(s);
    return 0;
}
