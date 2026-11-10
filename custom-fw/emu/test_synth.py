"""Icarus Synth (PA_SLOT_SYNTH) end to end, through the emulator DLL.

The focused 5-block chain keeps the synthesizer at slot 1:
    DYN -> SYNTH -> AMP -> CAB -> EQ

This suite drives the REAL firmware path (the emulator DLL) and checks the
things a unit-level synthesizer gate must catch:

  * every one of the 12 factory presets produces a finite, audible, stereo
    voice when SYNTH is on;
  * each preset really changes the dry guitar (a silently bypassed node would
    still pass an "is it finite?" check);
  * bypassing SYNTH while the chain runs stops it immediately - the dry path
    still passes, even before the state is garbage-collected;
  * the block keeps its preset across a live bypass/on cycle.

Usage:
    python custom-fw/emu/test_synth.py [path/to/icarus_emu.dll]

Exit 0 = PASS, 1 = FAIL.
"""
import ctypes
import math
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
DLL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "icarus_emu.dll")

os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_synth_"), "flash.bin")

DYN, SYNTH, AMP, CAB, EQ = range(5)
PRESET_NAMES = ["Metheny Lead", "GR Brass", "Vintage Solo", "Synth Bass",
                "Liquid Lead", "Analog Fuzz", "GR Texture", "Soft Pad",
                "Square Lead", "Dark Sequence", "Pipe Organ", "Pure GR"]

N = 64
SR = 44100
fails = 0


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    fails += 0 if ok else 1


d = ctypes.CDLL(DLL)
d.emu_init()
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float),
                                    ctypes.POINTER(ctypes.c_float),
                                    ctypes.POINTER(ctypes.c_float), ctypes.c_int]
d.emu_set_slot_enabled.argtypes = [ctypes.c_int, ctypes.c_int]
d.emu_set_slot_type.argtypes = [ctypes.c_int, ctypes.c_int]
d.emu_set_slot_order.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
d.emu_set_master_volume.argtypes = [ctypes.c_int]
d.emu_ui_pot.argtypes = [ctypes.c_uint16]

inp = (ctypes.c_float * N)()
outL = (ctypes.c_float * N)()
outR = (ctypes.c_float * N)()


def order_default():
    # emu_set_slot_order takes the full 8-entry array; slots 5..7 are empty.
    buf = (ctypes.c_uint8 * 8)(DYN, SYNTH, AMP, CAB, EQ, 5, 6, 7)
    d.emu_set_slot_order(buf)


def reset():
    d.emu_init()
    d.emu_set_master_volume(100)
    d.emu_ui_pot(2048)          # -20 dB: measure the block, not the limiter
    order_default()
    # The boot chain enables AMP + CAB by default. This suite measures the
    # synthesizer, so start from a chain with every block off.
    for s in range(5):
        d.emu_set_slot_enabled(s, 0)


def sine(freq=220.0, amp=0.2, seconds=1.0):
    return [amp * math.sin(2 * math.pi * freq * i / SR)
            for i in range(int(seconds * SR))]


def run(signal, collect=True):
    left, right = [], []
    for off in range(0, len(signal) - N + 1, N):
        for i in range(N):
            inp[i] = signal[off + i]
        d.emu_process_audio_f32(inp, outL, outR, N)
        if collect:
            left.extend(outL[i] for i in range(N))
            right.extend(outR[i] for i in range(N))
    return left, right


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def finite(x):
    return all(v == v and v not in (float("inf"), float("-inf")) for v in x)


SIG = sine(220.0, 0.2)
print("=== every Icarus factory preset ===")
for p, name in enumerate(PRESET_NAMES):
    reset()
    d.emu_set_slot_enabled(SYNTH, 1)
    d.emu_set_slot_type(SYNTH, p)
    run(SIG[:len(SIG) // 3], collect=False)          # settle
    left, right = run(SIG)
    spread = 0.0
    if rms(left) > 1e-9:
        spread = math.sqrt(sum((a - b) ** 2 for a, b in zip(left, right)) / len(left)) / rms(left)
    check(finite(left) and finite(right), f"[{p:2d}] {name}: finite output")
    check(rms(left) > 1e-4, f"[{p:2d}] {name}: audible (rms {rms(left):.4f})")
    check(spread > 0.005, f"[{p:2d}] {name}: stereo voice (spread {spread:.3f})")

print("\n=== bypass stops the synth immediately (no garbage collection needed) ===")
reset()
d.emu_set_slot_enabled(SYNTH, 1)
d.emu_set_slot_type(SYNTH, 0)
# Reference: the same chain with SYNTH never on.
reset()
ref, _ = run(SIG)

reset()
d.emu_set_slot_enabled(SYNTH, 1)
d.emu_set_slot_type(SYNTH, 0)
run(SIG, collect=False)                              # it is running
d.emu_set_slot_enabled(SYNTH, 0)                     # live bypass, no collect_garbage()
run(SIG[:len(SIG) // 3], collect=False)              # let the one-block click-free fade finish
left, _ = run(SIG)
change_on = math.sqrt(sum((a - b) ** 2 for a, b in zip(left, ref)) / len(left)) / max(rms(ref), 1e-9)
check(change_on < 0.05, f"after the bypass the dry path is back (relative difference {change_on:.3f})")
check(rms(left) > 1e-4, "the bypassed chain still passes the guitar")

print("\n=== the preset survives a live bypass / on cycle ===")
reset()
d.emu_set_slot_enabled(SYNTH, 1)
d.emu_set_slot_type(SYNTH, 4)                        # Liquid Lead
run(SIG[:len(SIG) // 2], collect=False)
on1, _ = run(SIG)
d.emu_set_slot_enabled(SYNTH, 0)
run(SIG, collect=False)
d.emu_set_slot_enabled(SYNTH, 1)
run(SIG[:len(SIG) // 2], collect=False)
on2, _ = run(SIG)
dev = abs(rms(on2) - rms(on1)) / max(rms(on1), 1e-9)
check(dev < 0.15, f"the same preset comes back (level difference {100 * dev:.0f} %)")

print("\n" + ("ICARUS SYNTH OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
