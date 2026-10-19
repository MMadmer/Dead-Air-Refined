# Writes the layout of the debug menu (UIDebugMenuWnd.cpp, docs/dead-air/DEBUG_MENU.md) for 4:3 and as its _16
# twin, from one geometry, in the palette of the Mods window (the sys_* looks and colours of ui_theme.xml) - the
# look of the main menu's own windows. Lengths are screen units, 768 to the screen's height, x squeezed by 0.75
# into a 16:9 file (system_windows_layout.py's rule).
#
#   python tools\ui\debug_menu_layout.py
#
# Every tab has the same three panes: the categories, the list with its search, and the card of the picked entry
# with its count, its two-way choice and its three buttons; the window shows and names what a tab uses.
import os

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', 'packaging', 'dead-air-x64', 'compatibility',
                    'gamedata', 'configs', 'ui')

W, H = 860.0, 620.0
PAD = 22.0
TAB_H = 30.0
TABS = 7
CAT_W = 156.0
CARD_W = 262.0
ROW_H = 42.0
CAT_ROW_H = 26.0

# the looks of a tab, a category row and a list row (normal, highlight, pushed, checked, disabled)
TAB_LOOKS = [
    '<normal fill="#00000000" line="#E8E6E066" border="1" radius="2" text="#ECE9E2FF"/>',
    '<highlight fill="#E8E6E01F" line="#E8E6E0CC" text="#ECE9E2FF"/>',
    '<pushed fill="#BDB9AEFF" line="#BDB9AEFF" text="#0B0B0BFF"/>',
    '<checked fill="#E8E6E0FF" line="#E8E6E0FF" text="#0B0B0BFF"/>',
    '<disabled fill="#00000000" line="#E8E6E02E" text="#5E5B55FF"/>',
]
ROW_LOOKS = [
    '<normal fill="#00000000" radius="2"/>',
    '<highlight fill="#FFFFFF0D" line="#E8E6E026" border="1"/>',
    '<checked fill="#FFFFFF17" line="#E8E6E0B3" border="1"/>',
]
BUTTON_LOOKS = [
    '<normal fill="#00000000" line="#E8E6E099" border="1" radius="2" text="#ECE9E2FF"/>',
    '<highlight fill="#E8E6E0FF" line="#E8E6E0FF" text="#0B0B0BFF"/>',
    '<pushed fill="#BDB9AEFF" line="#BDB9AEFF" text="#0B0B0BFF"/>',
    '<checked fill="#E8E6E0FF" line="#E8E6E0FF" text="#0B0B0BFF"/>',
    '<disabled fill="#00000000" line="#E8E6E02E" text="#5E5B55FF"/>',
]
PRIMARY_LOOKS = [
    '<normal fill="#00000000" line="#D2BE73FF" border="1" radius="2" text="#D2BE73FF"/>',
    '<highlight fill="#D2BE73FF" line="#D2BE73FF" text="#0B0B0BFF"/>',
    '<pushed fill="#B8A45EFF" line="#B8A45EFF" text="#0B0B0BFF"/>',
    '<checked fill="#D2BE73FF" line="#D2BE73FF" text="#0B0B0BFF"/>',
    '<disabled fill="#00000000" line="#E8E6E02E" text="#5E5B55FF"/>',
]
CAT_LOOKS = [
    '<normal fill="#00000000" radius="2" text="#9A968CFF"/>',
    '<highlight fill="#FFFFFF0D" text="#ECE9E2FF"/>',
    '<pushed fill="#FFFFFF17" text="#ECE9E2FF"/>',
    '<checked fill="#D2BE7314" line="#D2BE7366" border="1" text="#D2BE73FF"/>',
]


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


class Layout:
    def __init__(self, wide):
        self.kx = 0.75 if wide else 1.0
        self.screen_w = 1024.0 / self.kx
        self.x0 = (self.screen_w - W) / 2
        self.y0 = (768.0 - H) / 2
        self.out = ['<?xml version="1.0" encoding="utf-8"?>',
                    '<!-- The debug menu (UIDebugMenuWnd). Written by tools\\ui\\debug_menu_layout.py for 4:3 and 16:9 in',
                    '     the palette of the Mods window (sys_* of ui_theme.xml). -->', '<w>',
                    '    <main %s>' % self.abs(0, 0, self.screen_w, 768),
                    '        <colors text="#ECE9E2FF" muted="#9A968CFF" dim="#7C786FFF" good="#7FB069FF" bad="#DC645AFF" accent="#D2BE73FF"/>']
        self.depth = 2

    def abs(self, x, y, w, h):
        return 'x="%s" y="%s" width="%s" height="%s"' % (num(x * self.kx), num(y), num(w * self.kx), num(h))

    # relative to the window
    def rect(self, x, y, w, h):
        return self.abs(self.x0 + x, self.y0 + y, w, h)

    # relative to a group (its own origin)
    def local(self, x, y, w, h):
        return self.abs(x, y, w, h)

    def line(self, text):
        self.out.append('    ' * self.depth + text)

    def shape(self, node, r, attrs):
        self.line('<%s %s %s/>' % (node, r, attrs))

    def text(self, node, r, font='letterica16', align='l', valign='c', color='$sys_text', caption='', complex_mode=False):
        cm = ' complex_mode="1"' if complex_mode else ''
        self.line('<%s %s>' % (node, r))
        self.line('    <text font="%s" align="%s" vert_align="%s" color="%s"%s>%s</text>' % (font, align, valign, color, cm, caption))
        self.line('</%s>' % node)

    # a CUIShapeButton: its caption and a look per state
    def button(self, node, r, caption, looks=BUTTON_LOOKS, font='letterica16', align='c', extra='', text_x=0):
        self.line('<%s %s%s>' % (node, r, extra))
        tx = ' x="%s"' % num(text_x * self.kx) if text_x else ''
        self.line('    <text font="%s" align="%s" vert_align="c" color="$sys_text"%s>%s</text>' % (font, align, tx, caption))
        for look in looks:
            self.line('    ' + look)
        self.line('</%s>' % node)

    def open(self, node, r, extra=''):
        self.line('<%s %s%s>' % (node, r, extra))
        self.depth += 1

    def close(self, node):
        self.depth -= 1
        self.line('</%s>' % node)

    def finish(self):
        self.out += ['    </main>', '</w>']
        return '\n'.join(self.out) + '\n'


def layout(wide):
    L = Layout(wide)
    # the game under it stays visible: a light shade, the window on a soft shadow
    L.shape('shade', L.abs(0, 0, L.screen_w, 768), 'fill="#00000040"')
    L.shape('shadow', L.rect(-8, -6, W + 16, H + 16), 'fill="#000000B3" soft="18" radius="8"')
    L.shape('frame', L.rect(0, 0, W, H), 'fill="#000000EB" line="#E8E6E0CC" border="1" radius="3"')
    L.text('caption', L.rect(PAD, 12, 300, 34), font='graffiti22', caption='st_dbg_caption')
    L.text('hotkey_hint', L.rect(PAD + 300, 12, W - 2 * PAD - 300 - 110, 34), align='r', color='$sys_dim')
    L.button('close', L.rect(W - PAD - 100, 15, 100, 28), 'st_dbg_close')
    # the tabs, evenly over the window's width
    gap = 6.0
    tab_w = (W - 2 * PAD - gap * (TABS - 1)) / TABS
    L.open('tabs', L.rect(PAD, 56, W - 2 * PAD, TAB_H), ' step="%s"' % num((tab_w + gap) * L.kx))
    L.button('tab', L.local(0, 0, tab_w, TAB_H), '', looks=TAB_LOOKS)
    L.close('tabs')
    L.shape('separator', L.rect(PAD, 56 + TAB_H + 10, W - 2 * PAD, 1), 'fill="#E8E6E033"')

    top = 56 + TAB_H + 22
    bottom = H - PAD
    body_h = bottom - top

    # the categories
    L.shape('cat_back', L.rect(PAD, top, CAT_W, body_h), 'fill="#FFFFFF05" line="#E8E6E026" border="1" radius="3"')
    L.text('cat_caption', L.rect(PAD + 10, top + 4, CAT_W - 20, 24), color='$sys_dim', caption='st_dbg_categories')
    L.open('cats', L.rect(PAD + 5, top + 30, CAT_W - 10, body_h - 36), ' row_height="%s"' % num(CAT_ROW_H))
    L.button('cat', L.local(0, 0, CAT_W - 10, CAT_ROW_H - 2), '', looks=CAT_LOOKS, align='l', text_x=8)
    L.close('cats')

    # the search and the list
    lx = PAD + CAT_W + 12
    lw = W - 2 * PAD - CAT_W - 12 - CARD_W - 12
    L.shape('search_back', L.rect(lx, top, lw, 30), 'fill="#FFFFFF0A" line="#E8E6E066" border="1" radius="2"')
    L.shape('search_icon', L.rect(lx + 9, top + 8, 14, 14), 'kind="search" fill="#9A968CFF" border="1.6"')
    L.text('search_hint', L.rect(lx + 28, top, lw - 36, 30), color='#7C786FFF', caption='st_dbg_search_hint')
    L.line('<search %s max_symb_count="64">' % L.rect(lx + 28, top, lw - 36, 30))
    L.line('    <text font="letterica18" align="l" vert_align="c" color="$sys_text" x="0"/>')
    L.line('</search>')
    L.text('count', L.rect(lx, top + 32, lw, 20), align='r', color='$sys_dim')
    list_top = top + 54
    list_h = bottom - list_top
    L.shape('list_back', L.rect(lx, list_top, lw, list_h), 'fill="#FFFFFF05" line="#E8E6E026" border="1" radius="3"')
    L.open('list', L.rect(lx + 4, list_top + 4, lw - 8, list_h - 8), ' row_height="%s" bar_width="%s"' % (num(ROW_H), num(3 * L.kx)))
    L.shape('track', L.local(lw - 8 - 4, 0, 3, list_h - 8), 'fill="#E8E6E014" radius="1"')
    L.shape('thumb', L.local(lw - 8 - 4, 0, 3, 40), 'fill="#E8E6E080" radius="1"')
    L.open('row', L.local(0, 0, lw - 8 - 10, ROW_H))
    L.line('<back %s>' % L.local(0, 1, lw - 8 - 10, ROW_H - 2))
    for look in ROW_LOOKS:
        L.line('    ' + look)
    L.line('</back>')
    L.text('name', L.local(8, 3, lw - 8 - 10 - 16 - 70, 21), font='letterica18')
    L.text('sec', L.local(8, 23, lw - 8 - 10 - 16 - 70, 16), font='letterica16', color='$sys_dim')
    L.text('badge', L.local(lw - 8 - 10 - 8 - 70, 3, 70, 21), align='r', color='$sys_accent')
    L.text('note', L.local(lw - 8 - 10 - 8 - 90, 23, 90, 16), font='letterica16', align='r', color='$sys_dim')
    L.close('row')
    L.close('list')
    L.text('list_status', L.rect(lx + 10, list_top + 120, lw - 20, 80), align='c', color='$sys_muted', complex_mode=True)

    # the card
    cx = W - PAD - CARD_W
    L.open('card', L.rect(cx, top, CARD_W, body_h))
    L.shape('back', L.local(0, 0, CARD_W, body_h), 'fill="#FFFFFF05" line="#E8E6E026" border="1" radius="3"')
    L.text('hint', L.local(14, 150, CARD_W - 28, 80), align='c', color='$sys_muted', complex_mode=True, caption='st_dbg_pick_hint')
    L.line('<icon %s stretch="1"/>' % L.local(14, 14, CARD_W - 28, 86))
    L.text('title', L.local(14, 104, CARD_W - 28, 44), font='graffiti19', valign='t', complex_mode=True)
    L.text('sub', L.local(14, 148, CARD_W - 28, 18), font='letterica16', color='$sys_dim')
    L.shape('rule', L.local(14, 170, CARD_W - 28, 1), 'fill="#E8E6E033"')
    # the description scrolls in its own place
    bh = body_h - 178 - 228
    L.line('<body %s always_show_scroll="0" vert_interval="2" right_ident="6" left_ident="0" top_indent="0" bottom_indent="0" bar_width="2">' % L.local(14, 178, CARD_W - 28, bh))
    L.line('    <track fill="#E8E6E014"/>')
    L.line('    <thumb fill="#E8E6E080" radius="1"/>')
    L.line('</body>')
    L.text('body_text', L.local(0, 0, CARD_W - 28 - 8, 16), valign='t', color='$sys_muted', complex_mode=True)
    # what the last button did, over the choice
    L.text('status', L.local(14, body_h - 222, CARD_W - 28, 30), align='c', color='$sys_accent', complex_mode=True)
    # the two-way choice (into the inventory / in front), the count, the buttons
    y = body_h - 190
    half = (CARD_W - 28 - 6) / 2
    L.button('choice_a', L.local(14, y, half, 28), '', looks=TAB_LOOKS)
    L.button('choice_b', L.local(14 + half + 6, y, half, 28), '', looks=TAB_LOOKS)
    y += 38
    L.text('count_label', L.local(14, y, 90, 30), color='$sys_muted', caption='st_dbg_count')
    L.button('count_minus', L.local(CARD_W - 14 - 34 - 6 - 70 - 6 - 34, y, 34, 30), '-')
    L.line('<count %s max_symb_count="5" num_only="1">' % L.local(CARD_W - 14 - 34 - 6 - 70, y, 70, 30))
    L.line('    <shape style="sys_field"/>')
    L.line('    <text font="letterica18" align="c" vert_align="c" color="$sys_text"/>')
    L.line('</count>')
    L.button('count_plus', L.local(CARD_W - 14 - 34, y, 34, 30), '+')
    y += 42
    L.button('second', L.local(14, y, half, 30), '')
    L.button('third', L.local(14 + half + 6, y, half, 30), '')
    y += 38
    L.button('primary', L.local(14, y, CARD_W - 28, 34), '', looks=PRIMARY_LOOKS, font='letterica18')
    L.close('card')
    return L.finish()


def main():
    for wide, name in ((False, 'ui_debug_menu.xml'), (True, 'ui_debug_menu_16.xml')):
        with open(os.path.join(ROOT, name), 'w', encoding='utf-8', newline='\r\n') as f:
            f.write(layout(wide))
        print('wrote', name)


if __name__ == '__main__':
    main()
