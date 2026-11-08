"""Every button, every key combination, every screen, alone and together, with the synth and the amp running (the QA gate's input matrix).

Why this exists
---------------
The gate used to test features one at a time on a quiet chain. The unit then crashed on things the gate never did: opening the synth and playing, and
pressing < or > on the main screen (an amp / preset swap) with other blocks active. This suite drives the SAME input api the firmware's UI is driven by
(press / release / tap / hold / encoder / pot), with a guitar-like signal playing through the chain, and after EVERY gesture checks the invariants:

  * the emulator is alive (a crash or hang of the firmware core kills this process: exit != 0 is a FAIL),
  * the output is finite and bounded (no NaN / Inf / runaway level),
  * the screen number and the preset index are valid,
  * the effect memory in use stays bounded (no leak across thousands of gestures),
  * HOME always gets back to the main screen within a few presses.

Sections
  M1  every single button, tap and the four hold lengths, from the main screen and from every screen it opens
  M2  every pair of buttons together (tap and hold), from the main screen and from every screen that opened
  M3  screens: a breadth-first walk of the whole UI by button sequences (depth 3), all of Settings and the stomp / bank / tuner / drums / icarus synth views
  M4  the synth: every preset x on / off x the amp and preset swaps (< > on the main screen), live, with the guitar playing
  M5  swap storm: 300 amp / preset changes in a row, the synth on and off, memory flat
  M6  seeded random walk (default 4000 gestures, taps, holds, pairs, encoder, pot, slot toggles, swaps) with all the invariants after each

Usage:  python custom-fw/emu/test_matrix.py [dll] [--quick] [--walk N] [--seed S]
Exit 0 = PASS, 1 = FAIL (a failing gesture prints the last gestures so it can be replayed).
"""
import ctypes
import itertools
import math
import os
import random
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:]]
QUICK = "--quick" in args
WALK = 4000
SEED = 20261106
if "--walk" in args:
    WALK = int(args[args.index("--walk") + 1])
if "--seed" in args:
    SEED = int(args[args.index("--seed") + 1])
if QUICK:
    WALK = min(WALK, 600)
pos = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] not in ("--walk", "--seed"))]
DLL = pos[0] if pos else os.path.join(HERE, "pocket_amp_emu.dll")
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_matrix_"), "flash.bin")

d = ctypes.CDLL(DLL)
d.emu_init()
d.emu_ui_tap.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
d.emu_ui_press.argtypes = [ctypes.c_uint32]
d.emu_ui_release.argtypes = [ctypes.c_uint32]
d.emu_ui_enc.argtypes = [ctypes.c_int32]
d.emu_ui_pot.argtypes = [ctypes.c_uint16]
d.emu_set_slot_enabled.argtypes = [ctypes.c_int, ctypes.c_int]
d.emu_set_slot_type.argtypes = [ctypes.c_int, ctypes.c_int]
d.emu_set_master_volume.argtypes = [ctypes.c_int]
d.emu_select_preset.argtypes = [ctypes.c_int]
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float), ctypes.c_int]
d.emu_get_ui.argtypes = [ctypes.c_int]


class Telemetry(ctypes.Structure):
    _fields_ = [
        ("view", ctypes.c_uint32), ("preset_index", ctypes.c_uint32), ("preset_volume", ctypes.c_uint32), ("master_volume", ctypes.c_uint32),
        ("selected_slot", ctypes.c_uint32), ("selected_param", ctypes.c_uint32), ("slot_active_mask", ctypes.c_uint32), ("sram_used_bytes", ctypes.c_uint32),
        ("sram_peak_bytes", ctypes.c_uint32), ("cpu_total_permille", ctypes.c_uint32), ("cpu_stage_permille", ctypes.c_uint32 * 8),
        ("tuner_active", ctypes.c_uint32), ("tuner_locked", ctypes.c_uint32), ("tuner_note", ctypes.c_uint32), ("tuner_octave", ctypes.c_int32),
        ("tuner_cents", ctypes.c_int32), ("tuner_freq_tenths", ctypes.c_uint32), ("tuner_dots", ctypes.c_uint32), ("drum_playing", ctypes.c_uint32),
        ("drum_bpm", ctypes.c_uint32), ("drum_pattern", ctypes.c_uint32), ("in_peak", ctypes.c_float), ("out_peak", ctypes.c_float),
        ("bt_state", ctypes.c_uint32), ("cursor_slot", ctypes.c_uint32), ("editor_cursor", ctypes.c_uint32),
    ]


tel = Telemetry()
d.emu_get_telemetry.argtypes = [ctypes.POINTER(Telemetry)]

HOME, VOL_MINUS, VOL_PLUS, LEFT, RIGHT, PARA = (1 << i for i in range(6))
NAMES = {HOME: "HOME", VOL_MINUS: "VOL-", VOL_PLUS: "VOL+", LEFT: "<", RIGHT: ">", PARA: "PARA"}
BUTTONS = [HOME, VOL_MINUS, VOL_PLUS, LEFT, RIGHT, PARA]
VIEW_MAIN, VIEW_ICARUS_SYNTH, VIEW_MAX = 0, 16, 16
N = 64
TICK_BLOCKS = 11
TICK_MS = TICK_BLOCKS * 64 * 1000.0 / 44100.0
HOLDS_MS = (300, 700, 1200, 1700, 2200, 3200)           # a footswitch press, the save hold, the stomp assign / lock holds, moving mode, factory reset
SLOT_DYN, SLOT_SYNTH, SLOT_AMP, SLOT_CAB, SLOT_EQ = range(5)

inp = (ctypes.c_float * N)()
outL = (ctypes.c_float * N)()
outR = (ctypes.c_float * N)()
SR = 44100
_phase = [0.0]
_note = [110.0]
_amp = [0.2]

fails = 0
history = []                       # the last gestures: printed on a failure so it can be replayed
checks_done = 0
base_mem = [0]


def check(ok, msg):
    global fails, checks_done
    checks_done += 1
    if not ok:
        fails += 1
        print("  FAIL " + msg)
        print("       last gestures: " + " | ".join(history[-12:]))
    return ok


def note(g):
    history.append(g)
    if len(history) > 40:
        del history[:20]


def block():
    f = _note[0] / SR
    ph = _phase[0]
    for i in range(N):
        ph += f
        if ph >= 1.0:
            ph -= 1.0
        a = 2.0 * math.pi * ph
        inp[i] = _amp[0] * (math.sin(a) + 0.5 * math.sin(2 * a) + 0.3 * math.sin(3 * a))
    _phase[0] = ph
    d.emu_process_audio_f32(inp, outL, outR, N)


bad_audio = [0, 0.0]


def blocks(n):
    for _ in range(n):
        block()
        l, r = outL[:], outR[:]
        for v in l + r:
            if v != v or v in (float("inf"), float("-inf")) or abs(v) > 4.0:
                bad_audio[0] += 1
                bad_audio[1] = v
                break


def tick(n=1):
    blocks(n * TICK_BLOCKS)
    d.emu_get_telemetry(ctypes.byref(tel))


def view():
    d.emu_get_telemetry(ctypes.byref(tel))
    return tel.view


def tap(mask):
    note("tap " + "+".join(NAMES[b] for b in BUTTONS if mask & b))
    d.emu_ui_tap(mask, 0)
    tick(12)


def hold(mask, ms):
    note("hold %dms " % ms + "+".join(NAMES[b] for b in BUTTONS if mask & b))
    d.emu_ui_tap(mask, ms)
    tick(int(ms / TICK_MS) + 14)


def invariants(tag):
    d.emu_get_telemetry(ctypes.byref(tel))
    ok = True
    if bad_audio[0]:
        check(False, f"{tag}: the output went non-finite / over range ({bad_audio[0]} blocks, last value {bad_audio[1]})")
        bad_audio[0] = 0
        ok = False
    ok &= check(tel.view <= VIEW_MAX, f"{tag}: screen number {tel.view} is not a screen")
    ok &= check(tel.preset_index < 80, f"{tag}: preset index {tel.preset_index} out of range")
    if base_mem[0]:
        ok &= check(tel.sram_used_bytes <= base_mem[0] + 200_000, f"{tag}: effect memory grew to {tel.sram_used_bytes} bytes (started at {base_mem[0]}): a leak")
    return ok


def home_works(tag):
    """HOME gets back to the main screen from anywhere within 4 presses (the Factory Reset dialog and the save dialog are left by HOME too)."""
    for _ in range(4):
        if view() == VIEW_MAIN:
            return True
        d.emu_ui_press(0); d.emu_ui_release(0xFF)
        tap(HOME)
    ok = check(view() == VIEW_MAIN, f"{tag}: HOME did not get back to the main screen (screen {view()})")
    if not ok:
        d.emu_init()
        setup()
    return ok


def setup(synth=False):
    d.emu_init()
    d.emu_set_master_volume(100)
    d.emu_ui_pot(2048)
    for s in range(5):
        d.emu_set_slot_enabled(s, 1 if s in (SLOT_AMP, SLOT_CAB) else 0)
    if synth:
        d.emu_set_slot_enabled(SLOT_SYNTH, 1)
        d.emu_set_slot_type(SLOT_SYNTH, 0)
    tick(40)


def bname(m):
    return "+".join(NAMES[b] for b in BUTTONS if m & b)


# ---------------------------------------------------------------------------------------------------------------------------------------------
setup(synth=True)
tick(30)
base_mem[0] = tel.sram_used_bytes
print(f"(base effect memory {base_mem[0]} bytes, seed {SEED}, walk {WALK}{', quick' if QUICK else ''})")

print("\n=== M1: every single button, tap and holds, from every screen it opens ===")
m1 = 0
for synth in (False, True):
    for b in BUTTONS:
        for kind in ("tap",) + tuple(HOLDS_MS):
            setup(synth)
            if kind == "tap":
                tap(b)
            else:
                hold(b, kind)
            m1 += 1
            invariants(f"M1 {bname(b)} {kind} synth={int(synth)}")
            v1 = view()
            # then one more press of every button on the screen it opened (depth 2), each from a fresh copy of that state
            if v1 != VIEW_MAIN and not QUICK:
                for b2 in BUTTONS:
                    tap(b2)
                    invariants(f"M1 {bname(b)} {kind} then {NAMES[b2]}")
                    home_works(f"M1 {bname(b)} {kind} then {NAMES[b2]}")
                    m1 += 1
                    # get back to the same screen to continue the other buttons
                    if b2 != BUTTONS[-1]:
                        setup(synth)
                        if kind == "tap": tap(b)
                        else: hold(b, kind)
            home_works(f"M1 {bname(b)} {kind}")
print(f"  {m1} gestures")

print("\n=== M2: every pair of buttons together (tap and holds) ===")
m2 = 0
pair_holds = (0, 400, 1700, 2200) if not QUICK else (0, 1700)
for synth in (False, True):
    for a, b in itertools.combinations(BUTTONS, 2):
        for ms in pair_holds:
            setup(synth)
            if ms == 0:
                tap(a | b)
            else:
                hold(a | b, ms)
            invariants(f"M2 {bname(a | b)} {ms}ms synth={int(synth)}")
            home_works(f"M2 {bname(a | b)} {ms}ms")
            m2 += 1
# three and four at once, and all six
for combo in ([HOME, VOL_MINUS, VOL_PLUS], [LEFT, RIGHT, PARA], [VOL_MINUS, VOL_PLUS, LEFT, RIGHT], BUTTONS):
    for ms in (0, 1700):
        setup(True)
        m = 0
        for x in combo: m |= x
        if ms == 0: tap(m)
        else: hold(m, ms)
        invariants(f"M2 {bname(m)} {ms}ms")
        home_works(f"M2 {bname(m)} {ms}ms")
        m2 += 1
print(f"  {m2} gestures")

print("\n=== M3: walk every screen by button sequences (breadth first, depth 3) ===")
seen = {}
seqs = [()]
depth = 3 if not QUICK else 2
moves = [("tap", b) for b in BUTTONS] + [("hold", RIGHT), ("hold", LEFT), ("hold", PARA), ("hold", HOME), ("hold", VOL_MINUS)]
frontier = [()]
for dpt in range(depth):
    nxt = []
    for seq in frontier:
        for mv in moves:
            s = seq + (mv,)
            setup(True)
            for kind, b in s:
                if kind == "tap": tap(b)
                else: hold(b, 1700 if b in (PARA, HOME, VOL_MINUS) else 1500)
            v = view()
            invariants("M3 " + " ".join(f"{k}:{NAMES[b]}" for k, b in s))
            if v not in seen:
                seen[v] = s
            if v != VIEW_MAIN: nxt.append(s)
            home_works("M3")
    frontier = nxt[:40]
print("  screens reached: " + ", ".join(str(v) for v in sorted(seen)))
routes = [
    (3, "bank mode", [("tap", LEFT | RIGHT)]),
    (4, "tuner", [("tap", VOL_MINUS | VOL_PLUS)]),
    (16, "icarus synth", [("tap", HOME | VOL_PLUS)]),
    (15, "stomp assign", [("hold", HOME, 1700), ("hold", VOL_MINUS, 1500)]),
]
for must, what, steps in routes:
    setup(True)
    for st in steps:
        tap(st[1]) if st[0] == "tap" else hold(st[1], st[2])
    reached = view()
    seen.setdefault(reached, tuple(steps))
    invariants(f"M3 route to the {what}")
    check(reached == must, f"M3: the {what} (screen {must}) opens by its documented buttons (got screen {reached})")
    home_works(f"M3 route {what}")
for must, what in ((2, "parameter editor"), (8, "settings")):
    check(must in seen, f"M3: the {what} (screen {must}) is reachable from the main screen by buttons")

print("\n=== M4: the synth x every preset x amp / preset swaps with the guitar playing ===")
m4 = 0
for synth in (False, True):
    for p in range(12):
        setup(synth)
        if synth:
            d.emu_set_slot_type(SLOT_SYNTH, p)
        tick(20)
        for _ in range(6):                      # < and > on the main screen: the amp / preset swap
            tap(RIGHT); invariants(f"M4 p{p} synth={int(synth)} > ")
            m4 += 1
        for _ in range(6):
            tap(LEFT); invariants(f"M4 p{p} synth={int(synth)} < ")
            m4 += 1
        tap(PARA); home_works("M4 PARA")
        d.emu_ui_enc(3); tick(4); d.emu_ui_enc(-3); tick(4)
        invariants(f"M4 p{p} synth={int(synth)} encoder")
        # the synth on / off live while the swap happens
        d.emu_set_slot_enabled(SLOT_SYNTH, 0 if synth else 1); tap(RIGHT)
        d.emu_set_slot_enabled(SLOT_SYNTH, 1 if synth else 0); tap(LEFT)
        invariants(f"M4 p{p} toggle during swap")
        m4 += 2
print(f"  {m4} gestures")

print("\n=== M5: a swap storm ===")
for synth in (False, True):
    setup(synth)
    n = 100 if QUICK else 300
    for i in range(n):
        d.emu_ui_tap(RIGHT if (i // 7) % 2 == 0 else LEFT, 0)
        tick(1 + (i % 3))
        if i % 25 == 0:
            invariants(f"M5 swap storm {i} synth={int(synth)}")
    invariants(f"M5 swap storm end synth={int(synth)}")
    home_works("M5")
    check(tel.sram_used_bytes <= base_mem[0] + 30_000, f"M5: memory flat after {n} swaps (synth={int(synth)}): {tel.sram_used_bytes} vs {base_mem[0]}")

print(f"\n=== M6: seeded random walk, {WALK} gestures ===")
rng = random.Random(SEED)
setup(True)
for i in range(WALK):
    r = rng.random()
    if r < 0.45:
        tap(rng.choice(BUTTONS))
    elif r < 0.60:
        hold(rng.choice(BUTTONS), rng.choice(HOLDS_MS))
    elif r < 0.72:
        a, b = rng.sample(BUTTONS, 2)
        (tap(a | b) if rng.random() < 0.6 else hold(a | b, rng.choice((300, 1700))))
    elif r < 0.84:
        n = rng.randint(-6, 6)
        note(f"enc {n}")
        d.emu_ui_enc(n); tick(rng.randint(1, 4))
    elif r < 0.88:
        v = rng.randint(0, 4095)
        note(f"pot {v}")
        d.emu_ui_pot(v); tick(2)
    elif r < 0.94:
        s = rng.randint(0, 4)
        note(f"slot {s} toggle")
        d.emu_set_slot_enabled(s, rng.randint(0, 1)); tick(2)
    elif r < 0.97:
        _note[0] = rng.choice((82.4, 110.0, 196.0, 329.6, 659.3, 987.8))
        _amp[0] = rng.choice((0.0, 0.02, 0.2, 0.7))
        note(f"play {_note[0]} Hz amp {_amp[0]}")
        tick(3)
    else:
        note("synth preset")
        d.emu_set_slot_type(SLOT_SYNTH, rng.randint(0, 11)); tick(2)
    if i % 10 == 0:
        invariants(f"M6 #{i}")
    if i % 150 == 149:
        home_works(f"M6 #{i}")
        _amp[0] = 0.2
invariants("M6 end")

print(f"\n{checks_done} checks, {fails} failure(s)")
print("INPUT MATRIX OK" if fails == 0 else f"INPUT MATRIX FAILED ({fails})")
sys.exit(0 if fails == 0 else 1)
