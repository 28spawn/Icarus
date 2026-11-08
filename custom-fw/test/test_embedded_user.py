#!/usr/bin/env python3
"""custom-fw/test/test_embedded_user.py - imported NAM / IR content, end to end (pa_import.py -> .inc -> firmware C)

Builds a fixture from emu/test_capture.nam and a synthetic 48 kHz IR in a temp folder (never touches the owner's
user_content/ or the real .inc), compiles the firmware pieces with -DPA_USER_EMBEDDED_FORCE=1 pointing at the fixture, and runs test_embedded_user.c.
"""
import subprocess, sys, tempfile, wave
from pathlib import Path
import numpy as np

FW = Path(__file__).resolve().parent.parent
tmp = Path(tempfile.mkdtemp(prefix="pa_embtest_"))
(tmp / "nam").mkdir(); (tmp / "cab").mkdir()
(tmp / "nam" / "test_capture.nam").write_bytes((FW / "emu" / "test_capture.nam").read_bytes())
sr = 48000
t = np.arange(2500) / sr
x = np.concatenate([np.zeros(40), np.exp(-t * 800) * np.sin(2 * np.pi * 2500 * t) * np.random.default_rng(3).standard_normal(2500)])
with wave.open(str(tmp / "cab" / "fixture ir.wav"), "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes((x / np.max(np.abs(x)) * 20000).astype("<i2").tobytes())
inc = tmp / "fixture.inc"
r = subprocess.run([sys.executable, str(FW / "tools" / "pa_import.py"), "--nam-dir", str(tmp / "nam"), "--cab-dir", str(tmp / "cab"), "--out", str(inc)])
if r.returncode: sys.exit("pa_import.py failed")
exe = tmp / "t.exe"
sys.path.insert(0, str(FW / "tools"))
import sources
srcs = ["test/test_embedded_user.c"] + list(sources.sources_for("host_full"))
cmd = ["clang", "-O1", "-Wall", "-Wextra", "-Wno-unused-function", f"-I{FW/'include'}", "-DPA_HOST_SIM=1", "-DPA_USER_EMBEDDED_FORCE=1",
       f"-DPA_USER_EMBEDDED_INC=\"{inc.as_posix()}\"", *[str(FW / s) for s in srcs], "-lm", "-o", str(exe)]
print(" ".join(cmd[:3]), "...")
r = subprocess.run(cmd, cwd=FW, capture_output=True, text=True)
if r.returncode:
    print(r.stdout, r.stderr[-3000:]); sys.exit("build failed")
sys.exit(subprocess.run([str(exe)]).returncode)
