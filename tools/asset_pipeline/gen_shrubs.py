"""[season_shrubs] for dead_air_x64_details.ltx: per level, the indices of the detail objects whose sprite
is a woody shrub (catalogue kind 'shrub', seasons_details.KIND) - they keep standing when the season
takes the grass away."""
import os
import numpy as np
from PIL import Image
import cv2
import seasons_details as SD
import xr_level as X


def shrubs(lvl):
    img = np.asarray(Image.open(os.path.join(SD.DA_LEVELS, lvl, 'build_details.dds')).convert('RGBA'))
    H, W = img.shape[:2]
    out = []
    for i, (flags, mn, mx, V) in enumerate(X.detail_objects(os.path.join(SD.DA_LEVELS, lvl, 'level.details'))):
        if np.ptp(V[:, 1]) < 0.05:
            continue
        u0, u1, v0, v1 = V[:, 3].min(), V[:, 3].max(), V[:, 4].min(), V[:, 4].max()
        r = (max(0, int(round(u0 * W))), max(0, int(round(v0 * H))), min(W, int(round(u1 * W))), min(H, int(round(v1 * H))))
        if r[2] - r[0] < 4 or r[3] - r[1] < 4:
            continue
        mask = cv2.resize((img[r[1]:r[3], r[0]:r[2], 3] > 127).astype(np.float32), (24, 48), interpolation=cv2.INTER_AREA)
        scores = [SD.corr(mask, c['mask']) for c in SD.CATALOGUE]
        best = int(np.argmax(scores))
        if scores[best] > 0.8 and SD.KIND.get(best) == 'shrub':
            out.append(i)
    return out


if __name__ == '__main__':
    # Retired: [season_shrubs] left dead_air_x64_details.ltx and the engine (1.6.0, 7891f9d23).
    raise SystemExit('gen_shrubs.py is retired: [season_shrubs] is gone')
    for lvl in SD.OUTDOOR:
        ids = shrubs(lvl)
        print(f"{lvl:<20} = {', '.join(map(str, ids))}" if ids else f"; {lvl}: no woody details")
