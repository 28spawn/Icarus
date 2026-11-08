#!/usr/bin/env python3
"""
custom-fw/tools/pi32_loopcost.py - what the synth's hot loops cost on the UNIT, read from the code the target compiler really produces.

The host probe (test/icarus/gate_profile.c) times the synth on an x86 PC, which has a big register file and a fast cache: it cannot see what
makes the unit slow. On the unit's 16-register core the float state of a sample loop does not fit, so the compiler spills it to the stack and every
iteration pays loads and stores (a stack read-modify-write measured 22 ns, against 8 ns for a float add): the readout showed the synth running ~77x
slower than the PC, far more than the clock ratio. This tool compiles the synth sources for pi32v2 with the firmware's own flags, finds the loops in
the assembly, and counts per iteration: instructions, float operations, divides, memory accesses (stack spills and struct loads / stores), branches
and calls, and prices them with the unit's measured operation costs (qa/synth_cost.json "op_ns").

    python custom-fw/tools/pi32_loopcost.py                         every loop of >= 25 instructions in the synth sources, dearest first
    python custom-fw/tools/pi32_loopcost.py --func icarus_filter_process_block --asm out.s
    python custom-fw/tools/pi32_loopcost.py --compare old_dir       the same, against an older copy of the sources (a directory with alpha*.c)
"""
import argparse, os, re, subprocess, sys, tempfile
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
SRC = FW / "src" / "effects" / "icarus"

# ns per operation, from the unit's op-cost probe (jl_ram_read.py readout): float add / multiply 8.3, divide 13.9, int<->float 20-28, stack or struct
# word read-modify-write ~20; a plain load or store is priced at half of that, a taken branch at one add, a call 36.
OP_NS = {"fop": 8.3, "fdiv": 13.9, "load": 10.0, "store": 10.0, "branch": 8.3, "call": 36.0, "other": 4.0}


def toolchain():
    for c in (os.environ.get("PI32_ROOT"), str(Path.home() / "tools" / "jieli" / "C$" / "JL" / "pi32"), r"C:\JL\pi32", r"C:\jieli\pi32"):
        if c and (Path(c) / "bin" / "clang.exe").exists():
            return Path(c)
    return None


def compile_asm(root, src, out, incdir):
    cmd = [str(root / "bin" / "clang.exe"), "-target", "pi32v2", "-mcpu=r3", "-O2", "-ffreestanding", "-fno-builtin", "-isystem", str(root / "pi32v2-include"),
           "-DBOARD_POCKET_AMP=1", "-I" + str(FW / "include"), "-I" + str(incdir), "-S", str(src), "-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("compile failed for %s:\n%s" % (src, r.stderr[-800:]))


def functions(asm):
    cur, body = None, []
    for line in asm.splitlines():
        m = re.match(r"^([A-Za-z_][\w]*):", line)
        if m and not m.group(1).startswith((".L", "BB")):
            if cur:
                yield cur, body
            cur, body = m.group(1), []
        elif cur is not None:
            body.append(line)
    if cur:
        yield cur, body


def classify(line):
    t = line.split(";")[0].strip()
    if not t or t.startswith("."):
        return None
    if re.match(r"^\.LBB|^BB", t) or t.endswith(":"):
        return "label"
    if "(f)" in t:
        return "fdiv" if re.search(r"\s/\s", t) else "fop"
    if "call" in t or re.search(r"\brets\b", t) and "goto" in t:
        return "call"
    if "goto" in t:
        return "branch"
    if "= [" in t or "= b[" in t or "= h[" in t:
        return "load"
    if re.match(r"^(\[|b\[|h\[)", t):
        return "store"
    return "other"


def loops(body):
    labels = {}
    for i, ln in enumerate(body):
        m = re.match(r"^(\.LBB[\w]+):", ln)
        if m:
            labels[m.group(1)] = i
    found = []
    for i, ln in enumerate(body):
        m = re.search(r"goto (\.LBB[\w]+)", ln)
        if m and m.group(1) in labels and labels[m.group(1)] <= i:
            found.append((labels[m.group(1)], i))
    return found


def cost(body, lo, hi):
    c = {"fop": 0, "fdiv": 0, "load": 0, "store": 0, "branch": 0, "call": 0, "other": 0}
    for ln in body[lo:hi + 1]:
        k = classify(ln)
        if k and k != "label":
            c[k] += 1
    n = sum(c.values())
    ns = sum(c[k] * OP_NS[k] for k in c)
    return n, c, ns


def analyse(asm, only=None, min_instr=25):
    rows = []
    for name, body in functions(asm):
        if only and name != only:
            continue
        ls = loops(body)
        # innermost loops: those that contain no other loop
        for (lo, hi) in ls:
            if any((lo2, hi2) != (lo, hi) and lo <= lo2 and hi2 <= hi for (lo2, hi2) in ls):
                continue
            n, c, ns = cost(body, lo, hi)
            if n >= min_instr:
                rows.append((name, n, c, ns))
    return sorted(rows, key=lambda r: -r[3])


def show(rows, label=""):
    print(f"{label}{'function':34s} {'instr':>5s} {'fop':>4s} {'fdiv':>4s} {'load':>4s} {'store':>5s} {'br':>3s} {'call':>4s}   ns/iter  (us per 64-sample block)")
    for name, n, c, ns in rows:
        print(f"{label}{name:34s} {n:5d} {c['fop']:4d} {c['fdiv']:4d} {c['load']:4d} {c['store']:5d} {c['branch']:3d} {c['call']:4d}   {ns:7.0f}  ({ns * 64 / 1000:5.1f})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--func")
    ap.add_argument("--asm", help="also write the assembly of the sources to this file")
    ap.add_argument("--compare", help="a directory with another copy of icarus*.c to compare against")
    ap.add_argument("--min", type=int, default=25)
    ap.add_argument("--body", help="price the WHOLE static body of these functions (comma separated), for per-sample helpers the loops call (alpha_filter_step, alpha_gate_step, ...)")
    args = ap.parse_args()
    root = toolchain()
    if not root:
        print("no pi32 toolchain found (set PI32_ROOT)")
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="pa_pi32_"))
    def build(d):
        out = []
        for s in sorted(Path(d).glob("icarus*.c")):
            o = tmp / (Path(d).name + "_" + s.stem + ".s")
            compile_asm(root, s, o, d)
            out.append(o.read_text(errors="replace"))
        return "\n".join(out)
    new = build(SRC)
    if args.asm:
        Path(args.asm).write_text(new)
    rows = analyse(new, args.func, args.min)
    print("== this tree: innermost loops, by estimated ns per iteration ==")
    show(rows)
    if args.compare:
        old = build(args.compare)
        print("\n== " + args.compare + " ==")
        show(analyse(old, args.func, args.min))
    return 0


if __name__ == "__main__":
    sys.exit(main())
