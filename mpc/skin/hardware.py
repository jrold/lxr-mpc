"""VOLTKIT's look, drawn with Pillow: a dark graphite faceplate lit in
electric cyan.

Anodised graphite panels with light lettering and dark steel screws, cyan
section lamps, gunmetal knobs on black knurled skirts with a cyan pointer
and value arc, dark keys that light cyan, and blue-black display windows.
(Derived from TINBOX's and SACKFLEX's hardware drawing.) Everything is
drawn at SS times its size and scaled down for smooth edges. skin_post.py
places it all.
"""
import math
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

SS = 2

# The accent: electric cyan. (Still called AMBER so the drawing code shared
# with TINBOX and SACKFLEX reads the same.)
AMBER = (49, 208, 255)
AMBER_HI = (170, 238, 255)
AMBER_DEEP = (16, 132, 196)
AMBER_DIM = (12, 62, 86)
INK = (232, 236, 239)        # light lettering (on graphite)
INK_DARK = (232, 236, 239)   # panel lettering: light on this dark faceplate
INK_DIM = (134, 144, 154)
KEY_INK_ON = (4, 24, 36)     # dark lettering on a lit cyan key

# Knob travel: -0.8 pi to 0.8 pi, from the bottom-left round to the bottom-right.
ANGLE_START, ANGLE_END = -0.8 * math.pi, 0.8 * math.pi

FONT_BOLD = FONT_SEMI = None


def use_fonts(bold, semi):
    global FONT_BOLD, FONT_SEMI
    FONT_BOLD, FONT_SEMI = bold, semi


def font(px, bold=False):
    return ImageFont.truetype(FONT_BOLD if bold else FONT_SEMI, int(round(px * SS)))


# ---------------------------------------------------------------- helpers

def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def vertical(w, h, stops):
    """RGB vertical gradient from [(position 0..1, colour), ...]."""
    col = Image.new("RGB", (1, h))
    px = col.load()
    for y in range(h):
        t = y / max(1, h - 1)
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                px[0, y] = lerp(c0, c1, (t - p0) / (p1 - p0) if p1 > p0 else 0)
                break
        else:
            px[0, y] = stops[-1][1]
    return col.resize((w, h))


def brushed(w, h, seed, strength):
    """Horizontal brushed-metal streaks as an L image centred on 128."""
    random.seed(seed)
    noise = Image.effect_noise((max(1, w // 6), h), 60)
    streak = noise.resize((w, h), Image.Resampling.BILINEAR).filter(ImageFilter.BoxBlur(1))
    fine = Image.effect_noise((w, h), 20)
    out = ImageChops.add(streak, fine, 2.0, 0)
    return out.point(lambda v: int(128 + (v - 128) * strength))


def apply_texture(img, tex):
    rgb = Image.merge("RGB", [tex, tex, tex])
    return ImageChops.add(img.convert("RGB"), rgb, 1.0, -128)


def shadow(size, box, radius, blur, alpha, ellipse=False):
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    (d.ellipse if ellipse else (lambda b, fill: d.rounded_rectangle(b, radius, fill=fill)))(box, fill=int(255 * alpha))
    m = m.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.putalpha(m)
    return out


def glow(size, box, colour, blur, alpha, ellipse=False, radius=0):
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    if ellipse:
        d.ellipse(box, fill=int(255 * alpha))
    else:
        d.rounded_rectangle(box, radius, fill=int(255 * alpha))
    m = m.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new("RGBA", size, colour + (0,))
    out.putalpha(m)
    return out


def rounded_mask(w, h, r):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w - 1, h - 1), r, fill=255)
    return m


def spaced(d, xy, text, f, fill, spacing, anchor="lm"):
    x, y = xy
    for c in text:
        d.text((x, y), c, font=f, fill=fill, anchor=anchor)
        x += d.textlength(c, font=f) + spacing
    return x


# ---------------------------------------------------------------- page parts

def plate(w, h):
    """The page behind everything: dark anodised graphite."""
    img = vertical(w * SS, h * SS, [(0, (30, 32, 36)), (1, (16, 17, 20))])
    img = apply_texture(img, brushed(w * SS, h * SS, 11, 0.05))
    return img.convert("RGBA")


def screw(img, x, y, r=7):
    r *= SS
    img.alpha_composite(shadow(img.size, (x - r, y - r + SS, x + r, y + r + 2 * SS), 0, 2 * SS, 0.55, True))
    d = ImageDraw.Draw(img)
    for i in range(r, 0, -1):   # a domed steel head: light top-left
        t = i / r
        c = lerp((150, 156, 164), (52, 55, 60), t)
        d.ellipse((x - i - (r - i) * 0.25, y - i - (r - i) * 0.25, x + i - (r - i) * 0.25, y + i - (r - i) * 0.25), fill=c + (255,))
    d.ellipse((x - r, y - r, x + r, y + r), outline=(14, 15, 17, 255), width=SS)
    a = r * 0.55
    for dx, dy in ((a, 0), (0, a)):   # Phillips cross
        d.line((x - dx, y - dy, x + dx, y + dy), fill=(20, 21, 24, 255), width=int(1.6 * SS))


def panel(img, box, title=None):
    """A section of the faceplate: lighter graphite edged with a fine black
    rule and a hairline of cyan, steel screws in its corners, the title lit."""
    x0, y0, x1, y1 = (int(v * SS) for v in box)
    w, h = x1 - x0, y1 - y0
    r = 6 * SS
    img.alpha_composite(shadow(img.size, (x0, y0 + 3 * SS, x1, y1 + 5 * SS), r, 5 * SS, 0.35))
    face = vertical(w, h, [(0, (50, 53, 59)), (0.5, (40, 43, 48)), (1, (31, 33, 37))])
    face = apply_texture(face, brushed(w, h, x0 + y0, 0.05))
    img.paste(face, (x0, y0), rounded_mask(w, h, r))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((x0, y0, x1 - 1, y1 - 1), r, outline=(6, 6, 8, 255), width=2 * SS)
    d.line((x0 + r, y0 + 2 * SS, x1 - r, y0 + 2 * SS), fill=AMBER_DIM + (255,), width=SS)
    d.line((x0 + r, y1 - 3 * SS, x1 - r, y1 - 3 * SS), fill=(0, 0, 0, 70), width=SS)
    i = 15 * SS
    for sx, sy in ((x0 + i, y0 + i), (x1 - i, y0 + i), (x0 + i, y1 - i), (x1 - i, y1 - i)):
        screw(img, sx, sy)
    if title:
        led_title(img, x0 + 34 * SS, y0 + 26 * SS, title)


def led_title(img, x, y, text):
    """A small cyan lamp and the section name in light lettering."""
    s = 7 * SS
    d = ImageDraw.Draw(img)
    img.alpha_composite(glow(img.size, (x - s, y - s, x + s, y + s), AMBER, 6 * SS, 0.45))
    d.rectangle((x - s - SS, y - s - SS, x + s + SS, y + s + SS), fill=(6, 6, 8, 255))
    led = vertical(2 * s, 2 * s, [(0, AMBER_HI), (0.5, AMBER), (1, AMBER_DEEP)])
    img.paste(led, (x - s, y - s))
    f = font(26, bold=True)
    spaced(d, (x + s + 16 * SS, y + 2 * SS), text.upper(), f, (0, 0, 0, 160), 3 * SS)
    spaced(d, (x + s + 16 * SS, y), text.upper(), f, INK_DARK + (255,), 3 * SS)


def tray(img, box, screws=True):
    """A black anodised strip on the faceplate, behind a group of controls."""
    x0, y0, x1, y1 = (int(v * SS) for v in box)
    w, h = x1 - x0, y1 - y0
    r = 4 * SS
    face = vertical(w, h, [(0, (14, 15, 17)), (1, (8, 9, 10))])
    face = apply_texture(face, brushed(w, h, x1 + y1, 0.06))
    img.paste(face, (x0, y0), rounded_mask(w, h, r))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((x0, y0, x1, y1), r, outline=(10, 10, 10, 255), width=SS)
    d.line((x0 + r, y1 + SS, x1 - r, y1 + SS), fill=(255, 255, 255, 40), width=SS)   # the lit lower lip
    if screws:
        i = 12 * SS
        for sx, sy in ((x0 + i, y0 + i), (x1 - i, y0 + i)):
            screw(img, sx, sy, 5)


def well(img, box, dividers=(), radius=6):
    """A display window: blue-black glass, a dim cyan rim, faint scan lines,
    and hairlines between its rows."""
    x0, y0, x1, y1 = (int(v * SS) for v in box)
    r = radius * SS
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((x0 - 2 * SS, y0 - 2 * SS, x1 + 2 * SS, y1 + 2 * SS), r + 2 * SS, fill=(12, 12, 12, 255))
    d.rounded_rectangle((x0, y0, x1, y1), r, fill=(3, 7, 10, 255))
    img.alpha_composite(glow(img.size, (x0 + 30 * SS, y0, x1 - 30 * SS, y1), (6, 40, 58), 18 * SS, 0.35, radius=r))
    for yy in range(y0 + 2 * SS, y1 - 2 * SS, 3 * SS):
        d.line((x0 + 3 * SS, yy, x1 - 3 * SS, yy), fill=(49, 208, 255, 7), width=1)
    d.rounded_rectangle((x0, y0, x1, y1), r, outline=AMBER_DIM + (220,), width=SS)
    for dy in dividers:
        yy = int(dy * SS)
        d.line((x0 + 4 * SS, yy, x1 - 4 * SS, yy), fill=AMBER_DIM + (160,), width=SS)


def knob_base(img, cx, cy, frame):
    """What sits round a knob on the panel: its shadow and the scale ticks
    just outside the filmstrip (grey, the ends and centre cyan)."""
    cx, cy = cx * SS, cy * SS
    rb = frame * 0.42 * SS
    img.alpha_composite(shadow(img.size, (cx - rb, cy - rb + 5 * SS, cx + rb, cy + rb + 12 * SS), 0, 8 * SS, 0.9, True))
    d = ImageDraw.Draw(img)
    r0 = (frame / 2 + 3) * SS
    for i in range(13):
        a = ANGLE_START + i / 12 * (ANGLE_END - ANGLE_START)
        sx, sy = math.sin(a), -math.cos(a)
        r1 = r0 + (9 if i % 6 == 0 else 6) * SS
        d.line((cx + sx * r0, cy + sy * r0, cx + sx * r1, cy + sy * r1),
               fill=AMBER + (255,) if i % 6 == 0 else (112, 120, 128, 255), width=int(1.8 * SS))


def key(w, h, label, lit, led=True, size=None):
    """A key: a dark cap with light lettering, lit cyan (with a soft glow)
    when on. The body leaves a margin for the glow in both states so the two
    images line up."""
    s = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    m = 5 * SS
    box = (m, m, w * SS - m, h * SS - m - SS)
    bw, bh = box[2] - box[0], box[3] - box[1]
    r = 5 * SS
    if lit:
        s.alpha_composite(glow(s.size, (box[0] - SS, box[1] - SS, box[2] + SS, box[3] + SS), AMBER, 3 * SS, 0.7, radius=r))
    s.alpha_composite(shadow(s.size, (box[0], box[1] + 2 * SS, box[2], box[3] + 3 * SS), r, 2 * SS, 0.7))
    if lit:
        body = vertical(bw, bh, [(0, (160, 236, 255)), (0.45, (49, 208, 255)), (1, (14, 146, 206))])
    else:
        body = vertical(bw, bh, [(0, (70, 74, 81)), (0.55, (52, 55, 61)), (1, (38, 40, 45))])
    s.paste(body, (box[0], box[1]), rounded_mask(bw, bh, r))
    d = ImageDraw.Draw(s)
    d.rounded_rectangle(box, r, outline=(8, 80, 120, 255) if lit else (10, 10, 12, 255), width=SS)
    d.line((box[0] + r, box[1] + SS * 1.5, box[2] - r, box[1] + SS * 1.5), fill=(255, 255, 255, 200 if lit else 50), width=SS)

    text_x = (box[0] + box[2]) / 2
    if led:   # a small lamp at the left: dark, or lit
        lx, ly = box[0] + 13 * SS, (box[1] + box[3]) / 2
        lw, lh = 3 * SS, min(bh * 0.42, 22 * SS)
        d.rounded_rectangle((lx - lw, ly - lh / 2, lx + lw, ly + lh / 2), lw,
                            fill=(236, 252, 255, 255) if lit else (14, 44, 60, 255))
        text_x += 7 * SS
    if label:
        px = size or min(30, h * 0.40)
        f = font(px, bold=True)
        while d.textlength(label, font=f) > bw - (46 if led else 12) * SS and px > 12:
            px -= 1
            f = font(px, bold=True)
        ty = (box[1] + box[3]) / 2
        if lit:
            d.text((text_x, ty + SS), label, font=f, fill=(255, 255, 255, 110), anchor="mm")
            d.text((text_x, ty), label, font=f, fill=KEY_INK_ON + (255,), anchor="mm")
        else:
            d.text((text_x, ty + SS), label, font=f, fill=(0, 0, 0, 200), anchor="mm")
            d.text((text_x, ty), label, font=f, fill=INK + (255,), anchor="mm")
    return s.resize((w, h), Image.Resampling.LANCZOS)


def arrow_key(w, h, direction):
    s = key(w, h, "", False, led=False).resize((w * SS, h * SS), Image.Resampling.LANCZOS)
    d = ImageDraw.Draw(s)
    cx, cy = w * SS / 2, h * SS / 2 - SS
    a = min(w, h) * 0.18 * SS
    pts = [(cx - a * 0.7, cy - a), (cx - a * 0.7, cy + a), (cx + a * 0.9, cy)] if direction > 0 else \
          [(cx + a * 0.7, cy - a), (cx + a * 0.7, cy + a), (cx - a * 0.9, cy)]
    d.polygon(pts, fill=AMBER + (255,))
    return s.resize((w, h), Image.Resampling.LANCZOS)


def tile(w, h, lit):
    """A list row: a dark slot, or a cyan-edged one for the lit row."""
    s = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(s)
    r = 6 * SS
    box = (SS, SS, w * SS - SS, h * SS - SS)
    body = vertical(box[2] - box[0], box[3] - box[1],
                    [(0, (8, 46, 64)), (1, (4, 26, 38))] if lit else [(0, (30, 31, 34)), (1, (19, 20, 22))])
    s.paste(body, (box[0], box[1]), rounded_mask(body.width, body.height, r))
    d.rounded_rectangle(box, r, outline=AMBER + (255,) if lit else (6, 6, 6, 255), width=SS * (2 if lit else 1))
    d.line((box[0] + r, box[1] + SS * 1.5, box[2] - r, box[1] + SS * 1.5), fill=(255, 255, 255, 26), width=SS)
    if lit:
        d.rounded_rectangle((box[0] + 4 * SS, box[1] + 8 * SS, box[0] + 9 * SS, box[3] - 8 * SS), 2 * SS, fill=AMBER + (255,))
    return s.resize((w, h), Image.Resampling.LANCZOS)


def knob_cap(s, c, rk, rf):
    """The part of the knob that turns, drawn pointing up: the knurled skirt
    (ridges that visibly travel as it turns), a spun-metal face whose two
    bright sheen bands sweep round with it, and the cyan pointer."""
    cap = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(cap)
    d.ellipse((c - rk - 2 * SS, c - rk - 2 * SS, c + rk + 2 * SS, c + rk + 2 * SS), fill=(2, 2, 2, 255))
    d.ellipse((c - rk, c - rk, c + rk, c + rk), fill=(20, 20, 19, 255))
    ridges = 60
    for i in range(ridges):   # each ridge: a lit face and a shadowed face
        for half, col in ((0.0, (78, 77, 72)), (0.5, (8, 8, 8))):
            a = (i + half) / ridges * 2 * math.pi
            sx, sy = math.cos(a), math.sin(a)
            d.line((c + sx * (rf + SS), c + sy * (rf + SS), c + sx * (rk - SS), c + sy * (rk - SS)),
                   fill=col + (255,), width=int(1.6 * SS))

    # Spun-metal face: fine rings, and anisotropic sheen in two opposite bands.
    size = int(2 * rf)
    face = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = face.load()
    for y in range(size):
        dy = y + 0.5 - rf
        for x in range(size):
            dx = x + 0.5 - rf
            r = math.hypot(dx, dy)
            if r > rf:
                continue
            theta = math.atan2(dy, dx)
            sheen = abs(math.cos(theta - math.pi * 0.25)) ** 10
            rings = 5 * math.sin(r * 1.7)
            v = 74 + rings + 70 * sheen - 18 * (r / rf)   # spun gunmetal
            v = int(max(0, min(255, v)))
            px[x, y] = (v, min(255, v + 3), min(255, v + 8), 255)
    cap.alpha_composite(face, (int(c - rf), int(c - rf)))
    d.ellipse((c - rf, c - rf, c + rf, c + rf), outline=(4, 4, 4, 255), width=SS)

    # The pointer: a cyan line from near the centre to the rim.
    d.line((c, c - rf * 0.30, c, c - rf + 3 * SS), fill=(4, 20, 30, 255), width=7 * SS)
    d.line((c, c - rf * 0.32, c, c - rf + 4 * SS), fill=AMBER + (255,), width=4 * SS)
    return cap


def knob_light(s, c, rk):
    """Fixed lighting over the turning cap: a soft highlight from the top
    left and shade toward the bottom right, as from a lamp above the panel."""
    light = glow((s, s), (c - rk * 0.95, c - rk * 1.0, c + rk * 0.15, c - rk * 0.05), (255, 255, 255), 8 * SS, 0.20, True)
    shade = glow((s, s), (c - rk * 0.1, c + rk * 0.05, c + rk * 1.0, c + rk * 1.0), (0, 0, 0), 10 * SS, 0.45, True)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).ellipse((c - rk, c - rk, c + rk, c + rk), fill=255)
    out = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    out.alpha_composite(light)
    out.alpha_composite(shade)
    out.putalpha(ImageChops.multiply(out.getchannel("A"), mask))
    return out


def knob_strip(frame, frames):
    """The knob filmstrip: inside a glowing cyan value arc, a black knurled
    knob that turns with its value (knurling, face sheen and pointer all
    rotate) under fixed lighting. The scale ticks are on the page
    (knob_base), outside the frame."""
    s = frame * SS
    c = s / 2
    r_arc = c - 4 * SS        # value arc, at the frame's edge
    rk = c - 10 * SS          # knurled skirt
    rf = rk - 6 * SS          # face
    a0, a1 = math.degrees(ANGLE_START) - 90, math.degrees(ANGLE_END) - 90

    base = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(base).arc((c - r_arc, c - r_arc, c + r_arc, c + r_arc), a0, a1, fill=(8, 34, 48, 255), width=4 * SS)
    cap = knob_cap(s, c, rk, rf)
    light = knob_light(s, c, rk)

    out = Image.new("RGBA", (s, s * frames), (0, 0, 0, 0))
    for i in range(frames):
        fr = base.copy()
        a = ANGLE_START + i / (frames - 1) * (ANGLE_END - ANGLE_START)
        if i > 0:   # the value arc, with a soft glow kept inside the frame
            lit = Image.new("RGBA", (s, s), AMBER + (0,))
            ImageDraw.Draw(lit).arc((c - r_arc, c - r_arc, c + r_arc, c + r_arc), a0, math.degrees(a) - 90,
                                    fill=AMBER + (255,), width=6 * SS)
            lit.putalpha(lit.getchannel("A").filter(ImageFilter.GaussianBlur(2 * SS)).point(lambda v: int(v * 0.7)))
            fr.alpha_composite(lit)
            ImageDraw.Draw(fr).arc((c - r_arc, c - r_arc, c + r_arc, c + r_arc), a0, math.degrees(a) - 90,
                                   fill=AMBER + (255,), width=4 * SS)
        # PIL turns counter-clockwise; the knob turns clockwise as the value rises.
        fr.alpha_composite(cap.rotate(-math.degrees(a), resample=Image.Resampling.BICUBIC, center=(c, c)))
        fr.alpha_composite(light)
        out.paste(fr, (0, i * s))
    return out.resize((frame, frame * frames), Image.Resampling.LANCZOS)


# ---------------------------------------------------------------- the LXR panel

LCD_GLASS = (3, 14, 20)       # the display's backlit glass
LCD_GHOST = (10, 34, 46)      # an unlit dot
LCD_DOT = (138, 232, 255)     # a lit dot
LED_OFF = (14, 40, 54)


def lcd_window(img, box):
    """The display's window: a black bezel round backlit glass (the cells'
    strips draw their dots on it)."""
    x0, y0, x1, y1 = (int(v * SS) for v in box)
    d = ImageDraw.Draw(img)
    img.alpha_composite(shadow(img.size, (x0, y0 + 3 * SS, x1, y1 + 6 * SS), 10 * SS, 6 * SS, 0.6))
    d.rounded_rectangle((x0, y0, x1, y1), 10 * SS, fill=(8, 8, 9, 255))
    g = 12 * SS
    d.rounded_rectangle((x0 + g, y0 + g, x1 - g, y1 - g), 4 * SS, fill=LCD_GLASS + (255,))
    img.alpha_composite(glow(img.size, (x0 + 60 * SS, y0 + g, x1 - 60 * SS, y1 - g), (14, 70, 96), 30 * SS, 0.30, radius=4 * SS))
    d.rounded_rectangle((x0 + g, y0 + g, x1 - g, y1 - g), 4 * SS, outline=(0, 0, 0, 255), width=2 * SS)
    d.line((x0 + 14 * SS, y1 - 2 * SS, x1 - 14 * SS, y1 - 2 * SS), fill=(255, 255, 255, 40), width=SS)


def lcd_strip(cell, frames, glyph, pitch=5, dot=4):
    """A display cell's filmstrip: frame n is character n (glyph(n) -> 8 rows
    of 5 dots), lit dots glowing over the unlit grid, on transparent ground
    (the glass is on the page)."""
    s = cell * SS
    gw, gh = 5 * pitch * SS, 8 * pitch * SS
    ox, oy = (s - gw) // 2, (s - gh) // 2
    out = Image.new("RGBA", (s, s * frames), (0, 0, 0, 0))
    for n in range(frames):
        fr = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        lit = Image.new("RGBA", (s, s), LCD_DOT + (0,))
        d, dl = ImageDraw.Draw(fr), ImageDraw.Draw(lit)
        for r, row in enumerate(glyph(n)):
            for c, on in enumerate(row):
                x, y = ox + c * pitch * SS, oy + r * pitch * SS
                box = (x, y, x + dot * SS - 1, y + dot * SS - 1)
                d.rectangle(box, fill=(LCD_DOT if on else LCD_GHOST) + (255,))
                if on:
                    dl.rectangle(box, fill=LCD_DOT + (255,))
        halo = lit.getchannel("A").filter(ImageFilter.GaussianBlur(2 * SS)).point(lambda v: int(v * 0.45))
        lit.putalpha(halo)
        glowed = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        glowed.alpha_composite(lit)
        glowed.alpha_composite(fr)
        out.paste(glowed, (0, n * s))
    return out.resize((cell, cell * frames), Image.Resampling.LANCZOS)


def led_strip(size, frames):
    """A key's LED: dark in the first half of the strip, lit cyan with a glow
    in the second (the engine reports 0 or 1)."""
    s = size * SS
    c, r = s / 2, size * 0.26 * SS
    out = Image.new("RGBA", (s, s * frames), (0, 0, 0, 0))
    for lit in (False, True):
        fr = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        if lit:
            fr.alpha_composite(glow((s, s), (c - r * 1.3, c - r * 1.3, c + r * 1.3, c + r * 1.3), AMBER, 3 * SS, 0.9, True))
        d = ImageDraw.Draw(fr)
        d.ellipse((c - r - SS, c - r - SS, c + r + SS, c + r + SS), fill=(4, 4, 5, 255))
        body = vertical(int(2 * r), int(2 * r), [(0, AMBER_HI), (0.5, AMBER), (1, AMBER_DEEP)] if lit
                        else [(0, (30, 62, 78)), (1, LED_OFF)])
        m = Image.new("L", body.size, 0)
        ImageDraw.Draw(m).ellipse((0, 0, body.width - 1, body.height - 1), fill=255)
        fr.paste(body, (int(c - r), int(c - r)), m)
        for n in range(frames // 2, frames) if lit else range(frames // 2):
            out.paste(fr, (0, n * s))
    return out.resize((size, size * frames), Image.Resampling.LANCZOS)


def endless_strip(frame, frames):
    """An endless knob's filmstrip: no scale or arc, the cap turning a full
    circle across the strip with its pointer up at the middle frame (where
    the knob rests between turns)."""
    s = frame * SS
    c = s / 2
    rk = c - 6 * SS
    rf = rk - 6 * SS
    cap = knob_cap(s, c, rk, rf)
    light = knob_light(s, c, rk)
    out = Image.new("RGBA", (s, s * frames), (0, 0, 0, 0))
    for i in range(frames):
        a = (i / (frames - 1) - 0.5) * 2 * math.pi
        fr = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        fr.alpha_composite(cap.rotate(-math.degrees(a), resample=Image.Resampling.BICUBIC, center=(c, c)))
        fr.alpha_composite(light)
        out.paste(fr, (0, i * s))
    return out.resize((frame, frame * frames), Image.Resampling.LANCZOS)


def knob_well(img, cx, cy, frame):
    """The recess an endless knob sits in: its shadow and a dark ring."""
    cx, cy = cx * SS, cy * SS
    rb = frame * 0.46 * SS
    img.alpha_composite(shadow(img.size, (cx - rb, cy - rb + 5 * SS, cx + rb, cy + rb + 11 * SS), 0, 8 * SS, 0.9, True))
    d = ImageDraw.Draw(img)
    r0 = (frame / 2 + 2) * SS
    d.ellipse((cx - r0, cy - r0, cx + r0, cy + r0), outline=(8, 8, 9, 255), width=3 * SS)
    d.ellipse((cx - r0 - 3 * SS, cy - r0 - 3 * SS, cx + r0 + 3 * SS, cy + r0 + 3 * SS), outline=AMBER_DIM + (200,), width=SS)


# ---------------------------------------------------------------- faders

FADER_CAP_H = 30


def fader_base(img, cx, cy, h):
    """What sits round a fader on the panel: a scale of ticks either side
    (0..10, the ends and middle longer and cyan)."""
    cx, top = cx * SS, (cy - h / 2) * SS
    d = ImageDraw.Draw(img)
    travel = (h - FADER_CAP_H) * SS
    y0 = top + FADER_CAP_H * SS / 2
    for i in range(11):
        y = y0 + travel * (1 - i / 10)
        long_ = i in (0, 5, 10)
        for side in (-1, 1):
            x0 = cx + side * 32 * SS
            x1 = x0 + side * (9 if long_ else 5) * SS
            d.line((x0, y, x1, y), fill=(AMBER if long_ else (112, 120, 128)) + (255,), width=int(1.4 * SS))


def fader_strip(w, h, frames):
    """The fader filmstrip, frames of w x h stacked top to bottom: a black
    slot lit cyan below the cap, and a gunmetal cap with grip lines and a
    cyan pointer line. Frame 0 is the bottom of the travel."""
    s_w, s_h = w * SS, h * SS
    cap_h = FADER_CAP_H * SS
    slot_w = 8 * SS
    cx = s_w / 2
    travel = s_h - cap_h

    base = Image.new("RGBA", (s_w, s_h), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d.rounded_rectangle((cx - slot_w / 2 - SS, cap_h / 2 - 2 * SS, cx + slot_w / 2 + SS, s_h - cap_h / 2 + 2 * SS),
                        slot_w / 2, fill=(40, 43, 48, 255))
    d.rounded_rectangle((cx - slot_w / 2, cap_h / 2 - SS, cx + slot_w / 2, s_h - cap_h / 2 + SS), slot_w / 2,
                        fill=(6, 7, 8, 255))

    cap_w = s_w - 6 * SS
    cap = Image.new("RGBA", (cap_w, cap_h), (0, 0, 0, 0))
    body = vertical(cap_w, cap_h, [(0, (112, 118, 126)), (0.45, (70, 74, 81)), (0.55, (44, 47, 52)), (1, (78, 82, 88))])
    cap.paste(body, (0, 0), rounded_mask(cap_w, cap_h, 4 * SS))
    cd = ImageDraw.Draw(cap)
    cd.rounded_rectangle((0, 0, cap_w - 1, cap_h - 1), 4 * SS, outline=(6, 6, 6, 255), width=SS)
    for gy in (cap_h * 0.22, cap_h * 0.32, cap_h * 0.68, cap_h * 0.78):   # grip ridges
        cd.line((5 * SS, gy, cap_w - 5 * SS, gy), fill=(138, 146, 156, 255), width=SS)
    cd.line((3 * SS, cap_h / 2, cap_w - 3 * SS, cap_h / 2), fill=AMBER + (255,), width=int(2.5 * SS))

    out = Image.new("RGBA", (s_w, s_h * frames), (0, 0, 0, 0))
    for k in range(frames):
        t = k / max(1, frames - 1)
        fr = base.copy()
        y = (1 - t) * travel   # the cap's top
        if t > 0:
            ImageDraw.Draw(fr).rounded_rectangle((cx - slot_w / 2 + SS, y + cap_h / 2, cx + slot_w / 2 - SS,
                                                  s_h - cap_h / 2), slot_w / 2, fill=AMBER + (255,))
        fr.alpha_composite(shadow(fr.size, (3 * SS, y + 3 * SS, s_w - 3 * SS, y + cap_h + 4 * SS), 4 * SS, 2 * SS, 0.6))
        fr.alpha_composite(cap, (3 * SS, int(y)))
        out.paste(fr, (0, k * s_h))
    return out.resize((w, h * frames), Image.Resampling.LANCZOS)
