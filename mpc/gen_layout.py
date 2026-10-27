"""VOLTKIT's parameters (params.json) and touchscreen layout (layout.conf):
the LXR-02's voices as a voice module.

    python3 mpc/gen_layout.py

PANEL: the 2x16 display with the four endless knobs under its four columns
(the MPC's Q-Links turn them, on every page), ALL, and two rows of keys: the
pages (OSC AEG MOD FM CLICK FILTER LFO MIX; a page's key again shows its
second screen), and the seven voices. Each key has its LED above it. MIX has
the six faders and the output level; SETUP the pads.

The keys' button numbers are the firmware's (buttonHandler.h); the adapter
(VoltkitMpc.cpp) presses and lets go. The display cells and LEDs are
display-only knobs that skin/skin_post.py takes the touch from and paints.

Copyright (C) 2026 Jason Rold. GPL-3.0-only.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "VOLTKIT"

THEME = {
    "theme_bg": "121417", "theme_ink": "e8ecef", "theme_ink_dim": "86909a", "theme_accent": "31d0ff",
    "theme_accent_hi": "9be8ff", "theme_seg_active": "31d0ff", "theme_seg_inactive": "1b1e22",
    "theme_seg_active_tx": "0b0b0c", "theme_lcd": "05080a", "theme_knob_dot": "31d0ff", "theme_btn_on": "1a8fc0",
}

# ---- the panel's keys: (button number, caption) by row
VOICE_ROW = [(1, "DRUM 1"), (2, "DRUM 2"), (3, "DRUM 3"), (4, "SNARE"), (5, "CYM/CLP"), (6, "CL HH"), (7, "OP HH")]
SELECT_ROW = [(16, "OSC"), (17, "AEG"), (18, "MOD"), (19, "FM"), (8, "CLICK"), (9, "FILTER"), (10, "LFO"), (11, "MIX")]
PANEL_KEYS = [b for b, _ in VOICE_ROW + SELECT_ROW]

FADERS = ["DRUM 1", "DRUM 2", "DRUM 3", "SNARE", "CYM/CLP", "HIHAT"]

# ---- geometry (plugin area: x 0..1280, y 86..714)
X0, X1 = 24, 1256       # the key rows' extent
LED_R = 6               # a key's LED (a display-only knob), LED_ABOVE px over the key's top
LED_ABOVE = 11

LCD_CELL = 44           # a display cell: a square 128-frame strip, its character 5x8 dots
LCD_X, LCD_ROWS = 64, (146, 194)   # left edge of the first cell; the rows' centres
LCD_BEZEL = (36, 100, 64 + 16 * LCD_CELL + 28, 240)
KNOB_CY, KNOB_R = 300, 40
ALL_CX, ALL_CY, ALL_W, ALL_H = 1040, 262, 330, 132   # ALL, right of the display
TITLE_Y = 136

ROWS = {"select": (SELECT_ROW, 446, 112), "voice": (VOICE_ROW, 622, 112)}
KEY_SPACE = 12          # between two keys of a row

# SETUP
SETUP_KEY_H = 80
PADS_CY, LEARN_CX, CLEAR_CX = 200, 400, 880
PADS_INFO_Y = 300

FADER_CY, FADER_W, FADER_H = 392, 70, 460
FADER_NAME_Y = 134       # the faders' names, painted over them
MASTER_CX = 920


def row_keys(row, height):
    """[(button, caption, cx, w)] for a row spread across X0..X1."""
    pitch = (X1 - X0) / len(row)
    return [(b, cap, round(X0 + pitch * (n + 0.5)), round(pitch - KEY_SPACE)) for n, (b, cap) in enumerate(row)]


def knob_cx(i):
    """Knob i sits under the display's column i (four characters each)."""
    return LCD_X + (4 * i + 2) * LCD_CELL


def btn(b):
    return "btn_%02d" % b


def caption_of(b):
    for row, _, _ in ROWS.values():
        for k in row:
            if k and k[0] == b:
                return k[1]
    return None


def params():
    out = []
    for i in range(4):
        # the parameter's own value, as text (VoltkitMpc.cpp: knobN_display)
        out.append({"key": "knob%d" % (i + 1), "name": "Knob %d" % (i + 1), "min": 0, "max": 1, "default": 0,
                    "dynamic_display": True})
    out.append({"key": "all", "name": "All Voices", "options": ["Off", "On"], "default": 0})
    for b in PANEL_KEYS:
        name = caption_of(b)
        out.append({"key": btn(b), "name": name.title() if name.isupper() else name, "min": 0, "max": 1,
                    "momentary": True, "hold_ms": 120})
    for i, f in enumerate(FADERS):
        out.append({"key": "fader%d" % (i + 1), "name": "%s Fader" % f.title(), "min": 0, "max": 1, "default": 1,
                    "dynamic_display": True})
    out.append({"key": "master", "name": "Master", "min": -24, "max": 6, "default": 0, "dynamic_display": True})
    # set by the engine (it reports them; nothing else does)
    for b in PANEL_KEYS:
        out.append({"key": "led_%02d" % b, "name": "LED %d" % b, "min": 0, "max": 1, "default": 0, "display": "int"})
    for c in range(32):
        # 0..126: MPC shows frame round(value * 126) of the 128-frame strip, so
        # character n reads n + 0.3 (VoltkitMpc.cpp), inside frame n however
        # it rounds; 127 is never printable.
        out.append({"key": "lcd_%02d" % c, "name": "LCD %d" % (c + 1), "min": 0, "max": 126, "default": 32,
                    "display": "int"})
    # the pads (SETUP)
    out.append({"key": "learn", "name": "Pad Learn", "options": ["Off", "On"], "default": 0})
    out.append({"key": "pads_clear", "name": "Clear Pads", "min": 0, "max": 1, "momentary": True, "hold_ms": 120})
    out.append({"key": "pads_info", "name": "Pads", "min": 0, "max": 1, "display": "string", "type": "readout"})
    sections = [
        {"label": "Panel", "keys": ["knob1", "knob2", "knob3", "knob4", "all"] + [btn(b) for b in PANEL_KEYS]},
        {"label": "Mix", "keys": ["fader%d" % (i + 1) for i in range(6)] + ["master"]},
        {"label": "Setup", "keys": ["learn", "pads_clear", "pads_info"]},
    ]
    return {"name": NAME, "params": out, "sections": sections}


def layout():
    L = []
    w = L.append
    w("# %s for MPC OS touchscreens (MPC Live III and other 1280x800 units)." % NAME)
    w("# Plugin area: x 0..1280, y 86..714 (tools/shadow_skin.py). Written by")
    w("# gen_layout.py; skin/skin_post.py paints the panel round these controls.")
    for k, v in THEME.items():
        w("%s=%s" % (k, v))
    w("qlinks_track = knob1,knob2,knob3,knob4")
    w("")
    w("[tab PANEL]")
    w("# The four endless knobs, under the display's columns. First: a page's")
    w("# Q-Links go to its first controls in this file (and skin_post.py keeps")
    w("# every page's Q-Links on these four).")
    for i in range(4):
        w("knob    cx=%d cy=%d r=%d label=\"\" key=knob%d bw=%d ns=0 vs=0" % (knob_cx(i), KNOB_CY, KNOB_R, i + 1, 2 * KNOB_R + 20))
    w("# The display: a display-only knob per character cell (skin_post.py).")
    for r, cy in enumerate(LCD_ROWS):
        for c in range(16):
            w("knob    cx=%d cy=%d r=%d label=\"\" key=lcd_%02d bw=%d ns=0 vs=0"
              % (LCD_X + c * LCD_CELL + LCD_CELL // 2, cy, LCD_CELL // 2 - 5, r * 16 + c, LCD_CELL))
    w("toggle  cx=%d cy=%d label=\"ALL\" key=all bw=%d" % (ALL_CX, ALL_CY, ALL_W))
    w("# The keys, row by row, each with its LED above.")
    for name, (row, cy, h) in ROWS.items():
        for b, cap, cx, kw in row_keys(row, h):
            w("button  cx=%d cy=%d w=%d h=%d label=\"%s\" key=%s" % (cx, cy, kw, h, cap, btn(b)))
            w("knob    cx=%d cy=%d r=%d label=\"\" key=led_%02d bw=%d ns=0 vs=0"
              % (cx, cy - h // 2 - LED_ABOVE, LED_R, b, 2 * LED_R + 10))
    w("")
    w("[tab MIX]")
    w("# A fader per voice group and the output level.")
    pitch = 128
    for i in range(6):
        w("slider_v cx=%d cy=%d w=%d h=%d label=\"\" key=fader%d bw=120 ns=0 vs=24"
          % (110 + pitch * i, FADER_CY, FADER_W, FADER_H, i + 1))
    w("slider_v cx=%d cy=%d w=%d h=%d label=\"\" key=master bw=140 ns=0 vs=24" % (MASTER_CX, FADER_CY, FADER_W, FADER_H))
    w("")
    w("[tab SETUP]")
    w("# The pads play the seven voices: General MIDI drum notes, or the pads PAD")
    w("# LEARN learned; CLEAR PADS forgets them.")
    w("toggle  cx=%d cy=%d label=\"PAD LEARN\" key=learn bw=%d" % (LEARN_CX, PADS_CY, BIG_KEYS["learn"][1]))
    w("button  cx=%d cy=%d w=%d h=%d label=\"CLEAR PADS\" key=pads_clear" % (CLEAR_CX, PADS_CY, 280, SETUP_KEY_H))
    w("readout cx=640 cy=%d w=1180 h=46 label=\"\" key=pads_info tsize=19 tcolor=9be8ff" % PADS_INFO_Y)
    return "\n".join(L) + "\n"


# skin_post.py's on/off keys: toggle key -> (caption, w, h)
BIG_KEYS = {"all": ("ALL", ALL_W, ALL_H), "learn": ("PAD LEARN", 300, 80)}


if __name__ == "__main__":
    json.dump(params(), open(os.path.join(HERE, "params.json"), "w"), indent=1)
    open(os.path.join(HERE, "layout.conf"), "w").write(layout())
