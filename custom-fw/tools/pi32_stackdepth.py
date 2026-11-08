#!/usr/bin/env python3
"""
custom-fw/tools/pi32_stackdepth.py - the worst-case stack depth of the audio interrupt, from the firmware ELF.

The audio interrupt (isr_alnk0 -> pa_alnk0_isr_c -> the whole effect chain on core 0, the synth included) runs on the SYSTEM stack: 0x01C7A100..0x01C7C000, 7,936 bytes,
with no watermark (the readout's "user stack ... deepest use" is the main loop's stack and says nothing about it). A stack that is too small does not fail gracefully: the
write-limit guard fires, the fault handler needs the same stack, and the unit drops to the download mode with no crash record.

Frames are read from the disassembly (register pushes + `sp += -N`), the call graph from the direct calls; the calls through function pointers (the chain's slot run
functions, the node callbacks) are added by hand below. Every number is a STATIC upper bound on the path, not a measurement.

    python custom-fw/tools/pi32_stackdepth.py              the deepest path from the audio interrupt
    python custom-fw/tools/pi32_stackdepth.py --top 12     more functions
    python custom-fw/tools/pi32_stackdepth.py --limit 7936 exit 1 if the deepest path is over the limit (the gate uses this)
"""
import argparse, re, subprocess, sys
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
ELF = FW / "build_pi32" / "pocket_amp_custom.elf"
SYS_STACK = 0x01C7C000 - 0x01C7A100          # 7,936 bytes

# calls the call graph cannot see: slot run functions and node callbacks are reached through pointers
INDIRECT = {
    "pa_audio_core_process_half": ["pa_audio_core_process_block", "pa_fx_chain_run_stereo", "pa_fx_chain_run", "pa_mixlim_process"],
    "pa_audio_core_process_block": ["pa_fx_chain_run_stereo", "pa_fx_chain_run", "pa_mixlim_process"],
    "pa_fx_chain_run": ["pa_fx_chain_run_stereo"],
    "run_slot_range": ["fx_synth_run_mono", "fx_synth_run_stereo", "dynamics_run", "eq_run", "eq_run_stereo", "cab_ir_run", "cab_ir_run_stereo", "amp_slot_run",
                       "amp_post_run", "pa_wavenet_process_block_q21", "nam_gru_run_block"],
}
# what an interrupt costs on entry: isr_alnk0 pushes psr, rets, reti and r3-r0 (7 words) before the C handler
ISR_ENTRY = 7 * 4


def toolchain():
    for c in (str(Path.home() / "tools" / "jieli" / "C$" / "JL" / "pi32"), r"C:\JL\pi32", r"C:\jieli\pi32"):
        if (Path(c) / "bin" / "llvm-objdump.exe").exists():
            return Path(c)
    return None


def disassemble():
    root = toolchain()
    if not root or not ELF.exists():
        raise SystemExit("need build_pi32/pocket_amp_custom.elf and the pi32 toolchain (run build.ps1)")
    return subprocess.run([str(root / "bin" / "llvm-objdump.exe"), "-d", str(ELF)], capture_output=True, text=True).stdout


def parse(text):
    funcs, cur, order = {}, None, []
    for line in text.splitlines():
        m = re.match(r"^([0-9a-fA-F]+) <(.+)>:", line) or re.match(r"^([A-Za-z_][\w.]*):\s*$", line)
        if m and not line.startswith(" "):
            cur = m.group(m.lastindex)
            funcs[cur] = {"frame": 0, "calls": set(), "seen_ret": False, "n": 0}
            order.append(cur)
            continue
        if cur is None:
            continue
        f = funcs[cur]
        f["n"] += 1
        t = line.split("\t")[-1].strip()
        if "[--sp] = {" in t:
            lst = t.split("{", 1)[1].split("}", 1)[0]
            words = 0
            for part in lst.split(","):
                part = part.strip()
                r = re.match(r"r(\d+)-r(\d+)", part)
                words += (abs(int(r.group(1)) - int(r.group(2))) + 1) if r else (1 if part else 0)
            f["frame"] += 4 * words
        m2 = re.match(r"sp \+= (-?\d+)", t)
        if m2 and int(m2.group(1)) < 0:
            f["frame"] += -int(m2.group(1))
        m3 = re.search(r"\bcall \d+ <([^ >:]+)", t)
        if m3:
            f["calls"].add(m3.group(1))
    return funcs


def depth(funcs, name, memo, stack):
    if name in memo:
        return memo[name]
    if name in stack or name not in funcs:
        return (0, [name])
    stack.add(name)
    f = funcs[name]
    best = (0, [])
    targets = set(f["calls"]) | set(INDIRECT.get(name, []))
    for c in targets:
        d, chain = depth(funcs, c, memo, stack)
        if d > best[0]:
            best = (d, chain)
    stack.discard(name)
    memo[name] = (f["frame"] + best[0], [name] + best[1])
    return memo[name]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=3)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--root", default="pa_alnk0_isr_c")
    args = ap.parse_args()
    funcs = parse(disassemble())
    if args.root not in funcs:
        raise SystemExit(f"{args.root} not found in the ELF")
    memo = {}
    total, chain = depth(funcs, args.root, memo, set())
    total += ISR_ENTRY
    print(f"audio interrupt stack, worst static path: {total} bytes of {SYS_STACK} ({100 * total / SYS_STACK:.0f} %)")
    print("  path: " + " -> ".join(f"{n}({funcs[n]['frame']})" for n in chain if n in funcs))
    ranked = sorted(((memo[n][0], n) for n in memo if n in funcs), reverse=True)[:args.top]
    print("  deepest sub-trees: " + ", ".join(f"{n} {d}" for d, n in ranked))
    if args.limit and total > args.limit:
        print(f"  OVER the limit of {args.limit} bytes")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
