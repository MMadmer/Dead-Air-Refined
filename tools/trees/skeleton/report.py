# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Report sheet of one skeleton (REPORT_PNG/<skel>.png): stock wood with rings, axes and patches; the refit tubes
coloured by their distance to the stock; the tubes against the tree_trunk collision of a sampled placement; the
crown field (projections and slices) with the card bases."""
import os

import numpy as np
from PIL import Image, ImageDraw

import cform as CF
import common as C
import dtsk as DT
import render as RD
import tube as TB

W, H = 520, 760


def ring_loops(sk, max_rings=4000):
    """Model-space circles of the rings (profile at 24 angles)."""
    th = np.arange(25) * (2 * np.pi / 24)
    out = []
    for a in sk.axes:
        r = sk.rings[a['first_ring']:a['first_ring'] + a['n_rings']]
        for x in r:
            if len(out) >= max_rings:
                return out
            prof = np.ones_like(th)
            for k in range(1, 4):
                prof += x['f'][2 * k - 2] * np.cos(k * th) + x['f'][2 * k - 1] * np.sin(k * th)
            n = x['n'].astype(np.float64)
            t = x['t'].astype(np.float64)
            b = np.cross(t, n)
            P = x['c'] + (x['r0'] * prof)[:, None] * (np.cos(th)[:, None] * n + np.sin(th)[:, None] * b)
            out.append((P, a['level'], x['stock_sides'] > 0))
    return out


def sheet(sid, res, sk, kinds, wood, order, fields, fw, pbad, card_list, field, cf, gate_res):
    rep = kinds[0]
    bmin = sk.bmin.astype(np.float64)
    bmax = sk.bmax.astype(np.float64)
    if wood is not None and wood.topo.used.any():
        wb = (wood.topo.V[wood.topo.used].min(0), wood.topo.V[wood.topo.used].max(0))
    else:
        wb = (bmin, bmax)
    panels = []
    loops = ring_loops(sk)
    # 1-2: stock wood + patches + rings + axes, front and side
    for view in ('front', 'side'):
        cv = RD.Canvas(W, H, view, bmin, bmax)
        if wood is not None and len(wood.topo.F):
            col = np.where(pbad[:, None], np.array([[235, 135, 30]]), np.array([[135, 110, 85]]))
            cv.tris(wood.topo.V, wood.topo.F, col)
        for cp in rep.cap_parts():
            cv.tris(cp.P, cp.T, np.array([[235, 135, 30]]))
        for P, lv, stock in loops:
            if lv < 2:
                cv.lines(P, (255, 255, 255) if stock else (120, 220, 255), 1)
        for a in sk.axes:
            r = sk.rings[a['first_ring']:a['first_ring'] + a['n_rings']]
            colr = {0: (255, 250, 120), 1: (255, 200, 60), 2: (60, 200, 90)}[a['level']]
            cv.lines(r['c'].astype(np.float64), colr, 1)
        cv.label(f'{sid}  {view}: stock wood, patches (orange), rings (white stock / blue added), axes')
        panels.append(cv.flush())
    # 3: refit tubes coloured by the tube -> stock distance
    cv = RD.Canvas(W, H, 'front', bmin, bmax)
    for fd in fields:
        ev = TB.evaluate(fd.af.R, 4, 16)
        V, F, row = TB.mesh(ev)
        if fw is not None and fd.af.id in fw:
            e = fw[fd.af.id]
            d = np.where(e['hidden'], 0.0, np.minimum(e['d'], 0.2))
            # map the measurement rows (N_SUB, N_TH) onto this coarser mesh by the nearest sample
            rr = np.minimum((np.arange(ev['P'].shape[0]) * e['d'].shape[0]) // max(ev['P'].shape[0], 1),
                            e['d'].shape[0] - 1)
            cc = np.minimum((np.arange(ev['P'].shape[1]) * e['d'].shape[1]) // ev['P'].shape[1], e['d'].shape[1] - 1)
            dv = d[rr][:, cc].reshape(-1)
            cv.tris(V, F, RD.heat(dv[F].max(1), 0.0, 0.05))
        else:
            cv.tris(V, F, np.array([[150, 150, 150]]))
    cv.label('refit tubes: distance to the stock wood (0-5 cm)')
    img3 = cv.flush()
    RD.legend_bar(img3, (10, H - 30), 200, 10, 0, 5, 'cm', '')
    panels.append(img3)
    # 4: collision of the first sampled placement (model space) and the tubes
    cv = RD.Canvas(W, H, 'front', bmin, bmax)
    if cf is not None and len(cf['xforms']):
        for slot in range(len(cf['xforms'])):
            m = (cf['owner'] == slot) & (cf['kind'] == 1)
            if m.any():
                Tw = cf['tri'][m].astype(np.float64)
                V = CF.to_model(Tw.reshape(-1, 3), cf['xforms'][slot])
                F = np.arange(len(V)).reshape(-1, 3)
                cv.tris(V, F, np.array([[210, 60, 50]]))
                mh = (cf['owner'] == slot) & (cf['kind'] == 2)
                if mh.any():
                    Vh = CF.to_model(cf['tri'][mh].astype(np.float64).reshape(-1, 3), cf['xforms'][slot])
                    for t in Vh.reshape(-1, 3, 3):
                        cv.lines(t, (90, 120, 255), 1, closed=True)
                break
    for fd in fields:
        cv.lines(fd.C, (255, 255, 255), 1)
    g = gate_res or {}
    lab = 'collision (red), crown hull (blue), spines'
    if g.get('p99') is not None:
        lab += f'; gate p99 {100 * g["p99"]:.1f} cm'
    cv.label(lab)
    panels.append(cv.flush())
    # 5: crown field projections and slices
    img5 = Image.new('RGB', (W, H), (40, 44, 52))
    d5 = ImageDraw.Draw(img5)
    d5.text((4, 2), 'crown field: front max / top max / slices 25-50-75 %', fill=(255, 255, 255), font=RD.font(12))
    if field is not None:
        occ = field.occ.astype(np.float64)          # [x, y, z]
        front = occ.max(2).T[::-1]                  # rows = y (top first), cols = x
        top = occ.max(1).T[::-1]                    # rows = z, cols = x

        def put(a, x0, y0, w, h):
            a = np.clip(a, 0, 255).astype(np.uint8)
            im = Image.fromarray(a).convert('L')
            sc = min(w / max(im.width, 1), h / max(im.height, 1))
            im = im.resize((max(1, int(im.width * sc)), max(1, int(im.height * sc))), Image.NEAREST)
            img5.paste(Image.merge('RGB', (im, im.point(lambda v: int(v * 0.85)), im.point(lambda v: int(v * 0.4)))),
                       (x0, y0))
        put(front, 10, 22, W - 20, 300)
        put(top, 10, 330, (W - 30) // 2, 200)
        ny = field.ny
        for i, q in enumerate((0.25, 0.5, 0.75)):
            sl = occ[:, int(q * (ny - 1)), :].T[::-1]
            put(sl, 10 + i * ((W - 20) // 3), 545, (W - 40) // 3, 150)
        st = field.stats
        d5.text((10, 700), f'cell {field.cell:.2f} m, {field.nx}x{field.ny}x{field.nz}, occupied {st.get("occupied_final")}'
                           f', hulls {st.get("hulls_used")}/{st.get("hulls")}, clipped {100 * st.get("clipped_area_share", 0):.1f} %',
                fill=(255, 255, 255), font=RD.font(11))
    panels.append(img5)
    # text strip
    txt = Image.new('RGB', (W * 5, 150), (250, 250, 250))
    dt = ImageDraw.Draw(txt)
    lines = [f'{sid}  kinds {", ".join(res["kinds"])}  {"/".join(res["categories"])} {"/".join(res["states"])}  '
             f'species {", ".join(res["species"])}  flags {res["flags"]}',
             f'axes {res["axes"]} (level0 {res["axes_level0"]}, level1 {res["axes_level1"]}, cards {res["axes_level2"]}), '
             f'rings {res["rings"]} (stock axes {res["rings_stock"]}), patch tris {res["patch_tris"]} '
             f'(bark {res["patch_tris_bark"]}), cards {res["cards"]}, file {res["bytes"] / 1024:.1f} KB']
    if 'patch_area_share' in res:
        gg = res.get('gate') or {}
        p99 = '-' if gg.get('p99') is None else '%.2f cm' % (100 * gg['p99'])
        sp99 = '-' if gg.get('world_p99') is None else '%.2f cm' % (100 * gg['world_p99'])
        rp99 = '-' if gg.get('reverse_p99') is None else '%.2f cm' % (100 * gg['reverse_p99'])
        lines.append(f'patch area {100 * res["patch_area_share"]:.1f} %, ring fit_err p50 {100 * (res["fit_err_p50"] or 0):.1f} cm '
                     f'p99 {100 * (res["fit_err_p99"] or 0):.1f} cm, rings over tolerance {res["rings_over_tol"]}; '
                     f'gate: placements {gg.get("with_trunk", 0)}/{gg.get("placements", 0)}, climbable bark '
                     f'{100 * gg.get("climb_share", 0):.0f} %, p99 {p99} (world {sp99}, collision->wood {rp99})')
    if res.get('second_opinion'):
        lines.append('second reading of pieces mostly patched: ' + ', '.join(
            f'piece {v["piece"]} {v["chosen"]} (score {v["levelsets"]["score"]:.2f} -> '
            f'{v[v["chosen"]]["score"] if v["chosen"] != "levelsets" else v["levelsets"]["score"]:.2f})'
            for v in res['second_opinion'][:8]))
    if res.get('problems'):
        lines.append('PROBLEMS: ' + '; '.join(res['problems'][:6]))
    for i, ln in enumerate(lines):
        dt.text((8, 6 + 22 * i), ln, fill=(0, 0, 0), font=RD.font(14))
    out = Image.new('RGB', (W * 5, H + 150), (255, 255, 255))
    for i, p in enumerate(panels):
        out.paste(p, (W * i, 0))
    out.paste(txt, (0, H))
    C.atomic_png(os.path.join(C.REPORT_PNG, f'{sid}.png'), out)
