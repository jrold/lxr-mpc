"""VOLTKIT skin touch-ups, run by gen_vst.py after the skin is built:

    python3 skin/skin_post.py <skin dir>

1. Big on/off keys. A layout `button` fires Toggle Switch twice per tap, so
   on/off parameters are `toggle`s, and each one in KEYS becomes a key of
   that size (its own definition with key images, keeping the toggle's
   actions), placed around the toggle's centre.
2. Cyan knob values (TUI.json label colour).
3. The artwork, repainted as cyan-lit graphite hardware (hardware.py): the
   page backgrounds, every voice page's own image (the `when=` mode images),
   popup lists and their options, the page keys and other segments, the
   stepper arrows and the knob filmstrip. Geometry and bindings stay as
   generated.

Needs Pillow, the Titillium Web fonts build.sh copies to build/fonts/, and
mpc-vst-plugins' tools (layout parser, control geometry) in $MPC_VST or
mpc/.mpc-vst-plugins.
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
sys.path.insert(0, os.path.join(os.environ.get("MPC_VST") or os.path.join(HERE, ".mpc-vst-plugins"), "tools"))
import hardware as hw  # noqa: E402
import shadow_skin  # noqa: E402

W, H, Y_OFF = shadow_skin.W, shadow_skin.H, shadow_skin.Y_OFF
SS = hw.SS
TOGGLE_TOP = 18   # shadow_skin.py places a toggle at cy - 18

# toggle key -> (caption, width, height)
KEYS = {
    "learn": ("PAD LEARN", 400, 90),
}


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
        if isinstance(cd, dict) and str(cd.get("type", "")).startswith(prefix):
            yield node
        for v in node.values():
            yield from placements(v, prefix)
    elif isinstance(node, list):
        for x in node:
            yield from placements(x, prefix)


def big_keys(tui, params, save):
    defs = find_defs(tui)
    by_name = {d["key"]: d for d in defs}
    done = set()
    for placed in list(placements(tui, "shToggle")):
        index = int(placed["handle remapping"]["map"][0]["value"].split()[-1])
        name = params[index]["key"]
        if name not in KEYS:
            continue
        caption, w, h = KEYS[name]
        toggle = by_name[placed["componentData"]["type"]]
        new_key = "vkKey_%s" % name
        if new_key not in by_name:
            for lit in (False, True):
                save(hw.key(w, h, caption, lit), "vk_key_%s_%s.png" % (name, "on" if lit else "off"))
            value = copy.deepcopy(toggle["value"])
            focus = next(c for c in value["componentsData"] if c["componentData"]["type"] == "Focus")
            button = next(c for c in value["componentsData"] if c["componentData"]["type"] == "Button")
            focus["bounds"]["bounds"] = "0 0 %d %d" % (w, h)
            button["componentData"]["data"]["onImage"] = "vk_key_%s_on.png" % name
            button["componentData"]["data"]["offImage"] = "vk_key_%s_off.png" % name
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
    missing = set(KEYS) - done
    if missing:
        sys.exit("skin_post: no toggle placed for %s" % ", ".join(sorted(missing)))


# MPC slices a knob filmstrip as the generator's 128 square frames whatever
# the skin says, and a Live III mis-sliced a 16384 px strip (PerkyBits at
# r=59); r=54 (118 px frames, 15104 px) draws correctly. So keep 128 frames,
# under 16384 px.
def check_knob_strip(r, frame, frames):
    if frames != 128 or frame * frames >= 16384:
        sys.exit("skin_post: knob r=%d: a %d-frame, %d px strip won't slice right on MPC (keep r <= 58)"
                 % (r, frame, frame * frames))


def cyan_knob_values(tui):
    for d in find_defs(tui):
        if not d["key"].startswith("shKnob"):
            continue
        labels = [c for c in d["value"]["componentsData"] if c["componentData"]["type"] == "Label"]
        if len(labels) >= 2:   # name, then value
            labels[-1]["componentData"]["data"]["textStyle"]["colour"] = "ff" + "%02x%02x%02x" % hw.AMBER
            labels[0]["componentData"]["data"]["textStyle"]["colour"] = "ff" + "%02x%02x%02x" % hw.INK


# ---------------------------------------------------------------- artwork

def option_labels(w, params):
    if w.get("options"):
        return w["options"]
    for p in params:
        if p["key"] == w["key"]:
            return p.get("options", [])
    return []


def caption(img, cx, y, text, size=19):
    """A control's name, in light lettering above it (layout coordinates)."""
    d = ImageDraw.Draw(img)
    f = hw.font(size, bold=True)
    x = cx * SS - (d.textlength(text, font=f) + (len(text) - 1) * 2 * SS) / 2
    hw.spaced(d, (x, (y - Y_OFF) * SS + 2 * SS), text, f, (0, 0, 0, 170), 2 * SS)
    hw.spaced(d, (x, (y - Y_OFF) * SS), text, f, hw.INK_DIM + (255,), 2 * SS)


def draw_widgets(bg, widgets):
    def box(x0, y0, x1, y1):
        return (x0, y0 - Y_OFF, x1, y1 - Y_OFF)

    for w in widgets:
        if w["kind"] == "frame":
            hw.panel(bg, box(w["x"], w["y"], w["x"] + w["w"], w["y"] + w["h"]), w.get("title"))
    for w in widgets:
        k = w["kind"]
        if k == "readout":
            hw.well(bg, box(w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["cx"] + w["w"] // 2, w["cy"] + w["h"] // 2))
        elif k == "popup":
            hw.well(bg, box(w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["cx"] + w["w"] // 2, w["cy"] + w["h"] // 2))
            if w.get("label"):
                caption(bg, w["cx"], w["cy"] - w["h"] // 2 - 22, w["label"])
        elif k == "stepper":
            x0, h = w["cx"] - w["w"] // 2, w["h"]
            hw.well(bg, box(x0 + h + 4, w["cy"] - h // 2, x0 + w["w"] - h - 4, w["cy"] + h // 2))
            if w.get("label"):
                caption(bg, w["cx"], w["cy"] - h // 2 - 22, w["label"])
        elif k == "enum_h" and w.get("label"):
            rs = shadow_skin.seg_rects(w)
            caption(bg, w["cx"], min(r[1] for r in rs) - 22, w["label"])
        elif k == "knob":
            hw.knob_base(bg, w["cx"], w["cy"] - Y_OFF, 2 * w["r"] + 10)
            # shadow_skin's knob: the strip (2r + 10 square), then the name, then
            # the value label, centred frame/2 + 42 below the knob's centre.
            vy = w["cy"] + (2 * w["r"] + 10) // 2 + 42
            hw.well(bg, box(w["cx"] - 72, vy - 18, w["cx"] + 72, vy + 18), radius=5)


def page_image(widgets):
    bg = hw.plate(W, H)
    draw_widgets(bg, widgets)
    return bg.resize((W, H), Image.Resampling.LANCZOS).convert("RGB")


def modes_of(tab):
    """when= widgets grouped by mode, in the generator's order (shadow_skin.build)."""
    modes = {}
    for w in tab["widgets"]:
        if w.get("when"):
            modes.setdefault(w["when"], []).append(w)
    return modes


def mode_rect(ws):
    """The area a mode's image covers (shadow coords), as shadow_skin.build crops it."""
    rects = [shadow_skin.baked_rect(w) for w in ws if shadow_skin.baked_rect(w)]
    if not rects:
        return None
    bx, by = max(0, min(r[0] for r in rects)), max(Y_OFF, min(r[1] for r in rects))
    bw = min(W, max(r[0] + r[2] for r in rects)) - bx
    bh = min(Y_OFF + H, max(r[1] + r[3] for r in rects)) - by
    return bx, by, bw, bh


def popup_panel_image(pw, ph):
    s = Image.new("RGBA", (pw * SS, ph * SS), (0, 0, 0, 255))
    d = ImageDraw.Draw(s)
    d.rounded_rectangle((0, 0, pw * SS - 1, ph * SS - 1), 8 * SS, fill=(10, 12, 15, 255),
                        outline=hw.AMBER_DEEP + (255,), width=2 * SS)
    return s.resize((pw, ph), Image.Resampling.LANCZOS).convert("RGB")


def main():
    skin = sys.argv[1]
    pdir = skin if os.path.exists(os.path.join(skin, "TUI.json")) else os.path.join(skin, "Plugin Skins")
    tui_path = os.path.join(pdir, "TUI.json")
    tui = json.load(open(tui_path))
    params = json.load(open(os.path.join(HERE, "params.json")))["params"]
    tabs, top = shadow_skin.parse_layout(os.path.join(HERE, "layout.conf"))
    shadow_skin.apply_theme(top)
    for tab in tabs:   # option controls carry their parameter's options, as in shadow_skin.build
        for w in tab["widgets"]:
            if w["kind"] in ("enum_h", "enum_v", "popup") and not w.get("options"):
                w["options"] = option_labels(w, params)
    fonts = os.path.join(HERE, "build", "fonts")
    hw.use_fonts(os.path.join(fonts, "TitilliumWeb-Bold.ttf"), os.path.join(fonts, "TitilliumWeb-SemiBold.ttf"))
    written = []

    def save(img, name):
        img.save(os.path.join(pdir, name), optimize=True)
        written.append(name)

    big_keys(tui, params, save)
    cyan_knob_values(tui)
    json.dump(tui, open(tui_path, "w"), indent=1)

    knob_sizes = set()
    popopt = {}   # (text, w, h) -> image name, numbered as shadow_skin.build numbers them
    for t, tab in enumerate(tabs):
        widgets = tab["widgets"]
        base = [w for w in widgets if not w.get("when")]
        save(page_image(base), "sh_bg_%d.png" % t)
        for m, (_, ws) in enumerate(modes_of(tab).items()):
            rect = mode_rect(ws)
            if rect is None:
                continue
            bx, by, bw, bh = rect
            img = page_image(base + ws).crop((bx, by - Y_OFF, bx + bw, by - Y_OFF + bh))
            save(img, "sh_mode_%d_%d.png" % (t, m))

        for w in widgets:
            k = w["kind"]
            if k in ("enum_h", "enum_v"):
                labels = option_labels(w, params)
                for o, (_, _, sw, sh) in enumerate(shadow_skin.seg_rects(dict(w, options=labels))):
                    for lit in (False, True):
                        save(hw.key(sw, sh, labels[o].upper(), lit, led=False),
                             "sh_seg_%s_%d_%s.png" % (w["key"], o, "on" if lit else "off"))
            elif k == "popup":
                wo = dict(w, options=option_labels(w, params))
                (px, py, pw, ph), orects = shadow_skin.popup_panel(wo)
                save(popup_panel_image(pw, ph), "sh_pop_%d_%s.png" % (t, w["key"]))
                for o, (ox, oy, ow, oh) in enumerate(orects):
                    sig = (wo["options"][o], ow, oh)
                    if sig in popopt:
                        continue
                    name = "sh_po_%d" % len(popopt)
                    popopt[sig] = name
                    for lit in (False, True):
                        save(hw.key(ow, oh, wo["options"][o].upper(), lit, led=False, size=min(20, oh * 0.45)),
                             "%s_%s.png" % (name, "on" if lit else "off"))
            elif k == "button":
                for path in glob.glob(os.path.join(pdir, "sh_btn_%s_*_off.png" % w["key"])):
                    bw, bh = Image.open(path).size
                    label = w.get("label", "")
                    save(hw.key(bw, bh, label, False, led=False), os.path.basename(path))
                    save(hw.key(bw, bh, label, True, led=False), os.path.basename(path)[:-len("_off.png")] + "_on.png")
            elif k == "stepper":
                for side, direction in (("prev", -1), ("next", 1)):
                    for path in glob.glob(os.path.join(pdir, "sh_arrow_*_%s_%s.png" % (w["key"], side))):
                        aw, ah = Image.open(path).size
                        save(hw.arrow_key(aw, ah, direction), os.path.basename(path))
            elif k == "knob":
                knob_sizes.add(w["r"])

    # Every popup option image the generator made must have been repainted.
    made = set(os.path.basename(p) for p in glob.glob(os.path.join(pdir, "sh_po_*_off.png")))
    ours = set("%s_off.png" % n for n in popopt.values())
    if made != ours:
        sys.exit("skin_post: popup option images don't line up with the generator's (%d vs %d)" % (len(made), len(ours)))

    for r in knob_sizes:
        name = "sh_knob_r%d.png" % r
        strip = Image.open(os.path.join(pdir, name))
        frame, frames = strip.width, strip.height // strip.width
        check_knob_strip(r, frame, frames)
        save(hw.knob_strip(frame, frames), name)

    print("skin_post: %d big keys, %d images repainted" % (len(KEYS), len(written)))


if __name__ == "__main__":
    main()
