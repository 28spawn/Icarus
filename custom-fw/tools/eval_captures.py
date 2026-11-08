#!/usr/bin/env python3
"""
custom-fw/tools/eval_captures.py - judge Icarus Synth on DRY guitar captures: how well the pitch tracker follows the playing, how late it is, whether it invents notes in the silences,
and how loud / clean the synth voice is. The synth code is the firmware's own (tools/icarus_capture_eval.c, a host build of src/effects/icarus).

    python custom-fw/tools/eval_captures.py "C:/path/Dry Captures" [--presets 0,3,5] [--sens 0.5] [--smooth 0.35] [--render out_dir]

For each capture a reference pitch track is computed offline (a careful YIN on 2048-sample frames), then compared to what the unit's tracker said, block by block:
  voiced     frames where the reference is confident and the signal is above -45 dB
  ok         tracker valid and within 50 cents of the reference
  oct        tracker valid but an octave (or a fifth+octave) off
  miss       tracker not valid on a voiced frame
  false      tracker valid while the reference is silent (< -55 dB)
  latency    from a reference onset to the first block where the tracker is valid and right
and the voice: peak, rms, clipped blocks.   --render writes the stereo output of each (capture, preset) as a 16-bit wav for listening.
"""
import argparse, glob, os, subprocess, sys, tempfile, wave
from pathlib import Path
import numpy as np
import soundfile as sf

FW = Path(__file__).resolve().parent.parent
SRC = FW / "src" / "effects" / "icarus"
SR = 44100
BLK = 64


def build_exe(tmp: Path) -> Path:
    exe = tmp / "ace.exe"
    srcs = [str(p) for p in sorted(SRC.glob("icarus*.c"))]
    cmd = ["clang", "-O2", "-Wno-unused", f"-I{FW / 'include'}", f"-I{SRC}", "-DPA_HOST_SIM=1", "-DBOARD_POCKET_AMP=1", str(FW / "tools" / "icarus_capture_eval.c"), *srcs, "-o", str(exe), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("compile failed:\n" + r.stderr[-2000:])
    return exe


def yin_ref(x: np.ndarray, fmin=70.0, fmax=1400.0, win=2048, hop=256, thr=0.12):
    """Offline reference f0 (Hz, 0 = unvoiced) per hop, plus rms in dB."""
    n = (len(x) - win) // hop
    f0 = np.zeros(n); db = np.full(n, -120.0); conf = np.zeros(n)
    lo, hi = int(SR / fmax), int(SR / fmin)
    for i in range(n):
        fr = x[i * hop:i * hop + win].astype(np.float64)
        rms = np.sqrt(np.mean(fr * fr)) + 1e-12
        db[i] = 20 * np.log10(rms)
        if db[i] < -55:
            continue
        # difference function via FFT autocorrelation
        w = win // 2
        a = fr[:w];
        fa = np.fft.rfft(fr, 2 * win)
        r = np.fft.irfft(fa * np.conj(np.fft.rfft(a, 2 * win)))[:hi + 2]
        cs = np.concatenate([[0], np.cumsum(fr * fr)])
        e0 = cs[w] - cs[0]
        dd = np.zeros(hi + 2)
        for t in range(1, hi + 2):
            et = cs[t + w] - cs[t]
            dd[t] = e0 + et - 2 * r[t]
        cm = np.ones(hi + 2)
        run = np.cumsum(dd[1:])
        cm[1:] = dd[1:] * np.arange(1, hi + 2) / np.maximum(run, 1e-12)
        t = lo
        pick = 0
        while t < hi:
            if cm[t] < thr:
                while t + 1 < hi and cm[t + 1] < cm[t]:
                    t += 1
                pick = t
                break
            t += 1
        if pick:
            y0, y1, y2 = cm[pick - 1], cm[pick], cm[pick + 1]
            den = y0 - 2 * y1 + y2
            off = 0.5 * (y0 - y2) / den if abs(den) > 1e-9 else 0.0
            f0[i] = SR / (pick + max(-0.5, min(0.5, off)))
            conf[i] = 1 - cm[pick]
    return f0, db, conf


def cents(a, b):
    return 1200.0 * np.log2(np.maximum(a, 1e-3) / np.maximum(b, 1e-3))


def run_one(exe: Path, mono: np.ndarray, preset: int, tmp: Path, sens, smooth, tag):
    raw = tmp / (tag + "_in.f32")
    mono.astype(np.float32).tofile(raw)
    pre = tmp / tag
    cmd = [str(exe), str(raw), str(preset), str(pre), str(sens), str(smooth)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"{cmd} failed: {r.stderr}")
    rows = np.genfromtxt(str(pre) + ".csv", delimiter=",", names=True)
    out = np.fromfile(str(pre) + ".f32", dtype=np.float32).reshape(-1, 2)
    return rows, out


def evaluate(name, mono, rows, out, ref_f0, ref_db):
    # tracker per reference hop (256 samples = 4 blocks): the tracker state at the hop's centre block
    centre = (np.arange(len(ref_f0)) * 256 + 1024) // BLK
    centre = np.minimum(centre, len(rows) - 1)
    tf, tv = rows["freq"][centre], rows["valid"][centre] > 0
    voiced = (ref_f0 > 0) & (ref_db > -45)
    silent = ref_db < -55
    c = cents(tf, np.where(ref_f0 > 0, ref_f0, 1.0))
    ok = voiced & tv & (np.abs(c) < 50)
    octv = voiced & tv & ((np.abs(np.abs(c) - 1200) < 60) | (np.abs(np.abs(c) - 1902) < 60))
    miss = voiced & ~tv
    wrong = voiced & tv & ~ok & ~octv
    false = silent & tv
    # onsets: the reference goes from silent/unvoiced to voiced
    lat = []
    prev = False
    for i in range(len(voiced)):
        if voiced[i] and not prev and (i < 3 or not voiced[max(0, i - 12):i].any()):
            for k in range(i, min(i + 100, len(voiced))):
                if ok[k]:
                    lat.append((k - i) * 256 / SR * 1000.0)
                    break
            else:
                lat.append(float("nan"))
        prev = voiced[i]
    pk = float(np.max(np.abs(out))) if len(out) else 0.0
    rms = float(np.sqrt(np.mean(out.astype(np.float64) ** 2))) if len(out) else 0.0
    clipped = int(np.sum(np.abs(out) > 0.99))
    nv = max(1, int(voiced.sum()))
    return {
        "name": name, "voiced": int(voiced.sum()), "ok": ok.sum() / nv, "oct": octv.sum() / nv, "miss": miss.sum() / nv, "wrong": wrong.sum() / nv,
        "false": float(false.sum()) / max(1, int(silent.sum())), "lat_ms": float(np.nanmedian(lat)) if lat else float("nan"), "lat_n": len(lat),
        "lat_lost": int(np.sum(np.isnan(lat))) if lat else 0, "peak": pk, "rms_db": 20 * np.log10(rms + 1e-12), "clip": clipped,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--presets", default="0")
    ap.add_argument("--sens", type=float, default=-1.0)
    ap.add_argument("--smooth", type=float, default=-1.0)
    ap.add_argument("--render", default="")
    ap.add_argument("--gain_db", type=float, default=0.0, help="gain applied to the capture before it goes in (the unit's input trim)")
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix="pa_eval_"))
    exe = build_exe(tmp)
    files = sorted(glob.glob(os.path.join(a.folder, "*.wav")))
    presets = [int(p) for p in a.presets.split(",")]
    print(f"{len(files)} captures, presets {presets}, sensitivity {a.sens}, smoothing {a.smooth}, input gain {a.gain_db} dB")
    print(f"{'capture':14s} {'peak dB':>8s} {'rms dB':>7s} {'dual':>5s} | preset  voiced  ok%   oct%  miss%  wrong%  false%  lat_ms(lost)   out_pk  out_rms  clip")
    totals = {}
    for f in files:
        data, sr = sf.read(f, always_2d=True, dtype="float32")
        if sr != SR:
            print(f"{os.path.basename(f)}: sample rate {sr} (need {SR}); skipped")
            continue
        mono = data[:, 0] * (10 ** (a.gain_db / 20))
        dual = bool(np.allclose(data[:, 0], data[:, 1], atol=1e-4)) if data.shape[1] > 1 else True
        peak_db = 20 * np.log10(np.max(np.abs(mono)) + 1e-12)
        rms_db = 20 * np.log10(np.sqrt(np.mean(mono.astype(np.float64) ** 2)) + 1e-12)
        ref_f0, ref_db, _ = yin_ref(mono)
        for p in presets:
            tag = Path(f).stem.replace(" ", "_") + f"_p{p}"
            rows, out = run_one(exe, mono, p, tmp, a.sens, a.smooth, tag)
            m = evaluate(Path(f).stem, mono, rows, out, ref_f0, ref_db)
            print(f"{m['name']:14s} {peak_db:8.1f} {rms_db:7.1f} {str(dual):>5s} | {p:5d}  {m['voiced']:6d}  {100*m['ok']:4.0f}  {100*m['oct']:4.0f}  {100*m['miss']:5.0f}  {100*m['wrong']:6.0f}  {100*m['false']:6.1f}  "
                  f"{m['lat_ms']:6.0f} ({m['lat_lost']}/{m['lat_n']})  {m['peak']:6.2f}  {m['rms_db']:7.1f}  {m['clip']:4d}")
            g = totals.setdefault((m['name'].split()[0].lower(), p), []); g.append(m)
            if a.render:
                os.makedirs(a.render, exist_ok=True)
                sf.write(os.path.join(a.render, f"{Path(f).stem.replace(' ', '_')}_preset{p + 1}.wav"), out, SR, subtype="PCM_16")
    print("\nby kind and preset (mean over captures):")
    for (kind, p), ms in sorted(totals.items()):
        print(f"  {kind:7s} preset {p:2d}: ok {100*np.mean([m['ok'] for m in ms]):4.0f}%  oct {100*np.mean([m['oct'] for m in ms]):3.0f}%  miss {100*np.mean([m['miss'] for m in ms]):3.0f}%  "
              f"wrong {100*np.mean([m['wrong'] for m in ms]):3.0f}%  false {100*np.mean([m['false'] for m in ms]):4.1f}%  latency {np.nanmedian([m['lat_ms'] for m in ms]):4.0f} ms")


if __name__ == "__main__":
    main()
