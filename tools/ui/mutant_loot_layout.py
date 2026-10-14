# Writes configs\ui\ui_mutant_loot.xml (4:3) and ui_mutant_loot_16.xml (16:9) of the compatibility overlay:
# the window ui_mutant_loot.script opens over a carcass. Lengths are screen units, as in
# actor_menu_layout.py: 768 to the screen's height, x squeezed by 0.75 into a 16:9 file.
#
#   python tools\ui\mutant_loot_layout.py
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', 'packaging', 'dead-air-x64', 'compatibility',
                    'gamedata', 'configs', 'ui')

W, H = 560.0, 372.0


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def build(wide):
    kx = 0.75 if wide else 1.0
    screen_w = 1024.0 / kx

    def rect(x, y, w, h):
        return 'x="%s" y="%s" width="%s" height="%s"' % (num(x * kx), num(y), num(w * kx), num(h))

    fx, fy = (screen_w - W) / 2, (768.0 - H) / 2
    out = []
    w = out.append
    w('<?xml version="1.0" encoding="utf-8"?>')
    w('<!-- The carcass of a mutant (ui_mutant_loot.script): its picture, the parts it gives, a preview of the part')
    w('     selected. Written by tools\\ui\\mutant_loot_layout.py for 4:3 and 16:9; every surface and button is drawn')
    w('     by the xms_ui_shape shader (ui_theme.xml), the mutant\'s picture and the item icon are the textures. -->')
    w('<w>')
    w('    <loot>')
    w('        <shade %s shape="shadow"><shape fill="$shade" soft="0" radius="0"/></shade>' % rect(0, 0, screen_w, 768))
    w('        <form %s shape="panel">' % rect(fx, fy, W, H))
    w('            <image_frame %s shape="inset"/>' % rect(14, 14, 236, 120))
    w('            <image %s stretch="1"/>' % rect(16, 16, 232, 116))
    w('            <caption %s>' % rect(266, 18, 280, 26))
    w('                <text font="graffiti22" align="l" vert_align="c" color="$text">st_mutant_loot_caption</text>')
    w('            </caption>')
    w('            <hint %s complex_mode="1">' % rect(266, 48, 280, 94))
    w('                <text font="letterica16" align="l" vert_align="t" complex_mode="1" color="$text_dim">st_mutant_loot_hint</text>')
    w('            </hint>')
    w('            <list_frame %s shape="inset"/>' % rect(14, 146, 330, 170))
    w('            <list %s item_height="24" can_select="1" always_show_scroll="0" scroll_profile="thin">' % rect(18, 150, 322, 162))
    w('                <font font="letterica18" color="$text"/>')
    # the marks show what is chosen, so a row has no look of its own for the last one clicked
    w('                <item shape="list_row_marks"/>')
    w('            </list>')
    w('            <preview_frame %s shape="inset"/>' % rect(354, 146, 192, 170))
    w('            <icon %s stretch="1"/>' % rect(364, 156, 172, 118))
    w('            <icon_name %s complex_mode="1">' % rect(360, 278, 180, 34))
    w('                <text font="letterica16" align="c" vert_align="c" complex_mode="1" color="$text"/>')
    w('            </icon_name>')
    for node, x, width, caption, style in (('btn_cancel', 14, 120, 'ui_inv_cancel', 'button'),
                                           ('btn_ok', 280, 126, 'st_mutant_loot_take', 'button'),
                                           ('btn_ok_all', 414, 132, 'ui_st_take_all', 'button_primary')):
        w('            <%s %s>' % (node, rect(x, 326, width, 32)))
        w('                <shape style="%s"/>' % style)
        w('                <text font="letterica18" align="c" vert_align="c">%s</text>' % caption)
        w('            </%s>' % node)
    w('        </form>')
    w('        <file_item>')
    # a part marked for the next cut: the row tinted, a bar at its left edge
    w('            <mark %s><shape fill="$accent_soft" radius="2"/></mark>' % rect(0, 0, 318, 24))
    w('            <mark_bar %s><shape fill="$accent" radius="1"/></mark_bar>' % rect(0, 3, 3, 18))
    w('            <main width="%s" height="24"/>' % num(318 * kx))
    w('            <fn width="%s" height="24"/>' % num(250 * kx))
    w('            <fd width="%s" height="24"/>' % num(56 * kx))
    w('        </file_item>')
    w('    </loot>')
    w('</w>')
    return '\n'.join(out) + '\n'


def main():
    for wide, name in ((False, 'ui_mutant_loot.xml'), (True, 'ui_mutant_loot_16.xml')):
        with open(os.path.join(ROOT, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(build(wide))
        print('wrote', name)


if __name__ == '__main__':
    main()
