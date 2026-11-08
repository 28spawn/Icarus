# Icarus Licensing

Icarus is free software. Its **code** is licensed under the
GNU General Public License, version 3 only (`GPL-3.0-only`, full text in [LICENSE](LICENSE)),
with specific imported modules licensed under `MPL-2.0` (Mozilla Public License 2.0) and
`GPL-3.0-only OR MPL-2.0` (dual-licensed).

This project provides an open-source custom firmware, DSP engine, guitar synthesizer, desktop emulator,
and workbench for the **M-VAVE / Donner Pocket AMP** (JieLi AC791N / pi32v2 SoC). It builds on the
firmware architecture, loader, and packaging work of **Pendel** and **pendel-bsp** (Leo Kuroshita,
Hügelton Instruments), incorporates the Bluetooth Low Energy stack from **Optimist** (hdavid,
Optimist contributors), references standard JieLi AC79 hardware facts, and uses clean-room
audio datasets and synthetic models.

Copyright (C) 2026 the Icarus contributors\
Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments\
Copyright (C) 2026 hdavid (Optimist BLE stack)\
Modifications Copyright (C) 2026 the Icarus contributors

Each ported or derived part keeps its authors' notice in its files and is listed in the third-party
table below.

---

## What is code (GPL-3.0-only)

Every file in this tree that carries an `SPDX-License-Identifier: GPL-3.0-only` header, or is part of
the primary application codebase without an explicit secondary license, is GPL-3.0-only:

- **Firmware application core and HAL**: `custom-fw/src/` (board drivers, audio pipeline, display list engine, preset system, SysEx handler).
- **Icarus Guitar Synthesizer**: `custom-fw/src/effects/icarus/` (analog oscillator models, zero-delay feedback resonant filter, pitch follower, envelope generator).
- **DSP and neural amp modeler**: `custom-fw/src/dsp/` (hardware accelerator SIMD bridge, tuner, fixed-point math) and `custom-fw/src/nam/` (float32 WaveNet neural inference for NAM A2 models, GRU legacy path).
- **Effect chain**: `custom-fw/src/fx/` (dynamics, overdrive, EQ, modulation, delay, reverb, partitioned FIR cabinet convolution).
- **Groove Station drum machine**: `custom-fw/src/dsp/drums_synth.c`, `custom-fw/src/dsp/drum_codec.h`, authored rhythm patterns in `drum_patterns.inc`, and procedural voice models in `drum_kit.inc`.
- **Build and packaging tools**: `custom-fw/build.ps1`, `custom-fw/tools/` (`onyx_pkg_make.py`, `lz4blk.py`, `pack_ufw.py`, `qa_gate.py`, etc.).
- **Desktop emulator and workbench**: `custom-fw/emu/` (PortAudio ASIO bridge, `pocket_amp_emu.dll`, web frontend) and `custom-fw/wasm/`.
- **Host verification and regression test suites**: `custom-fw/test/` (DSP chain, YIN tuner, WaveNet cycle benchmark, memory reclaim, fault injection, robustness).
- **Web utilities**: `custom-fw/web/` (`Onyx_Web_Editor.html`, `Icarus_Web_Editor.html`).

You may use, study, change, and distribute it under GPL-3.0-only. If you distribute binaries or modified
versions of this firmware, you must make the complete corresponding source code available under the
same license.

---

## Permissive and dual-licensed components

### 1. The Bluetooth LE MIDI stack (GPL-3.0-only OR MPL-2.0)

The Bluetooth Low Energy stack in `custom-fw/third_party/optimist_ble/` is imported from
**Optimist** (<https://github.com/w0ts/optimist>, commit `fcdc4881e8788365c62ea8be7fa11b4d82966bac`,
pinned in `UPSTREAM-COMMIT.txt`).

- **Author & License**: Written for Optimist by **hdavid** and licensed under **GPL-3.0-only OR MPL-2.0**,
  at the user's choice (Optimist `LICENSING.md`). The full text of MPL-2.0 is in
  `custom-fw/third_party/optimist_ble/LICENSES/MPL-2.0.txt`.
- **Radio start-up tables**: `hal/ble_rf_tables_v15.h` contains hardware register constants and sequences
  under **Apache-2.0** (Zhuhai Jieli Technology / M-VAVE).
- **Local modifications**: Adaptations made for hardware integration:
  - `src/ble_hw_wl82.c`: Derived persistent static random BLE address (`pa_ble_static_addr()`), software channel remap table (`PA_BLE_SOFT_REMAP`), and receive window widening factor (`HW_WIDEN_X`).
  - `src/ble_ll.c`: Diagnostic ring tracking and instant tolerance window (accepting late `LL_CHANNEL_MAP_IND` and `LL_CONNECTION_UPDATE_IND` packets from Windows hosts within 64 events rather than immediately terminating the connection).
  - `hal/fm1_ble_rf.h`: Runtime hooks for VCO edge/settle calibration (`FM1_VCO_STOCK_SCAN`, `FM1_LUT_W1`).

All modifications remain licensed under `GPL-3.0-only OR MPL-2.0`. The stack is compiled into the firmware
only when explicitly enabled (`-BLE` build switch, `PA_BLE=1`).

### 2. The update loader and OTA protocol (MPL-2.0)

- **pendel-bsp**: `custom-fw/third_party/pendel_loader/` is the hardware layer from **pendel-bsp**
  (<https://github.com/hugelton/pendel-bsp>), Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton
  Instruments, licensed under **MPL-2.0** (full text in `custom-fw/LICENSES/MPL-2.0.txt`).
- **Modified loader files**:
  - `custom-fw/loader/onyx_loader.c` (adapted from `boards/pocketamp/pa_loader.c`)
  - `custom-fw/src/sys/pa_ota_session.c` (adapted from `chip/ota.c`)
  These files retain their `MPL-2.0` headers. Under MPL-2.0 Section 3.3, because these files do not carry
  an "Incompatible With Secondary Licenses" notice, they may be compiled and distributed as part of a
  larger `GPL-3.0-only` firmware executable without relicensing conflicts.

---

## What is deliberately NOT included (clean-room boundaries)

To respect third-party intellectual property and avoid distributing vendor proprietary code:

1. **No M-VAVE or Donner firmware binaries**: No stock `.fwsc`, bootloaders, or vendor factory updates are stored in this repository.
2. **No proprietary amp models or factory cabinet IRs**: The firmware contains zero extracted proprietary factory amp captures or cab WAV files. User amp captures and custom IRs are loaded via Web MIDI SysEx. If a user wishes to reference stock voicings, the firmware reads them locally and read-only from the user's own physical device flash partition at `0x0BF000` via `src/sys/stock_assets.c`. Updates never write or overwrite this partition.
3. **No M-VAVE dev kit proprietary library**: Neither `libfm1_engine.a` nor any GPL-linking-exception `app/` code from `m-vave/fm1_open` is used, linked, or included.
4. **JieLi AC79 SDK files are fetched, not checked in**: The three low-level binary blobs needed to package a flash image (`uboot.boot`, `cfg_tool.bin`, and `eq_cfg_hw.bin`) are Apache-2.0 (Zhuhai Jieli Technology). They are downloaded on demand at build time by `custom-fw/tools/get_sdk_files.py` from commit `5fa84a9b2f723a5e9337ad79dc2ad741fc98fe2d` and verified by SHA-256 hash.

---

## Audio assets and sample provenance

- **Procedural Groove Station drum kit**: The 12 drum voices (`kick`, `snare`, `rim`, `hats`, `ride`, `crash`, `toms`, `click`) in `src/dsp/drum_kit.inc` are **100% synthetically generated** by `custom-fw/tools/gen_drum_kit.py` using algorithmic oscillators, exponential envelopes, and filtered noise curves. Kick and tom drums are dynamically re-synthesized on-chip; other sounds are stored as 4-bit IMA-ADPCM, G.711 mu-law, or 16-bit PCM. No audio recordings were sampled from commercial hardware.
- **Rhythm pattern library**: 49 original drum patterns authored as 48-step grids in `src/dsp/drum_patterns.inc`.
- **Dry guitar DI captures**: 15 clean direct-input (DI) guitar performance captures (`jonas_chords_1..5.wav`, `jonas_lick_1..5.wav`, `jonas_riff_1..5.wav`) in `custom-fw/emu/samples/` provided courtesy of **Jonas Hemmersbach** for tone-stack auditioning, neural model verification, and emulator playback.
- **Default synthetic cabinet IR**: Generated cleanly by mathematical modeling of speaker low/high roll-off and resonance curves.

---

## How we got our build working

Building reliable custom firmware for the JieLi AC791N (pi32v2) architecture under Windows required
solving multiple deep toolchain, linker, memory layout, and communication pipeline issues. The sections
below document these hurdles and how they were resolved in `custom-fw/build.ps1` and related tools:

### 1. Halting on failure and eliminating stale objects (`Invoke-Checked`)

- **The issue**: In PowerShell, native CLI executables returning non-zero exit codes (`$LASTEXITCODE != 0`) do not trigger exceptions under `$ErrorActionPreference = "Stop"` (which only applies to PowerShell cmdlets). In earlier iterations of the build script, a compiler error, linker error, or failed unit test would silently print an error to stdout, but the script would proceed to the packaging step—generating a seemingly fresh `.fwsc` container containing stale object files from earlier builds.
- **The solution**: Every single host test execution, cross-compilation call, linker invocation, and packaging tool is wrapped in the `Invoke-Checked` function in `custom-fw/build.ps1`. If any command exits with a non-zero code, execution halts immediately with a terminating exception. Furthermore, the build directory `custom-fw/build_pi32` and previous output images (`build_app.bin`, `Icarus.ufw`, `Icarus.fwsc`) are completely removed at the start of every build run, guaranteeing that a failed build leaves nothing flashable behind.

### 2. Preventing source list drift (`sources.txt` manifest)

- **The issue**: The project maintains multiple compilation targets: host regression tests, the Windows native desktop emulator DLL (`pocket_amp_emu.dll`), the WebAssembly build, and the pi32v2 target firmware. Previously, each target maintained its own manual file list. Adding a new source module frequently broke the emulator or host tests because files were silently forgotten.
- **The solution**: A single authoritative manifest was established in `custom-fw/sources.txt`. The helper `Read-Sources.ps1` reads this manifest, and `custom-fw/tools/check_sources.py` runs as the very first step in `build.ps1`, failing the build immediately if any disk file is missing from the manifest.

### 3. pi32v2 toolchain and circular runtime library dependencies

- **The issue**: The target cross-compiler is JieLi Clang (`-target pi32v2 -mcpu=r3 -O2 -mllvm -enable-ipra=false -ffreestanding -fno-builtin`). When linking floating-point math functions (such as `tanh` used in neural network activation LUTs and saturators), `libm.a` calls the 64-bit/double soft-float helper `__subdf3`. In the JieLi toolchain, `__subdf3` resides in `libcompiler_rt.a`. In a linear linker scan, `libcompiler_rt.a` was evaluated before `libm.a` pulled in the reference, resulting in fatal `undefined reference to __subdf3` errors.
- **The solution**: In `custom-fw/build.ps1`, the linker (`pi32v2-ld.exe`) is invoked with explicit group scanning flags:
  ```powershell
  & "$pi32Bin\pi32v2-ld.exe" -T custom-fw/app.ld --gc-sections $objs `
      "--start-group" "$pi32Lib\libcompiler_rt.a" "$pi32Lib\libm.a" "$pi32Lib\libc.a" "--end-group" `
      -o (Join-Path $BuildDir "pocket_amp_custom.elf")
  ```
  This forces the linker to resolve circular and inter-library dependencies between compiler-rt, libm, and libc until all symbols settle.

### 4. Flash image layout and the 48-byte `.data` offset shift

- **The issue**: The AC791N SoC uses execute-in-place (XIP) from external SPI flash starting at `0x02000120`. However, time-critical code, interrupt service routines, and hardware controllers must run from internal SRAM (`0x01C00000`). Early builds naively concatenated the `.text` and `.data` binary extractions into `build_app.bin`. 
  However, the linker script places the RAM-executable sections (`.ram_text`, `.c1_text`, `.ramfn`, `.flashfn`) in flash between `.text` and `.data`. Because these sections were omitted from the naive binary concatenation, `.data` sat 48 bytes earlier in flash than where the startup assembly (`entry.S`) expected to copy it from. Every initialized global variable was corrupted by a 48-byte shift: stock amp lookup tables were off by 1 entry, cabinet lookup tables were off by 6 entries, and pointers read beyond memory bounds, causing instant device crashes.
- **The missing `.flashfn` probe**: The flash controller probe (`flash_ram.c`, located in `.flashfn`) runs entirely from SRAM to probe SPI NOR parameters. Because `.flashfn` was not being extracted and packed at its load address, flash contained all zeros at that location; the startup code jumped into null memory, causing devices to die before reaching stage 1 (the bug observed in packs 140 and 168).
- **The solution**: `custom-fw/build.ps1` queries `llvm-nm.exe` to discover the exact linker load symbols:
  `_rt_load`, `_c1_load`, `_rf_load`, `_ff_load`, and `_data_load`.
  Each section (`text.bin`, `ram_text.bin`, `c1_text.bin`, `ramfn.bin`, `flashfn.bin`, `data.bin`) is extracted individually with `llvm-objcopy.exe` and copied into the final binary image at its exact calculated byte offset relative to `XIP_BASE` (`0x02000120`):
  ```powershell
  $imgLen = ($syms['_data_load'] - $xipBase) + $dataBytes.Length
  $img = New-Object byte[] $imgLen
  [Array]::Copy($textBytes, 0, $img, 0, $textBytes.Length)
  [Array]::Copy($rtBytes,   0, $img, ($syms['_rt_load'] - $xipBase), $rtBytes.Length)
  [Array]::Copy($c1Bytes,   0, $img, ($syms['_c1_load'] - $xipBase), $c1Bytes.Length)
  [Array]::Copy($rfBytes,   0, $img, ($syms['_rf_load'] - $xipBase), $rfBytes.Length)
  [Array]::Copy($ffBytes,   0, $img, ($syms['_ff_load'] - $xipBase), $ffBytes.Length)
  [Array]::Copy($dataBytes, 0, $img, ($syms['_data_load'] - $xipBase), $dataBytes.Length)
  ```
  The script enforces runtime assertion checks on the generated binary image, confirming that `.flashfn` is present byte-for-byte and that `.data` byte lengths match the startup copy window.

### 5. USB bulk transfer packetizing rules on Windows

- **The issue**: In USB 2.0 full-speed bulk transfers, endpoints operate with 64-byte packets. When sending memory diagnostic readouts (such as 32-byte or 240-byte RAM reads) over USB SysEx/bulk IN endpoints, transfers that ended on an exact multiple of 64 bytes caused host USB stacks on Windows to hang indefinitely waiting for a short packet or zero-length packet (ZLP).
- **The solution**: Implemented strict packet chunking rules (`custom-fw/src/board/usb_ble.c` and verified in `custom-fw/test/test_usb_chunk.c`), ensuring responses are framed cleanly and never leave the host waiting for termination.

### 6. Hardware timer 24 MHz 32-bit wrap handling

- **The issue**: The hardware tick counter on the AC791N runs at 24 MHz and is 32 bits wide. It overflows and wraps to zero every 178.96 seconds (~3 minutes). Any naïve time-delta calculation based on raw comparisons (`now > deadline`) broke after three minutes of uptime, causing long-press button timers, toasts, and auto-dim features to malfunction.
- **The solution**: Implemented an explicit, wrap-safe millisecond tracking clock (`pa_ms_clock.c`), verified by `test_ms_clock.c`.

### 7. Dual-core lock-free audio processing (Core 0 / Core 1)

- **The issue**: Real-time Float32 WaveNet neural amp inference requires ~1,360 µs per 64-sample block (out of a 1,451 µs block deadline at 44.1 kHz). Running WaveNet together with stereo delay, reverb, IR convolution, and UI rendering on Core 0 exceeded 100% CPU capacity, causing audio dropouts and watchdog resets.
- **The solution**: Implemented lock-free SMP delegation to Core 1 (`custom-fw/src/sys/smp_cpu1.c`, `custom-fw/src/board/cpu1.c`). Core 1 executes the neural amp inference stage exclusively, communicating with Core 0 via a single-producer single-consumer (SPSC) mailbox with sequence numbers and hardware inter-processor interrupts (IPI).

### 8. Pure vendor-free package construction (`onyx_pkg_make.py`)

- **The issue**: Flashing the Pocket AMP via USB MIDI SysEx requires an `.fwsc` container containing the update loader, app binary, and configuration headers. Standard vendor tools bundle proprietary binary images.
- **The solution**: Ported and adapted `tools/onyx_pkg_make.py` from Hugelton's Pendel project. The tool assembles the entire signed container structure from scratch, integrating the MPL-2.0 `onyx_loader.bin` and the Apache-2.0 JieLi SDK boot/config files, producing a clean, fully redistributable firmware package containing zero proprietary vendor code.

---

## Third-party material provenance table

The following audited table lists the third-party components, sources, and references integrated into or utilized by this project:

| Component / Upstream Project | Author / Upstream Origin | License | Repository Location | Role & Implementation Notes |
|---|---|---|---|---|
| **Optimist BLE Stack** | hdavid, Optimist contributors (<https://github.com/w0ts/optimist>, commit `fcdc4881e8`) | **GPL-3.0-only OR MPL-2.0** | `custom-fw/third_party/optimist_ble/` | Bluetooth LE MIDI stack for the AC791N radio. Imported verbatim; local integration changes incorporated in `src/ble_hw_wl82.c`, `src/ble_ll.c`, `src/ble_smp.c`, and `hal/fm1_ble_rf.h`. Off by default (`-BLE` build flag). |
| **pendel-bsp** | Leo Kuroshita (@kurogedelic), Hügelton Instruments (<https://github.com/hugelton/pendel-bsp>) | **MPL-2.0** | `custom-fw/third_party/pendel_loader/` | Hardware abstraction layer, USB drivers, and bootloader routines. Imported unmodified with original license notices; allows secondary combination under GPL-3.0 per Section 3.3. |
| **Onyx Update Loader** | Derived from pendel-bsp `boards/pocketamp/pa_loader.c` by Leo Kuroshita | **MPL-2.0** | `custom-fw/loader/onyx_loader.c` | Standalone flash update loader executed in SRAM during USB firmware updates. Verified against target memory boundaries. |
| **OTA Protocol Session** | Derived from pendel-bsp `chip/ota.c` by Leo Kuroshita | **MPL-2.0** | `custom-fw/src/sys/pa_ota_session.c` | Implements the device side of the JieLi OTA update handshake. |
| **Pendel Package Builder** | Leo Kuroshita (@kurogedelic), Hügelton Instruments (<https://github.com/hugelton/Pendel>) | **GPL-3.0-only** | `custom-fw/tools/onyx_pkg_make.py`, `custom-fw/tools/lz4blk.py` | Standalone firmware container generator and LZ4 block packer for creating valid `.fwsc` update packages from scratch. |
| **JieLi AC79 AIoT SDK** | Zhuhai Jieli Technology Co., Ltd. (<https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>, commit `5fa84a9`) | **Apache-2.0** | Downloaded to `custom-fw/third_party/jieli_sdk_files/` via `tools/get_sdk_files.py`; `LICENSES/Apache-2.0.txt` | Hardware register specifications, partition structures, and binary headers (`uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin`). Checked by SHA-256; not tracked in git. |
| **Radio Register Tables** | JieLi / M-VAVE via Optimist | **Apache-2.0** | `custom-fw/third_party/optimist_ble/hal/ble_rf_tables_v15.h` | Radio baseband initialization register constants captured from stock firmware. |
| **Dry Guitar DI Captures** | Jonas Hemmersbach | Courtesy contribution / permission for Icarus | `custom-fw/emu/samples/jonas_*.wav` | 15 clean dry guitar DI captures (chords, licks, riffs) used for emulator auditioning and DSP test benches. |
| **WaveNet Architecture** | Steven Atkinson (Neural Amp Modeler) / van den Oord et al. | Architecture reference; clean C implementation | `custom-fw/src/nam/pa_wavenet.c`, `include/pa_wavenet.h` | Float32 WaveNet neural network inference engine running dilated convolution layers for guitar amp modeling. |
| **YIN Pitch Detection** | Alain de Cheveigné, Hideki Kawahara (2002) | Algorithm reference; clean C implementation | `custom-fw/src/dsp/tuner.c` | Chromatic tuner pitch detection based on difference functions and cumulative mean normalized difference. |
| **Audio EQ Cookbook Filters** | Robert Bristow-Johnson | Reference formulas; clean C implementation | `custom-fw/src/dsp/pa_biquad.c` | Parametric EQ, low-cut, and high-cut Direct Form II Transposed biquad filter implementations. |
| **Moog Ladder Filter Model** | Stilson & Smith (1996), Huovilainen (2004) | Algorithm reference; clean C implementation | `custom-fw/src/effects/icarus/alpha_filter.c` | Zero-delay feedback (ZDF) 4-pole resonant low-pass filter with Padé tanh saturation. |

---

## Contributions

Contributions are welcomed under **GPL-3.0-only**. By submitting a pull request, you certify that your
contribution is your own original work or is provided under an identical or compatible open-source license.
Contributions modifying the imported Bluetooth LE stack (`custom-fw/third_party/optimist_ble/`) or the update
loader components (`custom-fw/loader/`, `custom-fw/src/sys/pa_ota_session.c`) must be compatible with their
respective upstream dual-license or MPL-2.0 terms.

---

## Trademarks

- "M-VAVE" and "Donner" are trademarks of their respective owners. Icarus is an independent, unofficial interoperability and research project and is not affiliated with, endorsed by, or sponsored by M-VAVE, Donner, or Zhuhai Shengke.
- "JieLi" is a trademark of Zhuhai Jieli Technology Co., Ltd.
- "Roland", "Boss", "Tube Screamer", "Ibanez", "Klon", "Fulltone", "ProCo", "Electro-Harmonix", "Dunlop", "Moog", and other equipment model names are used strictly for informational purposes to identify the acoustic or electrical behavior modeled by software DSP algorithms. This project contains no proprietary presets, samples, or code from those manufacturers.

---

## Radio operation notice

By default, builds produced by `custom-fw/build.ps1` **do not enable the radio transmitter**. The wireless
radio hardware is uninitialized and inactive. Enabling Bluetooth LE MIDI requires passing the explicit
`-BLE` build switch, which compiles the open Optimist Bluetooth stack. Wi-Fi transmission is never initialized.
All radio usage must comply with local telecommunications regulations.

---

## Hardware Safety & Disclaimers

> [!WARNING]
> Flashing custom firmware involves writing to the physical SPI-NOR flash memory of your device. While Icarus enforces rigorous partition write protection (pa_flash_rw_ok()) and includes automated recovery mechanisms, flashing firmware is undertaken at your own risk. The authors and contributors accept no liability for hardware damage, warranty invalidation, or data loss. Ensure your device battery is adequately charged before initiating firmware flashing operations.
