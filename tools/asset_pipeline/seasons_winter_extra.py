"""More of winter, textures only:
  roofs       under snow, the seams and corrugation still reading through (90-100 % of every roof texture's
              area faces up on the levels, so a picture of snow is right there)
  bark        frost, and snow lodged in the deepest cracks
  cut faces   stumps and trunk caps under snow
  leaves      the fallen-leaf decals go under the snow (transparent)
  ivy         vine_rich drops its leaves (brown and sparse), vine_dried_up frosted
  bump maps   winter pairs for the snowed ground, the terrain detail layers and the roofs: the relief and the
              gloss of gravel, leaves and slate no longer show through the snow
Everything is encoded straight into the 1.6.0 tree under textures/seasons/winter/.
"""
import os
import sys
import tempfile

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bump  # noqa: E402
import seasons_ground as SG  # noqa: E402
from common import TREE16, WORK, install, load_rgba, orig_path, save_png, texconv  # noqa: E402

TMP = os.path.join(WORK, 'winter_extra_tmp')
ROOFS = ['roof_crete_01', 'roof_golima', 'roof_metal_01', 'roof_metal_01_holes', 'roof_schiffer_01',
         'roof_schiffer_01_redish_alternative', 'roof_schiffer_02', 'roof_smola', 'roof_smola_02']
# WP7: only the barks and cut faces stock models still wear (the procedural trees draw their own bark).
BARKS = ['trees_bark', 'trees_bark_a_01', 'trees_bark_a_02', 'trees_bark_mox', 'trees_bark_sosna_01', 'trees_bark_sosna_02',
         'trees_bark_sux', 'trees_bark_sux2', 'trees_bark_sux3', 'trees_bark_sux_mox', 'trees_bark_topol', 'trees_dub_bark']
CAPS = ['trees_spil']
LEAF_DECALS = ['decal_listja', 'decal_listja_vetki']
SNOWC = np.array([226, 231, 238], np.float32)


def base_of(rel):
    """What the 1.6.0 tree shows for a gamedata path: our file, else Dead Air's."""
    p = os.path.join(TREE16, rel)
    return p if os.path.isfile(p) else orig_path(rel)


def put(img, rel, src, cutout=False):
    fm, mips = SG.fmt(src)
    os.makedirs(TMP, exist_ok=True)
    work = tempfile.mkdtemp(dir=TMP)
    png = os.path.join(work, os.path.basename(rel)[:-4] + '.png')
    save_png(png, img)
    dds = texconv(png, work, fm, 0 if mips > 1 else 1, alpha_cutout=cutout)
    install(dds, f'textures/seasons/winter/{rel[len("textures/"):]}')
    print(rel, fm, flush=True)


def put_bump(rel_tex, cover):
    """Winter bump pair of a texture: normals flattened and gloss taken down as far as the snow covers it."""
    b = base_of(rel_tex[:-4] + '_bump.dds')
    if not b:
        return
    bx = base_of(rel_tex[:-4] + '_bump#.dds')
    n, gloss, height = bump.decode(b, bx)
    up = np.zeros_like(n)
    up[..., 2] = 1.0
    k = 0.85 * cover
    n = n * (1 - k) + up * k
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
    gloss = gloss * (1 - cover) + 0.04 * cover
    height = (height if height is not None else np.full(gloss.shape, 0.5, np.float32)) * (1 - 0.8 * cover) + 0.5 * 0.8 * cover
    os.makedirs(TMP, exist_ok=True)
    work = tempfile.mkdtemp(dir=TMP)
    name = os.path.basename(rel_tex)[:-4]
    ob, obx = os.path.join(work, name + '_bump.dds'), os.path.join(work, name + '_bump#.dds')
    bump.encode(n, gloss, height, ob, obx)
    sub = os.path.dirname(rel_tex)[len('textures/'):]
    install(ob, f'textures/seasons/winter/{sub}/{name}_bump.dds')
    install(obx, f'textures/seasons/winter/{sub}/{name}_bump#.dds')
    print(rel_tex[:-4] + '_bump pair', flush=True)


def frost_bark(img, seed):
    """Frost over bark, and snow in the deepest cracks (the darkest relief of the picture)."""
    rng = np.random.default_rng(seed)
    rgb = img[..., :3].astype(np.float32)
    lum = rgb.mean(2) / 255.0
    hp = lum - SG.wblur(lum, 4.0)
    hp /= max(float(hp.std()), 1e-4)
    crack = np.clip((-hp - 0.55) / 0.9, 0, 1)
    patchy = SG.wblur(rng.random(lum.shape).astype(np.float32), 6.0)
    patchy = (patchy - patchy.min()) / max(float(np.ptp(patchy)), 1e-4)
    crack *= np.clip(patchy * 1.6 - 0.2, 0, 1)
    grey = rgb.mean(2, keepdims=True)
    base = (rgb * 0.55 + grey * 0.45) * 1.08 + 14.0
    out = base * (1 - crack[..., None] * 0.9) + SNOWC * crack[..., None] * 0.9
    return np.dstack([np.clip(out + 0.5, 0, 255).astype(np.uint8), img[..., 3]])


def bare_ivy(img, seed):
    """Ivy in winter: the leaves dry to brown and most fall."""
    rng = np.random.default_rng(seed)
    rgb = img[..., :3].astype(np.float32)
    lab = cv2.cvtColor(np.clip(rgb, 0, 255).astype(np.uint8), cv2.COLOR_RGB2LAB).astype(np.float32)
    lab[..., 0] = lab[..., 0] * 0.85
    lab[..., 1] = 128 + (lab[..., 1] - 128) * 0.25 + 8
    lab[..., 2] = 128 + (lab[..., 2] - 128) * 0.25 + 14
    out = cv2.cvtColor(np.clip(lab + 0.5, 0, 255).astype(np.uint8), cv2.COLOR_LAB2RGB)
    H, W = img.shape[:2]
    blobs = cv2.resize(rng.random((max(4, H // 16), max(4, W // 16))).astype(np.float32), (W, H), interpolation=cv2.INTER_CUBIC)
    keep = blobs < 0.4
    a = np.where(keep, img[..., 3], 0).astype(np.uint8)
    return np.dstack([out, a])


def frosted(img):
    rgb = img[..., :3].astype(np.float32)
    grey = rgb.mean(2, keepdims=True)
    out = (rgb * 0.6 + grey * 0.4) * 1.05 + 10.0
    return np.dstack([np.clip(out + 0.5, 0, 255).astype(np.uint8), img[..., 3]])


def main():
    # 'roofs' and 'ground_bumps' are retired by WP7: the ground category (roofs included) never wears a season look
    what = sys.argv[1:] or ['barks', 'caps', 'decals', 'ivy']
    if 'roofs' in what:
        for k, n in enumerate(ROOFS):
            rel = f'textures/roof/{n}.dds'
            src = base_of(rel)
            if not src:
                continue
            # The corrugation, the seams and the slates have to read through: strong relief, ridges left bare
            cover = 0.62 if 'metal' in n else 0.72
            put(SG.snowy(load_rgba(src), cover, 100 + k, relief=0.2), rel, src)
            put_bump(rel, cover)
    if 'barks' in what:
        for k, n in enumerate(BARKS):
            rel = f'textures/trees/{n}.dds'
            src = base_of(rel)
            if src:
                put(frost_bark(load_rgba(src), 200 + k), rel, src)
    if 'caps' in what:
        for k, n in enumerate(CAPS):
            rel = f'textures/trees/{n}.dds'
            src = base_of(rel)
            if src:
                put(SG.snowy(load_rgba(src), 0.85, 300 + k), rel, src)
    if 'decals' in what:
        for n in LEAF_DECALS:
            rel = f'textures/decal/{n}.dds'
            src = base_of(rel)
            if src:
                img = load_rgba(src).copy()
                img[..., 3] = 0
                put(img, rel, src)
    if 'ivy' in what:
        for n, fn in (('vine_rich', 'bare'), ('vine_dried_up', 'frost')):
            rel = f'textures/vine/{n}.dds'
            src = base_of(rel)
            if src:
                img = load_rgba(src)
                put(bare_ivy(img, 400) if fn == 'bare' else frosted(img), rel, src, cutout=True)
    if 'ground_bumps' in what:
        import json
        plan = json.load(open(os.path.join(WORK, 'seasons_ground_out', 'plan.json')))
        for j in plan:
            rel = 'textures/' + j['rel']
            put_bump(rel, SG.cover_for(os.path.basename(rel)))


if __name__ == '__main__':
    main()
