"""Copy the per-species photo colours of the species research (colours.json: dated iNaturalist photos of Ukraine and
Belarus, measured per phenophase, METHOD.md of that research) into tools/seasons/data/species_photo_colours.json:
per species and bucket the median colour of each part (live leaf, glaucous leaf, dead leaf, head tints, flowers,
stems) with its photo count, and the glaucous index. make_species.py calibrates them into the season colours.

    python tools/seasons/data/extract_photo_colours.py <path to colours.json>
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KINDS = ['leaf', 'glauc', 'dead', 'tan', 'purple', 'pale', 'dark', 'stem', 'yellow', 'white', 'pink', 'red', 'blue', 'silver']


def main():
    src = json.load(open(sys.argv[1], encoding='utf-8'))
    out = {}
    for sid, s in src.items():
        b_out = {}
        for b, v in s['buckets'].items():
            ks = v.get('kinds', {})
            row = {k: [ks[k]['hex'], ks[k]['n']] for k in KINDS if k in ks and ks[k].get('n', 0) >= 1}
            fl = v.get('flower')
            if fl and fl.get('hex'):
                row['flower'] = [fl['hex'], fl.get('n', 0), fl.get('kind', '')]
            row['photos'] = v.get('n_photos', 0)
            b_out[b] = row
        out[sid] = dict(latin=s['latin'], taxa=s.get('taxa'), glaucous_index=s.get('glaucous_index'),
                        winter_green_share=s.get('winter_green_share'), buckets=b_out)
    meta = dict(what='Photo colours of the 64 species per phenophase bucket: [sRGB hex, photos]',
                source='iNaturalist research-grade observations (mostly Ukraine and Belarus), 4179 photos, '
                       'centre-weighted HSV masks, linear medians (species research 06-07.10.2026, METHOD.md)',
                buckets='spring Apr-May, early_summer Jun, mid_summer Jul, late_summer Aug-Sep, autumn Oct-Nov, '
                        'winter Dec-Mar')
    with open(os.path.join(HERE, 'species_photo_colours.json'), 'w', encoding='utf-8', newline='\r\n') as f:
        json.dump(dict(meta=meta, species=out), f, indent=1)
        f.write('\n')
    print('%d species' % len(out))


if __name__ == '__main__':
    main()
