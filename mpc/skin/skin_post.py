"""VOLTKIT skin touch-ups, run by gen_vst.py after the skin is built:

    python3 skin/skin_post.py <skin dir>

1. The display and the LEDs. Each display cell and each LED is a small knob
   in the layout (gen_layout.py); here it loses its touch (a display-only
   definition, just a filmstrip) and gets its strip: the display's 128
   characters (dotfont.py), or an LED dark then lit.
2. Big on/off keys. SHIFT, HOLD and MPC SYNC are `toggle`s (one flip per
   tap); each becomes a key of its size, with its own definition and key
   images, keeping the toggle's actions.
3. The artwork, repainted as cyan-lit graphite hardware (hardware.py): both
   pages' panels, every key's images, and the endless knobs' filmstrips.
   Geometry and bindings stay as generated.

Needs Pillow, the Titillium Web fonts build.sh copies to build/fonts/, and
mpc-vst-plugins' tools (layout parser) in $MPC_VST or mpc/.mpc-vst-plugins.

Copyright (C) 2026 Jason Rold. GPL-3.0-only.
"""
import copy
import glob
import json
import os
import sys

from PIL import Image, ImageDraw

SKIN_DIR = os.path.dirname(os.path.abspath(__file__))
HERE = os.path.dirname(SKIN_DIR)   # mpc/
sys.path.insert(0, SKIN_DIR)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.environ.get("MPC_VST") or os.path.join(HERE, ".mpc-vst-plugins"), "tools"))
import dotfont  # noqa: E402
import gen_layout as gl  # noqa: E402
import hardware as hw  # noqa: E402
import shadow_skin  # noqa: E402

W, H, Y_OFF = shadow_skin.W, shadow_skin.H, shadow_skin.Y_OFF
SS = hw.SS
TOGGLE_TOP = 18   # shadow_skin.py places a toggle at cy - 18
FRAMES = shadow_skin.FRAMES             # a knob strip's frames (MPC slices 128 square frames)
NUM_FRAMES = shadow_skin.ROT_FRAMES     # what a knob definition says: value v shows frame round(v * 127)
LED_SIZE = 2 * gl.LED_R + 10


# ---------------------------------------------------------------- TUI.json

def find_defs(node):
    """The list of {"key", "value"} component definitions."""
    if isinstance(node, list):
        if any(isinstance(x, dict) and str(x.get("key", "")).startswith("sh") for x in node):
            return node
        for x in node:
            found = find_defs(x)
            if found is not None:
                return found
    elif isinstance(node, dict):
        for v in node.values():
            found = find_defs(v)
            if found is not None:
                return found
    return None


def placements(node, prefix):
    if isinstance(node, dict):
        cd = node.get("componentData")
        if isinstance(cd, dict) and str(cd.get("type", "")).startswith(prefix) and "handle remapping" in node:
            yield node
        for v in node.values():
            yield from placements(v, prefix)
    elif isinstance(node, list):
        for x in node:
            yield from placements(x, prefix)


def param_of(placed, params):
    remap = placed.get("handle remapping", {}).get("map") or []
    return params[int(remap[0]["value"].split()[-1])]["key"] if remap else None


def big_keys(tui, params, save):
    defs = find_defs(tui)
    by_name = {d["key"]: d for d in defs}
    done = set()
    for placed in list(placements(tui, "shToggle")):
        name = param_of(placed, params)
        if name not in gl.BIG_KEYS:
            continue
        caption, w, h = gl.BIG_KEYS[name]
        toggle = by_name[placed["componentData"]["type"]]
        new_key = "sfKey_%s" % name
        if new_key not in by_name:
            for lit in (False, True):
                save(hw.key(w, h, caption, lit), "sf_key_%s_%s.png" % (name, "on" if lit else "off"))
            value = copy.deepcopy(toggle["value"])
            focus = next(c for c in value["componentsData"] if c["componentData"]["type"] == "Focus")
            button = next(c for c in value["componentsData"] if c["componentData"]["type"] == "Button")
            focus["bounds"]["bounds"] = "0 0 %d %d" % (w, h)
            button["componentData"]["data"]["onImage"] = "sf_key_%s_on.png" % name
            button["componentData"]["data"]["offImage"] = "sf_key_%s_off.png" % name
            button["bounds"]["bounds"] = "0 0 %d %d" % (w, h)
            value["componentsData"] = [focus, button]   # the caption is in the image
            entry = {"key": new_key, "value": value}
            defs.append(entry)
            by_name[new_key] = entry
        x, y, cw, _ = (int(v) for v in placed["bounds"]["bounds"].split())
        cx, cy = x + cw // 2, y + TOGGLE_TOP
        placed["componentData"]["type"] = new_key
        placed["bounds"]["bounds"] = "%d %d %d %d" % (cx - w // 2, cy - h // 2, w, h)
        done.add(name)
    missing = set(gl.BIG_KEYS) - done
    if missing:
        sys.exit("skin_post: no toggle placed for %s" % ", ".join(sorted(missing)))


def display_def(name, strip, size):
    """A definition that only shows a filmstrip: no touch, no focus."""
    return {"key": name, "value": {
        "version": 4, "actions": [],
        "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "0", "image": ""},
                           "unfocussed": {"version": 1, "colour": "0", "image": ""}},
        "ignoreMousePresses": True, "disableCoarseDataWheel": True, "repeats": 1, "hideQLinkBounds": True,
        "componentsData": [{"version": 2, "componentData": {
            "version": 1, "name": "Display", "type": "Knob",
            "data": {"version": 5, "knobType": "FilmStrip", "filmStrip": strip,
                     "numFrames": NUM_FRAMES, "invert": False, "dragOrientation": "Vertical", "handleName": "Data"}},
            "handle remapping": {"version": 1, "map": []},
            "bounds": {"version": 2, "acceptsHWFocus": "No", "showWhenDataModelInvalid": "Show",
                       "whenVisible": "Always", "boundsType": "Absolute", "bounds": "0 0 %d %d" % (size, size),
                       "additionalInvalidatingHandles": []}}]}}


def display_only(tui, params, save):
    """The display cells and LEDs: display-only, centred where the layout put
    their knobs."""
    defs = find_defs(tui)
    save(hw.lcd_strip(gl.LCD_CELL, FRAMES, dotfont.glyph), "sf_lcd.png")
    save(hw.led_strip(LED_SIZE, FRAMES), "sf_led.png")
    defs.append(display_def("sfLcd", "sf_lcd.png", gl.LCD_CELL))
    defs.append(display_def("sfLed", "sf_led.png", LED_SIZE))
    count = {"lcd_": 0, "led_": 0}
    for placed in list(placements(tui, "shKnob")):
        kind = (param_of(placed, params) or "")[:4]
        if kind not in count:
            continue
        size = gl.LCD_CELL if kind == "lcd_" else LED_SIZE
        x, y, w, h = (int(v) for v in placed["bounds"]["bounds"].split())
        cx, cy = x + w / 2, y + h / 2
        placed["componentData"]["type"] = "sfLcd" if kind == "lcd_" else "sfLed"
        placed["bounds"]["bounds"] = "%d %d %d %d" % (round(cx - size / 2), round(cy - size / 2), size, size)
        placed["bounds"]["acceptsHWFocus"] = "No"
        count[kind] += 1
    if count != {"lcd_": 32, "led_": 41}:
        sys.exit("skin_post: placed %d display cells and %d LEDs (want 32 and 41)" % (count["lcd_"], count["led_"]))


# ---------------------------------------------------------------- pages

def text(d, xy, s, px, fill, bold=True, spacing=0):
    """Centred lettering with a drop shadow, at a layout position."""
    f = hw.font(px, bold)
    x, y = xy[0] * SS, (xy[1] - Y_OFF) * SS
    if spacing:
        width = sum(d.textlength(c, font=f) for c in s) + spacing * SS * (len(s) - 1)
        hw.spaced(d, (x - width / 2, y + SS), s, f, (0, 0, 0, 170), spacing * SS)
        hw.spaced(d, (x - width / 2, y), s, f, fill, spacing * SS)
        return
    d.text((x, y + SS), s, font=f, fill=(0, 0, 0, 170), anchor="mm")
    d.text((x, y), s, font=f, fill=fill, anchor="mm")


def page_box(box):
    """A layout-coordinate box on the page image."""
    x0, y0, x1, y1 = box
    return x0, y0 - Y_OFF, x1, y1 - Y_OFF


def panel_page():
    img = hw.plate(W, H)
    hw.panel(img, page_box((12, 92, 1268, 348)))
    hw.lcd_window(img, page_box(gl.LCD_BEZEL))
    d = ImageDraw.Draw(img)
    for i in range(4):
        hw.knob_base(img, gl.knob_cx(i), gl.KNOB_CY - Y_OFF, 2 * gl.KNOB_R + 20)
        # a hairline from the display's column down to its knob
        cx = gl.knob_cx(i) * SS
        d.line((cx, (gl.LCD_BEZEL[3] - Y_OFF + 4) * SS, cx, (gl.KNOB_CY - gl.KNOB_R - 10 - Y_OFF) * SS),
               fill=hw.AMBER_DIM + (255,), width=SS)
    hw.knob_well(img, gl.ENC_CX, gl.ENC_CY - Y_OFF, 2 * gl.ENC_R)
    d = ImageDraw.Draw(img)
    text(d, (gl.ENC_CX, gl.ENC_CY + gl.ENC_R + 22), "DATA", 15, hw.INK_DIM + (255,), spacing=3)
    text(d, (gl.ENC_CX, 302), "VOLTKIT", 34, hw.INK + (255,), spacing=6)
    text(d, (gl.ENC_CX, 330), "DRUM COMPUTER", 13, hw.AMBER + (255,), spacing=4)

    # the key rows, each on its strip (the LEDs sit in its top margin)
    for name, (row, cy, h) in gl.ROWS.items():
        top = cy - h // 2 - 2 * gl.LED_R - gl.LED_ABOVE - 2
        bottom = cy + h // 2 + (30 if name == "steps" else 6)
        hw.tray(img, page_box((gl.X0 - 10, top, gl.X1 + 10, bottom)), screws=False)
    d = ImageDraw.Draw(img)
    steps = gl.row_keys(gl.STEP_ROW, 0)
    for (_, _, cx, _), label in zip(steps, gl.STEP_LABELS):
        text(d, (cx, gl.STEP_LABEL_Y), label, 13, hw.INK_DIM + (255,), bold=False)
    # SHIFT reads the step captions: a cyan rule under each group of four
    for g in range(4):
        x0 = steps[4 * g][2] - steps[4 * g][3] // 2
        x1 = steps[4 * g + 3][2] + steps[4 * g + 3][3] // 2
        y = (gl.STEP_LABEL_Y + 13 - Y_OFF) * SS
        d.line((x0 * SS, y, x1 * SS, y), fill=hw.AMBER_DIM + (255,), width=SS)
    return img.resize((W, H), Image.Resampling.LANCZOS)


def mix_page():
    img = hw.plate(W, H)
    hw.panel(img, page_box((12, 92, 1000, 700)))
    hw.panel(img, page_box((1012, 92, 1268, 700)), "Sync")
    d = ImageDraw.Draw(img)
    for i, name in enumerate(gl.FADERS + ["MASTER"]):
        cx = gl.MASTER_CX if name == "MASTER" else 110 + 128 * i
        text(d, (cx, gl.FADER_NAME_Y), name, 17, (hw.AMBER if name == "MASTER" else hw.INK) + (255,))
        hw.fader_base(img, cx, gl.FADER_CY - Y_OFF, gl.FADER_H)
        # the value's window, under the fader (where shadow_skin puts the value)
        top = gl.FADER_CY + gl.FADER_H // 2 + 3
        hw.well(img, page_box((cx - 52, top, cx + 52, top + 26)), radius=4)
    lines = ["The sequencer follows", "the MPC: its tempo,", "PLAY and STOP.", "",
             "Off: set a tempo on", "the PERF page (BPM).", "", "The pads play the", "voices on channel 1."]
    for n, line in enumerate(lines):
        text(d, (1140, 270 + 26 * n), line, 16, hw.INK_DIM + (255,), bold=False)
    return img.resize((W, H), Image.Resampling.LANCZOS)


def setup_page():
    img = hw.plate(W, H)
    hw.panel(img, page_box((12, 92, 1268, 340)), "Pads")
    hw.well(img, page_box((50, gl.PADS_INFO_Y - 23, 1230, gl.PADS_INFO_Y + 23)))
    hw.panel(img, page_box((12, 352, 1268, 700)), "Locks and recording")
    d = ImageDraw.Draw(img)
    text(d, (gl.PADS_X0 + (len(gl.PAD_MODES) * (gl.SWITCH_W + 2) - 2) // 2, gl.PADS_CY - 58), "PADS PLAY", 15,
         hw.INK_DIM + (255,), spacing=3)
    lines = [
        "VOICES: the pads play the seven voices. PAD LEARN, then hit the pads for D1, D2, D3, SN, CY, CH, OH.",
        "STEPS: the pads are the 16 step keys. PAD LEARN, then hit the pads for steps 1 to 16.",
        "",
        "To lock a step: hold its key (a pad in STEPS, or HOLD then the key on PANEL) for half a",
        "second, until its LED blinks; turn a knob or a Q-Link; let go (tap the key again after HOLD).",
        "",
        "To record: REC on PANEL, then PLAY on the MPC; the pads (in VOICES) record into the pattern.",
    ]
    for n, line in enumerate(lines):
        if line:
            text(d, (640, 430 + 34 * n), line, 18, hw.INK + (255,), bold=False)
    return img.resize((W, H), Image.Resampling.LANCZOS)


# ---------------------------------------------------------------- main

def main():
    skin = sys.argv[1]
    pdir = skin if os.path.exists(os.path.join(skin, "TUI.json")) else os.path.join(skin, "Plugin Skins")
    tui_path = os.path.join(pdir, "TUI.json")
    tui = json.load(open(tui_path))
    params = json.load(open(os.path.join(HERE, "params.json")))["params"]
    tabs, top = shadow_skin.parse_layout(os.path.join(HERE, "layout.conf"))
    shadow_skin.apply_theme(top)
    fonts = os.path.join(HERE, "build", "fonts")
    hw.use_fonts(os.path.join(fonts, "TitilliumWeb-Bold.ttf"), os.path.join(fonts, "TitilliumWeb-SemiBold.ttf"))
    written = []

    def save(img, name):
        img.save(os.path.join(pdir, name), optimize=True)
        written.append(name)

    if [t["name"] for t in tabs] != ["PANEL", "MIX", "SETUP"]:
        sys.exit("skin_post: expected the PANEL, MIX and SETUP tabs")
    display_only(tui, params, save)
    big_keys(tui, params, save)
    for d in find_defs(tui):   # the faders' values in cyan
        if d["key"].startswith("shSlider"):
            for c in d["value"]["componentsData"]:
                if c["componentData"]["type"] == "Label":
                    c["componentData"]["data"]["textStyle"]["colour"] = "ff31d0ff"

    save(panel_page(), "sh_bg_0.png")
    save(mix_page(), "sh_bg_1.png")
    save(setup_page(), "sh_bg_2.png")

    knob_sizes = set()
    for tab in tabs:
        for w in tab["widgets"]:
            if w["kind"] == "button":
                paths = glob.glob(os.path.join(pdir, "sh_btn_%s_*_off.png" % w["key"]))
                if len(paths) != 1:
                    sys.exit("skin_post: %d images for key %s" % (len(paths), w["key"]))
                bw, bh = Image.open(paths[0]).size
                on = paths[0][:-len("_off.png")] + "_on.png"
                arrow = {"btn_40": -1, "btn_00": 1}.get(w["key"])
                label = w.get("label", "")
                size = 30 if label.isdigit() else 17 if w["key"].startswith("btn_") else 22
                for lit, path in ((False, paths[0]), (True, on)):
                    if arrow:
                        img = hw.key(bw, bh, "", True, led=False) if lit else hw.arrow_key(bw, bh, arrow)
                    else:
                        img = hw.key(bw, bh, label, lit, led=False, size=size)
                    save(img, os.path.basename(path))
            elif w["kind"] == "enum_h":   # a switch: a key per option, lit when chosen
                options = w["options"] if isinstance(w["options"], list) else w["options"].split(",")
                for o, (_, _, sw, sh) in enumerate(shadow_skin.seg_rects(dict(w, options=options))):
                    for lit in (False, True):
                        save(hw.key(sw, sh, options[o].upper(), lit, led=False, size=22),
                             "sh_seg_%s_%d_%s.png" % (w["key"], o, "on" if lit else "off"))
            elif w["kind"] == "knob" and not w["key"].startswith(("lcd_", "led_")):
                knob_sizes.add(w["r"])

    for path in glob.glob(os.path.join(pdir, "sh_slider_v_*.png")):
        fw, fh = (int(v) for v in os.path.basename(path).split("_")[3].split("x"))
        save(hw.fader_strip(fw, fh, Image.open(path).height // fh), os.path.basename(path))

    for r in knob_sizes:
        name = "sh_knob_r%d.png" % r
        strip = Image.open(os.path.join(pdir, name))
        frame, frames = strip.width, strip.height // strip.width
        if frames != 128 or frame * frames > 16384:
            sys.exit("skin_post: knob r=%d: a %d-frame, %d px strip won't slice right on MPC" % (r, frames, frame * frames))
        # the encoder turns endlessly; the four knobs show their parameter's value
        save(hw.endless_strip(frame, frames) if r == gl.ENC_R else hw.knob_strip(frame, frames), name)

    # The generator's definitions and strips for the display cells' and LEDs'
    # knobs are unused now.
    defs = find_defs(tui)
    used = {p["componentData"]["type"] for p in placements(tui, "")}
    gone = [d["key"] for d in defs if "|" not in d["key"] and d["key"] not in used]
    defs[:] = [d for d in defs if d["key"] not in gone]
    for r in {w["r"] for t in tabs for w in t["widgets"] if w["kind"] == "knob" and w["key"].startswith(("lcd_", "led_"))}:
        if r not in knob_sizes:
            for path in glob.glob(os.path.join(pdir, "sh_knob_r%d.png" % r)):
                os.remove(path)
    json.dump(tui, open(tui_path, "w"), indent=1)

    print("skin_post: %d images painted, %d unused definitions dropped" % (len(written), len(gone)))


if __name__ == "__main__":
    main()
