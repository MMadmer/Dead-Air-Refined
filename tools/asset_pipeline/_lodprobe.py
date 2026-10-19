import os, sys, time
import numpy as np
from PIL import Image
import xr_level as X
import lod_render as R
from common import WORK, TREE16, ORIG

LV = os.path.join(WORK, "seasons/da_levels/levels", sys.argv[1] if len(sys.argv) > 1 else "l01_escape")
CARDS = os.path.join(WORK, "seasons_out")

def resolver(look):
    def r(name):
        rel = name.replace("\\", "/")
        if look != "stock":
            if rel.startswith("trees/"):
                for lk in (look, "autumn"):
                    p = os.path.join(CARDS, lk, rel[6:] + ".png")
                    if os.path.isfile(p):
                        return p
            p = os.path.join(TREE16, "textures", rel + ".dds")
            if os.path.isfile(p):
                return p
        for o in ORIG:
            p = os.path.join(o, "textures", rel + ".dds")
            if os.path.isfile(p):
                return p
        return None
    return r

if __name__ != "__main__":
    raise SystemExit
L = X.Level(LV)
atlas = np.asarray(Image.open(os.path.join(LV, "level_lods.dds")).convert("RGBA"))
AH, AW = atlas.shape[:2]
objs = R.lod_objects(L, AW, AH)
veg = []
for v, cells in objs:
    ms = R.leaf_meshes(L, v)
    if any(m[3].lower().startswith("trees\\") for m in ms):
        veg.append((v, cells, ms))
print("objects", len(objs), "vegetation", len(veg))
looks = ["stock", "autumn", "winter", "spring_early", "spring_mid", "summer"]
texs = {lk: R.TexCache(resolver(lk)) for lk in looks}
pick = veg[:: max(1, len(veg) // 8)][:8]
rows = []
t = time.time()
for v, cells, ms in pick:
    x0, y0, cw, ch = cells[1]
    frags, W, H = R.fragments(ms, v["facets"][1], cw, ch)
    row = [atlas[y0:y0 + ch, x0:x0 + cw]]
    for lk in looks:
        rgb, mask = R.shade(frags, W, H, texs[lk])
        col, cov = R.downsample(rgb, mask, cw, ch)
        row.append(np.dstack([np.clip(col * 255, 0, 255).astype(np.uint8), (cov * 255).astype(np.uint8)]))
    rows.append(row)
print("time per object (1 view, 6 looks)", (time.time() - t) / len(pick))
def comp(c):
    rgb = c[..., :3].astype(np.float32); a = c[..., 3:4].astype(np.float32) / 255
    return np.clip(rgb * a + 140 * (1 - a), 0, 255).astype(np.uint8)
sheet = np.vstack([np.hstack([comp(c) for c in row]) for row in rows])
Image.fromarray(sheet).resize((sheet.shape[1] * 5, sheet.shape[0] * 5), Image.NEAREST).save(os.path.join(WORK, "ingame", "lod_looks_probe.png"))
