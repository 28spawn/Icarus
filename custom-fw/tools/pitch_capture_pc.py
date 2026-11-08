#!/usr/bin/env python3
"""
custom-fw/tools/pitch_capture_pc.py - Real-time Pitch Detector Calibration & Evaluation Suite

Reads Icarus's real-time pitch tracker telemetry ring over the USB-MIDI diagnostic channel (RAM read 0x7D 0x02)
while you play prompted guitar shapes, and scores what the tracker reported against the ground truth.

Features:
  - Auto-detects USB-MIDI port (or accepts --port)
  - Batch-reads RAM telemetry ring in contiguous chunks (no MIDI queue saturation)
  - Throttled 40 Hz polling loop (safe for microcontroller CPU governor)
  - Evaluates fundamental tracking, octave jumps (+/-1200c), 5th harmonics (+/-700c/1900c),
    wrong notes, re-trigger count, pick-to-lock latency, tracking confidence, decay floor (dB), and jitter
  - Live interactive terminal countdown and tracking readout
  - Graceful Ctrl+C handling (retains and reports partial results)

Usage:
    python custom-fw/tools/pitch_capture_pc.py
    python custom-fw/tools/pitch_capture_pc.py --port "Pocket AMP"
    python custom-fw/tools/pitch_capture_pc.py --list "E2,A2,G3,E2+B2,Em" --hold 3.5 --gap 2.0 --save qa/pitch_calib.json
    python custom-fw/tools/pitch_capture_pc.py --analyse qa/pitch_calib.json
"""

from __future__ import annotations
import argparse
import json
import struct
import sys
import time
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

PTR_ADDR = 0x01C7C9B4
BLOCK_S = 64 / 44100.0
NOTE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi(name: str) -> int:
    """Parse note name like 'E2', 'F#3', 'Bb1' to MIDI note number."""
    n = NOTE[name[0].upper()]
    i = 1
    while i < len(name) and name[i] in "#b":
        n += 1 if name[i] == "#" else -1
        i += 1
    octave = int(name[i:])
    return 12 * (octave + 1) + n


def hz(mi: int, ref_a4: float = 440.0) -> float:
    return ref_a4 * (2.0 ** ((mi - 69) / 12.0))


SHAPES = {
    "E2": ("open low E string, single note", ["E2"]),
    "A2": ("open A string, single note", ["A2"]),
    "D3": ("open D string, single note", ["D3"]),
    "G3": ("open G string, single note", ["G3"]),
    "B3": ("open B string, single note", ["B3"]),
    "E4": ("open high E string, single note", ["E4"]),
    "E2+B2": ("power chord: open low E string + 2nd fret of A string (B2)", ["E2", "B2"]),
    "A5": ("power chord A2+E3 (open A string + 2nd fret D string)", ["A2", "E3"]),
    "A5o": ("A5 with octave: A2+E3+A3 (open A, 2nd fret D, 2nd fret G)", ["A2", "E3", "A3"]),
    "G5": ("power chord G2+D3 (3rd fret 6th string + 5th fret 5th string)", ["G2", "D3"]),
    "Em": ("open E minor chord, all six strings", ["E2", "B2", "E3", "G3", "B3", "E4"]),
    "G": ("open G major chord, all six strings", ["G2", "B2", "D3", "G3", "B3", "G4"]),
    "C": ("open C major chord, strings 5..1", ["C3", "E3", "G3", "C4", "E4"]),
    "D": ("open D major chord, strings 4..1", ["D3", "A3", "D4", "F#4"]),
    "Am": ("open A minor chord, strings 5..1", ["A2", "E3", "A3", "C4", "E4"]),
    "G3+B3": ("dyad: open G and open B strings together", ["G3", "B3"]),
    "A3+B3": ("dyad: 2nd fret G string (A3) + open B string (B3)", ["A3", "B3"]),
    "ring": ("let open low E ring, then PICK open A once while ringing", ["A2", "E2"]),
}
DEFAULT = ["E2", "A2", "G3", "E2+B2", "A5", "A5o", "G5", "Em", "G", "C", "D", "Am", "G3+B3", "ring"]


def find_midi_port(preferred: str = "") -> str:
    """Find a suitable USB-MIDI port with auto-detection fallback."""
    import rtmidi
    mo, mi = rtmidi.MidiOut(), rtmidi.MidiIn()
    outs = mo.get_ports()
    ins = mi.get_ports()

    if preferred:
        oi = next((p for p in outs if preferred.lower() in p.lower()), None)
        ii = next((p for p in ins if preferred.lower() in p.lower()), None)
        if oi and ii:
            return preferred
        raise SystemExit(f"No MIDI port matching '{preferred}'.\nAvailable outputs: {outs}\nAvailable inputs: {ins}")

    candidates = ["custom midi", "pocket amp", "icarus", "alpha"]
    for c in candidates:
        oi = next((p for p in outs if c in p.lower()), None)
        ii = next((p for p in ins if c in p.lower()), None)
        if oi and ii:
            return c

    # Fallback to any non-synth port
    valid_outs = [p for p in outs if "wavetable" not in p.lower()]
    if valid_outs and ins:
        return valid_outs[0]

    raise SystemExit(f"No compatible MIDI unit detected.\nAvailable outputs: {outs}\nAvailable inputs: {ins}")


def read_events(u: m.Unit, ptr: int, seen: int):
    """Batch-reads newly added pitch telemetry events from the unit's RAM ring buffer."""
    h = struct.unpack("<IIII", u.ram(ptr, 16))
    if h[0] != 0x4B525450:
        raise RuntimeError("No telemetry ring signature (is an OSC / HYBRID / GUITAR preset active?)")
    wr, blocks, n = h[1], h[2], h[3]
    new = []
    start = max(seen, wr - n + 1) if wr > seen else seen
    count = wr - start

    if count > 0:
        s_idx = start % n
        e_idx = wr % n
        ev_base = ptr + 16

        if s_idx < e_idx and count <= n:
            raw_bytes = u.ram(ev_base + s_idx * 12, count * 12)
            for k in range(count):
                blk, f, raw, fl, conf, mi, lv = struct.unpack_from("<IHHBBBB", raw_bytes, k * 12)
                new.append(dict(i=start + k, blk=blk, f=f / 32.0, raw=raw / 32.0,
                                flags=fl, conf=conf / 255.0, midi=mi, lvl=lv / 2.0 - 96.0))
        else:
            # Buffer wraps around end: read two slices
            first_n = n - s_idx
            second_n = count - first_n
            raw1 = u.ram(ev_base + s_idx * 12, first_n * 12)
            raw2 = u.ram(ev_base, second_n * 12) if second_n > 0 else b""
            combined = raw1 + raw2
            for k in range(count):
                blk, f, raw, fl, conf, mi, lv = struct.unpack_from("<IHHBBBB", combined, k * 12)
                new.append(dict(i=start + k, blk=blk, f=f / 32.0, raw=raw / 32.0,
                                flags=fl, conf=conf / 255.0, midi=mi, lvl=lv / 2.0 - 96.0))

    lost = (start - seen) if wr > seen else 0
    return new, wr, blocks, lost


def timeline(events: list[dict], t_a: float, t_b: float, blk_to_t):
    """Piecewise-constant (t_start, t_end, freq, valid, tracker_state) clipped to [t_a, t_b]."""
    seg = []
    cur = None
    for e in events:
        t = blk_to_t(e["blk"])
        if t < t_a:
            cur = (t_a, e["f"], e["flags"] & 1, (e["flags"] >> 4) & 3)
            continue
        if t > t_b:
            break
        if cur is not None:
            seg.append((cur[0], t, cur[1], cur[2]))
        cur = (t, e["f"], e["flags"] & 1, (e["flags"] >> 4) & 3)
    if cur is not None:
        seg.append((cur[0], t_b, cur[1], cur[2]))
    return seg


def score(name: str, notes: list[str], events: list[dict], t_play: float, t_end: float, blk_to_t, ref_a4: float = 440.0) -> dict:
    """Calculates comprehensive pitch detector calibration metrics."""
    exp = [hz(midi(n), ref_a4) for n in notes]
    target_root = exp[0]
    seg = timeline(events, t_play, t_end, blk_to_t)

    tot = 0.0
    root_dur = 0.0
    octave_dur = 0.0
    fifth_dur = 0.0
    other_chord_dur = 0.0
    wrong_dur = 0.0

    for (a, b, f, v) in seg:
        d = b - a
        if not v or f <= 0:
            continue
        tot += d
        cents_root = 1200.0 * np.log2(f / target_root)
        abs_c = abs(cents_root)

        if abs_c < 40.0:
            root_dur += d
        elif abs(abs_c - 1200.0) < 45.0 or abs(abs_c - 2400.0) < 45.0:
            octave_dur += d
        elif abs(abs_c - 700.0) < 45.0 or abs(abs_c - 1900.0) < 45.0:
            fifth_dur += d
        else:
            chord_cents = [abs(1200.0 * np.log2(f / x)) for x in exp[1:]]
            if chord_cents and min(chord_cents) < 40.0:
                other_chord_dur += d
            else:
                wrong_dur += d

    onsets = [e for e in events if t_play <= blk_to_t(e["blk"]) <= t_end and (e["flags"] & 2)]
    pick = next((blk_to_t(e["blk"]) for e in events if t_play <= blk_to_t(e["blk"]) <= t_end and ((e["flags"] >> 4) & 3) == 1), None)

    first_root = None
    for (a, b, f, v) in seg:
        if v and f > 0 and abs(1200.0 * np.log2(f / target_root)) < 40.0:
            first_root = a
            break

    lat = (first_root - pick) * 1000.0 if (pick is not None and first_root is not None) else None

    win_evs = [e for e in events if t_play <= blk_to_t(e["blk"]) <= t_end]
    avg_conf = float(np.mean([e["conf"] for e in win_evs])) * 100.0 if win_evs else 0.0
    valid_evs = [e for e in win_evs if e["flags"] & 1]
    min_lvl = float(np.min([e["lvl"] for e in valid_evs])) if valid_evs else -96.0

    raw_cents = [abs(1200.0 * np.log2(e["f"] / e["raw"])) for e in win_evs if e["f"] > 0 and e["raw"] > 0]
    jitter = float(np.median(raw_cents)) if raw_cents else 0.0

    window_dur = t_end - t_play
    unvoiced_dur = max(0.0, window_dur - tot)

    return dict(
        name=name,
        sounding=tot,
        unvoiced_pct=(unvoiced_dur / window_dur * 100.0) if window_dur else 0.0,
        root=root_dur / tot if tot else 0.0,
        octave=octave_dur / tot if tot else 0.0,
        fifth=fifth_dur / tot if tot else 0.0,
        other=other_chord_dur / tot if tot else 0.0,
        wrong=wrong_dur / tot if tot else 0.0,
        onsets=len(onsets),
        latency_ms=lat,
        avg_conf=avg_conf,
        min_lvl_db=min_lvl,
        jitter_cents=jitter,
        played=notes,
        heard=sorted({round(f) for (_, _, f, v) in seg if v})[:10],
    )


def report(rows: list[dict]):
    """Prints a structured calibration summary table."""
    print(f"\n{'shape':8s} {'sound':>6s} {'root':>6s} {'octave':>7s} {'5th':>6s} {'wrong':>6s} {'onsets':>6s} {'latency':>9s} {'conf':>6s} {'decay':>6s}   heard (Hz)")
    print("-" * 92)
    for r in rows:
        lat = f"{r['latency_ms']:.0f} ms" if r["latency_ms"] is not None else "--"
        print(f"{r['name']:8s} {r['sounding']:5.1f}s {r['root'] * 100:5.0f}% {r['octave'] * 100:6.0f}% "
              f"{r['fifth'] * 100:5.0f}% {r['wrong'] * 100:5.0f}% {r['onsets']:6d} {lat:>9s} "
              f"{r['avg_conf']:5.0f}% {r['min_lvl_db']:5.0f}dB   {r['heard']}")
    if rows:
        print("-" * 92)
        print(f"{'MEAN':8s} {'':6s} {np.mean([r['root'] for r in rows]) * 100:5.0f}% "
              f"{np.mean([r['octave'] for r in rows]) * 100:6.0f}% "
              f"{np.mean([r['fifth'] for r in rows]) * 100:5.0f}% "
              f"{np.mean([r['wrong'] for r in rows]) * 100:5.0f}% "
              f"{int(np.mean([r['onsets'] for r in rows])):6d} "
              f"{'':>9s} {np.mean([r['avg_conf'] for r in rows]):5.0f}%")


def main():
    ap = argparse.ArgumentParser(description="Real-time Pitch Tracker Calibration for Icarus")
    ap.add_argument("--port", default="", help="MIDI port name substring (default: auto-detect)")
    ap.add_argument("--list", default=",".join(DEFAULT), help="Comma-separated shapes: " + ",".join(SHAPES))
    ap.add_argument("--hold", type=float, default=3.0, help="Seconds to let each shape ring (default: 3.0)")
    ap.add_argument("--gap", type=float, default=2.0, help="Seconds of preparation silence before each shape (default: 2.0)")
    ap.add_argument("--repeat", type=int, default=2, help="Repetitions for each shape (default: 2)")
    ap.add_argument("--ref-a4", type=float, default=440.0, help="Reference tuning frequency for A4 in Hz (default: 440.0)")
    ap.add_argument("--preset", type=int, default=-1, help="Send MIDI Program Change to select preset before starting (e.g. 6 or 9)")
    ap.add_argument("--save", default="", help="Path to save JSON telemetry and score results")
    ap.add_argument("--analyse", default="", help="Analyse previously saved JSON run and exit")
    a = ap.parse_args()

    if a.analyse:
        z = json.load(open(a.analyse))
        c0, c1 = z["fit"]
        b2t = lambda b: c0 + c1 * b
        ref_a4 = z.get("ref_a4", 440.0)
        rows = [score(w["name"], w["notes"], z["events"], w["t0"], w["t1"], b2t, ref_a4) for w in z["windows"]]
        report(rows)
        return 0

    port_name = find_midi_port(a.port)
    print(f"Connecting to MIDI device: '{port_name}' ...", flush=True)
    u = m.Unit(port_name)
    build_str = u.build_id()
    print(f"Firmware Build: {build_str}", flush=True)

    if a.preset >= 0:
        print(f"Switching unit to Preset {a.preset} ...", flush=True)
        u.mo.send_message([0xC0, a.preset & 0x7F])
        time.sleep(0.3)

    ptr = struct.unpack("<I", u.ram(PTR_ADDR, 4))[0]
    if ptr < 0x01C00000 or ptr > 0x01C80000:
        print("\n[!] No active telemetry pointer at 0x01C7C9B4.")
        print("    Ensure the unit is running Icarus with Icarus synth enabled,")
        print("    and open an OSC / HYBRID / GUITAR preset (e.g. Preset 6 or 9).\n")
        return 2

    events, seen, pairs = [], 0, []

    def poll():
        nonlocal seen
        t = time.time()
        new, wr, blocks, lost = read_events(u, ptr, seen)
        pairs.append((t, blocks))
        events.extend(new)
        seen = wr
        if lost:
            print(f"   (ring overrun: {lost} events lost)", flush=True)

    poll()
    names = [s.strip() for s in a.list.split(",") if s.strip()] * a.repeat
    wins = []

    print("\n========================================================")
    print(" Icarus Pitch Detector Calibration Suite")
    print("========================================================")
    print("Instructions:")
    print("  1. Watch the countdown and prepare your hand on the fretboard.")
    print("  2. When 'STRUM NOW' appears, pick/strum firmly once and let ring.")
    print("  3. When 'MUTE' appears, mute the strings completely.")
    print("  Press Ctrl+C at any time to finish and evaluate current progress.\n")

    try:
        for idx, name in enumerate(names):
            desc, notes = SHAPES.get(name, ("custom shape", [name]))
            print(f"\n[{idx + 1}/{len(names)}] NEXT: {name}  -  {desc}")
            print(f"      Target Note(s): {' '.join(notes)}")

            # Preparation Gap with Countdown
            t_end_gap = time.time() + a.gap
            while time.time() < t_end_gap:
                rem = t_end_gap - time.time()
                sys.stdout.write(f"\r  >> Get ready: {rem:3.1f}s ...   ")
                sys.stdout.flush()
                poll()
                time.sleep(0.025)

            # Strum prompt
            sys.stdout.write(f"\r  >>> [ STRUM NOW: {' '.join(notes)} ] <<<\n")
            sys.stdout.flush()
            t0 = time.time()
            t_end_hold = t0 + a.hold

            # Hold / Ringing Loop with Live Tracker Feedback
            while time.time() < t_end_hold:
                elapsed = time.time() - t0
                last_info = ""
                if events:
                    last_ev = events[-1]
                    if last_ev["flags"] & 1 and last_ev["f"] > 0:
                        last_info = f"tracking {last_ev['f']:.1f} Hz (conf {last_ev['conf'] * 100:.0f}%, {last_ev['lvl']:.0f} dB)"
                sys.stdout.write(f"\r  [RINGING {elapsed:3.1f}s / {a.hold:.1f}s] {last_info}   ")
                sys.stdout.flush()
                poll()
                time.sleep(0.025)

            t1 = time.time()
            sys.stdout.write("\r  -- MUTE STRINGS --                             \n")
            sys.stdout.flush()
            wins.append(dict(name=name, notes=notes, t0=t0 + 0.05, t1=t1))

        t_fin = time.time() + 0.5
        while time.time() < t_fin:
            poll()
            time.sleep(0.025)

    except KeyboardInterrupt:
        print("\n\n[!] Test interrupted by user. Finalizing completed shapes ...", flush=True)

    if not pairs or not wins:
        print("No test data collected.")
        return 1

    # Fit host time to block counter
    tt = np.array([p[0] for p in pairs])
    bb = np.array([p[1] for p in pairs], dtype=float)
    off = tt - bb * BLOCK_S
    c0 = float(np.percentile(off, 5))
    c1 = BLOCK_S
    b2t = lambda b: c0 + c1 * b

    rows = [score(w["name"], w["notes"], events, w["t0"], w["t1"], b2t, a.ref_a4) for w in wins]
    report(rows)

    if a.save:
        out_path = Path(a.save)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        json.dump(dict(events=events, windows=wins, fit=[c0, c1], ref_a4=a.ref_a4), open(out_path, "w"), indent=2)
        print(f"\nSaved calibration results to {out_path.resolve()}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
