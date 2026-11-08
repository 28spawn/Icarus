#!/usr/bin/env python3
"""
custom-fw/tools/synth_dump_check.py - judge a diagnostic readout of the unit (the "what the unit was doing (TSTA)" text that jl_ram_read.py prints)
against the Icarus bring-up limits (HW-21), and record the result so the QA gate (tools/synth_gate.py, SYN-06) knows what the unit said about
THIS synth code.

    python custom-fw/tools/synth_dump_check.py dump.txt --stage ANALOG            judge and record
    python custom-fw/tools/synth_dump_check.py dump.txt --stage OSC --no-record    judge only
    type dump.txt | python custom-fw/tools/synth_dump_check.py - --stage OSC
    python custom-fw/tools/synth_dump_check.py dump.txt --stage OSC --calibrate    also re-take the gate's host -> unit cost calibration

Stages (TEST-PROTOCOL.md 5.1): OFF, ANALOG, OSC, HYBRID, GUITAR, PLAY (the 2-minute playing stages), SOAK.
"""
import argparse, datetime as dt, json, re, subprocess, sys
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import synth_gate  # noqa: E402

LIMITS = {"isr_max_us": 1160, "synth_avg_us": 280, "synth_max_us": 500}


def parse(text):
    m = {}
    g = lambda pat, flags=0: re.search(pat, text, flags)
    if (r := g(r"^BUILD\s+(\d+)", re.M)):
        m["build"] = r.group(1)
    if (r := g(r"up (\d+) s")):
        m["up_s"] = int(r.group(1))
    if (r := g(r"input peak (\d+)%")):
        m["input_peak_pct"] = int(r.group(1))
    if (r := g(r"entries (\d+), last (\d+) us, max (\d+) us \(budget (\d+) us\), overruns (\d+), auto-bypass (\d+)")):
        m.update(isr_entries=int(r.group(1)), isr_last_us=int(r.group(2)), isr_max_us=int(r.group(3)), isr_budget_us=int(r.group(4)),
                 overruns=int(r.group(5)), auto_bypass=int(r.group(6)))
    if (r := g(r"load governor: level (\d)[^\n]*average audio time (\d+) us")):
        m.update(gov_level=int(r.group(1)), avg_audio_us=int(r.group(2)))
    rows = {}
    for r in re.finditer(r"^\s*(DYN|DRV|SYNTH|AMP|CAB|EQ|MOD|DLY|REV|syn:\w+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$", text, re.M):
        rows[r.group(1)] = {"calls": int(r.group(2)), "avg_us": int(r.group(3)), "max_us": int(r.group(4)), "last_us": int(r.group(5))}
    m["slots"] = rows
    synth = rows.get("SYNTH") or rows.get("DRV")
    if synth:
        m["synth"] = synth
    # the synth's three stage rows: named by the new readout, or the old MOD / DLY / REV labels of the slots they borrow
    stages = {}
    for new, old in (("syn:front", "MOD"), ("syn:engines", "DLY"), ("syn:back", "REV")):
        if new in rows:
            stages[new] = rows[new]
        elif old in rows and rows[old]["calls"] and synth and rows[old]["calls"] == synth["calls"]:
            stages[new] = rows[old]
    if stages:
        m["stages"] = stages
    # A FREEZE is proven by the detector's own evidence: a FREEZE event in the log, or a PC captured at the freeze. The download-mode mark and the reset record alone are
    # NOT evidence: a deliberate button-combo hold, the auto-return timer and the freeze detector all set them (they were read as freezes for four dumps).
    m["download_mode"] = bool(g(r"rescue mark SET") or g(r"download-mode mark SET") or g(r"RESET RECORD"))
    m["rescue"] = bool(g(r"FREEZE: the (MAIN LOOP|AUDIO INTERRUPT) stopped") or g(r"AT THE FREEZE"))
    if (r := g(r"main loop last at (0x[0-9a-fA-F]+)")):
        m["main_loop_checkpoint"] = r.group(1)
    m["shed_event"] = bool(g(r"\bBYPASS\b") or g(r"SYNTH SHED"))
    if (r := g(r"->\s+(0x[0-9a-fA-F]+)\s*(<[^>\n]*>)?\s+<- stopped here")):
        m["freeze_pc"] = r.group(1)
        m["freeze_symbol"] = (r.group(2) or "").strip("<>")
    if (r := g(r"VERDICT: the last 4 samples are the SAME address \((0x[0-9a-fA-F]+)\)")):
        m["stuck_pc"] = r.group(1)
    return m


def judge(m):
    reasons = []
    if "isr_max_us" not in m:
        reasons.append("no audio-interrupt statistics in the readout")
    else:
        if m["isr_max_us"] > LIMITS["isr_max_us"]:
            reasons.append(f"audio interrupt longest {m['isr_max_us']} us > {LIMITS['isr_max_us']} us (80 % of the {m.get('isr_budget_us', 1451)} us block, HW-16)")
        if m["overruns"]:
            reasons.append(f"{m['overruns']} audio overruns (must be 0)")
        if m["auto_bypass"]:
            reasons.append("the audio interrupt auto-bypassed")
    if m.get("gov_level", 0) != 0:
        reasons.append(f"the load governor is at level {m['gov_level']} (it cut quality: the unit was short of time)")
    s = m.get("synth")
    if not s:
        reasons.append("no SYNTH row in the slot table (was the synth on?)")
    else:
        if s["avg_us"] > LIMITS["synth_avg_us"]:
            reasons.append(f"synth averages {s['avg_us']} us > {LIMITS['synth_avg_us']} us per block")
        if s["max_us"] > LIMITS["synth_max_us"]:
            reasons.append(f"synth worst block {s['max_us']} us > {LIMITS['synth_max_us']} us")
    if m.get("rescue"):
        cp = m.get("main_loop_checkpoint", "?")
        where = (f"; the CPU was stopped at {m['freeze_pc']} {m.get('freeze_symbol', '')}".rstrip() if m.get("freeze_pc")
                 else "; no PC captured (a readout of a build without the PC block)")
        reasons.append(f"the unit FROZE (the detector logged it); the main loop's last checkpoint was {cp}{where}")
    if m.get("shed_event"):
        reasons.append("the overload protection shed the synth (a BYPASS event is in the log)")
    return ("FAIL" if reasons else "PASS"), reasons


def main():
    ap = argparse.ArgumentParser(description="judge a unit diagnostic readout against the Icarus limits")
    ap.add_argument("dump", help="the readout text file, or - for stdin")
    ap.add_argument("--stage", required=True, help="OFF, ANALOG, OSC, HYBRID, GUITAR, PLAY or SOAK")
    ap.add_argument("--no-record", action="store_true")
    ap.add_argument("--calibrate", action="store_true", help="also re-take the gate's host -> unit cost calibration from this readout (the synth must have been playing)")
    args = ap.parse_args()
    if args.dump == "-":
        text = sys.stdin.read()
    else:
        raw = Path(args.dump).read_bytes()
        # PowerShell's `>` writes UTF-16 with a byte-order mark; python's own redirection and the tool's --out are UTF-8
        text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", errors="replace")
    m = parse(text)
    verdict, reasons = judge(m)
    print(f"build {m.get('build', '?')}, up {m.get('up_s', '?')} s, stage {args.stage}")
    if "isr_max_us" in m:
        print(f"  audio interrupt: longest {m['isr_max_us']} us (limit {LIMITS['isr_max_us']}), overruns {m['overruns']}, governor level {m.get('gov_level', '?')}, average {m.get('avg_audio_us', '?')} us")
    if m.get("synth"):
        s = m["synth"]
        print(f"  synth: avg {s['avg_us']} us (limit {LIMITS['synth_avg_us']}), worst {s['max_us']} us (limit {LIMITS['synth_max_us']})")
    for name, row in m.get("stages", {}).items():
        print(f"    {name:12s} avg {row['avg_us']:4d} us   worst {row['max_us']:4d} us")
    print(f"VERDICT: {verdict}")
    for r in reasons:
        print(f"  - {r}")
    if not args.no_record:
        rec = {"time": dt.datetime.now().isoformat(timespec="seconds"), "stage": args.stage, "build": m.get("build", "?"),
               "source_hash": synth_gate.synth_source_hash(), "verdict": verdict, "reasons": reasons,
               "metrics": {k: v for k, v in m.items() if k in ("isr_max_us", "overruns", "gov_level", "avg_audio_us", "synth", "stages", "up_s", "input_peak_pct", "main_loop_checkpoint", "freeze_pc", "freeze_symbol", "stuck_pc")}}
        try:
            recs = json.loads(synth_gate.STAGES.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            recs = []
        recs.append(rec)
        synth_gate.QA.mkdir(exist_ok=True)
        synth_gate.STAGES.write_text(json.dumps(recs, indent=2) + "\n", encoding="utf-8")
        print(f"recorded in {synth_gate.STAGES.relative_to(FW.parent)} (the source hash is the CURRENT working tree: record a dump right after flashing the build it came from)")
    if args.calibrate:
        s = m.get("synth")
        if not s:
            print("cannot calibrate: no SYNTH row")
            return 1
        return subprocess.call([sys.executable, str(Path(__file__).with_name("synth_gate.py")), "--calibrate", "--chip-avg", str(s["avg_us"]),
                                "--chip-max", str(s["max_us"]), "--build", m.get("build", "?")])
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
