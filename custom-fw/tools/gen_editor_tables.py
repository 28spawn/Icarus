#!/usr/bin/env python3
"""
custom-fw/tools/gen_editor_tables.py - the model and knob names of the web editor (web/Icarus_Web_Editor.html), taken from the firmware's own tables in src/board/ui_app.c, so the page can not drift
from the unit's screen. The amp and cabinet names are NOT here: the page reads them from the unit (cmd 81).

    python custom-fw/tools/gen_editor_tables.py            rewrite the block between the TABLES markers in web/Icarus_Web_Editor.html
    python custom-fw/tools/gen_editor_tables.py --check    exit 1 when the page's block differs from the firmware (the gate runs this)
"""
import json, re, sys
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
SRC = FW / "src" / "board" / "ui_app.c"
PAGE = FW / "web" / "Icarus_Web_Editor.html"
BEGIN, END = "/*TABLES-BEGIN (tools/gen_editor_tables.py: do not edit)*/", "/*TABLES-END*/"


def strs(body):
    return re.findall(r'"((?:[^"\\]|\\.)*)"', body)


def array(text, name, dims):
    m = re.search(r"static const char \* const " + name + r"\[[^=]*\]\s*=\s*\{(.*?)\n?\s*\};", text, re.S)
    if not m:
        raise SystemExit(f"gen_editor_tables: {name} not found in ui_app.c")
    flat = strs(re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S))
    if dims == 1:
        return flat
    assert len(flat) % dims == 0, name
    return [flat[i:i + dims] for i in range(0, len(flat), dims)]


def synth_names():
    """Slot 1 is the Icarus Synth synth: its models are the 12 factory presets in src/effects/icarus/icarus.c."""
    a = (FW / "src" / "effects" / "icarus" / "icarus.c").read_text(encoding="utf-8")
    return re.findall(r'\.name\s*=\s*"([^"]*)"', a)


def synth_presets():
    """Extract the full parameter dicts of the 12 factory synth presets from icarus.c for the Web Editor."""
    a = (FW / "src" / "effects" / "icarus" / "icarus.c").read_text(encoding="utf-8")
    body = re.search(r"static const icarus_preset_t k_factory_presets\[[^\]]*\]\s*=\s*\{(.*?)\n\};", a, re.S).group(1)
    enums = {
        "ICARUS_MODE_ANALOG": 0, "ICARUS_MODE_OSCILLATOR": 1, "ICARUS_MODE_HYBRID": 2, "ICARUS_MODE_GUITAR": 3,
        "ICARUS_DIST_SOFT_CLIP": 0, "ICARUS_DIST_HARD_CLIP": 1, "ICARUS_DIST_FOLDBACK": 2, "ICARUS_DIST_HEXA": 3, "ICARUS_DIST_FUZZ": 4,
        "ICARUS_WAVE_SAW": 0, "ICARUS_WAVE_SQUARE": 1, "ICARUS_WAVE_TRIANGLE": 2,
        "ICARUS_SUB_OFF": 0, "ICARUS_SUB_OCT1": 1, "ICARUS_SUB_OCT2": 2,
        "ICARUS_FILTER_LP12": 0, "ICARUS_FILTER_LP24": 1, "ICARUS_FILTER_HP12": 2, "ICARUS_FILTER_BP12": 3,
        "ICARUS_LFO_SINE": 0, "ICARUS_LFO_TRI": 1, "ICARUS_LFO_SQUARE": 2, "ICARUS_LFO_SAW": 3, "ICARUS_LFO_SH": 4,
        "ICARUS_LFO_DEST_FILTER_CUTOFF": 0, "ICARUS_LFO_DEST_OSC_PITCH": 1, "ICARUS_LFO_DEST_PULSE_WIDTH": 2,
        "ICARUS_LFO_DEST_OSC_LEVEL": 3, "ICARUS_LFO_DEST_STEREO_PAN": 4,
    }
    out = []
    for blk in re.findall(r"\{([^{}]+)\}", body):
        d = {}
        for k, v in re.findall(r"\.(\w+)\s*=\s*([^,\n]+)", blk):
            v = v.strip()
            if v.startswith('"'):
                d[k] = v.strip('"')
            elif v in enums:
                d[k] = enums[v]
            elif v.endswith("f"):
                d[k] = float(v[:-1])
            elif v.endswith("u"):
                d[k] = int(v[:-1])
            else:
                d[k] = int(v)
        mode = d.get("mode", 2)
        if "expression" not in d:
            d["expression"] = 0.80 if mode in (1, 2) else (0.90 if mode == 3 else 0.85)
        if "drift_cents" not in d:
            d["drift_cents"] = 0.0 if mode == 0 else 2.0
        out.append(d)
    return out


def tables():
    t = SRC.read_text(encoding="utf-8")
    f = t[t.index("get_slot_model_name(const pa_ui_ctx_t"):t.index("/* UI Primitives */")]
    cnt = re.search(r"static uint8_t get_slot_param_count.*?\n}\n", t, re.S).group(0)
    counts = dict((k, int(v)) for k, v in re.findall(r"case PA_SLOT_(\w+):\s+return (\d+);", cnt))
    eqn = [int(x) for x in re.search(r"static const uint8_t n\[3\] = \{([^}]*)\}", cnt).group(1).split(",")]
    return {
        "models": {"DYN": array(f, "dyn_models", 1), "SYNTH": synth_names(), "EQ": array(f, "eq_models", 1)},
        "params": {"DYN": array(f, "dyn_params", 4), "SYNTH": array(f, "syn_params", 1), "AMP": array(f, "amp_params", 1), "CAB": array(f, "cab_params", 1),
                   "EQ": [array(f, "eq_bands", 1), array(f, "eq_cut", 1), array(f, "eq_par", 1)]},
        "counts": {"DYN": counts["DYNAMICS"], "SYNTH": counts["SYNTH"], "AMP": counts["AMP"], "CAB": counts["CAB"], "EQ": eqn},
        "synthPresets": synth_presets(),
    }


def block():
    return BEGIN + "\nconst TABLES = " + json.dumps(tables(), separators=(",", ":")) + ";\n" + END


def main():
    page = PAGE.read_text(encoding="utf-8")
    a, b = page.find(BEGIN), page.find(END)
    if a < 0 or b < 0:
        print("web/Icarus_Web_Editor.html has no TABLES markers"); return 1
    new = page[:a] + block() + page[b + len(END):]
    if "--check" in sys.argv:
        if new != page:
            print("web/Icarus_Web_Editor.html: the model and knob tables differ from src/board/ui_app.c (run tools/gen_editor_tables.py)"); return 1
        print("editor tables: in sync with the firmware"); return 0
    PAGE.write_text(new, encoding="utf-8", newline="\n")
    print("editor tables written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
