/* include/pa_dsp_cost.h - calibrated per-slot DSP cycle cost.
 *
 * The device measures the real thing with the pi32v2 free-running cycle
 * counter, but the host emulator and the WebAssembly build cannot. These
 * numbers were measured on the device (see custom-fw/test/test_cpu_meter.c and
 * the reclaim test) and are used by ALL THREE so the emulator, the WASM build
 * and the hardware report the same load.
 *
 * One 64-sample block, 360 MHz core, 44.1 kHz:
 *   the whole block budget is (360e6 * 64 / 44100) = 522,448 cycles.
 */
#ifndef PA_DSP_COST_H
#define PA_DSP_COST_H

#include <stdint.h>
#include "pa_fx.h"

#define PA_DSP_COST_NUM_SLOTS 8

static const uint32_t pa_dsp_slot_cycles[PA_DSP_COST_NUM_SLOTS] = {
    14000u,  /* 0 DYNAMICS */
    85000u,  /* 1 SYNTH (Project Alpha) */
    135000u, /* 2 AMP      */
    38000u,  /* 3 CAB      */
    15000u,  /* 4 EQ       */
    0u,      /* 5 unused   */
    0u,      /* 6 unused   */
    0u,      /* 7 unused   */
};

#endif /* PA_DSP_COST_H */
