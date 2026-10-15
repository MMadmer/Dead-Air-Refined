"""Spent cartridge cases of every calibre the game fires, from their published case dimensions (mm).

A case is turned: a profile of (height, radius) from the head to the mouth, revolved. Heads differ by
kind: rimless (an extractor groove under a rim as wide as the body), rimmed (a rim wider than the body,
no groove), rebated (a rim narrower than the body), belted (a belt above the groove). Shotgun hulls are
plastic over a brass head.

finish: brass, steel_green (lacquered steel of Soviet production), steel_brown, nickel, hull_red.
"""

CASES = {
    #           length, rim r, groove r, head kind, base r, shoulder (z, r) or None, neck (z, r), mouth r
    '9x18':     dict(length=18.0, rim=4.95, groove=4.2, head='rimless', base=4.97, shoulder=None, neck=None, mouth=4.92,
                     finish='steel_green', primer=2.2, segments=12, sound='pistol', mass=3.4),
    '9x19':     dict(length=19.15, rim=4.98, groove=4.0, head='rimless', base=4.98, shoulder=None, neck=None, mouth=4.83,
                     finish='brass', primer=2.2, segments=12, sound='pistol', mass=4.0),
    '45acp':    dict(length=22.8, rim=6.1, groove=5.3, head='rimless', base=6.0, shoulder=None, neck=None, mouth=6.0,
                     finish='brass', primer=2.6, segments=12, sound='pistol', mass=6.0),
    '762x25':   dict(length=25.0, rim=4.95, groove=4.2, head='rimless', base=4.9, shoulder=(18.0, 4.73), neck=(19.2, 4.25),
                     mouth=4.25, finish='steel_green', primer=2.2, segments=12, sound='pistol', mass=4.7),
    '57x28':    dict(length=28.9, rim=3.98, groove=3.3, head='rimless', base=3.97, shoulder=(22.0, 3.9), neck=(23.2, 3.2),
                     mouth=3.2, finish='brass', primer=2.2, segments=12, sound='pistol', mass=2.4),
    '545x39':   dict(length=39.82, rim=5.0, groove=4.25, head='rimless', base=5.0, shoulder=(30.0, 4.5), neck=(32.2, 3.15),
                     mouth=3.15, finish='steel_green', primer=2.6, segments=14, sound='rifle', mass=6.5),
    '556x45':   dict(length=44.7, rim=4.78, groove=4.0, head='rimless', base=4.79, shoulder=(36.5, 4.5), neck=(39.6, 3.2),
                     mouth=3.2, finish='brass', primer=2.2, segments=14, sound='rifle', mass=6.2),
    '762x39':   dict(length=38.7, rim=5.67, groove=4.8, head='rimless', base=5.67, shoulder=(30.5, 5.04), neck=(33.0, 4.3),
                     mouth=4.3, finish='steel_green', primer=2.6, segments=14, sound='rifle', mass=7.0),
    '9x39':     dict(length=38.7, rim=5.67, groove=4.8, head='rimless', base=5.67, shoulder=(33.5, 5.15), neck=(34.5, 4.95),
                     mouth=4.95, finish='steel_brown', primer=2.6, segments=14, sound='rifle', mass=7.0),
    '792x33':   dict(length=33.0, rim=5.95, groove=5.0, head='rimless', base=5.95, shoulder=(24.0, 5.5), neck=(26.0, 4.45),
                     mouth=4.45, finish='steel_green', primer=2.6, segments=14, sound='rifle', mass=7.5),
    '762x51':   dict(length=51.2, rim=5.97, groove=5.1, head='rimless', base=5.97, shoulder=(39.6, 5.77), neck=(43.6, 4.39),
                     mouth=4.39, finish='brass', primer=2.65, segments=14, sound='rifle', mass=12.0),
    '762x54r':  dict(length=53.7, rim=7.24, groove=None, head='rimmed', base=6.19, shoulder=(40.5, 5.8), neck=(45.0, 4.27),
                     mouth=4.27, finish='steel_brown', primer=2.65, segments=14, sound='rifle', mass=12.5),
    '300wm':    dict(length=66.5, rim=6.75, groove=5.8, head='belted', base=6.5, shoulder=(53.5, 6.25), neck=(56.5, 4.25),
                     mouth=4.25, finish='brass', primer=2.65, segments=14, sound='large', mass=16.0),
    '127x55':   dict(length=55.0, rim=7.7, groove=6.6, head='rimless', base=7.7, shoulder=(40.0, 7.35), neck=(42.5, 7.05),
                     mouth=7.05, finish='steel_green', primer=2.65, segments=16, sound='large', mass=20.0),
    '127x99':   dict(length=99.0, rim=10.2, groove=8.6, head='rimless', base=10.2, shoulder=(72.0, 9.2), neck=(80.0, 7.0),
                     mouth=7.0, finish='brass', primer=3.9, segments=16, sound='large', mass=58.0),
    '30carbine': dict(length=32.8, rim=4.57, groove=3.9, head='rimless', base=4.52, shoulder=None, neck=None, mouth=4.2,
                     finish='brass', primer=2.2, segments=12, sound='pistol', mass=4.5),
    '50ae':     dict(length=32.6, rim=6.5, groove=5.8, head='rebated', base=6.95, shoulder=None, neck=None, mouth=6.8,
                     finish='brass', primer=2.6, segments=14, sound='pistol', mass=9.0),
    '12ga':     dict(length=70.0, rim=11.2, groove=None, head='hull', base=10.25, shoulder=None, neck=None, mouth=10.1,
                     finish='hull_red', primer=3.0, segments=16, sound='shotgun', mass=7.5, head_height=12.0),
}

# Atlas bands (rows of 64 px in a 512 x 512 texture): the side of a case, then the heads.
FINISHES = ['brass', 'steel_green', 'steel_brown', 'nickel', 'hull_red']
BAND_PX = 64
ATLAS = 512
