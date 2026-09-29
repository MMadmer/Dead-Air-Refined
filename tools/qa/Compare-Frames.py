#!/usr/bin/env python3
"""Frame parity for the stand's lossless shots (PLAN.md 0.6, vrs.md 9.3).

Two or more reference frames of one configuration (ref_a, ref_b, --ref ...) give the noise floor:
whatever differs between any two of them (NPCs, particles, water, the HUD's idle motion) is motion,
not a rendering change. More references catch more of the run-to-run variation. A new frame is compared with
a reference; a pixel counts as different only where |new - ref| > max(level, noise), the noise map
dilated by 2 px, and the metrics are taken on the static pixels (outside the dilated moving mask).

    python tools/qa/Compare-Frames.py REF_A REF_B              # the noise floor of the pair
    python tools/qa/Compare-Frames.py REF_A REF_B NEW          # "equal" (default thresholds)
    python tools/qa/Compare-Frames.py REF_A REF_B NEW --ref REF_C   # floor from three references
    python tools/qa/Compare-Frames.py REF_A REF_B NEW --no-equal --min-psnr 45 --heatmap d.png
    python tools/qa/Compare-Frames.py REF_A REF_B NEW --against b --min-psnr 40 --max-p999 12 \\
        --max-tile 4 --max-blockiness 5 --rect weapon=0.55,0.55,1,1 --min-rect-psnr 48

Arguments are image files (TGA, PNG, JPEG) or run folders (the --pick-th *.tga, sorted by name).
"equal" = at least --min-within % (99.9) of the static pixels within --level/255 (2) and no cluster
of differing pixels larger than --max-cluster px (16). With three or more references "equal" is
calibrated instead: every reference is compared, leave-one-out, against a floor made of the others,
and the new frame, put through the same trials, must not look worse than the worst reference does
(within % - 0.1, clusters over --max-cluster + max(3, half), largest cluster x2 or +64 px) - the
run-to-run variation that a floor made of a few runs cannot hold. Exit code 0 PASS, 1 FAIL, 2 bad input.
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image


def die(message):
    print(message, file=sys.stderr)
    sys.exit(2)


def load(path, pick):
    if not os.path.exists(path):
        die(f"{path}: not found")
    if os.path.isdir(path):
        for ext in (".tga", ".png", ".jpg"):
            files = sorted(f for f in os.listdir(path) if f.lower().endswith(ext))
            if files:
                if pick >= len(files):
                    die(f"{path}: only {len(files)} {ext} file(s), --pick {pick}")
                path = os.path.join(path, files[pick])
                break
        else:
            die(f"{path}: no image in the folder")
    with Image.open(path) as img:
        return path, np.asarray(img.convert("RGB"), dtype=np.int16)


def max_filter(a, r):
    """Separable (2r+1)^2 maximum filter."""
    out = a.copy()
    for axis in (0, 1):
        src = out.copy()
        for d in range(1, r + 1):
            if axis == 0:
                np.maximum(out[d:], src[:-d], out=out[d:])
                np.maximum(out[:-d], src[d:], out=out[:-d])
            else:
                np.maximum(out[:, d:], src[:, :-d], out=out[:, d:])
                np.maximum(out[:, :-d], src[:, d:], out=out[:, :-d])
    return out


def clusters(mask):
    """Sizes of the 8-connected components of a boolean mask (union-find over row runs)."""
    h, w = mask.shape
    if not mask.any():
        return np.zeros(0, dtype=np.int64)
    padded = np.zeros((h, w + 1), dtype=np.int8)
    padded[:, :w] = mask
    flat = np.concatenate(([0], padded.ravel()))
    edge = np.diff(flat)
    starts = np.nonzero(edge == 1)[0]
    ends = np.nonzero(edge == -1)[0]
    rows = starts // (w + 1)
    s = starts % (w + 1)
    e = ends - rows * (w + 1)
    n = len(starts)
    parent = list(range(n))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    # Runs of consecutive rows touch (8-connected) when [s1, e1] and [s2, e2] overlap by +-1.
    row_first = {}
    for i, r in enumerate(rows.tolist()):
        row_first.setdefault(r, i)
    bounds = sorted(row_first.items())
    for k in range(len(bounds) - 1):
        r, i0 = bounds[k]
        r2, j0 = bounds[k + 1]
        if r2 != r + 1:
            continue
        i1 = j0
        j1 = bounds[k + 2][1] if k + 2 < len(bounds) else n
        i, j = i0, j0
        while i < i1 and j < j1:
            if s[j] <= e[i] and s[i] <= e[j]:
                a, b = find(i), find(j)
                if a != b:
                    parent[b] = a
            if e[i] < e[j]:
                i += 1
            else:
                j += 1
    roots = np.array([find(i) for i in range(n)])
    return np.bincount(roots, weights=(e - s)).astype(np.int64)[np.unique(roots)]


def floor_of(frames, lv, dilate):
    """Noise map (largest difference of any two frames), dilated moving mask, per-pixel floor."""
    noise = np.zeros(frames[0].shape[:2], dtype=np.int16)
    for i in range(len(frames)):
        for j in range(i + 1, len(frames)):
            np.maximum(noise, np.abs(frames[i] - frames[j]).max(axis=2), out=noise)
    moving_dil = max_filter((noise > lv).astype(np.uint8), dilate).astype(bool)
    return noise, moving_dil, np.maximum(max_filter(noise, dilate), lv)


def equal_trials(sample_of, frames, lv, dilate, max_cluster):
    """Worst (within %, clusters over max, largest cluster) of sample_of(k) against the floor of the
    frames without k."""
    worst_within, worst_clusters, worst_largest = 100.0, 0, 0
    for k in range(len(frames)):
        others = frames[:k] + frames[k + 1:]
        _, moving_dil, fl = floor_of(others, lv, dilate)
        d = np.abs(sample_of(k) - others[0]).max(axis=2)
        ds = d[~moving_dil]
        within = 100.0 * float((ds <= lv).mean()) if ds.size else 100.0
        sizes = clusters(d > fl)
        worst_within = min(worst_within, within)
        worst_clusters = max(worst_clusters, int((sizes > max_cluster).sum()))
        worst_largest = max(worst_largest, int(sizes.max()) if sizes.size else 0)
    return worst_within, worst_clusters, worst_largest


def psnr(x, y, mask):
    if not mask.any():
        return float("nan")
    diff = (x[mask] - y[mask]).astype(np.float64) / 255.0
    mse = float(np.mean(diff * diff))
    return math.inf if mse == 0 else 10.0 * math.log10(1.0 / mse)


def luma(img):
    return img[..., 0] * 0.299 + img[..., 1] * 0.587 + img[..., 2] * 0.114


def blockiness(img, static):
    """Gradient energy across even pixel borders (2x2 block edges) over the one inside the blocks."""
    y = luma(img.astype(np.float64))
    gx = np.diff(y, axis=1) ** 2
    gy = np.diff(y, axis=0) ** 2
    mx = static[:, 1:] & static[:, :-1]
    my = static[1:, :] & static[:-1, :]
    # gx[:, i] is the border between x = i and i + 1: a block edge when i is odd.
    edge = gx[:, 1::2][mx[:, 1::2]].sum() + gy[1::2, :][my[1::2, :]].sum()
    inner = gx[:, 0::2][mx[:, 0::2]].sum() + gy[0::2, :][my[0::2, :]].sum()
    return edge / inner if inner > 0 else float("nan")


def worst_tile(d, static, size=16):
    h, w = d.shape
    th, tw = h // size, w // size
    dd = (d[: th * size, : tw * size] * static[: th * size, : tw * size]).reshape(th, size, tw, size).sum(axis=(1, 3))
    cnt = static[: th * size, : tw * size].reshape(th, size, tw, size).sum(axis=(1, 3))
    means = np.where(cnt >= size * size // 2, dd / np.maximum(cnt, 1), -1.0)
    k = int(np.argmax(means))
    return float(means.flat[k]), (k % tw * size, k // tw * size)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref_a")
    ap.add_argument("ref_b")
    ap.add_argument("new", nargs="?")
    ap.add_argument("--ref", action="append", default=[], help="a further reference of the same configuration")
    ap.add_argument("--against", choices=("a", "b"), default="a", help="reference the new frame is compared with")
    ap.add_argument("--pick", type=int, default=0, help="index of the shot inside a run folder")
    ap.add_argument("--level", type=int, default=2, help="per-channel difference (of 255) that counts as equal")
    ap.add_argument("--dilate", type=int, default=2, help="px the moving mask and the noise map grow by")
    ap.add_argument("--no-equal", action="store_true", help="drop the default 'equal' criteria")
    ap.add_argument("--min-within", type=float, default=99.9, help="%% of static pixels within --level")
    ap.add_argument("--max-cluster", type=int, default=16, help="largest allowed cluster beyond the floor (px)")
    ap.add_argument("--min-psnr", type=float)
    ap.add_argument("--max-p999", type=float, help="99.9th percentile of the static error, in /255")
    ap.add_argument("--max-tile", type=float, help="worst 16x16 tile mean error, in /255")
    ap.add_argument("--max-blockiness", type=float, help="blockiness increase over the reference, %%")
    ap.add_argument("--rect", action="append", default=[], help="name=x0,y0,x1,y1 in fractions of the frame")
    ap.add_argument("--min-rect-psnr", type=float)
    ap.add_argument("--heatmap", help="PNG: differences beyond the floor in red, moving pixels in blue")
    ap.add_argument("--json", help="write the metrics here")
    args = ap.parse_intermixed_args()

    pa, a = load(args.ref_a, args.pick)
    pb, b = load(args.ref_b, args.pick)
    refs = [(pa, a), (pb, b)] + [load(r, args.pick) for r in args.ref]
    for pr, r in refs[1:]:
        if r.shape != a.shape:
            print(f"size mismatch: {pa} {a.shape[1]}x{a.shape[0]} vs {pr} {r.shape[1]}x{r.shape[0]}")
            return 2
    lv = args.level
    # The floor is the largest difference between any two references.
    noise, moving_dil, floor = floor_of([r for _, r in refs], lv, args.dilate)
    moving = noise > lv
    static = ~moving_dil
    total = noise.size
    out = {
        "ref_a": pa, "ref_b": pb, "refs": [p for p, _ in refs[2:]], "size": [int(a.shape[1]), int(a.shape[0])],
        "noise": {
            "moving_pct": 100.0 * moving.sum() / total,
            "moving_dilated_pct": 100.0 * moving_dil.sum() / total,
            "max": int(noise.max()),
            "p999": float(np.percentile(noise, 99.9)),
            "psnr": psnr(a, b, np.ones_like(static)),
        },
    }
    n = out["noise"]
    names = " vs ".join(os.path.basename(os.path.dirname(pr)) or pr for pr, _ in refs)
    print(f"noise floor {names}: "
          f"moving {n['moving_pct']:.3f} % (dilated {n['moving_dilated_pct']:.3f} %), max {n['max']}/255, "
          f"p99.9 {n['p999']:.1f}/255, PSNR {n['psnr']:.2f} dB")

    if not args.new:
        if args.heatmap:
            base = (luma(a.astype(np.float32)) * 0.35).astype(np.uint8)
            heat = np.stack([base, base, base], axis=2)
            heat[moving_dil, 2] = np.maximum(heat[moving_dil, 2], 110)
            red = np.clip(noise.astype(np.int32) * 16, 64, 255).astype(np.uint8)
            heat[moving] = np.stack([red[moving], np.zeros_like(red[moving]), np.zeros_like(red[moving])], axis=1)
            Image.fromarray(heat, "RGB").save(args.heatmap)
        if args.json:
            with open(args.json, "w") as f:
                json.dump(out, f, indent=1)
        return 0

    pn, new = load(args.new, args.pick)
    if new.shape != a.shape:
        print(f"size mismatch: {pn} {new.shape[1]}x{new.shape[0]} vs {a.shape[1]}x{a.shape[0]}")
        return 2
    ref = a if args.against == "a" else b
    d = np.abs(new - ref).max(axis=2)
    beyond = d > floor
    sizes = clusters(beyond)
    ds = d[static]
    within = 100.0 * float((ds <= lv).mean()) if ds.size else 100.0
    tile, tile_at = worst_tile(d, static)
    block_new, block_ref = blockiness(new, static), blockiness(ref, static)
    m = {
        "new": pn, "against": args.against,
        "static_pct": 100.0 * static.sum() / total,
        "within_pct": within,
        "beyond_px": int(beyond.sum()),
        "largest_cluster": int(sizes.max()) if sizes.size else 0,
        "clusters_over_max": int((sizes > args.max_cluster).sum()),
        "psnr": psnr(new, ref, static),
        "max": int(ds.max()) if ds.size else 0,
        "p999": float(np.percentile(ds, 99.9)) if ds.size else 0.0,
        "worst_tile": tile, "worst_tile_at": list(tile_at),
        "blockiness_pct": 100.0 * (block_new / block_ref - 1.0) if block_ref and block_ref == block_ref else float("nan"),
        "rects": {},
    }
    h, w = d.shape
    for spec in args.rect:
        name, _, coords = spec.partition("=")
        x0, y0, x1, y1 = (float(v) for v in coords.split(","))
        sel = np.zeros_like(static)
        sel[int(y0 * h): int(y1 * h), int(x0 * w): int(x1 * w)] = True
        m["rects"][name] = psnr(new, ref, sel & static)
    out["compare"] = m

    print(f"{os.path.basename(os.path.dirname(pn)) or pn} vs ref_{args.against}: static {m['static_pct']:.2f} % of pixels, "
          f"within {lv}/255 {m['within_pct']:.4f} %, beyond the floor {m['beyond_px']} px, largest cluster "
          f"{m['largest_cluster']} px ({m['clusters_over_max']} over {args.max_cluster})")
    print(f"  PSNR {m['psnr']:.2f} dB, max {m['max']}/255, p99.9 {m['p999']:.2f}/255, worst 16x16 tile "
          f"{m['worst_tile']:.2f}/255 at {tuple(m['worst_tile_at'])}, blockiness {m['blockiness_pct']:+.2f} %"
          + "".join(f", {k} PSNR {v:.2f} dB" for k, v in m["rects"].items()))

    fails = []
    if not args.no_equal and len(refs) >= 3:
        frames = [r for _, r in refs]
        nat_within, nat_clusters, nat_largest = equal_trials(lambda k: frames[k], frames, lv, args.dilate, args.max_cluster)
        new_within, new_clusters, new_largest = equal_trials(lambda k: new, frames, lv, args.dilate, args.max_cluster)
        # A handful of references estimates the spread only roughly: small slack on every count.
        lim_within = min(args.min_within, nat_within) - 0.1
        lim_clusters = nat_clusters + max(3, nat_clusters // 2)
        lim_largest = max(nat_largest * 2, nat_largest + 64)
        m["calibrated"] = {"refs_within": nat_within, "refs_clusters": nat_clusters, "refs_largest": nat_largest,
                           "new_within": new_within, "new_clusters": new_clusters, "new_largest": new_largest}
        print(f"  calibrated (leave-one-out over {len(frames)} references): references within {nat_within:.4f} %, "
              f"{nat_clusters} cluster(s) > {args.max_cluster} px, largest {nat_largest} px; new within "
              f"{new_within:.4f} %, {new_clusters}, largest {new_largest} px")
        if new_within < lim_within:
            fails.append(f"within {new_within:.4f} % < {lim_within:.4f} %")
        if new_clusters > lim_clusters:
            fails.append(f"{new_clusters} cluster(s) > {args.max_cluster} px, allowed {lim_clusters}")
        if new_largest > lim_largest:
            fails.append(f"largest cluster {new_largest} px, allowed {lim_largest}")
    elif not args.no_equal:
        if m["within_pct"] < args.min_within:
            fails.append(f"within {m['within_pct']:.4f} % < {args.min_within} %")
        if m["clusters_over_max"]:
            fails.append(f"{m['clusters_over_max']} cluster(s) > {args.max_cluster} px")
    if args.min_psnr is not None and not m["psnr"] >= args.min_psnr:
        fails.append(f"PSNR {m['psnr']:.2f} < {args.min_psnr}")
    if args.max_p999 is not None and m["p999"] > args.max_p999:
        fails.append(f"p99.9 {m['p999']:.2f} > {args.max_p999}")
    if args.max_tile is not None and m["worst_tile"] > args.max_tile:
        fails.append(f"tile {m['worst_tile']:.2f} > {args.max_tile}")
    if args.max_blockiness is not None and not m["blockiness_pct"] <= args.max_blockiness:
        fails.append(f"blockiness {m['blockiness_pct']:+.2f} % > {args.max_blockiness} %")
    if args.min_rect_psnr is not None:
        for k, v in m["rects"].items():
            if not v >= args.min_rect_psnr:
                fails.append(f"{k} PSNR {v:.2f} < {args.min_rect_psnr}")
    m["verdict"] = "FAIL" if fails else "PASS"
    m["fails"] = fails
    print(m["verdict"] + ("" if not fails else ": " + "; ".join(fails)))

    if args.heatmap:
        base = (luma(ref.astype(np.float32)) * 0.35).astype(np.uint8)
        heat = np.stack([base, base, base], axis=2)
        heat[moving_dil, 2] = np.maximum(heat[moving_dil, 2], 110)
        red = np.clip(d.astype(np.int32) * 16, 64, 255).astype(np.uint8)
        heat[beyond] = np.stack([red[beyond], np.zeros_like(red[beyond]), np.zeros_like(red[beyond])], axis=1)
        Image.fromarray(heat, "RGB").save(args.heatmap)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1, default=float)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
