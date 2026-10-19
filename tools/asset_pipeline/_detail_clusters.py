import os, pickle
import cv2
import numpy as np
from PIL import Image, ImageDraw
import xr_level as X
from common import WORK
import seasons_lods as SLD

base = os.path.join(WORK, "seasons/da_levels/levels")


def sprites_of(lvl):
    lv = os.path.join(base, lvl)
    p = os.path.join(lv, "build_details.dds")
    if not os.path.isfile(p):
        return []
    img = np.asarray(Image.open(p).convert("RGBA"))
    H, W = img.shape[:2]
    out, seen = [], set()
    for flags, mn, mx, V in X.detail_objects(os.path.join(lv, "level.details")):
        u0, u1, v0, v1 = V[:, 3].min(), V[:, 3].max(), V[:, 4].min(), V[:, 4].max()
        r = (max(0, int(u0 * W)), max(0, int(v0 * H)), min(W, int(np.ceil(u1 * W))), min(H, int(np.ceil(v1 * H))))
        if r in seen or r[2] - r[0] < 4 or r[3] - r[1] < 4:
            continue
        seen.add(r)
        crop = img[r[1]:r[3], r[0]:r[2]]
        small = cv2.resize((crop[..., 3] > 127).astype(np.float32), (24, 48), interpolation=cv2.INTER_AREA)
        out.append({'level': lvl, 'rect': r, 'flat': bool(np.ptp(V[:, 1]) < 0.05), 'mask': small, 'crop': crop})
    return out


def corr(a, b):
    a = a.ravel() - a.mean()
    b = b.ravel() - b.mean()
    return float(a @ b / max(np.linalg.norm(a) * np.linalg.norm(b), 1e-6))


if __name__ == '__main__':
    sprites = [s for lvl in SLD.OUTDOOR for s in sprites_of(lvl)]
    print('sprites', len(sprites))
    clusters = []
    for s in sprites:
        best, bi = -1, -1
        for i, c in enumerate(clusters):
            r = corr(s['mask'], c['mask'])
            if r > best:
                best, bi = r, i
        if best > 0.9:
            clusters[bi]['members'].append(s)
        else:
            clusters.append({'mask': s['mask'], 'crop': s['crop'], 'flat': s['flat'], 'members': [s]})
    print('clusters', len(clusters))
    T, cols = 96, 16
    rows = (len(clusters) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * T, rows * (T * 2 + 18)), (150, 160, 170))
    d = ImageDraw.Draw(sheet)
    for i, c in enumerate(clusters):
        cr = c['crop'].astype(np.float32)
        a = cr[..., 3:4] / 255
        comp = (cr[..., :3] * a + np.array([150, 160, 170]) * (1 - a)).astype(np.uint8)
        im = Image.fromarray(comp).resize((T, T * 2))
        x, y = (i % cols) * T, (i // cols) * (T * 2 + 18)
        sheet.paste(im, (x, y + 18))
        d.text((x + 2, y + 2), f"{i}{' F' if c['flat'] else ''} n{len(c['members'])}", fill=(255, 255, 0))
    sheet.save(os.path.join(WORK, "ingame", "detail_clusters.png"))
    pickle.dump([{'mask': c['mask'], 'flat': c['flat'], 'n': len(c['members'])} for c in clusters], open(os.path.join(WORK, "detail_clusters.pkl"), "wb"))
