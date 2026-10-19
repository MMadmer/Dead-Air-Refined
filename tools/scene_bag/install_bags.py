"""Our scene bags into the 1.6.0 assets tree (by rename), the replaced bag and its texture out of it (links:
the main tree keeps its own), the dead EA backpack configs out of both installs' loose gamedata.
    python install_bags.py [apply]"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = r"D:\Games\Dead-Air-Refined_Assets\source\gamedata"
VARIANTS = ['default', 'kit_hunt', 'backpack_light', 'backpack_heavy', 'backpack_travel', 'backpack_stalker',
            ]
GONE = ['meshes/dynamics/weapons/wpn_eat/backpack/wpn_backpack_act_hud.ogf',
        'meshes/dynamics/weapons/wpn_eat/backpack/wpn_backpack_act_hud_monolith.ogf',
        'textures/act/act_morthbag.dds', 'textures/act/act_morthbag_bump.dds',
        'textures/act/act_morthbag_monolith.dds', 'textures/act/act_morthbag_monolith_bump.dds',
        # the exoskeleton is not a backpack: it gets the default bag
        'meshes/dynamics/weapons/wpn_eat/backpack/dar_bag_exobackpack_hud.ogf',
        'textures/act/dar_bag_exobackpack.dds', 'textures/act/dar_bag_exobackpack.thm',
        'textures/act/dar_bag_exobackpack_bump.dds', 'textures/act/dar_bag_exobackpack_bump#.dds',
        'textures/act/dar_bag_exobackpack_bump.thm']
STALE = ['gamedata/configs/items/items/items_anm_backpack.ltx', 'gamedata/configs/items/items/ea_addon_backpack.ltx']
INSTALLS = [r"D:\Games\Dead Air"]

apply = len(sys.argv) > 1 and sys.argv[1] == 'apply'
for v in VARIANTS:
    print(v, sorted(os.listdir(os.path.join(HERE, 'out', v, 'game'))))
print('to remove from 1.6.0:', [g for g in GONE if os.path.isfile(os.path.join(TREE, g))])
print('stale loose configs:', [os.path.join(i, s) for i in INSTALLS for s in STALE if os.path.isfile(os.path.join(i, s))])
if apply:
    for v in VARIANTS:
        r = subprocess.run([sys.executable, os.path.join(HERE, 'bp_textures.py'), v, 'install'], capture_output=True,
                           text=True, encoding='utf-8', env=dict(os.environ, PYTHONIOENCODING='utf-8'))
        print(v, 'install rc', r.returncode, r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr[-300:])
    for g in GONE:
        p = os.path.join(TREE, g)
        if os.path.isfile(p):
            os.remove(p)
    for i in INSTALLS:
        for s in STALE:
            p = os.path.join(i, s)
            if os.path.isfile(p):
                os.remove(p)
    have = [f for f in os.listdir(os.path.join(TREE, 'meshes/dynamics/weapons/wpn_eat/backpack'))]
    print('bag folder now:', sorted(have))
    print('act textures now:', sorted(f for f in os.listdir(os.path.join(TREE, 'textures/act')) if 'bag' in f))
