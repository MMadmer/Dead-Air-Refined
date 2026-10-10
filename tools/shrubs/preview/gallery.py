# Copyright (c) 2026 XFined-Ray
# Released under the MIT licence, see License.txt
"""A contact sheet of the shrub species as the engine's generator grows them (tools/shrubs/preview).

    python gallery.py <shrubs.ltx> <out_dir> [species ...] [--exe shrub_preview.exe] [--month 7] [--lod 0]

For every species two bushes (two thirds of its height range and its top) go through shrub_preview.exe into an
OBJ; one headless Blender session renders them all (render_preview.py's scene) in the species' leaf colour of the
month; the sheet is <out_dir>/gallery.png.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def read_ltx(path):
    sections, cur = {}, None
    for raw in open(path, encoding='cp1251'):
        line = raw.split(';', 1)[0].strip()
        if not line:
            continue
        if line.startswith('['):
            cur = line[1:line.index(']')]
            sections[cur] = {}
        elif '=' in line and cur:
            k, v = line.split('=', 1)
            sections[cur][k.strip()] = v.strip()
    return sections


def curve_colour(value, day):
    """The colour of a 'day:#hex, ...' key list at the day (nearest key before it, wrapping)."""
    pts = []
    for item in value.split(','):
        if ':' not in item:
            continue
        d, h = item.split(':', 1)
        pts.append((float(d), h.strip()))
    pts.sort()
    if not pts:
        return '#5E7E3A'
    best = pts[-1][1]
    for d, h in pts:
        if d <= day:
            best = h
    return best


def lin(h):
    h = h.lstrip('#')
    c = [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    return ','.join('%.4f' % (x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4) for x in c)


def main():
    args = sys.argv[1:]
    ltx, out = args[0], args[1]
    exe = os.path.join(HERE, 'bin', 'shrub_preview.exe')
    month, lod, only = 7, 0, []
    i = 2
    while i < len(args):
        if args[i] == '--exe':
            exe = args[i + 1]
            i += 2
        elif args[i] == '--month':
            month = int(args[i + 1])
            i += 2
        elif args[i] == '--lod':
            lod = int(args[i + 1])
            i += 2
        else:
            only.append(args[i])
            i += 1
    os.makedirs(out, exist_ok=True)
    day = [14.5, 45.5, 73.5, 104.5, 134.5, 165.5, 195.5, 226.5, 257.5, 287.5, 318.5, 348.5][month - 1]
    secs = read_ltx(ltx)
    jobs = []
    for name, s in secs.items():
        if not name.startswith('shrub_species_'):
            continue
        sid = s['name']
        if only and sid not in only:
            continue
        h0, h1 = [float(x) for x in s['height'].split(',')]
        w0, w1 = [float(x) for x in s['width'].split(',')]
        sizes = []
        for h in (h0 + (h1 - h0) * 0.66, h1):
            sizes.append('%.2f,%.2f' % (h, h * 0.5 * (w0 + w1)))
        obj = os.path.join(out, sid + '.obj')
        log = subprocess.run([exe, ltx, sid, obj] + sizes + ['--lod', str(lod)], capture_output=True, text=True)
        stats = log.stdout.strip().replace('\n', ' | ')
        leaf = curve_colour(s.get('c_summer', ''), day)
        bloom = s.get('c_petal', '0:#E8E6DA').split(':', 1)[1]
        jobs.append(dict(id=sid, obj=obj, png=os.path.join(out, sid + '.png'), leaf=lin(leaf), bloom=lin(bloom),
                         wood=lin(curve_colour(s.get('c_bark', ''), 15)), stats=stats))
        print(sid, stats)
    job_file = os.path.join(out, 'jobs.json')
    json.dump(jobs, open(job_file, 'w'), indent=1)
    blender = os.environ.get('BLENDER_PATH', 'blender')
    subprocess.run([blender, '-b', '--factory-startup', '--python', os.path.join(HERE, 'render_gallery.py'), '--', job_file],
                   check=False, capture_output=True)
    sheet(jobs, os.path.join(out, 'gallery.png'))


def sheet(jobs, path):
    from PIL import Image, ImageDraw
    tiles = [(j['id'], Image.open(j['png']).convert('RGB')) for j in jobs if os.path.exists(j['png'])]
    if not tiles:
        return
    w, h = 600, 300
    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    img = Image.new('RGB', (cols * w, rows * h), (30, 30, 30))
    d = ImageDraw.Draw(img)
    for n, (sid, t) in enumerate(tiles):
        x, y = (n % cols) * w, (n // cols) * h
        img.paste(t.resize((w, h)), (x, y))
        d.text((x + 8, y + 6), sid, fill=(255, 255, 255))
    img.save(path)
    print('sheet', path)


if __name__ == '__main__':
    main()
