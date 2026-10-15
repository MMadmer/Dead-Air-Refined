"""The ice's sounds, synthesized: sounds\\dead_air_x64\\ice\\<name>.ogg (docs/dead-air/WEATHER.md, ice).

    python tools/ice/ice_sounds.py <out dir> [install]

Lake ice talks in three voices. A blow or a load snaps it - a broadband crack - and the crack runs off through the
sheet as a dispersive wave: the high frequencies arrive first, so a struck pond answers with a falling whistle
("pew") and a few echoes off the far edge. Thin ice crumbles instead: a crunch of many small breaks. Breaking through
ends in the water: plates knocking, a plunge, bubbles. A boot on ice with a dusting of snow is a dull knock under a
crisp crunch. Every file is normalized to -8 dB of peak like every sound of the project.

  step_1..4          a boot on ice (with every creature: the ice material's step pairs)
  knock_1..3         an object dropped on ice (the material's collisions)
  bullet_1..4        a bullet stopped by ice: a crack, chips, the whistle of the sheet
  bullet_thin_1..2   a bullet through thin ice: a crack and a plop
  crack_1..6         ice cracking under a weight: crackle and whistles, louder as it goes
  break_1..3         ice giving way: the crash, the plates, the plunge
"""
import os
import subprocess
import sys
import wave

import numpy as np

sys.path.insert(0, r"D:\Games\Dead-Air-AssetCompare\pipeline")

OUT = sys.argv[1]
STAMP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'audio', 'stamp_xray_ogg_comment.py')
RATE = 44100
rng = np.random.default_rng(1510)


def onepole(x, cutoff):
    a = np.exp(-2 * np.pi * cutoff / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = (1 - a) * v + a * acc
        y[i] = acc
    return y


def band(x, lo, hi):
    return onepole(x, hi) - onepole(x, lo)


def grain(n, at, length, lo, hi, amp):
    k = max(int(length * RATE), 8)
    g = rng.standard_normal(k).astype(np.float32) * np.hanning(k).astype(np.float32)
    g = band(g, lo, hi)
    out = np.zeros(n, np.float32)
    i = int(at * RATE)
    if i < n:
        m = min(k, n - i)
        out[i:i + m] = g[:m] * amp
    return out


def crunch(n, start, dur, count, lo=1800.0, hi=7000.0, amp=1.0):
    """Many small breaks, thinning out over dur."""
    out = np.zeros(n, np.float32)
    for _ in range(count):
        t = start + dur * rng.random() ** 1.8
        out += grain(n, t, rng.uniform(0.002, 0.008), lo * rng.uniform(0.7, 1.3), hi * rng.uniform(0.8, 1.2),
                     amp * rng.uniform(0.3, 1.0) * (1.0 - 0.7 * (t - start) / max(dur, 1e-3)))
    return out


def snap(n, at, amp=1.0, bright=6000.0):
    k = int(0.004 * RATE)
    burst = rng.standard_normal(k).astype(np.float32) * np.exp(-np.arange(k) / (k / 5)).astype(np.float32)
    burst = burst - onepole(burst, 700.0)
    burst = onepole(burst, bright)
    out = np.zeros(n, np.float32)
    i = int(at * RATE)
    if i < n:
        m = min(k, n - i)
        out[i:i + m] = burst[:m] * amp * 3.0
    return out


def whistle(n, at, f0, f1, tau, decay, amp, echoes=3):
    """The dispersive wave of a crack in a sheet: falling from f0 to f1, with echoes off the far edge."""
    out = np.zeros(n, np.float32)
    for e in range(echoes + 1):
        start = at + (0 if e == 0 else rng.uniform(0.03, 0.09) * e)
        i = int(start * RATE)
        if i >= n:
            break
        t = np.arange(n - i) / RATE
        f = f1 + (f0 - f1) * np.exp(-t / tau)
        phase = 2 * np.pi * np.cumsum(f) / RATE
        env = np.exp(-t / decay) * (1 - np.exp(-t / 0.002))
        out[i:] += (np.sin(phase) * env * amp * (0.45 ** e)).astype(np.float32)
    return out


def thud(n, at, cutoff, length, amp):
    k = int(length * RATE)
    noise = rng.standard_normal(k).astype(np.float32) * np.exp(-np.arange(k) / (k / 4)).astype(np.float32)
    noise = onepole(onepole(noise, cutoff), cutoff)
    out = np.zeros(n, np.float32)
    i = int(at * RATE)
    if i < n:
        m = min(k, n - i)
        out[i:i + m] = noise[:m] * amp * 6.0
    return out


def splash(n, at, length, amp):
    k = int(length * RATE)
    t = np.arange(k) / RATE
    noise = rng.standard_normal(k).astype(np.float32)
    # water is dark: the splash's noise falls off steeply above a kilohertz, under a plunge's thump
    body = onepole(onepole(onepole(noise, 900.0), 900.0), 1200.0) * 3.0
    body = body * (np.exp(-t / (length * 0.3)) * (1 - np.exp(-t / 0.01))).astype(np.float32)
    body += onepole(onepole(noise, 160.0), 160.0) * 8.0 * np.exp(-t / 0.08).astype(np.float32)
    out = np.zeros(n, np.float32)
    i = int(at * RATE)
    m = min(k, n - i)
    if m > 0:
        out[i:i + m] += body[:m] * amp * 4.0
    # bubbles: resonances that rise as they close
    for _ in range(int(10 * length)):
        bt = at + rng.uniform(0.02, length * 0.8)
        j = int(bt * RATE)
        if j >= n:
            continue
        bk = int(rng.uniform(0.015, 0.04) * RATE)
        bt_ = np.arange(bk) / RATE
        f = rng.uniform(350.0, 1100.0) * (1 + 2.5 * bt_ / bt_[-1])
        b = np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-bt_ / 0.012)
        mm = min(bk, n - j)
        out[j:j + mm] += (b[:mm] * amp * rng.uniform(0.1, 0.3)).astype(np.float32)
    return out


def step():
    n = int(0.35 * RATE)
    return (thud(n, 0.0, 180.0, 0.05, 0.9) + crunch(n, 0.004, 0.09, 26, 2200.0, 8000.0, 0.55) +
            snap(n, 0.002, 0.15, 5000.0))


def knock():
    n = int(0.5 * RATE)
    return (thud(n, 0.0, 300.0, 0.03, 0.9) + snap(n, 0.0, 0.5) +
            whistle(n, 0.005, rng.uniform(1500, 2400), 500.0, 0.04, 0.12, 0.25, 1))


def bullet():
    n = int(0.8 * RATE)
    return (snap(n, 0.0, 1.0, 9000.0) + crunch(n, 0.002, 0.05, 30, 3000.0, 10000.0, 0.6) +
            whistle(n, 0.004, rng.uniform(3500, 5000), rng.uniform(600, 900), rng.uniform(0.05, 0.09), 0.25, 0.5, 3))


def bullet_thin():
    n = int(0.9 * RATE)
    return (snap(n, 0.0, 0.9, 8000.0) + crunch(n, 0.002, 0.06, 20, 2500.0, 8000.0, 0.5) +
            splash(n, 0.01, 0.35, 0.35))


def crack(severity):
    n = int((0.7 + 0.8 * severity) * RATE)
    out = np.zeros(n, np.float32)
    # a crackle: a few snaps and a crunch, more the further it goes
    for _ in range(2 + int(5 * severity)):
        out += snap(n, rng.uniform(0.0, 0.25 + 0.3 * severity), rng.uniform(0.3, 0.9), rng.uniform(4000, 9000))
    out += crunch(n, 0.0, 0.2 + 0.3 * severity, int(20 + 40 * severity), 1500.0, 7000.0, 0.35 + 0.3 * severity)
    # the sheet's whistles
    for _ in range(1 + int(2 * severity)):
        out += whistle(n, rng.uniform(0.0, 0.3), rng.uniform(1800, 3200), rng.uniform(250, 500),
                       rng.uniform(0.06, 0.18), rng.uniform(0.2, 0.45), rng.uniform(0.35, 0.6), 2)
    return out


def brk():
    n = int(1.8 * RATE)
    out = snap(n, 0.0, 1.0, 7000.0) + snap(n, 0.03, 0.7, 5000.0)
    out += crunch(n, 0.0, 0.45, 160, 1200.0, 7000.0, 0.9)
    # plates knocking
    for _ in range(5):
        out += thud(n, rng.uniform(0.05, 0.5), 400.0, 0.03, rng.uniform(0.3, 0.6))
    out += whistle(n, 0.0, 2600.0, 350.0, 0.1, 0.35, 0.4, 2)
    out += splash(n, 0.08, 1.2, 0.9)
    return out


def write(name, x, min_d, max_d):
    work = os.path.join(OUT, 'wav')
    game = os.path.join(OUT, 'game')
    os.makedirs(work, exist_ok=True)
    os.makedirs(game, exist_ok=True)
    fade = np.ones(len(x), np.float32)
    k = int(0.03 * RATE)
    fade[-k:] = np.linspace(1, 0, k, dtype=np.float32)
    x = x * fade
    x = x / max(np.abs(x).max(), 1e-6) * (10 ** (-8 / 20))
    data = (np.clip(x, -1, 1) * 32767).astype('<i2')
    wav = os.path.join(work, name + '.wav')
    with wave.open(wav, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())
    ogg = os.path.join(game, name + '.ogg')
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', wav, '-c:a', 'libvorbis', '-q:a', '5', ogg], check=True)
    subprocess.run([sys.executable, STAMP, ogg, str(min_d), str(max_d), '1.0', '0', str(max_d)], check=True,
                   capture_output=True)
    return name


def main():
    made = []
    for i in range(1, 5):
        made.append(write('step_%d' % i, step(), 0.5, 30))
    for i in range(1, 4):
        made.append(write('knock_%d' % i, knock(), 0.5, 30))
    for i in range(1, 5):
        made.append(write('bullet_%d' % i, bullet(), 1.0, 70))
    for i in range(1, 3):
        made.append(write('bullet_thin_%d' % i, bullet_thin(), 1.0, 60))
    for i, sev in enumerate((0.0, 0.2, 0.4, 0.6, 0.8, 1.0), 1):
        made.append(write('crack_%d' % i, crack(sev), 1.0, 60))
    for i in range(1, 4):
        made.append(write('break_%d' % i, brk(), 1.5, 90))
    print('SOUNDS', len(made))
    if len(sys.argv) > 2 and sys.argv[2] == 'install':
        from common import install  # noqa: E402
        game = os.path.join(OUT, 'game')
        for f in sorted(os.listdir(game)):
            install(os.path.join(game, f), 'sounds/dead_air_x64/ice/' + f)
        print('installed into sounds/dead_air_x64/ice')


main()
