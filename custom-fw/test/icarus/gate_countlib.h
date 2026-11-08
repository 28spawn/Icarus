/* custom-fw/test/icarus/gate_countlib.h — -include'd into every synth source by the gate's libm census (tools/synth_gate.py).
 * Counts calls to the libm functions the unit pays a library-call price for (the firmware builds with -fno-builtin; measured on the unit:
 * sqrtf 1,158 ns, fabsf 48 ns as a call). icarus_fastmath.h (included after <math.h> by the synth sources) replaces fabsf and sqrtf, so a
 * count here is a call that is STILL a library call. */
#ifndef GATE_COUNTLIB_H
#define GATE_COUNTLIB_H
#include <math.h>
#include <stdint.h>
enum { C_FLOORF, C_SINF, C_COSF, C_EXPF, C_LOGF, C_LOG2F, C_POWF, C_TANHF, C_FMODF, C_ATAN2F, C_TANF, C_N };
extern unsigned long g_lc[C_N];
static inline float c_floorf(float x) { g_lc[C_FLOORF]++; return __builtin_floorf(x); }
static inline float c_sinf(float x)   { g_lc[C_SINF]++;   return __builtin_sinf(x); }
static inline float c_cosf(float x)   { g_lc[C_COSF]++;   return __builtin_cosf(x); }
static inline float c_expf(float x)   { g_lc[C_EXPF]++;   return __builtin_expf(x); }
static inline float c_logf(float x)   { g_lc[C_LOGF]++;   return __builtin_logf(x); }
static inline float c_log2f(float x)  { g_lc[C_LOG2F]++;  return __builtin_log2f(x); }
static inline float c_powf(float x, float y) { g_lc[C_POWF]++; return __builtin_powf(x, y); }
static inline float c_tanhf(float x)  { g_lc[C_TANHF]++;  return __builtin_tanhf(x); }
static inline float c_fmodf(float x, float y) { g_lc[C_FMODF]++; return __builtin_fmodf(x, y); }
static inline float c_atan2f(float y, float x) { g_lc[C_ATAN2F]++; return __builtin_atan2f(y, x); }
static inline float c_tanf(float x)   { g_lc[C_TANF]++;   return __builtin_tanf(x); }
#define floorf c_floorf
#define sinf c_sinf
#define cosf c_cosf
#define expf c_expf
#define logf c_logf
#define log2f c_log2f
#define powf c_powf
#define tanhf c_tanhf
#define fmodf c_fmodf
#define atan2f c_atan2f
#define tanf c_tanf
#endif
