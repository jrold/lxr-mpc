"""VOLTKIT's parameters (params.json) and touchscreen layout (layout.conf):
the LXR-02's front panel.

    python3 mpc/gen_layout.py

PANEL is the machine: the 2x16 display with the four endless knobs under its
four columns, the encoder with its push, SHIFT and HOLD; then the keys, as on
the hardware: the voice row, the page-select and mode row, and the 16 steps.
Each key has its LED above it. MIX has the six faders, the output level and
the MPC sync switch.

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

# ---- the panel's keys: (button number, caption) by row; None leaves a gap
VOICE_ROW = [(40, "◀"), (0, "▶"), None,
             (1, "DRUM 1"), (2, "DRUM 2"), (3, "DRUM 3"), (4, "SNARE"), (5, "CYM/CLP"), (6, "CL HH"), (7, "OP HH"),
             None, (34, "REC"), (33, "PLAY"), (35, "COPY")]
SELECT_ROW = [(16, "OSC"), (17, "AEG"), (18, "MOD"), (19, "FM"), (8, "CLICK"), (9, "FILTER"), (10, "LFO"), (11, "MIX"),
              None, (24, "VOICE"), (25, "PERF"), (26, "LOAD"), (27, "SAVE")]
# SEQ1..SEQ16, with what SHIFT makes of each (painted under the keys)
STEP_BUTTONS = [39, 38, 37, 36, 31, 30, 29, 28, 23, 22, 21, 20, 15, 14, 13, 12]
STEP_LABELS = ["DRUM1", "DRUM2", "DRUM3", "SNARE", "CLP/CYM", "CL HH", "OP HH", "KIT",
               "MORPH", "SONG", "PROJECT", "CONFIG", "RELOAD", "LAST STP", "FX", "PATTERN"]
STEP_ROW = []
for i, b in enumerate(STEP_BUTTONS):
    if i and i % 4 == 0:
        STEP_ROW.append(None)
    STEP_ROW.append((b, str(i + 1)))

FADERS = ["DRUM 1", "DRUM 2", "DRUM 3", "SNARE", "CYM/CLP", "HIHAT"]

# ---- geometry (plugin area: x 0..1280, y 86..714)
X0, X1 = 24, 1256       # the key rows' extent
GAP = 26                # the space a None leaves
LED_R = 6               # a key's LED (a display-only knob), LED_ABOVE px over the key's top
LED_ABOVE = 11

LCD_CELL = 44           # a display cell: a square 128-frame strip, its character 5x8 dots
LCD_X, LCD_ROWS = 64, (146, 194)   # left edge of the first cell; the rows' centres
LCD_BEZEL = (36, 100, 64 + 16 * LCD_CELL + 28, 240)
KNOB_CY, KNOB_R = 300, 40
ENC_CX, ENC_CY, ENC_R = 925, 190, 56
SIDE_CX, SIDE_W, SIDE_H = 1150, 170, 62
SIDE_KEYS = [("enc_push", "PUSH", 140), ("shift", "SHIFT", 222), ("hold", "HOLD", 304)]

ROWS = {"voice": (VOICE_ROW, 412, 60), "select": (SELECT_ROW, 508, 60), "steps": (STEP_ROW, 606, 56)}
STEP_LABEL_Y = 652      # the SHIFT captions under the steps

FADER_CY, FADER_W, FADER_H = 392, 70, 460
FADER_NAME_Y = 134       # the faders' names, painted over them
MASTER_CX = 920


def row_keys(row, height):
    """[(button, caption, cx, w)] for a row spread across X0..X1."""
    keys = [k for k in row if k]
    gaps = len(row) - len(keys)
    pitch = (X1 - X0 - gaps * GAP) / len(keys)
    width = round(pitch - 10)
    out, x = [], X0
    for k in row:
        if k is None:
            x += GAP
            continue
        out.append((k[0], k[1], round(x + pitch / 2), width))
        x += pitch
    return out


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
        out.append({"key": "knob%d" % (i + 1), "name": "Knob %d" % (i + 1), "min": 0, "max": 1, "default": 0.5})
    out.append({"key": "enc", "name": "Encoder", "min": 0, "max": 1, "default": 0.5})
    out.append({"key": "enc_push", "name": "Encoder Push", "min": 0, "max": 1, "momentary": True, "hold_ms": 120})
    out.append({"key": "shift", "name": "Shift", "options": ["Off", "On"], "default": 0})
    out.append({"key": "hold", "name": "Hold", "options": ["Off", "On"], "default": 0})
    for b in range(41):
        c = caption_of(b) or ("Step %d" % (STEP_BUTTONS.index(b) + 1) if b in STEP_BUTTONS else "Key %d" % b)
        name = c if not c.isdigit() else "Step " + c
        name = {"◀": "Page Down", "▶": "Page Up"}.get(name, name.title() if name.isupper() else name)
        if b == 32:
            continue   # SHIFT is its own toggle
        out.append({"key": btn(b), "name": name, "min": 0, "max": 1, "momentary": True, "hold_ms": 120})
    for i, f in enumerate(FADERS):
        out.append({"key": "fader%d" % (i + 1), "name": "%s Fader" % f.title(), "min": 0, "max": 1, "default": 1,
                    "dynamic_display": True})
    out.append({"key": "master", "name": "Master", "min": -24, "max": 6, "default": 0, "dynamic_display": True})
    out.append({"key": "sync", "name": "MPC Sync", "options": ["Off", "On"], "default": 1})
    # set by the engine (it reports them; nothing else does)
    for b in range(41):
        out.append({"key": "led_%02d" % b, "name": "LED %d" % b, "min": 0, "max": 1, "default": 0, "display": "int"})
    for c in range(32):
        # 0..126: MPC shows frame round(value * 126) of the 128-frame strip, so
        # character n reads n + 0.3 (VoltkitMpc.cpp), inside frame n however
        # it rounds; 127 is never printable.
        out.append({"key": "lcd_%02d" % c, "name": "LCD %d" % (c + 1), "min": 0, "max": 126, "default": 32,
                    "display": "int"})
    sections = [
        {"label": "Panel", "keys": ["knob1", "knob2", "knob3", "knob4", "enc", "enc_push", "shift", "hold"]
         + [btn(b) for b in range(41) if b != 32]},
        {"label": "Mix", "keys": ["fader%d" % (i + 1) for i in range(6)] + ["master", "sync"]},
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
    w("# The display: a display-only knob per character cell (skin_post.py).")
    for r, cy in enumerate(LCD_ROWS):
        for c in range(16):
            w("knob    cx=%d cy=%d r=%d label=\"\" key=lcd_%02d bw=%d ns=0 vs=0"
              % (LCD_X + c * LCD_CELL + LCD_CELL // 2, cy, LCD_CELL // 2 - 5, r * 16 + c, LCD_CELL))
    w("# The four endless knobs, under the display's columns; the encoder.")
    for i in range(4):
        w("knob    cx=%d cy=%d r=%d label=\"\" key=knob%d bw=%d ns=0 vs=0" % (knob_cx(i), KNOB_CY, KNOB_R, i + 1, 2 * KNOB_R + 20))
    w("knob    cx=%d cy=%d r=%d label=\"\" key=enc bw=%d ns=0 vs=0" % (ENC_CX, ENC_CY, ENC_R, 2 * ENC_R + 20))
    for key, cap, cy in SIDE_KEYS:
        kind = "button" if key == "enc_push" else "toggle"
        if kind == "button":
            w("button  cx=%d cy=%d w=%d h=%d label=\"%s\" key=%s" % (SIDE_CX, cy, SIDE_W, SIDE_H, cap, key))
        else:
            w("toggle  cx=%d cy=%d label=\"%s\" key=%s bw=%d" % (SIDE_CX, cy, cap, key, SIDE_W))
    w("# The keys, row by row, each with its LED above.")
    for name, (row, cy, h) in ROWS.items():
        for b, cap, cx, kw in row_keys(row, h):
            w("button  cx=%d cy=%d w=%d h=%d label=\"%s\" key=%s" % (cx, cy, kw, h, cap, btn(b)))
            w("knob    cx=%d cy=%d r=%d label=\"\" key=led_%02d bw=%d ns=0 vs=0"
              % (cx, cy - h // 2 - LED_ABOVE, LED_R, b, 2 * LED_R + 10))
    # SHIFT's LED, by its key
    w("knob    cx=%d cy=%d r=%d label=\"\" key=led_32 bw=%d ns=0 vs=0"
      % (SIDE_CX - SIDE_W // 2 - 16, 222, LED_R, 2 * LED_R + 10))
    w("")
    w("[tab MIX]")
    w("# A fader per voice group, the output level and the MPC sync switch.")
    pitch = 128
    for i in range(6):
        w("slider_v cx=%d cy=%d w=%d h=%d label=\"\" key=fader%d bw=120 ns=0 vs=24"
          % (110 + pitch * i, FADER_CY, FADER_W, FADER_H, i + 1))
    w("slider_v cx=%d cy=%d w=%d h=%d label=\"\" key=master bw=140 ns=0 vs=24" % (MASTER_CX, FADER_CY, FADER_W, FADER_H))
    w("toggle  cx=%d cy=%d label=\"MPC SYNC\" key=sync bw=%d" % (1140, 200, SIDE_W))
    return "\n".join(L) + "\n"


# skin_post.py's on/off keys: toggle key -> (caption, w, h)
BIG_KEYS = {"shift": ("SHIFT", SIDE_W, SIDE_H), "hold": ("HOLD", SIDE_W, SIDE_H), "sync": ("MPC SYNC", SIDE_W, SIDE_H)}


if __name__ == "__main__":
    json.dump(params(), open(os.path.join(HERE, "params.json"), "w"), indent=1)
    open(os.path.join(HERE, "layout.conf"), "w").write(layout())
