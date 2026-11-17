"""Builds the sheet of the round link pictures a module's card shows (docs/dead-air/MOD_UPDATES.md 1.5).

    python tools/graphics/make_xms_links_sheet.py [--preview out.png]

Writes:
    textures/ui/ui_xms_links.dds                   512x64 A8R8G8B8, no mips
    configs/ui/textures_descr/ui_xms_links.xml     the ids
both under packaging/dead-air-x64/compatibility/gamedata: the sheet is small and belongs to the window that draws
it, so it travels with the game's own files rather than with the content.

One round picture per kind of link, in the order of XMS::LinkKind, made of the services' own marks and nothing
drawn here: Telegram, AP-PRO and GitHub are the menu's community page's pictures (remote/icons); Discord, Boosty and
Patreon are their official marks in their brand colours, ModDB its own site icon (tools/graphics/link_icons, where
the README names each source). RGB runs to the edge of each disc so bilinear filtering never darkens the rim.
"""
import io
import os
import struct
import sys

import pymupdf as fitz
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT_ROOT = os.path.join(REPO, "packaging", "dead-air-x64", "compatibility", "gamedata")
ICONS = os.path.join(REPO, "remote", "icons")
MARKS = os.path.join(HERE, "link_icons")
SHEET_W, SHEET_H = 512, 64
CELL = 48      # a picture on the sheet
PITCH = 64     # from one picture to the next
PAD = (PITCH - CELL) // 2
W = 512        # working size of a picture

# XMS::LinkKind, in order; the key is the one [links] uses
KINDS = ["ap_pro", "moddb", "github", "telegram", "discord", "boosty", "patreon"]


def disc_mask(size):
    from PIL import ImageDraw
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).ellipse((0, 0, size - 1, size - 1), fill=255)
    return m


def from_file(folder, name):
    return Image.open(os.path.join(folder, name)).convert("RGBa").resize((W, W), Image.LANCZOS).convert("RGBA")


def mark(name, colour):
    """An official mark (a one-path SVG) in `colour`, at `W` pixels square."""
    svg = open(os.path.join(MARKS, name), encoding="utf-8").read().replace("<path ", '<path fill="%s" ' % colour, 1)
    doc = fitz.open(stream=svg.encode("utf-8"), filetype="svg")
    page = doc[0]
    pix = page.get_pixmap(matrix=fitz.Matrix(W / page.rect.width, W / page.rect.height), alpha=True)
    return Image.open(io.BytesIO(pix.tobytes("png"))).convert("RGBA").resize((W, W), Image.LANCZOS)


def on_disc(background, name, colour, scale):
    """The mark centred on a disc of `background`, `scale` of its width, as the services draw their app icons."""
    img = Image.new("RGBA", (W, W), background)
    side = int(W * scale)
    m = mark(name, colour).resize((side, side), Image.LANCZOS)
    img.alpha_composite(m, ((W - side) // 2, (W - side) // 2))
    return img


MAKERS = {
    "ap_pro": lambda: from_file(ICONS, "ap-pro.png"),
    # the site's own icon is a square: its middle, where the wrench is, makes the disc
    "moddb": lambda: from_file(MARKS, "moddb.png"),
    "github": lambda: from_file(ICONS, "github.png"),
    "telegram": lambda: from_file(ICONS, "telegram.png"),
    "discord": lambda: on_disc((0x58, 0x65, 0xF2, 255), "discord.svg", "#FFFFFF", 0.58),
    "boosty": lambda: on_disc((0xF1, 0x5F, 0x2C, 255), "boosty.svg", "#FFFFFF", 0.56),
    "patreon": lambda: on_disc((0xFF, 0xFF, 0xFF, 255), "patreon.svg", "#000000", 0.5),
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
