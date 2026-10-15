# Copyright (c) 2026 Grizzly
# Released under the MIT licence, see License.txt
"""Renders a list of shrub OBJs in one headless Blender session (gallery.py's jobs.json).

    blender -b --factory-startup --python render_gallery.py -- jobs.json
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_preview  # noqa: E402


def rgb(s):
    return tuple(float(x) for x in s.split(','))


jobs = json.load(open(sys.argv[sys.argv.index('--') + 1]))
for j in jobs:
    render_preview.render_scene(j['obj'], j['png'], rgb(j['leaf']), rgb(j['bloom']), rgb(j['wood']), (1200, 600))
