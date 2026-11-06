#!/usr/bin/env python3
"""Snapshot the screens that matter for the Project Alpha look: the home grid and list, every block page, the Project Alpha pages,
settings. Writes PNGs to custom-fw/emu/ui_shots/ (or argv[1]).  Run after build.ps1 so the DLL is current.

    python custom-fw/emu/snap_screens.py [out_dir]
"""
import ctypes, os, sys, tempfile
from pathlib import Path
from PIL import Image

EMU = Path(__file__).resolve().parent
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else EMU / "ui_shots"
OUT.mkdir(parents=True, exist_ok=True)
os.environ["PA_FLASH_FILE"] = os.path.join(tempfile.mkdtemp(prefix="pa_snap_"), "flash.bin")

d = ctypes.CDLL(str(EMU / "pocket_amp_emu.dll"))
d.emu_init.restype = None
d.emu_ui_tap.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
d.emu_ui_enc.argtypes = [ctypes.c_int32]
d.emu_process_audio_f32.argtypes = [ctypes.POINTER(ctypes.c_float)] * 3 + [ctypes.c_int]
d.emu_get_lcd_rgba.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
d.emu_set_slot_enabled.argtypes = [ctypes.c_int, ctypes.c_int]

HOME, VOLM, VOLP, LEFT, RIGHT, PARA = 1, 2, 4, 8, 16, 32
N = 64
inp = (ctypes.c_float * N)(); oL = (ctypes.c_float * N)(); oR = (ctypes.c_float * N)()
phase = [0.0]


def tick(n=1, play=True):
    import math
    for _ in range(n * 11):
        for i in range(N):
            phase[0] += 196.0 / 44100.0
            inp[i] = (0.15 * math.sin(6.2831853 * phase[0]) + 0.07 * math.sin(12.566 * phase[0])) if play else 0.0
        d.emu_process_audio_f32(inp, oL, oR, N)


class _Tel(ctypes.Structure):
    _fields_ = [("view", ctypes.c_uint32)] + [(f"f{i}", ctypes.c_uint32) for i in range(40)]
_tel = _Tel()
d.emu_get_telemetry.argtypes = [ctypes.POINTER(_Tel)]


def view():
    d.emu_get_telemetry(ctypes.byref(_tel))
    return _tel.view


def tap(m): d.emu_ui_tap(m, 0); tick(25)


def to_grid():
    if view() != 0:
        tap(HOME)
def enc(n): d.emu_ui_enc(n); tick(25)


def shot(name):
    buf = (ctypes.c_uint8 * (240 * 240 * 4))()
    d.emu_get_lcd_rgba(buf)
    Image.frombytes("RGBA", (240, 240), bytes(buf)).resize((480, 480), Image.NEAREST).save(OUT / f"{name}.png")
    print("  ", name)


d.emu_init()
tick(40)
shot("home_grid")
for i in range(5):
    tap(RIGHT)
    tick(5)
    shot(f"home_cursor{i + 1}")
# block pages: the cursor starts on AMP (index 2); open each block from there
for i, step in enumerate((-2, -1, 0, 1, 2)):
    to_grid()
    for _ in range(abs(step)):
        tap(LEFT if step < 0 else RIGHT)
    tap(PARA); tick(10)
    shot(f"page_slot{i}")
    to_grid()
    for _ in range(abs(step)):
        tap(RIGHT if step < 0 else LEFT)   # put the cursor back on AMP
# the settings menu
d.emu_init(); tick(40)
tap(LEFT); tick(5)
shot("settings_or_prev")
# Project Alpha: HOME + VOL+ enters, < > walk the tabs
d.emu_init(); tick(40)
d.emu_ui_tap(HOME | VOLP, 0); tick(40)
shot("alpha_main")
for t in range(1, 8):
    tap(RIGHT); tick(10)
    shot(f"alpha_tab{t}")
