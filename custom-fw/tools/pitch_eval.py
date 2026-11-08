#!/usr/bin/env python3
"""
custom-fw/tools/pitch_eval.py - judge Icarus's pitch tracker on signals whose pitch is KNOWN, plus the real dry captures.

    python custom-fw/tools/pitch_eval.py [--captures "C:/path/Dry Captures"] [--seed 1] [--notes 60] [--quiet]

SYNTHETIC SET (ground truth): Karplus-Strong plucked strings (the notes decay like a real string: the higher partials die first and the fundamental rings out), single notes and legato pairs
(a hammer-on / pull-off: a second pitch without a new pick), E2..E5, -34..-8 dB picks, a -62 dB noise floor and 50 Hz hum, separated by silences. For each note the tracker's output (frequency,
valid, onset: the host build of the firmware's own code, tools/icarus_capture_eval.c, with the main-loop poll once per UI tick) is compared with the truth:
  latency   from the pick (or the legato note change) to the first block where the tracker is valid AND within 50 cents (lost = never in the note)
  settle    ... and stays within 50 cents for 30 ms
  err       median / 90th-percentile absolute pitch error in cents over the first 400 ms of the note, once it has settled
  octave    share of valid frames in the note that are an octave (or a fifth + octave) away: the classic tracker failure
  ghost     time the tracker stays VALID after the string has decayed 30 dB below its own peak (a note the player is no longer playing: the voice would keep sounding it) and
            the time it is valid in the silence between notes
  retrig    onsets reported inside a note that was not a new pick or a legato change (a decaying string re-triggering the voice)
REAL CAPTURES: the same checks against an offline reference (2048-sample YIN) and per-note decay (the level falls 30 dB below the note's own peak).
"""
import argparse, glob, os, subprocess, sys, tempfile
from pathlib import Path
import numpy as np

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
        raise SystemExit("compile failed:\n" + r.stderr[-3000:])
    return exe


def run_exe(exe: Path, mono: np.ndarray, preset: int, tmp: Path, tag: str):
    raw = tmp / (tag + ".f32")
    mono.astype(np.float32).tofile(raw)
    pre = tmp / tag
    r = subprocess.run([str(exe), str(raw), str(preset), str(pre)], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr)
    return np.genfromtxt(str(pre) + ".csv", delimiter=",", names=True)


# ------------------------------------------------------------------ synthetic strings
def pluck(f0, dur, amp_db, rng, bright=0.5, decay=0.9985):
    """Karplus-Strong string: noise burst through a one-pole smoother, then the classic averaging loop (blockwise, so it is fast)."""
    n = max(4, int(round(SR / f0)))
    buf = rng.uniform(-1, 1, n)
    prev = 0.0
    for i in range(n):                                  # soften the pick
        prev = bright * buf[i] + (1 - bright) * prev
        buf[i] = prev
    buf -= buf.mean()
    out = [buf.copy()]
    total = n
    cur = buf
    while total < int(dur * SR):
        nxt = decay * 0.5 * (cur + np.roll(cur, -1))
        out.append(nxt)
        cur = nxt
        total += n
    x = np.concatenate(out)[: int(dur * SR)]
    x = x / (np.max(np.abs(x)) + 1e-9) * 10 ** (amp_db / 20)
    return x


def make_synth(seed=1, notes=60):
    rng = np.random.default_rng(seed)
    segs, truth = [], []          # truth: dict(t0, t1, f, kind, peak_t)
    t = 0
    x = np.zeros(int(0.3 * SR))
    t = len(x)
    midi_pool = list(range(40, 77))     # E2 .. E5
    k = 0
    while k < notes:
        m = int(rng.choice(midi_pool))
        f = 440.0 * 2 ** ((m - 69) / 12)
        dur = float(rng.uniform(0.35, 1.3))
        amp = float(rng.uniform(-30, -8))
        s = pluck(f, dur, amp, rng, bright=float(rng.uniform(0.3, 0.8)), decay=float(rng.uniform(0.9975, 0.9992)))
        t0 = len(x)
        x = np.concatenate([x, s])
        truth.append(dict(t0=t0, t1=len(x), f=f, kind="pick", amp=amp))
        k += 1
        if rng.random() < 0.3 and k < notes:                  # legato: hammer-on / pull-off, no new pick: the string simply changes pitch, here by a crossfade into the second pitch
            dm = int(rng.choice([-5, -3, -2, 2, 3, 4, 5]))
            m2 = min(max(m + dm, 40), 80)
            f2 = 440.0 * 2 ** ((m2 - 69) / 12)
            d2 = float(rng.uniform(0.3, 0.8))
            s2 = pluck(f2, d2, amp - 6, rng, bright=0.3, decay=0.999)
            s2 *= np.minimum(1.0, np.arange(len(s2)) / (0.004 * SR))     # the new pitch comes in over 4 ms
            t0 = len(x) - int(0.01 * SR)
            tail = x[t0:]
            x = x[:t0]
            x = np.concatenate([x, tail * np.linspace(1, 0, len(tail)) * 0.0, np.zeros(0)])
            x = np.concatenate([x, s2])
            truth.append(dict(t0=t0, t1=len(x), f=f2, kind="legato", amp=amp - 6))
            k += 1
        gap = int(rng.uniform(0.15, 0.6) * SR)
        x = np.concatenate([x, np.zeros(gap)])
    x = np.concatenate([x, np.zeros(int(0.4 * SR))])
    tt = np.arange(len(x)) / SR
    x = x + 10 ** (-62 / 20) * rng.standard_normal(len(x)) + 10 ** (-52 / 20) * np.sin(2 * np.pi * 50 * tt)
    return x.astype(np.float32), truth


def cents(a, b):
    return 1200.0 * np.log2(np.maximum(a, 1e-3) / np.maximum(b, 1e-3))


def eval_synth(rows, truth, x, verbose=True):
    t_blk = rows["t"]
    freq, valid, onset = rows["freq"], rows["valid"] > 0, rows["onset"] > 0
    lat, lost, settle_l, errs, octs, ghost_note, retrig = [], 0, [], [], 0, 0.0, 0
    lat_k = {'pick': [], 'legato': []}
    n_val = 0
    owned = np.zeros(len(t_blk), bool)
    for k, nt in enumerate(truth):
        b0 = nt["t0"] // BLK
        b1 = min(len(t_blk), nt["t1"] // BLK)
        # the note's own envelope (peak, and where the string has decayed 30 dB below it)
        seg = x[nt["t0"]: nt["t1"]]
        w = 441
        env = np.sqrt(np.convolve(seg ** 2, np.ones(w) / w, mode="same"))
        ipk = int(np.argmax(env))
        below = np.where(env[ipk:] < env[ipk] * 10 ** (-30 / 20))[0]
        end30 = (nt["t0"] + ipk + int(below[0])) // BLK if len(below) else b1
        owned[b0:b1] = True
        f_ok = np.abs(cents(freq[b0:b1], nt["f"])) < 50
        ok = valid[b0:b1] & f_ok
        first = np.where(ok)[0]
        if len(first):
            lat.append(first[0] * BLK / SR * 1000.0); lat_k[nt['kind']].append(first[0] * BLK / SR * 1000.0)
            # settled: first block from which 30 ms (10 blocks) stay ok
            run = np.convolve(ok.astype(int), np.ones(10, int), mode="valid")
            idx = np.where(run == 10)[0]
            settle_l.append(idx[0] * BLK / SR * 1000.0 if len(idx) else float("nan"))
            win = slice(b0 + first[0], min(b1, b0 + first[0] + int(0.4 * SR / BLK)))
            e = np.abs(cents(freq[win], nt["f"]))[valid[win]]
            errs.extend(e.tolist())
        else:
            lost += 1
        vv = valid[b0:b1]
        oct_mask = vv & ((np.abs(np.abs(cents(freq[b0:b1], nt["f"])) - 1200) < 60) | (np.abs(np.abs(cents(freq[b0:b1], nt["f"])) - 1902) < 60))
        octs += int(oct_mask.sum()); n_val += int(vv.sum())
        # ghost: valid after the string is 30 dB down
        if end30 < b1:
            ghost_note += float(valid[end30:b1].sum()) * BLK / SR * 1000.0
        # re-trigger: onsets inside the note that are not its start
        n_on = int(onset[b0:end30].sum()) if end30 > b0 else 0
        retrig += max(0, n_on - 1)                            # the note's own onset (the first one) is expected; more inside it are re-triggers
    ghost_gap = float(valid[~owned].sum()) * BLK / SR * 1000.0
    out = {
        "notes": len(truth), "lost": lost, "lat_med": float(np.nanmedian(lat)) if lat else float("nan"), "lat_p90": float(np.nanpercentile(lat, 90)) if lat else float("nan"),
        "settle_med": float(np.nanmedian(settle_l)) if settle_l else float("nan"), "err_med": float(np.median(errs)) if errs else float("nan"),
        "err_p90": float(np.percentile(errs, 90)) if errs else float("nan"), "oct_pct": 100.0 * octs / max(1, n_val),
        "ghost_note_ms": ghost_note / max(1, len(truth)), "ghost_gap_ms": ghost_gap / max(1, len(truth)), "retrig": retrig,
        "lat_pick": float(np.nanmedian(lat_k['pick'])) if lat_k['pick'] else float('nan'), "lat_leg": float(np.nanmedian(lat_k['legato'])) if lat_k['legato'] else float('nan'),
    }
    return out


def show_synth(o, tag=""):
    print(f"  synthetic{tag}: {o['notes']} notes, lost {o['lost']}; latency median {o['lat_med']:.0f} ms (p90 {o['lat_p90']:.0f}; picks {o.get('lat_pick', float('nan')):.0f}, legato {o.get('lat_leg', float('nan')):.0f}), settled {o['settle_med']:.0f} ms; "
          f"pitch error median {o['err_med']:.1f} c (p90 {o['err_p90']:.0f} c); octave {o['oct_pct']:.1f}%; "
          f"ghost: {o['ghost_note_ms']:.0f} ms/note after the string is 30 dB down, {o['ghost_gap_ms']:.0f} ms/note in the gaps; re-triggers {o['retrig']}")


# ------------------------------------------------------------------ real captures
def yin_ref(x, fmin=48.0, fmax=1400.0, win=2048, hop=256, thr=0.12):
    n = (len(x) - win) // hop
    f0 = np.zeros(n); db = np.full(n, -120.0)
    lo, hi = int(SR / fmax), int(SR / fmin)
    for i in range(n):
        fr = x[i * hop:i * hop + win].astype(np.float64)
        db[i] = 20 * np.log10(np.sqrt(np.mean(fr * fr)) + 1e-12)
        if db[i] < -55:
            continue
        w = win // 2
        fa = np.fft.rfft(fr, 2 * win)
        r = np.fft.irfft(fa * np.conj(np.fft.rfft(fr[:w], 2 * win)))[:hi + 2]
        cs = np.concatenate([[0], np.cumsum(fr * fr)])
        e0 = cs[w] - cs[0]
        dd = np.zeros(hi + 2)
        t = np.arange(1, hi + 2)
        dd[1:] = e0 + (cs[t + w] - cs[t]) - 2 * r[1:hi + 2]
        run = np.cumsum(dd[1:])
        cm = np.ones(hi + 2)
        cm[1:] = dd[1:] * t / np.maximum(run, 1e-12)
        t = lo; pick = 0
        while t < hi:
            if cm[t] < thr:
                while t + 1 < hi and cm[t + 1] < cm[t]:
                    t += 1
                pick = t; break
            t += 1
        if pick:
            y0, y1, y2 = cm[pick - 1], cm[pick], cm[pick + 1]
            den = y0 - 2 * y1 + y2
            off = 0.5 * (y0 - y2) / den if abs(den) > 1e-9 else 0.0
            f0[i] = SR / (pick + max(-0.5, min(0.5, off)))
    return f0, db


def eval_capture(rows, mono, ref_f0, ref_db):
    centre = np.minimum((np.arange(len(ref_f0)) * 256 + 1024) // BLK, len(rows) - 1)
    tf, tv = rows["freq"][centre], rows["valid"][centre] > 0
    voiced = (ref_f0 > 0) & (ref_db > -45)
    c = cents(tf, np.where(ref_f0 > 0, ref_f0, 1.0))
    ok = voiced & tv & (np.abs(c) < 50)
    octv = voiced & tv & ((np.abs(np.abs(c) - 1200) < 60) | (np.abs(np.abs(c) - 1902) < 60))
    nv = max(1, int(voiced.sum()))
    # onsets of the reference and the latency to a correct, valid pitch
    lat = []
    prev = False
    for i in range(len(voiced)):
        if voiced[i] and not prev and (i < 3 or not voiced[max(0, i - 12):i].any()):
            for k in range(i, min(i + 100, len(voiced))):
                if ok[k]:
                    lat.append((k - i) * 256 / SR * 1000.0); break
            else:
                lat.append(float("nan"))
        prev = voiced[i]
    # ghost: valid while the level is 30 dB below the running peak of the current phrase (peak decays with a 1.5 s time constant)
    pk = 0.0; ghost = 0; n_ghost_frames = 0
    for i in range(len(ref_db)):
        pk = max(ref_db[i], pk - 0.0065 * 256 / 44.1 * 6) if ref_db[i] > -55 else pk - 0.2
        if tv[i] and ref_db[i] < pk - 30 and pk > -45:
            ghost += 1
    silent = ref_db < -55
    return {"voiced": int(voiced.sum()), "ok": ok.sum() / nv, "oct": octv.sum() / nv, "false": float((silent & tv).sum()) / max(1, int(silent.sum())),
            "lat": float(np.nanmedian(lat)) if lat else float("nan"), "ghost_ms": ghost * 256 / SR * 1000.0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--captures", default="")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--notes", type=int, default=60)
    ap.add_argument("--preset", type=int, default=0)
    ap.add_argument("--seeds", type=int, default=3, help="number of synthetic sets (seed, seed+1, ...)")
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix="pa_icarus_pitch_"))
    exe = build_exe(tmp)
    allr = []
    for s in range(a.seeds):
        x, truth = make_synth(a.seed + s, a.notes)
        rows = run_exe(exe, x, a.preset, tmp, f"syn{s}")
        o = eval_synth(rows, truth, x)
        show_synth(o, f" (seed {a.seed + s})")
        allr.append(o)
    keys = ["lost", "lat_pick", "lat_leg", "lat_med", "lat_p90", "settle_med", "err_med", "err_p90", "oct_pct", "ghost_note_ms", "ghost_gap_ms", "retrig"]
    avg = {k: float(np.nanmean([o[k] for o in allr])) for k in keys}
    avg["notes"] = int(np.mean([o["notes"] for o in allr]))
    print("  synthetic MEAN of seeds:")
    show_synth(avg, " mean")
    if a.captures:
        import soundfile as sf
        print("\n  real captures (reference: 2048-sample YIN):")
        tot = []
        for f in sorted(glob.glob(os.path.join(a.captures, "*.wav"))):
            d, sr = sf.read(f, always_2d=True, dtype="float32")
            if sr != SR or "chords" in os.path.basename(f).lower():
                continue
            mono = d[:, 0]
            f0, db = yin_ref(mono)
            rows = run_exe(exe, mono, a.preset, tmp, "cap")
            r = eval_capture(rows, mono, f0, db)
            tot.append(r)
            print(f"    {os.path.basename(f):12s} voiced {r['voiced']:5d}  ok {100*r['ok']:4.0f}%  oct {100*r['oct']:3.0f}%  false {100*r['false']:5.1f}%  latency {r['lat']:4.0f} ms  ghost {r['ghost_ms']:5.0f} ms")
        if tot:
            print(f"    MEAN (single-note captures): ok {100*np.mean([r['ok'] for r in tot]):.0f}%  oct {100*np.mean([r['oct'] for r in tot]):.0f}%  false {100*np.mean([r['false'] for r in tot]):.1f}%  "
                  f"latency {np.nanmedian([r['lat'] for r in tot]):.0f} ms  ghost {np.mean([r['ghost_ms'] for r in tot]):.0f} ms/capture")


if __name__ == "__main__":
    main()
