"""Ours against the original: for hand vertices the original leaves outside its body and within 6 mm of
it, how much closer our surface comes (unsigned distances) - new contact our geometry would add.
    python bp_depthcmp.py <tag> ..."""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
O = np.load(os.path.join(HERE, 'depth_orig.npz'))
H = np.load(os.path.join(HERE, 'hands_wpn_hand_00.npz'))
for tag in sys.argv[1:]:
    N = np.load(os.path.join(HERE, f'depth_{tag}.npz'))
    for clip in ('open', 'close'):
        near = (O[clip] == 0) & (O[clip + '_u'] < 0.006)
        closer = np.where(near, O[clip + '_u'] - N[clip + '_u'], -1)
        fi, vi = np.unravel_index(np.argmax(closer), closer.shape)
        bad = (closer > 0.0013).sum(1)
        print('%-17s %-5s closer by up to %.2f mm (frame %d, hand at %s), frames with a vertex >1.3 mm closer: %d/%d' % (
            tag, clip, closer.max() * 1e3, fi, np.round(H[clip][fi, vi], 3), (bad > 0).sum(), len(bad)))
