"""The interface pack's own drawings, in the PC interface's palette: worn iron,
dark leather and brass. What the PC pak has no picture of at all is drawn here
- the pad's buttons first of all - shaded rather than outlined, so it sits
beside the PC's art (`build_ui_pack.PC`) without looking like a wireframe.
"""
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

SS = 4

IRON_LIGHT = (226, 220, 204, 255)
IRON = (148, 140, 124, 255)
IRON_DARK = (70, 64, 56, 255)
IRON_EDGE = (22, 19, 16, 255)
LEATHER = (38, 31, 24, 240)
LEATHER_LIT = (62, 50, 34, 245)
INSET = (14, 12, 10, 235)
BRASS = (232, 186, 88, 255)
BRASS_DARK = (132, 92, 30, 255)
INK = (236, 228, 208, 255)

PAD_COLOURS = {
    "A": ((132, 214, 84), (40, 112, 34)),
    "B": ((240, 104, 84), (140, 28, 24)),
    "X": ((98, 160, 246), (24, 62, 150)),
    "Y": ((250, 212, 78), (164, 108, 14)),
}


class PC:
    """A rectangle of one of the PC pak's own textures, drawn under a name of
    the pack's: the pack ships the coordinates, never the picture. `tint`
    multiplies it by a colour ("RRGGBB"); `flip` mirrors it left to right,
    `vflip` top to bottom."""

    def __init__(self, texture, x, y, w, h, tint=None, flip=False, vflip=False):
        self.texture, self.tint = texture, tint
        self.x, self.y, self.w, self.h = x, y, w, h
        if flip:
            self.x, self.w = x + w, -w
        if vflip:
            self.y, self.h = y + h, -h


UI1 = "media/ui/ui1.dds"            # the PC's `GuiLook`
UI2 = "media/ui/ui2.dds"
UIICONS = "media/ui/uiicons.dds"
PANEL_TEX = "media/UI/models/{0}/{0}_{1}.dds"   # inventory, character, journal... top/bottom


def _font(px):
    return ImageFont.load_default(size=max(4, round(px)))


def _mask(w, h, draw):
    """An antialiased mask `w` x `h`: `draw(d, k)` paints it white at `k` times
    the size."""
    big = Image.new("L", (w * SS, h * SS), 0)
    draw(ImageDraw.Draw(big), SS)
    return big.resize((w, h), Image.LANCZOS)


def _gradient(w, h, top, bottom):
    column = Image.new("RGBA", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        column.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return column.resize((w, h))


def _fill(mask, top, bottom):
    out = _gradient(mask.width, mask.height, top, bottom)
    out.putalpha(ImageChops.multiply(out.getchannel("A"), mask))
    return out


def _stack(w, h, *layers):
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for layer in layers:
        out = Image.alpha_composite(out, layer)
    return out


def _letter(w, h, text, px, colour, cx=None, cy=None, shadow=(0, 0, 0, 200)):
    big = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    cx = w / 2 if cx is None else cx
    cy = h / 2 if cy is None else cy
    font = _font(px * SS)
    if shadow:
        d.text((cx * SS + SS * px * 0.05, cy * SS + SS * px * 0.07), text, font=font, fill=shadow,
               anchor="mm")
    d.text((cx * SS, cy * SS), text, font=font, fill=colour, anchor="mm",
           stroke_width=max(1, round(px * SS * 0.035)), stroke_fill=colour)
    return big.resize((w, h), Image.LANCZOS)


def _shape(w, h, draw, top, bottom, edge=IRON_EDGE, rim=1.6, shine=0.0):
    """A solid shape with a dark edge, a vertical gradient inside it and, with
    `shine`, a soft highlight over its upper part."""
    outer = _mask(w, h, lambda d, k: draw(d, k, 0.0))
    inner = _mask(w, h, lambda d, k: draw(d, k, rim * w / 24.0 if rim else 0.0))
    layers = [_fill(outer, edge, edge), _fill(inner, top, bottom)]
    if shine > 0:
        gloss = _mask(w, h, lambda d, k: draw(d, k, rim * w / 24.0 + w * 0.06))
        fade = _gradient(w, h, (255, 255, 255, round(255 * shine)), (255, 255, 255, 0))
        fade = fade.crop((0, 0, w, max(1, h // 2))).resize((w, h))
        top_half = Image.new("L", (w, h), 0)
        top_half.paste(255, (0, 0, w, round(h * 0.5)))
        fade.putalpha(ImageChops.multiply(ImageChops.multiply(fade.getchannel("A"), gloss),
                                          top_half.filter(ImageFilter.GaussianBlur(h * 0.06))))
        layers.append(fade)
    return _stack(w, h, *layers)


# --------------------------------------------------------------------------
# The pad.
# --------------------------------------------------------------------------

def face_button(n, letter):
    light, dark = PAD_COLOURS[letter]

    def disc(d, k, inset):
        d.ellipse([(0.5 + inset) * k, (0.5 + inset) * k, (n - 0.5 - inset) * k,
                   (n - 0.5 - inset) * k], fill=255)

    body = _shape(n, n, disc, light + (255,), dark + (255,), rim=1.5, shine=0.55)
    return _stack(n, n, body, _letter(n, n, letter, n * 0.62, (255, 255, 255, 255), cy=n * 0.52))


def _slab(w, h, radius, label, px, top=(132, 124, 110, 255), bottom=(52, 47, 41, 255), poly=None):
    def body(d, k, inset):
        if poly is not None:
            cx, cy = w / 2, h / 2
            pts = [((x + (inset if x < cx else -inset)) * k, (y + (inset if y < cy else -inset)) * k)
                   for x, y in poly]
            d.polygon(pts, fill=255)
        else:
            d.rounded_rectangle([(0.5 + inset) * k, (0.5 + inset) * k, (w - 0.5 - inset) * k,
                                 (h - 0.5 - inset) * k], radius=max(0.0, radius - inset) * k,
                                fill=255)

    out = _shape(w, h, body, top, bottom, rim=1.3 * 24.0 / w * (min(w, h) / 22.0), shine=0.35)
    if label:
        out = _stack(w, h, out, _letter(w, h, label, px, INK, cy=h * 0.54))
    return out


def bumper(w, h, label):
    return _slab(w, h, h * 0.42, label, h * 0.56)


def trigger(w, h, label, right):
    # Seen from behind: a tall key, its outer shoulder cut away.
    cut = w * 0.34
    poly = ([(0.5, 0.5), (w - cut, 0.5), (w - 0.5, cut), (w - 0.5, h - 0.5), (0.5, h - 0.5)]
            if right else
            [(cut, 0.5), (w - 0.5, 0.5), (w - 0.5, h - 0.5), (0.5, h - 0.5), (0.5, cut)])
    out = _slab(w, h, 0, "", 0, poly=poly)
    return _stack(w, h, out, _letter(w, h, label, w * 0.5, INK, cy=h * 0.6))


def pill_button(w, h, arrow):
    """Back and Start: a small metal pill with a triangle, "left" or "right"."""
    out = _slab(w, h, h * 0.5, "", 0)
    m, a = h / 2, h * 0.2
    sign = -1 if arrow == "left" else 1

    def tri(d, k, inset):
        d.polygon([((w / 2 - sign * a * 0.8) * k, (m - a) * k),
                   ((w / 2 + sign * a) * k, m * k),
                   ((w / 2 - sign * a * 0.8) * k, (m + a) * k)], fill=255)

    return _stack(w, h, out, _fill(_mask(w, h, lambda d, k: tri(d, k, 0)), INK, INK))


def stick(w, h, letter, arrows):
    """A thumbstick, with arrows for the ways it is pushed: any of "l", "r",
    "u", "d"."""
    r = min(w, h) * (0.3 if arrows else 0.46)
    cx, cy = w / 2, h / 2

    def cap(d, k, inset):
        d.ellipse([(cx - r + inset) * k, (cy - r + inset) * k, (cx + r - inset) * k,
                   (cy + r - inset) * k], fill=255)

    def arrowheads(d, k, inset):
        a, gap = r * 0.42, r + 2
        if "u" in arrows:
            d.polygon([((cx - a) * k, (cy - gap) * k), (cx * k, (cy - gap - a * 1.3) * k),
                       ((cx + a) * k, (cy - gap) * k)], fill=255)
        if "d" in arrows:
            d.polygon([((cx - a) * k, (cy + gap) * k), (cx * k, (cy + gap + a * 1.3) * k),
                       ((cx + a) * k, (cy + gap) * k)], fill=255)
        if "l" in arrows:
            d.polygon([((cx - gap) * k, (cy - a) * k), ((cx - gap - a * 1.3) * k, cy * k),
                       ((cx - gap) * k, (cy + a) * k)], fill=255)
        if "r" in arrows:
            d.polygon([((cx + gap) * k, (cy - a) * k), ((cx + gap + a * 1.3) * k, cy * k),
                       ((cx + gap) * k, (cy + a) * k)], fill=255)

    body = _shape(w, h, cap, IRON, IRON_DARK, rim=1.5 * 24.0 / w * (2 * r / 22.0), shine=0.3)
    heads = _mask(w, h, lambda d, k: arrowheads(d, k, 0))
    shadow = _fill(heads.filter(ImageFilter.GaussianBlur(1.2)), IRON_EDGE, IRON_EDGE)
    return _stack(w, h, shadow, _fill(heads, IRON_LIGHT, IRON_LIGHT), body,
                  _letter(w, h, letter, r * 1.05, INK, cx=cx, cy=cy + r * 0.04))


def dpad(n, arrows):
    """The d-pad, the arms in `arrows` ("lr" or "ud") lit."""
    k0 = n / 24.0
    arm = [(8.5, 1.5), (15.5, 1.5), (15.5, 8.5), (22.5, 8.5), (22.5, 15.5), (15.5, 15.5),
           (15.5, 22.5), (8.5, 22.5), (8.5, 15.5), (1.5, 15.5), (1.5, 8.5), (8.5, 8.5)]

    def cross(d, k, inset):
        c = 12.0
        pts = [((x + (inset / k0 if x < c else -inset / k0)) * k0 * k,
                (y + (inset / k0 if y < c else -inset / k0)) * k0 * k) for x, y in arm]
        d.polygon(pts, fill=255)

    def heads(d, k, inset):
        s = k0 * k
        if "l" in arrows:
            d.polygon([(3.2 * s, 12 * s), (7.2 * s, 9.6 * s), (7.2 * s, 14.4 * s)], fill=255)
        if "r" in arrows:
            d.polygon([(20.8 * s, 12 * s), (16.8 * s, 9.6 * s), (16.8 * s, 14.4 * s)], fill=255)
        if "u" in arrows:
            d.polygon([(12 * s, 3.2 * s), (9.6 * s, 7.2 * s), (14.4 * s, 7.2 * s)], fill=255)
        if "d" in arrows:
            d.polygon([(12 * s, 20.8 * s), (9.6 * s, 16.8 * s), (14.4 * s, 16.8 * s)], fill=255)

    body = _shape(n, n, cross, IRON, IRON_DARK, rim=1.3, shine=0.25)
    lit = _fill(_mask(n, n, lambda d, k: heads(d, k, 0)), INK, INK)
    return _stack(n, n, body, lit)


# --------------------------------------------------------------------------
# Plates, boxes and marks.
# --------------------------------------------------------------------------

def box(w, h, radius=8.0, top=LEATHER, bottom=None, edge=IRON, edge_width=1.4, inner_shadow=True):
    """A panel: an iron rim around dark leather, darker towards its top edge."""
    bottom = bottom or top

    def body(d, k, inset):
        d.rounded_rectangle([(0.5 + inset) * k, (0.5 + inset) * k, (w - 0.5 - inset) * k,
                             (h - 0.5 - inset) * k], radius=max(0.5, radius - inset) * k, fill=255)

    outer = _mask(w, h, lambda d, k: body(d, k, 0))
    mid = _mask(w, h, lambda d, k: body(d, k, 1.0))
    inner = _mask(w, h, lambda d, k: body(d, k, 1.0 + edge_width))
    layers = [_fill(outer, IRON_EDGE, IRON_EDGE),
              _fill(mid, tuple(min(255, c + 40) for c in edge[:3]) + (edge[3],), edge),
              _fill(inner, IRON_EDGE, IRON_EDGE),
              _fill(_mask(w, h, lambda d, k: body(d, k, 1.8 + edge_width)), top, bottom)]
    if inner_shadow:
        depth = max(2, min(h // 2, 14))
        full_shade = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        full_shade.paste(_gradient(w, depth, (0, 0, 0, 120), (0, 0, 0, 0)), (0, 2))
        full_shade.putalpha(ImageChops.multiply(
            full_shade.getchannel("A"), _mask(w, h, lambda d, k: body(d, k, 1.8 + edge_width))))
        layers.append(full_shade)
    return _stack(w, h, *layers)


def glow_outline(w, h, inner, colour=BRASS, radius=10.0, width=2.2):
    """A lit outline around `inner` = (x0, y0, x1, y1) of a larger transparent
    picture, with a soft halo: the frame that marks the chosen row."""
    x0, y0, x1, y1 = inner

    def ring(d, k, inset):
        d.rounded_rectangle([x0 * k, y0 * k, x1 * k, y1 * k], radius=radius * k, outline=255,
                            width=max(1, round(width * k)))

    line = _mask(w, h, lambda d, k: ring(d, k, 0))
    halo = line.filter(ImageFilter.GaussianBlur(3.0))
    halo = halo.point(lambda v: min(255, int(v * 1.6)))
    bright = tuple(min(255, c + 50) for c in colour[:3]) + (255,)
    return _stack(w, h, _fill(halo, colour[:3] + (150,), colour[:3] + (150,)),
                  _fill(line, bright, colour))


def chevron(w, h, up, colour=IRON_LIGHT):
    def mark(d, k, inset):
        m = w / 2
        pts = ([(m - w * 0.28, h * 0.72), (m, h * 0.26), (m + w * 0.28, h * 0.72)] if up else
               [(m - w * 0.28, h * 0.28), (m, h * 0.74), (m + w * 0.28, h * 0.28)])
        d.line([(x * k, y * k) for x, y in pts], fill=255, width=max(1, round(h * 0.14 * k)),
               joint="curve")

    line = _mask(w, h, lambda d, k: mark(d, k, 0))
    shadow = _fill(line.filter(ImageFilter.GaussianBlur(1.5)), IRON_EDGE, IRON_EDGE)
    return _stack(w, h, shadow, shadow, _fill(line, colour, colour))


def arrowhead(w, h, right, colour=BRASS):
    def mark(d, k, inset):
        pts = ([(w * 0.2, h * 0.16), (w * 0.84, h / 2), (w * 0.2, h * 0.84)] if right else
               [(w * 0.8, h * 0.16), (w * 0.16, h / 2), (w * 0.8, h * 0.84)])
        d.polygon([(x * k, y * k) for x, y in pts], fill=255)

    shape = _mask(w, h, lambda d, k: mark(d, k, 0))
    edge = _fill(shape.filter(ImageFilter.MaxFilter(3)), IRON_EDGE, IRON_EDGE)
    bright = tuple(min(255, c + 40) for c in colour[:3]) + (255,)
    dark = tuple(round(c * 0.6) for c in colour[:3]) + (255,)
    return _stack(w, h, edge, _fill(shape, bright, dark))


def controller(w, h):
    """The pad, seen from the front: the Help screen's picture."""
    k0 = w / 268.0

    def body(d, k, inset):
        s = k0 * k
        i = inset / k0
        d.rounded_rectangle([(40 + i) * s, (30 + i) * s, (228 - i) * s, (120 - i) * s],
                            radius=(40 - i) * s, fill=255)
        d.ellipse([(14 + i) * s, (60 + i) * s, (104 - i) * s, (180 - i) * s], fill=255)
        d.ellipse([(164 + i) * s, (60 + i) * s, (254 - i) * s, (180 - i) * s], fill=255)

    def shoulders(d, k, inset):
        s = k0 * k
        d.rounded_rectangle([56 * s, 18 * s, 106 * s, 44 * s], radius=10 * s, fill=255)
        d.rounded_rectangle([162 * s, 18 * s, 212 * s, 44 * s], radius=10 * s, fill=255)

    def disc(cx, cy, r):
        def draw(d, k, inset):
            i = min(inset, r * k0 * 0.45)
            d.ellipse([(cx - r) * k0 * k + i * k, (cy - r) * k0 * k + i * k,
                       (cx + r) * k0 * k - i * k, (cy + r) * k0 * k - i * k], fill=255)
        return draw

    layers = [
        _shape(w, h, shoulders, IRON, IRON_DARK, rim=0.12, shine=0.2),
        _shape(w, h, body, (96, 90, 80, 255), (40, 36, 32, 255), rim=0.2, shine=0.12),
    ]
    for cx, cy, r in ((80, 68, 17), (164, 104, 15)):       # the two sticks, in their wells
        layers.append(_fill(_mask(w, h, lambda d, k, c=(cx, cy, r + 4): disc(*c)(d, k, 0)),
                            IRON_EDGE, IRON_EDGE))
        layers.append(_shape(w, h, disc(cx, cy, r), IRON, IRON_DARK, rim=0.1, shine=0.3))
    for cx, cy in ((122, 70), (146, 70)):                  # Back and Start
        layers.append(_shape(w, h, disc(cx, cy, 5), IRON_LIGHT, IRON, rim=0.06))
    for (cx, cy), letter in zip(((196, 50), (212, 66), (180, 66), (196, 82)), "YBXA"):
        light, dark = PAD_COLOURS[letter]
        layers.append(_shape(w, h, disc(cx, cy, 8), light + (255,), dark + (255,), rim=0.08,
                             shine=0.5))
    out = _stack(w, h, *layers)
    pad = dpad(round(34 * k0), "")
    out.alpha_composite(pad, (round(87 * k0), round(88 * k0)))
    return out
