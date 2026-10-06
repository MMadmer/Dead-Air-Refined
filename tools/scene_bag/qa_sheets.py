"""The two pictures for the user from the in-game runs: every bag in the held pose next to the original, and
the pull being taken and drawn, original | ours.  python qa_sheets.py <new run> [ref run]"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
QA = os.path.join(HERE, 'qa')
NEW = os.path.join(QA, sys.argv[1] if len(sys.argv) > 1 else 'bags-final')
REF = os.path.join(QA, sys.argv[2] if len(sys.argv) > 2 else 'bags-ref')
FONT = ImageFont.truetype(r"C:\Windows\Fonts\arial.ttf", 22)


def cap(run, k):
    c = sorted(x for x in os.listdir(run) if x.startswith('capture_'))[k]
    return os.path.join(run, c), sorted(os.listdir(os.path.join(run, c)))


def label(im, text):
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, d.textlength(text, font=FONT) + 16, 34), fill=(0, 0, 0))
    d.text((8, 5), text, font=FONT, fill=(255, 225, 90))
    return im


def held(run, k, text):
    d, fs = cap(run, k)
    im = Image.open(os.path.join(d, fs[24])).convert('RGB').crop((700, 620, 2400, 1440)).resize((680, 328))
    return label(im, text)


tiles = [held(REF, 1, 'Оригинал (Mirrowel)'),
         held(NEW, 1, 'Дефолт: нет рюкзака, костюм, экзо, баллон'),
         held(NEW, 2, 'Охотничий'), held(NEW, 3, 'Походный'), held(NEW, 4, 'Тактический'),
         held(NEW, 5, 'Туристический «Ермак»'), held(NEW, 6, 'Сталкерский')]
W = Image.new('RGB', (680 * 2 + 6, 334 * 4), (16, 16, 16))
for i, t in enumerate(tiles):
    W.paste(t, ((i % 2) * 686, (i // 2) * 334))
W.save(os.path.join(QA, 'bags_final_held.jpg'), quality=90)

# the pull: the backpack_stalker pass played the original bag in the reference run (one frame later there)
dr, fr = cap(REF, 6)
dn, fn = cap(NEW, 6)
rows = []
for i in (13, 14, 15, 16):
    a = Image.open(os.path.join(dr, fr[i + 1])).convert('RGB').crop((560, 600, 2560, 1440)).resize((700, 294))
    b = Image.open(os.path.join(dn, fn[i])).convert('RGB').crop((560, 600, 2560, 1440)).resize((700, 294))
    r = Image.new('RGB', (1406, 294), (16, 16, 16))
    r.paste(label(a, 'Оригинал'), (0, 0))
    r.paste(label(b, 'Наша'), (706, 0))
    rows.append(r)
W = Image.new('RGB', (1406, 300 * 4), (16, 16, 16))
for i, r in enumerate(rows):
    W.paste(r, (0, i * 300))
W.save(os.path.join(QA, 'bags_final_pinch.jpg'), quality=90)
print('sheets ok')
