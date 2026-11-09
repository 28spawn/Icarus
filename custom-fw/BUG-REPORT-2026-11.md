# Bug hunt, November 2026

What was looked for, what was found, what was fixed, and what was left alone.

## How the code was checked

| Check | Scope | Result |
|---|---|---|
| AddressSanitizer + UndefinedBehaviorSanitizer (`clang -O1 -g -fsanitize=address,undefined`) | every host suite `build.ps1` runs, plus `test_ms_clock`, `test_ota`, `test_robustness` (35 suites) | 6 defects, all fixed; the rerun is clean (every suite exits 0, no sanitizer line) |
| clang static analyzer (`clang --analyze`) | 55 host-compilable C files | 1 finding (B7), fixed |
| Compiler warnings (`-Wall -Wextra`) | same suites | 2 real defects (B5, B6); the rest are unused code, listed below |
| pyflakes | every tracked `.py` | 4 defects (P1..P4), all fixed |
| Full `build.ps1` (host suites at `-O2`, pi32 cross-compile, emulator matrix, package checks) | after all fixes | passes, package clean |

Not covered: `src/board/main.c` and the other target-only files cannot be compiled for the host, so they
only got compiler warnings from the pi32 build and a read-through, not the sanitizers or the analyzer.
The web editor's JavaScript was not linted. Nothing was tested on a unit.

## Fixed: firmware

### B1. NAM hardware self-test read past its weight matrices (out-of-bounds read)
`src/nam/nam_engine.c`, `nam_hw_selftest`. The matrices were declared with 16 rows, but the accelerator
control words `PA_HW_CTRL_GRU_9x17_B` and `PA_HW_CTRL_GRU_9x16` carry a row field of 10, and the second
pass starts at row 7, so it reads row 16: one row beyond the array. ASan reported a
global-buffer-overflow in `hw_accel.c` and aborted `pa_test_dsp` and `pa_test_icarus_chain`. On the unit
the read lands in whatever follows the array, so the self-test's reference result depended on unrelated
memory. The matrices now have `PA_NAM_HIDDEN + 1` rows; the comments in `include/pa_hw_accel.h` now
state the real row field.

### B2. USB capture: 24-bit packing overflowed before it saturated
`src/board/usb_ble.c`, `put24`. The sample was shifted left by 2 and only then clamped, so a value
of 2^29 or more wrapped to the opposite sign before the clamp saw it (a full-scale click instead of
clipping), and the shift of a negative value is undefined behaviour in C. It now clamps first and then
multiplies.

### B3. Sign extension of 24-bit samples by shifting a signed value
`src/board/usb_ble.c` (`pa_usb_audio_ep3_out_rx`), `src/sys/pa_audio_core.c` (`s24_sign_extend`),
`emu/emu_bridge.c` (three places), and the `s24()` helper in `test/test_usb_stereo.c`,
`test_usb_capture_tone.c`, `test_usb_playback_tone.c`. `x << 8 >> 8` on an `int32_t` shifts into the
sign bit, which is undefined. Every compiler in use gives the expected result today, so nothing
audible changes, but an optimiser is allowed to assume it never happens. The shift is now done
unsigned and the result converted back.

### B4. Tuner: the same signed shift on the ring samples
`src/dsp/tuner.c`, the normalisation of the two ring taps. Same fix as B3.

### B5. `main.c`: `pos` modified and read in one expression
`src/board/main.c`, the SRAM pattern test, case 3: `sram[pos++ & 4095u] = acc + pos;`. The order of the
increment and the read is unsequenced, so the value written was up to the compiler. Now the increment
is its own statement.

### B6. `main.c`: a function used outside the block that declared it
`src/board/main.c`: `extern uint32_t pa_alnk_isr_entries(void);` was declared inside one block and
called from another (an implicit declaration in the second place). The declaration is now at file scope.

### B7. `pa_audio_core_apply_model`: `ui` checked once, then used unchecked
`src/sys/pa_audio_core.c`. The fallback path tested `if (ui)` but the lines after it dereferenced `ui`
unconditionally. The pointer is the module's `s_ui`, which is null until the UI context is registered;
the other users of it (`master_vol`, the CPU meter) already guard. The amp path now guards every use.

## Fixed: tests

### T1. `test_full_firmware.c`: the tracking allocator returned misaligned blocks on Windows
The header in front of every block was one `long`: 4 bytes on Windows, so every allocation was offset by
4 and misaligned for 8-byte types (276 UBSan reports). The header is now 16 bytes.

## Fixed: PC tools

| | File | Defect | Effect |
|---|---|---|---|
| P1 | `tools/core1_watch_pc.py` | a string literal had real line breaks in place of `\n` | SyntaxError: the tool could not start at all |
| P2 | `tools/perf_monitor_pc.py` | uses `os.environ` at import time without `import os` | NameError: the tool could not start at all |
| P3 | `tools/hw_test.py` | `symbols()` uses `os` without importing it | NameError on `read <name> --elf ...` |
| P4 | `tools/ram_dump.py` | `--auto` tests `addr in skip`, and `skip` is never defined | NameError on the first read of every `--auto` dump; the dead test is removed (pages that fail twice were already skipped by the code below it) |

## Noted, not changed

- `src/board/audio_alnk.c` redefines `PA_ISR_CP`, which `include/pa_plat.h` already defines with the
  same value. A warning only; one of the two should go.
- Unused variables and functions the compiler reports in `pa_wavenet.c`, `fx_graph.c`, `nam_engine.c`,
  `ui_app.c`, `gpio_scan.c`, `pa_ota_session.c`, `pa_audio_core.c`. Dead code, not defects.
- `emu/server.py` declares `global g_loaded_samples` and `global g_sample_clock` in functions that never
  assign them. Harmless.
- `ARCHITECTURE.md`, `LICENSING.md`, `custom-fw/README.md` and `custom-fw/test/EFFECT_MATRIX.md` still
  call the emulator library `pocket_amp_emu.dll`; the launchers build and load `icarus_emu.dll`.
- The images in `custom-fw/release/` were built before these fixes. They were deliberately not rebuilt
  in this change; the next release build picks the fixes up.
