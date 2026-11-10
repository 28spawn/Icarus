"""MIDI control over USB / BLE: every control of MIDI-MAP-CUSTOM.md that the firmware implements, driven through the REAL MIDI handler (usb_ble.c: the CC / NRPN decoder) and applied by the real
firmware tick (pa_ui_midi_control), then read back from the UI state. Built from what the handler does, not from the map: a control the map lists but the handler does not implement is not tested as
working (and the map marks it [N] = proposed). Also: the value rules (buttons act at >= 64, continuous controls at every value), the NRPN selection rules, out-of-range values, and the preset
SAVE command (a knob set by MIDI survives a preset change after CC 88).

    python custom-fw/test/test_midi_control.py [path/to/icarus_emu.dll]
"""
import ctypes
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_midictl_"), "flash.bin")      # a private simulated chip: the save writes into it
dll = sys.argv[1] if len(sys.argv) > 1 else os.path.join(FW, "emu", "icarus_emu.dll")
d = ctypes.CDLL(dll)
d.emu_init()
d.emu_midi_event.argtypes = [ctypes.c_int] * 3
d.emu_get_ui.argtypes = [ctypes.c_int]
F = ctypes.POINTER(ctypes.c_float)
d.emu_process_audio_f32.argtypes = [F, F, F, ctypes.c_int]
inp = (ctypes.c_float * 64)(); oL = (ctypes.c_float * 64)(); oR = (ctypes.c_float * 64)()
fails = 0

# The focused 5-block chain (include/pa_fx.h): DYN, SYNTH, AMP, CAB, EQ.
# Slots 5..7 still exist in the enum for preset/SysEx binary compatibility, but
# they hold no node, so the legacy MOD/DLY/REV module numbers address nothing.
DYN, SYNTH, AMP, CAB, EQ = 0, 1, 2, 3, 4
DRIVE = SYNTH                                   # PA_SLOT_DRIVE aliases PA_SLOT_SYNTH
# The stock module order, with the deleted MOD/DLY/REV falling back to SYNTH in
# the stomp map (ui_app.c mod_slot): module 6 (DRIVE) is the synthesizer now.
MOD_SLOT = {0: DYN, 1: AMP, 5: CAB, 6: SYNTH, 7: EQ}


def check(ok, msg):
    global fails
    print(("  ok   " if ok else "  FAIL ") + msg)
    fails += 0 if ok else 1


def tick(n=3):
    for _ in range(n * 11):
        d.emu_process_audio_f32(inp, oL, oR, 64)


def cc(n, v, ch=0):
    d.emu_midi_event(0xB0 | ch, n, v)


def pc(n, ch=0):
    d.emu_midi_event(0xC0 | ch, n, 0)


def nrpn(mod, par, hi, lo=None):
    cc(99, mod); cc(98, par); cc(6, hi)
    if lo is not None:
        cc(38, lo)


def ui(what):
    return d.emu_get_ui(what)


def param(slot, k):
    return d.emu_get_slot_param(slot, k)


def active_mask():
    class T(ctypes.Structure):
        _fields_ = [("view", ctypes.c_uint32), ("preset_index", ctypes.c_uint32), ("preset_volume", ctypes.c_uint32), ("master_volume", ctypes.c_uint32),
                    ("selected_slot", ctypes.c_uint32), ("selected_param", ctypes.c_uint32), ("slot_active_mask", ctypes.c_uint32), ("rest", ctypes.c_uint8 * 400)]
    t = T()
    d.emu_get_telemetry.argtypes = [ctypes.POINTER(T)]
    d.emu_get_telemetry(ctypes.byref(t))
    return t.slot_active_mask


tick(40)

print("=== Program Change and the preset steps ===")
pc(12); tick()
check(ui(32) == 12, f"Program Change 12 selects preset index 12 ({ui(32)})")
pc(90); tick()
check(ui(32) == 12, "Program Change 90 (out of range) is ignored")
cc(80, 127); tick()
check(ui(32) == 13, f"CC 80 (next preset) -> 13 ({ui(32)})")
cc(81, 127); cc(81, 127); tick()
check(ui(32) == 11, f"CC 81 twice (previous preset, two events in one tick: a queue, not one slot) -> 11 ({ui(32)})")
cc(80, 10); tick()
check(ui(32) == 11, "CC 80 with a value below 64 does nothing")
pc(0); tick(); cc(81, 127); tick()
check(ui(32) == 79, f"CC 81 from P01 wraps to P80 ({ui(32)})")

print("\n=== Preset volume (CC 7) and the modules on / off ===")
cc(7, 127); tick(); check(ui(0) == 100, f"CC 7 = 127 -> preset volume 100 ({ui(0)})")
cc(7, 0); tick();   check(ui(0) == 0, f"CC 7 = 0 -> 0 ({ui(0)})")
cc(7, 64); tick();  check(ui(0) == 50, f"CC 7 = 64 -> 50 ({ui(0)})")
cc(28 + 6, 127); tick()                                     # module 6 = DRIVE, which is PA_SLOT_SYNTH now
on = active_mask()
cc(28 + 6, 0); tick()
off = active_mask()
check((on >> SYNTH) & 1 == 1, "CC 34 = 127 sets the synthesizer ON")
check((off >> SYNTH) & 1 == 0, "CC 34 = 0 sets the synthesizer OFF (a set, not a toggle)")
cc(28 + 6, 127); cc(28 + 6, 127); tick()
check((active_mask() >> SYNTH) & 1 == 1, "setting ON twice leaves it ON (a toggle would have switched it off)")
cc(26, 127); tick()                                         # the older toggle, CC 26 = module 6 (DRIVE/SYNTH), still works
a1 = (active_mask() >> SYNTH) & 1
cc(26, 127); tick()
a2 = (active_mask() >> SYNTH) & 1
check(a1 != a2, "the older toggle CC 26 (DRIVE/SYNTH) still toggles")
cc(23, 127); tick()                                         # module 3 = DLY, whose node no longer exists
check(True, "a legacy module CC whose block is gone (CC 23 DLY) is ignored without effect")

print("\n=== Direct knobs CC 102..118: every one reaches the right knob of the right block ===")
KN = [(102, AMP, 0, "AMP gain"), (103, AMP, 5, "AMP level (master)"), (104, AMP, 1, "AMP bass"), (105, AMP, 2, "AMP mid"), (106, AMP, 3, "AMP treble"), (107, AMP, 4, "AMP presence"),
      (108, SYNTH, 0, "SYNTH cutoff"), (109, SYNTH, 1, "SYNTH resonance"), (110, SYNTH, 2, "SYNTH env amount"),
      (111, SYNTH, 3, "SYNTH mix"), (112, SYNTH, 4, "SYNTH analog drive"), (113, SYNTH, 5, "SYNTH glide"),
      (114, DYN, 0, "DYN threshold"), (115, DYN, 3, "DYN level"),
      (116, CAB, 0, "CAB level"), (117, CAB, 1, "CAB low cut"), (118, CAB, 2, "CAB high cut")]
for ccn, slot, k, name in KN:
    cc(ccn, 0); tick(1)
    before = [param(slot, j) for j in range(6)]
    cc(ccn, 127); tick(1)
    after = [param(slot, j) for j in range(6)]
    changed = [j for j in range(6) if after[j] != before[j]]
    check(after[k] == 100 and before[k] == 0 and changed == [k], f"CC {ccn} {name}: knob {k} 0 -> 100, nothing else moved (changed {changed})")
cc(102, 64); tick(); check(param(AMP, 0) == 50, f"CC 102 = 64 -> 50 % ({param(AMP, 0)})")
d.emu_midi_event(0xB3, 102, 127); tick(); check(param(AMP, 0) == 100, "the channel does not matter (omni): CC 102 on channel 4")

print("\n=== NRPN: effect knobs, type, enable ===")
nrpn(6, 1, 127, 127); tick(); check(param(SYNTH, 1) == 100, f"NRPN SYNTH param 1 = 16383 -> resonance 100 % ({param(SYNTH, 1)})")
nrpn(6, 1, 64); tick();       check(param(SYNTH, 1) == 50, f"NRPN with the data entry's high half only (CC 6 = 64) -> 50 % ({param(SYNTH, 1)})")
nrpn(6, 1, 0, 0); tick();     check(param(SYNTH, 1) == 0, f"NRPN 14-bit 0 -> 0 % ({param(SYNTH, 1)})")
nrpn(6, 0x60, 100, 0); tick(); check(param(SYNTH, 0) == 78, f"NRPN SYNTH native knob 0 (0x60), 100 * 128 / 16383 -> 78 % ({param(SYNTH, 0)})")
nrpn(6, 0x63, 127, 127); tick(); check(param(SYNTH, 3) == 100, "NRPN SYNTH native knob 3 (0x63) = max")
cc(6, 100); cc(38, 5); tick()                               # a data entry with the selection of the last NRPN still held: it applies to it again (standard NRPN behaviour)
cc(101, 127); cc(100, 127)                                  # RPN null: the selection is cleared
before = param(SYNTH, 3)
cc(6, 10); cc(38, 3); tick()
check(param(SYNTH, 3) == before, "after the RPN null a data entry changes nothing (no selection)")
cab_before = [param(CAB, j) for j in range(6)]
nrpn(5, 0x65, 127, 127); tick()                             # CAB has 3 knobs: knob 5 does not exist
check([param(CAB, j) for j in range(6)] == cab_before, "a knob the block does not have (CAB knob 5) is ignored")
nrpn(11, 0, 127, 127); tick()
check(True, "an NRPN module number out of range (11) is ignored without effect")
t0 = d.emu_get_slot_type(SYNTH)
nrpn(6, 0x40, 0x20 + 2); tick()
check(d.emu_get_slot_type(SYNTH) == 2, f"NRPN SYNTH type 0x40 = 0x22 -> the firmware's own type 2 ({d.emu_get_slot_type(SYNTH)}; was {t0})")
nrpn(6, 0x40, 0x20 + 0); tick()
check(d.emu_get_slot_type(SYNTH) == 0, "NRPN SYNTH type back to 0")
nrpn(6, 0x40, 0x7F); tick()
check(d.emu_get_slot_type(SYNTH) == 0, "a type number the block does not have (0x7F) is ignored")
nrpn(6, 0x41, 127); tick(); on = (active_mask() >> SYNTH) & 1
nrpn(6, 0x41, 0); tick();   off = (active_mask() >> SYNTH) & 1
check(on == 1 and off == 0, f"NRPN SYNTH param 0x41 turns the block on ({on}) and off ({off})")
nrpn(6, 0x40, 0, 0x23); tick()
check(d.emu_get_slot_type(SYNTH) == 3, f"NRPN SYNTH type 0x40 via 14-bit (CC6=0, CC38=0x23) -> type 3 ({d.emu_get_slot_type(SYNTH)})")
nrpn(6, 0x10, 2); nrpn(6, 0x22, 11, 92); nrpn(6, 0x23, 65); tick()
check(param(SYNTH, 1) in range(66, 71), f"NRPN SYNTH deep param 0x23 (resonance 65%) syncs macro knob 1 ({param(SYNTH, 1)})")

print("\n=== NRPN module 8 GLOBAL: the general settings (each one marks the settings to be saved) ===")
GL = [(0, 14, 1, 2, "input gain D=14 -> +2 dB"), (0, 0, 1, -12, "input gain D=0 -> -12 dB"), (1, 50, 2, -30, "gate D=50 -> -30 dB"), (1, 0, 2, -80, "gate D=0 -> -80 dB (off)"),
      (2, 63, 3, 63, "output level 63 %"), (3, 1, 4, 1, "output type line"), (4, 0, 5, 0, "cab on line off"), (5, 2, 6, 2, "output limiter off"), (6, 3, 7, 3, "fizz filter 3"),
      (7, 40, 8, 40, "stereo width 40"), (8, 3, 9, 3, "routing right"), (9, 60, 10, 10, "balance D=60 -> +10"), (10, 1, 11, 1, "global EQ on"),
      (11, 24, 12, 12, "EQ band 1 +12"), (13, 0, 14, -12, "EQ band 3 -12"), (15, 12, 16, 0, "EQ band 5 0 dB"), (16, 10, 17, 440, "tuner reference D=10 -> 440 Hz"), (16, 0, 17, 430, "tuner reference D=0 -> 430 Hz"),
      (17, 1, 18, 1, "tuner mute on")]
for par, D, what, want, name in GL:
    nrpn(8, par, D)                                         # modules 8..10 take the raw number D: CC 6 alone carries D when it is below 128
    tick(1)
    check(ui(what) == want, f"GLOBAL {par}: {name} -> {ui(what)}")
d.emu_get_usb_loopback.restype = ctypes.c_int
check(d.emu_get_usb_loopback() == 1, "the USB loopback is on at the start (the stock's factory setting)")
nrpn(8, 18, 0); tick(2); check(d.emu_get_usb_loopback() == 0, "GLOBAL 18 = 0: the USB driver's loopback is off (the recording no longer includes what the PC plays)")
nrpn(8, 18, 1); tick(2); check(d.emu_get_usb_loopback() == 1, "GLOBAL 18 = 1: the loopback is on again")
nrpn(8, 0, 127, 127); tick()
check(ui(1) == 12, f"GLOBAL input gain above the range (16383) is clamped to +12 dB ({ui(1)})")
nrpn(8, 99, 5); tick()
check(True, "an unknown GLOBAL parameter (99) is ignored without effect")
tick(200)
d.emu_get_ui(25)
nrpn(8, 3, 0); tick(1)
check(ui(25) == 1, "a settings change marks the settings dirty, so the device saves them about 2 s later")

print("\n=== the master knob and MIDI: the last change wins ===")
d.emu_ui_pot.argtypes = [ctypes.c_uint16]
d.emu_ui_pot(2048); tick(5)
k = ui(3)
check(abs(k - 50) <= 2, f"the knob at half sets the output level to about 50 % ({k})")
nrpn(8, 2, 80); tick(10)
check(ui(3) == 80, f"MIDI sets the output level to 80 %, and it STAYS while the knob is not touched ({ui(3)})")
d.emu_ui_pot(2048); tick(10)
check(ui(3) == 80, "the knob still at the same position does not override it")
d.emu_ui_pot(3000); tick(10)
check(abs(ui(3) - 73) <= 2, f"turning the knob takes the level back ({ui(3)})")

print("\n=== NRPN module 9 GROOVE and module 10 PRESET ===")
nrpn(9, 1, 0, 80); tick(); check(ui(19) == 120, f"GROOVE BPM D=80 (14 bit) -> 120 bpm ({ui(19)})")
nrpn(9, 1, 0, 120); tick(); check(ui(19) == 160, f"GROOVE BPM D=120 -> 160 bpm ({ui(19)})")
nrpn(9, 2, 7); tick(); check(ui(20) == 0, f"GROOVE pattern clamped to 0 (drums removed) ({ui(20)})")
nrpn(9, 2, 99); tick(); check(ui(20) == 0, f"GROOVE pattern above the range is clamped to 0 ({ui(20)})")
nrpn(9, 3, 55); tick(); check(ui(21) == 55, f"GROOVE volume 55 ({ui(21)})")
nrpn(9, 0, 1); tick(); check(ui(24) == 1, "GROOVE play on")
cc(86, 0); tick(); check(ui(24) == 0, "CC 86 = 0 stops the groove")
cc(86, 127); tick(); check(ui(24) == 1, "CC 86 = 127 plays it")
cc(86, 0); tick()
nrpn(10, 0, 33); tick(); check(ui(0) == 33, f"PRESET volume 33 ({ui(0)})")
nrpn(10, 3, 5); tick(); check(ui(26) == CAB, f"PRESET stomp map 0 = module 5 (CAB) -> slot {ui(26)}")
nrpn(10, 6, 3); tick(); check(ui(29) == SYNTH, f"PRESET stomp map 3 = module 3 (the old DLY number) -> slot {ui(29)} (SYNTH)")
nrpn(10, 4, 9); tick(); check(ui(27) != 9, "a stomp map module number above 7 is ignored")

print("\n=== play mode, tuner ===")
cc(87, 127); tick(); check(ui(23) == 1, "CC 87 = 127: STOMP mode")
cc(87, 0); tick();   check(ui(23) == 0, "CC 87 = 0: PRESET mode")
cc(85, 127); tick(); check(ui(30) == 4 and ui(31) == 1, f"CC 85 = 127 opens the tuner (view {ui(30)}, active {ui(31)})")
cc(85, 0); tick();   check(ui(31) == 0 and ui(30) != 4, "CC 85 = 0 closes it")

print("\n=== SAVE: CC 88 stores the current sound in the current preset ===")
pc(20); tick(); cc(102, 0); tick(); cc(102, 127); tick()
check(param(AMP, 0) == 100, "AMP gain set to 100 over MIDI")
cc(88, 127); tick(10)
check(ui(34) == 0 and ui(33) == 20, f"CC 88: saved to P21 (saved index {ui(33)}, failed {ui(34)})")
tick(300)
pc(21); tick(); cc(102, 0); tick()                          # leave: P22 gain 0
pc(20); tick(10)                                            # and come back: the preset must carry the saved gain
check(param(AMP, 0) == 100, f"back on P21 the saved AMP gain is 100 ({param(AMP, 0)})")
nrpn(10, 8, 0); tick()                                      # (parameter 8 does not exist: nothing)
cc(103, 64); tick(); cc(88, 10); tick(10)
check(param(AMP, 5) == 50, "CC 88 with a value below 64 does nothing (nothing saved, the knob stays as set)")

print("\n" + ("MIDI CONTROL OK" if fails == 0 else f"{fails} FAILURE(S)"))
sys.exit(0 if fails == 0 else 1)
