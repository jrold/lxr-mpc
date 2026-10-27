"""VOLTKIT's controls, in one place.

    python3 mpc/gen_controls.py

writes mpc/params.json (the plug-in's parameters), mpc/layout.conf (where they
sit on the touchscreen) and mpc/Controls.inc (the adapter's table: which LXR
firmware parameter each one sets, and how). Run it after changing anything
here; build.sh runs it too.

The pages follow the LXR's own menu (firmware/Core/Inc/menu/menuPages.h):
each voice has its oscillator, amp envelope, modulation, FM or metal
oscillators, transient ("click"), filter, LFO and mix pages. The parameter
numbers, the init kit's values (the defaults) and the modulation targets are
read from the firmware sources, so they can't drift from the engine.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, "..", "firmware", "Core")


# ------------------------------------------------------------ firmware facts

def parse_param_enum():
    """Parameters.h's ParamEnums: name -> number."""
    src = open(os.path.join(FW, "Inc", "Parameters.h")).read()
    body = src[src.index("enum ParamEnums"):]
    body = body[body.index("{") + 1:body.index("};")]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    out, n = {}, -1
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            name, val = (s.strip() for s in item.split("=", 1))
            n = out[val] if val in out else int(val, 0)
        else:
            name = item
            n += 1
        out[name] = n
    return out


def parse_init_kit():
    """initKit.c: the 8-byte name, then one byte per parameter."""
    src = open(os.path.join(FW, "Src", "Preset", "initKit.c")).read()
    data = src[src.index("{", src.index("initKit[")) + 1:src.index("};")]
    return [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", data)]


def parse_mod_targets():
    """Cc2Text.c's modTargets: [(TEXT_..., PAR_...)] and each voice's range."""
    src = open(os.path.join(FW, "Src", "menu", "Cc2Text.c")).read()
    table = src[src.index("const ModTarg modTargets[]"):]
    table = table[:table.index("};")]
    table = re.sub(r"//[^\n]*", "", table)
    targets = re.findall(r"\{\s*(TEXT_\w+)\s*,\s*(PAR_\w+)\s*\}", table)
    offs = src[src.index("modTargetVoiceOffsets[6]"):]
    offs = re.sub(r"/\*.*?\*/", "", offs[:offs.index("};")], flags=re.S)
    ranges = [(int(a), int(b)) for a, b in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*\}", offs)]
    return targets, ranges


def parse_limits():
    """Each sound parameter's range, as the LXR's menu clamps it (menu.c
    parameter_dtypes and the option lists in MenuText.h): [(min, max)]."""
    text = open(os.path.join(FW, "Inc", "menu", "MenuText.h")).read()
    counts = {m.group(1): int(m.group(2)) for m in
              re.finditer(r"const char (\w+)\[\]\[\d+\]\s*=\s*\{\s*\{(\d+)\}", text)}
    menu = open(os.path.join(FW, "Src", "menu", "menu.c")).read()
    sw = menu[menu.index("static uint8_t getMaxEntriesForMenu(uint8_t menuId)\n{"):]
    sw = sw[:sw.index("\n}\n")]
    menus = dict(re.findall(r"case (MENU_\w+):\s*return \(?(?:uint8_t\))?\s*\(?(\w+)\[0\]\[0\]", sw))
    table = menu[menu.index("parameter_dtypes[NUM_PARAMS] = {"):]
    table = re.sub(r"//[^\n]*", "", table[table.index("{") + 1:table.index("};")])
    table = re.sub(r"/\*.*?\*/", "", table, flags=re.S)
    entries = [e.strip() for e in table.split(",") if e.strip()]
    plain = {"DTYPE_0B127": (0, 127), "DTYPE_PM63": (0, 127), "DTYPE_0B100": (0, 100), "DTYPE_0B255": (0, 255),
             "DTYPE_ON_OFF": (0, 1), "DTYPE_0b1": (0, 1), "DTYPE_1B16": (1, 16), "DTYPE_0B15": (0, 15),
             "DTYPE_1B64": (1, 64), "DTYPE_NOTE_NAME": (0, 127), "DTYPE_VOICE_LFO": (1, 6),
             # indices into modTargets: the engine checks them against the table
             "DTYPE_TARGET_SELECTION_LFO": (0, 255), "DTYPE_TARGET_SELECTION_VELO": (0, 255),
             "DTYPE_AUTOM_TARGET": (0, 255)}
    out = []
    for e in entries:
        m = re.match(r"DTYPE_MENU\s*\|\s*\((MENU_\w+)\s*<<\s*8\)", e)
        if m:
            out.append((0, counts[menus[m.group(1)]] - 1))
        else:
            out.append(plain[e])
    return out, {k: counts[v] for k, v in menus.items()}


PAR = parse_param_enum()
LIMITS, MENU_COUNTS = parse_limits()
KIT = parse_init_kit()
TARGETS, TARGET_RANGES = parse_mod_targets()
END = PAR["END_OF_SOUND_PARAMETERS"]
assert END == 247, END
assert len(TARGET_RANGES) == 6


def kit_value(par):
    i = 8 + PAR[par]
    return KIT[i] if i < len(KIT) else 0


# ------------------------------------------------------------ option lists

# The firmware's menu lists (MenuText.h), spelled out.
OPTIONS = {
    "wave": ["Sine", "Triangle", "Saw", "Square", "Noise", "PWM"],
    "fmmode": ["FM", "Mix"],
    "trans": ["Snap", "Offset", "Click", "Click 2", "Tick", "Kick", "Rim", "Drop", "Hat", "Clap",
              "Kick 2", "Snare", "Tom", "Snap 2"],
    "filter": ["Low Pass", "High Pass", "Band Pass", "Unity BP", "Notch", "Peak", "Low Pass 2", "Off"],
    "sync": ["Free", "4/1", "2/1", "1/1", "1/2", "1/3", "1/4", "1/6", "1/8", "1/12", "1/16", "1/32"],
    "lfowave": ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random", "Exp Up", "Exp Down"],
    "retrig": ["Off", "Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "Hats"],
    "lfovoice": ["Drum 1", "Drum 2", "Drum 3", "Snare", "Cymbal", "Hats"],
    "out": ["Main", "FX"],
    "fx": ["Off", "Drive", "Ring", "Comp", "Delay"],
    "dist": ["Tube", "Fold", "Clip"],
    "ratio": ["1:1", "2:1", "3:1", "4:1", "6:1", "8:1"],
    "delay": ["Delay", "Ping Pong"],
    "onoff": ["Off", "On"],
}

# Our spelled-out lists must have the firmware's number of entries.
for _name, _menu in (("wave", "MENU_WAVEFORM"), ("trans", "MENU_TRANS"), ("filter", "MENU_FILTER"),
                     ("sync", "MENU_SYNC_RATES"), ("lfowave", "MENU_LFO_WAVES"), ("retrig", "MENU_RETRIGGER"),
                     ("fmmode", "MENU_MIX_FM"), ("fx", "MENU_FX"), ("dist", "MENU_DIST"), ("ratio", "MENU_RATIO"),
                     ("delay", "MENU_DELAY")):
    assert len(OPTIONS[_name]) == MENU_COUNTS[_menu], (_name, len(OPTIONS[_name]), MENU_COUNTS[_menu])

# A modulation target's name: what our own controls call that parameter.
PAGE_ABBREV = {"OSC": "Osc", "AMP": "Amp", "MOD": "Mod", "FM": "FM", "METAL": "Metal", "CLICK": "Click",
               "FILTER": "Flt", "LFO": "LFO", "MIX": "Mix"}


# ------------------------------------------------------------ the voices

class C:
    """One control: key suffix, name, LXR parameter, kind, option list."""

    def __init__(self, suffix, name, par, kind="knob", opts=None, qlink=False):
        self.suffix, self.name, self.par, self.kind, self.opts, self.qlink = suffix, name, par, kind, opts, qlink


def osc(n, extra=()):
    return [C("coarse", "Coarse", "PAR_COARSE%d" % n, "pm60", qlink=True),
            C("fine", "Fine", "PAR_FINE%d" % n, "pm63")] + list(extra)


def mod_page(n, pitch):
    out = []
    if pitch:
        out += [C("pdecay", "Pitch Decay", "PAR_MOD_EG%d" % n, qlink=True),
                C("pslope", "Pitch Slope", "PAR_PITCH_SLOPE%d" % n),
                C("pamount", "Pitch Amount", "PAR_MODAMNT%d" % n, qlink=True)]
    return out + [C("veldest", "Velo Dest", "PAR_VEL_DEST_%d" % n, "veldest"),
                  C("velamt", "Velo Amount", "PAR_VELO_MOD_AMT_%d" % n),
                  C("velvol", "Velo Volume", "PAR_VOLUME_MOD_ON_OFF%d" % n, "menu", "onoff")]


def click(n):
    return [C("twave", "Click Wave", "PAR_TRANS%d_WAVE" % n, "menu", "trans"),
            C("tlevel", "Click Level", "PAR_TRANS%d_VOL" % n, qlink=True),
            C("tfreq", "Click Pitch", "PAR_TRANS%d_FREQ" % n)]


def filt(n):
    return [C("cutoff", "Cutoff", "PAR_FILTER_FREQ_%d" % n, qlink=True),
            C("reso", "Resonance", "PAR_RESO_%d" % n, qlink=True),
            C("ftype", "Filter Type", "PAR_FILTER_TYPE_%d" % n, "menu", "filter"),
            C("fdrive", "Filter Drive", "PAR_FILTER_DRIVE_%d" % n)]


def lfo(n):
    return [C("lrate", "LFO Rate", "PAR_FREQ_LFO%d" % n, qlink=True),
            C("lsync", "LFO Sync", "PAR_SYNC_LFO%d" % n, "menu", "sync"),
            C("lamt", "LFO Amount", "PAR_AMOUNT_LFO%d" % n, qlink=True),
            C("lwave", "LFO Wave", "PAR_WAVE_LFO%d" % n, "menu", "lfowave"),
            C("lretrig", "LFO Retrigger", "PAR_RETRIGGER_LFO%d" % n, "menu", "retrig"),
            C("loffset", "LFO Phase", "PAR_OFFSET_LFO%d" % n),
            C("lvoice", "LFO Voice", "PAR_VOICE_LFO%d" % n, "lfovoice", "lfovoice"),
            C("ldest", "LFO Dest", "PAR_TARGET_LFO%d" % n, "lfodest")]


def mix(n, drive):
    return [C("volume", "Volume", "PAR_VOL%d" % n, qlink=True),
            C("pan", "Pan", "PAR_PAN%d" % n, "pm63"),
            C("crush", "Sample Rate", "PAR_VOICE_DECIMATION%d" % n),
            C("drive", "Drive", drive, qlink=True),
            C("out", "Output", "PAR_AUDIO_OUT%d" % n, "out", "out")]


def drum(n):
    return [
        ("OSC", "OSCILLATOR", osc(n, [C("wave", "Wave", "PAR_OSC_WAVE_DRUM%d" % n, "menu", "wave"),
                                      C("pwm", "PWM", "PAR_PWM_%d" % n)])),
        ("AMP", "AMP ENVELOPE", [C("attack", "Attack", "PAR_VELOA%d" % n),
                                 C("decay", "Decay", "PAR_VELOD%d" % n, qlink=True),
                                 C("slope", "Slope", "PAR_VOL_SLOPE%d" % n)]),
        ("MOD", "PITCH AND VELOCITY", mod_page(n, True)),
        ("FM", "FM / MIX OSCILLATOR", [C("fmamt", "FM Amount", "PAR_FMAMNT%d" % n, qlink=True),
                                       C("fmfreq", "FM Pitch", "PAR_FM_FREQ%d" % n, "pm60"),
                                       C("fmwave", "FM Wave", "PAR_MOD_WAVE_DRUM%d" % n, "menu", "wave"),
                                       C("fmmode", "FM Mode", "PAR_MIX_MOD_%d" % n, "menu", "fmmode")]),
        ("CLICK", "TRANSIENT", click(n)),
        ("FILTER", "FILTER", filt(n)),
        ("LFO", "LFO", lfo(n)),
        ("MIX", "MIX", mix(n, "PAR_DRIVE%d" % n)),
    ]


def snare():
    return [
        ("OSC", "OSCILLATOR AND NOISE", osc(4, [C("noise", "Noise Freq", "PAR_NOISE_FREQ1", qlink=True),
                                                C("nmix", "Noise Mix", "PAR_MIX1", qlink=True),
                                                C("wave", "Wave", "PAR_OSC_WAVE_SNARE", "menu", "wave"),
                                                C("pwm", "PWM", "PAR_PWM_4")])),
        ("AMP", "AMP ENVELOPE", [C("attack", "Attack", "PAR_VELOA4"),
                                 C("decay", "Decay", "PAR_VELOD4", qlink=True),
                                 C("repeat", "Repeat", "PAR_REPEAT4"),
                                 C("slope", "Slope", "PAR_VOL_SLOPE4")]),
        ("MOD", "PITCH AND VELOCITY", mod_page(4, True)),
        ("CLICK", "TRANSIENT", click(4)),
        ("FILTER", "FILTER", filt(4)),
        ("LFO", "LFO", lfo(4)),
        ("MIX", "MIX", mix(4, "PAR_SNARE_DISTORTION")),
    ]


def metal(n, cym):
    sfx = "_CYM" if cym else ""
    amp = [C("attack", "Attack", "PAR_VELOA%d" % n)]
    if cym:
        amp += [C("decay", "Decay", "PAR_VELOD5", qlink=True), C("repeat", "Repeat", "PAR_REPEAT5")]
    else:
        amp += [C("cdecay", "Closed Decay", "PAR_VELOD6_CLOSED", qlink=True),
                C("odecay", "Open Decay", "PAR_VELOD6_OPEN", qlink=True)]
    amp += [C("slope", "Slope", "PAR_VOL_SLOPE%d" % n)]
    w = "CYM" if cym else "HH"
    return [
        ("OSC", "OSCILLATOR", osc(n, [C("wave", "Wave", "PAR_WAVE1_%s" % w, "menu", "wave"),
                                      C("pwm", "PWM", "PAR_PWM_%d" % n)])),
        ("AMP", "AMP ENVELOPE", amp),
        ("MOD", "VELOCITY", mod_page(n, False)),
        ("METAL", "MODULATOR OSCILLATORS", [C("m1pitch", "Mod 1 Pitch", "PAR_MOD_OSC_F1%s" % sfx, "pm60", qlink=True),
                                            C("m1gain", "Mod 1 Gain", "PAR_MOD_OSC_GAIN1%s" % sfx, qlink=True),
                                            C("m1wave", "Mod 1 Wave", "PAR_WAVE2_%s" % w, "menu", "wave"),
                                            C("m2pitch", "Mod 2 Pitch", "PAR_MOD_OSC_F2%s" % sfx, "pm60"),
                                            C("m2gain", "Mod 2 Gain", "PAR_MOD_OSC_GAIN2%s" % sfx),
                                            C("m2wave", "Mod 2 Wave", "PAR_WAVE3_%s" % w, "menu", "wave")]),
        ("CLICK", "TRANSIENT", click(n)),
        ("FILTER", "FILTER", filt(n)),
        ("LFO", "LFO", lfo(n)),
        ("MIX", "MIX", mix(n, "PAR_CYMBAL_DISTORTION" if cym else "PAR_HAT_DISTORTION")),
    ]


# key prefix, tab name, firmware voice (1..6), pages
VOICES = [
    ("d1", "DRUM 1", 1, drum(1)),
    ("d2", "DRUM 2", 2, drum(2)),
    ("d3", "DRUM 3", 3, drum(3)),
    ("sn", "SNARE", 4, snare()),
    ("cy", "CYMBAL", 5, metal(5, True)),
    ("hh", "HATS", 6, metal(6, False)),
]
VOICE_SHORT = {"d1": "D1", "d2": "D2", "d3": "D3", "sn": "SN", "cy": "CY", "hh": "HH"}

# The FX bus: P1..P4 mean something different for every effect, so each
# effect keeps its own four (the adapter sends the active one's).
FX_TYPES = [
    ("Drive", [("drv_drive", "Drive"), ("drv_shape", "Shape"), ("drv_tone", "Tone"), ("drv_level", "Level")],
     C("fx_dist", "Drive Type", "PAR_FX_DIST_TYPE", "menu", "dist")),
    ("Ring", [("ring_freq", "Ring Freq")], C("fx_ringwave", "Ring Wave", "PAR_FX_WAVE", "menu", "wave")),
    ("Comp", [("cmp_attack", "Attack"), ("cmp_release", "Release"), ("cmp_thresh", "Threshold"), ("cmp_gain", "Makeup")],
     C("fx_ratio", "Ratio", "PAR_FX_RATIO", "menu", "ratio")),
    ("Delay", [("dly_time", "Time"), ("dly_range", "Range"), ("dly_tone", "Tone"), ("dly_feedback", "Feedback")],
     C("fx_delaytype", "Delay Type", "PAR_FX_DELAY_TYPE", "menu", "delay")),
]


# ------------------------------------------------------------ helpers

def target_names(voice):
    """The names of voice's modulation targets (1..6), in modTargets order."""
    by_par = {}
    for _, _, _, pages in VOICES:
        for page, _, controls in pages:
            for c in controls:
                # one-word names get their page, so "Decay" reads "Amp Decay"
                by_par.setdefault(c.par, c.name if " " in c.name else "%s %s" % (PAGE_ABBREV[page], c.name))
    fx = {"PAR_FX_P%d" % i: "FX P%d" % i for i in range(1, 5)}
    lo, hi = TARGET_RANGES[voice - 1]
    names = []
    for text, par in TARGETS[lo:hi + 1]:
        names.append(fx.get(par) or by_par.get(par) or text[5:].replace("_", " ").title())
    return names


def default_of(c, voice):
    v = kit_value(c.par)
    if c.kind == "veldest":
        lo, hi = TARGET_RANGES[voice - 1]
        return v - lo + 1 if lo <= v <= hi else 0
    if c.kind == "lfodest":
        return 0   # the adapter derives it from the kit and the LFO's voice
    if c.kind == "lfovoice":
        return min(max(v, 1), 6) - 1
    if c.kind == "out":
        return 1 if v == 6 else 0
    if c.kind == "menu":
        return min(v, len(OPTIONS[c.opts]) - 1)
    return v


def param_entry(key, c, voice, short):
    name = "%s %s" % (short, c.name)
    d = default_of(c, voice)
    if c.kind in ("menu", "out", "lfovoice"):
        return {"key": key, "name": name, "options": OPTIONS[c.opts], "default": d}
    if c.kind == "veldest":
        return {"key": key, "name": name, "options": ["Off"] + target_names(voice), "default": d}
    if c.kind == "lfodest":
        # 0 = off, then the LFO voice's targets; the text comes from the adapter.
        return {"key": key, "name": name, "min": 0, "max": 40, "default": d, "display": "int",
                "dynamic_display": True, "nudge_pct": 2.5, "type": "stepper"}
    return {"key": key, "name": name, "min": 0, "max": 127, "default": d, "display": "int",
            "dynamic_display": True, "nudge_pct": 100 / 127}


# ------------------------------------------------------------ layout

COLS = [170, 490, 810, 1130]   # four columns of controls
ROWS = [330, 560]
PAGE_Y = 140                  # the page keys

THEME = """theme_bg=0b0b0c
theme_ink=e8ecef
theme_ink_dim=86909a
theme_accent=31d0ff
theme_accent_hi=9be8ff
theme_seg_active=31d0ff
theme_seg_inactive=1b1e22
theme_seg_active_tx=0b0b0c
theme_lcd=05080a
theme_knob_dot=31d0ff
theme_btn_on=1a8fc0"""


def control_line(key, c, cx, cy, when):
    w = " when=%s" % when if when else ""
    label = c.name.upper()
    if c.kind in ("menu", "out", "lfovoice", "veldest"):
        n = len(OPTIONS.get(c.opts, [])) if c.kind != "veldest" else 99
        if n <= 2:
            return "enum_h  cx=%d cy=%d key=%s label=\"%s\" sw=120 sh=56%s" % (cx, cy, key, label, w)
        return "popup   cx=%d cy=%d w=240 h=64 key=%s label=\"%s\"%s" % (cx, cy, key, label, w)
    if c.kind == "lfodest":
        return "stepper cx=%d cy=%d w=256 h=60 label=\"%s\" key=%s prev=%s_prev next=%s_next tsize=19%s" % (
            cx, cy, label, key, key, key, w)
    return "knob    cx=%d cy=%d r=54 key=%s bw=150 ns=20 vs=26%s" % (cx, cy, key, w)


def main():
    params, layout, table = [], [], []
    layout += ["# VOLTKIT for MPC OS touchscreens (1280x800). Generated by mpc/gen_controls.py:",
               "# edit that, not this. skin/skin_post.py repaints the artwork.", THEME]
    qtrack = []

    for prefix, tab, voice, pages in VOICES:
        short = VOICE_SHORT[prefix]
        page_key = prefix + "_page"
        page_names = [p for p, _, _ in pages]
        params.append({"key": page_key, "name": "%s Page" % short, "options": page_names, "default": 0})
        table.append('{"%s", -1, Kind::page, %d},' % (page_key, voice))
        layout += ["", "[tab %s]" % tab,
                   "enum_h  cx=640 cy=%d key=%s sw=%d sh=60" % (PAGE_Y, page_key, min(150, 1240 // len(page_names) - 4))]
        qlinks = []
        for page, title, controls in pages:
            when = "%s:%s" % (page_key, page)
            layout.append("frame   x=16 y=188 w=1248 h=520 title=\"%s\" when=%s" % (title, when))
            per_row = 4
            for i, c in enumerate(controls):
                key = "%s_%s" % (prefix, c.suffix)
                params.append(param_entry(key, c, voice, short))
                table.append('{"%s", %d, Kind::%s, %d},' % (key, PAR[c.par], c.kind, voice))
                row, col = divmod(i, per_row)
                if len(controls) <= 4:
                    row = 0
                cy = ROWS[min(row, 1)] if len(controls) > 4 else 440
                layout.append(control_line(key, c, COLS[col], cy, when))
                if c.kind == "lfodest":
                    for side, delta in (("prev", -1), ("next", 1)):
                        params.append({"key": "%s_%s" % (key, side), "name": "%s %s" % (short, side.title()),
                                       "min": 0, "max": 1, "momentary": True, "step_of": key, "step_delta": delta})
                if c.qlink and c.kind in ("knob", "pm60", "pm63"):
                    qlinks.append(key)
        layout.append("qlinks \"%s\" = %s" % (tab, ",".join(qlinks[:16])))
        qtrack.append(qlinks[:2])

    # FX
    params.append({"key": "fx_type", "name": "FX Type", "options": OPTIONS["fx"], "default": kit_value("PAR_FX_TYPE")})
    table.append('{"fx_type", %d, Kind::menu, 0},' % PAR["PAR_FX_TYPE"])
    params.append({"key": "fx_mix", "name": "FX Mix", "min": 0, "max": 100, "default": min(kit_value("PAR_FX_DRY_WET"), 100),
                   "display": "int", "dynamic_display": True, "nudge_pct": 1})
    table.append('{"fx_mix", %d, Kind::knob, 0},' % PAR["PAR_FX_DRY_WET"])
    params.append({"key": "crush_all", "name": "Sample Rate All", "min": 0, "max": 127,
                   "default": kit_value("PAR_VOICE_DECIMATION_ALL"), "display": "int", "dynamic_display": True,
                   "nudge_pct": 100 / 127})
    table.append('{"crush_all", %d, Kind::knob, 0},' % PAR["PAR_VOICE_DECIMATION_ALL"])
    params.append({"key": "volume", "name": "Volume", "min": -24, "max": 6, "default": 0, "dynamic_display": True})
    table.append('{"volume", -1, Kind::master, 0},')

    layout += ["", "[tab FX]",
               "enum_h  cx=640 cy=%d key=fx_type sw=150 sh=60" % PAGE_Y,
               "frame   x=16 y=188 w=300 h=520 title=\"MASTER\"",
               "knob    cx=166 cy=330 r=54 key=volume bw=150 ns=20 vs=26",
               "knob    cx=166 cy=560 r=54 key=crush_all bw=150 ns=20 vs=26",
               "frame   x=332 y=188 w=932 h=520 title=\"FX BUS\"",
               "knob    cx=462 cy=440 r=54 key=fx_mix bw=150 ns=20 vs=26"]
    fx_q = ["fx_mix", "volume", "crush_all"]
    fx_cols = [480, 740, 1000]
    for i, (fx, knobs, sel) in enumerate(FX_TYPES):
        when = "fx_type:%s" % fx
        # each effect's own panel (it also carries the effect's knob artwork)
        layout.append("frame   x=592 y=240 w=656 h=456 title=\"%s\" when=%s" % (fx.upper(), when))
        params.append(param_entry(sel.suffix, sel, 0, "FX"))
        table.append('{"%s", %d, Kind::menu, 0},' % (sel.suffix, PAR[sel.par]))
        layout.append(control_line(sel.suffix, sel, 920, 360, when))
        for j, (suffix, name) in enumerate(knobs):
            key = "fx_" + suffix
            p = PAR["PAR_FX_P%d" % (j + 1)]
            params.append({"key": key, "name": "FX %s" % name, "min": 0, "max": 127, "default": kit_value("PAR_FX_P%d" % (j + 1)),
                           "display": "int", "dynamic_display": True, "nudge_pct": 100 / 127})
            table.append('{"%s", %d, Kind::fxp, %d},' % (key, p, i + 1))
            x = [676, 838, 1000, 1162][j]
            layout.append("knob    cx=%d cy=540 r=54 key=%s bw=150 ns=20 vs=26 when=%s" % (x, key, when))
            fx_q.append(key)
    layout.append("qlinks \"FX\" = %s" % ",".join(fx_q[:16]))

    # SETUP
    params += [
        {"key": "learn", "name": "Pad Learn", "options": ["Off", "On"], "default": 0},
        {"key": "pads_clear", "name": "Clear Pads", "min": 0, "max": 1, "momentary": True, "hold_ms": 120},
        {"key": "init_kit", "name": "Init Kit", "min": 0, "max": 1, "momentary": True, "hold_ms": 120},
        {"key": "pads_info", "name": "Pads", "min": 0, "max": 1, "display": "string", "type": "readout"},
    ]
    table += ['{"learn", -1, Kind::learn, 0},', '{"pads_clear", -1, Kind::padsClear, 0},',
              '{"init_kit", -1, Kind::initKit, 0},', '{"pads_info", -1, Kind::padsInfo, 0},']
    layout += ["", "[tab SETUP]",
               "# PAD LEARN: tap it, then hit the pads for DRUM 1, DRUM 2, DRUM 3, SNARE, CYMBAL,",
               "# CLOSED HAT and OPEN HAT in that order; it finishes by itself after the seventh.",
               "frame   x=16 y=96 w=1248 h=612 title=\"PADS AND KIT\"",
               "toggle  cx=360 cy=300 label=\"PAD LEARN\" key=learn bw=400",
               "button  cx=920 cy=300 w=400 h=90 label=\"CLEAR PADS\" key=pads_clear",
               "readout cx=640 cy=430 w=1160 h=52 label=\"\" key=pads_info tsize=22 tcolor=31d0ff",
               "button  cx=640 cy=580 w=400 h=90 label=\"INIT KIT\" key=init_kit"]

    layout.insert(3, "qlinks_track = " + ",".join(k for pair in qtrack[:4] for k in pair))

    keys = [p["key"] for p in params]
    dupes = {k for k in keys if keys.count(k) > 1}
    if dupes:
        sys.exit("gen_controls: duplicate keys %s" % sorted(dupes))

    json.dump({"name": "VOLTKIT", "params": params}, open(os.path.join(HERE, "params.json"), "w"), indent=1)
    open(os.path.join(HERE, "layout.conf"), "w").write("\n".join(layout) + "\n")
    with open(os.path.join(HERE, "Controls.inc"), "w") as f:
        f.write("// Generated by mpc/gen_controls.py: each plug-in parameter, the firmware\n"
                "// parameter it sets (-1: none), how, and its voice (1..6; FX P1..P4: the effect).\n")
        f.write("\n".join(table) + "\n")
    with open(os.path.join(HERE, "..", "engine", "param_limits.h"), "w") as f:
        f.write("// Generated by mpc/gen_controls.py from the firmware's menu (menu.c\n"
                "// parameter_dtypes, MenuText.h): each sound parameter's range, as the\n"
                "// LXR's menu clamps it before the engine sees it.\n#pragma once\n#include <stdint.h>\n\n")
        lims = LIMITS[:END]
        f.write("static const uint8_t voltParamMin[%d] = {%s};\n" % (END, ", ".join(str(a) for a, _ in lims)))
        f.write("static const uint8_t voltParamMax[%d] = {%s};\n" % (END, ", ".join(str(b) for _, b in lims)))
    with open(os.path.join(HERE, "FirmwareParams.h"), "w") as f:
        f.write("// Generated by mpc/gen_controls.py from firmware/Core/Inc/Parameters.h.\n#pragma once\n\n")
        for name in ("PAR_FX_TYPE", "PAR_FX_DRY_WET", "PAR_FX_P1", "PAR_VOICE_LFO1", "PAR_TARGET_LFO1"):
            f.write("constexpr int k%s_ = %d;\n" % ("".join(w.title() for w in name.split("_")), PAR[name]))
    with open(os.path.join(HERE, "Targets.inc"), "w") as f:
        f.write("// Generated by mpc/gen_controls.py: each voice's modulation target names,\n"
                "// in the firmware's modTargets order (Cc2Text.c), and that voice's range.\n")
        for v in range(1, 7):
            lo, hi = TARGET_RANGES[v - 1]
            names = ", ".join('"%s"' % n for n in target_names(v))
            f.write("{%d, %d, {%s}},\n" % (lo, hi, names))
    print("gen_controls: %d parameters, %d layout lines" % (len(params), len(layout)))


if __name__ == "__main__":
    main()
