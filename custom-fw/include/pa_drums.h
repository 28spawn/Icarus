/* custom-fw/include/pa_drums.h — Drum Machine stubs (drum synth removed)
 *
 * The drum machine has been removed to conserve RAM and CPU capacity.
 * This header provides zero-overhead stubs maintaining API compatibility.
 */
#ifndef PA_DRUMS_H
#define PA_DRUMS_H

#include <stdint.h>
#include <string.h>

#define PA_DRUM_STYLE_COUNT        1u
#define PA_DRUM_GROOVE_STYLE_COUNT 1u
#define PA_DRUM_STEPS_PER_BAR      48u
#define PA_DRUM_STEP_TICKS         40u
#define PA_DRUM_MAX_STEPS          48u
#define PA_DRUM_BAR_BLOCKS         16u
#define PA_DRUM_METRO_4_4          0u
#define PA_DRUM_VOICE_COUNT        1u

typedef struct {
    uint8_t  playing;
    uint8_t  style;
    uint16_t bpm;
    int32_t  level_q21;
    uint16_t current_step;
    uint8_t  bus_idle;
    uint8_t  dbg_active;
} pa_drums_t;

static inline uint8_t pa_drums_bar_block(uint16_t step) {
    (void)step;
    return 0;
}

static inline void pa_drums_init(pa_drums_t *d) {
    if (d) {
        memset(d, 0, sizeof(*d));
        d->bus_idle = 1;
    }
}

static inline void pa_drums_set_bpm(pa_drums_t *d, uint16_t bpm) {
    if (d) d->bpm = bpm;
}

static inline void pa_drums_set_style(pa_drums_t *d, uint8_t style) {
    if (d) d->style = style;
}

static inline void pa_drums_set_playing(pa_drums_t *d, uint8_t playing) {
    (void)d; (void)playing;
}

static inline void pa_drums_set_level(pa_drums_t *d, uint8_t pct) {
    (void)d; (void)pct;
}

static inline const char *pa_drums_get_style_name(uint8_t style) {
    (void)style;
    return "DISABLED";
}

static inline uint16_t pa_drums_style_length(uint8_t style) {
    (void)style;
    return 48;
}

static inline uint16_t pa_drums_sounding_step(const pa_drums_t *d) {
    (void)d;
    return 0;
}

static inline uint32_t pa_drums_voice_decode(uint32_t voice, float *out, uint32_t max) {
    (void)voice; (void)out; (void)max;
    return 0;
}

static inline void pa_drums_mix_block(pa_drums_t *d, int32_t *q21_buf, int n) {
    (void)d; (void)q21_buf; (void)n;
}

#endif /* PA_DRUMS_H */
