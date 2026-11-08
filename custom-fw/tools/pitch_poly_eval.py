"""custom-fw/tools/pitch_poly_eval.py - what does the (monophonic) pitch tracker say when two or more strings sound at once?

Scenarios (Karplus-Strong strings, known pitches): dyads of every common interval, power chords, triads / open-position chords strummed with a few ms between strings, and a picked note over a
string that is still ringing sympathetically (a few dB down). For each: the share of the voiced time the tracker reports the LOWEST note (the root), another chord note, or something else
(an octave / a sub-harmonic / a pitch that is not in the chord), and how many times it re-triggered.

  python tools/pitch_poly_eval.py [--preset 0] [--seeds 3]
"""
import argparse, sys, tempfile
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pitch_eval import build_exe, run_exe, pluck, SR, BLK


def hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def chord(rng, midis, strum_ms=0.0, amps=None, dur=1.0, extra_ring=None):
    """midis: notes sounded (lowest first); strum_ms between successive strings; extra_ring: (midi, dB) a string already ringing under the whole thing."""
    n = int((dur + 0.3) * SR)
    x = np.zeros(n)
    t0 = int(0.25 * SR)
    for i, m in enumerate(midis):
        a = -14.0 if amps is None else amps[i]
        s = pluck(hz(m), dur, a, rng, bright=float(rng.uniform(0.35, 0.7)), decay=float(rng.uniform(0.9978, 0.9990)))
        o = t0 + int(i * strum_ms * 1e-3 * SR)
        x[o:o + len(s)] += s[: n - o]
    if extra_ring:
        m, db = extra_ring
        s = pluck(hz(m), dur + 0.25, db, rng, bright=0.4, decay=0.9992)
        x[: len(s)] += s[:n]
    x += 10 ** (-62 / 20) * rng.standard_normal(n)
    return x.astype(np.float32), t0


def judge(rows, midis, t_start):
    """classify every voiced block after the pick (+30 ms) by the pitch reported"""
    f = rows["freq"]; v = rows["valid"]; t = rows["t"]; on = rows["onset"]
    sel = (t >= t_start + 0.03) & (v > 0)
    ff = f[sel]
    n = int(sel.sum())
    if n == 0:
        return dict(n=0, root=0, other_note=0, off=0, onsets=int(on[t >= t_start].sum()), lat=None)
    cents = lambda a, b: 1200 * np.log2(a / b)
    root = np.abs(cents(ff, hz(midis[0]))) < 40
    inchord = np.zeros(len(ff), bool)
    for m in midis[1:]:
        inchord |= np.abs(cents(ff, hz(m))) < 40
    other = inchord & ~root
    off = ~root & ~inchord
    # latency to a valid root
    tv = t[(t >= t_start) & (v > 0) & (np.abs(cents(np.maximum(f, 1), hz(midis[0]))) < 40)]
    return dict(n=n, root=root.mean(), other_note=other.mean(), off=off.mean(), onsets=int(on[t >= t_start].sum()),
                lat=(tv[0] - t_start) * 1000 if len(tv) else None)


SCEN = [
    ("dyad  minor 3rd  E2+G2", [40, 43], 0), ("dyad  major 3rd  E2+G#2", [40, 44], 0), ("dyad  4th  A2+D3", [45, 50], 0),
    ("dyad  5th  E2+B2", [40, 47], 0), ("dyad  minor 6th  A2+F3", [45, 53], 0), ("dyad  major 6th  A2+F#3", [45, 54], 0),
    ("dyad  minor 7th  A2+G3", [45, 55], 0), ("dyad  octave  E2+E3", [40, 52], 0), ("dyad  tritone  A2+Eb3", [45, 51], 0),
    ("dyad  high 3rd  G3+B3", [55, 59], 0), ("dyad  high 6th  D4+B4", [62, 71], 0), ("dyad  2nd  A3+B3", [57, 59], 0),
    ("power E5  E2+B2+E3 (strum 8ms)", [40, 47, 52], 8), ("power A5  A2+E3+A3 (strum 8ms)", [45, 52, 57], 8),
    ("Em open  (6 strings, strum 12ms)", [40, 47, 52, 55, 59, 64], 12), ("G open  (strum 12ms)", [43, 47, 50, 55, 59, 67], 12),
    ("C open  (5 strings, strum 12ms)", [48, 52, 55, 60, 64], 12), ("D triad  D3+A3+D4+F#4", [50, 57, 62, 66], 10),
    ("Am triad  A3+C4+E4", [57, 60, 64], 6), ("B7 jazz  B2+D#3+A3+C#4", [47, 51, 57, 61], 10),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", type=int, default=0)
    ap.add_argument("--seeds", type=int, default=3)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        exe = build_exe(tmp)
        print(f"{'scenario':38s} {'root':>6s} {'other':>6s} {'off':>6s} {'onsets':>7s} {'lat ms':>7s}")
        tot = []
        for name, midis, strum in SCEN:
            acc = []
            for sd in range(a.seeds):
                rng = np.random.default_rng(100 + sd)
                x, t0 = chord(rng, midis, strum_ms=strum)
                rows = run_exe(exe, x, a.preset, tmp, "c")
                acc.append(judge(rows, midis, t0 / SR))
            g = lambda k: np.mean([r[k] for r in acc if r["n"]] or [0])
            lat = [r["lat"] for r in acc if r["lat"] is not None]
            print(f"{name:38s} {g('root')*100:5.0f}% {g('other_note')*100:5.0f}% {g('off')*100:5.0f}% {np.mean([r['onsets'] for r in acc]):7.1f} {np.mean(lat) if lat else float('nan'):7.0f}")
            tot.append((g('root'), g('other_note'), g('off')))
        print(f"{'MEAN':38s} {np.mean([t[0] for t in tot])*100:5.0f}% {np.mean([t[1] for t in tot])*100:5.0f}% {np.mean([t[2] for t in tot])*100:5.0f}%")
        print("\nsympathetic ring: a picked note (-14 dB) over a string already ringing (dB below the pick)")
        print(f"{'scenario':38s} {'root':>6s} {'other':>6s} {'off':>6s} {'onsets':>7s} {'lat ms':>7s}")
        for name, m, ring in [("A2 over ringing E3, ring -12 dB", 45, (52, -26)), ("A2 over ringing E3, ring -20 dB", 45, (52, -34)), ("E2 over ringing B2, ring -15 dB", 40, (47, -29)),
                              ("G3 over ringing D3, ring -12 dB", 55, (50, -26)), ("A3 over ringing E2, ring -12 dB", 57, (40, -26)), ("D3 over ringing A3, ring -12 dB", 50, (57, -26)),
                              ("A3 over ringing E2, ring -6 dB", 57, (40, -20)), ("E3 over ringing G#2, ring -20 dB", 52, (44, -34))]:
            acc = []
            for sd in range(a.seeds):
                rng = np.random.default_rng(200 + sd)
                x, t0 = chord(rng, [m], extra_ring=ring)
                rows = run_exe(exe, x, a.preset, tmp, "c")
                acc.append(judge(rows, [m, ring[0]], t0 / SR))
            g = lambda k: np.mean([r[k] for r in acc if r["n"]] or [0])
            lat = [r["lat"] for r in acc if r["lat"] is not None]
            print(f"{name:38s} {g('root')*100:5.0f}% {g('other_note')*100:5.0f}% {g('off')*100:5.0f}% {np.mean([r['onsets'] for r in acc]):7.1f} {np.mean(lat) if lat else float('nan'):7.0f}")


if __name__ == "__main__":
    main()
