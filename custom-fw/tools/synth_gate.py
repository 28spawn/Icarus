#!/usr/bin/env python3
"""
custom-fw/tools/synth_gate.py - the pre-build gate for Icarus (the synth). It exists because the synth was flashed three times and froze
the unit three times with a green QA gate: the gate checked that the synth made sound, not what it COST, how it behaved under hostile input,
or whether the same code had already failed on the unit.

    python custom-fw/tools/synth_gate.py                     run every check (exit 1 if any FAIL)
    python custom-fw/tools/synth_gate.py --quick             skip the emulator soak
    python custom-fw/tools/synth_gate.py --calibrate --chip-avg 347 --chip-max 785 --build 2611061319 [--preset 1]
                                                             re-take the host -> unit cost calibration from a hardware readout
    python custom-fw/tools/synth_gate.py --json              results as JSON (qa_gate.py uses the module directly)

The checks (ids are the gate's):
  SYN-01  predicted unit cost: the host probe (test/icarus/gate_profile.c) timed per preset on three kinds of playing, scaled to the
          unit with the ratio measured on a real readout (qa/synth_cost.json), against the limits derived from HW-16 (the whole chain must
          stay under 1.16 ms: the synth may use 280 us on average, 500 us in its worst block).
  SYN-02  libm census: calls per block to sin/cos/exp/log/pow/tanh/fmod must be zero in the audio-interrupt side (the main-loop poll is counted apart) (each is a library routine on the
          unit, and the firmware is built with -fno-builtin); floorf is capped.
  SYN-03  robustness (test/icarus/test_robust.c): NaN, Inf, denormals, full scale, DC, 1e6x full scale: output finite and bounded
          while it lasts and sounding again afterwards, for every preset; denormal input no dearer than 4x a normal note; state size.
  SYN-04  stack frames: no function in the synth over 2 KB of frame (it runs inside the audio interrupt).
  SYN-05  emulator soak: the Icarus synth view, several kinds of playing, minutes of emulated time, no stall, no non-finite output.
  SYN-06  hardware record (qa/synth_stages.json, written by tools/synth_dump_check.py): the SAME synth code that already failed a bring-up
          stage on the unit does not go through again until something changed.
"""
import argparse, datetime as dt, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
ROOT = FW.parent
QA = FW / "qa"
CAL = QA / "synth_cost.json"
STAGES = QA / "synth_stages.json"
ICARUS_SRC = sorted((FW / "src" / "effects" / "icarus").glob("icarus*.c"))

# What the synth may cost on the unit (us per 64-sample block), from HW-16: the whole chain's average under 1.16 ms (80 % of the 1.451 ms
# block). The rest of the chain (amp hand-over, cabinet, EQ, conditioning, tail) averaged ~880 us in the bench readouts, so the synth's share
# is ~280 us on average; its worst block gets 500 us because the readouts' worst blocks never lined up with the other work's.
DEFAULT_LIMITS = {"avg_us": 280.0, "max_us": 500.0}
SAFETY = 1.15            # the host's stage proportions differ from the unit's by about +-25 %: be a little pessimistic
LIBM_ZERO = ("sinf", "cosf", "expf", "logf", "log2f", "powf", "tanhf", "fmodf", "atan2f", "tanf")
LIBM_CAPS = {"floorf": 100.0}


def clang():
    c = shutil.which("clang") or shutil.which("cc")
    if c:
        return c
    for p in (Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WinGet" / "Packages").glob("MartinStorsjo.LLVM-MinGW*/*/bin/clang.exe"):
        return str(p)
    return None


def run(cmd, timeout=600, cwd=ROOT):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired as e:
        return 124, f"timed out after {timeout} s\n{(e.stdout or '')}"


def tail(text, n=8):
    return "\n".join(text.strip().splitlines()[-n:])


def synth_source_hash():
    """What a hardware result is about: the synth, the code that runs it in the audio interrupt and the screens/main loop it shares. CRLF-normalised."""
    h = hashlib.sha256()
    files = sorted((FW / "src" / "effects" / "icarus").glob("*")) + sorted((FW / "src" / "ui").glob("*")) + [
        FW / "src" / "board" / "audio_alnk.c", FW / "src" / "fx" / "fx_graph.c", FW / "src" / "sys" / "pa_audio_core.c", FW / "src" / "board" / "ui_app.c"]
    for f in files:
        if f.is_file():
            h.update(f.name.encode())
            h.update(f.read_bytes().replace(b"\r\n", b"\n"))
    return h.hexdigest()


def build(cc, out, srcs, extra=()):
    cmd = [cc, "-O2", "-Wall", "-Wextra", "-DPA_HOST_SIM=1", "-Icustom-fw/include", "-Icustom-fw/src/effects/icarus", *extra, *[str(s) for s in srcs], "-lm", "-o", str(out)]
    return run(cmd)


# ---------------------------------------------------------------------------------------------------------------- checks
def profile(cc, tmp):
    exe = tmp / "gate_profile.exe"
    rc, out = build(cc, exe, ["custom-fw/test/icarus/gate_profile.c", *["custom-fw/src/effects/icarus/" + p.name for p in ICARUS_SRC]])
    if rc != 0:
        return None, "gate_profile.c does not compile:\n" + tail(out)
    rc, out = run([str(exe)], timeout=900)
    if rc != 0:
        return None, "gate_profile failed:\n" + tail(out)
    try:
        return json.loads(out), ""
    except json.JSONDecodeError as e:
        return None, f"gate_profile output is not JSON ({e}):\n{tail(out)}"


def load_cal():
    try:
        return json.loads(CAL.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def check_cost(cc, tmp):
    """SYN-01"""
    prof, err = profile(cc, tmp)
    if prof is None:
        return [("SYN-01", "FAIL", "predicted unit cost: host probe", err)]
    cal = load_cal()
    if not cal:
        return [("SYN-01", "FAIL", "predicted unit cost: no calibration (qa/synth_cost.json)",
                 "calibrate it from a hardware readout:\n  python custom-fw/tools/synth_gate.py --calibrate --chip-avg <SYNTH avg us> --chip-max <SYNTH max us> --build <BUILD>")]
    ref = cal["reference"]
    lim = cal.get("limits", DEFAULT_LIMITS)
    speed = prof["bench_us"] / ref["bench_us"]                     # >1: this host is slower than it was at calibration
    k_avg = ref["chip_avg_us"] / (ref["host_avg_us"])              # unit us per host us (on the calibration host)
    peak_ratio = ref["chip_max_us"] / ref["chip_avg_us"]           # the unit's worst block over its average (785 / 347 on build 2611061319). The host's own p99 is
                                                                   # dominated by scheduler noise (x145 once scaled), so the worst block is predicted from the average.
    rows, worst_avg, worst_max, bad = [], 0.0, 0.0, []
    for p in prof["presets"]:
        for kind in ("play", "chord"):
            k = p["kinds"][kind]
            pa = k["avg_us"] / speed * k_avg * SAFETY
            pm = pa * peak_ratio
            worst_avg, worst_max = max(worst_avg, pa), max(worst_max, pm)
            if pa > lim["avg_us"] or pm > lim["max_us"]:
                bad.append(f"[{p['preset']:2d}] {p['name']:15s} {kind:5s}: avg {pa:5.0f} us (limit {lim['avg_us']:.0f}), worst block {pm:5.0f} us (limit {lim['max_us']:.0f})")
    waived = set(cal.get("waived_presets", []))
    bad_nw = [b for b in bad if int(b[1:3]) not in waived]
    title = (f"predicted unit cost: worst preset {worst_avg:.0f} us avg / {worst_max:.0f} us worst block "
             f"(limits {lim['avg_us']:.0f} / {lim['max_us']:.0f}; calibrated on build {ref.get('build', '?')}, host speed x{speed:.2f})")
    if bad_nw:
        return [("SYN-01", "FAIL", title, "over budget:\n" + "\n".join(bad_nw[:12]) + ("\n..." if len(bad_nw) > 12 else ""))]
    return [("SYN-01", "PASS", title, "")]


def check_census(cc, tmp):
    """SYN-02"""
    exe = tmp / "gate_census.exe"
    incl = str((FW / "test" / "icarus" / "gate_countlib.h").as_posix())
    rc, out = build(cc, exe, ["custom-fw/test/icarus/gate_census.c", *["custom-fw/src/effects/icarus/" + p.name for p in ICARUS_SRC]], extra=["-include", incl])
    if rc != 0:
        return [("SYN-02", "FAIL", "libm census does not compile", tail(out))]
    rc, out = run([str(exe)])
    if rc != 0:
        return [("SYN-02", "FAIL", "libm census failed", tail(out))]
    data = json.loads(out)
    bad = []
    for p in data["presets"]:
        c = p["calls_per_block"]
        for fn in LIBM_ZERO:
            if c.get(fn, 0) > 0.0:
                bad.append(f"[{p['preset']:2d}] {p['name']}: {fn} x{c[fn]:.1f} per block")
        for fn, cap in LIBM_CAPS.items():
            if c.get(fn, 0) > cap:
                bad.append(f"[{p['preset']:2d}] {p['name']}: {fn} x{c[fn]:.0f} per block (cap {cap:.0f})")
    if bad:
        return [("SYN-02", "FAIL", "libm census: library calls in the audio path (each costs 0.4-1.2 us on the unit)", "\n".join(bad[:12]))]
    return [("SYN-02", "PASS", "libm census: no library maths in the audio path (fabsf / sqrtf are inlined, floorf within its cap)", "")]


def check_robust(cc, tmp):
    """SYN-03"""
    exe = tmp / "test_robust.exe"
    rc, out = build(cc, exe, ["custom-fw/test/icarus/test_robust.c", *["custom-fw/src/effects/icarus/" + p.name for p in ICARUS_SRC]])
    if rc != 0:
        return [("SYN-03", "FAIL", "test_robust.c does not compile", tail(out))]
    rc, out = run([str(exe)], timeout=900)
    nfail = sum(1 for ln in out.splitlines() if ln.startswith("  FAIL"))
    if rc != 0 or nfail:
        return [("SYN-03", "FAIL", f"robustness: {nfail} checks failed (NaN / Inf / denormal / extreme input)", "\n".join([ln for ln in out.splitlines() if ln.startswith("  FAIL")][:10]))]
    npass = sum(1 for ln in out.splitlines() if ln.startswith("  ok"))
    return [("SYN-03", "PASS", f"robustness: {npass} checks (every preset x NaN / Inf / denormal / full scale / DC / 1e6x): bounded, finite, recovers, no denormal slow-down", "")]


def check_frames(cc, tmp):
    """SYN-04"""
    bad = []
    for s in ICARUS_SRC:
        rc, out = run([cc, "-O2", "-DPA_HOST_SIM=1", "-Icustom-fw/include", "-Icustom-fw/src/effects/icarus", "-Wframe-larger-than=2048", "-c", str(s), "-o", str(tmp / (s.stem + ".o"))])
        if rc != 0 and "error" in out:
            return [("SYN-04", "FAIL", f"stack frames: {s.name} does not compile", tail(out))]
        bad += [ln.strip() for ln in out.splitlines() if "frame-larger-than" in ln or "stack frame size" in ln]
    if bad:
        return [("SYN-04", "FAIL", "stack frames: a synth function has a frame over 2 KB (it runs in the audio interrupt)", "\n".join(bad[:8]))]
    return [("SYN-04", "PASS", "stack frames: every synth function under 2 KB", "")]


def check_soak(quick):
    """SYN-05"""
    dll = FW / "emu" / "icarus_emu.dll"
    if quick or not dll.exists():
        return [("SYN-05", "SKIP", "emulator soak skipped" + (" (--quick)" if quick else " (no emulator DLL: run build.ps1)"), "")]
    results = []
    for preset in (0, 3, 6):
        rc, out = run([sys.executable, str(FW / "emu" / "soak_icarus.py"), "90", str(preset)], timeout=600)
        last = out.strip().splitlines()[-1] if out.strip() else "(no output)"
        if rc != 0 or "SOAK OK" not in last:
            return [("SYN-05", "FAIL", f"emulator soak (preset {preset + 1}): {last}", tail(out))]
        results.append(last)
    return [("SYN-05", "PASS", "emulator soak: the Icarus view for 3 x 90 s of mixed playing, no stall", "")]


def check_hw_record():
    """SYN-06"""
    try:
        recs = json.loads(STAGES.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        recs = []
    h = synth_source_hash()
    same = [r for r in recs if r.get("source_hash") == h]
    if not same:
        last = recs[-1] if recs else None
        why = f"; the last recorded dump (build {last['build']}, stage {last['stage']}) was {last['verdict']}" if last else ""
        return [("SYN-06", "INFO", f"no hardware dump recorded for this synth code{why} (tools/synth_dump_check.py)", "")]
    failed = [r for r in same if r["verdict"] == "FAIL"]
    if failed:
        r = failed[-1]
        return [("SYN-06", "FAIL", f"this exact synth code already FAILED on the unit (build {r['build']}, stage {r['stage']}): change something before flashing it again",
                 "\n".join(r.get("reasons", [])[:8]))]
    return [("SYN-06", "PASS", f"hardware: this synth code passed {len(same)} recorded stage(s) on the unit (last: {same[-1]['stage']}, build {same[-1]['build']})", "")]


def run_all(quick=False):
    cc = clang()
    if not cc:
        return [("SYN-01", "FAIL", "no host C compiler (clang)", "")]
    tmp = Path(tempfile.mkdtemp(prefix="pa_synth_gate_"))
    out = []
    try:
        out += check_cost(cc, tmp)
        out += check_census(cc, tmp)
        out += check_robust(cc, tmp)
        out += check_frames(cc, tmp)
        out += check_soak(quick)
        out += check_hw_record()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return out


def calibrate(args):
    cc = clang()
    tmp = Path(tempfile.mkdtemp(prefix="pa_synth_cal_"))
    try:
        prof, err = profile(cc, tmp)
        if prof is None:
            print(err)
            return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    p = next(x for x in prof["presets"] if x["preset"] == args.preset)
    k = p["kinds"]["play"]
    old = load_cal() or {}
    cal = {
        "about": "Host -> unit cost calibration for the synth gate (tools/synth_gate.py). chip_* come from a hardware readout (the SYNTH row of "
                 "'measured time per effect slot', preset `preset`, the synth playing); host_* are the probe's numbers for the same preset on the "
                 "machine that took the calibration; bench_us is that machine's speed kernel. Re-take it whenever a new readout is available.",
        "reference": {"preset": args.preset, "name": p["name"], "kind": "play", "build": args.build, "chip_avg_us": args.chip_avg, "chip_max_us": args.chip_max,
                      "host_avg_us": k["avg_us"], "host_p99_us": k["p99_us"], "bench_us": prof["bench_us"], "date": dt.date.today().isoformat()},
        "limits": old.get("limits", DEFAULT_LIMITS),
        "waived_presets": old.get("waived_presets", []),
    }
    QA.mkdir(exist_ok=True)
    CAL.write_text(json.dumps(cal, indent=2) + "\n", encoding="utf-8")
    print(f"calibrated: preset {args.preset} ({p['name']}) host {k['avg_us']:.2f} us avg / {k['p99_us']:.2f} us p99  ->  unit {args.chip_avg} / {args.chip_max} us  (x{args.chip_avg / k['avg_us']:.0f})")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Icarus Synth pre-build gate")
    ap.add_argument("--quick", action="store_true", help="skip the emulator soak")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--calibrate", action="store_true")
    ap.add_argument("--chip-avg", type=float)
    ap.add_argument("--chip-max", type=float)
    ap.add_argument("--build", default="?")
    ap.add_argument("--preset", type=int, default=1)
    args = ap.parse_args()
    if args.calibrate:
        if args.chip_avg is None or args.chip_max is None:
            ap.error("--calibrate needs --chip-avg and --chip-max (the SYNTH row's avg and max in us)")
        return calibrate(args)
    res = run_all(args.quick)
    if args.json:
        print(json.dumps(res, indent=2))
    else:
        for cid, st, title, detail in res:
            print(f"  [{ {'PASS': 'ok  ', 'FAIL': 'FAIL', 'SKIP': 'skip', 'INFO': 'info'}[st] }] {cid:7} {title}")
            if detail and st == "FAIL":
                for ln in detail.splitlines()[:12]:
                    print(f"              {ln}")
    return 1 if any(r[1] == "FAIL" for r in res) else 0


if __name__ == "__main__":
    sys.exit(main())
