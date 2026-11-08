/* custom-fw/src/effects/icarus/icarus_fastmath.h — libm calls the unit pays for, replaced in the synth's code.
 *
 * Measured on the unit (the "op cost probe" of the diagnostic readout, ns per operation): a dependent float add or multiply 8.3, a divide 13.9,
 * __builtin_fabsf 11.1 -- but fabsf() as a library call 47.8 (the firmware builds with -fno-builtin, so fabsf is a CALL) and sqrtf() 1,155
 * (a software routine). The synth makes ~850-1,150 fabsf calls and 2-5 sqrtf calls per 64-sample block: ~40 us and ~5 us of a block that has
 * 1,451 us in all, before anything else. Include this AFTER <math.h>. */
#ifndef ICARUS_FASTMATH_H
#define ICARUS_FASTMATH_H

#include <stdint.h>
#include <math.h>

#undef fabsf
#define fabsf(x) __builtin_fabsf(x)

/* sqrt by a bit-trick estimate (3.5 % off) and two Newton steps (2e-7 relative): ~10 float operations instead of a library routine. */
static inline float icarus_fast_sqrtf(float x) {
    if (!(x > 0.0f)) return 0.0f;
    union { float f; uint32_t u; } v;
    v.f = x;
    v.u = 0x1FBD1DF5u + (v.u >> 1);
    float y = v.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
}

#undef sqrtf
#define sqrtf(x) icarus_fast_sqrtf(x)

/* floor without the library call (22 calls a block): exact for |x| < 2^31, which every use here is (phases, clamped exponents) */
static inline float icarus_fast_floorf(float x) {
    const int32_t i = (int32_t)x;
    const float   f = (float)i;
    return (f > x) ? (f - 1.0f) : f;
}

#undef floorf
#define floorf(x) icarus_fast_floorf(x)

#endif /* ICARUS_FASTMATH_H */
