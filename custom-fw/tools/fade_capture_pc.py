#!/usr/bin/env python3
"""
custom-fw/tools/fade_capture_pc.py - do the module on/off switches crack?  Records the unit's USB recording endpoint (the samples the firmware hands the codec) while this tool switches each module
on and off by MIDI (CC 28..35, "set on / off") at known times, and measures the discontinuities around every switch: the largest second difference  x[n] - 2 x[n-1] + x[n-2]  against the 99.9th
percentile of the same measure on the steady signal before the switch, and the level step.  A steady signal must go through the unit (--tone-device plays one on a PC output cabled to its input).

    python custom-fw/tools/fade_capture_pc.py --tone-device "Line 1/2 (HeadRush" [--modules dyn,amp,synth,cab,eq] [--rounds 6] [--every 1.5] [--preset 12] [--save qa/fade.npz] [--wav qa/fade.wav]
    python custom-fw/tools/fade_capture_pc.py --analyse qa/fade.npz
"""
import argparse, sys, time
from pathlib import Path
import numpy as np
import sounddevice as sd
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m

SR = 44100
CC = {"dyn": 28, "amp": 29, "synth": 30, "cab": 33, "eq": 35}


def analyse(x, t0, ev, limit):
    d2 = np.concatenate(([0.0, 0.0], np.abs(x[2:] - 2 * x[1:-1] + x[:-2])))
    out = []
    for (ts, name, on) in ev:
        i = int((ts - t0) * SR)
        if i < int(0.5 * SR) or i + int(0.4 * SR) > len(x): continue
        pre = d2[i - int(0.40 * SR): i - int(0.02 * SR)]
        ref = float(np.percentile(pre, 99.9)) + 1e-9
        seg = d2[i - int(0.002 * SR): i + int(0.12 * SR)]
        j = int(np.argmax(seg)); worst = float(seg[j])
        over = np.where(seg > limit * ref)[0]
        r_pre = float(np.sqrt(np.mean(x[i - int(0.30 * SR): i - int(0.02 * SR)] ** 2)))
        r_post = float(np.sqrt(np.mean(x[i + int(0.25 * SR): i + int(0.40 * SR)] ** 2)))
        out.append(dict(name=name, on=on, ratio=worst / ref, at_ms=(j - int(0.002 * SR)) * 1000 / SR, n_over=len(over),
                        span_ms=(over[-1] - over[0]) * 1000 / SR if len(over) else 0.0, pre=r_pre, post=r_post))
    return out


def report(rows, limit):
    print(f"\n{len(rows)} switches measured (glitch = worst step over {limit:.0f}x the steady 99.9th percentile)")
    print(f"{'module':7s} {'->':3s} {'worst step':>11s} {'at ms':>7s} {'glitch ms':>10s} {'level before':>13s} {'after':>9s}")
    for r in rows:
        flag = "  <== CRACK" if r["n_over"] else ""
        print(f"{r['name']:7s} {'on' if r['on'] else 'off':3s} {r['ratio']:10.1f}x {r['at_ms']:7.1f} {r['span_ms']:10.1f} {r['pre']:13.4f} {r['post']:9.4f}{flag}")
    for name in sorted({r["name"] for r in rows}):
        rr = [r for r in rows if r["name"] == name]
        bad = [r for r in rr if r["n_over"]]
        print(f"  {name:6s}: {len(bad)} of {len(rr)} switches glitched, median worst step {np.median([r['ratio'] for r in rr]):.1f}x, max {max(r['ratio'] for r in rr):.1f}x")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modules", default="dyn,amp,synth,cab,eq")
    ap.add_argument("--rounds", type=int, default=6, help="on/off cycles per module")
    ap.add_argument("--every", type=float, default=1.5, help="seconds between switches")
    ap.add_argument("--preset", type=int, default=-1, help="load this preset (0..79) first; -1 = leave the current one")
    ap.add_argument("--limit", type=float, default=6.0)
    ap.add_argument("--device", default="Custom MIDI")
    ap.add_argument("--tone-device", default="")
    ap.add_argument("--tone-level", type=float, default=0.05)
    ap.add_argument("--save", default="")
    ap.add_argument("--wav", default="")
    ap.add_argument("--analyse", default="")
    a = ap.parse_args()
    if a.analyse:
        z = np.load(a.analyse)
        ev = [(float(t), str(n), bool(o)) for t, n, o in zip(z["ts"], z["names"], z["ons"])]
        report(analyse(z["x"][:, 0].astype(np.float64), float(z["t0"]), ev, a.limit), a.limit)
        return 0
    cand = [(i, d) for i, d in enumerate(sd.query_devices()) if a.device.lower() in d["name"].lower() and d["max_input_channels"] > 0 and sd.query_hostapis(d["hostapi"])["name"] == "MME"]
    if not cand:
        print("no recording device containing", a.device); return 2
    u = m.Unit("Custom MIDI")
    print("BUILD", u.build_id(), flush=True)
    chunks, t_first = [], [None]

    def cb(indata, frames, tinfo, status):
        if t_first[0] is None: t_first[0] = time.time() - frames / SR
        chunks.append(indata.copy())
    tone = None
    if a.tone_device:
        od = [(i, d) for i, d in enumerate(sd.query_devices()) if a.tone_device.lower() in d["name"].lower() and d["max_output_channels"] > 0 and sd.query_hostapis(d["hostapi"])["name"] == "MME"]
        if not od:
            print("no output device containing", a.tone_device); return 2
        ph = [0]

        def tcb(outdata, frames, tinfo, status):
            t = (ph[0] + np.arange(frames)) / SR
            outdata[:, :] = (a.tone_level * (np.sin(2 * np.pi * 110 * t) + 0.5 * np.sin(2 * np.pi * 220 * t + 0.7) + 0.3 * np.sin(2 * np.pi * 330 * t + 1.9)) / 1.8)[:, None]
            ph[0] += frames
        tone = sd.OutputStream(device=od[0][0], channels=2, samplerate=SR, callback=tcb, dtype="float32")
        tone.start(); print("test tone on", od[0][1]["name"], flush=True); time.sleep(1.0)
    ev = []
    mods = [s.strip() for s in a.modules.split(",") if s.strip()]
    with sd.InputStream(device=cand[0][0], channels=2, samplerate=SR, callback=cb, dtype="float32"):
        time.sleep(1.0)
        if a.preset >= 0:
            u.mo.send_message([0xC0, a.preset]); time.sleep(2.5)
        for name in mods:
            cc = CC[name]
            u.mo.send_message([0xB0, cc, 127]); time.sleep(a.every)          # make sure it starts ON
            for _ in range(a.rounds):
                for on in (False, True):
                    ev.append((time.time(), name, on)); u.mo.send_message([0xB0, cc, 127 if on else 0]); time.sleep(a.every)
        time.sleep(0.8)
    if tone: tone.stop()
    x2 = np.concatenate(chunks)
    x = x2[:, 0].astype(np.float64)
    print(f"recorded {len(x) / SR:.1f} s, peak {np.abs(x).max():.4f}, rms {np.sqrt(np.mean(x ** 2)):.5f}", flush=True)
    if a.save:
        np.savez(a.save, x=x2, t0=t_first[0], ts=np.array([e[0] for e in ev]), names=np.array([e[1] for e in ev]), ons=np.array([e[2] for e in ev]))
    if a.wav:
        import wave
        with wave.open(a.wav, "wb") as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes((np.clip(x2, -1, 1) * 32767).astype("<i2").tobytes())
    report(analyse(x, t_first[0], ev, a.limit), a.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
