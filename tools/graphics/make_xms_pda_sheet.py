"""Builds the texture sheet of XMS PDA layouts (docs/dead-air/MODDING.md, "PDA layouts").

    python tools/graphics/make_xms_pda_sheet.py [--preview out.png]

Writes:
    textures/ui/ui_xms_pda.dds                   512x512 A8R8G8B8, no mips (asset source, Dead-Air-Refined_Assets)
    configs/ui/textures_descr/ui_xms_pda.xml     the ids (packaging/dead-air-x64/compatibility/gamedata)

The sheet holds a white block (every rectangle a layout draws samples its middle and is
tinted by the vertex colour), a kit of white icons a layout tints with icon_color, and four
fades. Everything is white with alpha, so any colour works; RGB stays white where alpha is 0
so bilinear filtering never darkens an edge. Drawn at 4x and filtered down.
"""
import math
import os
import struct
import sys

from PIL import Image, ImageChops, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT_ROOT = os.path.join(REPO, "packaging", "dead-air-x64", "compatibility", "gamedata")
# The sheet itself is content: it goes to the asset source (Dead-Air-Refined_Assets), the ids stay in the overlay.
ASSETS = os.environ.get("DAR_ASSETS_GAMEDATA", os.path.join(REPO, "..", "..", "Dead-Air-Refined_Assets", "source", "gamedata"))
SHEET = 512
CELL = 64
SS = 4  # supersampling
W = CELL * SS  # working size of one icon

ICONS = {}


def icon(name):
    def reg(fn):
        ICONS[name] = fn
        return fn
    return reg


def canvas():
    return Image.new("L", (W, W), 0)


def star_points(cx, cy, outer, inner, n=5, rot=-90.0):
    pts = []
    for i in range(n * 2):
        r = outer if i % 2 == 0 else inner
        a = math.radians(rot + i * 180.0 / n)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def ring(d, box, width):
    d.ellipse(box, fill=255)
    x1, y1, x2, y2 = box
    d.ellipse((x1 + width, y1 + width, x2 - width, y2 - width), fill=0)


def stroke(d, pts, width):
    d.line(pts, fill=255, width=width, joint="curve")
    r = width / 2.0
    for x, y in (pts[0], pts[-1]):
        d.ellipse((x - r, y - r, x + r, y + r), fill=255)


@icon("star")
def _star(img, d):
    d.polygon(star_points(128, 136, 112, 46), fill=255)


def star_outline(img, d):
    d.polygon(star_points(128, 136, 112, 46), fill=255)
    d.polygon(star_points(128, 136, 80, 30), fill=0)


@icon("star_empty")
def _star_empty(img, d):
    star_outline(img, d)


@icon("star_half")
def _star_half(img, d):
    star_outline(img, d)
    full = canvas()
    ImageDraw.Draw(full).polygon(star_points(128, 136, 112, 46), fill=255)
    left = canvas()
    ImageDraw.Draw(left).rectangle((0, 0, 127, W), fill=255)
    img.paste(ImageChops.lighter(img, ImageChops.multiply(full, left)))


@icon("circle")
def _circle(img, d):
    d.ellipse((32, 32, 224, 224), fill=255)


@icon("ring")
def _ring(img, d):
    ring(d, (32, 32, 224, 224), 22)


@icon("dot")
def _dot(img, d):
    d.ellipse((88, 88, 168, 168), fill=255)


@icon("pin")
def _pin(img, d):
    d.ellipse((52, 24, 204, 176), fill=255)
    d.polygon([(62, 128), (194, 128), (128, 236)], fill=255)
    d.ellipse((96, 68, 160, 132), fill=0)


@icon("box")
def _box(img, d):
    top, left, right, bottom, mid = (128, 28), (36, 76), (220, 76), (128, 228), (128, 124)
    stroke(d, [top, right, (220, 180), bottom, (36, 180), left, top], 18)
    stroke(d, [left, mid, right], 18)
    stroke(d, [mid, bottom], 18)


@icon("bag")
def _bag(img, d):
    d.rounded_rectangle((36, 84, 220, 228), radius=28, fill=255)
    d.rounded_rectangle((58, 106, 198, 206), radius=14, fill=0)
    d.rectangle((58, 140, 198, 156), fill=255)
    stroke(d, [(88, 90), (88, 58), (104, 38), (152, 38), (168, 58), (168, 90)], 18)


@icon("map")
def _map(img, d):
    pts = [(32, 64), (96, 36), (160, 64), (224, 36), (224, 196), (160, 224), (96, 196), (32, 224)]
    d.polygon(pts, fill=255)
    inner = [(52, 76), (88, 60), (88, 184), (52, 200)]
    d.polygon(inner, fill=0)
    d.polygon([(108, 60), (148, 78), (148, 202), (108, 184)], fill=0)
    d.polygon([(168, 78), (204, 60), (204, 184), (168, 202)], fill=0)


@icon("list")
def _list(img, d):
    for y in (68, 128, 188):
        d.ellipse((36, y - 14, 64, y + 14), fill=255)
        d.rounded_rectangle((88, y - 11, 224, y + 11), radius=11, fill=255)


@icon("chat")
def _chat(img, d):
    d.rounded_rectangle((28, 40, 228, 180), radius=40, fill=255)
    d.polygon([(64, 170), (52, 232), (124, 172)], fill=255)


@icon("user")
def _user(img, d):
    d.ellipse((80, 28, 176, 124), fill=255)
    d.pieslice((40, 140, 216, 316), 180, 360, fill=255)


@icon("clock")
def _clock(img, d):
    ring(d, (28, 28, 228, 228), 20)
    stroke(d, [(128, 128), (128, 68)], 20)
    stroke(d, [(128, 128), (174, 154)], 20)


@icon("check")
def _check(img, d):
    stroke(d, [(52, 132), (104, 184), (206, 74)], 28)


@icon("cross")
def _cross(img, d):
    stroke(d, [(64, 64), (192, 192)], 28)
    stroke(d, [(192, 64), (64, 192)], 28)


@icon("plus")
def _plus(img, d):
    stroke(d, [(128, 48), (128, 208)], 28)
    stroke(d, [(48, 128), (208, 128)], 28)


@icon("minus")
def _minus(img, d):
    stroke(d, [(48, 128), (208, 128)], 28)


@icon("chevron_left")
def _cl(img, d):
    stroke(d, [(160, 48), (84, 128), (160, 208)], 28)


@icon("chevron_right")
def _cr(img, d):
    stroke(d, [(96, 48), (172, 128), (96, 208)], 28)


@icon("chevron_up")
def _cu(img, d):
    stroke(d, [(48, 160), (128, 84), (208, 160)], 28)


@icon("chevron_down")
def _cd(img, d):
    stroke(d, [(48, 96), (128, 172), (208, 96)], 28)


@icon("arrow_left")
def _al(img, d):
    stroke(d, [(212, 128), (52, 128)], 26)
    stroke(d, [(116, 64), (52, 128), (116, 192)], 26)


@icon("arrow_right")
def _ar(img, d):
    stroke(d, [(44, 128), (204, 128)], 26)
    stroke(d, [(140, 64), (204, 128), (140, 192)], 26)


@icon("menu")
def _menu(img, d):
    for y in (68, 128, 188):
        d.rounded_rectangle((36, y - 13, 220, y + 13), radius=13, fill=255)


@icon("dots")
def _dots(img, d):
    for x in (56, 128, 200):
        d.ellipse((x - 22, 106, x + 22, 150), fill=255)


@icon("search")
def _search(img, d):
    ring(d, (32, 32, 168, 168), 22)
    stroke(d, [(152, 152), (216, 216)], 30)


@icon("bell")
def _bell(img, d):
    d.pieslice((52, 36, 204, 188), 180, 360, fill=255)
    d.polygon([(52, 112), (204, 112), (224, 188), (32, 188)], fill=255)
    d.ellipse((104, 196, 152, 236), fill=255)
    d.ellipse((116, 18, 140, 42), fill=255)


@icon("home")
def _home(img, d):
    d.polygon([(128, 28), (232, 124), (24, 124)], fill=255)
    d.rectangle((52, 116, 204, 228), fill=255)
    d.rectangle((104, 156, 152, 228), fill=0)


@icon("settings")
def _settings(img, d):
    for i in range(8):
        a = math.radians(i * 45)
        cx, cy = 128 + 88 * math.cos(a), 128 + 88 * math.sin(a)
        pts = []
        for dx, dy in ((-22, -24), (22, -24), (22, 24), (-22, 24)):
            rx = dx * math.cos(a + math.pi / 2) - dy * math.sin(a + math.pi / 2)
            ry = dx * math.sin(a + math.pi / 2) + dy * math.cos(a + math.pi / 2)
            pts.append((cx + rx, cy + ry))
        d.polygon(pts, fill=255)
    d.ellipse((50, 50, 206, 206), fill=255)
    d.ellipse((96, 96, 160, 160), fill=0)


@icon("info")
def _info(img, d):
    ring(d, (28, 28, 228, 228), 20)
    d.ellipse((114, 64, 142, 92), fill=255)
    d.rounded_rectangle((115, 108, 141, 192), radius=10, fill=255)


@icon("warning")
def _warning(img, d):
    d.polygon([(128, 24), (236, 220), (20, 220)], fill=255)
    d.polygon([(128, 70), (196, 196), (60, 196)], fill=0)
    d.rounded_rectangle((117, 104, 139, 160), radius=8, fill=255)
    d.ellipse((115, 170, 141, 194), fill=255)


@icon("coin")
def _coin(img, d):
    ring(d, (28, 28, 228, 228), 18)
    ring(d, (64, 64, 192, 192), 16)


@icon("route")
def _route(img, d):
    d.ellipse((24, 172, 84, 232), fill=255)
    for (x, y) in ((100, 190), (128, 160), (140, 124), (164, 100)):
        d.ellipse((x - 10, y - 10, x + 10, y + 10), fill=255)
    d.ellipse((156, 20, 236, 100), fill=255)
    d.ellipse((182, 46, 210, 74), fill=0)


@icon("phone")
def _phone(img, d):
    d.rounded_rectangle((68, 20, 188, 236), radius=24, fill=255)
    d.rounded_rectangle((84, 48, 172, 196), radius=6, fill=0)
    d.ellipse((116, 204, 140, 228), fill=0)


@icon("lock")
def _lock(img, d):
    d.arc((76, 24, 180, 128), 180, 360, fill=255, width=22)
    d.rectangle((76, 72, 98, 116), fill=255)
    d.rectangle((158, 72, 180, 116), fill=255)
    d.rounded_rectangle((44, 108, 212, 232), radius=18, fill=255)
    d.ellipse((114, 146, 142, 174), fill=0)
    d.rectangle((122, 160, 134, 200), fill=0)


@icon("heart")
def _heart(img, d):
    d.ellipse((28, 44, 136, 152), fill=255)
    d.ellipse((120, 44, 228, 152), fill=255)
    d.polygon([(36, 122), (220, 122), (128, 226)], fill=255)


@icon("flag")
def _flag(img, d):
    d.rounded_rectangle((44, 24, 66, 236), radius=10, fill=255)
    d.polygon([(66, 32), (220, 32), (184, 88), (220, 144), (66, 144)], fill=255)


@icon("target")
def _target(img, d):
    ring(d, (36, 36, 220, 220), 18)
    d.ellipse((104, 104, 152, 152), fill=255)
    for a, b in (((128, 8), (128, 64)), ((128, 192), (128, 248)), ((8, 128), (64, 128)), ((192, 128), (248, 128))):
        stroke(d, [a, b], 18)


@icon("battery")
def _battery(img, d):
    d.rounded_rectangle((20, 72, 212, 184), radius=16, fill=255)
    d.rounded_rectangle((38, 90, 194, 166), radius=6, fill=0)
    d.rounded_rectangle((212, 104, 236, 152), radius=6, fill=255)
    for x in (50, 96, 142):
        d.rectangle((x, 100, x + 36, 156), fill=255)


@icon("signal")
def _signal(img, d):
    for i, h in enumerate((56, 104, 152, 200)):
        x = 36 + i * 50
        d.rounded_rectangle((x, 228 - h, x + 34, 228), radius=8, fill=255)


@icon("eye")
def _eye(img, d):
    d.ellipse((16, 64, 240, 192), fill=255)
    d.ellipse((40, 84, 216, 172), fill=0)
    d.ellipse((92, 92, 164, 164), fill=255)


@icon("camera")
def _camera(img, d):
    d.rounded_rectangle((20, 68, 236, 212), radius=22, fill=255)
    d.polygon([(84, 70), (104, 40), (152, 40), (172, 70)], fill=255)
    d.ellipse((80, 92, 176, 188), fill=0)
    d.ellipse((104, 116, 152, 164), fill=255)


@icon("skull")
def _skull(img, d):
    d.ellipse((40, 24, 216, 184), fill=255)
    d.rounded_rectangle((80, 150, 176, 228), radius=14, fill=255)
    d.ellipse((72, 88, 116, 132), fill=0)
    d.ellipse((140, 88, 184, 132), fill=0)
    d.polygon([(128, 136), (116, 160), (140, 160)], fill=0)
    for x in (104, 126, 148):
        d.rectangle((x, 196, x + 6, 228), fill=0)


# fades: alpha from 1 to 0 across the cell, for gradient headers and shadows under a bar
FADES = {
    "fade_down": lambda x, y: 1.0 - y,
    "fade_up": lambda x, y: y,
    "fade_right": lambda x, y: 1.0 - x,
    "fade_left": lambda x, y: x,
}


def render_icon(fn):
    img = canvas()
    fn(img, ImageDraw.Draw(img))
    return img.resize((CELL - 8, CELL - 8), Image.LANCZOS)


def build():
    alpha = Image.new("L", (SHEET, SHEET), 0)
    entries = [("ui_xms_pda_white", 0, 0, 16, 16, "white block for fills")]
    # the white block: 16x16 solid in the corner (fills sample its middle, 8/512)
    alpha.paste(255, (0, 0, 16, 16))

    cell = 1  # cell 0 holds the white block
    for name, fn in ICONS.items():
        cx, cy = (cell % 8) * CELL, (cell // 8) * CELL
        alpha.paste(render_icon(fn), (cx + 4, cy + 4))
        entries.append(("ui_xms_icon_" + name, cx + 4, cy + 4, CELL - 8, CELL - 8, None))
        cell += 1
    if cell > 56:
        raise SystemExit("too many icons for the sheet")

    # the last row: fades, one cell each (a 1px margin keeps the neighbours out of the filter)
    for i, (name, f) in enumerate(FADES.items()):
        cx, cy = i * CELL, 7 * CELL
        g = Image.new("L", (CELL - 2, CELL - 2))
        px = g.load()
        n = CELL - 3
        for y in range(CELL - 2):
            for x in range(CELL - 2):
                px[x, y] = int(round(255 * max(0.0, min(1.0, f(x / n, y / n)))))
        alpha.paste(g, (cx + 1, cy + 1))
        entries.append(("ui_xms_pda_" + name, cx + 1, cy + 1, CELL - 2, CELL - 2, None))

    rgba = Image.merge("RGBA", (Image.new("L", alpha.size, 255),) * 3 + (alpha,))
    return rgba, entries


def write_dds(path, img):
    w, h = img.size
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000  # caps, height, width, pitch, pixelformat
    header = struct.pack("<4s7I", b"DDS ", 124, flags, h, w, w * 4, 0, 1)
    header += b"\0" * 44
    # DDPF_RGB | DDPF_ALPHAPIXELS, 32 bpp, A8R8G8B8
    header += struct.pack("<8I", 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    r, g, b, a = img.split()
    bgra = Image.merge("RGBA", (b, g, r, a)).tobytes()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header)
        f.write(bgra)


def write_descr(path, entries):
    lines = ['<?xml version="1.0" encoding="windows-1251"?>', "<w>",
             "    <!-- XMS PDA layouts: generated by tools/graphics/make_xms_pda_sheet.py, do not edit -->",
             '    <file name="ui\\ui_xms_pda">']
    for name, x, y, w, h, _ in entries:
        lines.append('        <texture id="%s" x="%d" y="%d" width="%d" height="%d" />' % (name, x, y, w, h))
    lines += ["    </file>", "</w>", ""]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="cp1251", newline="\r\n") as f:
        f.write("\n".join(lines))


def main():
    img, entries = build()
    write_dds(os.path.join(ASSETS, "textures", "ui", "ui_xms_pda.dds"), img)
    write_descr(os.path.join(OUT_ROOT, "configs", "ui", "textures_descr", "ui_xms_pda.xml"), entries)
    if "--preview" in sys.argv:
        out = sys.argv[sys.argv.index("--preview") + 1]
        bg = Image.new("RGBA", img.size, (40, 44, 52, 255))
        bg.alpha_composite(img)
        bg.save(out)
    print("%d textures on the sheet" % len(entries))


if __name__ == "__main__":
    main()
