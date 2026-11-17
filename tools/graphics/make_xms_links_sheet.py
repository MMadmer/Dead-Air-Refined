"""Builds the sheet of the round link pictures a module's card shows (docs/dead-air/MOD_UPDATES.md 1.5).

    python tools/graphics/make_xms_links_sheet.py [--preview out.png]

Writes:
    textures/ui/ui_xms_links.dds                   512x64 A8R8G8B8, no mips
    configs/ui/textures_descr/ui_xms_links.xml     the ids
both under packaging/dead-air-x64/compatibility/gamedata: the sheet is small and belongs to the window that draws
it, so it travels with the game's own files rather than with the content.

One round picture per kind of link, in the order of XMS::LinkKind, the way the menu's community page shows
its own: Telegram, AP-PRO and GitHub are that page's pictures (remote/icons), the rest are drawn here in the
same manner - a disc in the service's colour with its mark in white. Drawn at 4x and filtered down; RGB runs
to the edge of each disc so bilinear filtering never darkens the rim.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT_ROOT = os.path.join(REPO, "packaging", "dead-air-x64", "compatibility", "gamedata")
ICONS = os.path.join(REPO, "remote", "icons")
SHEET_W, SHEET_H = 512, 64
CELL = 48      # a picture on the sheet
PITCH = 64     # from one picture to the next
PAD = (PITCH - CELL) // 2
W = 512        # working size of a drawn picture

# XMS::LinkKind, in order; the key is the one [links] uses
KINDS = ["ap_pro", "moddb", "github", "telegram", "discord", "boosty", "patreon"]


def disc_mask(size):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).ellipse((0, 0, size - 1, size - 1), fill=255)
    return m


def font(name, size):
    return ImageFont.truetype(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", name), size)


def centred(d, text, f, cy, fill, dx=0):
    box = d.textbbox((0, 0), text, font=f)
    d.text(((W - (box[2] - box[0])) / 2 - box[0] + dx, cy - (box[3] - box[1]) / 2 - box[1]), text, font=f, fill=fill)


def from_file(name):
    return Image.open(os.path.join(ICONS, name)).convert("RGBa").resize((W, W), Image.LANCZOS).convert("RGBA")


def moddb():
    img = Image.new("RGBA", (W, W), (176, 26, 26, 255))
    d = ImageDraw.Draw(img)
    centred(d, "DB", font("ariblk.ttf", 250), W * 0.5, (255, 255, 255, 255))
    return img


def discord():
    blurple = (88, 101, 242, 255)
    img = Image.new("RGBA", (W, W), blurple)
    mark = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(mark)
    u = W / 100.0
    body = [(27, 33), (40, 28), (43, 33), (57, 33), (60, 28), (73, 33), (84, 63), (71, 73), (66, 66), (34, 66), (29, 73),
            (16, 63)]
    d.polygon([(x * u, y * u) for x, y in body], fill=255)
    # the corners of the polygon rounded
    mark = mark.filter(ImageFilter.GaussianBlur(2.2 * u)).point(lambda v: 255 if v > 127 else 0)
    mark = mark.filter(ImageFilter.GaussianBlur(0.35 * u))
    d = ImageDraw.Draw(mark)
    for cx in (40, 60):
        d.ellipse(((cx - 6.3) * u, (51 - 7.3) * u, (cx + 6.3) * u, (51 + 7.3) * u), fill=0)
    img.paste((255, 255, 255, 255), (0, 0), mark)
    return img


def boosty():
    img = Image.new("RGBA", (W, W), (241, 95, 44, 255))
    d = ImageDraw.Draw(img)
    centred(d, "b", font("arialbi.ttf", 400), W * 0.47, (255, 255, 255, 255), dx=-W * 0.02)
    return img


def patreon():
    img = Image.new("RGBA", (W, W), (255, 66, 77, 255))
    d = ImageDraw.Draw(img)
    u = W / 100.0
    d.rectangle((25 * u, 24 * u, 36 * u, 77 * u), fill=(255, 255, 255, 255))
    d.ellipse((41 * u, 24 * u, 79 * u, 62 * u), fill=(255, 255, 255, 255))
    return img


MAKERS = {
    "ap_pro": lambda: from_file("ap-pro.png"),
    "moddb": moddb,
    "github": lambda: from_file("github.png"),
    "telegram": lambda: from_file("telegram.png"),
    "discord": discord,
    "boosty": boosty,
    "patreon": patreon,
}


def round_cell(img):
    """`img` at the sheet's size as a disc: its own alpha, and never past the disc's."""
    small = np.asarray(img.convert("RGBa").resize((CELL, CELL), Image.LANCZOS).convert("RGBA")).astype("float32")
    mask = np.asarray(disc_mask(CELL * 4).resize((CELL, CELL), Image.LANCZOS)).astype("float32")
    small[..., 3] = np.minimum(small[..., 3], mask)
    return small


def bleed(cell):
    """Colour under the transparent rim: the nearest opaque colour, so filtering finds no black there."""
    rgb = cell[..., :3].copy()
    alpha = cell[..., 3]
    known = alpha > 0
    for _ in range(8):
        total = np.zeros_like(rgb)
        count = np.zeros(alpha.shape, "float32")
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            near = np.roll(np.roll(known, dy, 0), dx, 1)
            total += np.roll(np.roll(rgb, dy, 0), dx, 1) * near[..., None]
            count += near
        grow = (~known) & (count > 0)
        rgb[grow] = total[grow] / count[grow][..., None]
        known |= grow
    return Image.fromarray(np.dstack([rgb, alpha]).clip(0, 255).astype("uint8"), "RGBA")


def build():
    sheet = Image.new("RGBA", (SHEET_W, SHEET_H), (0, 0, 0, 0))
    entries = []
    for i, kind in enumerate(KINDS):
        cell = bleed(round_cell(MAKERS[kind]()))
        x, y = PAD + PITCH * i, PAD
        sheet.paste(cell, (x, y))
        entries.append(("ui_xms_link_" + kind, x, y, CELL, CELL))
    return sheet, entries


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
             "    <!-- The link pictures of a module's card: generated by tools/graphics/make_xms_links_sheet.py, do not edit -->",
             '    <file name="ui\\ui_xms_links">']
    for name, x, y, w, h in entries:
        lines.append('        <texture id="%s" x="%d" y="%d" width="%d" height="%d" />' % (name, x, y, w, h))
    lines += ["    </file>", "</w>", ""]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="cp1251", newline="\r\n") as f:
        f.write("\n".join(lines))


def main():
    img, entries = build()
    write_dds(os.path.join(OUT_ROOT, "textures", "ui", "ui_xms_links.dds"), img)
    write_descr(os.path.join(OUT_ROOT, "configs", "ui", "textures_descr", "ui_xms_links.xml"), entries)
    if "--preview" in sys.argv:
        out = sys.argv[sys.argv.index("--preview") + 1]
        scale = 3
        bg = Image.new("RGBA", img.size, (28, 28, 28, 255))
        bg.alpha_composite(img)
        bg.resize((img.size[0] * scale, img.size[1] * scale), Image.LANCZOS).save(out)
    print("%d pictures on the sheet" % len(entries))


if __name__ == "__main__":
    main()
