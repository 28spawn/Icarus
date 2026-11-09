#!/usr/bin/env python3
"""Soak the Icarus Synth view in the emulator: enter it, play varied guitar-like signals (silence, plucks, loud chords, noise, sweeps,
DC steps, near-full-scale hits) for several minutes of emulated time, press nothing, and report the first stall.

The firmware's main loop runs inside emu_process_audio_f32 (a tick every 11 blocks), so a hang in the main-loop code is a hang of this
process: the parent runs the soak in a child with a timeout and says WHERE it was (the last progress line).

    python custom-fw/emu/soak_alpha.py [seconds_of_emulated_time=240] [preset=0..11]
"""
import ctypes, math, os, random, subprocess, sys, tempfile
from pathlib import Path

EMU = Path(__file__).resolve().parent
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 240
PRESET = int(sys.argv[2]) if len(sys.argv) > 2 else 0

if os.environ.get("PA_SOAK_CHILD") != "1":
    env = dict(os.environ, PA_SOAK_CHILD="1")
    try:
        r = subprocess.run([sys.executable, __file__, str(SECS), str(PRESET)], env=env, timeout=SECS + 120,
                           capture_output=True, text=True)
        print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "(no output)")
        sys.exit(r.returncode)
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        last = out.strip().splitlines()[-1] if out.strip() else "(no progress line)"
        print("HUNG: the emulator did not come back; last progress:", last)
        sys.exit(1)

os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_soak_"), "flash.bin")
d = ctypes.CDLL(str(EMU / "icarus_emu.dll"))
d.emu_ui_tap.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float)] * 3 + [ctypes.c_int]
d.emu_select_preset.argtypes = [ctypes.c_int]
d.emu_set_slot_type.argtypes = [ctypes.c_int, ctypes.c_int]
d.emu_init()
N = 64
inp = (ctypes.c_float * N)(); oL = (ctypes.c_float * N)(); oR = (ctypes.c_float * N)()
HOME, VOLP = 1, 4
SR = 44100


def run(blocks, gen):
    for b in range(blocks):
        for i in range(N):
            inp[i] = gen(b * N + i)
        d.emu_process_audio_f32(inp, oL, oR, N)


run(200, lambda n: 0.0)
d.emu_ui_tap(HOME | VOLP, 0)                       # enter Icarus Synth
run(400, lambda n: 0.0)
d.emu_set_slot_type(1, PRESET)
rnd = random.Random(7)
ph = [0.0]


def tone(f, a):
    def g(n):
        ph[0] += f / SR
        return a * (math.sin(6.2831853 * ph[0]) + 0.5 * math.sin(12.566 * ph[0]) + 0.3 * math.sin(18.85 * ph[0]))
    return g


segments = [
    ("silence", lambda n: 0.0, 3.0),
    ("A2 pluck 110", tone(110.0, 0.25), 2.0),
    ("E2 low 82", tone(82.41, 0.40), 2.0),
    ("loud chord ~53%", lambda n: 0.5 * (math.sin(n * 0.0623) + math.sin(n * 0.0831) + math.sin(n * 0.0991)) / 1.6, 3.0),
    ("near full scale", tone(196.0, 0.55), 2.0),
    ("white noise 30%", lambda n: rnd.uniform(-0.3, 0.3), 2.0),
    ("sweep 60-2k", lambda n: 0.3 * math.sin(6.2831853 * (60 * (n / SR) + 0.5 * (2000 - 60) / 4.0 * (n / SR) ** 2 % 1.0)), 3.0),
    ("DC step", lambda n: 0.4 if (n // 20000) % 2 else -0.4, 2.0),
    ("high E 1.2k", tone(1244.5, 0.2), 2.0),
    ("silence", lambda n: 0.0, 4.0),
]
elapsed, seg_i = 0.0, 0
while elapsed < SECS:
    name, gen, dur = segments[seg_i % len(segments)]
    print(f"t={elapsed:6.1f}s  {name}", flush=True)
    run(int(dur * SR / N), gen)
    elapsed += dur
    seg_i += 1
print(f"SOAK OK: {elapsed:.0f} s of emulated time, preset {PRESET + 1}, no stall", flush=True)
