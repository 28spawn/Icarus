"""Render synthetic guitar licks through every Icarus factory preset (via the emulator DLL)
and write 16-bit stereo WAVs for listening.

The "guitar" is a Karplus-Strong plucked string (no recordings), played as three licks:
  lead   - A minor pentatonic phrase in the mid register (single notes)
  riff   - low E-string palm-mute style riff (E2 / G2 / A2)
  chords - strummed Em / G / D / A chords (polyphonic: tests how the mono tracker copes)

Usage:
    python custom-fw/tools/render_synth_licks.py [out_dir]

Default out_dir: custom-fw/qa/renders/synth_licks
"""
import ctypes
import math
import os
import random
import struct
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
DLL = os.path.join(FW, "emu", "pocket_amp_emu.dll")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(FW, "qa", "renders", "synth_licks")
os.makedirs(OUT, exist_ok=True)
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_render_"), "flash.bin")

SR = 44100
N = 64
DYN, SYNTH, AMP, CAB, EQ = range(5)
PRESET_NAMES = ["Metheny Lead", "GR Brass", "Vintage Solo", "Synth Bass",
                "Liquid Lead", "Analog Fuzz", "GR Texture", "Soft Pad",
                "Square Lead", "Dark Sequence", "Octave Machine", "Pure GR"]


# ---------------------------------------------------------------- guitar
def midi_hz(m):
    return 440.0 * 2 ** ((m - 69) / 12.0)


def pluck(freq, dur, amp=0.35, seed=1, damp=0.996, bright=0.5):
    """Karplus-Strong string, lightly low-passed; returns dur seconds of samples."""
    rnd = random.Random(seed)
    n = max(2, int(SR / freq))
    buf = [rnd.uniform(-1, 1) for _ in range(n)]
    # soften the pick: one-pole smoothing of the initial noise burst
    prev = 0.0
    for i in range(n):
        prev = bright * buf[i] + (1 - bright) * prev
        buf[i] = prev
    out = []
    idx = 0
    total = int(dur * SR)
    last = 0.0
    for _ in range(total):
        cur = buf[idx]
        nxt = buf[(idx + 1) % n]
        buf[idx] = damp * 0.5 * (cur + nxt)
        out.append(cur)
        idx = (idx + 1) % n
    peak = max(abs(v) for v in out) or 1.0
    return [v / peak * amp for v in out]


def mix_into(track, samples, start):
    end = start + len(samples)
    if end > len(track):
        track.extend([0.0] * (end - len(track)))
    for i, v in enumerate(samples):
        track[start + i] += v


def lick(notes, tail=1.2):
    """notes: (midi, start_sec, dur_sec[, amp])"""
    track = []
    for k, nt in enumerate(notes):
        m, t0, dur = nt[0], nt[1], nt[2]
        amp = nt[3] if len(nt) > 3 else 0.35
        mix_into(track, pluck(midi_hz(m), dur, amp, seed=100 + k), int(t0 * SR))
    track.extend([0.0] * int(tail * SR))
    # pad to a block multiple
    track.extend([0.0] * ((-len(track)) % N))
    return track


def lead_lick():
    # A minor pentatonic phrase, ends on a held A
    seq = [(69, 0.00, 0.30), (72, 0.30, 0.30), (74, 0.60, 0.30), (76, 0.90, 0.45),
           (74, 1.40, 0.30), (72, 1.70, 0.30), (69, 2.00, 0.30), (67, 2.30, 0.30),
           (69, 2.60, 1.60)]
    return lick(seq)


def riff_lick():
    e, g, a = 40, 43, 45
    seq = []
    t = 0.0
    for m in [e, e, g, e, a, e, g, a]:
        seq.append((m, t, 0.28, 0.4))
        t += 0.25
    seq.append((e, t, 1.6, 0.45))
    return lick(seq)


def chord_lick():
    def strum(notes, t0, amp=0.22):
        return [(m, t0 + 0.02 * i, 1.4, amp) for i, m in enumerate(notes)]
    seq = []
    seq += strum([40, 47, 52, 55, 59, 64], 0.0)     # Em
    seq += strum([43, 47, 50, 55, 59, 67], 1.6)     # G
    seq += strum([50, 57, 62, 66], 3.2)             # D
    seq += strum([45, 52, 57, 61, 64], 4.8)         # A
    return lick(seq, tail=2.0)


LICKS = {"lead": lead_lick(), "riff": riff_lick(), "chords": chord_lick()}


# ---------------------------------------------------------------- emulator
d = ctypes.CDLL(DLL)
d.emu_init()
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float),
                                    ctypes.POINTER(ctypes.c_float), ctypes.c_int]
d.emu_set_slot_enabled.argtypes = [ctypes.c_int, ctypes.c_int]
d.emu_set_slot_type.argtypes = [ctypes.c_int, ctypes.c_int]
d.emu_set_slot_order.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
d.emu_set_master_volume.argtypes = [ctypes.c_int]
d.emu_ui_pot.argtypes = [ctypes.c_uint16]

inp = (ctypes.c_float * N)()
oL = (ctypes.c_float * N)()
oR = (ctypes.c_float * N)()


def reset(synth_preset=None, amp_cab=False):
    d.emu_init()
    d.emu_set_master_volume(100)
    d.emu_ui_pot(4095)
    d.emu_set_slot_order((ctypes.c_uint8 * 8)(DYN, SYNTH, AMP, CAB, EQ, 5, 6, 7))
    for s in range(5):
        d.emu_set_slot_enabled(s, 0)
    if amp_cab:
        d.emu_set_slot_enabled(AMP, 1)
        d.emu_set_slot_enabled(CAB, 1)
    if synth_preset is not None:
        d.emu_set_slot_enabled(SYNTH, 1)
        d.emu_set_slot_type(SYNTH, synth_preset)


def render(sig):
    L, R = [], []
    for off in range(0, len(sig), N):
        for i in range(N):
            inp[i] = sig[off + i]
        d.emu_process_audio_f32(inp, oL, oR, N)
        L.extend(oL[i] for i in range(N))
        R.extend(oR[i] for i in range(N))
    return L, R


def write_wav(path, L, R):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        frames = bytearray()
        for a, b in zip(L, R):
            a = max(-1.0, min(1.0, a))
            b = max(-1.0, min(1.0, b))
            frames += struct.pack("<hh", int(a * 32767), int(b * 32767))
        w.writeframes(bytes(frames))


def peak_rms(L, R):
    pk = max(max(abs(v) for v in L), max(abs(v) for v in R))
    r = math.sqrt(sum(v * v for v in L) / len(L))
    return pk, r


rows = []
# Dry reference of each lick (what the guitar sounds like going in)
for lname, sig in LICKS.items():
    reset(None)
    L, R = render(sig)
    write_wav(os.path.join(OUT, f"00_dry_{lname}.wav"), L, R)
    pk, r = peak_rms(L, R)
    rows.append(("dry", lname, pk, r))

for p, pname in enumerate(PRESET_NAMES):
    for lname, sig in LICKS.items():
        reset(p)
        # settle the synth on silence/first block so the preset is fully loaded
        render(sig[:N * 50])
        reset(p)
        L, R = render(sig)
        fname = f"{p + 1:02d}_{pname.replace(' ', '_')}_{lname}.wav"
        write_wav(os.path.join(OUT, fname), L, R)
        pk, r = peak_rms(L, R)
        rows.append((pname, lname, pk, r))
        print(f"{fname:42s} peak {pk:5.2f}  rms {r:5.3f}" + ("  CLIPS" if pk >= 0.999 else ""))

print(f"\nwrote {len(rows)} files to {OUT}")
