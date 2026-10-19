"""Bounces of spent cases, synthesized: sounds\\dead_air_x64\\shells\\<class>_<surface>_<n>.ogg.

    python shells_sounds.py <out dir> [install]

A brass case rings: a few inharmonic modes of a thin tube (higher and shorter for a pistol case, lower and
longer for a rifle or a 12.7), started by a click. The surface decides how much of it survives: stone and
wood let it ring, earth swallows it in a thud, steel adds a ring of its own. A shotgun hull is plastic over
a brass head: a dull knock with a short tick. Every file is one hit, sometimes with a second small one a
few tens of milliseconds later, normalized to -8 dB of peak like every sound of the project.
"""
import os
import subprocess
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'asset_pipeline'))
from common import install  # noqa: E402

OUT = sys.argv[1]
STAMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'audio', 'stamp_xray_ogg_comment.py')
RATE = 44100
VARIANTS = 4
rng = np.random.default_rng(2611)

# base frequency of the case's first mode, its decay (s), its loudness
CLASSES = {
    'pistol': (5600.0, 0.10, 0.9),
    'rifle': (3900.0, 0.16, 1.0),
    'large': (2500.0, 0.26, 1.0),
}
MODES = ((1.0, 1.0, 1.0), (1.53, 0.55, 0.7), (2.37, 0.32, 0.5), (3.14, 0.18, 0.35))


def lowpass(x, cutoff):
    a = np.exp(-2 * np.pi * cutoff / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1 - a) * v + a * acc
        y[i] = acc
    return y


def ring(f0, decay, n, gain=1.0):
    t = np.arange(n) / RATE
    out = np.zeros(n, np.float32)
    for ratio, amp, dk in MODES:
        f = f0 * ratio * rng.uniform(0.96, 1.04)
        out += (amp * np.sin(2 * np.pi * f * t + rng.uniform(0, 6.28)) * np.exp(-t / (decay * dk))).astype(np.float32)
    return out * gain


def click(n, bright=1.0):
    k = int(0.002 * RATE)
    burst = rng.standard_normal(k).astype(np.float32) * np.linspace(1, 0, k, dtype=np.float32)
    burst = burst - lowpass(burst, 2500.0 * bright)
    out = np.zeros(n, np.float32)
    out[:k] = burst
    return out


def thud(n, cutoff=700.0, length=0.05):
    k = int(length * RATE)
    noise = rng.standard_normal(k).astype(np.float32) * np.exp(-np.arange(k) / (k / 4)).astype(np.float32)
    out = np.zeros(n, np.float32)
    out[:k] = lowpass(noise, cutoff) * 3.0
    return out


def hit(cls, surface, n):
    if cls == 'shotgun':
        body = thud(n, 1200.0, 0.06) * 1.2 + ring(900.0, 0.03, n, 0.25) + ring(3200.0, 0.02, n, 0.2) + click(n, 0.6) * 0.3
        if surface == 'soft':
            body = thud(n, 600.0, 0.07) + ring(3200.0, 0.01, n, 0.08)
        elif surface == 'metal':
            body += ring(1700.0, 0.12, n, 0.35)
        return body
    f0, decay, gain = CLASSES[cls]
    f0 *= rng.uniform(0.95, 1.05)
    if surface == 'soft':
        return thud(n, 500.0, 0.045) * 0.9 + ring(f0, decay * 0.12, n, 0.18 * gain) + click(n, 0.4) * 0.15
    if surface == 'metal':
        return ring(f0, decay * 1.4, n, gain) + ring(rng.uniform(1300.0, 2400.0), 0.14, n, 0.45) + click(n) * 0.6
    return ring(f0, decay, n, gain) + click(n) * 0.5 + thud(n, 1800.0, 0.01) * 0.2


def sound(cls, surface):
    length = 0.7 if surface != 'soft' else 0.25
    n = int(length * RATE)
    out = hit(cls, surface, n)
    # a second small hit as the case comes back down, on hard ground
    if surface != 'soft' and rng.random() < 0.7:
        lag = int(rng.uniform(0.035, 0.09) * RATE)
        out[lag:] += hit(cls, surface, n - lag) * rng.uniform(0.25, 0.45)
    fade = np.ones(n, np.float32)
    fade[-int(0.03 * RATE):] = np.linspace(1, 0, int(0.03 * RATE), dtype=np.float32)
    out *= fade
    peak = np.abs(out).max()
    return out / peak * (10 ** (-8 / 20))


def main():
    work = os.path.join(OUT, 'wav')
    game = os.path.join(OUT, 'game')
    os.makedirs(work, exist_ok=True)
    os.makedirs(game, exist_ok=True)
    made = []
    for cls in ('pistol', 'rifle', 'large', 'shotgun'):
        for surface in ('hard', 'soft', 'metal'):
            for v in range(1, VARIANTS + 1):
                name = f'{cls}_{surface}_{v}'
                data = (np.clip(sound(cls, surface), -1, 1) * 32767).astype('<i2')
                wav = os.path.join(work, name + '.wav')
                with wave.open(wav, 'wb') as w:
                    w.setnchannels(1)
                    w.setsampwidth(2)
                    w.setframerate(RATE)
                    w.writeframes(data.tobytes())
                ogg = os.path.join(game, name + '.ogg')
                subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', wav, '-c:a', 'libvorbis', '-q:a', '5', ogg],
                               check=True)
                # the engine's own block first: distances of a small sound, no AI hearing (game type 0)
                subprocess.run([sys.executable, STAMP, ogg, '0.5', '20', '1.0', '0', '20'], check=True,
                               capture_output=True)
                made.append(name)
    print('SOUNDS', len(made))
    if len(sys.argv) > 2 and sys.argv[2] == 'install':
        for f in sorted(os.listdir(game)):
            install(os.path.join(game, f), 'sounds/dead_air_x64/shells/' + f)
        print('installed into sounds/dead_air_x64/shells')


main()
