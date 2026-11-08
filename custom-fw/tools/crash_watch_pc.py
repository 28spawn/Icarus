#!/usr/bin/env python3
"""
custom-fw/tools/crash_watch_pc.py - watch the RUNNING unit over USB and capture the last moments before it stops. READ-ONLY (RAM reads over the diag channel, nothing is written).

It samples, as fast as the channel allows (about 15-40 samples a second), the crumbs the firmware keeps in fixed SRAM:
  main-loop checkpoint (0x01C7C3F4), audio-interrupt entries (0x01C7C250), the swap step / flash step / synth stage (0x01C7C980 / 984 / 9B0), the timer interrupt's count and the PC it
  interrupted (0x01C7C9C0), the audio interrupt's last interrupted PC (0x01C7C900), the test state (view, preset, uptime; 0x01C7C390), the SYNTH slot's call count (0x01C7C510).
Every sample goes to a log file with its time; the screen shows changes only. When the unit stops answering for 1.5 s the tool prints the LAST GOOD SAMPLE and the 40 before it (what changed,
and when), says which counters had stopped first, and keeps listening, so it also sees the unit come back (a reset into the download mode does not come back on this port).

    python -u custom-fw/tools/crash_watch_pc.py [--seconds 600] [--log crash_watch.log] [--port "Custom MIDI"]

Run it, THEN open the Icarus screen and play. The unit must be running (not in the download mode) and nothing else may hold its MIDI port.
"""
import argparse
import collections
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ble_matrix_pc as m  # noqa: E402

BLOCKS = [("cp", 0x01C7C3F0, 16), ("isr", 0x01C7C250, 16), ("steps", 0x01C7C980, 16), ("stage", 0x01C7C9B0, 16), ("t5", 0x01C7C9C0, 16), ("pcb", 0x01C7C900, 16),
          ("tsta", 0x01C7C390, 16), ("syn", 0x01C7C510, 16), ("lag", 0x01C7C9D0, 16), ("race", 0x01C7C9E0, 32), ("lad", 0x01C7C9A0, 16), ("hb", 0x01C7C5C0, 32)]
SYM = None


def sym(a):
    global SYM
    if SYM is None:
        try:
            import jl_ram_read as j  # noqa: E402
            SYM = j._sym
        except Exception:  # noqa: BLE001
            SYM = lambda x: ""  # noqa: E731
    return SYM(a) if 0x02000000 <= a < 0x02100000 else ""


def decode(raw):
    w = {k: struct.unpack("<%dI" % (len(raw[k]) // 4), raw[k]) for k in raw}
    return {
        "t_main": w["tsta"][2], "view": w["tsta"][3] & 0xFF, "preset": (w["tsta"][3] >> 8) & 0xFF,
        "cp": w["cp"][1] & 0xFFFF, "isr_n": w["isr"][0],
        "swap": w["steps"][0] & 0xFF, "flash": w["steps"][1] & 0xFF, "stage": w["stage"][0] & 0xFF,
        "t5_n": w["t5"][0], "t5_pc": w["t5"][1], "isr_pc": w["pcb"][1], "isr_rets": w["pcb"][2],
        "syn_calls": w["syn"][2], "lag": w["lag"][0], "lags_done": w["lag"][2], "race_start": w["race"][1], "race_mid": w["race"][2], "race_slices": w["race"][0], "snap_entry": w["stage"][1], "snap_lag": w["stage"][2], "snap_exit": w["stage"][3], "race_restart": w["race"][4], "snapshots": w["race"][6], "calls": w["race"][7], "ladder": w["lad"][3], "hb_ticks": w["hb"][1], "hb_ms": w["hb"][2], "polls": w["hb"][3], "polls_ret": w["hb"][4], "race_cancel": w["race"][5],
    }


def show(s):
    return (f"main={s['t_main']}s view={s['view']} cp={s['cp']:#x} isr#={s['isr_n']} stage={s['stage']:#x} swap={s['swap']:#x} flash={s['flash']:#x} syn_calls={s['syn_calls']} "
            f"lag={s['lag']:#x} lags_done={s['lags_done']} hb[ticks={s['hb_ticks']} ms={s['hb_ms']} polls={s['polls']}/{s['polls_ret']}] race[calls={s['calls']} slices={s['race_slices']} snaps={s['snapshots']} start={s['race_start']} mid={s['race_mid']}] t5#={s['t5_n']} t5_pc={s['t5_pc']:#x}{sym(s['t5_pc'])} isr_pc={s['isr_pc']:#x}{sym(s['isr_pc'])}")


def make_unit(port):
    """The unit's diag channel, polled instead of called back: the rtmidi callback raised SystemError after 12-50 s on this PC (host side, not the unit), which looked like a dead unit."""
    u = m.Unit(port)
    try:
        u.mi.cancel_callback()
    except Exception:  # noqa: BLE001
        pass

    def _call(inner, rep, timeout=1.5, _u=u):
        while _u.mi.get_message():
            pass
        _u.mo.send_message([0xF0] + list(m.pack_8to7(inner)) + [0xF7])
        end = time.time() + timeout
        while time.time() < end:
            ev = _u.mi.get_message()
            if not ev:
                time.sleep(0.002)
                continue
            msg = ev[0]
            if msg and msg[0] == 0xF0 and msg[-1] == 0xF7:
                body = m.unpack_7to8(bytes(msg[1:-1]))
                if len(body) >= 2 and body[0] == 0x7D and body[1] == rep:
                    return body
        raise TimeoutError(f"no reply {rep:#x} from the unit")
    u._call = _call
    return u


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=600.0)
    ap.add_argument("--log", default="crash_watch.log")
    ap.add_argument("--port", default="Custom MIDI")
    a = ap.parse_args()
    u = make_unit(a.port)
    print("BUILD", u.build_id(), flush=True)
    log = open(a.log, "w", encoding="utf-8")
    ring = collections.deque(maxlen=40)
    t0 = time.time()
    last_good = None
    last_print = None
    dead_since = None
    n = 0
    while time.time() - t0 < a.seconds:
        t = time.time() - t0
        try:
            raw = {name: u.ram(addr, ln) for name, addr, ln in BLOCKS}
            s = decode(raw)
        except BaseException as e:  # noqa: BLE001
            if isinstance(e, KeyboardInterrupt):
                raise
            if dead_since is None:
                dead_since = time.time()
            if isinstance(e, SystemError) or not isinstance(e, TimeoutError):
                try:                                  # a host-side USB / MIDI error is not the unit stopping: reopen the port and carry on
                    u = make_unit(a.port)
                except BaseException:  # noqa: BLE001
                    time.sleep(0.3)
            if time.time() - dead_since > 1.5 and last_good is not None:
                print(f"\n===== NO ANSWER from the unit (it stopped OR the USB / MIDI port failed: check jl_ram_read.py synth for its uptime) at t={last_good[0]:.2f}s ({type(e).__name__}) =====", flush=True)
                print("last good sample:\n  " + show(last_good[1]), flush=True)
                print("the 40 samples before it (oldest first; only the fields that changed from the previous one):", flush=True)
                prev = None
                for tt, ss in ring:
                    ch = {k: v for k, v in ss.items() if prev is None or prev.get(k) != v}
                    print(f"  t={tt:7.2f}s " + " ".join((f"{k}={v:#x}{sym(v)}" if k.endswith("_pc") or k.endswith("_rets") else f"{k}={v:#x}") if isinstance(v, int) and v > 255 else f"{k}={v}" for k, v in ch.items()), flush=True)
                    prev = ss
                log.write("STOPPED\n"); log.flush()
                last_good = None
            continue
        dead_since = None
        n += 1
        last_good = (t, s)
        ring.append((t, s))
        log.write(f"{t:.3f} " + show(s) + "\n")
        if n % 20 == 0:
            log.flush()
        key = (s["view"], s["cp"], s["stage"], s["swap"], s["flash"])
        if key != last_print:
            print(f"t={t:7.2f}s " + show(s), flush=True)
            last_print = key
    print(f"done: {n} samples, log in {a.log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
