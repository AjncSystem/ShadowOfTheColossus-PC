# SPDX-License-Identifier: GPL-2.0-or-later
"""The controller test's picture: a controller as it looks (PlayStation, Xbox or Switch Pro layout),
drawn with Pillow at three times the size and scaled down, so every edge is smooth. Pressed buttons
light up, the triggers fill and the sticks move. Our own drawing: no maker's artwork or logo. From the bloodborne_pc
launcher (bbport_pad_art.py, github.com/Supermedo/bloodborne_pc, GPL-2.0).

The controller at rest is drawn once per layout (base); a frame only adds what is pressed, the
triggers' fill and the sticks, so the picture can follow the controller live."""
from PIL import Image, ImageChops, ImageDraw, ImageFilter

import sotc_theme as theme

W, H = 600, 380  # the drawing's own units; render() scales them
SS = 3  # drawn this many times larger, then reduced: smooth edges
BODY, EDGE, WELL = '#202226', '#3a3d43', '#0e0f11'
PART, PART_EDGE, SYMBOL = '#2b2d32', '#45484f', theme.MUTED
LIT, LIT_SYMBOL = theme.SIGIL_LIGHT, theme.DEEP  # the sword's light

# Where each part is, per layout (centres in drawing units).
LAYOUTS = {
    'playstation': {'dpad': (165, 152), 'face': (435, 152), 'left_stick': (238, 232), 'right_stick': (362, 232),
                    'back': (212, 106), 'start': (388, 106), 'guide': (300, 222), 'touchpad': (300, 126)},
    'xbox': {'dpad': (238, 226), 'face': (422, 148), 'left_stick': (178, 148), 'right_stick': (362, 226),
             'back': (262, 150), 'start': (338, 150), 'guide': (300, 104), 'touchpad': None},
    'switch': {'dpad': (238, 226), 'face': (422, 148), 'left_stick': (178, 148), 'right_stick': (362, 226),
               'back': (258, 124), 'start': (342, 124), 'guide': (330, 168), 'touchpad': None},
}
# The face buttons' signs on each layout: north, east, south, west
FACE = {'playstation': ('triangle', 'circle', 'cross', 'square'), 'xbox': ('Y', 'B', 'A', 'X'),
        'switch': ('X', 'A', 'B', 'Y')}
SHOULDERS = {'playstation': ('L1', 'R1', 'L2', 'R2'), 'xbox': ('LB', 'RB', 'LT', 'RT'),
             'switch': ('L', 'R', 'ZL', 'ZR')}
_BASES = {}


def _font(size):
    from PIL import ImageFont
    for path in (theme.ASSETS / 'fonts' / 'Inter-Bold.ttf', 'segoeuib.ttf', 'arialbd.ttf'):
        try:
            return ImageFont.truetype(str(path), size)
        except OSError:
            continue
    return ImageFont.load_default()


def _silhouette(kind, s, size):
    """The body's mask: a wide middle and two grips that reach down and out."""
    mask = Image.new('L', size, 0)
    d = ImageDraw.Draw(mask)
    top = 82 if kind == 'playstation' else 78
    d.rounded_rectangle((92 * s, top * s, 508 * s, 268 * s), radius=78 * s, fill=255)
    for side in (-1, 1):
        grip = Image.new('L', size, 0)
        cx = 300 + side * 128
        ImageDraw.Draw(grip).ellipse(((cx - 74) * s, 150 * s, (cx + 74) * s, 372 * s), fill=255)
        grip = grip.rotate(side * 17, center=(cx * s, 240 * s), resample=Image.BICUBIC)
        mask = ImageChops.lighter(mask, grip)
    # blurred and cut again: the corners where grip and body meet become round
    return mask.filter(ImageFilter.GaussianBlur(9 * s)).point(lambda v: 255 if v >= 128 else 0)


def _symbol(d, name, cx, cy, r, color, font, s):
    """A face button's sign: PlayStation's shapes, else the letter."""
    w = max(2, round(2.2 * s))
    if name == 'triangle':
        d.polygon([(cx, cy - r * 0.62), (cx + r * 0.6, cy + r * 0.42), (cx - r * 0.6, cy + r * 0.42)], outline=color,
                  width=w)
    elif name == 'circle':
        k = r * 0.5
        d.ellipse((cx - k, cy - k, cx + k, cy + k), outline=color, width=w)
    elif name == 'cross':
        k = r * 0.46
        d.line((cx - k, cy - k, cx + k, cy + k), fill=color, width=w)
        d.line((cx - k, cy + k, cx + k, cy - k), fill=color, width=w)
    elif name == 'square':
        k = r * 0.44
        d.rectangle((cx - k, cy - k, cx + k, cy + k), outline=color, width=w)
    else:
        d.text((cx, cy + s), name, fill=color, font=font, anchor='mm')


class _Pen:
    """Draws the parts of one layout, all at rest (base) or only what is in use (live)."""

    def __init__(self, img, kind, buttons, axes, live, s, shapes=None):
        self.img, self.d, self.s, self.shapes = img, ImageDraw.Draw(img), s, shapes
        self.w = max(1, round(s))  # a line one drawing unit wide
        self.kind, self.at = kind, LAYOUTS[kind]
        self.buttons, self.axes, self.live = buttons, axes, live
        self.small, self.letters = _font(round(13 * s)), _font(round(15 * s))

    def on(self, name):
        return bool(self.buttons.get(name))

    def shown(self, name):
        """At rest every part; live only the pressed ones (the rest is in the base)."""
        return self.on(name) if self.live else True

    def box(self, box, name, radius=6, label=None):
        if not self.shown(name):
            return
        s, on = self.s, self.on(name) and self.live
        self.d.rounded_rectangle([v * s for v in box], radius=radius * s, fill=LIT if on else PART,
                                 outline=LIT if on else PART_EDGE, width=self.w)
        if label:
            self.d.text(((box[0] + box[2]) / 2 * s, (box[1] + box[3]) / 2 * s + s), label,
                        fill=LIT_SYMBOL if on else SYMBOL, font=self.small, anchor='mm')

    def circle(self, x, y, r, name):
        if not self.shown(name):
            return False
        s, on = self.s, self.on(name) and self.live
        self.d.ellipse(
            [(x - r) * s, (y - r) * s, (x + r) * s, (y + r) * s], fill=LIT if on else PART,
            outline=LIT if on else PART_EDGE, width=self.w)
        return True

    def shoulders(self):
        """L1/R1 as a band around the body's upper corner, L2/R2 as a second band behind it; a trigger
        turns gold the further it is pulled."""
        s, names = self.s, SHOULDERS[self.kind]
        for shoulder, trigger, label, tlabel in (('left_shoulder', 'left_trigger', names[0], names[2]),
                                                 ('right_shoulder', 'right_trigger', names[1], names[3])):
            for name, text in ((trigger, tlabel), (shoulder, label)):
                (mask, inner, at), (lx, ly) = self.shapes[name]
                if name == trigger:
                    amount = max(0.0, min(1.0, self.axes.get(trigger, 0.0))) if self.live else 0.0
                    if self.live and amount <= 0.02:
                        continue
                    lit = amount > 0.5
                    fill = theme.blend(PART, LIT, amount) if self.live else PART
                else:
                    if self.live and not self.on(name):
                        continue
                    lit = self.live
                    fill = LIT if lit else PART
                self.img.paste(LIT if self.live else PART_EDGE, at, mask)
                self.img.paste(fill, at, inner)
                self.d.text((lx * s, ly * s + s), text, fill=LIT_SYMBOL if lit else SYMBOL, font=self.small,
                            anchor='mm')

    def dpad(self):
        s = self.s
        cx, cy = self.at['dpad']
        arm, reach = 24, 26
        arms = (('dpad_up', (cx - arm / 2, cy - reach - arm / 2, cx + arm / 2, cy - arm / 2)),
                ('dpad_down', (cx - arm / 2, cy + arm / 2, cx + arm / 2, cy + reach + arm / 2)),
                ('dpad_left', (cx - reach - arm / 2, cy - arm / 2, cx - arm / 2, cy + arm / 2)),
                ('dpad_right', (cx + arm / 2, cy - arm / 2, cx + reach + arm / 2, cy + arm / 2)))
        for name, box in arms:
            if not self.shown(name):
                continue
            self.box(box, name, radius=5)
            on = self.on(name) and self.live
            mx, my, k = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2, 5
            tip = {'dpad_up': [(mx, my - k), (mx - k, my + k * 0.6), (mx + k, my + k * 0.6)],
                   'dpad_down': [(mx, my + k), (mx - k, my - k * 0.6), (mx + k, my - k * 0.6)],
                   'dpad_left': [(mx - k, my), (mx + k * 0.6, my - k), (mx + k * 0.6, my + k)],
                   'dpad_right': [(mx + k, my), (mx - k * 0.6, my - k), (mx - k * 0.6, my + k)]}[name]
            self.d.polygon([(x * s, y * s) for x, y in tip], fill=LIT_SYMBOL if on else SYMBOL)
        if not self.live:
            self.d.rectangle([(cx - arm / 2 + 1) * s, (cy - arm / 2 + 1) * s, (cx + arm / 2 - 1) * s,
                              (cy + arm / 2 - 1) * s], fill=PART)

    def face(self):
        s = self.s
        cx, cy = self.at['face']
        r, gap = 17, 34
        for (name, dx, dy), sign in zip((('north', 0, -gap), ('east', gap, 0), ('south', 0, gap), ('west', -gap, 0)),
                                        FACE[self.kind]):
            if self.circle(cx + dx, cy + dy, r, name):
                _symbol(self.d, sign, (cx + dx) * s, (cy + dy) * s, r * s,
                        LIT_SYMBOL if self.on(name) and self.live else SYMBOL, self.letters, s)

    def middle(self):
        s = self.s
        if self.at['touchpad']:
            x, y = self.at['touchpad']
            self.box((x - 74, y - 40, x + 74, y + 38), 'touchpad', radius=16)
        for name in ('back', 'start'):
            x, y = self.at[name]
            if self.kind == 'playstation':
                self.box((x - 6, y - 13, x + 6, y + 13), name, radius=6)
            elif self.circle(x, y, 10, name) and self.kind == 'switch':
                self.d.text((x * s, y * s + s), {'back': '−', 'start': '+'}[name],
                            fill=LIT_SYMBOL if self.on(name) and self.live else SYMBOL, font=self.small, anchor='mm')
        x, y = self.at['guide']
        self.circle(x, y, 15 if self.kind == 'xbox' else 11, 'guide')

    def sticks(self):
        """The wells at rest; live the caps, moved by the stick (gold while pressed or pushed)."""
        s = self.s
        for name in ('left', 'right'):
            x, y = self.at[f'{name}_stick']
            on = self.on(f'{name}_stick')
            if not self.live:
                self.d.ellipse([(x - 34) * s, (y - 34) * s, (x + 34) * s, (y + 34) * s], fill=WELL,
                               outline=PART_EDGE, width=self.w)
                continue
            if on:
                self.d.ellipse([(x - 34) * s, (y - 34) * s, (x + 34) * s, (y + 34) * s], outline=LIT, width=self.w)
            dx, dy = self.axes.get(f'{name}_x', 0.0), self.axes.get(f'{name}_y', 0.0)
            moved = abs(dx) > 0.15 or abs(dy) > 0.15
            cx, cy = x + 11 * dx, y + 11 * dy
            self.d.ellipse([(cx - 24) * s, (cy - 24) * s, (cx + 24) * s, (cy + 24) * s], fill=LIT if on else PART,
                           outline=LIT if (on or moved) else PART_EDGE, width=2 * self.w if moved else self.w)
            self.d.ellipse([(cx - 15) * s, (cy - 15) * s, (cx + 15) * s, (cy + 15) * s],
                           outline=LIT_SYMBOL if on else (LIT if moved else EDGE), width=self.w)


def _shoulder_shapes(kind, s, size):
    """{button: ((mask, inner mask, place), label centre)} of L1/R1/L2/R2: bands around the body's upper
    corners (the body's rounded rectangle grown by d1..d2), cut to the corner and rounded at the ends."""
    top = 82 if kind == 'playstation' else 78
    shapes = {}
    for name, (d1, d2), (x0, x1), reach, label_x in (('shoulder', (5, 23), (100, 246), 48, 200),
                                                       ('trigger', (28, 47), (112, 230), 22, 186)):
        band = Image.new('L', size, 0)
        g = ImageDraw.Draw(band)
        g.rounded_rectangle([(92 - d2) * s, (top - d2) * s, (508 + d2) * s, (268 + d2) * s], radius=(78 + d2) * s,
                            fill=255)
        g.rounded_rectangle([(92 - d1) * s, (top - d1) * s, (508 + d1) * s, (268 + d1) * s], radius=(78 + d1) * s,
                            fill=0)
        for side in ('left', 'right'):
            lo, hi = (x0, x1) if side == 'left' else (600 - x1, 600 - x0)
            keep = Image.new('L', size, 0)
            ImageDraw.Draw(keep).rectangle([lo * s, 0, hi * s, (top + reach) * s], fill=255)
            part = ImageChops.multiply(band, keep)
            # soft and cut again: round ends instead of straight cuts
            part = part.filter(ImageFilter.GaussianBlur(4 * s)).point(lambda v: 255 if v >= 140 else 0)
            box = part.getbbox()
            mask = part.crop(box)
            inner = mask.filter(ImageFilter.MinFilter(2 * max(1, round(s)) + 1))
            centre = (label_x if side == 'left' else 600 - label_x, top - (d1 + d2) / 2)
            shapes[f'{side}_{name}'] = ((mask, inner, box[:2]), centre)
    return shapes


_SHAPES = {}


def base(kind, width):
    """The controller at rest, without the stick caps, drawn SS times larger (once per layout and size)."""
    key = (kind, width)
    if key not in _BASES:
        s = width * SS / W
        size = (width * SS, round(width * H / W) * SS)
        img = Image.new('RGBA', size, (0, 0, 0, 0))
        shapes = _SHAPES[key] = _shoulder_shapes(kind, s, size)
        _Pen(img, kind, {}, {}, False, s, shapes).shoulders()
        mask = _silhouette(kind, s, size)
        img.paste(EDGE, (0, 0), mask.filter(ImageFilter.MaxFilter(2 * round(s) + 1)))
        img.paste(BODY, (0, 0), mask)
        pen = _Pen(img, kind, {}, {}, False, s)
        pen.middle()
        pen.dpad()
        pen.face()
        pen.sticks()
        _BASES[key] = img
    return _BASES[key]


def render(kind, buttons=None, axes=None, width=560):
    """The controller's picture `width` pixels wide: kind 'playstation', 'xbox' or 'switch'; buttons
    {name: pressed}, axes {name: -1..1, triggers 0..1} as bbport_pad gives them (None: at rest)."""
    kind = kind if kind in LAYOUTS else 'playstation'
    img = base(kind, width).copy()
    pen = _Pen(img, kind, buttons or {}, axes or {}, True, width * SS / W, _SHAPES[(kind, width)])
    pen.shoulders()
    pen.middle()
    pen.dpad()
    pen.face()
    pen.sticks()
    return img.reduce(SS)  # SS pixels into one: smooth edges, exactly `width` wide
