# Host verification & evaluation tools

Build each one on a PC (gcc/clang) together with the firmware sources. `PA_HOST_SIM` replaces the
hardware, and the device audio path (`pa_audio_process_half`) runs unchanged:

    SRCS="src/board/main.c src/board/audio_alnk.c src/board/ui_app.c src/board/ui_5x7.c \
          src/board/usb_ble.c src/board/spi_nor.c src/board/gpio_scan.c src/sys/*.c src/fx/*.c \
          src/dsp/*.c src/nam/*.c src/effects/icarus/*.c"
    gcc -O2 -DPA_HOST_SIM=1 -DPA_EMU_DLL=1 -DPA_POOL_TEST=1 -Iinclude -Isrc/dsp -Isrc/effects/icarus tools/<tool>.c $SRCS -lm -o tool

| Tool | Purpose |
|---|---|
| `preset_click_probe.c` | `probe <from> <to> <sine dBFS>`: largest sample step at a preset switch vs. the normal step size |
| `ui_screenshot_harness.c` | Renders front-panel screens into a 240×240 RGB565 framebuffer (`*.raw`) |
| `icarus_capture_eval.c` | Host evaluation harness for live guitar DI input through the Icarus DSP and pitch tracking pipeline |

The guitar auditioning DIs are 16-bit mono 44.1 kHz WAVs with standard 44-byte headers located in `emu/samples/*.wav` (courtesy of Jonas Hemmersbach).

## Icarus DSP & Synthesizer Evaluation Tools

| Tool | Purpose |
|---|---|
| `eval_captures.py` | Automated pitch tracking accuracy, latency, and octave-lock evaluation across authentic clean guitar DI captures |
| `synth_gate.py` | Automated CI test harness measuring transient onset detection, envelope trigger response, and pitch fidelity |
| `pitch_eval.py` | Benchmarks fundamental pitch tracking error against ground-truth synthetic sine sweeps and guitar DI files |
| `pitch_poly_eval.py` | Evaluates monophonic tracking behavior under polyphonic/multi-string input conditions |
| `synth_dump_check.py` | Verifies oscillator wave integrity, filter resonance stability, and DSP state bounds |
| `pi32_loopcost.py` | Analyzes assembly loop cycle cost and branch delays for the JieLi pi32v2 target |
| `pi32_stackdepth.py` | Static call-graph analysis of maximum call-stack depth for Core 0 and Core 1 |
| `perf_monitor_pc.py` | Real-time CPU, cycle headroom, and audio buffer deadline monitor |
| `crash_watch_pc.py` | Continuous fuzzing and stress monitor across rapid preset/model swaps |
